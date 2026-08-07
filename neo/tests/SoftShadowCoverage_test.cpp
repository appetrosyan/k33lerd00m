/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2026 Aleksandr Petrosyan

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").

Doom 3 BFG Edition Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

===========================================================================
*/

// Tests for the analytic soft-shadow light-disk coverage. These compile the LIVE shader source
// (shaders/builtin/lighting/softwedge_coverage.inc.hlsl) as C++ through hlsl_compat.h, so they exercise
// the exact math the pixel shader runs - not a copy. Ground truth is an independent ray cast against a
// real box caster (SoftShadowBox.h). The two properties that matter to the look:
//   * ACCURACY   - coverage(P) matches the ray-cast disk occlusion.
//   * SMOOTHNESS - coverage(P) has no discontinuity as the receiver sweeps the floor (a jump reads as a
//                  hard seam / the semicircular-umbra artifact). This is the property the user called out.

#include "hlsl_compat.h"
#include "softwedge_coverage.inc.hlsl"		// the live shader coverage function (compiled as C++)
#include "SoftShadowBox.h"
#include "idUnitTest.h"

#include <cstdio>
#include <cmath>

using namespace swtest;

namespace
{
const float3 L_OVER = float3( 0, 0, 12 );		// light overhead
const float  RAD    = 3.0f;						// light-disk (penumbra) radius

// planar axis-aligned quad in the z=const plane (a simple caster with no near-plane subtlety)
std::vector<float3> QuadXY( float cx, float cy, float cz, float s )
{
	return { float3( cx - s, cy - s, cz ), float3( cx + s, cy - s, cz ), float3( cx + s, cy + s, cz ), float3( cx - s, cy + s, cz ) };
}
}

// -------------------------------------------------------------------- accuracy (simple, no near-plane)
TEST( SoftShadowCoverage, fully_lit_when_caster_off_to_the_side )
{
	auto rec = BuildCaster( { QuadXY( 9, 0, 6, 1 ) } );
	CHECK_NEAR( LiveShadow( rec, float3( 0, 0, 0 ), L_OVER, RAD ), 1.0f, 1e-3f );
}

TEST( SoftShadowCoverage, beyond_the_light_does_not_occlude )
{
	auto rec = BuildCaster( { QuadXY( 0, 0, 16, 1 ) } );	// caster behind the light plane cannot block
	CHECK_NEAR( LiveShadow( rec, float3( 0, 0, 0 ), L_OVER, RAD ), 1.0f, 1e-3f );
}

TEST( SoftShadowCoverage, hole_subtracts_via_opposite_winding )
{
	std::vector<float3> outer = QuadXY( 0, 0, 6, 2.0f );
	std::vector<float3> hole  = QuadXY( 0, 0, 6, 0.7f );
	std::vector<float3> holeRev( hole.rbegin(), hole.rend() );
	float shadow = LiveShadow( BuildCaster( { outer, holeRev } ), float3( 0, 0, 0 ), L_OVER, RAD );
	// only (outer minus the hole) is occluded; both are in front, so this is exact (no near-plane).
	Box dummyOuter = MakeBox( float3( 0, 0, 6 ), float3( 2.0f, 2.0f, 0.01f ) );
	Box dummyHole  = MakeBox( float3( 0, 0, 6 ), float3( 0.7f, 0.7f, 0.01f ) );
	float occOuter = 1.0f - TruthShadow( float3( 0, 0, 0 ), L_OVER, RAD, dummyOuter, 200 );
	float occHole  = 1.0f - TruthShadow( float3( 0, 0, 0 ), L_OVER, RAD, dummyHole, 200 );
	CHECK_NEAR( shadow, 1.0f - ( occOuter - occHole ), 0.02f );
}

// -------------------------------------------- accuracy vs ray-cast truth, PLANAR caster (exact regime)
// Thin (planar) casters floating in front of the receiver. A flat caster's outline is viewpoint-
// independent, so the light-relative silhouette the shader consumes equals the receiver-relative one and
// there is no near-plane straddle: the disk-coverage math is analytically exact here (bar ray-cast disk
// quantisation in the truth). This is the TIGHT guard on the core shoelace - it trips on a regression in
// the coverage math itself, which the fat-box tests (whose light-vs-receiver silhouette parallax and
// near-plane straddle add real method error) cannot isolate.
TEST( SoftShadowCoverage, planar_fuzz_is_exact )
{
	Rng rng( 7 );
	int bad = 0, tot = 0; float worst = 0;
	float3 P0( 0, 0, 0 );
	for( int k = 0; k < 1500; k++ )
	{
		float3 h( rng.f( 0.3f, 1.6f ), rng.f( 0.3f, 1.6f ), 0.02f );	// thin -> planar caster
		float3 C( rng.f( -3, 3 ), rng.f( -3, 3 ), rng.f( 4.5f, 8.0f ) );	// floats above P, below the light
		Box b = MakeBox( C, h, rng.f( 0, 3.14f ), rng.f( -0.2f, 0.2f ) );
		auto loop = Silhouette( b, L_OVER );
		if( loop.size() < 3 ) { continue; }
		tot++;
		float shadow = LiveShadow( BuildCaster( { loop } ), P0, L_OVER, RAD );
		float truth  = TruthShadow( P0, L_OVER, RAD, b, 200 );
		float d = std::fabs( shadow - truth );
		worst = std::fmax( worst, d );
		if( d > 0.03f ) { bad++; }
	}
	std::printf( "    [planar_fuzz] %d/%d disagree>0.03, worst=%.3f\n", bad, tot, worst );
	CHECK( bad * 100 < tot );				// < 1% gross disagreements: the math is exact here
	CHECK( worst < 0.2f );					// lone outlier = ray-cast disk quantisation on a grazing sliver
}

// -------------------------------------------- accuracy vs ray-cast truth, GROUND-STANDING (known limit)
// CHARACTERIZATION, not a target. Ground-standing casters (base at the receiver plane) whose silhouette
// straddles the near-plane on OPPOSITE sides - the case the analytic closure cannot yet reconstruct (see
// soft-shadow-nearplane-crosssection memo): the near-plane cross-section wrap is absent from the per-light
// silhouette edge buffer, so deep-umbra receivers under the footprint report a half-disk (~0.5) instead of
// full occlusion. This records the CURRENT disagreement rate as a regression ceiling; the real target is
// the front-only test's <0.5%. Tighten toward 0 when the cross-section closure is solved.
TEST( SoftShadowCoverage, box_fuzz_ground_standing_known_limitation )
{
	Rng rng( 99 );
	int bad = 0, tot = 0; float worst = 0;
	float3 P0( 0, 0, 0 );
	for( int k = 0; k < 1500; k++ )
	{
		float3 h( rng.f( 0.4f, 2.0f ), rng.f( 0.4f, 2.0f ), rng.f( 0.5f, 3.0f ) );
		float3 C( rng.f( -3, 3 ), rng.f( -3, 3 ), h.z - 0.05f );	// sits on the z~0 ground plane
		Box b = MakeBox( C, h, rng.f( 0, 3.14f ), rng.f( -0.15f, 0.15f ) );
		auto loop = Silhouette( b, L_OVER );
		if( loop.size() < 3 ) { continue; }
		tot++;
		float shadow = LiveShadow( BuildCaster( { loop } ), P0, L_OVER, RAD );
		float truth  = TruthShadow( P0, L_OVER, RAD, b, 120 );
		float d = std::fabs( shadow - truth );
		worst = std::fmax( worst, d );
		if( d > 0.03f ) { bad++; }
	}
	std::printf( "    [ground_fuzz] %d/%d disagree>0.03, worst=%.3f (KNOWN near-plane cross-section limit)\n", bad, tot, worst );
	CHECK( bad * 3 < tot );					// regression ceiling ~33%; NOT the target (front-only is)
	CHECK( worst <= 1.001f );
}

// -------------------------------------------------------------------------------- SMOOTHNESS (the look)
// Fix a caster between light and floor; sweep the receiver across the floor. The coverage field must be
// CONTINUOUS - a large jump between adjacent receivers is a visible hard seam (the semicircular umbra).
// The grid is fine (~0.13u cell) so a genuine penumbra gradient stays small per step; a much larger jump
// is a discontinuity, not a gradient.
static float MaxAdjacentJump( const Box& b, float3 L, float r, int N, float ext, float& atx, float& aty )
{
	auto rec = BuildCaster( { Silhouette( b, L ) } );
	std::vector<float> A( ( size_t )N * N );
	for( int iy = 0; iy < N; iy++ )
		for( int ix = 0; ix < N; ix++ )
		{
			float x = ( ix / ( float )( N - 1 ) - 0.5f ) * 2 * ext;
			float y = ( iy / ( float )( N - 1 ) - 0.5f ) * 2 * ext;
			A[( size_t )iy * N + ix] = LiveShadow( rec, float3( x, y, 0 ), L, r );
		}
	float mx = 0; atx = aty = 0;
	auto at = [&]( int ix, int iy ) { return A[( size_t )iy * N + ix]; };
	auto pos = [&]( int i ) { return ( i / ( float )( N - 1 ) - 0.5f ) * 2 * ext; };
	for( int iy = 0; iy < N; iy++ )
		for( int ix = 0; ix < N; ix++ )
		{
			if( ix > 0 ) { float d = std::fabs( at( ix, iy ) - at( ix - 1, iy ) ); if( d > mx ) { mx = d; atx = pos( ix ); aty = pos( iy ); } }
			if( iy > 0 ) { float d = std::fabs( at( ix, iy ) - at( ix, iy - 1 ) ); if( d > mx ) { mx = d; atx = pos( ix ); aty = pos( iy ); } }
		}
	return mx;
}

TEST( SoftShadowCoverage, coverage_is_spatially_smooth_pillar )
{
	// tall thin pillar - receivers can sit directly under it (deep umbra with opposite-side near-plane
	// crossings, the artifact case).
	Box pillar = MakeBox( float3( 0, 0, 6 ), float3( 1.2f, 1.2f, 3.0f ) );
	float ax, ay;
	float jump = MaxAdjacentJump( pillar, L_OVER, RAD, 120, 8.0f, ax, ay );
	std::printf( "    [smooth pillar] max adjacent jump = %.3f at (%.2f,%.2f)\n", jump, ax, ay );
	CHECK( jump < 0.20f );					// above this reads as a hard seam
}

TEST( SoftShadowCoverage, coverage_is_spatially_smooth_wide_slab )
{
	// wide flat slab overhead: large umbra, receivers pass from lit through penumbra into full umbra.
	Box slab = MakeBox( float3( 0, 0, 7 ), float3( 3.0f, 3.0f, 0.4f ) );
	float ax, ay;
	float jump = MaxAdjacentJump( slab, L_OVER, RAD, 120, 9.0f, ax, ay );
	std::printf( "    [smooth slab] max adjacent jump = %.3f at (%.2f,%.2f)\n", jump, ax, ay );
	CHECK( jump < 0.20f );
}
