/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 2026 Aleksandr Petrosyan

This file is part of the Doom 3 BFG Edition GPL Source Code.

===========================================================================
*/

// SoftScan_FillPoly parity: the coplanar-merge caster path (r_softShadowCoplanarMerge) replaces a FLAT
// caster's triangle stream with ONE analytic convex polygon, filled by SoftScan_FillPoly. Two contracts:
//   (a) LOSSLESS: for a convex flat polygon, FillPoly over its boundary loop == the bit-exact union of
//       SoftScan_FillTri over ANY triangulation of that polygon (here a fan). This is the drop-in claim -
//       the same primitive the box silhouette uses, so the flat caster is a byte-for-byte substitution.
//   (b) CONCAVE guard: for a concave flat caster (L-shape), the convex hull OVER-shadows (covers strictly
//       more than the real triangles). The test proves hull != tris, which is exactly why the emitter must
//       KEEP triangles for concave faces - merging them would darken the notch. Lossless is non-negotiable.
// The .inc.hlsl dual-compiles as C++, so this exercises the LIVE shader math, not a reimplementation.

#include "hlsl_compat.h"

// Compile the live coverage source as C++ with the Fubini scanline enabled, isolated in a namespace so it
// does not ODR-collide with the SW_SCANLINE 0 bodies other test TUs compile. Mirror SoftShadowFillBox.
#define SW_SCANLINE 1
#define SW_SCAN_BITS 32			// CPU emulation uses uint32 grids; pin the word (SwGridWord = uint). GPU ships 64.
#ifndef SW_SCAN_CHORDS
	#define SW_SCAN_CHORDS 16
#endif
#define inout					// HLSL inout on the grid param: C++ array params decay to pointers, so mutation matches
namespace swpoly
{
#include "softwedge_coverage.inc.hlsl"
swAttrib_t g_swAttrib = {};
bool       g_swAttribOn = false;
}
using namespace swpoly;

#include "SoftShadowBox.h"		// swtest::v3 / len3 helpers, Rng
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

// gTri is a subset of gHull under the disk mask (every bit the triangles set, the hull also sets)
bool Subset( const uint32_t sub[SW_SCAN_CHORDS], const uint32_t sup[SW_SCAN_CHORDS], const uint32_t msk[SW_SCAN_CHORDS] )
{
	for( int i = 0; i < SW_SCAN_CHORDS; i++ ) { if( ( sub[i] & msk[i] ) & ~( sup[i] & msk[i] ) ) { return false; } }
	return true;
}

// a flat polygon: closed CCW loop of world verts, all coplanar. Built from 2D points on an (O;U,V) plane.
struct FlatPoly
{
	std::vector<float3> loop;
	float3 At( int i ) const { return loop[( i % ( int )loop.size() + ( int )loop.size() ) % ( int )loop.size()]; }
};

FlatPoly MakeFlat( float3 O, float3 U, float3 V, const std::vector<std::pair<float, float>>& pts2 )
{
	FlatPoly p;
	for( auto& q : pts2 ) { p.loop.push_back( O + U * q.first + V * q.second ); }
	return p;
}

// fill a convex polygon by its boundary loop -> FillPoly (the analytic caster)
void FillPolyLoop( uint32_t grid[SW_SCAN_CHORDS], const FlatPoly& p, float3 P, softFrame_t F, float swR, float swEps )
{
	float3 lp[SW_POLY_MAX_VERTS];
	int n = ( int )p.loop.size();
	if( n > SW_POLY_MAX_VERTS ) { n = SW_POLY_MAX_VERTS; }
	for( int i = 0; i < n; i++ ) { lp[i] = p.loop[i]; }
	SoftScan_FillPoly( grid, lp, n, P, F, swR, swEps );
}

// fill a convex polygon by fan triangulation -> union of FillTri (the shipped triangle path it replaces)
void FillFanTris( uint32_t grid[SW_SCAN_CHORDS], const FlatPoly& p, float3 P, softFrame_t F, float swR, float swEps )
{
	int n = ( int )p.loop.size();
	for( int i = 1; i + 1 < n; i++ )
	{
		SoftScan_FillTri( grid, p.loop[0], p.loop[i], p.loop[i + 1], P, F, swR, swEps );
	}
}

// 2D monotone-chain convex hull of the projected loop, returned as the world-space hull loop.
FlatPoly ConvexHull( float3 O, float3 U, float3 V, const std::vector<std::pair<float, float>>& pts2 )
{
	std::vector<std::pair<float, float>> s( pts2 );
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
	return MakeFlat( O, U, V, h );
}
}

TEST( SoftShadowFillPoly, convexParity )
{
	// LOSSLESS: FillPoly(boundary) == union of FillTri over the polygon's fan, bit for bit under the disk.
	// Horizontal + tilted planes, receiver below, broad overhead light: full and grazing/partial coverage.
	const float3 P0 = float3( 0, 0, -30 );
	struct Case { float3 O, U, V, P, L; float r; std::vector<std::pair<float, float>> pts; };
	const Case cases[] =
	{
		// axis-aligned quad, receiver under centre (full cover)
		{ float3( 0, 0, 0 ), float3( 1, 0, 0 ), float3( 0, 1, 0 ), P0, float3( 0, 0, 60 ), 8.0f,
			{ { -10, -10 }, { 10, -10 }, { 10, 10 }, { -10, 10 } } },
		// convex pentagon, off-centre receiver (partial / grazing edge)
		{ float3( 2, 1, 0 ), float3( 1, 0, 0 ), float3( 0, 1, 0 ), float3( 14, 8, -26 ), float3( 3, 2, 62 ), 13.0f,
			{ { -9, -6 }, { 8, -8 }, { 11, 4 }, { 1, 10 }, { -8, 6 } } },
		// tilted plane (rotated basis), convex hexagon, receiver under the centroid (partial / grazing)
		{ float3( 1, -2, 0 ), normalize( float3( 1, 0, 0.35f ) ), normalize( float3( 0.2f, 1, 0 ) ),
			float3( 1, -2, -28 ), float3( 4, 1, 58 ), 12.0f,
			{ { -10, -5 }, { -4, -10 }, { 8, -7 }, { 10, 4 }, { 2, 10 }, { -8, 7 } } },
	};

	for( int ci = 0; ci < ( int )( sizeof( cases ) / sizeof( cases[0] ) ); ci++ )
	{
		const Case& c = cases[ci];
		FlatPoly poly = MakeFlat( c.O, c.U, c.V, c.pts );
		const float swR = c.r, swEps = SW_NEAR_EPS;
		softFrame_t F = SoftShadow_Frame( c.P, c.L );

		uint32_t diskMask[SW_SCAN_CHORDS]; int diskBits = 0;
		for( int m = 0; m < SW_SCAN_CHORDS; m++ )
		{
			diskMask[m] = SoftScan_Run( -SW_SCAN_HC[m], SW_SCAN_HC[m] );
			diskBits += __builtin_popcount( diskMask[m] );
		}

		uint32_t gPoly[SW_SCAN_CHORDS] = {}, gTri[SW_SCAN_CHORDS] = {};
		FillPolyLoop( gPoly, poly, c.P, F, swR, swEps );
		FillFanTris( gTri, poly, c.P, F, swR, swEps );

		// (a) EXACT drop-in: analytic polygon coverage == fan-triangulation union, bit for bit.
		for( int m = 0; m < SW_SCAN_CHORDS; m++ )
		{
			CHECK( ( gPoly[m] & diskMask[m] ) == ( gTri[m] & diskMask[m] ) );
		}
		// non-trivial: the polygon actually shadows P (else parity is a vacuous 0 == 0)
		CHECK( MaskedBits( gTri, diskMask ) > 0 );
	}
}

TEST( SoftShadowFillPoly, concaveOverShadows )
{
	// L-shape (full [-10,10]^2 square MINUS the [0,10]x[0,10] top-right quadrant): CONCAVE, one reflex vertex
	// at (0,0). Its convex hull cuts the reflex corner, adding the triangle (10,0)(0,0)(0,10) - so the hull
	// coverage is a STRICT superset of the real triangles. This is why the emitter must keep triangles for
	// concave faces: merging to the hull would darken the notch. Receiver placed under the notch so the
	// difference is visible in the coverage grid.
	const float3 O( 0, 0, 0 ), U( 1, 0, 0 ), V( 0, 1, 0 );
	const std::vector<std::pair<float, float>> L =
	{
		{ -10, -10 }, { 10, -10 }, { 10, 0 }, { 0, 0 }, { 0, 10 }, { -10, 10 }
	};
	// the L, tiled exactly by 4 triangles (two rectangles): bottom [-10,10]x[-10,0], left [-10,0]x[0,10]
	auto W = [&]( float x, float y ) { return O + U * x + V * y; };
	const float3 tris[4][3] =
	{
		{ W( -10, -10 ), W( 10, -10 ), W( 10, 0 ) }, { W( -10, -10 ), W( 10, 0 ), W( -10, 0 ) },
		{ W( -10, 0 ),  W( 0, 0 ),   W( 0, 10 ) },  { W( -10, 0 ),  W( 0, 10 ), W( -10, 10 ) },
	};
	FlatPoly hull = ConvexHull( O, U, V, L );	// 5 verts: reflex (0,0) dropped

	const float3 P( 3, 3, -30 ), Lgt( 3, 3, 60 );	// receiver directly under the notch cut triangle
	const float swR = 12.0f, swEps = SW_NEAR_EPS;
	softFrame_t F = SoftShadow_Frame( P, Lgt );

	uint32_t diskMask[SW_SCAN_CHORDS];
	for( int m = 0; m < SW_SCAN_CHORDS; m++ ) { diskMask[m] = SoftScan_Run( -SW_SCAN_HC[m], SW_SCAN_HC[m] ); }

	uint32_t gTri[SW_SCAN_CHORDS] = {}, gHull[SW_SCAN_CHORDS] = {};
	for( int t = 0; t < 4; t++ ) { SoftScan_FillTri( gTri, tris[t][0], tris[t][1], tris[t][2], P, F, swR, swEps ); }
	FillPolyLoop( gHull, hull, P, F, swR, swEps );

	// hull covers a SUPERSET of the real triangles, and STRICTLY more (the notch) - so it over-shadows.
	CHECK( Subset( gTri, gHull, diskMask ) );
	CHECK( MaskedBits( gHull, diskMask ) > MaskedBits( gTri, diskMask ) );
}
