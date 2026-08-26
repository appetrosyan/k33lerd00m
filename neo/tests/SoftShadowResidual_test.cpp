/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 2026 Aleksandr Petrosyan

This file is part of the Doom 3 BFG Edition GPL Source Code.

===========================================================================
*/

// Brush-recovery soft shadows, PACKAGE B (renderer consumer, residual half). dmap now partitions each BSP
// area's shadow brushes into HULL-emittable brushes (the `shadowHulls` block, package A) and the residual,
// NON-emittable brushes (>cap verts / not hull-walkable), which it emits as an ordinary per-area triangle
// stream in a new `shadowResiduals` block. The renderer must consume BOTH, so that
//
//     coverage( hulls(emittable) + residual-triangles(non-emittable) )  ==  coverage( ALL brushes as triangles )
//
// bit-exact - lossless. That is the property the OLD all-or-nothing completeness rule was protecting by
// falling a whole area back to triangles; package B replaces it with the explicit split. This TU verifies:
//   (1) PARSE : a hand-written `shadowResiduals` fixture -> the per-area (verts, indexes) table (grammar
//               mirror, exactly as SoftShadowBrushHull_test mirrors the `shadowHulls` grammar with std streams).
//   (2) PARITY: the split above equals the all-triangle union, bit for bit, over sampled fragments - AND that
//               the residual half actually MATTERS (hulls-only differs from the truth: the bug B fixes).
//
// The coverage math is the LIVE shader source (softwedge_coverage.inc.hlsl dual-compiled as C++), mirroring
// SoftShadowFillHull_test - not a reimplementation.

#include "hlsl_compat.h"

#define SW_SCANLINE 1
#define SW_SCAN_BITS 32			// CPU emulation uses uint32 grids (SwGridWord = uint); GPU ships 64.
#ifndef SW_SCAN_CHORDS
	#define SW_SCAN_CHORDS 16
#endif
#define inout
namespace swresid
{
#include "softwedge_coverage.inc.hlsl"
swAttrib_t g_swAttrib = {};
bool       g_swAttribOn = false;
}
using namespace swresid;

#include "SoftShadowBox.h"		// swtest::v3 / MakeBox / Box helpers
#include "idUnitTest.h"

#include <array>
#include <cmath>
#include <sstream>
#include <string>
#include <vector>

using namespace swtest;

namespace
{

// ---------------------------------------------------------------- (1) `shadowResiduals` grammar parser
// Mirrors R_ParseShadowResiduals (RenderWorld_load.cpp). The engine uses idLexer; here std streams. Grammar
// (comments in the .proc are cosmetic - idLexer skips /* */, so the fixture omits them):
//   shadowResiduals { <numAreas>  <a> { V I  ( x y z  s t  nx ny nz )...V  i0 i1 i2...I  } ... }
struct ResidualAreaCPU
{
	std::vector<std::array<float, 8>> verts;	// x y z  s t  nx ny nz  (MAP space)
	std::vector<int>                  indexes;	// triangle list into verts (multiple of 3)
};

bool ParseResiduals( const char* text, std::vector<ResidualAreaCPU>& byArea )
{
	std::istringstream in( text );
	std::string tok;
	auto expect  = [&]( const char* s ) -> bool { return ( in >> tok ) && tok == s; };
	auto readInt = [&]( int& v ) -> bool { return static_cast<bool>( in >> v ); };
	auto readF   = [&]( float& v ) -> bool { return static_cast<bool>( in >> v ); };

	if( !expect( "shadowResiduals" ) || !expect( "{" ) ) { return false; }
	int numAreas = 0;
	if( !readInt( numAreas ) ) { return false; }
	byArea.assign( numAreas, {} );

	for( int a = 0; a < numAreas; a++ )
	{
		int areaIdx = 0, V = 0, I = 0;
		if( !readInt( areaIdx ) || !expect( "{" ) || !readInt( V ) || !readInt( I ) ) { return false; }
		const bool store = ( areaIdx >= 0 && areaIdx < ( int )byArea.size() );
		for( int v = 0; v < V; v++ )
		{
			if( !expect( "(" ) ) { return false; }
			std::array<float, 8> f{};
			for( int k = 0; k < 8; k++ ) { if( !readF( f[k] ) ) { return false; } }
			if( !expect( ")" ) ) { return false; }
			if( store ) { byArea[areaIdx].verts.push_back( f ); }
		}
		for( int i = 0; i < I; i++ )
		{
			int idx = 0;
			if( !readInt( idx ) ) { return false; }
			if( store ) { byArea[areaIdx].indexes.push_back( idx ); }
		}
		if( !expect( "}" ) ) { return false; }
	}
	return expect( "}" );
}

const char* kFixture =
	"shadowResiduals { 2 "
	"  0 { 4 6 "
	"    ( 0 0 0    0 0  0 0 1 ) "
	"    ( 10 0 0   1 0  0 0 1 ) "
	"    ( 10 10 0  1 1  0 0 1 ) "
	"    ( 0 10 0   0 1  0 0 1 ) "
	"    0 1 2  0 2 3 "
	"  } "
	"  1 { 0 0 } "			// presence-gated: an area with no residuals => empty
	"}";

// ---------------------------------------------------------------- (2) parity: brushes as hull vs triangles
// A shadow brush is a convex solid: its vertex set (the hull input) + a triangulation of its faces (the
// residual/all-triangle reference). Coverage is a UNION (bit-OR of grids), so splitting a brush set into
// "hull-emitted" and "triangle-emitted" must reproduce the all-triangle union iff FillHull subsumes the
// brush's triangle union (proven in SoftShadowFillHull_test::convexParity) - this test proves the SPLIT.
struct Brush
{
	std::vector<float3>                verts;
	std::vector<std::array<float3, 3>> tris;
};

Brush BrushFromBox( const Box& b )
{
	Brush br;
	for( int i = 0; i < 8; i++ ) { br.verts.push_back( b.c[i] ); }
	for( int f = 0; f < 6; f++ )
	{
		const int i0 = b.f[f][0], i1 = b.f[f][1], i2 = b.f[f][2], i3 = b.f[f][3];
		br.tris.push_back( {{ b.c[i0], b.c[i1], b.c[i2] }} );
		br.tris.push_back( {{ b.c[i0], b.c[i2], b.c[i3] }} );
	}
	return br;
}

Brush MakeWedge( float3 C )
{
	const float3 a0 = C + v3( -12, -8, -8 ), b0 = C + v3( 12, -8, -8 ), c0 = C + v3( 0, 12, -8 );
	const float3 a1 = C + v3( -12, -8,  8 ), b1 = C + v3( 12, -8,  8 ), c1 = C + v3( 0, 12,  8 );
	Brush br;
	br.verts = { a0, b0, c0, a1, b1, c1 };
	br.tris  =
	{
		{{ a0, b0, c0 }}, {{ a1, c1, b1 }},
		{{ a0, a1, b1 }}, {{ a0, b1, b0 }},
		{{ b0, b1, c1 }}, {{ b0, c1, c0 }},
		{{ c0, c1, a1 }}, {{ c0, a1, a0 }},
	};
	return br;
}

Brush MakePyramid( float3 C )
{
	const float3 q00 = C + v3( -10, -10, -8 ), q10 = C + v3( 10, -10, -8 );
	const float3 q11 = C + v3(  10,  10, -8 ), q01 = C + v3( -10, 10, -8 );
	const float3 ap  = C + v3( 0, 0, 14 );
	Brush br;
	br.verts = { q00, q10, q11, q01, ap };
	br.tris  =
	{
		{{ q00, q11, q10 }}, {{ q00, q01, q11 }},
		{{ q00, q10, ap }}, {{ q10, q11, ap }}, {{ q11, q01, ap }}, {{ q01, q00, ap }},
	};
	return br;
}

void FillHullBrush( uint32_t grid[SW_SCAN_CHORDS], const Brush& b, float3 P, softFrame_t F, float swR, float swEps )
{
	float3 hv[SW_POLY_MAX_VERTS];
	int n = ( int )b.verts.size();
	if( n > SW_POLY_MAX_VERTS ) { n = SW_POLY_MAX_VERTS; }
	for( int i = 0; i < n; i++ ) { hv[i] = b.verts[i]; }
	SoftScan_FillHull( grid, hv, n, P, F, swR, swEps );
}

void FillTriBrush( uint32_t grid[SW_SCAN_CHORDS], const Brush& b, float3 P, softFrame_t F, float swR, float swEps )
{
	for( const auto& t : b.tris ) { SoftScan_FillTri( grid, t[0], t[1], t[2], P, F, swR, swEps ); }
}

int MaskedBits( const uint32_t g[SW_SCAN_CHORDS], const uint32_t m[SW_SCAN_CHORDS] )
{
	int n = 0;
	for( int i = 0; i < SW_SCAN_CHORDS; i++ ) { n += __builtin_popcount( g[i] & m[i] ); }
	return n;
}

} // namespace

TEST( SoftShadowResidual, ParseTableMatchesFixture )
{
	std::vector<ResidualAreaCPU> byArea;
	CHECK( ParseResiduals( kFixture, byArea ) );
	CHECK( ( int )byArea.size() == 2 );				// numAreas header
	CHECK( byArea[0].verts.size() == 4 );			// one quad brush residual
	CHECK( byArea[0].indexes.size() == 6 );			// two triangles
	CHECK( byArea[1].verts.size() == 0 );			// presence-gated empty area
	CHECK( byArea[1].indexes.size() == 0 );
	// vertex positions survive the 8-float round trip (x y z  s t  nx ny nz)
	CHECK_NEAR( byArea[0].verts[0][0], 0.0f, 1e-6f );
	CHECK_NEAR( byArea[0].verts[2][0], 10.0f, 1e-6f );	// vert2 = (10,10,0)
	CHECK_NEAR( byArea[0].verts[2][1], 10.0f, 1e-6f );
	CHECK_NEAR( byArea[0].verts[3][1], 10.0f, 1e-6f );	// vert3 = (0,10,0)
	CHECK_NEAR( byArea[0].verts[0][7], 1.0f, 1e-6f );	// normal.z of vert0
	// index stream survives
	CHECK( byArea[0].indexes[0] == 0 );
	CHECK( byArea[0].indexes[3] == 0 );
	CHECK( byArea[0].indexes[5] == 3 );
}

TEST( SoftShadowResidual, HullPlusResidualEqualsAllTriangles )
{
	// A brush set: some brushes go HULL (emittable), the rest go RESIDUAL TRIANGLES (non-emittable). The
	// per-brush partition is arbitrary for coverage - a brush's hull fill == its triangle-union fill - so
	// the split's union must equal the all-triangle union bit for bit. Receiver below, broad light above,
	// brushes between: each casts a real, overlapping shadow onto the sampled fragments.
	std::vector<Brush> brushes;
	brushes.push_back( BrushFromBox( MakeBox( v3(  6,  4, 0 ), v3( 8, 8, 9 ) ) ) );					// emittable (8-hull)
	brushes.push_back( MakeWedge( v3( -6, -2, 1 ) ) );												// emittable (6-hull)
	brushes.push_back( MakePyramid( v3(  2, -6, 0 ) ) );											// residual (streamed as tris)
	brushes.push_back( BrushFromBox( MakeBox( v3( -4,  6, 0 ), v3( 7, 6, 10 ), 0.6f, 0.3f ) ) );	// residual (streamed as tris)
	// emit[] = brushes 0,1 (hull); residual[] = brushes 2,3 (triangles). Together = all four, once each.
	const bool emit[4] = { true, true, false, false };

	// sample several receiver fragments around the shadow footprint; broad light overhead.
	const float3 L = v3( 1, -1, 60 );
	const float  swR = 11.0f, swEps = SW_NEAR_EPS;
	const float3 Ps[] = { v3( 0, 0, -30 ), v3( 8, 5, -28 ), v3( -7, 4, -27 ), v3( 3, -8, -31 ), v3( -3, -3, -26 ) };

	int teeth = 0;			// fragments where hulls-only differs from the truth (residuals genuinely matter)
	for( const float3& P : Ps )
	{
		const softFrame_t F = SoftShadow_Frame( P, L );

		uint32_t diskMask[SW_SCAN_CHORDS]; int diskBits = 0;
		for( int m = 0; m < SW_SCAN_CHORDS; m++ )
		{
			diskMask[m] = SoftScan_Run( -SW_SCAN_HC[m], SW_SCAN_HC[m] );
			diskBits += __builtin_popcount( diskMask[m] );
		}

		uint32_t gSplit[SW_SCAN_CHORDS] = {};		// package B: hull(emit) + residual triangles
		uint32_t gAllTri[SW_SCAN_CHORDS] = {};		// reference: every brush as triangles
		uint32_t gHullOnly[SW_SCAN_CHORDS] = {};	// pre-B bug: residuals dropped
		for( int i = 0; i < ( int )brushes.size(); i++ )
		{
			if( emit[i] ) { FillHullBrush( gSplit, brushes[i], P, F, swR, swEps ); FillHullBrush( gHullOnly, brushes[i], P, F, swR, swEps ); }
			else          { FillTriBrush( gSplit, brushes[i], P, F, swR, swEps ); }
			FillTriBrush( gAllTri, brushes[i], P, F, swR, swEps );
		}

		// LOSSLESS: the split union == the all-triangle union, bit for bit under the disk mask.
		for( int m = 0; m < SW_SCAN_CHORDS; m++ )
		{
			CHECK( ( gSplit[m] & diskMask[m] ) == ( gAllTri[m] & diskMask[m] ) );
		}
		CHECK( MaskedBits( gAllTri, diskMask ) > 0 );		// non-trivial: the brushes actually shadow P

		// TEETH: at this or some fragment, dropping residuals MUST change coverage (else the test is vacuous
		// and would pass even if B never emitted residuals). Count fragments where hulls-only under-shadows.
		if( MaskedBits( gHullOnly, diskMask ) != MaskedBits( gAllTri, diskMask ) ) { teeth++; }
	}
	CHECK( teeth > 0 );		// residuals demonstrably contribute coverage the hulls alone do not
}
