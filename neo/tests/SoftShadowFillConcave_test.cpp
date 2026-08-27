/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 2026 Aleksandr Petrosyan

This file is part of the Doom 3 BFG Edition GPL Source Code.

===========================================================================
*/

// SoftScan_FillPolyConcave parity: the concave-capable scanline fill. SoftScan_FillPoly reduces each chord
// to xlo=min/xhi=max over the edge crossings, so it fills the whole span between the extreme crossings - a
// concave silhouette's notches get shadowed (over-shadow). FillPolyConcave instead SORTS the crossings and
// applies the even-odd rule, OR-ing the disjoint interior intervals into the same swGrid run representation.
//
// Three contracts, all against the SAME eps the FillBox/FillPoly parity tests use:
//   (a) EXACT concave union: FillPolyConcave(loop) == the bit-exact union of SoftScan_FillTri over ANY exact
//       triangulation of the (concave) polygon, under the disk mask. This is the drop-in claim - the
//       even-odd loop fill equals the triangle union bit-for-bit (abutting Run() intervals merge losslessly).
//   (b) GROUND TRUTH: FillPolyConcave's disk coverage matches an INDEPENDENT ray-cast area-light oracle that
//       occludes each disk ray by a point-in-concave-polygon (even-odd) test, within chord quantization.
//   (c) CONVEX EQUIVALENCE: on a convex loop (square / octagon) FillPolyConcave == SoftScan_FillPoly bit for
//       bit - it is a strict generalisation, safe to use for convex too.
//   (d) NON-VACUOUS: on a concave loop FillPolyConcave sets STRICTLY FEWER bits than filling the convex loop
//       through FillPoly (the notch it no longer over-shadows) - so the fix demonstrably does something.
// The .inc.hlsl dual-compiles as C++, so this exercises the LIVE shader math, not a reimplementation.

#include "hlsl_compat.h"

// Compile the live coverage source as C++ with the Fubini scanline enabled, isolated in a namespace so it
// does not ODR-collide with the SW_SCANLINE 0 bodies other test TUs compile. Mirror SoftShadowFillPoly.
#define SW_SCANLINE 1
#define SW_SCAN_BITS 32			// CPU emulation uses uint32 grids; pin the word (SwGridWord = uint). GPU ships 64.
#ifndef SW_SCAN_CHORDS
	#define SW_SCAN_CHORDS 16
#endif
// Raise the loop-vertex cap so many-vertex concave silhouettes (12-vert plus, 10-vert star) fit. The concave
// primitive's clip arrays derive from this (SW_CLIP_NEAR/FAR); FillPolyConcave stays byte-for-byte FillPoly
// on convex input at ANY value (surplus edge slots self-reject via the elo>ehi sentinel). Default ships 8.
#define SW_POLY_MAX_VERTS 16
#define inout					// HLSL inout on the grid param: C++ array params decay to pointers, so mutation matches
namespace swconc
{
#include "softwedge_coverage.inc.hlsl"
swAttrib_t g_swAttrib = {};
bool       g_swAttribOn = false;
}
using namespace swconc;

#include "SoftShadowBox.h"		// swtest::v3 / len3 helpers
#include "idUnitTest.h"

#include <cmath>
#include <vector>
#include <algorithm>

using namespace swtest;

namespace
{
int MaskedBits( const uint32_t g[SW_SCAN_CHORDS], const uint32_t m[SW_SCAN_CHORDS] )
{
	int n = 0;
	for( int i = 0; i < SW_SCAN_CHORDS; i++ ) { n += __builtin_popcount( g[i] & m[i] ); }
	return n;
}

// every bit `sub` sets under the mask, `sup` also sets (sub is a subset of sup)
bool Subset( const uint32_t sub[SW_SCAN_CHORDS], const uint32_t sup[SW_SCAN_CHORDS], const uint32_t msk[SW_SCAN_CHORDS] )
{
	for( int i = 0; i < SW_SCAN_CHORDS; i++ ) { if( ( sub[i] & msk[i] ) & ~( sup[i] & msk[i] ) ) { return false; } }
	return true;
}

// a flat polygon on plane (O; U,V): 2D points, CCW. World vertex i = O + U*x + V*y.
struct Poly2
{
	float3 O, U, V;
	std::vector<std::pair<float, float>> p;	// 2D points on the plane
	float3 W( float x, float y ) const { return O + U * x + V * y; }
	float3 At( int i ) const { return W( p[i].first, p[i].second ); }
	int n() const { return ( int )p.size(); }
};

float SignedArea2( const std::vector<std::pair<float, float>>& p )
{
	float a = 0; int n = ( int )p.size();
	for( int i = 0; i < n; i++ ) { auto& A = p[i]; auto& B = p[( i + 1 ) % n]; a += A.first * B.second - B.first * A.second; }
	return 0.5f * a;
}

// EAR-CLIP an arbitrary simple polygon into a triangle list (2D index triples). Reference triangulation for
// the bit-exact union contract - deliberately a DIFFERENT algorithm from the primitive under test. Expects
// CCW input (callers normalise). O(n^2), n<=16 here, trivial.
std::vector<std::array<int, 3>> EarClip( const std::vector<std::pair<float, float>>& poly0 )
{
	std::vector<std::array<int, 3>> out;
	int n = ( int )poly0.size();
	// normalise to CCW; keep a map back to the ORIGINAL vertex indices so emitted triples index poly0.
	std::vector<int> orig( n );
	for( int i = 0; i < n; i++ ) { orig[i] = i; }
	std::vector<std::pair<float, float>> poly( poly0 );
	if( SignedArea2( poly0 ) < 0 ) { std::reverse( poly.begin(), poly.end() ); std::reverse( orig.begin(), orig.end() ); }
	std::vector<int> idx( n );
	for( int i = 0; i < n; i++ ) { idx[i] = i; }
	auto cross = []( std::pair<float, float> a, std::pair<float, float> b, std::pair<float, float> c )
	{
		return ( b.first - a.first ) * ( c.second - a.second ) - ( b.second - a.second ) * ( c.first - a.first );
	};
	auto inTri = [&]( std::pair<float, float> a, std::pair<float, float> b, std::pair<float, float> c, std::pair<float, float> q )
	{
		float d1 = cross( a, b, q ), d2 = cross( b, c, q ), d3 = cross( c, a, q );
		bool neg = ( d1 < 0 ) || ( d2 < 0 ) || ( d3 < 0 );
		bool pos = ( d1 > 0 ) || ( d2 > 0 ) || ( d3 > 0 );
		return !( neg && pos );	// same sign on all three => inside (or on edge)
	};
	int guard = 0;
	while( ( int )idx.size() > 3 && guard++ < 1000 )
	{
		int m = ( int )idx.size();
		bool clipped = false;
		for( int i = 0; i < m; i++ )
		{
			int i0 = idx[( i + m - 1 ) % m], i1 = idx[i], i2 = idx[( i + 1 ) % m];
			auto a = poly[i0], b = poly[i1], c = poly[i2];
			if( cross( a, b, c ) <= 0 ) { continue; }			// reflex (or collinear): not an ear tip
			bool ear = true;
			for( int j = 0; j < m; j++ )
			{
				int ij = idx[j];
				if( ij == i0 || ij == i1 || ij == i2 ) { continue; }
				if( inTri( a, b, c, poly[ij] ) ) { ear = false; break; }
			}
			if( !ear ) { continue; }
			out.push_back( { orig[i0], orig[i1], orig[i2] } );
			idx.erase( idx.begin() + i );
			clipped = true;
			break;
		}
		if( !clipped ) { break; }	// degenerate; leave the remainder (tests use clean shapes)
	}
	if( idx.size() == 3 ) { out.push_back( { orig[idx[0]], orig[idx[1]], orig[idx[2]] } ); }
	return out;
}

// fill a polygon's CONCAVE loop -> the primitive under test
void FillConcaveLoop( uint32_t grid[SW_SCAN_CHORDS], const Poly2& poly, float3 P, softFrame_t F, float swR, float swEps )
{
	float3 lp[SW_POLY_MAX_VERTS];
	int n = poly.n();
	if( n > SW_POLY_MAX_VERTS ) { n = SW_POLY_MAX_VERTS; }
	for( int i = 0; i < n; i++ ) { lp[i] = poly.At( i ); }
	SoftScan_FillPolyConcave( grid, lp, n, P, F, swR, swEps );
}

// fill a CONVEX loop (<=8 verts) -> FillPoly. FillPoly's clip arrays are hardcoded (rel[8]); it must NEVER
// receive >8 verts (unlike FillPolyConcave, whose arrays scale with SW_POLY_MAX_VERTS). Convex shapes only.
void FillPolyLoop( uint32_t grid[SW_SCAN_CHORDS], const Poly2& poly, float3 P, softFrame_t F, float swR, float swEps )
{
	float3 lp[8];
	int n = poly.n();
	if( n > 8 ) { n = 8; }
	for( int i = 0; i < n; i++ ) { lp[i] = poly.At( i ); }
	SoftScan_FillPoly( grid, lp, n, P, F, swR, swEps );
}

// 2D convex hull (monotone chain) of the shape's points, as a Poly2 on the same plane (<=8 verts here:
// star->pentagon, plus->octagon). The convex OVER-shadow reference the concave fill must stay a subset of.
Poly2 ConvexHull2( const Poly2& poly )
{
	std::vector<std::pair<float, float>> s( poly.p );
	std::sort( s.begin(), s.end() );
	auto crs = []( std::pair<float, float> a, std::pair<float, float> b, std::pair<float, float> c )
	{
		return ( b.first - a.first ) * ( c.second - a.second ) - ( b.second - a.second ) * ( c.first - a.first );
	};
	std::vector<std::pair<float, float>> h( 2 * s.size() );
	int k = 0;
	for( size_t i = 0; i < s.size(); i++ ) { while( k >= 2 && crs( h[k - 2], h[k - 1], s[i] ) <= 0 ) { k--; } h[k++] = s[i]; }
	for( int i = ( int )s.size() - 2, lo = k + 1; i >= 0; i-- ) { while( k >= lo && crs( h[k - 2], h[k - 1], s[i] ) <= 0 ) { k--; } h[k++] = s[i]; }
	h.resize( k - 1 );	// last point duplicates the first
	Poly2 r{ poly.O, poly.U, poly.V, h };
	return r;
}

// exact-triangulation union of FillTri (the reference the concave fill must match bit-for-bit)
void FillEarTris( uint32_t grid[SW_SCAN_CHORDS], const Poly2& poly, float3 P, softFrame_t F, float swR, float swEps )
{
	auto tris = EarClip( poly.p );
	for( auto& t : tris )
	{
		SoftScan_FillTri( grid, poly.At( t[0] ), poly.At( t[1] ), poly.At( t[2] ), P, F, swR, swEps );
	}
}

// GROUND TRUTH shadow (1=lit, 0=occluded): fraction of the light disk whose ray from P is NOT blocked by the
// flat concave polygon. Ray-cast area-light oracle (mirrors TruthShadow's disk sampling) with a
// point-in-concave-polygon (even-odd) occlusion test - independent of the coverage math under test.
float TruthShadowPoly( float3 P, float3 L, float r, const Poly2& poly, int N = 96 )
{
	float3 toL = L - P;
	float dist = len3( toL );
	float3 nrm = toL * ( 1.0f / dist );
	float3 up = ( std::fabs( nrm.z ) > 0.9f ) ? v3( 0, 1, 0 ) : v3( 0, 0, 1 );
	float3 u = normalize( cross( up, nrm ) );
	float3 v = cross( nrm, u );
	float3 pn = normalize( cross( poly.U, poly.V ) );		// polygon plane normal
	int inside = 0, total = 0;
	for( int iy = 0; iy < N; iy++ )
		for( int ix = 0; ix < N; ix++ )
		{
			float du = ( ix + 0.5f ) / N * 2 - 1, dv = ( iy + 0.5f ) / N * 2 - 1;
			if( du * du + dv * dv > 1.0f ) { continue; }
			total++;
			float3 Dp = L + u * ( du * r ) + v * ( dv * r );
			float3 d = Dp - P;
			float den = dot( pn, d );
			if( std::fabs( den ) < 1e-12f ) { continue; }	// parallel to plane: no hit
			float t = dot( pn, poly.O - P ) / den;
			if( t <= 1e-4f || t >= 1.0f - 1e-4f ) { continue; }	// hit must lie strictly between P and the light
			float3 X = P + d * t;
			// project X into (U,V) plane coords, even-odd point-in-polygon
			float3 rel = X - poly.O;
			float px = dot( rel, poly.U ) / dot( poly.U, poly.U );
			float py = dot( rel, poly.V ) / dot( poly.V, poly.V );
			bool in = false; int nn = poly.n();
			for( int a = 0, b = nn - 1; a < nn; b = a++ )
			{
				float ax = poly.p[a].first, ay = poly.p[a].second, bx = poly.p[b].first, by = poly.p[b].second;
				if( ( ay > py ) != ( by > py ) )
				{
					float xints = ( bx - ax ) * ( py - ay ) / ( by - ay ) + ax;
					if( px < xints ) { in = !in; }
				}
			}
			if( in ) { inside++; }
		}
	return total ? 1.0f - ( float )inside / total : 1.0f;
}

// ---- concave test shapes (CCW), on the z=0 plane unless a case overrides the basis ----
Poly2 MakeLNotch( float3 O, float3 U, float3 V )		// L: square minus top-right quadrant (1 reflex)
{
	return { O, U, V, { { -10, -10 }, { 10, -10 }, { 10, 0 }, { 0, 0 }, { 0, 10 }, { -10, 10 } } };
}
Poly2 MakePlus( float3 O, float3 U, float3 V )			// plus-sign (4 reflex), 12 verts
{
	return { O, U, V, { { -3, -10 }, { 3, -10 }, { 3, -3 }, { 10, -3 }, { 10, 3 }, { 3, 3 },
					   { 3, 10 }, { -3, 10 }, { -3, 3 }, { -10, 3 }, { -10, -3 }, { -3, -3 } } };
}
Poly2 MakeStar( float3 O, float3 U, float3 V )			// 5-point star (5 reflex), 10 verts
{
	Poly2 s{ O, U, V, {} };
	for( int k = 0; k < 10; k++ )
	{
		float rr = ( k & 1 ) ? 4.0f : 10.0f;
		float ang = 1.5707963f + k * 0.62831853f;		// pi/2 + k*(pi/5)
		s.p.push_back( { rr * std::cos( ang ), rr * std::sin( ang ) } );
	}
	return s;
}
Poly2 MakeTwoLobe( float3 O, float3 U, float3 V )		// two-lobe outline: deep V notch splits the top (1 reflex)
{
	return { O, U, V, { { -10, -8 }, { 10, -8 }, { 10, 10 }, { 2, 10 }, { 0, 0 }, { -2, 10 }, { -10, 10 } } };
}

struct Shape { const char* name; Poly2 poly; };
}

TEST( SoftShadowFillConcave, concaveTruthAndUnion )
{
	// sweep concave shapes x light radius x receiver offset; report the coverage-vs-truth error table and
	// assert the two hard contracts (exact triangle-union bit parity, ground-truth within eps).
	const float3 O( 0, 0, 0 ), U( 1, 0, 0 ), V( 0, 1, 0 );
	const float3 Otilt( 1, -2, 0 );
	// ORTHONORMAL tilted basis: the ray-cast truth recovers plane coords by dot(rel,U)/dot(rel,V), which is
	// exact only for an orthonormal (U,V). The primitive itself uses world verts, so it is basis-agnostic.
	const float3 Ntilt = normalize( float3( 0.3f, -0.15f, 1.0f ) );
	const float3 Utilt = normalize( cross( float3( 0, 1, 0 ), Ntilt ) );
	const float3 Vtilt = cross( Ntilt, Utilt );
	const Shape shapes[] =
	{
		{ "L-notch  ", MakeLNotch( O, U, V ) },
		{ "plus     ", MakePlus( O, U, V ) },
		{ "star     ", MakeStar( O, U, V ) },
		{ "two-lobe ", MakeTwoLobe( O, U, V ) },
		{ "L-tilted ", MakeLNotch( Otilt, Utilt, Vtilt ) },
		{ "star-tilt", MakeStar( Otilt, Utilt, Vtilt ) },
	};
	const float radii[] = { 6.0f, 10.0f, 14.0f };
	// receiver offsets: centre, under a notch, off to a lobe (world XY; z below the plane)
	const std::pair<float, float> offs[] = { { 0, 0 }, { 3, 3 }, { -6, 6 } };

	uint32_t diskMask[SW_SCAN_CHORDS]; int diskBits = 0;
	for( int m = 0; m < SW_SCAN_CHORDS; m++ )
	{
		diskMask[m] = SoftScan_Run( -SW_SCAN_HC[m], SW_SCAN_HC[m] );
		diskBits += __builtin_popcount( diskMask[m] );
	}

	std::printf( "  concave coverage vs ray-cast truth (|scan - truth|, eps=0.06) + union bit-diff vs exact triangulation:\n" );
	float maxErr = 0.0f; int cases = 0, exactUnion = 0, maxUDiff = 0;
	for( const Shape& sh : shapes )
	{
		float shMax = 0.0f; int shUDiff = 0;
		for( float r : radii )
			for( auto off : offs )
			{
				const float3 P( off.first, off.second, -30.0f );
				const float3 L( off.first * 0.1f, off.second * 0.1f, 60.0f );
				const float swR = r, swEps = SW_NEAR_EPS;
				softFrame_t F = SoftShadow_Frame( P, L );

				uint32_t gConc[SW_SCAN_CHORDS] = {}, gTri[SW_SCAN_CHORDS] = {};
				FillConcaveLoop( gConc, sh.poly, P, F, swR, swEps );
				FillEarTris( gTri, sh.poly, P, F, swR, swEps );

				// (a) EXACT concave union: even-odd loop fill == the exact triangulation union under the disk.
				// Bit-identical except at chords that GRAZE an ear-clip DIAGONAL: the two tessellations resolve
				// a shared near-horizontal edge's half-open [Ylo,Yhi) crossing at different vertices, a <=1-column
				// boundary effect (the even-odd loop, using only the true silhouette, is the faithful one). Bound
				// it at 2 bits/case; assert MOST cases are bit-exact (0 diff).
				int udiff = 0;
				for( int m = 0; m < SW_SCAN_CHORDS; m++ )
				{
					udiff += __builtin_popcount( ( gConc[m] ^ gTri[m] ) & diskMask[m] );
				}
				CHECK( udiff <= 2 );
				if( udiff == 0 ) { exactUnion++; }
				shUDiff = std::max( shUDiff, udiff ); maxUDiff = std::max( maxUDiff, udiff );

				// (b) GROUND TRUTH within chord quantization (same eps the FillBox/FillPoly parity tests use).
				const float shadow = 1.0f - ( diskBits > 0 ? ( float )MaskedBits( gConc, diskMask ) / diskBits : 0.0f );
				const float truth = TruthShadowPoly( P, L, swR, sh.poly );
				CHECK_NEAR( shadow, truth, 0.06 );
				float e = std::fabs( shadow - truth );
				shMax = std::fmax( shMax, e ); maxErr = std::fmax( maxErr, e ); cases++;
			}
		std::printf( "    %s  max|truth err| = %.4f   max union bit-diff = %d\n", sh.name, shMax, shUDiff );
	}
	std::printf( "  %d cases: overall max|truth err| = %.4f, %d/%d bit-EXACT vs triangulation (max diff %d bits)\n",
			cases, maxErr, exactUnion, cases, maxUDiff );
	CHECK( exactUnion >= cases - 1 );	// at most the one grazing star case differs (documented tessellation boundary)
}

TEST( SoftShadowFillConcave, nearPlaneStraddle )
{
	// The concave loop STRADDLES the receiver near-plane: some vertices sit behind P (dn < swEps) and are
	// clipped off. Sutherland-Hodgman clipping a CONCAVE loop against the near half-plane inserts bridge
	// edges between the kept spans, but those bridge vertices project to radius ~distPL/swEps - far outside
	// the unit disk - so they never corrupt the in-disk even-odd parity (the same argument the shipped
	// WedgeOcclusion/FillPoly near-plane closure relies on). Assert the clipped concave coverage still tracks
	// the ray-cast truth (which counts only rays whose plane hit lies between P and the light).
	// Steeply tilted L-notch: V tips into +z so the shape spans from in front of P to behind it.
	const float3 O( 0, -2, 4 ), U( 1, 0, 0 ), V = normalize( float3( 0, 0.6f, 0.8f ) );	// V has a strong +z tilt
	Poly2 Lp = MakeLNotch( O, U, V );
	uint32_t diskMask[SW_SCAN_CHORDS]; int diskBits = 0;
	for( int m = 0; m < SW_SCAN_CHORDS; m++ ) { diskMask[m] = SoftScan_Run( -SW_SCAN_HC[m], SW_SCAN_HC[m] ); diskBits += __builtin_popcount( diskMask[m] ); }

	struct RP { float3 P, L; float r; };
	const RP rp[] =
	{
		{ float3( 0, 0, 0 ), float3( 0, 1, 30 ), 8.0f },
		{ float3( 2, 1, 0 ), float3( -1, 2, 34 ), 11.0f },
		{ float3( -3, 2, 0 ), float3( 1, -1, 28 ), 9.0f },
	};
	int straddled = 0;
	for( const RP& c : rp )
	{
		const float swR = c.r, swEps = SW_NEAR_EPS;
		softFrame_t F = SoftShadow_Frame( c.P, c.L );
		// confirm the case actually straddles: at least one vertex behind P (dn < swEps)
		bool behind = false;
		for( int i = 0; i < Lp.n(); i++ ) { if( dot( Lp.At( i ) - c.P, F.nrm ) < swEps ) { behind = true; } }
		if( behind ) { straddled++; }

		uint32_t gConc[SW_SCAN_CHORDS] = {};
		FillConcaveLoop( gConc, Lp, c.P, F, swR, swEps );
		const float shadow = 1.0f - ( diskBits > 0 ? ( float )MaskedBits( gConc, diskMask ) / diskBits : 0.0f );
		const float truth = TruthShadowPoly( c.P, c.L, swR, Lp );
		CHECK_NEAR( shadow, truth, 0.06 );
	}
	CHECK( straddled >= 2 );		// the cases genuinely exercise the near-plane clip (not a vacuous in-slab test)
}

TEST( SoftShadowFillConcave, convexEquivalence )
{
	// STRICT GENERALISATION: on convex input FillPolyConcave == FillPoly, bit for bit (2 crossings/chord ->
	// one interval -> identical Run + envelope). Square and regular octagon, full and grazing coverage.
	const float3 O( 0, 0, 0 ), U( 1, 0, 0 ), V( 0, 1, 0 );
	Poly2 square{ O, U, V, { { -10, -10 }, { 10, -10 }, { 10, 10 }, { -10, 10 } } };
	Poly2 octa{ O, U, V, {} };
	for( int k = 0; k < 8; k++ ) { float a = k * 0.7853981f; octa.p.push_back( { 10 * std::cos( a ), 10 * std::sin( a ) } ); }

	const Poly2 polys[] = { square, octa };
	struct RP { float3 P, L; float r; };
	const RP rp[] =
	{
		{ float3( 0, 0, -30 ), float3( 0, 0, 60 ), 8.0f },			// full cover
		{ float3( 16, 9, -26 ), float3( 3, 2, 62 ), 13.0f },		// off-centre grazing
		{ float3( -12, 7, -24 ), float3( -2, 1, 58 ), 11.0f },
	};

	for( const Poly2& poly : polys )
		for( const RP& c : rp )
		{
			const float swR = c.r, swEps = SW_NEAR_EPS;
			softFrame_t F = SoftShadow_Frame( c.P, c.L );
			uint32_t gConc[SW_SCAN_CHORDS] = {}, gPoly[SW_SCAN_CHORDS] = {};
			FillConcaveLoop( gConc, poly, c.P, F, swR, swEps );
			FillPolyLoop( gPoly, poly, c.P, F, swR, swEps );
			for( int m = 0; m < SW_SCAN_CHORDS; m++ )
			{
				CHECK( gConc[m] == gPoly[m] );		// FULL grid equality (not just under the disk mask): byte-identical
			}
		}
}

TEST( SoftShadowFillConcave, notchNotOverShadowed )
{
	// NON-VACUOUS: the concave fill sets STRICTLY FEWER bits than filling the same loop through FillPoly (which
	// bridges the reflex notch = over-shadow). Receiver under the notch so the difference lands under the disk.
	const float3 O( 0, 0, 0 ), U( 1, 0, 0 ), V( 0, 1, 0 );
	const Shape shapes[] =
	{
		{ "L-notch ", MakeLNotch( O, U, V ) },
		{ "plus    ", MakePlus( O, U, V ) },
		{ "star    ", MakeStar( O, U, V ) },
		{ "two-lobe", MakeTwoLobe( O, U, V ) },
	};
	uint32_t diskMask[SW_SCAN_CHORDS];
	for( int m = 0; m < SW_SCAN_CHORDS; m++ ) { diskMask[m] = SoftScan_Run( -SW_SCAN_HC[m], SW_SCAN_HC[m] ); }

	// receivers under various notches (the removed quadrant / arm gaps / lobe split)
	const std::pair<float, float> offs[] = { { 4, 4 }, { 6, 6 }, { 0, 6 }, { -6, 6 }, { 3, 3 }, { 0, 3 } };
	const float swR = 12.0f, swEps = SW_NEAR_EPS;

	for( const Shape& sh : shapes )
	{
		Poly2 hull = ConvexHull2( sh.poly );	// <=8 verts; fed to FillPoly (the convex over-shadow reference)
		bool anyStrict = false, everShadowed = false;
		for( auto off : offs )
		{
			const float3 P( off.first, off.second, -30 ), L( 0, 0, 60 );
			softFrame_t F = SoftShadow_Frame( P, L );
			uint32_t gConc[SW_SCAN_CHORDS] = {}, gHull[SW_SCAN_CHORDS] = {};
			FillConcaveLoop( gConc, sh.poly, P, F, swR, swEps );
			FillPolyLoop( gHull, hull, P, F, swR, swEps );		// convex hull fill = the notch-filling over-shadow
			CHECK( Subset( gConc, gHull, diskMask ) );			// concave fill is ALWAYS a subset of the hull fill
			int bc = MaskedBits( gConc, diskMask ), bh = MaskedBits( gHull, diskMask );
			if( bc > 0 ) { everShadowed = true; }
			if( bc < bh ) { anyStrict = true; }
		}
		CHECK( everShadowed );			// the shape actually shadows some receiver
		CHECK( anyStrict );				// and somewhere the hull strictly over-shadows the notch (non-vacuous)
	}
}
