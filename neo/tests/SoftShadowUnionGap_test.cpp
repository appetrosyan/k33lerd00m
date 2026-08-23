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

// GENERAL-CASE structure study (data-driven, 2026-08-22). The general soft-shadow coverage is
// occlusion(P) = Area( lightDisk INTERSECT UNION_t project(tri_t) ) / piR^2 - a UNION, so overlapping
// occluders do NOT add. The banding-free, open-geometry-robust candidate is the per-triangle analytic
// sum Sigma_solo = sum_t SurfBuild_SoloCovExact (continuous, no 1/N quantum, no closed-silhouette needed),
// whose ONLY error is overlap double-counting (Sigma_solo >= union). This measures that gap on REAL
// capture geometry, binned across the penumbra gradient, to decide the whole approach:
//   - if saturate(Sigma_solo) ~= union across the penumbra  -> per-triangle sum IS a banding-free general
//     coverage (overlaps sparse); only umbra/apex texels need a correction.
//   - if Sigma_solo >> union everywhere -> irreducibly a union; a different decomposition is needed.
// Union truth = MeshTruthShadowSoup (ray oracle). Env: CAP (required), UG_N (disk side, def 32),
// UG_SAMP (recv samples/caster, def 40), UG_CASTERS (max casters, def 120), UG_MAXTRI (skip huge, def 1500).

#include "hlsl_compat.h"
#include "softwedge_coverage.inc.hlsl"		// SoftShadow_Frame, softFrame_t, SoftDisk_CircleTriArea
#include "softsurf_classify.inc.hlsl"		// SurfBuild_SoloCovExact (per-triangle analytic occlusion)
#include "SoftShadowMesh.h"					// Cap, LoadCap, MeshTruthShadowSoup (union ray oracle)
#include "idUnitTest.h"

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <unordered_map>
#include <map>
#include <cstdint>
#include <utility>
#include <array>
#include <algorithm>

using namespace swtest;

// FUBINI / scanline union: area(2D union) = integral over horizontal chords of the EXACT 1D interval union.
// Project each triangle (near-plane clipped) to the disk plane, intersect with M horizontal chords; on each
// chord the triangles' x-intervals union EXACTLY by sort+merge - capturing the spatial overlap the scalar
// estimators cannot, with NO 1/N quantum in the chord direction (banding only in the perpendicular, M chords).
// Winding-agnostic (a triangle is opaque regardless of facing). This is the untested non-scalar identity.
static float ScanlineUnionCov( float3 P, float3 L, float swR, const float* rv, const uint32_t* ri, uint32_t numIdx, int M )
{
	const softFrame_t f = SoftShadow_Frame( P, L );
	const float R = swR, eps = SW_NEAR_EPS;
	std::vector<std::array<float2, 4>> polys; std::vector<int> pn;
	const int nT = ( int )( numIdx / 3 );
	for( int t = 0; t < nT; t++ )
	{
		float3 v[3]; float3 rel[3]; float dn[3];
		for( int j = 0; j < 3; j++ ) { uint32_t gi = ri[t * 3 + j]; v[j] = float3( rv[gi * 3], rv[gi * 3 + 1], rv[gi * 3 + 2] ); rel[j] = v[j] - P; dn[j] = dot( rel[j], f.nrm ); }
		float2 q[4]; int qn = 0;								// clip edges against dn >= eps, project survivors
		for( int e = 0; e < 3 && qn < 4; e++ )
		{
			const int i = e, jj = ( e + 1 ) % 3;
			const bool ai = dn[i] >= eps, bi = dn[jj] >= eps;
			if( ai && qn < 4 ) { q[qn++] = SoftShadow_ProjectVert( rel[i], dn[i], f ); }
			if( ai != bi && qn < 4 ) { float tt = ( eps - dn[i] ) / ( dn[jj] - dn[i] ); float3 rc = rel[i] + ( rel[jj] - rel[i] ) * tt; q[qn++] = SoftShadow_ProjectVert( rc, eps, f ); }
		}
		if( qn < 3 ) { continue; }
		std::array<float2, 4> a{}; for( int k = 0; k < qn; k++ ) { a[k] = q[k]; }
		polys.push_back( a ); pn.push_back( qn );
	}
	double area = 0.0; const double dy = 2.0 * R / M;
	std::vector<std::pair<float, float>> iv;
	for( int m = 0; m < M; m++ )
	{
		const float Y = -R + ( ( float )m + 0.5f ) / M * 2.0f * R;
		const float hc = std::sqrt( std::fmax( 0.0f, R * R - Y * Y ) );
		if( hc <= 0.0f ) { continue; }
		iv.clear();
		for( size_t pi = 0; pi < polys.size(); pi++ )
		{
			const std::array<float2, 4>& poly = polys[pi]; const int n = pn[pi];
			float xlo = 1e30f, xhi = -1e30f; bool any = false;
			for( int e = 0; e < n; e++ )
			{
				const float2 A = poly[e], B = poly[( e + 1 ) % n];
				if( ( A.y <= Y ) != ( B.y <= Y ) ) { const float tt = ( Y - A.y ) / ( B.y - A.y ); const float x = A.x + ( B.x - A.x ) * tt; xlo = std::fmin( xlo, x ); xhi = std::fmax( xhi, x ); any = true; }
			}
			if( any ) { const float aa = std::fmax( xlo, -hc ), bb = std::fmin( xhi, hc ); if( bb > aa ) { iv.push_back( { aa, bb } ); } }
		}
		std::sort( iv.begin(), iv.end() );
		double len = 0.0; float curS = 0, curE = 0; bool have = false;
		for( const auto& I : iv )
		{
			if( !have ) { curS = I.first; curE = I.second; have = true; }
			else if( I.first <= curE ) { if( I.second > curE ) { curE = I.second; } }
			else { len += curE - curS; curS = I.first; curE = I.second; }
		}
		if( have ) { len += curE - curS; }
		area += len * dy;
	}
	const float c = ( float )( area / ( PI * R * R ) );
	return c > 1.0f ? 1.0f : c;
}

// GPU-SHAPED scanline: M chords x K bits (K<=32, one uint/chord). Per triangle, fill a CONTIGUOUS bit-run
// per chord (interval -> [c0,c1] -> mask OR); union across triangles is a free OR (exactly like the shipped
// SW_FACE_SAMPLES mask); coverage = popcount(grid & diskMask) / popcount(diskMask). NO SORT - this is the
// form that ports to the term CS. Validates the discretisation cost (K bits/chord) vs the analytic scanline.
static float ScanlineBitGridCov( float3 P, float3 L, float swR, const float* rv, const uint32_t* ri, uint32_t numIdx, int M, int K )
{
	const softFrame_t f = SoftShadow_Frame( P, L );
	const float R = swR, eps = SW_NEAR_EPS;
	if( M > 64 ) { M = 64; } if( K > 32 ) { K = 32; }
	uint32_t grid[64] = {}, diskMask[64] = {};
	for( int m = 0; m < M; m++ )								// per-chord disk mask (columns whose centre is inside)
	{
		const float Y = -R + ( ( float )m + 0.5f ) / M * 2.0f * R;
		const float hc = std::sqrt( std::fmax( 0.0f, R * R - Y * Y ) );
		uint32_t dm = 0;
		for( int c = 0; c < K; c++ ) { const float x = -R + ( ( float )c + 0.5f ) / K * 2.0f * R; if( std::fabs( x ) <= hc ) { dm |= ( 1u << c ); } }
		diskMask[m] = dm;
	}
	const int nT = ( int )( numIdx / 3 );
	for( int t = 0; t < nT; t++ )
	{
		float3 v[3], rel[3]; float dn[3];
		for( int j = 0; j < 3; j++ ) { uint32_t gi = ri[t * 3 + j]; v[j] = float3( rv[gi * 3], rv[gi * 3 + 1], rv[gi * 3 + 2] ); rel[j] = v[j] - P; dn[j] = dot( rel[j], f.nrm ); }
		float2 q[4]; int qn = 0;								// near-clip + project (same as ScanlineUnionCov)
		for( int e = 0; e < 3 && qn < 4; e++ )
		{
			const int i = e, jj = ( e + 1 ) % 3;
			const bool ai = dn[i] >= eps, bi = dn[jj] >= eps;
			if( ai && qn < 4 ) { q[qn++] = SoftShadow_ProjectVert( rel[i], dn[i], f ); }
			if( ai != bi && qn < 4 ) { float tt = ( eps - dn[i] ) / ( dn[jj] - dn[i] ); float3 rc = rel[i] + ( rel[jj] - rel[i] ) * tt; q[qn++] = SoftShadow_ProjectVert( rc, eps, f ); }
		}
		if( qn < 3 ) { continue; }
		for( int m = 0; m < M; m++ )							// fill this triangle's bit-run into every crossed chord
		{
			const float Y = -R + ( ( float )m + 0.5f ) / M * 2.0f * R;
			float xlo = 1e30f, xhi = -1e30f; bool any = false;
			for( int e = 0; e < qn; e++ )
			{
				const float2 A = q[e], B = q[( e + 1 ) % qn];
				if( ( A.y <= Y ) != ( B.y <= Y ) ) { const float tt = ( Y - A.y ) / ( B.y - A.y ); const float x = A.x + ( B.x - A.x ) * tt; xlo = std::fmin( xlo, x ); xhi = std::fmax( xhi, x ); any = true; }
			}
			if( !any ) { continue; }
			int c0 = ( int )std::floor( ( xlo + R ) / ( 2.0f * R ) * K );
			int c1 = ( int )std::ceil( ( xhi + R ) / ( 2.0f * R ) * K ) - 1;
			if( c0 < 0 ) { c0 = 0; } if( c1 > K - 1 ) { c1 = K - 1; }
			if( c1 < c0 ) { continue; }
			const uint32_t run = ( c1 - c0 + 1 >= 32 ) ? 0xFFFFFFFFu : ( ( ( 1u << ( c1 - c0 + 1 ) ) - 1u ) << c0 );
			grid[m] |= run;
		}
	}
	int cov = 0, tot = 0;
	for( int m = 0; m < M; m++ ) { cov += SoftPopcount32( grid[m] & diskMask[m] ); tot += SoftPopcount32( diskMask[m] ); }
	return tot ? ( float )cov / ( float )tot : 0.0f;
}

// GRID-OR fold study support (task #49): build the 8x32 bit-grid ONCE at a texel-centre C, then re-score it
// at arbitrary fragments P WITHOUT rebuilding - the whole point of caching. Also records the occluder DEPTH
// FRACTION phi = dn/distPL per contributing vertex; the intra-texel drift is a depth-DEPENDENT disk-space
// translation (~|delta|/(R*phi)), so a single grid is exact only where the depth spread is thin. This struct
// carries phi min/max/mean so the study can bin error against depth-spread (the decisive parallax axis).
struct GridResult
{
	uint32_t grid[64], diskMask[64];
	int M, K, tot;
	float R;
	softFrame_t f;
	float phiMin, phiMax, phiMean; int phiN;
};
static void ScanlineBuildGridAt( float3 C, float3 L, float swR, const float* rv, const uint32_t* ri, uint32_t numIdx, int M, int K, GridResult& g )
{
	if( M > 64 ) { M = 64; } if( K > 32 ) { K = 32; }
	g.M = M; g.K = K; g.R = swR; g.f = SoftShadow_Frame( C, L );
	g.phiMin = 1e30f; g.phiMax = -1e30f; g.phiMean = 0; g.phiN = 0;
	const softFrame_t f = g.f; const float R = swR, eps = SW_NEAR_EPS;
	for( int m = 0; m < 64; m++ ) { g.grid[m] = 0; g.diskMask[m] = 0; }
	for( int m = 0; m < M; m++ )
	{
		const float Y = -R + ( ( float )m + 0.5f ) / M * 2.0f * R;
		const float hc = std::sqrt( std::fmax( 0.0f, R * R - Y * Y ) );
		uint32_t dm = 0;
		for( int c = 0; c < K; c++ ) { const float x = -R + ( ( float )c + 0.5f ) / K * 2.0f * R; if( std::fabs( x ) <= hc ) { dm |= ( 1u << c ); } }
		g.diskMask[m] = dm;
	}
	const int nT = ( int )( numIdx / 3 );
	for( int t = 0; t < nT; t++ )
	{
		float3 v[3], rel[3]; float dn[3];
		for( int j = 0; j < 3; j++ ) { uint32_t gi = ri[t * 3 + j]; v[j] = float3( rv[gi * 3], rv[gi * 3 + 1], rv[gi * 3 + 2] ); rel[j] = v[j] - C; dn[j] = dot( rel[j], f.nrm ); }
		float2 q[4]; int qn = 0;
		for( int e = 0; e < 3 && qn < 4; e++ )
		{
			const int i = e, jj = ( e + 1 ) % 3;
			const bool ai = dn[i] >= eps, bi = dn[jj] >= eps;
			if( ai && qn < 4 ) { q[qn++] = SoftShadow_ProjectVert( rel[i], dn[i], f ); }
			if( ai != bi && qn < 4 ) { float tt = ( eps - dn[i] ) / ( dn[jj] - dn[i] ); float3 rc = rel[i] + ( rel[jj] - rel[i] ) * tt; q[qn++] = SoftShadow_ProjectVert( rc, eps, f ); }
		}
		if( qn < 3 ) { continue; }
		bool touched = false;
		for( int m = 0; m < M; m++ )
		{
			const float Y = -R + ( ( float )m + 0.5f ) / M * 2.0f * R;
			float xlo = 1e30f, xhi = -1e30f; bool any = false;
			for( int e = 0; e < qn; e++ )
			{
				const float2 A = q[e], B = q[( e + 1 ) % qn];
				if( ( A.y <= Y ) != ( B.y <= Y ) ) { const float tt = ( Y - A.y ) / ( B.y - A.y ); const float x = A.x + ( B.x - A.x ) * tt; xlo = std::fmin( xlo, x ); xhi = std::fmax( xhi, x ); any = true; }
			}
			if( !any ) { continue; }
			int c0 = ( int )std::floor( ( xlo + R ) / ( 2.0f * R ) * K );
			int c1 = ( int )std::ceil( ( xhi + R ) / ( 2.0f * R ) * K ) - 1;
			if( c0 < 0 ) { c0 = 0; } if( c1 > K - 1 ) { c1 = K - 1; }
			if( c1 < c0 ) { continue; }
			const uint32_t run = ( c1 - c0 + 1 >= 32 ) ? 0xFFFFFFFFu : ( ( ( 1u << ( c1 - c0 + 1 ) ) - 1u ) << c0 );
			g.grid[m] |= run; touched = true;
		}
		if( touched )										// this triangle actually shadows: fold its depth into the spread
		{
			for( int j = 0; j < 3; j++ ) if( dn[j] >= eps ) { const float phi = dn[j] / std::fmax( f.distPL, 1e-4f ); g.phiMin = std::fmin( g.phiMin, phi ); g.phiMax = std::fmax( g.phiMax, phi ); g.phiMean += phi; g.phiN++; }
		}
	}
	if( g.phiN ) { g.phiMean /= g.phiN; } else { g.phiMin = g.phiMax = g.phiMean = 0; }
	int tot = 0; for( int m = 0; m < M; m++ ) { tot += SoftPopcount32( g.diskMask[m] ); }
	g.tot = tot;
}
static float GridScore( const GridResult& g )			// A: center grid, no interp (P-independent)
{
	int cov = 0; for( int m = 0; m < g.M; m++ ) { cov += SoftPopcount32( g.grid[m] & g.diskMask[m] ); }
	return g.tot ? ( float )cov / ( float )g.tot : 0.0f;
}
// B: score C's grid AS IF translated by (dcol,dchord) disk cells - the first-order intra-texel depth-shift.
// Grid pattern slides by +dq to reach P's view; dest chord m samples source chord m-dchord, bits shift by dcol.
static float GridScoreShift( const GridResult& g, int dcol, int dchord )
{
	int cov = 0;
	for( int m = 0; m < g.M; m++ )
	{
		const int sm = m - dchord; if( sm < 0 || sm >= g.M ) { continue; }
		const uint32_t row = g.grid[sm];
		const uint32_t sh = dcol >= 0 ? ( dcol >= 32 ? 0u : ( row << dcol ) ) : ( ( -dcol ) >= 32 ? 0u : ( row >> ( -dcol ) ) );
		cov += SoftPopcount32( sh & g.diskMask[m] );
	}
	return g.tot ? ( float )cov / ( float )g.tot : 0.0f;
}

// PER-DEPTH-LAYER grid (the parallax fix the single Jacobian couldn't do): split occluders into NB depth
// bands by 1/phi (uniform in 1/phi so the within-band shift SPREAD is bounded), one bit-grid per band. At P
// each band shifts by its OWN -delta/(R*phi_band) - constant within a thin band - and the shifted band-grids
// OR into one accumulator (bands overlap on the disk, so union not sum), popcount once. K->1 == single shift
// B (dead); K large -> approaches D (grid@P). Measures how many bands buy back the intra-texel drift.
#define SW_MAX_BANDS 8
struct BandGrids
{
	uint32_t grid[SW_MAX_BANDS][64], diskMask[64];
	float phiBand[SW_MAX_BANDS]; int phiN[SW_MAX_BANDS];
	int M, K, tot, nBands; float R; softFrame_t f;
};
static void ScanlineBuildBandsAt( float3 C, float3 L, float swR, const float* rv, const uint32_t* ri, uint32_t numIdx, int M, int Kbits, int nBands, BandGrids& g )
{
	if( M > 64 ) { M = 64; } if( Kbits > 32 ) { Kbits = 32; } if( nBands > SW_MAX_BANDS ) { nBands = SW_MAX_BANDS; } if( nBands < 1 ) { nBands = 1; }
	g.M = M; g.K = Kbits; g.R = swR; g.nBands = nBands; g.f = SoftShadow_Frame( C, L );
	const softFrame_t f = g.f; const float R = swR, eps = SW_NEAR_EPS;
	for( int b = 0; b < SW_MAX_BANDS; b++ ) { g.phiBand[b] = 0; g.phiN[b] = 0; for( int m = 0; m < 64; m++ ) { g.grid[b][m] = 0; } }
	for( int m = 0; m < 64; m++ ) { g.diskMask[m] = 0; }
	for( int m = 0; m < M; m++ )
	{
		const float Y = -R + ( ( float )m + 0.5f ) / M * 2.0f * R;
		const float hc = std::sqrt( std::fmax( 0.0f, R * R - Y * Y ) );
		uint32_t dm = 0;
		for( int c = 0; c < Kbits; c++ ) { const float x = -R + ( ( float )c + 0.5f ) / Kbits * 2.0f * R; if( std::fabs( x ) <= hc ) { dm |= ( 1u << c ); } }
		g.diskMask[m] = dm;
	}
	const int nT = ( int )( numIdx / 3 );
	std::vector<std::array<float2, 4>> polys; std::vector<int> pn; std::vector<float> pphi;	// pass 1: project + per-tri phi
	polys.reserve( nT ); pn.reserve( nT ); pphi.reserve( nT );
	float iphiMin = 1e30f, iphiMax = -1e30f;
	for( int t = 0; t < nT; t++ )
	{
		float3 v[3], rel[3]; float dn[3];
		for( int j = 0; j < 3; j++ ) { uint32_t gi = ri[t * 3 + j]; v[j] = float3( rv[gi * 3], rv[gi * 3 + 1], rv[gi * 3 + 2] ); rel[j] = v[j] - C; dn[j] = dot( rel[j], f.nrm ); }
		float2 q[4]; int qn = 0; float phiSum = 0; int phiC = 0;
		for( int e = 0; e < 3 && qn < 4; e++ )
		{
			const int i = e, jj = ( e + 1 ) % 3;
			const bool ai = dn[i] >= eps, bi = dn[jj] >= eps;
			if( ai && qn < 4 ) { q[qn++] = SoftShadow_ProjectVert( rel[i], dn[i], f ); phiSum += dn[i] / std::fmax( f.distPL, 1e-4f ); phiC++; }
			if( ai != bi && qn < 4 ) { float tt = ( eps - dn[i] ) / ( dn[jj] - dn[i] ); float3 rc = rel[i] + ( rel[jj] - rel[i] ) * tt; q[qn++] = SoftShadow_ProjectVert( rc, eps, f ); phiSum += eps / std::fmax( f.distPL, 1e-4f ); phiC++; }
		}
		if( qn < 3 || phiC == 0 ) { continue; }
		std::array<float2, 4> a{}; for( int k = 0; k < qn; k++ ) { a[k] = q[k]; }
		const float phi = std::fmax( phiSum / phiC, 1e-4f );
		polys.push_back( a ); pn.push_back( qn ); pphi.push_back( phi );
		const float iphi = 1.0f / phi; iphiMin = std::fmin( iphiMin, iphi ); iphiMax = std::fmax( iphiMax, iphi );
	}
	const float span = std::fmax( iphiMax - iphiMin, 1e-6f );	// pass 2: band by 1/phi, fill band grid
	for( size_t pi = 0; pi < polys.size(); pi++ )
	{
		const float iphi = 1.0f / pphi[pi];
		int b = ( int )( ( iphi - iphiMin ) / span * nBands ); if( b < 0 ) { b = 0; } if( b >= nBands ) { b = nBands - 1; }
		g.phiBand[b] += pphi[pi]; g.phiN[b]++;
		const std::array<float2, 4>& poly = polys[pi]; const int n = pn[pi];
		for( int m = 0; m < M; m++ )
		{
			const float Y = -R + ( ( float )m + 0.5f ) / M * 2.0f * R;
			float xlo = 1e30f, xhi = -1e30f; bool any = false;
			for( int e = 0; e < n; e++ )
			{
				const float2 A = poly[e], B = poly[( e + 1 ) % n];
				if( ( A.y <= Y ) != ( B.y <= Y ) ) { const float tt = ( Y - A.y ) / ( B.y - A.y ); const float x = A.x + ( B.x - A.x ) * tt; xlo = std::fmin( xlo, x ); xhi = std::fmax( xhi, x ); any = true; }
			}
			if( !any ) { continue; }
			int c0 = ( int )std::floor( ( xlo + R ) / ( 2.0f * R ) * Kbits );
			int c1 = ( int )std::ceil( ( xhi + R ) / ( 2.0f * R ) * Kbits ) - 1;
			if( c0 < 0 ) { c0 = 0; } if( c1 > Kbits - 1 ) { c1 = Kbits - 1; }
			if( c1 < c0 ) { continue; }
			const uint32_t run = ( c1 - c0 + 1 >= 32 ) ? 0xFFFFFFFFu : ( ( ( 1u << ( c1 - c0 + 1 ) ) - 1u ) << c0 );
			g.grid[b][m] |= run;
		}
	}
	int tot = 0; for( int m = 0; m < M; m++ ) { tot += SoftPopcount32( g.diskMask[m] ); }
	g.tot = tot;
}
// E: score band grids at P - each band shifts by its own depth, shifted grids OR into one accumulator.
static float GridScoreBandsShift( const BandGrids& g, float du, float dv )
{
	uint32_t acc[64] = {};
	for( int b = 0; b < g.nBands; b++ )
	{
		if( g.phiN[b] == 0 ) { continue; }
		const float phi = std::fmax( g.phiBand[b] / g.phiN[b], 1e-3f );
		const int dcol = ( int )std::lround( ( -du / phi ) / ( 2.0f * g.R ) * g.K );
		const int dchord = ( int )std::lround( ( -dv / phi ) / ( 2.0f * g.R ) * g.M );
		for( int m = 0; m < g.M; m++ )
		{
			const int sm = m - dchord; if( sm < 0 || sm >= g.M ) { continue; }
			const uint32_t row = g.grid[b][sm];
			acc[m] |= dcol >= 0 ? ( dcol >= 32 ? 0u : ( row << dcol ) ) : ( ( -dcol ) >= 32 ? 0u : ( row >> ( -dcol ) ) );
		}
	}
	int cov = 0; for( int m = 0; m < g.M; m++ ) { cov += SoftPopcount32( acc[m] & g.diskMask[m] ); }
	return g.tot ? ( float )cov / ( float )g.tot : 0.0f;
}

// mesh -> per-edge (two adjacent face normals + boundary flag), WELDED by position so T-junction
// duplicate verts do not masquerade as boundaries. Mirrors BuildCasterEdges in SoftShadowPrimitives_test.
struct RAEdge2 { float3 a, b, nA, nB; bool boundary; int va = 0, vb = 0; };
static std::vector<RAEdge2> BuildWeldedCasterEdges( const Cap& cap, const capCaster_t& cs, float weld )
{
	std::unordered_map<uint64_t, int> wmap;
	auto weld1 = [&]( uint32_t gi ) -> int
	{
		const float* p = &cap.meshVerts[gi * 3];
		int64_t x = ( int64_t )std::llround( p[0] / weld ), y = ( int64_t )std::llround( p[1] / weld ), z = ( int64_t )std::llround( p[2] / weld );
		uint64_t k = ( ( uint64_t )( uint32_t )( x + 1048576 ) << 42 ) | ( ( uint64_t )( uint32_t )( y + 1048576 ) << 21 ) | ( uint64_t )( uint32_t )( z + 1048576 );  // collision-free pack (|coord| < 2^20 quantized units)
		auto it = wmap.find( k );
		if( it != wmap.end() ) { return it->second; }
		int id = ( int )wmap.size(); wmap[k] = id; return id;
	};
	const int nT = ( int )( cs.numIndex / 3 );
	std::map<std::pair<int, int>, int> emap;
	std::vector<RAEdge2> out;
	for( int t = 0; t < nT; t++ )
	{
		int wi[3]; float3 v[3];
		for( int j = 0; j < 3; j++ ) { uint32_t gi = cap.meshIdx[cs.firstIndex + t * 3 + j]; wi[j] = weld1( gi ); v[j] = float3( cap.meshVerts[gi * 3], cap.meshVerts[gi * 3 + 1], cap.meshVerts[gi * 3 + 2] ); }
		float3 nf = cross( v[1] - v[0], v[2] - v[0] );
		for( int e = 0; e < 3; e++ )
		{
			int u = wi[e], w = wi[( e + 1 ) % 3];
			std::pair<int, int> key( std::min( u, w ), std::max( u, w ) );
			auto it = emap.find( key );
			if( it == emap.end() ) { RAEdge2 re; re.a = v[e]; re.b = v[( e + 1 ) % 3]; re.nA = nf; re.nB = float3( 0, 0, 0 ); re.boundary = true; re.va = u; re.vb = w; emap[key] = ( int )out.size(); out.push_back( re ); }
			else { out[it->second].nB = nf; out[it->second].boundary = false; }
		}
	}
	return out;
}
static std::vector<RAEdge2> BuildEdgesRaw( const float* verts, const uint32_t* idx, uint32_t numIdx, float weld )
{
	std::unordered_map<uint64_t, int> wmap;
	auto weld1 = [&]( uint32_t gi ) -> int
	{
		const float* p = &verts[gi * 3];
		int64_t x = ( int64_t )std::llround( p[0] / weld ), y = ( int64_t )std::llround( p[1] / weld ), z = ( int64_t )std::llround( p[2] / weld );
		uint64_t k = ( ( uint64_t )( uint32_t )( x + 1048576 ) << 42 ) | ( ( uint64_t )( uint32_t )( y + 1048576 ) << 21 ) | ( uint64_t )( uint32_t )( z + 1048576 );  // collision-free pack (|coord| < 2^20 quantized units)
		auto it = wmap.find( k ); if( it != wmap.end() ) { return it->second; }
		int id = ( int )wmap.size(); wmap[k] = id; return id;
	};
	std::map<std::pair<int, int>, int> emap; std::vector<RAEdge2> out;
	for( uint32_t k = 0; k + 2 < numIdx; k += 3 )
	{
		int wi[3]; float3 v[3];
		for( int j = 0; j < 3; j++ ) { uint32_t gi = idx[k + j]; wi[j] = weld1( gi ); v[j] = float3( verts[gi * 3], verts[gi * 3 + 1], verts[gi * 3 + 2] ); }
		float3 nf = cross( v[1] - v[0], v[2] - v[0] );
		for( int e = 0; e < 3; e++ )
		{
			int u = wi[e], w = wi[( e + 1 ) % 3];
			std::pair<int, int> key( std::min( u, w ), std::max( u, w ) );
			auto it = emap.find( key );
			if( it == emap.end() ) { RAEdge2 re; re.a = v[e]; re.b = v[( e + 1 ) % 3]; re.nA = nf; re.nB = float3( 0, 0, 0 ); re.boundary = true; re.va = u; re.vb = w; emap[key] = ( int )out.size(); out.push_back( re ); }
			else { out[it->second].nB = nf; out[it->second].boundary = false; }
		}
	}
	return out;
}
// serialize RAEdge2 list into ProcCaster's 4-float4/edge record format
static std::vector<float4> ProcRecords( const std::vector<RAEdge2>& edges )
{
	std::vector<float4> cand; cand.reserve( edges.size() * 4 );
	for( const RAEdge2& e : edges )
	{
		cand.push_back( float4( e.a.x, e.a.y, e.a.z, ( float )e.va ) );
		cand.push_back( float4( e.b.x, e.b.y, e.b.z, ( float )e.vb ) );
		cand.push_back( float4( e.nA.x, e.nA.y, e.nA.z, e.boundary ? 1.0f : 0.0f ) );
		cand.push_back( float4( e.nB.x, e.nB.y, e.nB.z, 0.0f ) );
	}
	return cand;
}

// ANALYTIC silhouette coverage: sum SoftDisk_CircleTriArea over the silhouette edges (boundary OR
// facing-flip, receiver-apex orientation - the F6b-validated ProcCaster method). Connector-free (accepts
// small near-plane-clip error). This is the banding-free, winding-consistent candidate for the union.
static float SilhouetteCov( const std::vector<RAEdge2>& edges, float3 P, float3 L, float swR )
{
	softFrame_t f = SoftShadow_Frame( P, L );
	const float swR2 = swR * swR;
	float area = 0.0f;
	for( const RAEdge2& e : edges )
	{
		const bool va = dot( e.nA, P - e.a ) > 0.0f;
		bool sil, flip;
		if( e.boundary ) { sil = true; flip = !va; }
		else { const bool vb = dot( e.nB, P - e.a ) > 0.0f; sil = ( va != vb ); flip = !va; }
		if( !sil ) { continue; }
		const float3 A = flip ? e.b : e.a, B = flip ? e.a : e.b;
		const float3 a = A - P, b = B - P;
		const float dnA = dot( a, f.nrm ), dnB = dot( b, f.nrm ), d = dnB - dnA;
		softClip_t cl = SoftShadow_ClipSlab( dnA, dnB, SW_NEAR_EPS, f.distPL );
		if( cl.empty ) { continue; }
		float2 q0 = SoftShadow_ProjectVert( a + cl.t0 * ( b - a ), dnA + cl.t0 * d, f );
		float2 q1 = SoftShadow_ProjectVert( a + cl.t1 * ( b - a ), dnA + cl.t1 * d, f );
		area += SoftDisk_CircleTriArea( q0, q1, swR2 );
	}
	float c = std::fabs( area ) / ( PI * swR2 );
	return c > 1.0f ? 1.0f : c;
}

STUDY_TEST( SoftShadowUnionGap, quantify )
{
	const char* path = std::getenv( "CAP" );
	if( path == NULL ) { std::printf( "    [uniongap] CAP unset; skipping\n" ); CHECK( true ); return; }
	Cap cap;
	if( !LoadCap( path, cap ) ) { std::printf( "    [uniongap] cannot load %s\n", path ); CHECK( false ); return; }
	auto envI = []( const char* k, int d ) { const char* s = std::getenv( k ); return s ? std::atoi( s ) : d; };
	const int N = envI( "UG_N", 32 ), SAMP = envI( "UG_SAMP", 40 ), MAXC = envI( "UG_CASTERS", 120 ), MAXTRI = envI( "UG_MAXTRI", 1500 );

	// receiver centroids per light (copied from the proxyfit harness)
	std::vector<std::vector<float3>> lightRecv( cap.lights.size() );
	for( const capReceiver_t& r : cap.receivers )
	{
		if( r.lightIndex >= cap.lights.size() ) { continue; }
		for( uint32_t t = 0; t < r.numIndex / 3; t++ )
		{
			const uint32_t i0 = cap.recvIdx[r.firstIndex + t * 3 + 0], i1 = cap.recvIdx[r.firstIndex + t * 3 + 1], i2 = cap.recvIdx[r.firstIndex + t * 3 + 2];
			float3 p0( cap.recvVerts[i0 * 3], cap.recvVerts[i0 * 3 + 1], cap.recvVerts[i0 * 3 + 2] );
			float3 p1( cap.recvVerts[i1 * 3], cap.recvVerts[i1 * 3 + 1], cap.recvVerts[i1 * 3 + 2] );
			float3 p2( cap.recvVerts[i2 * 3], cap.recvVerts[i2 * 3 + 1], cap.recvVerts[i2 * 3 + 2] );
			lightRecv[r.lightIndex].push_back( ( p0 + p1 + p2 ) * ( 1.0f / 3.0f ) );
		}
	}

	const int NB = 10;						// bins by UNION occlusion [0,1)
	double bAbs[NB] = {}, bSgn[NB] = {}, bSum[NB] = {}, bUni[NB] = {}, bRatio[NB] = {}, bRatioAll[NB] = {}; long bN[NB] = {};
	long nSamp = 0, nNear = 0, nPen = 0, nPenNear = 0;		// nNear = |clampedSum-union|<=0.06 ; nPen = penumbra samples
	int usedC = 0;

	for( uint32_t c = 0; c < cap.casters.size() && usedC < MAXC; c++ )
	{
		const capCaster_t& cs = cap.casters[c];
		const int nT = ( int )( cs.numIndex / 3 );
		if( nT < 1 || nT > MAXTRI || cs.lightIndex >= cap.lights.size() ) { continue; }
		const std::vector<float3>& recv = lightRecv[cs.lightIndex];
		if( recv.empty() ) { continue; }
		const float3 L( cap.lights[cs.lightIndex].origin[0], cap.lights[cs.lightIndex].origin[1], cap.lights[cs.lightIndex].origin[2] );
		const float  swR = std::fmax( cap.lights[cs.lightIndex].penumbraSize, 1e-2f );
		const float  swR2 = swR * swR;
		const float* rv = cap.meshVerts.data();
		const uint32_t* ri = &cap.meshIdx[cs.firstIndex];
		const size_t stride = std::max( ( size_t )1, recv.size() / ( size_t )SAMP );
		bool anyPen = false;
		for( size_t s = 0; s < recv.size(); s += stride )
		{
			const float3 P = recv[s];
			const float lit = MeshTruthShadowSoup( rv, ri, cs.numIndex, P, L, swR, N );	// 1 = lit
			const float uni = 1.0f - lit;												// UNION occlusion
			if( uni < 0.002f ) { continue; }											// this caster does not shadow P
			anyPen = true;
			// per-triangle analytic occlusion, summed (the continuous, robust candidate)
			const softFrame_t f = SoftShadow_Frame( P, L );
			float sum = 0.0f, sumFF = 0.0f;		// all triangles vs FRONT-FACING (normal toward light) only
			for( int t = 0; t < nT; t++ )
			{
				const uint32_t a = cap.meshIdx[cs.firstIndex + t * 3 + 0], b = cap.meshIdx[cs.firstIndex + t * 3 + 1], d = cap.meshIdx[cs.firstIndex + t * 3 + 2];
				float3 v0( rv[a * 3], rv[a * 3 + 1], rv[a * 3 + 2] );
				float3 v1( rv[b * 3], rv[b * 3 + 1], rv[b * 3 + 2] );
				float3 v2( rv[d * 3], rv[d * 3 + 1], rv[d * 3 + 2] );
				const float solo = SurfBuild_SoloCovExact( P, v0, v1, v2, f, swR2 );
				sum += solo;
				const float3 gn = cross( v1 - v0, v2 - v0 );			// geometric normal
				const float3 tc = ( v0 + v1 + v2 ) * ( 1.0f / 3.0f );
				if( dot( gn, L - tc ) > 0.0f ) { sumFF += solo; }		// keep faces turned toward the light
			}
			const float clamped = sum < 0.0f ? 0.0f : ( sum > 1.0f ? 1.0f : sum );
			const float clampedFF = sumFF < 0.0f ? 0.0f : ( sumFF > 1.0f ? 1.0f : sumFF );
			const float gap = clampedFF - uni;		// PRIMARY metric now: front-face sum vs union
			int bi = ( int )( uni * NB ); if( bi < 0 ) { bi = 0; } if( bi >= NB ) { bi = NB - 1; }
			bAbs[bi] += std::fabs( gap ); bSgn[bi] += gap; bSum[bi] += clampedFF; bUni[bi] += uni;
			bRatio[bi] += ( uni > 0.02f ? sumFF / uni : 1.0f ); bRatioAll[bi] += ( uni > 0.02f ? sum / uni : 1.0f ); bN[bi]++;
			nSamp++;
			if( std::fabs( gap ) <= 0.06f ) { nNear++; }
			if( uni < 0.998f ) { nPen++; if( std::fabs( gap ) <= 0.06f ) { nPenNear++; } }
		}
		if( anyPen ) { usedC++; }
	}

	if( nSamp == 0 ) { std::printf( "    [uniongap] no shadowing samples\n" ); CHECK( true ); return; }
	std::printf( "    [uniongap] %s | casters %d, samples %ld, N=%d disk\n", path, usedC, nSamp, N );
	std::printf( "    [uniongap] saturate(FRONT-FACE Sigma_solo) vs UNION:  |gap|<=0.06 on %.1f%% of ALL samples, %.1f%% of PENUMBRA samples (nPen %ld)\n",
				 100.0 * nNear / nSamp, nPen ? 100.0 * nPenNear / nPen : 0.0, nPen );
	std::printf( "    [uniongap] union-occlusion bin -> n | union | sat(FF-Ssolo) | |gap| | signed | FF overcount | ALL-tri overcount\n" );
	for( int b = 0; b < NB; b++ )
	{
		if( bN[b] == 0 ) { continue; }
		std::printf( "      occ [%.1f,%.1f)  n%6ld  union %.3f  FF %.3f  |gap| %.4f  signed %+.4f  FFx %.2f  ALLx %.2f\n",
					 b * 0.1, b * 0.1 + 0.1, bN[b], bUni[b] / bN[b], bSum[b] / bN[b], bAbs[b] / bN[b], bSgn[b] / bN[b], bRatio[b] / bN[b], bRatioAll[b] / bN[b] );
	}
	CHECK( true );
}

// GRID-OR FOLD go/no-go (task #49). Today's cache folds static coverage into 4 fp16 corner scalars, bilerped
// at runtime - a bilinear model that structurally CANNOT represent silhouette-edge occluders (they spill to a
// residual walk, and >1024 -> the texel is abandoned "walk-always"; measured 99% on the cinematic). The fix is
// to cache the Fubini bit-grid (8x32) built once at the texel CENTRE and OR the dynamic residual at runtime.
// The cost: a bit-grid, unlike 4 scalar corners, cannot bilerp across the texel. This study measures whether
// the centre grid is accurate enough, and whether a first-order analytic depth-shift recovers the intra-texel
// drift. Methods per fragment P in a G-cell whose centre is C:
//   A center-grid, no interp   = GridScore(grid@C)            (P-independent; the plain proposal)
//   B center-grid + depth-shift = GridScoreShift(grid@C, ...)  (translate by -delta/(R*phi) disk cells)
//   C scalar bilerp (baseline)  = bilerp of 4 corner grid covs (what ships today)
//   D grid rebuilt at P (floor)  = ScanlineBitGridCov(P)       (isolates discretisation from drift)
// Truth = MeshTruthShadowSoup(P) union. Bin by penumbra AND by occluder depth-spread (1/phiMin-1/phiMax) - the
// decisive parallax axis. Headline = of samples where the scalar bilerp FAILS (|C-truth|>0.06, i.e. today's
// walk-always population), what fraction the grid CONVERTS to |err|<0.06. Env: CAP, UG_G (cell, def 8),
// UG_M/UG_K (grid dims, def 8x32), reuse UG_N/UG_SAMP/UG_CASTERS/UG_MAXTRI. Sweep UG_G in {4,8,16} by hand.
STUDY_TEST( SoftShadowCenterGridDrift, quantify )
{
	const char* path = std::getenv( "CAP" );
	if( path == NULL ) { std::printf( "    [griddrift] CAP unset; skipping\n" ); CHECK( true ); return; }
	Cap cap;
	if( !LoadCap( path, cap ) ) { std::printf( "    [griddrift] cannot load %s\n", path ); CHECK( false ); return; }
	auto envI = []( const char* k, int d ) { const char* s = std::getenv( k ); return s ? std::atoi( s ) : d; };
	auto envF = []( const char* k, float d ) { const char* s = std::getenv( k ); return s ? ( float )std::atof( s ) : d; };
	const int N = envI( "UG_N", 32 ), SAMP = envI( "UG_SAMP", 40 ), MAXC = envI( "UG_CASTERS", 120 ), MAXTRI = envI( "UG_MAXTRI", 1500 );
	const int M = envI( "UG_M", 8 ), K = envI( "UG_K", 32 ), BANDS = envI( "UG_BANDS", 4 );
	const float G = envF( "UG_G", 8.0f );

	std::vector<std::vector<std::pair<float3, float3>>> lightRecv( cap.lights.size() );	// (centroid, normal)
	for( const capReceiver_t& r : cap.receivers )
	{
		if( r.lightIndex >= cap.lights.size() ) { continue; }
		for( uint32_t t = 0; t < r.numIndex / 3; t++ )
		{
			const uint32_t i0 = cap.recvIdx[r.firstIndex + t * 3 + 0], i1 = cap.recvIdx[r.firstIndex + t * 3 + 1], i2 = cap.recvIdx[r.firstIndex + t * 3 + 2];
			float3 p0( cap.recvVerts[i0 * 3], cap.recvVerts[i0 * 3 + 1], cap.recvVerts[i0 * 3 + 2] );
			float3 p1( cap.recvVerts[i1 * 3], cap.recvVerts[i1 * 3 + 1], cap.recvVerts[i1 * 3 + 2] );
			float3 p2( cap.recvVerts[i2 * 3], cap.recvVerts[i2 * 3 + 1], cap.recvVerts[i2 * 3 + 2] );
			float3 nf = cross( p1 - p0, p2 - p0 ); float l = std::sqrt( dot( nf, nf ) ); nf = l > 1e-9f ? nf * ( 1.0f / l ) : float3( 0, 0, 1 );
			lightRecv[r.lightIndex].push_back( { ( p0 + p1 + p2 ) * ( 1.0f / 3.0f ), nf } );
		}
	}

	auto gc = []( const float3& p, int i ) { return i == 0 ? p.x : ( i == 1 ? p.y : p.z ); };
	auto sc = []( float3 & p, int i, float val ) { if( i == 0 ) { p.x = val; } else if( i == 1 ) { p.y = val; } else { p.z = val; } };

	const int NB = 10;						// penumbra bins (union occlusion)
	double eA[NB] = {}, eB[NB] = {}, eC[NB] = {}, eD[NB] = {}, eE[NB] = {}, uni_[NB] = {}; long bN[NB] = {};
	const float dsEdge[6] = { 0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 1e30f };	// depth-spread bin upper edges
	double dsA[6] = {}, dsB[6] = {}, dsE[6] = {}; long dsN[6] = {};
	long nSamp = 0, okA = 0, okB = 0, okC = 0, okD = 0, okE = 0;
	long failC = 0, convA = 0, convB = 0, convE = 0;	// bilerp-fails, converted by grid A / shifted B / bands E
	double shiftBits = 0; long shiftN = 0;
	int usedC = 0;

	for( uint32_t c = 0; c < cap.casters.size() && usedC < MAXC; c++ )
	{
		const capCaster_t& cs = cap.casters[c];
		const int nT = ( int )( cs.numIndex / 3 );
		if( nT < 1 || nT > MAXTRI || cs.lightIndex >= cap.lights.size() ) { continue; }
		const std::vector<std::pair<float3, float3>>& recv = lightRecv[cs.lightIndex];
		if( recv.empty() ) { continue; }
		const float3 L( cap.lights[cs.lightIndex].origin[0], cap.lights[cs.lightIndex].origin[1], cap.lights[cs.lightIndex].origin[2] );
		const float  swR = std::fmax( cap.lights[cs.lightIndex].penumbraSize, 1e-2f );
		const float* rv = cap.meshVerts.data();
		const uint32_t* ri = &cap.meshIdx[cs.firstIndex];
		const size_t stride = std::max( ( size_t )1, recv.size() / ( size_t )SAMP );
		bool anyPen = false;
		for( size_t s = 0; s < recv.size(); s += stride )
		{
			const float3 P = recv[s].first, nrmR = recv[s].second;
			const float uni = 1.0f - MeshTruthShadowSoup( rv, ri, cs.numIndex, P, L, swR, N );
			if( uni < 0.002f ) { continue; }
			anyPen = true;

			const float3 an( std::fabs( nrmR.x ), std::fabs( nrmR.y ), std::fabs( nrmR.z ) );	// dominant-axis plane (matches the cache)
			const int d = ( an.x >= an.y && an.x >= an.z ) ? 0 : ( ( an.y >= an.z ) ? 1 : 2 );
			const int a1 = ( d == 0 ) ? 1 : ( ( d == 1 ) ? 2 : 0 ), a2 = ( d == 0 ) ? 2 : ( ( d == 1 ) ? 0 : 1 );
			const float p1v = gc( P, a1 ), p2v = gc( P, a2 );
			const float cu = std::floor( p1v / G ), cv = std::floor( p2v / G );
			const float fu = p1v / G - cu, fv = p2v / G - cv;
			float3 C = P; sc( C, a1, ( cu + 0.5f ) * G ); sc( C, a2, ( cv + 0.5f ) * G );	// texel centre, height = P

			GridResult g; ScanlineBuildGridAt( C, L, swR, rv, ri, cs.numIndex, M, K, g );
			const float A = GridScore( g );
			const float D = ScanlineBitGridCov( P, L, swR, rv, ri, cs.numIndex, M, K );

			float corner[4];									// C-baseline: 4 cell corners, bilerp
			const float cx[4] = { 0, 1, 0, 1 }, cy[4] = { 0, 0, 1, 1 };
			for( int k = 0; k < 4; k++ ) { float3 Q = P; sc( Q, a1, ( cu + cx[k] ) * G ); sc( Q, a2, ( cv + cy[k] ) * G ); corner[k] = ScanlineBitGridCov( Q, L, swR, rv, ri, cs.numIndex, M, K ); }
			const float Cbil = ( corner[0] * ( 1 - fu ) + corner[1] * fu ) * ( 1 - fv ) + ( corner[2] * ( 1 - fu ) + corner[3] * fu ) * fv;

			const float3 delta = P - C;							// B: first-order depth-shift
			const float du = dot( delta, g.f.u ), dv = dot( delta, g.f.v );
			const float phi = std::fmax( g.phiMean, 1e-3f );
			const float dqu = -du / phi, dqv = -dv / phi;		// disk-space translation (radius R units)
			const int dcol = ( int )std::lround( dqu / ( 2.0f * swR ) * K );
			const int dchord = ( int )std::lround( dqv / ( 2.0f * swR ) * M );
			const float B = GridScoreShift( g, dcol, dchord );
			shiftBits += std::fabs( ( double )dcol ) + std::fabs( ( double )dchord ); shiftN++;

			BandGrids bg; ScanlineBuildBandsAt( C, L, swR, rv, ri, cs.numIndex, M, K, BANDS, bg );	// E: per-depth-layer shift
			const float E = GridScoreBandsShift( bg, du, dv );

			const float erA = std::fabs( A - uni ), erB = std::fabs( B - uni ), erC = std::fabs( Cbil - uni ), erD = std::fabs( D - uni ), erE = std::fabs( E - uni );
			int bi = ( int )( uni * NB ); if( bi < 0 ) { bi = 0; } if( bi >= NB ) { bi = NB - 1; }
			eA[bi] += erA; eB[bi] += erB; eC[bi] += erC; eD[bi] += erD; eE[bi] += erE; uni_[bi] += uni; bN[bi]++;
			nSamp++;
			if( erA <= 0.06f ) { okA++; } if( erB <= 0.06f ) { okB++; } if( erC <= 0.06f ) { okC++; } if( erD <= 0.06f ) { okD++; } if( erE <= 0.06f ) { okE++; }
			if( erC > 0.06f ) { failC++; if( erA <= 0.06f ) { convA++; } if( erB <= 0.06f ) { convB++; } if( erE <= 0.06f ) { convE++; } }
			const float spread = ( g.phiMin > 0 && g.phiMax > 0 ) ? ( 1.0f / g.phiMin - 1.0f / g.phiMax ) : 0.0f;
			int di = 0; while( di < 5 && spread > dsEdge[di] ) { di++; }
			dsA[di] += erA; dsB[di] += erB; dsE[di] += erE; dsN[di]++;
		}
		if( anyPen ) { usedC++; }
	}

	if( nSamp == 0 ) { std::printf( "    [griddrift] no shadowing samples\n" ); CHECK( true ); return; }
	std::printf( "    [griddrift] %s | casters %d, samples %ld, grid %dx%d, cell G=%.1f, bands=%d, disk N=%d\n", path, usedC, nSamp, M, K, G, BANDS, N );
	std::printf( "    [griddrift] %% |err|<=0.06:  A(center)%.1f  B(shift)%.1f  E(%dbands+shift)%.1f  C(bilerp)%.1f  D(grid@P floor)%.1f\n",
				 100.0 * okA / nSamp, 100.0 * okB / nSamp, BANDS, 100.0 * okE / nSamp, 100.0 * okC / nSamp, 100.0 * okD / nSamp );
	std::printf( "    [griddrift] CONVERSION (headline): of %ld bilerp-FAIL samples, grid-A converts %.1f%%, shift-B %.1f%%, bands-E %.1f%%\n",
				 failC, failC ? 100.0 * convA / failC : 0.0, failC ? 100.0 * convB / failC : 0.0, failC ? 100.0 * convE / failC : 0.0 );
	std::printf( "    [griddrift] mean disk-shift magnitude: %.2f cells (col+chord) - if <1 everywhere, drift is negligible\n", shiftN ? shiftBits / shiftN : 0.0 );
	std::printf( "    [griddrift] penumbra bin -> n | union | meanErr A | B | E | C | D\n" );
	for( int b = 0; b < NB; b++ )
	{
		if( bN[b] == 0 ) { continue; }
		std::printf( "      occ [%.1f,%.1f)  n%6ld  union %.3f  A %.4f  B %.4f  E %.4f  C %.4f  D %.4f\n",
					 b * 0.1, b * 0.1 + 0.1, bN[b], uni_[b] / bN[b], eA[b] / bN[b], eB[b] / bN[b], eE[b] / bN[b], eC[b] / bN[b], eD[b] / bN[b] );
	}
	std::printf( "    [griddrift] depth-spread bin (1/phiMin-1/phiMax) -> n | meanErr A | B | E | E-A  (per-band vs center)\n" );
	const float loEdge[6] = { 0, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f };
	for( int b = 0; b < 6; b++ )
	{
		if( dsN[b] == 0 ) { continue; }
		std::printf( "      spread [%.1f,%s)  n%6ld  A %.4f  B %.4f  E %.4f  E-A %+.4f\n",
					 loEdge[b], b < 5 ? ( b == 0 ? "0.5" : ( b == 1 ? "1.0" : ( b == 2 ? "2.0" : ( b == 3 ? "4.0" : "8.0" ) ) ) ) : "inf",
					 dsN[b], dsA[b] / dsN[b], dsB[b] / dsN[b], dsE[b] / dsN[b], ( dsE[b] - dsA[b] ) / dsN[b] );
	}
	CHECK( true );
}

// REPRESENTATION RETHINK (task #49, after translate-only grid + per-band shift BOTH refuted): the cache must
// be REPROJECTABLE, which a grid is not (only translatable -> umbra breaks). The reprojectable-yet-cheap
// candidate keeps a little GEOMETRY: per texel store only the top-K static triangles that shape THIS texel's
// shadow (ranked by continuous solo coverage at the centre C), and at the true fragment P walk just those K
// (exact union via the same 8x32 grid@P, so no umbra failure, cost bounded by K). This is exactly the
// existing residual pool but CAPPED at top-K instead of abandoning the texel at resCount>1024. THE question:
// how small can K be? R(K) ranked-prefix coverage at P vs full-union truth; K=all == D (grid@P, ~96% floor).
// If a small K hits 90%+, the 99% walk-always population is rescued by a bounded, reprojectable cache with a
// bounded per-fragment walk. Env: CAP, UG_G (cell, def 8), reuse UG_N/UG_SAMP/UG_CASTERS/UG_MAXTRI.
STUDY_TEST( SoftShadowReducedSet, quantify )
{
	const char* path = std::getenv( "CAP" );
	if( path == NULL ) { std::printf( "    [redset] CAP unset; skipping\n" ); CHECK( true ); return; }
	Cap cap;
	if( !LoadCap( path, cap ) ) { std::printf( "    [redset] cannot load %s\n", path ); CHECK( false ); return; }
	auto envI = []( const char* k, int d ) { const char* s = std::getenv( k ); return s ? std::atoi( s ) : d; };
	auto envF = []( const char* k, float d ) { const char* s = std::getenv( k ); return s ? ( float )std::atof( s ) : d; };
	const int N = envI( "UG_N", 32 ), SAMP = envI( "UG_SAMP", 40 ), MAXC = envI( "UG_CASTERS", 120 ), MAXTRI = envI( "UG_MAXTRI", 1500 );
	const int M = envI( "UG_M", 8 ), Kbits = envI( "UG_K", 32 );
	const float G = envF( "UG_G", 8.0f );
	const int KLIST[7] = { 8, 16, 32, 64, 128, 256, 100000 };	// last = ALL (== D floor)
	const int NK = 7;

	std::vector<std::vector<std::pair<float3, float3>>> lightRecv( cap.lights.size() );
	for( const capReceiver_t& r : cap.receivers )
	{
		if( r.lightIndex >= cap.lights.size() ) { continue; }
		for( uint32_t t = 0; t < r.numIndex / 3; t++ )
		{
			const uint32_t i0 = cap.recvIdx[r.firstIndex + t * 3 + 0], i1 = cap.recvIdx[r.firstIndex + t * 3 + 1], i2 = cap.recvIdx[r.firstIndex + t * 3 + 2];
			float3 p0( cap.recvVerts[i0 * 3], cap.recvVerts[i0 * 3 + 1], cap.recvVerts[i0 * 3 + 2] );
			float3 p1( cap.recvVerts[i1 * 3], cap.recvVerts[i1 * 3 + 1], cap.recvVerts[i1 * 3 + 2] );
			float3 p2( cap.recvVerts[i2 * 3], cap.recvVerts[i2 * 3 + 1], cap.recvVerts[i2 * 3 + 2] );
			float3 nf = cross( p1 - p0, p2 - p0 ); float l = std::sqrt( dot( nf, nf ) ); nf = l > 1e-9f ? nf * ( 1.0f / l ) : float3( 0, 0, 1 );
			lightRecv[r.lightIndex].push_back( { ( p0 + p1 + p2 ) * ( 1.0f / 3.0f ), nf } );
		}
	}
	auto gc = []( const float3& p, int i ) { return i == 0 ? p.x : ( i == 1 ? p.y : p.z ); };
	auto sc = []( float3 & p, int i, float val ) { if( i == 0 ) { p.x = val; } else if( i == 1 ) { p.y = val; } else { p.z = val; } };

	const int NB = 10;
	long okR[NK] = {}, kNeed[NK + 1] = {};			// %<0.06 per K ; min-K histogram (last bucket = none)
	double eR32[NB] = {}, uni_[NB] = {}; long bN[NB] = {};	// per-penumbra err for the K=32 candidate
	long nSamp = 0; double meanFullTri = 0; int usedC = 0;
	// SHIPPED-POLICY validation: reduced mode keeps occluders with cell-max-solo > CUTOFF, capped at K (not
	// pure top-K). Since rank is sorted desc, "solo>cutoff" is a prefix, so this measures the exact shader
	// selection. Sweep cutoff to justify the r_softShadowSurfCacheReducedCutoff default; capK matches K.
	const float CUT[4] = { 0.001f, 0.003f, 0.01f, 0.03f };
	const int capK = envI( "UG_CAPK", 64 );
	long okCut[4] = {}; double setSz[4] = {};

	for( uint32_t c = 0; c < cap.casters.size() && usedC < MAXC; c++ )
	{
		const capCaster_t& cs = cap.casters[c];
		const int nT = ( int )( cs.numIndex / 3 );
		if( nT < 1 || nT > MAXTRI || cs.lightIndex >= cap.lights.size() ) { continue; }
		const std::vector<std::pair<float3, float3>>& recv = lightRecv[cs.lightIndex];
		if( recv.empty() ) { continue; }
		const float3 L( cap.lights[cs.lightIndex].origin[0], cap.lights[cs.lightIndex].origin[1], cap.lights[cs.lightIndex].origin[2] );
		const float  swR = std::fmax( cap.lights[cs.lightIndex].penumbraSize, 1e-2f ), swR2 = swR * swR;
		const float* rv = cap.meshVerts.data();
		const uint32_t* ri = &cap.meshIdx[cs.firstIndex];
		const size_t stride = std::max( ( size_t )1, recv.size() / ( size_t )SAMP );
		bool anyPen = false;
		for( size_t s = 0; s < recv.size(); s += stride )
		{
			const float3 P = recv[s].first, nrmR = recv[s].second;
			const float truth = 1.0f - MeshTruthShadowSoup( rv, ri, cs.numIndex, P, L, swR, N );
			if( truth < 0.002f ) { continue; }
			anyPen = true;
			const float3 an( std::fabs( nrmR.x ), std::fabs( nrmR.y ), std::fabs( nrmR.z ) );
			const int d = ( an.x >= an.y && an.x >= an.z ) ? 0 : ( ( an.y >= an.z ) ? 1 : 2 );
			const int a1 = ( d == 0 ) ? 1 : ( ( d == 1 ) ? 2 : 0 ), a2 = ( d == 0 ) ? 2 : ( ( d == 1 ) ? 0 : 1 );
			const float cu = std::floor( gc( P, a1 ) / G ), cv = std::floor( gc( P, a2 ) / G );
			float3 C = P; sc( C, a1, ( cu + 0.5f ) * G ); sc( C, a2, ( cv + 0.5f ) * G );
			// rank tris by MAX solo coverage over the whole CELL (centre + 4 corners): the relevant triangle
			// SET is P-dependent (a tri may shadow a corner but not the centre), so a centre-only rank drops
			// tris that matter off-centre. 5 sample points capture the cell's full occluder set.
			float3 cellPt[5]; softFrame_t cellF[5]; cellPt[0] = C; cellF[0] = SoftShadow_Frame( C, L );
			const float ox[4] = { 0, 1, 0, 1 }, oy[4] = { 0, 0, 1, 1 };
			for( int k = 0; k < 4; k++ ) { float3 Q = P; sc( Q, a1, ( cu + ox[k] ) * G ); sc( Q, a2, ( cv + oy[k] ) * G ); cellPt[k + 1] = Q; cellF[k + 1] = SoftShadow_Frame( Q, L ); }

			std::vector<std::pair<float, int>> rank; rank.reserve( nT );
			for( int t = 0; t < nT; t++ )
			{
				const uint32_t a = ri[t * 3 + 0], b = ri[t * 3 + 1], e = ri[t * 3 + 2];
				float3 v0( rv[a * 3], rv[a * 3 + 1], rv[a * 3 + 2] ), v1( rv[b * 3], rv[b * 3 + 1], rv[b * 3 + 2] ), v2( rv[e * 3], rv[e * 3 + 1], rv[e * 3 + 2] );
				float solo = 0; for( int p5 = 0; p5 < 5; p5++ ) { solo = std::fmax( solo, SurfBuild_SoloCovExact( cellPt[p5], v0, v1, v2, cellF[p5], swR2 ) ); }
				if( solo > 1e-5f ) { rank.push_back( { solo, t } ); }
			}
			std::sort( rank.begin(), rank.end(), []( const std::pair<float, int>& x, const std::pair<float, int>& y ) { return x.first > y.first; } );
			meanFullTri += ( double )rank.size();

			std::vector<uint32_t> sub; sub.reserve( std::min( ( size_t )256, rank.size() ) * 3 );	// top-256 index triples, rank order
			const int cap256 = ( int )std::min( ( size_t )256, rank.size() );
			for( int k = 0; k < cap256; k++ ) { const int t = rank[k].second; sub.push_back( ri[t * 3 + 0] ); sub.push_back( ri[t * 3 + 1] ); sub.push_back( ri[t * 3 + 2] ); }

			int minK = NK;								// smallest KLIST index achieving <0.06
			int bi = ( int )( truth * NB ); if( bi < 0 ) { bi = 0; } if( bi >= NB ) { bi = NB - 1; }
			uni_[bi] += truth; bN[bi]++;
			for( int ki = 0; ki < NK; ki++ )
			{
				const int K = std::min( KLIST[ki], ( int )rank.size() );
				const float covK = ScanlineBitGridCov( P, L, swR, rv, sub.data(), ( uint32_t )( 3 * std::min( K, cap256 ) ), M, Kbits );
				const float err = std::fabs( covK - truth );
				if( err <= 0.06f ) { okR[ki]++; if( ki < minK ) { minK = ki; } }
				if( KLIST[ki] == 32 ) { eR32[bi] += err; }
			}
			kNeed[minK]++;								// minK==NK means none of the listed K reached <0.06
			for( int ci = 0; ci < 4; ci++ )			// shipped policy: keep solo>cutoff (prefix), cap at capK
			{
				int kc = 0; while( kc < ( int )rank.size() && rank[kc].first > CUT[ci] ) { kc++; }
				kc = std::min( kc, std::min( capK, cap256 ) );
				const float covC = ScanlineBitGridCov( P, L, swR, rv, sub.data(), ( uint32_t )( 3 * kc ), M, Kbits );
				if( std::fabs( covC - truth ) <= 0.06f ) { okCut[ci]++; }
				setSz[ci] += kc;
			}
			nSamp++;
		}
		if( anyPen ) { usedC++; }
	}

	if( nSamp == 0 ) { std::printf( "    [redset] no shadowing samples\n" ); CHECK( true ); return; }
	std::printf( "    [redset] %s | casters %d, samples %ld, grid %dx%d, cell G=%.1f, mean occluders/texel %.0f\n",
				 path, usedC, nSamp, M, Kbits, G, meanFullTri / nSamp );
	std::printf( "    [redset] %% |err|<=0.06 by top-K (K=all == D floor):\n" );
	for( int ki = 0; ki < NK; ki++ )
	{
		char kb[16]; if( KLIST[ki] >= 100000 ) { std::snprintf( kb, sizeof( kb ), "ALL" ); } else { std::snprintf( kb, sizeof( kb ), "%d", KLIST[ki] ); }
		std::printf( "        K=%-5s  %.1f%%\n", kb, 100.0 * okR[ki] / nSamp );
	}
	std::printf( "    [redset] min-K needed for <0.06 (cumulative reprojectable-cache sizing):\n" );
	long cum = 0;
	for( int ki = 0; ki < NK; ki++ )
	{
		cum += kNeed[ki];
		char kb[16]; if( KLIST[ki] >= 100000 ) { std::snprintf( kb, sizeof( kb ), "ALL" ); } else { std::snprintf( kb, sizeof( kb ), "<=%d", KLIST[ki] ); }
		std::printf( "        %-7s  %.1f%% of texels\n", kb, 100.0 * cum / nSamp );
	}
	std::printf( "        none    %.1f%% of texels (even ALL tris > 0.06 = grid discretisation, not set size)\n", 100.0 * kNeed[NK] / nSamp );
	std::printf( "    [redset] SHIPPED POLICY (keep solo>cutoff, cap K=%d) - validates r_softShadowSurfCacheReducedCutoff:\n", capK );
	for( int ci = 0; ci < 4; ci++ ) { std::printf( "        cutoff %.3f  %.1f%% <0.06  | mean set %.1f tris\n", CUT[ci], 100.0 * okCut[ci] / nSamp, setSz[ci] / nSamp ); }
	std::printf( "    [redset] penumbra bin -> n | union | meanErr K=32\n" );
	for( int b = 0; b < NB; b++ ) { if( bN[b] ) { std::printf( "      occ [%.1f,%.1f)  n%6ld  union %.3f  K32 %.4f\n", b * 0.1, b * 0.1 + 0.1, bN[b], uni_[b] / bN[b], eR32[b] / bN[b] ); } }
	CHECK( true );
}

// WINDING / TOPOLOGY of the captured caster meshes - is the winding actually INCONSISTENT, and is it
// FIXABLE? Method: weld coincident verts by position (Doom3 duplicates verts at brush/model junctions,
// so raw indices lie about sharing), then over the welded topology per caster:
//  - classify each edge by share count: 1 = BOUNDARY (open), 2 = MANIFOLD, >2 = NON-MANIFOLD (unfixable).
//  - a manifold edge is CONSISTENT iff its two triangles traverse it in OPPOSITE directions.
//  - ORIENTABILITY (fixability by a global per-triangle flip): union-find with parity over triangles
//    linked by manifold edges; a contradiction (odd cycle) = NON-orientable = winding cannot be made
//    consistent by flipping. Non-manifold edges also make it unfixable (no consistent silhouette exists).
// Verdict per caster: CLEAN (closed, manifold, orientable) -> the analytic silhouette WOULD work here,
// so "inconsistent winding" is fixable and prior closing arguments can be relitigated. Otherwise report
// which failure dominates (open boundary vs non-manifold vs non-orientable).
STUDY_TEST( SoftShadowWinding, quantify )
{
	const char* path = std::getenv( "CAP" );
	if( path == NULL ) { std::printf( "    [winding] CAP unset; skipping\n" ); CHECK( true ); return; }
	Cap cap;
	if( !LoadCap( path, cap ) ) { std::printf( "    [winding] cannot load %s\n", path ); CHECK( false ); return; }
	auto envI = []( const char* k, int d ) { const char* s = std::getenv( k ); return s ? std::atoi( s ) : d; };
	auto envF = []( const char* k, float d ) { const char* s = std::getenv( k ); return s ? ( float )std::atof( s ) : d; };
	const int MINTRI = envI( "WD_MINTRI", 32 ), MAXTRI = envI( "WD_MAXTRI", 40000 );
	const float WELD = envF( "WD_WELD", 0.05f );

	long totB = 0, totM = 0, totNM = 0, totCons = 0, totIncons = 0;
	long cTot = 0, cClean = 0, cOrientable = 0, cClosed = 0, cAlreadyConsistent = 0, cNonManifold = 0, cNonOrientable = 0, cOpen = 0;
	double sumBoundaryFrac = 0;
	long inconB2B = 0, inconDup = 0, inconFold = 0;		// inconsistent-edge kinds: back-to-back / duplicate / genuine fold
	auto triNrm = [&]( const capCaster_t& cs2, int t ) -> float3
	{
		const uint32_t a = cap.meshIdx[cs2.firstIndex + t * 3 + 0], b = cap.meshIdx[cs2.firstIndex + t * 3 + 1], d = cap.meshIdx[cs2.firstIndex + t * 3 + 2];
		float3 v0( cap.meshVerts[a * 3], cap.meshVerts[a * 3 + 1], cap.meshVerts[a * 3 + 2] );
		float3 v1( cap.meshVerts[b * 3], cap.meshVerts[b * 3 + 1], cap.meshVerts[b * 3 + 2] );
		float3 v2( cap.meshVerts[d * 3], cap.meshVerts[d * 3 + 1], cap.meshVerts[d * 3 + 2] );
		float3 n = cross( v1 - v0, v2 - v0 ); float l = std::sqrt( dot( n, n ) );
		return l > 1e-12f ? n * ( 1.0f / l ) : float3( 0, 0, 0 );
	};

	for( uint32_t c = 0; c < cap.casters.size(); c++ )
	{
		const capCaster_t& cs = cap.casters[c];
		const int nT = ( int )( cs.numIndex / 3 );
		if( nT < MINTRI || nT > MAXTRI ) { continue; }
		cTot++;

		std::unordered_map<uint64_t, int> weldMap;
		auto weld = [&]( uint32_t gi ) -> int
		{
			const float* p = &cap.meshVerts[gi * 3];
			int64_t x = ( int64_t )std::llround( p[0] / WELD ), y = ( int64_t )std::llround( p[1] / WELD ), z = ( int64_t )std::llround( p[2] / WELD );
			uint64_t k = ( ( uint64_t )( uint32_t )( x + 1048576 ) << 42 ) | ( ( uint64_t )( uint32_t )( y + 1048576 ) << 21 ) | ( uint64_t )( uint32_t )( z + 1048576 );  // collision-free pack (|coord| < 2^20 quantized units)
			auto it = weldMap.find( k );
			if( it != weldMap.end() ) { return it->second; }
			int id = ( int )weldMap.size(); weldMap[k] = id; return id;
		};
		std::vector<int> tw( nT * 3 );
		for( int t = 0; t < nT; t++ )
			for( int k = 0; k < 3; k++ ) { tw[t * 3 + k] = weld( cap.meshIdx[cs.firstIndex + t * 3 + k] ); }

		struct ERec { int t; bool lh; };
		std::unordered_map<uint64_t, std::vector<ERec>> edges;
		for( int t = 0; t < nT; t++ )
			for( int k = 0; k < 3; k++ )
			{
				int a = tw[t * 3 + k], b = tw[t * 3 + ( k + 1 ) % 3];
				if( a == b ) { continue; }
				int lo = a < b ? a : b, hi = a < b ? b : a;
				edges[( ( uint64_t )( uint32_t )lo << 32 ) | ( uint32_t )hi].push_back( { t, a < b } );
			}

		// union-find with parity (0 = same flip, 1 = opposite flip) over triangles
		std::vector<int> uf( nT ), par( nT, 0 );
		for( int i = 0; i < nT; i++ ) { uf[i] = i; }
		auto find = [&]( int x, int& p ) -> int { p = 0; while( uf[x] != x ) { p ^= par[x]; x = uf[x]; } return x; };

		long lB = 0, lM = 0, lNM = 0, lCons = 0, lIncons = 0; bool orientable = true, hasNonManifold = false;
		for( auto& e : edges )
		{
			const std::vector<ERec>& r = e.second;
			if( r.size() == 1 ) { lB++; continue; }
			if( r.size() > 2 ) { lNM++; hasNonManifold = true; continue; }
			lM++;
			const bool consistent = ( r[0].lh != r[1].lh );	// opposite traversal = consistently wound
			if( consistent ) { lCons++; }
			else
			{
				lIncons++;
				const float cosA = dot( triNrm( cs, r[0].t ), triNrm( cs, r[1].t ) );
				if( cosA < -0.99f ) { inconB2B++; }			// normals ~opposite: back-to-back double-sided face
				else if( cosA > 0.99f ) { inconDup++; }		// normals ~equal: duplicate/coplanar triangle
				else { inconFold++; }						// genuine wrong-wound fold
			}
			const int rel = consistent ? 0 : 1;				// flip parity needed to make consistent
			int pa, pb; int ra = find( r[0].t, pa ), rb = find( r[1].t, pb );
			if( ra == rb ) { if( ( ( pa ^ pb ) & 1 ) != rel ) { orientable = false; } }
			else { uf[ra] = rb; par[ra] = ( pa ^ pb ^ rel ) & 1; }
		}
		totB += lB; totM += lM; totNM += lNM; totCons += lCons; totIncons += lIncons;
		const double bf = ( lB + lM + lNM ) ? ( double )lB / ( lB + lM + lNM ) : 0.0;
		sumBoundaryFrac += bf;
		if( lB > 0 ) { cOpen++; }
		if( hasNonManifold ) { cNonManifold++; }
		if( !orientable ) { cNonOrientable++; }
		if( orientable && !hasNonManifold ) { cOrientable++; }
		if( lB == 0 ) { cClosed++; }
		if( lIncons == 0 && lM > 0 ) { cAlreadyConsistent++; }
		if( lB == 0 && !hasNonManifold && orientable ) { cClean++; }	// closed + manifold + orientable
	}

	if( cTot == 0 ) { std::printf( "    [winding] no casters in [%d,%d] tris\n", MINTRI, MAXTRI ); CHECK( true ); return; }
	const long totE = totB + totM + totNM;
	std::printf( "    [winding] %s | %ld casters (>=%d tris), weld %.2fu\n", path, cTot, MINTRI, WELD );
	std::printf( "    [winding] EDGES: boundary %.1f%% | manifold %.1f%% | NON-manifold %.1f%%   (of %ld welded edges)\n",
				 100.0 * totB / totE, 100.0 * totM / totE, 100.0 * totNM / totE, totE );
	std::printf( "    [winding] of MANIFOLD edges: consistently wound %.1f%% | inconsistent %.1f%% (%ld edges)\n",
				 totM ? 100.0 * totCons / totM : 0.0, totM ? 100.0 * totIncons / totM : 0.0, totIncons );
	std::printf( "    [winding] the INCONSISTENT edges are: back-to-back double-sided %.1f%% | duplicate/coplanar %.1f%% | GENUINE wrong-wound fold %.1f%%\n",
				 totIncons ? 100.0 * inconB2B / totIncons : 0.0, totIncons ? 100.0 * inconDup / totIncons : 0.0, totIncons ? 100.0 * inconFold / totIncons : 0.0 );
	std::printf( "    [winding] mean boundary-edge fraction per caster: %.1f%% (open surfaces have a real silhouette hole)\n", 100.0 * sumBoundaryFrac / cTot );
	std::printf( "    [winding] CASTER verdict: CLEAN(closed+manifold+orientable) %.1f%% | already-consistent %.1f%% | orientable(fixable) %.1f%%\n",
				 100.0 * cClean / cTot, 100.0 * cAlreadyConsistent / cTot, 100.0 * cOrientable / cTot );
	std::printf( "    [winding] failure modes: OPEN(has boundary) %.1f%% | NON-manifold %.1f%% | NON-orientable %.1f%%\n",
				 100.0 * cOpen / cTot, 100.0 * cNonManifold / cTot, 100.0 * cNonOrientable / cTot );
	CHECK( true );
}

// THE TEST (winding now known consistent): does the ANALYTIC SILHOUETTE coverage match the ray-truth
// UNION on the real, open, single-sided capture geometry? If yes across the penumbra, we have a
// banding-free general-case coverage the analytic-drains/inconsistent-winding arguments wrongly foreclosed.
STUDY_TEST( SoftShadowSilhouetteCov, quantify )
{
	const char* path = std::getenv( "CAP" );
	if( path == NULL ) { std::printf( "    [silcov] CAP unset; skipping\n" ); CHECK( true ); return; }
	Cap cap;
	if( !LoadCap( path, cap ) ) { std::printf( "    [silcov] cannot load %s\n", path ); CHECK( false ); return; }
	auto envI = []( const char* k, int d ) { const char* s = std::getenv( k ); return s ? std::atoi( s ) : d; };
	auto envF = []( const char* k, float d ) { const char* s = std::getenv( k ); return s ? ( float )std::atof( s ) : d; };
	const int N = envI( "UG_N", 32 ), SAMP = envI( "UG_SAMP", 60 ), MAXC = envI( "UG_CASTERS", 200 ), MAXTRI = envI( "UG_MAXTRI", 4000 );
	const float WELD = envF( "WD_WELD", 0.05f );

	std::vector<std::vector<float3>> lightRecv( cap.lights.size() );
	for( const capReceiver_t& r : cap.receivers )
	{
		if( r.lightIndex >= cap.lights.size() ) { continue; }
		for( uint32_t t = 0; t < r.numIndex / 3; t++ )
		{
			const uint32_t i0 = cap.recvIdx[r.firstIndex + t * 3 + 0], i1 = cap.recvIdx[r.firstIndex + t * 3 + 1], i2 = cap.recvIdx[r.firstIndex + t * 3 + 2];
			float3 p0( cap.recvVerts[i0 * 3], cap.recvVerts[i0 * 3 + 1], cap.recvVerts[i0 * 3 + 2] );
			float3 p1( cap.recvVerts[i1 * 3], cap.recvVerts[i1 * 3 + 1], cap.recvVerts[i1 * 3 + 2] );
			float3 p2( cap.recvVerts[i2 * 3], cap.recvVerts[i2 * 3 + 1], cap.recvVerts[i2 * 3 + 2] );
			lightRecv[r.lightIndex].push_back( ( p0 + p1 + p2 ) * ( 1.0f / 3.0f ) );
		}
	}

	const int NB = 10;
	double bAbs[NB] = {}, bSgn[NB] = {}, bSil[NB] = {}, bUni[NB] = {}; long bN[NB] = {};
	long nSamp = 0, nPen = 0, nPenNear = 0; int usedC = 0;
	for( uint32_t c = 0; c < cap.casters.size() && usedC < MAXC; c++ )
	{
		const capCaster_t& cs = cap.casters[c];
		const int nT = ( int )( cs.numIndex / 3 );
		if( nT < 1 || nT > MAXTRI || cs.lightIndex >= cap.lights.size() ) { continue; }
		const std::vector<float3>& recv = lightRecv[cs.lightIndex];
		if( recv.empty() ) { continue; }
		const float3 L( cap.lights[cs.lightIndex].origin[0], cap.lights[cs.lightIndex].origin[1], cap.lights[cs.lightIndex].origin[2] );
		const float  swR = std::fmax( cap.lights[cs.lightIndex].penumbraSize, 1e-2f );
		const std::vector<RAEdge2> edges = BuildWeldedCasterEdges( cap, cs, WELD );
		const float* rv = cap.meshVerts.data();
		const uint32_t* ri = &cap.meshIdx[cs.firstIndex];
		const size_t stride = std::max( ( size_t )1, recv.size() / ( size_t )SAMP );
		bool anyPen = false;
		for( size_t s = 0; s < recv.size(); s += stride )
		{
			const float3 P = recv[s];
			const float uni = 1.0f - MeshTruthShadowSoup( rv, ri, cs.numIndex, P, L, swR, N );
			if( uni < 0.002f ) { continue; }
			anyPen = true;
			const float sil = SilhouetteCov( edges, P, L, swR );
			const float gap = sil - uni;
			int bi = ( int )( uni * NB ); if( bi < 0 ) { bi = 0; } if( bi >= NB ) { bi = NB - 1; }
			bAbs[bi] += std::fabs( gap ); bSgn[bi] += gap; bSil[bi] += sil; bUni[bi] += uni; bN[bi]++;
			nSamp++;
			if( uni < 0.998f ) { nPen++; if( std::fabs( gap ) <= 0.06f ) { nPenNear++; } }
		}
		if( anyPen ) { usedC++; }
	}
	if( nSamp == 0 ) { std::printf( "    [silcov] no shadowing samples\n" ); CHECK( true ); return; }
	std::printf( "    [silcov] %s | casters %d, samples %ld, N=%d\n", path, usedC, nSamp, N );
	std::printf( "    [silcov] ANALYTIC silhouette vs UNION: |gap|<=0.06 on %.1f%% of PENUMBRA samples (nPen %ld)\n",
				 nPen ? 100.0 * nPenNear / nPen : 0.0, nPen );
	std::printf( "    [silcov] union bin -> n | union | silhouette | |gap| | signed\n" );
	for( int b = 0; b < NB; b++ )
	{
		if( bN[b] == 0 ) { continue; }
		std::printf( "      occ [%.1f,%.1f)  n%6ld  union %.3f  sil %.3f  |gap| %.4f  signed %+.4f\n",
					 b * 0.1, b * 0.1 + 0.1, bN[b], bUni[b] / bN[b], bSil[b] / bN[b], bAbs[b] / bN[b], bSgn[b] / bN[b] );
	}
	CHECK( true );
}

// HARNESS VALIDATION (guard): the analytic silhouette (my SilhouetteCov AND the shipped ProcCaster, via my
// BuildEdgesRaw->ProcRecords path) must match the ray union on a CLOSED box - both do, to ~0.002. This test
// caught a collision-prone vertex weld (an XOR hash key collapsed the symmetric cube's 8 corners to ~3,
// giving 3 edges and coverage 0); the weld is now a collision-free packed key. Real captures use irregular
// coords that never collided, so the capture ProcCaster-vs-union numbers were valid throughout.
TEST( SilhouetteImpl, box_matches_union )
{
	std::vector<float> V; std::vector<uint32_t> I;
	for( int i = 0; i < 8; i++ ) { V.push_back( ( i & 1 ) ? 1.f : -1.f ); V.push_back( ( i & 2 ) ? 1.f : -1.f ); V.push_back( ( i & 4 ) ? 1.f : -1.f ); }
	const int quads[6][4] = { {0, 2, 6, 4}, {1, 5, 7, 3}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 1, 3, 2}, {4, 6, 7, 5} };
	for( int q = 0; q < 6; q++ ) { int a = quads[q][0], b = quads[q][1], c = quads[q][2], d = quads[q][3]; I.push_back( a ); I.push_back( b ); I.push_back( c ); I.push_back( a ); I.push_back( c ); I.push_back( d ); }
	for( size_t k = 0; k < I.size(); k += 3 )		// fix winding OUTWARD (consistent)
	{
		float3 v0( V[I[k] * 3], V[I[k] * 3 + 1], V[I[k] * 3 + 2] ), v1( V[I[k + 1] * 3], V[I[k + 1] * 3 + 1], V[I[k + 1] * 3 + 2] ), v2( V[I[k + 2] * 3], V[I[k + 2] * 3 + 1], V[I[k + 2] * 3 + 2] );
		float3 n = cross( v1 - v0, v2 - v0 ), ctr = ( v0 + v1 + v2 ) * ( 1.f / 3.f );
		if( dot( n, ctr ) < 0.0f ) { uint32_t tmp = I[k + 1]; I[k + 1] = I[k + 2]; I[k + 2] = tmp; }
	}
	std::vector<RAEdge2> edges = BuildEdgesRaw( V.data(), I.data(), ( uint32_t )I.size(), 0.01f );
	int nb = 0; for( const RAEdge2& e : edges ) { if( e.boundary ) { nb++; } }
	const std::vector<float4> cand = ProcRecords( edges );		// validate MY ProcCaster path on a known box
	const float3 L( 0, 0, 30 ); const float R = 5;
	float maxErr = 0, maxProcErr = 0, maxScanErr = 0;
	for( float x = 0; x <= 8.0f; x += 1.0f )
	{
		float3 P( x, 0, -10 );
		float uni = 1.f - MeshTruthShadowSoup( V.data(), I.data(), ( uint32_t )I.size(), P, L, R, 128 );
		float sil = SilhouetteCov( edges, P, L, R );
		float scan = ScanlineUnionCov( P, L, R, V.data(), I.data(), ( uint32_t )I.size(), 64 );
		float grid = ScanlineBitGridCov( P, L, R, V.data(), I.data(), ( uint32_t )I.size(), 16, 32 );
		maxScanErr = std::fmax( maxScanErr, std::fmax( std::fabs( scan - uni ), std::fabs( grid - uni ) ) );
		bool blocks = RayHitsMesh( P, L - P, V.data(), I.data(), ( uint32_t )I.size() );
		int nsil = 0; for( const RAEdge2& e : edges ) { bool fa = dot( e.nA, P - e.a ) > 0.f; bool fb = dot( e.nB, P - e.a ) > 0.f; if( e.boundary || ( fa != fb ) ) { nsil++; } }
		float proc = SoftShadow_ProcCaster( P, L, R, blocks, 0, ( int )edges.size(), SoftEdgeBuffer{ cand.data(), ( int )cand.size() } );
		std::printf( "    [silbox] P.x=%.0f  union %.3f  sil %.3f  ProcCaster %.3f (err %.3f)  blocks=%d nSil=%d nEdge=%d\n", x, uni, sil, proc, std::fabs( proc - uni ), blocks ? 1 : 0, nsil, ( int )edges.size() );
		maxErr = std::fmax( maxErr, std::fabs( sil - uni ) );
		maxProcErr = std::fmax( maxProcErr, std::fabs( proc - uni ) );
	}
	std::printf( "    [silbox] boundary edges=%d (want 0), sil maxErr=%.3f, MY-PATH ProcCaster maxErr=%.3f, SCANLINE maxErr=%.3f (want <0.06)\n", nb, maxErr, maxProcErr, maxScanErr );
	CHECK( nb == 0 );
	CHECK( maxProcErr < 0.06f );		// if MY BuildEdgesRaw->ProcRecords->ProcCaster path is correct, this matches
	CHECK( maxScanErr < 0.06f );		// scanline Fubini union must match the ray union on a closed box
}

// THE DEFINITIVE open-geometry test: feed the SHIPPED SoftShadow_ProcCaster (validated 0.001 on a box,
// F6b - it does the ordered chain-walk + near-plane connectors my SilhouetteCov omitted) the REAL caster
// edge records, and compare to the ray UNION across the penumbra. This settles whether the analytic
// silhouette works on the open/non-manifold capture geometry (winding now known consistent). Cap at
// SW_RA_MAX(1024) edges so ProcCaster never clamps.
STUDY_TEST( SoftShadowProcVsUnion, quantify )
{
	const char* path = std::getenv( "CAP" );
	if( path == NULL ) { std::printf( "    [procun] CAP unset; skipping\n" ); CHECK( true ); return; }
	Cap cap;
	if( !LoadCap( path, cap ) ) { std::printf( "    [procun] cannot load %s\n", path ); CHECK( false ); return; }
	auto envI = []( const char* k, int d ) { const char* s = std::getenv( k ); return s ? std::atoi( s ) : d; };
	auto envF = []( const char* k, float d ) { const char* s = std::getenv( k ); return s ? ( float )std::atof( s ) : d; };
	const int N = envI( "UG_N", 32 ), SAMP = envI( "UG_SAMP", 50 ), MAXC = envI( "UG_CASTERS", 200 ), MAXTRI = envI( "UG_MAXTRI", 300 );
	const float WELD = envF( "WD_WELD", 0.05f );
	const float FN = envI( "UG_FLIPN", 0 ) ? -1.0f : 1.0f;		// negate face normals (test inward-vs-outward winding convention)

	std::vector<std::vector<float3>> lightRecv( cap.lights.size() );
	for( const capReceiver_t& r : cap.receivers )
	{
		if( r.lightIndex >= cap.lights.size() ) { continue; }
		for( uint32_t t = 0; t < r.numIndex / 3; t++ )
		{
			const uint32_t i0 = cap.recvIdx[r.firstIndex + t * 3 + 0], i1 = cap.recvIdx[r.firstIndex + t * 3 + 1], i2 = cap.recvIdx[r.firstIndex + t * 3 + 2];
			float3 p0( cap.recvVerts[i0 * 3], cap.recvVerts[i0 * 3 + 1], cap.recvVerts[i0 * 3 + 2] );
			float3 p1( cap.recvVerts[i1 * 3], cap.recvVerts[i1 * 3 + 1], cap.recvVerts[i1 * 3 + 2] );
			float3 p2( cap.recvVerts[i2 * 3], cap.recvVerts[i2 * 3 + 1], cap.recvVerts[i2 * 3 + 2] );
			lightRecv[r.lightIndex].push_back( ( p0 + p1 + p2 ) * ( 1.0f / 3.0f ) );
		}
	}

	const int NB = 10;
	double bAbs[NB] = {}, bSgn[NB] = {}, bProc[NB] = {}, bUni[NB] = {}; long bN[NB] = {};
	long nPen = 0, nPenNear = 0; int usedC = 0, skippedBig = 0;
	// split by caster openness: does the ProcCaster gap correlate with boundary edges? (repair hypothesis)
	double clAbs = 0, opAbs = 0, clUmbAbs = 0, opUmbAbs = 0; long clN = 0, opN = 0, clUmbN = 0, opUmbN = 0;
	for( uint32_t c = 0; c < cap.casters.size() && usedC < MAXC; c++ )
	{
		const capCaster_t& cs = cap.casters[c];
		const int nT = ( int )( cs.numIndex / 3 );
		if( nT < 1 || nT > MAXTRI || cs.lightIndex >= cap.lights.size() ) { continue; }
		const std::vector<float3>& recv = lightRecv[cs.lightIndex];
		if( recv.empty() ) { continue; }
		const std::vector<RAEdge2> edges = BuildWeldedCasterEdges( cap, cs, WELD );
		if( ( int )edges.size() > 1024 ) { skippedBig++; continue; }		// would clamp SW_RA_MAX
		long nbnd = 0; for( const RAEdge2& e : edges ) { if( e.boundary ) { nbnd++; } }
		const bool clean = !edges.empty() && ( double )nbnd / edges.size() < 0.03;	// ~closed (few boundary edges)
		std::vector<float4> cand; cand.reserve( edges.size() * 4 );
		for( const RAEdge2& e : edges )
		{
			cand.push_back( float4( e.a.x, e.a.y, e.a.z, ( float )e.va ) );
			cand.push_back( float4( e.b.x, e.b.y, e.b.z, ( float )e.vb ) );
			cand.push_back( float4( e.nA.x * FN, e.nA.y * FN, e.nA.z * FN, e.boundary ? 1.0f : 0.0f ) );
			cand.push_back( float4( e.nB.x * FN, e.nB.y * FN, e.nB.z * FN, 0.0f ) );
		}
		const float3 L( cap.lights[cs.lightIndex].origin[0], cap.lights[cs.lightIndex].origin[1], cap.lights[cs.lightIndex].origin[2] );
		const float  swR = std::fmax( cap.lights[cs.lightIndex].penumbraSize, 1e-2f );
		const float* rv = cap.meshVerts.data();
		const uint32_t* ri = &cap.meshIdx[cs.firstIndex];
		const size_t stride = std::max( ( size_t )1, recv.size() / ( size_t )SAMP );
		bool anyPen = false;
		for( size_t s = 0; s < recv.size(); s += stride )
		{
			const float3 P = recv[s];
			const float uni = 1.0f - MeshTruthShadowSoup( rv, ri, cs.numIndex, P, L, swR, N );
			if( uni < 0.002f ) { continue; }
			anyPen = true;
			const bool blocks = RayHitsMesh( P, L - P, rv, ri, cs.numIndex );
			const float proc = SoftShadow_ProcCaster( P, L, swR, blocks, 0, ( int )edges.size(), SoftEdgeBuffer{ cand.data(), ( int )cand.size() } );
			const float gap = proc - uni;
			int bi = ( int )( uni * NB ); if( bi < 0 ) { bi = 0; } if( bi >= NB ) { bi = NB - 1; }
			bAbs[bi] += std::fabs( gap ); bSgn[bi] += gap; bProc[bi] += proc; bUni[bi] += uni; bN[bi]++;
			nPen++; if( uni < 0.998f && std::fabs( gap ) <= 0.06f ) { nPenNear++; }
			if( clean ) { clAbs += std::fabs( gap ); clN++; if( uni >= 0.9f ) { clUmbAbs += std::fabs( gap ); clUmbN++; } }
			else        { opAbs += std::fabs( gap ); opN++; if( uni >= 0.9f ) { opUmbAbs += std::fabs( gap ); opUmbN++; } }
		}
		if( anyPen ) { usedC++; }
	}
	long tot = 0; for( int b = 0; b < NB; b++ ) { tot += bN[b]; }
	if( tot == 0 ) { std::printf( "    [procun] no shadowing samples (skippedBig %d)\n", skippedBig ); CHECK( true ); return; }
	std::printf( "    [procun] %s | casters %d (skippedBig %d), samples %ld, N=%d\n", path, usedC, skippedBig, tot, N );
	std::printf( "    [procun] SHIPPED ProcCaster vs UNION: |gap|<=0.06 on %.1f%% of penumbra samples\n", nPen ? 100.0 * nPenNear / nPen : 0.0 );
	std::printf( "    [procun] REPAIR test (does the gap correlate with openness?): CLEAN casters (bnd<3%%) mean|gap| %.4f (umbra %.4f, n%ld) | OPEN casters mean|gap| %.4f (umbra %.4f, n%ld)\n",
				 clN ? clAbs / clN : 0.0, clUmbN ? clUmbAbs / clUmbN : 0.0, clN, opN ? opAbs / opN : 0.0, opUmbN ? opUmbAbs / opUmbN : 0.0, opN );
	std::printf( "    [procun] union bin -> n | union | ProcCaster | |gap| | signed\n" );
	for( int b = 0; b < NB; b++ )
	{
		if( bN[b] == 0 ) { continue; }
		std::printf( "      occ [%.1f,%.1f)  n%6ld  union %.3f  proc %.3f  |gap| %.4f  signed %+.4f\n",
					 b * 0.1, b * 0.1 + 0.1, bN[b], bUni[b] / bN[b], bProc[b] / bN[b], bAbs[b] / bN[b], bSgn[b] / bN[b] );
	}
	CHECK( true );
}

// PROTOTYPE the union-algebra identities (plan noble-sniffing-rose). Per receiver sample on real casters,
// compare candidate estimators to the ray UNION across the penumbra:
//   union      = ray oracle (truth)
//   sumClamped = saturate(Sum solo)                    (inclusion-exclusion 1st term; OVERcounts overlap)
//   indep      = 1 - PROD(1 - solo) = 1 - exp(Sum log(1-solo))   (independence; bounded, saturates to umbra)
//   maxSolo    = max solo                              (lower bracket: maxSolo <= union <= sumClamped)
//   corr       = lerp(indep, sumClamped, BETA)         (does one global knob close the tiling undercount?)
// Winding-agnostic (SurfBuild_SoloCovExact uses |area|), banding-free (continuous solo). Env UG_BETA (0..1).
STUDY_TEST( SoftShadowIndepProduct, quantify )
{
	const char* path = std::getenv( "CAP" );
	if( path == NULL ) { std::printf( "    [indep] CAP unset; skipping\n" ); CHECK( true ); return; }
	Cap cap;
	if( !LoadCap( path, cap ) ) { std::printf( "    [indep] cannot load %s\n", path ); CHECK( false ); return; }
	auto envI = []( const char* k, int d ) { const char* s = std::getenv( k ); return s ? std::atoi( s ) : d; };
	auto envF = []( const char* k, float d ) { const char* s = std::getenv( k ); return s ? ( float )std::atof( s ) : d; };
	const int N = envI( "UG_N", 32 ), SAMP = envI( "UG_SAMP", 60 ), MAXC = envI( "UG_CASTERS", 200 ), MAXTRI = envI( "UG_MAXTRI", 1500 );
	const float BETA = envF( "UG_BETA", 0.5f );

	std::vector<std::vector<float3>> lightRecv( cap.lights.size() );
	for( const capReceiver_t& r : cap.receivers )
	{
		if( r.lightIndex >= cap.lights.size() ) { continue; }
		for( uint32_t t = 0; t < r.numIndex / 3; t++ )
		{
			const uint32_t i0 = cap.recvIdx[r.firstIndex + t * 3 + 0], i1 = cap.recvIdx[r.firstIndex + t * 3 + 1], i2 = cap.recvIdx[r.firstIndex + t * 3 + 2];
			float3 p0( cap.recvVerts[i0 * 3], cap.recvVerts[i0 * 3 + 1], cap.recvVerts[i0 * 3 + 2] );
			float3 p1( cap.recvVerts[i1 * 3], cap.recvVerts[i1 * 3 + 1], cap.recvVerts[i1 * 3 + 2] );
			float3 p2( cap.recvVerts[i2 * 3], cap.recvVerts[i2 * 3 + 1], cap.recvVerts[i2 * 3 + 2] );
			lightRecv[r.lightIndex].push_back( ( p0 + p1 + p2 ) * ( 1.0f / 3.0f ) );
		}
	}

	const int NB = 10;
	double bUni[NB] = {}, bInd[NB] = {}, bSum[NB] = {}, bIndAbs[NB] = {}, bSumAbs[NB] = {}, bCorAbs[NB] = {}, bMaxAbs[NB] = {}, bMax[NB] = {}, bScan[NB] = {}, bScanAbs[NB] = {}, bGrid[NB] = {}, bGridAbs[NB] = {}; long bN[NB] = {};
	long nPen = 0, indNear = 0, sumNear = 0, corNear = 0, maxNear = 0, scanNear = 0, gridNear = 0, brOK = 0, brInd = 0; int usedC = 0;
	const int SCANM = getenv( "UG_M2" ) ? atoi( getenv( "UG_M2" ) ) : 24;		// scanline chord count (perpendicular resolution)
	for( uint32_t c = 0; c < cap.casters.size() && usedC < MAXC; c++ )
	{
		const capCaster_t& cs = cap.casters[c];
		const int nT = ( int )( cs.numIndex / 3 );
		if( nT < 1 || nT > MAXTRI || cs.lightIndex >= cap.lights.size() ) { continue; }
		const std::vector<float3>& recv = lightRecv[cs.lightIndex];
		if( recv.empty() ) { continue; }
		const float3 L( cap.lights[cs.lightIndex].origin[0], cap.lights[cs.lightIndex].origin[1], cap.lights[cs.lightIndex].origin[2] );
		const float  swR = std::fmax( cap.lights[cs.lightIndex].penumbraSize, 1e-2f );
		const float  swR2 = swR * swR;
		const float* rv = cap.meshVerts.data();
		const uint32_t* ri = &cap.meshIdx[cs.firstIndex];
		const size_t stride = std::max( ( size_t )1, recv.size() / ( size_t )SAMP );
		bool anyPen = false;
		for( size_t s = 0; s < recv.size(); s += stride )
		{
			const float3 P = recv[s];
			const float uni = 1.0f - MeshTruthShadowSoup( rv, ri, cs.numIndex, P, L, swR, N );
			if( uni < 0.002f ) { continue; }
			anyPen = true;
			const softFrame_t f = SoftShadow_Frame( P, L );
			float rawSum = 0.0f, logLit = 0.0f, maxSolo = 0.0f;
			std::unordered_map<uint64_t, double> clu;		// DEPTH-ORDERED: sum solo WITHIN a coplanar surface
			for( int t = 0; t < nT; t++ )
			{
				const uint32_t a = cap.meshIdx[cs.firstIndex + t * 3 + 0], b = cap.meshIdx[cs.firstIndex + t * 3 + 1], d = cap.meshIdx[cs.firstIndex + t * 3 + 2];
				float3 v0( rv[a * 3], rv[a * 3 + 1], rv[a * 3 + 2] ), v1( rv[b * 3], rv[b * 3 + 1], rv[b * 3 + 2] ), v2( rv[d * 3], rv[d * 3 + 1], rv[d * 3 + 2] );
				const float solo = SurfBuild_SoloCovExact( P, v0, v1, v2, f, swR2 );
				rawSum += solo;
				const float sc = solo > ( 1.0f - 1e-4f ) ? ( 1.0f - 1e-4f ) : ( solo < 0.0f ? 0.0f : solo );
				logLit += std::log( 1.0f - sc );
				if( solo > maxSolo ) { maxSolo = solo; }
				// plane key: UNORIENTED plane (canonical normal sign) + offset, so a wall's coplanar triangles
				// (and its front/back if two-sided) land in ONE cluster; different surfaces are separate clusters.
				float3 gn = cross( v1 - v0, v2 - v0 ); float gl = std::sqrt( dot( gn, gn ) );
				if( gl > 1e-9f ) { gn = gn * ( 1.0f / gl ); }
				if( gn.x < 0.0f || ( gn.x == 0.0f && ( gn.y < 0.0f || ( gn.y == 0.0f && gn.z < 0.0f ) ) ) ) { gn = gn * -1.0f; }
				int nx = ( int )std::lround( gn.x * 64.0f ) + 128, ny = ( int )std::lround( gn.y * 64.0f ) + 128, nz = ( int )std::lround( gn.z * 64.0f ) + 128;
				int of = ( int )std::lround( dot( gn, v0 ) ); if( of < -8191 ) { of = -8191; } if( of > 8191 ) { of = 8191; }
				uint64_t pk = ( ( uint64_t )( uint32_t )nx ) | ( ( uint64_t )( uint32_t )ny << 9 ) | ( ( uint64_t )( uint32_t )nz << 18 ) | ( ( uint64_t )( uint32_t )( of + 8192 ) << 27 );
				clu[pk] += solo;
			}
			const float sumC = rawSum > 1.0f ? 1.0f : rawSum;
			const float indep = 1.0f - std::exp( logLit );
			double logLitDO = 0.0;						// DEPTH-ORDERED: product ACROSS surfaces of (1 - clusterSum)
			for( const auto& kv : clu ) { double cs2 = kv.second; if( cs2 > 1.0 - 1e-4 ) { cs2 = 1.0 - 1e-4; } if( cs2 < 0.0 ) { cs2 = 0.0; } logLitDO += std::log( 1.0 - cs2 ); }
			const float corr = ( float )( 1.0 - std::exp( logLitDO ) );		// depth-ordered (sum-within / product-across)
			const float scan = ScanlineUnionCov( P, L, swR, rv, ri, cs.numIndex, SCANM );	// FUBINI: exact 1D union per chord
			const float grid = ScanlineBitGridCov( P, L, swR, rv, ri, cs.numIndex, SCANM, 32 );	// GPU-shaped bit-grid (M x 32)
			int bi = ( int )( uni * NB ); if( bi < 0 ) { bi = 0; } if( bi >= NB ) { bi = NB - 1; }
			bUni[bi] += uni; bInd[bi] += indep; bSum[bi] += sumC; bMax[bi] += maxSolo; bScan[bi] += scan; bGrid[bi] += grid;
			bIndAbs[bi] += std::fabs( indep - uni ); bSumAbs[bi] += std::fabs( sumC - uni ); bCorAbs[bi] += std::fabs( corr - uni ); bMaxAbs[bi] += std::fabs( maxSolo - uni ); bScanAbs[bi] += std::fabs( scan - uni ); bGridAbs[bi] += std::fabs( grid - uni );
			bN[bi]++; nPen++;
			if( std::fabs( indep - uni ) <= 0.06f ) { indNear++; }
			if( std::fabs( sumC - uni ) <= 0.06f ) { sumNear++; }
			if( std::fabs( corr - uni ) <= 0.06f ) { corNear++; }
			if( std::fabs( maxSolo - uni ) <= 0.06f ) { maxNear++; }
			if( std::fabs( scan - uni ) <= 0.06f ) { scanNear++; }
			if( std::fabs( grid - uni ) <= 0.06f ) { gridNear++; }
			if( maxSolo <= uni + 1e-3f && uni <= sumC + 1e-3f ) { brOK++; }		// bracket holds
			if( indep >= maxSolo - 1e-3f && indep <= sumC + 1e-3f ) { brInd++; }	// indep between the brackets
		}
		if( anyPen ) { usedC++; }
	}
	if( nPen == 0 ) { std::printf( "    [indep] no shadowing samples\n" ); CHECK( true ); return; }
	std::printf( "    [indep] %s | casters %d, samples %ld, N=%d, BETA=%.2f\n", path, usedC, nPen, N, BETA );
	std::printf( "    [indep] |gap|<=0.06:  GRID(Mx32) %.1f%%  SCAN(M=%d) %.1f%%  MAXSOLO %.1f%%  INDEP %.1f%%  SUM %.1f%%   | bracket maxSolo<=union<=sum holds %.1f%%\n",
				 100.0 * gridNear / nPen, SCANM, 100.0 * scanNear / nPen, 100.0 * maxNear / nPen, 100.0 * indNear / nPen, 100.0 * sumNear / nPen, 100.0 * brOK / nPen );
	std::printf( "    [indep] union bin -> n | union | GRID(|gap|) | SCAN(|gap|) | maxSolo(|gap|) | indep(|gap|) | sum(|gap|)\n" );
	for( int b = 0; b < NB; b++ )
	{
		if( bN[b] == 0 ) { continue; }
		std::printf( "      occ [%.1f,%.1f)  n%6ld  union %.3f  grid %.3f (%.4f)  scan %.3f (%.4f)  max %.3f (%.4f)  indep %.3f (%.4f)  sum %.3f (%.4f)\n",
					 b * 0.1, b * 0.1 + 0.1, bN[b], bUni[b] / bN[b], bGrid[b] / bN[b], bGridAbs[b] / bN[b], bScan[b] / bN[b], bScanAbs[b] / bN[b], bMax[b] / bN[b], bMaxAbs[b] / bN[b], bInd[b] / bN[b], bIndAbs[b] / bN[b], bSum[b] / bN[b], bSumAbs[b] / bN[b] );
	}
	CHECK( true );
}
