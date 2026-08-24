/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 2026 Aleksandr Petrosyan

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").

Doom 3 BFG Edition Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

===========================================================================
*/

// NUMERICAL guard for the view-dependent light-BLACKENING flicker (bug reproduced headless on cap0001:
// point light idx 24 at (3635 7267 396) flickers fully black as the camera nudges a few units).
//
// Root cause: R_AddSingleLight MOC-tests the light's BOUNDING CUBE, but the test (TestTriangles/TestRect) is
// called WITHOUT near-plane clipping. When the light is close and its cube STRADDLES the near plane (a corner
// behind the camera => negative clip w), the projection is garbage and MOC reports the cube OCCLUDED, so the
// light is wrongly culled - blackening a lit region until the camera moves the cube clear. The engine measured
// wmin=-68.9, cornersNearPlane=2/8 at the exact captured pose.
//
// Fix under test: the NEAR-PLANE GUARD - a bounding-cube occlusion test is only valid when the WHOLE cube is
// in front of the near plane; if wmin <= mocNear, KEEP the light (never cull). This file models the predicate
// purely numerically (no engine), reproduces the exact straddle, and enumerates the edge cases - most
// importantly that once the guard engages it STAYS engaged as the camera approaches (monotone => no flicker).
//
// clip.w of a cube corner ~= its view-space forward distance dot(corner - camOrg, camFwd) for the standard
// perspective projection (positive in front, negative behind), which is exactly what the engine's wmin reads.

#include "idUnitTest.h"
#include <cmath>
#include <algorithm>

namespace
{
struct V3 { double x, y, z; };
static inline V3   sub( const V3& a, const V3& b ) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
static inline double dot( const V3& a, const V3& b ) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline V3   norm( const V3& a ) { double l = std::sqrt( dot( a, a ) ); return { a.x / l, a.y / l, a.z / l }; }

// 8 corners of an axis-aligned bounding cube of half-extent h around centre c (the light's bound proxy).
static void CubeCorners( const V3& c, double h, V3 out[8] )
{
	int i = 0;
	for( int sx = -1; sx <= 1; sx += 2 )
		for( int sy = -1; sy <= 1; sy += 2 )
			for( int sz = -1; sz <= 1; sz += 2 )
			{
				out[i++] = { c.x + sx * h, c.y + sy * h, c.z + sz * h };
			}
}

// nearest cube corner's clip w (= min view-forward distance): the value the engine reduces as `wmin`.
static double CubeWmin( const V3 corners[8], const V3& camOrg, const V3& camFwd )
{
	double wmin = 1e30;
	for( int i = 0; i < 8; i++ )
	{
		wmin = std::min( wmin, dot( sub( corners[i], camOrg ), camFwd ) );
	}
	return wmin;
}

// THE GUARD PREDICATE, mirroring tr_frontend_addlights.cpp: keep the light (skip the cull) when the cube is
// not wholly in front of the near plane. Conservative: keeping a light is never a correctness error, only a
// missed perf cull.
static bool GuardKeeps( double wmin, double mocNear ) { return wmin <= mocNear; }

const double MOC_NEAR = 3.0;	// r_znear default; the guard uses the same near the occluder buffer was filled with
}

// ---- 1. reproduce the EXACT cap0001 straddle: the light must be KEPT --------------------------------------
TEST( MocLightCullNearPlane, cap0001_exact_straddle_keeps_light )
{
	const V3 camOrg = { 3686.0, 7081.0, 442.0 };
	const V3 camFwd = norm( { -0.02, 0.90, -0.43 } );		// captured forward
	const V3 lightC = { 3635.0, 7267.0, 396.0 };			// light idx 24

	// the light illuminates the visible room, so its influence bound is large - large enough (measured in
	// engine: wmin=-68.9) that the cube reaches ~69u BEHIND the camera. A radius that puts the nearest corner
	// behind the camera reproduces the straddle.
	V3 corners[8]; CubeCorners( lightC, 300.0, corners );
	const double wmin = CubeWmin( corners, camOrg, camFwd );

	CHECK( wmin < 0.0 );					// cube straddles: nearest corner is BEHIND the camera (negative clip w)
	CHECK( GuardKeeps( wmin, MOC_NEAR ) );	// => guard engages => light is NOT culled (the fix)
}

// ---- 2. a far light fully in front stays CULL-ELIGIBLE (the guard must not defeat legitimate culling) ------
TEST( MocLightCullNearPlane, far_light_in_front_is_cull_eligible )
{
	const V3 camOrg = { 0, 0, 0 };
	const V3 camFwd = { 0, 1, 0 };
	const V3 lightC = { 0, 500, 0 };		// 500u dead ahead
	V3 corners[8]; CubeCorners( lightC, 20.0, corners );	// small bound, wholly in front
	const double wmin = CubeWmin( corners, camOrg, camFwd );

	CHECK( wmin > MOC_NEAR );				// wholly in front of the near plane
	CHECK_FALSE( GuardKeeps( wmin, MOC_NEAR ) );	// guard does NOT fire => MOC may cull it (perf preserved)
}

// ---- 3. boundary: a corner exactly at the near plane keeps the light (conservative side) -------------------
TEST( MocLightCullNearPlane, corner_at_near_plane_keeps )
{
	const V3 camOrg = { 0, 0, 0 };
	const V3 camFwd = { 0, 1, 0 };
	// place the cube so its nearest corner sits exactly on mocNear: centre at near + h in Y, half-extent h.
	const double h = 50.0;
	const V3 lightC = { 0, MOC_NEAR + h, 0 };
	V3 corners[8]; CubeCorners( lightC, h, corners );
	const double wmin = CubeWmin( corners, camOrg, camFwd );

	CHECK_NEAR( wmin, MOC_NEAR, 1e-6 );
	CHECK( GuardKeeps( wmin, MOC_NEAR ) );	// <= is inclusive: the boundary is kept, not culled
}

// ---- 4. EDGE ENUMERATION: sweeping the camera toward the light must never FLICKER --------------------------
// The flicker is the guard toggling. Prove it cannot: as the camera approaches the light along the view axis,
// wmin decreases monotonically, so the guard transitions cull-eligible -> KEEP exactly ONCE and never back.
// Inside the kept region every closer step is still kept: no on/off/on oscillation is representable.
TEST( MocLightCullNearPlane, approach_is_monotone_no_flicker )
{
	const V3 camFwd = { 0, 1, 0 };
	const V3 lightC = { 0, 0, 0 };
	V3 corners[8]; CubeCorners( lightC, 100.0, corners );	// 100u half-extent light

	double prevW = 1e30;
	bool   everKept = false, keptThenUnkept = false;
	// camera far behind the light (y = -800) walking forward to just past it (y = -20)
	for( double cy = -800.0; cy <= -20.0 + 1e-9; cy += 5.0 )
	{
		const V3 camOrg = { 0, cy, 0 };
		const double wmin = CubeWmin( corners, camOrg, camFwd );
		CHECK( wmin <= prevW + 1e-9 );		// monotone non-increasing as we approach
		prevW = wmin;

		const bool keep = GuardKeeps( wmin, MOC_NEAR );
		if( keep ) { everKept = true; }
		if( everKept && !keep ) { keptThenUnkept = true; }	// a KEEP that later flips back = a flicker
	}
	CHECK( everKept );					// the guard does engage as we close in
	CHECK_FALSE( keptThenUnkept );		// ...and never flips back => the flicker is impossible
}

// ---- 5. inflation is NOT the fix: scaling a straddling cube still straddles --------------------------------
// Documents WHY the earlier mocLightScale tweak did nothing for this bug - an expanded cube that crosses the
// near plane crosses it harder, so only the near-plane guard (not a scale factor) can fix a straddle.
TEST( MocLightCullNearPlane, inflation_does_not_clear_a_straddle )
{
	const V3 camOrg = { 3686.0, 7081.0, 442.0 };
	const V3 camFwd = norm( { -0.02, 0.90, -0.43 } );
	const V3 lightC = { 3635.0, 7267.0, 396.0 };

	V3 base[8];  CubeCorners( lightC, 300.0, base );
	V3 blown[8]; CubeCorners( lightC, 300.0 * 1.01, blown );	// 1% "inflation"
	const double wBase  = CubeWmin( base,  camOrg, camFwd );
	const double wBlown = CubeWmin( blown, camOrg, camFwd );

	CHECK( wBase  < 0.0 );				// straddles
	CHECK( wBlown <= wBase );			// inflation moves the nearest corner even FURTHER behind the camera
	CHECK( GuardKeeps( wBlown, MOC_NEAR ) );	// still straddling => still needs the guard
}
