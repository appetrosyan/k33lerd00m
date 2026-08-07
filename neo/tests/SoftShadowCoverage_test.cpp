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

// ------------------------------------------------------- ROOT CAUSE: the fed silhouette is the ceiling
// The ground-standing disagreement is NOT primarily the near-plane cross-section wrap (that is a minority
// of the cases). The disk-coverage integral projects the silhouette from the RECEIVER, so the boundary of
// the occluded disk region is the caster's outline as seen from the RECEIVER. The engine instead feeds the
// LIGHT-relative silhouette (R_CollectPenumbraEdges builds it from the light), which for a fat 3D caster is
// a different outline - and that mismatch, not the closure, dominates the error. Proof: re-running the SAME
// coverage math on the receiver-relative silhouette collapses the failures by ~20x and holds across radii,
// so it does not vanish with the disk (it is not parallax) and it is not an implementation bug. The lone
// residual worst=1.0 is one grazing config where a tiny receiver silhouette flips fully occluded/lit; the
// RATE is the honest measure. This test locks that decomposition in: if someone "fixes" the near-plane
// closure and the light-silhouette rate barely moves, this is why.
TEST( SoftShadowCoverage, ceiling_is_the_light_silhouette_not_the_closure )
{
	Rng rng( 99 );							// same caster distribution as the ground-standing test
	float3 P0( 0, 0, 0 );
	int badLight = 0, badRecv = 0, tot = 0;
	for( int k = 0; k < 1500; k++ )
	{
		float3 h( rng.f( 0.4f, 2.0f ), rng.f( 0.4f, 2.0f ), rng.f( 0.5f, 3.0f ) );
		float3 C( rng.f( -3, 3 ), rng.f( -3, 3 ), h.z - 0.05f );
		Box b = MakeBox( C, h, rng.f( 0, 3.14f ), rng.f( -0.15f, 0.15f ) );
		std::vector<float3> loopL = Silhouette( b, L_OVER );	// what the engine feeds
		std::vector<float3> loopP = Silhouette( b, P0 );		// receiver-relative silhouette
		if( loopL.size() < 3 || loopP.size() < 3 ) { continue; }
		tot++;
		float truth = TruthShadow( P0, L_OVER, RAD, b, 160 );
		if( std::fabs( LiveShadow( BuildCaster( { loopL } ), P0, L_OVER, RAD ) - truth ) > 0.03f ) { badLight++; }
		if( std::fabs( LiveShadow( BuildCaster( { loopP } ), P0, L_OVER, RAD ) - truth ) > 0.03f ) { badRecv++; }
	}
	std::printf( "    [silhouette_source] light-fed %d/%d fail   receiver-fed %d/%d fail\n",
			badLight, tot, badRecv, tot );
	CHECK( badRecv * 8 < badLight );		// receiver silhouette is >8x more accurate: the fed outline IS the ceiling
	CHECK( badRecv * 20 < tot );			// and it is near-exact in absolute terms (< 5%)
}

// ============================================================= THE CONFINE SPEC (documented invariants)
// The optimization runs the expensive coverage loop only inside a confine band, and its LOSSLESSNESS rests
// on four invariants of the coverage field that were previously asserted only by looking at the screen:
//   (1) LIT      - a fragment the caster does not occlude reads coverage 0 exactly (the band excludes it).
//   (2) PENUMBRA - a partially-occluded fragment reads strictly between 0 and 1 (the band runs coverage).
//   (3) UMBRA    - a fully-occluded fragment reads 1 (solid; the band may draw it black without the loop).
//   (4) NO OUTER CLIP - coverage stays > 0 across the WHOLE true penumbra; it never collapses to lit while
//       any occlusion remains. This is the one the confine cannot violate: clipping the outer penumbra to
//       lit is a visible hard edge and a lossy cut. It is the conservative (superset) side of the band.
// Asserted on PLANAR casters, where the light-disk math is exact (fat-caster fidelity is bounded separately,
// see ceiling_is_the_light_silhouette_not_the_closure). A planar caster is a razor-thin box, so these run
// the same live silhouette + ray-cast-truth path as every other test - no bespoke geometry.

static Box PlanarBox( float cx, float cy, float cz, float sx, float sy, float yaw = 0.0f )
{
	return MakeBox( float3( cx, cy, cz ), float3( sx, sy, 0.02f ), yaw, 0.0f );	// razor-thin => planar silhouette
}

TEST( SoftShadowCoverage, confine_regimes_lit_penumbra_umbra )
{
	float3 P0( 0, 0, 0 );
	// (3) UMBRA: a wide planar caster midway to the light throws a shadow covering the whole disk.
	Box umbra = PlanarBox( 0, 0, 6, 4.0f, 4.0f );
	float sU = LiveShadow( BuildCaster( { Silhouette( umbra, L_OVER ) } ), P0, L_OVER, RAD );
	CHECK_NEAR( TruthShadow( P0, L_OVER, RAD, umbra, 200 ), 0.0f, 1e-3f );	// truth agrees: fully occluded
	CHECK_NEAR( sU, 0.0f, 1e-3f );											// SOLID umbra (shadow 0 == coverage 1)

	// (2) PENUMBRA: a small caster over the centre occludes only part of the disk.
	Box pen = PlanarBox( 0, 0, 6, 0.8f, 0.8f );
	float sP = LiveShadow( BuildCaster( { Silhouette( pen, L_OVER ) } ), P0, L_OVER, RAD );
	float tP = TruthShadow( P0, L_OVER, RAD, pen, 1200 );					// fine grid: ray-cast boundary quantisation ~1/N
	CHECK( tP > 0.02f && tP < 0.98f );										// truth is genuinely partial
	CHECK( sP > 0.02f && sP < 0.98f );										// coverage RUNS (strictly between)
	CHECK_NEAR( sP, tP, 0.01f );											// and matches truth exactly (planar): closed form = 0.638

	// (1) LIT: a caster off to the side occludes nothing.
	Box lit = PlanarBox( 10, 0, 6, 1.0f, 1.0f );
	float sL = LiveShadow( BuildCaster( { Silhouette( lit, L_OVER ) } ), P0, L_OVER, RAD );
	CHECK_NEAR( sL, 1.0f, 1e-4f );											// exactly lit (coverage 0)
}

TEST( SoftShadowCoverage, confine_never_clips_the_outer_penumbra )
{
	// Slide a planar caster from over the centre outwards until its shadow fully leaves the disk. Across the
	// whole traversal the coverage must track truth and, crucially, stay > 0 for as long as ANY occlusion
	// remains (invariant 4): the moment coverage hits lit while truth is still partial, the confine would
	// clip a real penumbra to a hard lit edge. We also confirm the far end genuinely reaches lit (no false
	// permanent shadow) so the test cannot pass by clamping everything to shadowed.
	float3 P0( 0, 0, 0 );
	int sawPartial = 0, reachedLit = 0;
	float worst = 0.0f;
	for( int i = 0; i <= 40; i++ )
	{
		float cx = i * 0.15f;								// 0 .. 6 units off-centre
		Box b = PlanarBox( cx, 0, 6, 0.8f, 0.8f );
		float shadow = LiveShadow( BuildCaster( { Silhouette( b, L_OVER ) } ), P0, L_OVER, RAD );
		float truth  = TruthShadow( P0, L_OVER, RAD, b, 220 );
		worst = std::fmax( worst, std::fabs( shadow - truth ) );
		if( truth < 0.999f )								// truth still sees occlusion here
		{
			CHECK( shadow < 0.9999f );						// => coverage must NOT have collapsed to lit
			sawPartial++;
		}
		if( truth > 0.9999f && shadow > 0.9999f ) { reachedLit++; }
	}
	CHECK( sawPartial > 5 );								// the sweep really did traverse the penumbra
	CHECK( reachedLit > 0 );								// and really did reach full light at the far end
	CHECK( worst < 0.02f );									// planar: exact the whole way (no premature clip)
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

// ============================================== MULTI-PART CASTER: joints must not leak (tripod / rock)
// A tripod or a rock is several caster SURFACES that abut at joints. In the shipping frontend each surface
// gets its own casterId, and the coverage loop UNIONS casters by max( occ ). Where two surfaces meet, no
// SINGLE surface occludes the whole light disk - one covers one half, the other the other half - so max()
// reports ~0.5 at the seam: a hairline lit crack running through what should be solid umbra (USER: joints
// and where multiple faces meet produce no coverage; rocks show hairline lit lines inside the full umbra).
// The union is fully occluded; the bug is purely the max combine. Grouping the surfaces as ONE caster makes
// their coverage integrals SUM (disjoint areas on the disk add), reconstructing the union - the per-entity
// grouping fix. This test reproduces the leak and proves the fix on the exact geometry that triggers it.
//
// Two thin half-slabs meeting at the seam x=0, both between the light and the receiver directly under the
// seam. Each half alone covers about half the disk; together they fully occlude it.
TEST( SoftShadowCoverage, multipart_joint_does_not_leak_when_summed )
{
	float3 P0( 0, 0, 0 );
	// two rectangles splitting a 3.2 x 3.2 slab at the seam x=0; each half is 1.6 x 3.2, wound CCW. The union
	// projects (scale 12/6 = 2) to 6.4 x 6.4, comfortably covering the r=3 disk => the union is FULL umbra.
	std::vector<float3> left  = { float3( -1.6f, -1.6f, 6 ), float3( 0, -1.6f, 6 ), float3( 0, 1.6f, 6 ), float3( -1.6f, 1.6f, 6 ) };
	std::vector<float3> right = { float3( 0, -1.6f, 6 ), float3( 1.6f, -1.6f, 6 ), float3( 1.6f, 1.6f, 6 ), float3( 0, 1.6f, 6 ) };

	// ground truth: the union is one solid slab spanning both halves.
	Box unionSlab = MakeBox( float3( 0, 0, 6 ), float3( 1.6f, 1.6f, 0.02f ) );
	float truth = TruthShadow( P0, L_OVER, RAD, unionSlab, 400 );

	// SHIPPING BUG: two separate casters (two headers) -> the loop max-combines them.
	std::vector<float4> sep = BuildCaster( { left } );
	std::vector<float4> rr  = BuildCaster( { right } );
	sep.insert( sep.end(), rr.begin(), rr.end() );				// concatenate = two casterIds in one light
	float maxCombine = LiveShadow( sep, P0, L_OVER, RAD );

	// FIX: one caster carrying both surfaces -> the loop sums their coverage.
	float sumCombine = LiveShadow( BuildCaster( { left, right } ), P0, L_OVER, RAD );

	std::printf( "    [joint] truth=%.3f  max-combine(shipping)=%.3f  sum-combine(fixed)=%.3f\n",
			truth, maxCombine, sumCombine );
	CHECK( truth < 0.05f );							// the union genuinely occludes the disk (solid umbra)
	CHECK( maxCombine > truth + 0.2f );				// max-combine LEAKS: a bright crack at the joint (the bug)
	CHECK_NEAR( sumCombine, truth, 0.03f );			// per-entity SUM is solid: the joint closes (the fix)
}
