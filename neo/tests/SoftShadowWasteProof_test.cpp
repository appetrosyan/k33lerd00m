/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 2026 Aleksandr Petrosyan

This program is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version. See <http://www.gnu.org/licenses/>.

===========================================================================
*/

// WASTE PROOF (study). The GPU walk counters CLAIM that 60-95% of survivor iterations contribute
// nothing to the image ("lit-waste" fragments walk whole lists for zero coverage). A counter is an
// assertion; this study is the PROOF, per survivor, on real capture data, with the SHIPPED
// SoftScan_FillTri as the ground truth:
//
//   1. For every (fragment, cone-cull-survivor) pair it computes the survivor's SOLO masked
//      contribution (FillTri into an empty grid, popcount against the disk mask). soloMasked == 0
//      is ABSOLUTE waste: by the OR-union's monotonicity the triangle can contribute nothing in
//      ANY walk order. This is verified, not assumed - step 3.
//   2. Each waste survivor is CLASSIFIED by its earliest catchable exit, replicating FillTri's
//      stages: SLAB (clipped away), YSPAN (projects between chord rows), CIRCLE (the shipped
//      disk reject fires), MASKEDBBOX (a candidate predicate the shipped code does NOT have:
//      the projected-AABB column run ANDed with the disk mask is empty on every spanned chord -
//      <= 8 ANDs, strictly stronger than CIRCLE, still conservative), or FILLED-OUTSIDE
//      (fills raw grid bits but none inside the mask - only FillTri itself can tell).
//      Every classification is CROSS-CHECKED against the real FillTri result; a mismatch fails.
//   3. DROP-INVARIANCE: the fragment's grid is rebuilt walking ONLY the soloMasked>0 survivors;
//      the masked grid must be BIT-IDENTICAL to the full walk's. This is the proof that the
//      soloMasked==0 class is pure waste.
//   4. Survivors with soloMasked>0 whose cumulative delta was 0 are tallied REDUNDANT (their bits
//      were already covered in this order) - real work in principle, skippable only by
//      order-dependent reasoning (the shipped covered-skip); kept distinct from absolute waste.
//
// The output ranks: how much of the measured "waste" is provably waste, how much of that the
// shipped rejects already catch, how much the candidate MASKEDBBOX predicate would newly catch,
// and how much is uncatchable without running FillTri. Those numbers - not the counters - decide
// whether a cheap sound rejector is worth building and what its ceiling is.
//
// Run:  CAP=/path/to/foo.cap ./rbdoom3bfg_tests @study:SoftShadowWasteProof

#include "hlsl_compat.h"

// The live shader source compiled as C++ WITH the Fubini scanline enabled, isolated in a namespace:
// other test TUs compile the same include with SW_SCANLINE 0, so the inline walker bodies differ -
// the namespace keeps the two variants from colliding (no ODR violation). The SW_ATTRIB globals the
// include declares extern are defined inside the namespace below.
#define SW_SCANLINE 1
#define inout						// HLSL inout on FillTri's grid param: C++ array params decay to pointers, so mutation semantics match
namespace swproof
{
#include "softwedge_coverage.inc.hlsl"
swAttrib_t g_swAttrib = {};
bool       g_swAttribOn = false;
}

#include "SoftShadowMesh.h"					// Cap + LoadCap (namespace swtest; standalone - no walker refs)
#include "idUnitTest.h"

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <vector>
#include <algorithm>

using namespace swtest;

namespace
{

int MaskedBits( const uint32_t g[SW_SCAN_CHORDS], const uint32_t m[SW_SCAN_CHORDS] )
{
	int n = 0;
	for( int i = 0; i < SW_SCAN_CHORDS; i++ )
	{
		n += __builtin_popcount( g[i] & m[i] );
	}
	return n;
}

int RawBits( const uint32_t g[SW_SCAN_CHORDS] )
{
	int n = 0;
	for( int i = 0; i < SW_SCAN_CHORDS; i++ )
	{
		n += __builtin_popcount( g[i] );
	}
	return n;
}

// survivor exit classes, ordered by "earliest catchable"
enum wasteClass_t
{
	WC_SLAB = 0,		// slab clip leaves < 3 verts: FillTri returns before projecting
	WC_YSPAN,			// projects outside the chord rows (mLo > mHi)
	WC_CIRCLE,			// the SHIPPED disk reject fires (AABB nearest point outside unit circle)
	WC_MASKEDBBOX,		// candidate predicate: bbox column run ANDed with the disk mask empty on every spanned chord
	WC_FILLOUT,			// fills raw bits, none masked: only FillTri itself can tell
	WC_CONTRIB,			// soloMasked > 0: real work
	WC_TOTAL
};

const char* wcName[WC_TOTAL] = { "slab", "yspan", "circle", "maskbbox", "fillout", "contrib" };

}

STUDY_TEST( SoftShadowWasteProof, decompose )
{
	using namespace swproof;

	const char* path = std::getenv( "CAP" );
	if( path == NULL )
	{
		std::printf( "    [wasteproof] CAP unset; skipping (set it to a .cap)\n" );
		CHECK( true );
		return;
	}
	Cap cap;
	if( !LoadCap( path, cap ) )
	{
		std::printf( "    [wasteproof] cannot load %s\n", path );
		CHECK( false );
		return;
	}

	const int MAX_RECV = 200;			// per-light receiver sample budget (each runs 2-3 full walks)

	auto F4 = [&]( size_t j ) -> const float*
	{
		return ( j & 1 ) ? cap.edges[j >> 1].e1 : cap.edges[j >> 1].e0;
	};

	// aggregate tallies: per fragment-outcome (lit/pen/umb) x survivor exit class
	long tally[3][WC_TOTAL] = {};
	long redundant[3] = {};				// soloMasked>0 but cumulative delta 0 (order-dependent skip)
	long fragN[3] = {};
	long invarianceChecked = 0, invarianceFailed = 0, classMismatch = 0;
	int  lightsProbed = 0;

	for( size_t li = 0; li < cap.lights.size(); li++ )
	{
		const capLight_t& L = cap.lights[li];
		if( L.penumbraSize <= 0.0f || L.edgeCount == 0 )
		{
			continue;
		}
		const size_t base4 = ( size_t )L.firstEdge * 2;
		const size_t nTris = ( ( size_t )L.edgeCount * 2 ) / 3;
		if( nTris == 0 )
		{
			continue;
		}

		// receiver-centroid sample set (same construction as the walk-attribution study)
		std::vector<float3> recvPts;
		for( const capReceiver_t& r : cap.receivers )
		{
			if( r.lightIndex != ( uint32_t )li )
			{
				continue;
			}
			const uint32_t tc = r.numIndex / 3;
			for( uint32_t t = 0; t < tc; t++ )
			{
				const uint32_t i0 = cap.recvIdx[r.firstIndex + t * 3 + 0];
				const uint32_t i1 = cap.recvIdx[r.firstIndex + t * 3 + 1];
				const uint32_t i2 = cap.recvIdx[r.firstIndex + t * 3 + 2];
				float3 p0( cap.recvVerts[i0 * 3], cap.recvVerts[i0 * 3 + 1], cap.recvVerts[i0 * 3 + 2] );
				float3 p1( cap.recvVerts[i1 * 3], cap.recvVerts[i1 * 3 + 1], cap.recvVerts[i1 * 3 + 2] );
				float3 p2( cap.recvVerts[i2 * 3], cap.recvVerts[i2 * 3 + 1], cap.recvVerts[i2 * 3 + 2] );
				recvPts.push_back( ( p0 + p1 + p2 ) * ( 1.0f / 3.0f ) );
			}
		}
		if( recvPts.empty() )
		{
			continue;
		}
		const size_t stride = std::max( ( size_t )1, recvPts.size() / MAX_RECV );

		const float3 Lp( L.origin[0], L.origin[1], L.origin[2] );
		const float  swR = std::fmax( L.penumbraSize, 1e-2f );
		const float  swEps = SW_NEAR_EPS;

		for( size_t s = 0; s < recvPts.size(); s += stride )
		{
			const float3 P = recvPts[s];
			softFrame_t  F = SoftShadow_Frame( P, Lp );

			uint32_t diskMask[SW_SCAN_CHORDS];
			int diskBits = 0;
			for( int m = 0; m < SW_SCAN_CHORDS; m++ )
			{
				diskMask[m] = SoftScan_Run( -SW_SCAN_HC[m], SW_SCAN_HC[m] );
				diskBits += __builtin_popcount( diskMask[m] );
			}
			if( diskBits == 0 )
			{
				continue;
			}

			uint32_t grid[SW_SCAN_CHORDS] = {};
			uint32_t gridContribOnly[SW_SCAN_CHORDS] = {};
			// per-survivor records for this fragment (class + solo contribution)
			std::vector<uint8_t> cls;
			cls.reserve( 256 );

			for( size_t t = 0; t < nTris; t++ )
			{
				const float* r0 = F4( base4 + t * 3 + 0 );
				const float* r1 = F4( base4 + t * 3 + 1 );
				const float* r2 = F4( base4 + t * 3 + 2 );
				const float3 v0( r0[0], r0[1], r0[2] );
				const float3 v1( r1[0], r1[1], r1[2] );
				const float3 v2( r2[0], r2[1], r2[2] );
				// the walk's tight cone cull, verbatim (non-survivors are outside this proof's scope)
				const float3 tcen = ( v0 + v1 + v2 ) * ( 1.0f / 3.0f );
				const float3 rc = tcen - P;
				const float  cd = dot( rc, F.nrm );
				const float  triRad = r1[3];
				if( cd + triRad < swEps )
				{
					continue;
				}
				if( cd - triRad > F.distPL )
				{
					continue;
				}
				const float3 perp = rc - cd * F.nrm;
				const float  coneR = swR * ( cd + triRad ) / F.distPL;
				if( dot( perp, perp ) > ( coneR + triRad ) * ( coneR + triRad ) )
				{
					continue;
				}

				// ---- SOLO contribution via the REAL shipped FillTri (the ground truth) ----
				uint32_t solo[SW_SCAN_CHORDS] = {};
				SoftScan_FillTri( solo, v0, v1, v2, P, F, swR, swEps );
				const int soloMasked = MaskedBits( solo, diskMask );

				// ---- classify by earliest catchable exit (replicated stages, then cross-checked) ----
				// slab clip: near (dn >= eps) then far (dn <= distPL), same construction as FillTri
				float3 rel[3] = { v0 - P, v1 - P, v2 - P };
				float  dn[3] = { dot( rel[0], F.nrm ), dot( rel[1], F.nrm ), dot( rel[2], F.nrm ) };
				float3 nRel[4];
				float  nDn[4];
				int nn = 0;
				for( int e = 0; e < 3; e++ )
				{
					const int k = ( e + 1 ) % 3;
					const bool ai = dn[e] >= swEps, bi = dn[k] >= swEps;
					if( ai && nn < 4 )
					{
						nRel[nn] = rel[e];
						nDn[nn] = dn[e];
						nn++;
					}
					if( ( ai != bi ) && nn < 4 )
					{
						const float tt = ( swEps - dn[e] ) / ( dn[k] - dn[e] );
						nRel[nn] = rel[e] + ( rel[k] - rel[e] ) * tt;
						nDn[nn] = swEps;
						nn++;
					}
				}
				int fn = 0;
				float3 fRel[5];
				float  fDn[5];
				if( nn >= 3 )
				{
					for( int e = 0; e < nn; e++ )
					{
						const int k = ( e + 1 ) % nn;
						const bool ai = nDn[e] <= F.distPL, bi = nDn[k] <= F.distPL;
						if( ai && fn < 5 )
						{
							fRel[fn] = nRel[e];
							fDn[fn] = nDn[e];
							fn++;
						}
						if( ( ai != bi ) && fn < 5 )
						{
							const float tt = ( F.distPL - nDn[e] ) / ( nDn[k] - nDn[e] );
							fRel[fn] = nRel[e] + ( nRel[k] - nRel[e] ) * tt;
							fDn[fn] = F.distPL;
							fn++;
						}
					}
				}

				wasteClass_t c;
				if( nn < 3 || fn < 3 )
				{
					c = WC_SLAB;
				}
				else
				{
					const float invR = 1.0f / swR;
					float xmin = 1e30f, xmax = -1e30f, ymin = 1e30f, ymax = -1e30f;
					for( int j = 0; j < fn; j++ )
					{
						const float2 p = SoftShadow_ProjectVert( fRel[j], fDn[j], F ) * invR;
						xmin = std::fmin( xmin, p.x );
						xmax = std::fmax( xmax, p.x );
						ymin = std::fmin( ymin, p.y );
						ymax = std::fmax( ymax, p.y );
					}
					const float halfC = SW_SCAN_CHORDS * 0.5f;
					const int mLo = std::max( ( int )std::ceil( ( ymin + 1.0f ) * halfC - 0.5f ), 0 );
					const int mHi = std::min( ( int )std::floor( ( ymax + 1.0f ) * halfC - 0.5f ), SW_SCAN_CHORDS - 1 );
					const float nx = ( xmin > 0.0f ) ? xmin : ( ( xmax < 0.0f ) ? xmax : 0.0f );
					const float ny = ( ymin > 0.0f ) ? ymin : ( ( ymax < 0.0f ) ? ymax : 0.0f );
					if( mLo > mHi )
					{
						c = WC_YSPAN;
					}
					else if( nx * nx + ny * ny > 1.0f )
					{
						c = WC_CIRCLE;			// the SHIPPED reject
					}
					else
					{
						// candidate MASKEDBBOX: any masked column inside the bbox run on a spanned chord?
						const uint32_t bboxRun = SoftScan_Run( xmin, xmax );
						bool touches = false;
						for( int m = mLo; m <= mHi; m++ )
						{
							if( ( bboxRun & diskMask[m] ) != 0 )
							{
								touches = true;
								break;
							}
						}
						if( !touches )
						{
							c = WC_MASKEDBBOX;
						}
						else if( soloMasked == 0 )
						{
							c = WC_FILLOUT;		// fills bits, none masked: uncatchable pre-fill
						}
						else
						{
							c = WC_CONTRIB;
						}
					}
				}

				// CROSS-CHECK the classification against the real FillTri: every non-CONTRIB class
				// must have contributed zero masked bits (a mismatch = replication drift = the study
				// is lying; fail loudly, do not publish numbers off a broken instrument).
				if( c != WC_CONTRIB && soloMasked != 0 )
				{
					classMismatch++;
				}
				cls.push_back( ( uint8_t )c );

				// cumulative walks
				const int pre = MaskedBits( grid, diskMask );
				SoftScan_FillTri( grid, v0, v1, v2, P, F, swR, swEps );
				const int post = MaskedBits( grid, diskMask );
				if( soloMasked > 0 )
				{
					SoftScan_FillTri( gridContribOnly, v0, v1, v2, P, F, swR, swEps );
					if( post == pre )
					{
						// counted below once the fragment outcome is known
						cls.back() = ( uint8_t )( 0x80 | WC_CONTRIB );	// redundant marker
					}
				}
			}

			// fragment outcome
			const int covBits = MaskedBits( grid, diskMask );
			const float cov = ( float )covBits / ( float )diskBits;
			const int outcome = ( covBits == 0 ) ? 0 : ( ( cov >= 0.99f ) ? 2 : 1 );	// lit / pen / umb
			fragN[outcome]++;

			for( uint8_t cc : cls )
			{
				if( cc & 0x80 )
				{
					redundant[outcome]++;
					tally[outcome][WC_CONTRIB]++;
				}
				else
				{
					tally[outcome][cc]++;
				}
			}

			// DROP-INVARIANCE: the contrib-only walk must reproduce the masked grid EXACTLY
			invarianceChecked++;
			for( int m = 0; m < SW_SCAN_CHORDS; m++ )
			{
				if( ( grid[m] & diskMask[m] ) != ( gridContribOnly[m] & diskMask[m] ) )
				{
					invarianceFailed++;
					break;
				}
			}
		}
		lightsProbed++;
	}

	if( lightsProbed == 0 )
	{
		std::printf( "    [wasteproof] no soft lights with receivers in this capture\n" );
		CHECK( true );
		return;
	}

	const char* oname[3] = { "LIT", "PEN", "UMB" };
	std::printf( "    [wasteproof] ============ %s ============\n", path );
	for( int o = 0; o < 3; o++ )
	{
		long tot = 0;
		for( int c = 0; c < WC_TOTAL; c++ )
		{
			tot += tally[o][c];
		}
		if( tot == 0 )
		{
			continue;
		}
		std::printf( "    [wasteproof] %s frags %ld | survivors %ld:", oname[o], fragN[o], tot );
		for( int c = 0; c < WC_TOTAL; c++ )
		{
			std::printf( "  %s %.1f%%", wcName[c], 100.0 * tally[o][c] / tot );
		}
		std::printf( "  (redundant-in-order %.1f%%)\n", 100.0 * redundant[o] / tot );
	}
	long waste = 0, wasteCircle = 0, wasteMaskb = 0, wasteFillout = 0, wasteGeo = 0, tot = 0;
	for( int o = 0; o < 3; o++ )
	{
		for( int c = 0; c < WC_TOTAL; c++ )
		{
			tot += tally[o][c];
			if( c != WC_CONTRIB )
			{
				waste += tally[o][c];
			}
		}
		wasteGeo += tally[o][WC_SLAB] + tally[o][WC_YSPAN];
		wasteCircle += tally[o][WC_CIRCLE];
		wasteMaskb += tally[o][WC_MASKEDBBOX];
		wasteFillout += tally[o][WC_FILLOUT];
	}
	std::printf( "    [wasteproof] TOTAL survivors %ld: PROVEN-WASTE %.1f%% (slab+yspan %.1f%% | shipped-circle %.1f%% | NEW maskbbox +%.1f%% | uncatchable-fillout %.1f%%) | contrib %.1f%%\n",
				 tot, 100.0 * waste / tot, 100.0 * wasteGeo / tot, 100.0 * wasteCircle / tot,
				 100.0 * wasteMaskb / tot, 100.0 * wasteFillout / tot, 100.0 * ( tot - waste ) / tot );
	std::printf( "    [wasteproof] proofs: drop-invariance %ld checked, %ld FAILED | class-vs-FillTri mismatches %ld\n",
				 invarianceChecked, invarianceFailed, classMismatch );

	CHECK( invarianceFailed == 0 );
	CHECK( classMismatch == 0 );
}

// PER-CELL CONTRIBUTOR UNION (study). The waste proof established that only 4-5% of survivors
// (3-5 triangles) contribute per FRAGMENT. The reduced-set walk precomputes, per world CELL, the
// UNION of the contributor sets over the cell's receivers and walks only that union - exact by
// the same drop-invariance argument (union superset-of each P's contributors), with the union's
// redundancy as the accepted price. This study measures the only two numbers that matter:
//   |union| per cell at G = 4 / 8 / 16 world units  (the K the storage must carry), and
//   survivors / |union|  (the walk speedup on a cache hit).
// A per-P exactness spot-check re-walks a sample of receivers with ONLY the cell union and
// asserts the masked grid is bit-identical (belt and braces on the superset argument).
//
// Run:  CAP=/path/to/foo.cap ./rbdoom3bfg_tests @study:SoftShadowCellUnion

STUDY_TEST( SoftShadowCellUnion, sizes )
{
	using namespace swproof;

	const char* path = std::getenv( "CAP" );
	if( path == NULL )
	{
		std::printf( "    [cellunion] CAP unset; skipping (set it to a .cap)\n" );
		CHECK( true );
		return;
	}
	Cap cap;
	if( !LoadCap( path, cap ) )
	{
		std::printf( "    [cellunion] cannot load %s\n", path );
		CHECK( false );
		return;
	}

	const int   MAX_RECV_PER_LIGHT = 600;	// receiver samples per light (cells need multiple P's each)
	const float GRID[3] = { 4.0f, 8.0f, 16.0f };

	auto F4 = [&]( size_t j ) -> const float*
	{
		return ( j & 1 ) ? cap.edges[j >> 1].e1 : cap.edges[j >> 1].e0;
	};

	long exactChecked = 0, exactFailed = 0;

	for( int gi = 0; gi < 3; gi++ )
	{
		const float G = GRID[gi];
		// aggregate over all lights at this G
		std::vector<int> unionSizes;			// one entry per (light, cell)
		double survPerFragTot = 0;
		long   fragTot = 0;

		for( size_t li = 0; li < cap.lights.size(); li++ )
		{
			const capLight_t& L = cap.lights[li];
			if( L.penumbraSize <= 0.0f || L.edgeCount == 0 )
			{
				continue;
			}
			const size_t base4 = ( size_t )L.firstEdge * 2;
			const size_t nTris = ( ( size_t )L.edgeCount * 2 ) / 3;
			if( nTris == 0 )
			{
				continue;
			}

			std::vector<float3> recvPts;
			for( const capReceiver_t& r : cap.receivers )
			{
				if( r.lightIndex != ( uint32_t )li )
				{
					continue;
				}
				const uint32_t tc = r.numIndex / 3;
				for( uint32_t t = 0; t < tc; t++ )
				{
					const uint32_t i0 = cap.recvIdx[r.firstIndex + t * 3 + 0];
					const uint32_t i1 = cap.recvIdx[r.firstIndex + t * 3 + 1];
					const uint32_t i2 = cap.recvIdx[r.firstIndex + t * 3 + 2];
					float3 p0( cap.recvVerts[i0 * 3], cap.recvVerts[i0 * 3 + 1], cap.recvVerts[i0 * 3 + 2] );
					float3 p1( cap.recvVerts[i1 * 3], cap.recvVerts[i1 * 3 + 1], cap.recvVerts[i1 * 3 + 2] );
					float3 p2( cap.recvVerts[i2 * 3], cap.recvVerts[i2 * 3 + 1], cap.recvVerts[i2 * 3 + 2] );
					recvPts.push_back( ( p0 + p1 + p2 ) * ( 1.0f / 3.0f ) );
				}
			}
			if( recvPts.empty() )
			{
				continue;
			}
			const size_t stride = std::max( ( size_t )1, recvPts.size() / MAX_RECV_PER_LIGHT );

			const float3 Lp( L.origin[0], L.origin[1], L.origin[2] );
			const float  swR = std::fmax( L.penumbraSize, 1e-2f );
			const float  swEps = SW_NEAR_EPS;

			// per-P contributor set, bucketed by 3D world cell
			struct cellAcc_t
			{
				std::vector<uint32_t> tris;			// union of contributor tri indices (sorted-unique on demand)
				std::vector<float3>   pts;			// sampled receivers (for the exactness spot-check)
			};
			std::unordered_map<uint64_t, cellAcc_t> cells;

			auto ContribSet = [&]( const float3 & P, std::vector<uint32_t>& out, double* survOut )
			{
				softFrame_t F = SoftShadow_Frame( P, Lp );
				uint32_t diskMask[SW_SCAN_CHORDS];
				for( int m = 0; m < SW_SCAN_CHORDS; m++ )
				{
					diskMask[m] = SoftScan_Run( -SW_SCAN_HC[m], SW_SCAN_HC[m] );
				}
				double surv = 0;
				for( size_t t = 0; t < nTris; t++ )
				{
					const float* r0 = F4( base4 + t * 3 + 0 );
					const float* r1 = F4( base4 + t * 3 + 1 );
					const float* r2 = F4( base4 + t * 3 + 2 );
					const float3 v0( r0[0], r0[1], r0[2] );
					const float3 v1( r1[0], r1[1], r1[2] );
					const float3 v2( r2[0], r2[1], r2[2] );
					const float3 tcen = ( v0 + v1 + v2 ) * ( 1.0f / 3.0f );
					const float3 rc = tcen - P;
					const float  cd = dot( rc, F.nrm );
					const float  triRad = r1[3];
					if( cd + triRad < swEps || cd - triRad > F.distPL )
					{
						continue;
					}
					const float3 perp = rc - cd * F.nrm;
					const float  coneR = swR * ( cd + triRad ) / F.distPL;
					if( dot( perp, perp ) > ( coneR + triRad ) * ( coneR + triRad ) )
					{
						continue;
					}
					surv += 1.0;
					uint32_t solo[SW_SCAN_CHORDS] = {};
					SoftScan_FillTri( solo, v0, v1, v2, P, F, swR, swEps );
					if( MaskedBits( solo, diskMask ) > 0 )
					{
						out.push_back( ( uint32_t )t );
					}
				}
				if( survOut )
				{
					*survOut = surv;
				}
			};

			for( size_t s = 0; s < recvPts.size(); s += stride )
			{
				const float3 P = recvPts[s];
				const int64_t cx = ( int64_t )std::floor( P.x / G ), cy = ( int64_t )std::floor( P.y / G ), cz = ( int64_t )std::floor( P.z / G );
				const uint64_t key = ( ( uint64_t )( cx & 0x1FFFFF ) << 42 ) | ( ( uint64_t )( cy & 0x1FFFFF ) << 21 ) | ( uint64_t )( cz & 0x1FFFFF );
				cellAcc_t& acc = cells[key];
				std::vector<uint32_t> contrib;
				double surv = 0;
				ContribSet( P, contrib, &surv );
				acc.tris.insert( acc.tris.end(), contrib.begin(), contrib.end() );
				acc.pts.push_back( P );
				survPerFragTot += surv;
				fragTot++;
			}

			for( auto& kv : cells )
			{
				std::sort( kv.second.tris.begin(), kv.second.tris.end() );
				kv.second.tris.erase( std::unique( kv.second.tris.begin(), kv.second.tris.end() ), kv.second.tris.end() );
				unionSizes.push_back( ( int )kv.second.tris.size() );

				// exactness spot-check (G=8 only, 1 receiver per cell): walking ONLY the union must
				// reproduce the full walk's masked grid bit-exactly (superset argument, verified)
				if( gi == 1 && !kv.second.pts.empty() )
				{
					const float3 P = kv.second.pts[0];
					softFrame_t F = SoftShadow_Frame( P, Lp );
					uint32_t diskMask[SW_SCAN_CHORDS];
					for( int m = 0; m < SW_SCAN_CHORDS; m++ )
					{
						diskMask[m] = SoftScan_Run( -SW_SCAN_HC[m], SW_SCAN_HC[m] );
					}
					uint32_t full[SW_SCAN_CHORDS] = {}, uni[SW_SCAN_CHORDS] = {};
					for( size_t t = 0; t < nTris; t++ )
					{
						const float* r0 = F4( base4 + t * 3 + 0 );
						const float* r1 = F4( base4 + t * 3 + 1 );
						const float* r2 = F4( base4 + t * 3 + 2 );
						SoftScan_FillTri( full, float3( r0[0], r0[1], r0[2] ), float3( r1[0], r1[1], r1[2] ), float3( r2[0], r2[1], r2[2] ), P, F, swR, swEps );
					}
					for( uint32_t t : kv.second.tris )
					{
						const float* r0 = F4( base4 + ( size_t )t * 3 + 0 );
						const float* r1 = F4( base4 + ( size_t )t * 3 + 1 );
						const float* r2 = F4( base4 + ( size_t )t * 3 + 2 );
						SoftScan_FillTri( uni, float3( r0[0], r0[1], r0[2] ), float3( r1[0], r1[1], r1[2] ), float3( r2[0], r2[1], r2[2] ), P, F, swR, swEps );
					}
					exactChecked++;
					for( int m = 0; m < SW_SCAN_CHORDS; m++ )
					{
						if( ( full[m] & diskMask[m] ) != ( uni[m] & diskMask[m] ) )
						{
							exactFailed++;
							break;
						}
					}
				}
			}
		}

		if( unionSizes.empty() )
		{
			continue;
		}
		std::sort( unionSizes.begin(), unionSizes.end() );
		const size_t n = unionSizes.size();
		double mean = 0;
		long over16 = 0, over32 = 0, over64 = 0;
		for( int v : unionSizes )
		{
			mean += v;
			over16 += ( v > 16 );
			over32 += ( v > 32 );
			over64 += ( v > 64 );
		}
		mean /= ( double )n;
		const double survPerFrag = fragTot ? survPerFragTot / fragTot : 0.0;
		std::printf( "    [cellunion] G=%-2.0f cells %zu | union size mean %.1f med %d p90 %d p99 %d max %d | over K=16/32/64: %.1f%%/%.1f%%/%.1f%% | survivors/frag %.0f -> speedup vs union-med %.0fx\n",
					 G, n, mean, unionSizes[n / 2], unionSizes[( size_t )( n * 0.90 )], unionSizes[( size_t )( n * 0.99 )], unionSizes[n - 1],
					 100.0 * over16 / n, 100.0 * over32 / n, 100.0 * over64 / n,
					 survPerFrag, unionSizes[n / 2] > 0 ? survPerFrag / unionSizes[n / 2] : 0.0 );
	}

	std::printf( "    [cellunion] exactness spot-checks (G=8): %ld checked, %ld FAILED\n", exactChecked, exactFailed );
	CHECK( exactFailed == 0 );
}
