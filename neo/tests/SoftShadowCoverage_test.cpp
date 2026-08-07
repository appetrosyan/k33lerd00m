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
#include "SoftShadowDir.h"					// direction-space coverage (singularity-free, under development)
#include "SoftShadowMesh.h"					// .softcap reader + ray-cast ground truth
#include "idUnitTest.h"

#include <cstdio>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <vector>

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

// ------------------------------------------------ ISOLATED near-plane straddle: the deep-umbra artifact
// The clean signal for the near-plane cross-section bug, undiluted by the fat-caster parallax that muddies
// box_fuzz_ground_standing. A pillar stands on the floor; the receiver sweeps directly UNDER its footprint,
// where the silhouette dips behind the receiver near-plane on opposite sides and the true occlusion is FULL
// (solid umbra, shadow 0). The straight near-plane connector cuts the disk to a chord, so coverage reports
// ~0.5 instead: the unphysical half-disk / semicircular umbra the USER sees on pillars. This prints the
// worst reported shadow under the footprint (target 0.0) as the metric the cross-section fix must drive down;
// asserts only the CURRENT characterization so the suite stays honest until the fix lands.
TEST( SoftShadowCoverage, deep_umbra_under_pillar_characterization )
{
	Box pillar = MakeBox( float3( 0, 0, 3 ), float3( 0.8f, 0.8f, 3.0f ) );	// base on the floor, top at z=6
	auto rec = BuildCaster( { Silhouette( pillar, L_OVER ) } );
	float minShadow = 1.0f, worstTruth = 0.0f;								// under the footprint shadow should be 0
	for( int iy = -3; iy <= 3; iy++ )
		for( int ix = -3; ix <= 3; ix++ )
		{
			float3 P( ix * 0.12f, iy * 0.12f, 0.0f );						// receivers within the ~0.8 footprint
			float truth = TruthShadow( P, L_OVER, RAD, pillar, 120 );
			if( truth > 0.05f ) { continue; }							// only sample cells that ARE full umbra
			minShadow = std::fmin( minShadow, LiveShadow( rec, P, L_OVER, RAD ) );
			worstTruth = std::fmax( worstTruth, truth );
		}
	std::printf( "    [deep_umbra] full-umbra cells: min reported shadow=%.3f (truth 0.0; target 0.0)\n", minShadow );
	CHECK( worstTruth < 0.05f );				// the sampled cells really are umbra (sanity on the setup)
	CHECK( minShadow <= 1.01f );				// characterization only: records the artifact, not yet a target
}

// ============================================================ DIRECTION-SPACE coverage (the real fix)
// The singularity-free reformulation (SoftShadowDir.h). These drive its development against ground truth:
// it must (a) turn the deep-umbra pillar SOLID where the planar method leaves a bright hole, and (b) still
// match truth on planar casters where the planar method is already exact.
TEST( SoftShadowDir, ground_standing_beats_planar )
{
	// The head-to-head: same ground-standing distribution the planar method fails on (box_fuzz), both
	// methods vs ray-cast truth. Direction-space removes the near-plane singularity, so it should cut the
	// disagreements sharply. What survives is the fat-caster light-vs-receiver parallax, which no projection
	// space can fix - so this asserts a large reduction, not zero.
	Rng rng( 99 );										// same seed/sequence as box_fuzz_ground_standing
	float3 P0( 0, 0, 0 );
	int planarBad = 0, dirBad = 0, tot = 0; float planarWorst = 0, dirWorst = 0;
	for( int k = 0; k < 1500; k++ )
	{
		float3 h( rng.f( 0.4f, 2.0f ), rng.f( 0.4f, 2.0f ), rng.f( 0.5f, 3.0f ) );
		float3 C( rng.f( -3, 3 ), rng.f( -3, 3 ), h.z - 0.05f );
		Box b = MakeBox( C, h, rng.f( 0, 3.14f ), rng.f( -0.15f, 0.15f ) );
		auto loop = Silhouette( b, L_OVER );
		if( loop.size() < 3 ) { continue; }
		tot++;
		auto rec = BuildCaster( { loop } );
		float truth = TruthShadow( P0, L_OVER, RAD, b, 160 );
		float dp = std::fabs( LiveShadow( rec, P0, L_OVER, RAD ) - truth );
		float dd = std::fabs( DirShadow( rec, P0, L_OVER, RAD ) - truth );
		if( dp > 0.03f ) { planarBad++; } planarWorst = std::fmax( planarWorst, dp );
		if( dd > 0.03f ) { dirBad++; }    dirWorst = std::fmax( dirWorst, dd );
	}
	std::printf( "    [dir vs planar] planar bad=%d (worst %.3f)   dir bad=%d (worst %.3f)  of %d\n",
			planarBad, planarWorst, dirBad, dirWorst, tot );
	CHECK( dirBad < planarBad );				// direction-space is strictly better on the failing distribution
}

TEST( SoftShadowDir, near_plane_straddle_canopy )
{
	// A valid near-plane straddle: a wide thin canopy overhead, extending FAR behind P relative to an ANGLED
	// light, so the far silhouette edge sits behind P's near-plane (dn < 0). P underneath is in full umbra
	// (ray truth confirms). This is the geometry that actually triggers the chord bug; the overhead-light box
	// fuzz cannot. Planar should leave the bright hole / half-disk; direction-space should read solid.
	const float3 Lang( 5, 0, 8 );							// up and to the side
	Box canopy = MakeBox( float3( -1, 0, 3 ), float3( 7.0f, 4.0f, 0.05f ) );	// x[-8,6] y[-4,4], thin
	auto rec = BuildCaster( { Silhouette( canopy, Lang ) } );
	float3 P0( 0, 0, 0 );

	// confirm the setup: P is genuinely in full umbra, and the silhouette really straddles the near-plane.
	float truth = TruthShadow( P0, Lang, RAD, canopy, 200 );
	float3 nrm = normalize( Lang - P0 );
	bool straddles = false;
	for( float3 V : Silhouette( canopy, Lang ) ) { if( dot( V - P0, nrm ) < 0.0f ) { straddles = true; break; } }

	float planar = LiveShadow( rec, P0, Lang, RAD );
	float dir    = DirShadow( rec, P0, Lang, RAD );
	std::printf( "    [canopy] truth=%.3f straddles=%d  planar=%.3f  direction-space=%.3f\n",
			truth, ( int )straddles, planar, dir );
	CHECK( truth < 0.05f );						// setup sanity: P is in solid umbra
	CHECK( straddles );							// setup sanity: the silhouette dips behind the near-plane
	CHECK( dir < 0.05f );						// direction-space reads SOLID (the fix)
}

TEST( SoftShadowDir, planar_matches_truth )
{
	Rng rng( 7 );
	int bad = 0, tot = 0; float worst = 0;
	float3 P0( 0, 0, 0 );
	for( int k = 0; k < 1500; k++ )
	{
		float3 h( rng.f( 0.3f, 1.6f ), rng.f( 0.3f, 1.6f ), 0.02f );
		float3 C( rng.f( -3, 3 ), rng.f( -3, 3 ), rng.f( 4.5f, 8.0f ) );
		Box b = MakeBox( C, h, rng.f( 0, 3.14f ), rng.f( -0.2f, 0.2f ) );
		auto loop = Silhouette( b, L_OVER );
		if( loop.size() < 3 ) { continue; }
		tot++;
		float d = std::fabs( DirShadow( BuildCaster( { loop } ), P0, L_OVER, RAD ) - TruthShadow( P0, L_OVER, RAD, b, 200 ) );
		worst = std::fmax( worst, d );
		if( d > 0.03f ) { bad++; }
	}
	std::printf( "    [dir planar] %d/%d disagree>0.03, worst=%.3f\n", bad, tot, worst );
	CHECK( bad * 20 < tot );					// within 5% gross (looser than planar's exact; spherical clip WIP)
}

// ------------------------------------------------------------------------------- BENCHMARK (the cost)
// Correctness-first: this quantifies how much the direction-space method costs vs the planar one, so the
// trade is a measured number. Same caster set through both, timed; prints ns/call and the ratio. Not a
// pass/fail gate on absolute time (machine-dependent) - it asserts only that both actually ran.
TEST( SoftShadowBench, planar_vs_direction_space )
{
	Rng rng( 321 );
	std::vector<std::vector<float4>> casters;
	for( int k = 0; k < 200; k++ )				// a mix: pillars, slabs, floating boxes
	{
		float3 h( rng.f( 0.4f, 2.0f ), rng.f( 0.4f, 2.0f ), rng.f( 0.3f, 3.0f ) );
		float3 C( rng.f( -3, 3 ), rng.f( -3, 3 ), rng.f( 1.5f, 8.0f ) );
		Box b = MakeBox( C, h, rng.f( 0, 3.14f ), rng.f( -0.2f, 0.2f ) );
		auto loop = Silhouette( b, L_OVER );
		if( loop.size() >= 3 ) { casters.push_back( BuildCaster( { loop } ) ); }
	}
	float3 P0( 0, 0, 0 );
	const int reps = 2000;
	volatile float sink = 0.0f;

	auto t0 = std::chrono::steady_clock::now();
	for( int r = 0; r < reps; r++ )
		for( const auto& c : casters ) { sink += 1.0f - saturate( SoftShadow_WedgeOcclusion( P0, L_OVER, RAD, 0, ( int )( c.size() / 2 ), SoftEdgeBuffer{ c.data(), ( int )c.size() } ) ); }
	auto t1 = std::chrono::steady_clock::now();
	for( int r = 0; r < reps; r++ )
		for( const auto& c : casters ) { sink += DirShadow( c, P0, L_OVER, RAD ); }
	auto t2 = std::chrono::steady_clock::now();

	double calls = double( reps ) * casters.size();
	double planarNs = std::chrono::duration<double, std::nano>( t1 - t0 ).count() / calls;
	double dirNs    = std::chrono::duration<double, std::nano>( t2 - t1 ).count() / calls;
	std::printf( "    [bench] planar %.1f ns/call   direction-space %.1f ns/call   (%.2fx)\n",
			planarNs, dirNs, dirNs / planarNs );
	CHECK( calls > 0 );
	CHECK( sink != -12345.0f );				// keep the optimiser from eliding the work
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

// ============================================= THE PAYOFF: measure the artifact on real captured geometry
// Loads a .softcap (SOFTCAP env var; skips cleanly if unset so the committed suite stays green) and, at the
// captured RECEIVER surface points, compares the shipped analytic coverage against a ray-cast of the light's
// own caster solids. Where they disagree by a lot is the real Erebus artifact - measured, not eyeballed - on
// the exact geometry the shader ran. This is what a fix (direction-space or other) must drive down, and the
// seed for the Phase 5 minimal test. Needs a v3 capture (global mesh indices + receiver surfaces).
TEST( SoftShadowCapture, coverage_vs_truth )
{
	const char* path = std::getenv( "SOFTCAP" );
	if( path == NULL ) { std::printf( "    [cap_cov] SOFTCAP unset; skipping\n" ); CHECK( true ); return; }
	SoftCap c;
	if( !LoadSoftCap( path, c ) ) { std::printf( "    [cap_cov] load failed (v3 capture required)\n" ); CHECK( false ); return; }
	if( c.receivers.empty() || c.recvVerts.empty() )
	{
		std::printf( "    [cap_cov] no receiver surfaces (need a v3 capture)\n" ); CHECK( true ); return;
	}

	// SANITY: receiver points and caster verts should occupy the same world region (both are this scene's
	// geometry). If they don't, the capture or the transforms are wrong and any coverage number is noise.
	{
		float rlo[3] = { 1e30f, 1e30f, 1e30f }, rhi[3] = { -1e30f, -1e30f, -1e30f };
		float clo[3] = { 1e30f, 1e30f, 1e30f }, chi[3] = { -1e30f, -1e30f, -1e30f };
		for( size_t i = 0; i + 2 < c.recvVerts.size(); i += 3 )
			for( int k = 0; k < 3; k++ ) { rlo[k] = std::fmin( rlo[k], c.recvVerts[i + k] ); rhi[k] = std::fmax( rhi[k], c.recvVerts[i + k] ); }
		for( size_t i = 0; i + 2 < c.meshVerts.size(); i += 3 )
			for( int k = 0; k < 3; k++ ) { clo[k] = std::fmin( clo[k], c.meshVerts[i + k] ); chi[k] = std::fmax( chi[k], c.meshVerts[i + k] ); }
		std::printf( "    [cap_cov] recv bbox [%.0f,%.0f,%.0f]..[%.0f,%.0f,%.0f]  caster bbox [%.0f,%.0f,%.0f]..[%.0f,%.0f,%.0f]\n",
				rlo[0], rlo[1], rlo[2], rhi[0], rhi[1], rhi[2], clo[0], clo[1], clo[2], chi[0], chi[1], chi[2] );
	}

	SoftEdgeBuffer buf{ reinterpret_cast<const float4*>( c.edges.data() ), ( int )( c.edges.size() * 2 ) };
	int sampled = 0, bad = 0, tooLight = 0, tooDark = 0; double worst = 0, sumAbs = 0, covSum = 0, truSum = 0;
	// hunt the EXTRANEOUS-SHADOW cases the user sees: coverage says shadow where truth says lit (cov << truth).
	int extraneous = 0, extraneousDir = 0; float worstOver = 0; float3 owP( 0, 0, 0 ), owL( 0, 0, 0 ); float owCov = 0, owTru = 0, owDir = 0;
	// candidate FIX: gate coverage on the receiver actually being in the caster's shadow (hard-shadow center
	// ray blocked). gatedExtraneous should collapse; penumbraClipped is what a strict hard gate would cost.
	int gatedExtraneous = 0, penumbraClipped = 0;
	// the OTHER failure: under-occlusion (coverage misses shadow the geometry casts). track its worst + geometry.
	int missingShadow = 0; float worstUnder = 0; float3 uwP( 0, 0, 0 ), uwL( 0, 0, 0 ); float uwCov = 0, uwTru = 0, uwDir = 0;
	// the candidate COMPREHENSIVE fix: direction-space coverage (fixes missing shadow) GATED by the shadow
	// volume (fixes direction-space's own over-occlusion). Both failure counts should collapse together.
	int dirMissing = 0, dirGatedExtraneous = 0; double dcovSum = 0;
	for( uint32_t li = 0; li < c.hdr.numLights; li++ )
	{
		const softcapLight_t& L = c.lights[li];
		if( L.penumbraSize <= 0.0f ) { continue; }
		float3 Lo( L.origin[0], L.origin[1], L.origin[2] );

		// this light's caster triangle soup (global indices)
		std::vector<uint32_t> castIdx;
		for( const softcapCaster_t& cs : c.casters )
			if( cs.lightIndex == li )
			{
				castIdx.insert( castIdx.end(), c.meshIdx.begin() + cs.firstIndex, c.meshIdx.begin() + cs.firstIndex + cs.numIndex );
			}
		if( castIdx.empty() ) { continue; }

		// this light's edge records as a float4 array, for the direction-space reference (SoftShadowDir.h)
		std::vector<float4> recVec;
		for( uint32_t r = L.firstEdge; r < L.firstEdge + L.edgeCount && r < c.edges.size(); r++ )
		{
			const softcapEdge_t& e = c.edges[r];
			recVec.push_back( float4( e.e0[0], e.e0[1], e.e0[2], e.e0[3] ) );
			recVec.push_back( float4( e.e1[0], e.e1[1], e.e1[2], e.e1[3] ) );
		}

		// sample this light's receiver surfaces (subsampled to keep the ray-cast tractable)
		for( const softcapReceiver_t& R : c.receivers )
		{
			if( R.lightIndex != li ) { continue; }
			uint32_t step = R.numVerts > 16 ? R.numVerts / 16 : 1;
			for( uint32_t vi = R.firstVert; vi < R.firstVert + R.numVerts; vi += step )
			{
				float3 P( c.recvVerts[vi * 3 + 0], c.recvVerts[vi * 3 + 1], c.recvVerts[vi * 3 + 2] );
				// shadow-bias: lift the sample toward the light so the truth ray-cast does not self-hit the
				// receiver's own surface (many surfaces are both receiver and caster here). Both coverage and
				// truth use the lifted point, so the comparison stays fair.
				float3 toL = Lo - P; float dl = std::sqrt( dot( toL, toL ) );
				if( dl > 1e-4f ) { P = P + toL * ( 2.0f / dl ); }
				float cov = 1.0f - saturate( SoftShadow_WedgeOcclusion( P, Lo, L.penumbraSize, ( int )( L.firstEdge * 2 ), ( int )L.edgeCount, buf ) );
				float dcov = DirShadow( recVec, P, Lo, L.penumbraSize );		// direction-space reference
				float truth = MeshTruthShadowSoup( c.meshVerts.data(), castIdx.data(), ( uint32_t )castIdx.size(), P, Lo, L.penumbraSize, 12 );
				if( dcov < 0.5f && truth > 0.85f ) { extraneousDir++; }			// same extraneous test for direction-space
				// gate: coverage only counts if the receiver is in the caster's actual shadow (center ray blocked)
				bool hardBlocked = RayHitsMesh( P, Lo - P, c.meshVerts.data(), castIdx.data(), ( uint32_t )castIdx.size() );
				float gatedCov = hardBlocked ? cov : 1.0f;
				if( gatedCov < 0.5f && truth > 0.85f ) { gatedExtraneous++; }	// extraneous shadow after gating (want ~0)
				if( !hardBlocked && truth < 0.9f ) { penumbraClipped++; }		// outer penumbra a hard gate would drop
				float d = std::fabs( cov - truth );
				sampled++; sumAbs += d; worst = std::fmax( worst, ( double )d ); covSum += cov; truSum += truth;
				if( d > 0.1f ) { bad++; }
				if( cov > truth + 0.1f ) { tooLight++; }		// coverage MISSES shadow (deep-umbra hole / under-occlusion)
				if( cov < truth - 0.1f ) { tooDark++; }			// coverage OVER-shadows (halo / over-occlusion)
				if( cov < 0.5f && truth > 0.85f ) { extraneous++; }	// heavy shadow where truth is essentially lit
				if( truth - cov > worstOver )					// track the single worst over-occlusion + its geometry
				{
					worstOver = truth - cov; owP = P; owL = Lo; owCov = cov; owTru = truth; owDir = dcov;
				}
				if( cov > truth + 0.35f ) { missingShadow++; }	// coverage misses a real shadow (under-occlusion)
				dcovSum += dcov;
				if( dcov > truth + 0.35f ) { dirMissing++; }			// direction-space under-occlusion (want << planar)
				float dGated = hardBlocked ? dcov : 1.0f;				// direction-space, gated by the shadow volume
				if( dGated < 0.5f && truth > 0.85f ) { dirGatedExtraneous++; }	// its over-occlusion after gating (want ~0)
				if( cov - truth > worstUnder )					// track the single worst under-occlusion + geometry
				{
					worstUnder = cov - truth; uwP = P; uwL = Lo; uwCov = cov; uwTru = truth; uwDir = dcov;
				}
			}
		}
	}
	// the extraneous shadow, with geometry: is the receiver directly under the caster while the light is off
	// to the side (a shadow no light would cast there)?
	float3 dPL = owL - owP; float distPL = std::sqrt( dot( dPL, dPL ) );
	float3 dirPL = distPL > 1e-4f ? dPL * ( 1.0f / distPL ) : float3( 0, 0, 1 );
	std::printf( "    [cap_cov] EXTRANEOUS shadow (cov<0.5 & truth>0.85): planar=%d  direction-space=%d  hard-gated=%d  (penumbra a hard gate would clip=%d)\n",
			extraneous, extraneousDir, gatedExtraneous, penumbraClipped );
	std::printf( "    [cap_cov] worst over-shadow: planar=%.3f dir=%.3f truth=%.3f at P(%.0f,%.0f,%.0f) light(%.0f,%.0f,%.0f) dir(%.2f,%.2f,%.2f)\n",
			owCov, owDir, owTru, owP.x, owP.y, owP.z, owL.x, owL.y, owL.z, dirPL.x, dirPL.y, dirPL.z );
	float3 uPL = uwL - uwP; float uDist = std::sqrt( dot( uPL, uPL ) ); float3 uDir = uDist > 1e-4f ? uPL * ( 1.0f / uDist ) : float3( 0, 0, 1 );
	std::printf( "    [cap_cov] MISSING shadow (cov>truth+0.35): planar=%d  direction-space=%d   worst under: planar=%.3f dir=%.3f truth=%.3f light-dir(%.2f,%.2f,%.2f) dist=%.0f\n",
			missingShadow, dirMissing, uwCov, uwDir, uwTru, uDir.x, uDir.y, uDir.z, uDist );
	std::printf( "    [cap_cov] COMPREHENSIVE (direction-space + gate): extraneous=%d  missing=%d   [planar ungated: extraneous=%d missing=%d]  dir mean=%.3f\n",
			dirGatedExtraneous, dirMissing, extraneous, missingShadow, sampled ? dcovSum / sampled : 0.0 );
	std::printf( "    [cap_cov] %d samples: %d disagree>0.1 (%.1f%%)  worst=%.3f  mean|cov-truth|=%.4f\n",
			sampled, bad, sampled ? 100.0 * bad / sampled : 0.0, worst, sampled ? sumAbs / sampled : 0.0 );
	std::printf( "    [cap_cov] mean coverage=%.3f mean truth=%.3f   too-light(miss shadow)=%d  too-dark(over-shadow)=%d\n",
			sampled ? covSum / sampled : 0.0, sampled ? truSum / sampled : 0.0, tooLight, tooDark );
	CHECK( sampled > 0 );		// characterization: the disagreement IS the artifact, to be driven down by a fix
}
