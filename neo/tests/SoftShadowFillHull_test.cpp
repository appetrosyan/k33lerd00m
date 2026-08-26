/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 2026 Aleksandr Petrosyan

This file is part of the Doom 3 BFG Edition GPL Source Code.

===========================================================================
*/

// SoftScan_FillHull parity (brush-recovery, package C). A shadow-casting brush is a 3-D convex POLYHEDRON,
// so its occlusion is its SILHOUETTE from the receiver, not a flat face. FillHull projects the N hull verts,
// takes their 2-D convex hull (= the silhouette of a convex solid under central projection), and fills that
// world-space silhouette loop through the SHARED SoftScan_FillPoly core. FillHull must SUBSUME the two
// committed primitives - the accuracy guarantee:
//   1. convexParity   - FillHull over a 3-D convex point set == the coverage of the hull's TRIANGLE union
//                       (SoftScan_FillTri over its faces), bit for bit under the disk. The silhouette of a
//                       convex solid equals the union of ALL its projected faces, so the grids are identical.
//   2. coplanarReg    - FillHull on COPLANAR input == SoftScan_FillPoly, bit for bit (the committed coplanar
//                       gate must NOT move: a flat caster is already its own silhouette).
//   3. boxReg         - FillHull on a box's 8 corners == SoftScan_FillBox, bit for bit (a box is an 8-hull).
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
namespace swhull
{
#include "softwedge_coverage.inc.hlsl"
swAttrib_t g_swAttrib = {};
bool       g_swAttribOn = false;
}
using namespace swhull;			// bring the shader symbols into scope BEFORE SoftShadowBox.h parses its callers

#include "SoftShadowBox.h"		// swtest::v3 / MakeBox / Box helpers
#include "idUnitTest.h"

#include <cmath>
#include <vector>

using namespace swtest;

namespace
{
int MaskedBits( const uint32_t g[SW_SCAN_CHORDS], const uint32_t m[SW_SCAN_CHORDS] )
{
	int n = 0;
	for( int i = 0; i < SW_SCAN_CHORDS; i++ ) { n += __builtin_popcount( g[i] & m[i] ); }
	return n;
}

// a convex solid: its vertex set (the hull input) + a triangulation of its faces (the parity reference).
struct Solid
{
	std::vector<float3>                    verts;	// <= SW_POLY_MAX_VERTS hull vertices, ANY order
	std::vector<std::array<float3, 3>>     tris;	// face triangles (all faces, both windings irrelevant)
};

// fill a hull by its vertex set -> SoftScan_FillHull (the primitive under test)
void FillHullVerts( uint32_t grid[SW_SCAN_CHORDS], const std::vector<float3>& verts, float3 P, softFrame_t F, float swR, float swEps )
{
	float3 hv[SW_POLY_MAX_VERTS];
	int n = ( int )verts.size();
	if( n > SW_POLY_MAX_VERTS ) { n = SW_POLY_MAX_VERTS; }
	for( int i = 0; i < n; i++ ) { hv[i] = verts[i]; }
	SoftScan_FillHull( grid, hv, n, P, F, swR, swEps );
}

// union of FillTri over the solid's face triangles (the exact coverage FillHull must reproduce)
void FillSolidTris( uint32_t grid[SW_SCAN_CHORDS], const Solid& s, float3 P, softFrame_t F, float swR, float swEps )
{
	for( const auto& t : s.tris )
	{
		SoftScan_FillTri( grid, t[0], t[1], t[2], P, F, swR, swEps );
	}
}

// triangular prism (wedge): triangle base extruded along +z. 6 verts, 8 face triangles (2 caps + 3 quads).
Solid MakeWedge( float3 C )
{
	const float3 a0 = C + v3( -12, -8, -8 ), b0 = C + v3( 12, -8, -8 ), c0 = C + v3( 0, 12, -8 );
	const float3 a1 = C + v3( -12, -8,  8 ), b1 = C + v3( 12, -8,  8 ), c1 = C + v3( 0, 12,  8 );
	Solid s;
	s.verts = { a0, b0, c0, a1, b1, c1 };
	s.tris =
	{
		{{ a0, b0, c0 }}, {{ a1, c1, b1 }},							// bottom / top caps
		{{ a0, a1, b1 }}, {{ a0, b1, b0 }},							// side a-b
		{{ b0, b1, c1 }}, {{ b0, c1, c0 }},							// side b-c
		{{ c0, c1, a1 }}, {{ c0, a1, a0 }},							// side c-a
	};
	return s;
}

// square-base pyramid: 4 base corners + 1 apex. 5 verts, 6 face triangles (2 base + 4 sides).
Solid MakePyramid( float3 C )
{
	const float3 b00 = C + v3( -10, -10, -8 ), b10 = C + v3( 10, -10, -8 );
	const float3 b11 = C + v3(  10,  10, -8 ), b01 = C + v3( -10, 10, -8 );
	const float3 ap  = C + v3( 0, 0, 14 );
	Solid s;
	s.verts = { b00, b10, b11, b01, ap };
	s.tris =
	{
		{{ b00, b11, b10 }}, {{ b00, b01, b11 }},					// base quad
		{{ b00, b10, ap }}, {{ b10, b11, ap }}, {{ b11, b01, ap }}, {{ b01, b00, ap }},	// 4 sides
	};
	return s;
}

// a box (axis-aligned or rotated) as a Solid: 8 corners + 12 face triangles from the auto-oriented faces.
Solid BoxSolid( const Box& b )
{
	Solid s;
	for( int i = 0; i < 8; i++ ) { s.verts.push_back( b.c[i] ); }
	for( int f = 0; f < 6; f++ )
	{
		int i0 = b.f[f][0], i1 = b.f[f][1], i2 = b.f[f][2], i3 = b.f[f][3];
		s.tris.push_back( {{ b.c[i0], b.c[i1], b.c[i2] }} );
		s.tris.push_back( {{ b.c[i0], b.c[i2], b.c[i3] }} );
	}
	return s;
}
}

TEST( SoftShadowFillHull, convexParity )
{
	// FillHull over a 3-D convex point set == the projected union of the solid's faces, bit for bit under
	// the disk. Receiver below, broad light above, solid between: each casts a real shadow.
	struct Case { Solid s; float3 P, L; float r; };
	std::vector<Case> cases;
	cases.push_back( { BoxSolid( MakeBox( v3( 0, 0, 0 ), v3( 10, 10, 10 ) ) ),      v3(  3,  2, -30 ), v3( 0, 0, 60 ),  8.0f } );
	cases.push_back( { BoxSolid( MakeBox( v3( 0, 0, 0 ), v3( 9, 12, 10 ), 0.7f, 0.35f ) ), v3( 6, 4, -28 ), v3( 2, 1, 62 ), 12.0f } );	// rotated OBB = "random" convex hull
	cases.push_back( { MakeWedge( v3( 1, -1, 2 ) ),                                 v3(  2, -1, -28 ), v3( 1, 0, 58 ), 11.0f } );
	cases.push_back( { MakePyramid( v3( -1, 2, 1 ) ),                               v3( -1,  2, -27 ), v3( 0, 1, 55 ), 10.0f } );

	for( int ci = 0; ci < ( int )cases.size(); ci++ )
	{
		const Case& c = cases[ci];
		const float swR = c.r, swEps = SW_NEAR_EPS;
		softFrame_t F = SoftShadow_Frame( c.P, c.L );

		uint32_t diskMask[SW_SCAN_CHORDS]; int diskBits = 0;
		for( int m = 0; m < SW_SCAN_CHORDS; m++ )
		{
			diskMask[m] = SoftScan_Run( -SW_SCAN_HC[m], SW_SCAN_HC[m] );
			diskBits += __builtin_popcount( diskMask[m] );
		}

		uint32_t gHull[SW_SCAN_CHORDS] = {}, gTri[SW_SCAN_CHORDS] = {};
		FillHullVerts( gHull, c.s.verts, c.P, F, swR, swEps );
		FillSolidTris( gTri, c.s, c.P, F, swR, swEps );

		// EXACT: the silhouette fill == the projected union of the faces, bit for bit under the disk mask.
		for( int m = 0; m < SW_SCAN_CHORDS; m++ )
		{
			CHECK( ( gHull[m] & diskMask[m] ) == ( gTri[m] & diskMask[m] ) );
		}
		// coverage fraction agrees to 1e-4 (equivalent statement; guards against a vacuous 0 == 0 too).
		const float covH = diskBits > 0 ? ( float )MaskedBits( gHull, diskMask ) / diskBits : 0.0f;
		const float covT = diskBits > 0 ? ( float )MaskedBits( gTri, diskMask ) / diskBits : 0.0f;
		CHECK_NEAR( covH, covT, 1e-4 );
		CHECK( MaskedBits( gTri, diskMask ) > 0 );	// non-trivial: the solid actually shadows P
	}
}

TEST( SoftShadowFillHull, coplanarRegression )
{
	// COPLANAR input: FillHull == SoftScan_FillPoly, bit for bit. The committed coplanar gate must not move.
	// Convex flat polygons on horizontal + tilted planes (mirrors the FillPoly convexParity coverage).
	const float3 P0 = v3( 0, 0, -30 );
	struct Case { float3 O, U, V, P, L; float r; std::vector<std::pair<float, float>> pts; };
	const Case cases[] =
	{
		{ v3( 0, 0, 0 ), v3( 1, 0, 0 ), v3( 0, 1, 0 ), P0, v3( 0, 0, 60 ), 8.0f,
			{ { -10, -10 }, { 10, -10 }, { 10, 10 }, { -10, 10 } } },
		{ v3( 2, 1, 0 ), v3( 1, 0, 0 ), v3( 0, 1, 0 ), v3( 14, 8, -26 ), v3( 3, 2, 62 ), 13.0f,
			{ { -9, -6 }, { 8, -8 }, { 11, 4 }, { 1, 10 }, { -8, 6 } } },
		{ v3( 1, -2, 0 ), normalize( v3( 1, 0, 0.35f ) ), normalize( v3( 0.2f, 1, 0 ) ),
			v3( 1, -2, -28 ), v3( 4, 1, 58 ), 12.0f,
			{ { -10, -5 }, { -4, -10 }, { 8, -7 }, { 10, 4 }, { 2, 10 }, { -8, 7 } } },
	};

	for( int ci = 0; ci < ( int )( sizeof( cases ) / sizeof( cases[0] ) ); ci++ )
	{
		const Case& c = cases[ci];
		std::vector<float3> loop;
		for( auto& q : c.pts ) { loop.push_back( c.O + c.U * q.first + c.V * q.second ); }
		const float swR = c.r, swEps = SW_NEAR_EPS;
		softFrame_t F = SoftShadow_Frame( c.P, c.L );

		uint32_t diskMask[SW_SCAN_CHORDS];
		for( int m = 0; m < SW_SCAN_CHORDS; m++ ) { diskMask[m] = SoftScan_Run( -SW_SCAN_HC[m], SW_SCAN_HC[m] ); }

		float3 lp[SW_POLY_MAX_VERTS]; int n = ( int )loop.size();
		for( int i = 0; i < n; i++ ) { lp[i] = loop[i]; }

		uint32_t gHull[SW_SCAN_CHORDS] = {}, gPoly[SW_SCAN_CHORDS] = {};
		SoftScan_FillHull( gHull, lp, n, c.P, F, swR, swEps );
		SoftScan_FillPoly( gPoly, lp, n, c.P, F, swR, swEps );

		for( int m = 0; m < SW_SCAN_CHORDS; m++ )
		{
			CHECK( ( gHull[m] & diskMask[m] ) == ( gPoly[m] & diskMask[m] ) );
		}
		CHECK( MaskedBits( gPoly, diskMask ) > 0 );	// non-trivial
	}
}

TEST( SoftShadowFillHull, boxRegression )
{
	// A box is an 8-vertex hull: FillHull on its 8 corners == SoftScan_FillBox, bit for bit. Mirrors the
	// FillBox parity configs (off-centre, near-plane straddle, thin slab, rotated OBBs).
	struct Cfg { float3 C, h; float yaw, pitch; float3 P, L; float r; };
	const Cfg cfgs[] =
	{
		{ v3( 0, 0, 0 ), v3( 10, 10, 10 ), 0.0f, 0.0f, v3(  3,  2, -30 ), v3( 0, 0, 60 ),  8.0f },
		{ v3( 0, 0, 0 ), v3( 12,  8, 10 ), 0.0f, 0.0f, v3( 18, -6, -24 ), v3( 4, 3, 70 ), 12.0f },
		{ v3( 0, 0, 0 ), v3( 10, 10, 29 ), 0.0f, 0.0f, v3(  5,  5, -30 ), v3( 0, 0, 40 ),  6.0f },  // near-plane straddle (box bottom 1u above the receiver; well-posed, P not embedded in a face)
		{ v3( 0, 0, 0 ), v3( 20, 20,  3 ), 0.0f, 0.0f, v3(  2, -3, -25 ), v3( 1, 1, 55 ), 10.0f },  // thin slab
		{ v3( 0, 0, 0 ), v3( 10, 14,  9 ), 0.6f, 0.3f,  v3(  6,  4, -28 ), v3( 2, 1, 62 ),  9.0f },  // OBB, full cover
		{ v3( 0, 0, 0 ), v3(  9, 12, 10 ), 0.7f, 0.35f, v3( 26, 20, -26 ), v3( 0, 0, 60 ), 14.0f },  // OBB, partial
		{ v3( 0, 0, 0 ), v3( 11,  7, 13 ), 1.1f, 0.5f,  v3( -22, 15, -24 ), v3( 3, -2, 58 ), 12.0f },// OBB, partial, other axis
	};

	for( int ci = 0; ci < ( int )( sizeof( cfgs ) / sizeof( cfgs[0] ) ); ci++ )
	{
		const Cfg& g = cfgs[ci];
		Box b = MakeBox( g.C, g.h, g.yaw, g.pitch );
		const float swR = g.r >= 1e-2f ? g.r : 1e-2f, swEps = SW_NEAR_EPS;
		softFrame_t F = SoftShadow_Frame( g.P, g.L );

		uint32_t diskMask[SW_SCAN_CHORDS];
		for( int m = 0; m < SW_SCAN_CHORDS; m++ ) { diskMask[m] = SoftScan_Run( -SW_SCAN_HC[m], SW_SCAN_HC[m] ); }

		uint32_t gHull[SW_SCAN_CHORDS] = {}, gBox[SW_SCAN_CHORDS] = {};
		SoftScan_FillHull( gHull, b.c, 8, g.P, F, swR, swEps );
		SoftScan_FillBox( gBox, b.c, g.P, F, swR, swEps );

		for( int m = 0; m < SW_SCAN_CHORDS; m++ )
		{
			CHECK( ( gHull[m] & diskMask[m] ) == ( gBox[m] & diskMask[m] ) );
		}
		CHECK( MaskedBits( gBox, diskMask ) > 0 );	// non-trivial
	}
}
