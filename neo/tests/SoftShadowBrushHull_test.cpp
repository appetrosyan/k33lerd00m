/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 2026 Aleksandr Petrosyan

This file is part of the Doom 3 BFG Edition GPL Source Code.

===========================================================================
*/

// Brush-recovery soft shadows, package B (renderer consumer). Offline verification, no engine build:
//   (1) PARSE: a hand-written `shadowHulls` fixture (the .proc grammar A emits) -> the per-area table.
//   (2) EMIT : each hull -> SoftHull_EmitRecord (the SHARED encoder the frontend calls) -> assert the
//              analytic record layout the shader decodes (softterm.cs.hlsl:919-931): slot0 .w = -N, then
//              N verts' .xyz, padded to whole triangles. This is the CONTRACT with package C's FillHull.
//
// The parser here mirrors the documented grammar (idLexer in the engine); the EMITTER is the real shared
// function from SoftShadowHull.h, so the shader-facing record layout is exercised for real, not re-derived.

#include "../renderer/SoftShadowHull.h"		// SoftHull_EmitRecord + areaShadowHull_t (plain floats, no idlib)
#include "idUnitTest.h"

#include <vector>
#include <sstream>
#include <string>
#include <cmath>

namespace
{

// ---- fixture: three areas. area 0 = one quad (4 verts) + one triangle (3 verts); area 1 = no hulls
// (empty block -> triangle fallback); area 2 = one pentagon (5 verts). Grammar per plan Phase 0:
//   shadowHulls { N  areaIdx { M  { K ( x y z )... } ... } ... }
const char* kFixture =
	"shadowHulls { 3 "
	"  0 { 2 "
	"    { 4 ( 0 0 0 ) ( 10 0 0 ) ( 10 10 0 ) ( 0 10 0 ) } "
	"    { 3 ( -1 -2 -3 ) ( 4 5 6 ) ( 7 8 9 ) } "
	"  } "
	"  1 { 0 } "
	"  2 { 1 "
	"    { 5 ( 0 0 5 ) ( 2 0 5 ) ( 3 2 5 ) ( 1 4 5 ) ( -1 2 5 ) } "
	"  } "
	"}";

// Minimal grammar parser (mirrors R_ParseShadowHulls; std streams instead of idLexer). Fills byArea[area].
bool ParseFixture( const char* text, std::vector< std::vector<areaShadowHull_t> >& byArea )
{
	std::istringstream in( text );
	std::string tok;
	auto expect = [&]( const char* s ) -> bool { return ( in >> tok ) && tok == s; };
	auto readInt = [&]( int& v ) -> bool { return static_cast<bool>( in >> v ); };
	auto readFloat = [&]( float& v ) -> bool { return static_cast<bool>( in >> v ); };

	if( !expect( "shadowHulls" ) || !expect( "{" ) ) { return false; }
	int numAreas = 0;
	if( !readInt( numAreas ) ) { return false; }
	byArea.assign( numAreas, {} );

	for( int a = 0; a < numAreas; a++ )
	{
		int areaIdx = 0, numHulls = 0;
		if( !readInt( areaIdx ) || !expect( "{" ) || !readInt( numHulls ) ) { return false; }
		for( int h = 0; h < numHulls; h++ )
		{
			if( !expect( "{" ) ) { return false; }
			int k = 0;
			if( !readInt( k ) ) { return false; }
			areaShadowHull_t hull{};
			hull.numVerts = k;
			for( int v = 0; v < k; v++ )
			{
				float x, y, z;
				if( !expect( "(" ) || !readFloat( x ) || !readFloat( y ) || !readFloat( z ) || !expect( ")" ) ) { return false; }
				if( v < SW_HULL_MAX_VERTS ) { hull.verts[v * 3 + 0] = x; hull.verts[v * 3 + 1] = y; hull.verts[v * 3 + 2] = z; }
			}
			if( !expect( "}" ) ) { return false; }
			if( k >= 3 && k <= SW_HULL_MAX_VERTS && areaIdx >= 0 && areaIdx < ( int )byArea.size() )
			{
				byArea[areaIdx].push_back( hull );
			}
		}
		if( !expect( "}" ) ) { return false; }
	}
	return expect( "}" );
}

} // namespace

TEST( SoftShadowBrushHull, ParseTableMatchesFixture )
{
	std::vector< std::vector<areaShadowHull_t> > byArea;
	CHECK( ParseFixture( kFixture, byArea ) );
	CHECK( ( int )byArea.size() == 3 );			// numAreas from the block header
	CHECK( byArea[0].size() == 2 );				// quad + triangle
	CHECK( byArea[1].size() == 0 );				// empty block -> triangle fallback
	CHECK( byArea[2].size() == 1 );				// pentagon
	CHECK( byArea[0][0].numVerts == 4 );
	CHECK( byArea[0][1].numVerts == 3 );
	CHECK( byArea[2][0].numVerts == 5 );
	// vertex positions survive the round trip
	CHECK_NEAR( byArea[0][0].verts[0 * 3 + 0], 0.0f, 1e-6f );
	CHECK_NEAR( byArea[0][0].verts[2 * 3 + 0], 10.0f, 1e-6f );	// quad vert 2 = (10,10,0)
	CHECK_NEAR( byArea[0][0].verts[2 * 3 + 1], 10.0f, 1e-6f );
	CHECK_NEAR( byArea[0][1].verts[0 * 3 + 2], -3.0f, 1e-6f );	// tri vert 0 = (-1,-2,-3)
}

TEST( SoftShadowBrushHull, EmitRecordLayoutQuad )
{
	// a 4-vertex hull -> numTris = (4+2)/3 = 2 -> 6 float4 slots.
	const float quad[] = { 0, 0, 0,  10, 0, 0,  10, 10, 0,  0, 10, 0 };
	float slots[ 12 * 4 ] = {};
	const int numSlots = SoftHull_EmitRecord( quad, 4, slots );
	CHECK( numSlots == 6 );							// 2 triangles worth of slots

	// CONTRACT: slot0 .w = -N (the FillHull discriminator)
	CHECK_NEAR( slots[0 * 4 + 3], -4.0f, 0.0f );
	// verts 0..3 land in slots 0..3 (.xyz), other .w == 0
	CHECK_NEAR( slots[1 * 4 + 3], 0.0f, 0.0f );
	CHECK_NEAR( slots[2 * 4 + 0], 10.0f, 1e-6f );	// vert2.x
	CHECK_NEAR( slots[2 * 4 + 1], 10.0f, 1e-6f );	// vert2.y
	CHECK_NEAR( slots[3 * 4 + 0], 0.0f, 1e-6f );	// vert3.x
	CHECK_NEAR( slots[3 * 4 + 1], 10.0f, 1e-6f );	// vert3.y
	// padding slots (4,5) repeat the last real vertex (vert3), so the stream stays a triple stream
	CHECK_NEAR( slots[4 * 4 + 0], 0.0f, 1e-6f );
	CHECK_NEAR( slots[4 * 4 + 1], 10.0f, 1e-6f );
	CHECK_NEAR( slots[5 * 4 + 0], 0.0f, 1e-6f );
	CHECK_NEAR( slots[5 * 4 + 1], 10.0f, 1e-6f );
}

TEST( SoftShadowBrushHull, EmitRecordTriangleIsSingleTri )
{
	// N=3 -> exactly one triangle (3 slots), no padding, .w = -3. This is the coplanar-poly regression
	// point: a flat 3-vertex hull must reduce to the same 1-triangle record the merge path produces.
	const float tri[] = { -1, -2, -3,  4, 5, 6,  7, 8, 9 };
	float slots[ 12 * 4 ] = {};
	const int numSlots = SoftHull_EmitRecord( tri, 3, slots );
	CHECK( numSlots == 3 );
	CHECK_NEAR( slots[0 * 4 + 3], -3.0f, 0.0f );
	CHECK_NEAR( slots[0 * 4 + 2], -3.0f, 1e-6f );	// vert0.z
	CHECK_NEAR( slots[1 * 4 + 0], 4.0f, 1e-6f );
	CHECK_NEAR( slots[2 * 4 + 2], 9.0f, 1e-6f );	// vert2.z
}

TEST( SoftShadowBrushHull, EmitRejectsOutOfRange )
{
	float slots[ 12 * 4 ] = {};
	const float two[] = { 0, 0, 0,  1, 1, 1 };
	CHECK( SoftHull_EmitRecord( two, 2, slots ) == 0 );				// degenerate (< 3 verts)
	CHECK( SoftHull_EmitRecord( two, SW_HULL_MAX_VERTS + 1, slots ) == 0 );	// over the shader walk cap
}

TEST( SoftShadowBrushHull, EmitEndToEndFromFixture )
{
	// the shipped path: parsed hull -> shared emitter -> shader-facing record. Prove the pentagon-shaped
	// fixture hull (if it were in-range) and the stored area-0 hulls encode with the right slot counts.
	std::vector< std::vector<areaShadowHull_t> > byArea;
	CHECK( ParseFixture( kFixture, byArea ) );
	float slots[ 12 * 4 ] = {};
	// area 0, hull 0 (quad) -> 6 slots, .w=-4
	CHECK( SoftHull_EmitRecord( byArea[0][0].verts, byArea[0][0].numVerts, slots ) == 6 );
	CHECK_NEAR( slots[0 * 4 + 3], -4.0f, 0.0f );
	// area 0, hull 1 (triangle) -> 3 slots, .w=-3
	CHECK( SoftHull_EmitRecord( byArea[0][1].verts, byArea[0][1].numVerts, slots ) == 3 );
	CHECK_NEAR( slots[0 * 4 + 3], -3.0f, 0.0f );
	// area 2, hull 0 (pentagon, 5 verts) -> numTris=(5+2)/3=2 -> 6 slots, .w=-5
	CHECK( SoftHull_EmitRecord( byArea[2][0].verts, byArea[2][0].numVerts, slots ) == 6 );
	CHECK_NEAR( slots[0 * 4 + 3], -5.0f, 0.0f );
	CHECK_NEAR( slots[4 * 4 + 0], -1.0f, 1e-6f );	// pad slot repeats last vert (-1,2,5)
	CHECK_NEAR( slots[4 * 4 + 2], 5.0f, 1e-6f );
}
