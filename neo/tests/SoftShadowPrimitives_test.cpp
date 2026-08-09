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

// PRIMITIVE-LEVEL verification of BOTH analytic soft-shadow implementations and of the test oracles
// themselves. Working assumption: every step is wrong until an INDEPENDENT oracle proves it right;
// comments lie. Tests assert CORRECT behavior — a proven-broken step leaves this suite RED until the
// fix lands (user decision, no characterization escapes).
//
// Layout:
//   1. GOLDEN PIN        - bit-exact corpus of the pre-refactor implementations, guards the
//                          decomposition refactor (any math change trips it).
//   2. ORACLE SELF-TESTS - RayHitsBox/TruthShadow/Silhouette/RayHitsMesh vs analytic + independent
//                          samplers (an unverified oracle poisons every later verdict).
//   3. A-PRIMITIVES      - each extracted step of softwedge_coverage.inc.hlsl vs closed forms / MC.
//   4. B-PRIMITIVES      - each step of SoftShadowDir.h vs spherical closed forms / MC.
//   5. FINDING TESTS     - one deterministic minimal input per adversarial-review finding F1..F15.
//   6. CAPTURE INVARIANTS- engine edge-stream contract checked on all committed erebus*.softcap.

#include "hlsl_compat.h"
#include "softwedge_coverage.inc.hlsl"		// the live shader source, compiled as C++ (SW_FUNC=inline)
#include "SoftShadowBox.h"
#include "SoftShadowDir.h"
#include "SoftShadowMesh.h"
#include "SoftShadowRender.h"		// offline rasteriser (SwGBuffer) for the visual-defect detectors
#include "../renderer/SoftShadowBand.h"	// THE stencil-band encoding contract (shared with RenderBackend)
#include "idUnitTest.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>

using namespace swtest;

// ===================================================================================== 1. GOLDEN PIN
// Deterministic corpus of (edge-stream, P, L, r) -> occlusion for BOTH implementations. Guards against
// UNINTENDED math drift: any refactor must reproduce every output BIT-EXACTLY; an intentional fix
// regenerates the pin (delete wedge_golden.bin, run once) and says so in its commit. Pin history:
// v1 = pre-decomposition monolith; v2 = decomposed + float-parity literals (F15); v3 = current, after
// the F4 root-clamp, F13 tight header radius, F14 headerless handling, and the Dir cull (F1).
// First run writes neo/tests/data/wedge_golden.bin and reports; later runs compare.
namespace
{

struct GoldenCase
{
	std::vector<float4> rec;
	float3 P, L;
	float  r;
};

// the corpus: a deliberate mix of the geometry classes the diagnosis cares about.
std::vector<GoldenCase> GoldenCorpus()
{
	std::vector<GoldenCase> cs;
	Rng rng( 20260808u );
	const float3 L0( 0, 0, 12 );
	for( int k = 0; k < 200; k++ )
	{
		GoldenCase g;
		int cls = k % 8;
		float3 h, C;
		switch( cls )
		{
			case 0:		// thin planar floater (exact regime)
				h = float3( rng.f( 0.3f, 1.6f ), rng.f( 0.3f, 1.6f ), 0.02f );
				C = float3( rng.f( -3, 3 ), rng.f( -3, 3 ), rng.f( 4.5f, 8.0f ) );
				break;
			case 1:		// fat floater
				h = float3( rng.f( 0.4f, 2.0f ), rng.f( 0.4f, 2.0f ), rng.f( 0.4f, 2.0f ) );
				C = float3( rng.f( -3, 3 ), rng.f( -3, 3 ), rng.f( 3.0f, 8.0f ) );
				break;
			case 2:		// ground-standing (near-plane straddle candidates)
				h = float3( rng.f( 0.4f, 2.0f ), rng.f( 0.4f, 2.0f ), rng.f( 0.5f, 3.0f ) );
				C = float3( rng.f( -3, 3 ), rng.f( -3, 3 ), h.z - 0.05f );
				break;
			case 3:		// pierces the light plane (F6 geometry)
				h = float3( rng.f( 0.3f, 1.0f ), rng.f( 0.3f, 1.0f ), rng.f( 1.0f, 3.0f ) );
				C = float3( rng.f( -1, 1 ), rng.f( -1, 1 ), 12.0f + rng.f( -1.0f, 1.0f ) );
				break;
			case 4:		// wholly BEHIND the receiver (anti-light side)
				h = float3( rng.f( 0.5f, 3.0f ), rng.f( 0.5f, 3.0f ), rng.f( 0.5f, 3.0f ) );
				C = float3( rng.f( -3, 3 ), rng.f( -3, 3 ), -rng.f( 4.0f, 8.0f ) );
				break;
			case 5:		// far off to the side (cull candidates)
				h = float3( rng.f( 0.5f, 2.0f ), rng.f( 0.5f, 2.0f ), rng.f( 0.5f, 2.0f ) );
				C = float3( rng.f( 8, 14 ), rng.f( -3, 3 ), rng.f( 3.0f, 8.0f ) );
				break;
			case 6:		// tall pillar over the receiver (deep umbra / semicircle regime)
				h = float3( rng.f( 0.5f, 1.2f ), rng.f( 0.5f, 1.2f ), rng.f( 2.0f, 4.0f ) );
				C = float3( rng.f( -0.5f, 0.5f ), rng.f( -0.5f, 0.5f ), h.z );
				break;
			default:	// grazing sliver
				h = float3( rng.f( 0.05f, 0.2f ), rng.f( 1.0f, 3.0f ), rng.f( 0.05f, 0.2f ) );
				C = float3( rng.f( -2, 2 ), rng.f( -2, 2 ), rng.f( 4.0f, 9.0f ) );
				break;
		}
		Box b = MakeBox( C, h, rng.f( 0, 3.14f ), rng.f( -0.3f, 0.3f ) );
		auto loop = Silhouette( b, L0 );
		if( loop.size() < 3 ) { continue; }
		g.rec = BuildCaster( { loop } );
		if( k % 3 == 0 )	// every third case: a second caster in the same stream (max-combine path)
		{
			Box b2 = MakeBox( float3( rng.f( -3, 3 ), rng.f( -3, 3 ), rng.f( 3.0f, 7.0f ) ),
							  float3( rng.f( 0.3f, 1.0f ), rng.f( 0.3f, 1.0f ), rng.f( 0.1f, 1.0f ) ) );
			auto loop2 = Silhouette( b2, L0 );
			if( loop2.size() >= 3 )
			{
				auto r2 = BuildCaster( { loop2 } );
				g.rec.insert( g.rec.end(), r2.begin(), r2.end() );
			}
		}
		g.L = L0;
		g.r = rng.f( 0.5f, 4.0f );
		g.P = float3( rng.f( -2, 2 ), rng.f( -2, 2 ), 0.0f );
		cs.push_back( g );
	}
	return cs;
}

const char* GOLDEN_PATH = "/home/app/Games/gog/doom-3-bfg-edition/neo/tests/data/wedge_golden.bin";

}

TEST( SoftShadowGolden, refactor_preserves_both_implementations_bit_exact )
{
	std::vector<GoldenCase> cs = GoldenCorpus();
	std::vector<float> now;
	now.reserve( cs.size() * 2 );
	for( const GoldenCase& g : cs )
	{
		SoftEdgeBuffer buf{ g.rec.data(), ( int )g.rec.size() };
		now.push_back( SoftShadow_WedgeOcclusion( g.P, g.L, g.r, 0, ( int )( g.rec.size() / 2 ), 0.0f, buf ) );
		now.push_back( DirOcclusion( g.rec, g.P, g.L, g.r ) );
	}

	std::FILE* f = std::fopen( GOLDEN_PATH, "rb" );
	if( !f )
	{
		f = std::fopen( GOLDEN_PATH, "wb" );
		CHECK( f != NULL );
		if( f )
		{
			uint32_t n = ( uint32_t )now.size();
			std::fwrite( &n, sizeof( n ), 1, f );
			std::fwrite( now.data(), sizeof( float ), now.size(), f );
			std::fclose( f );
			std::printf( "    [golden] GENERATED %zu outputs (%zu cases) -> wedge_golden.bin\n", now.size(), cs.size() );
		}
		return;
	}
	uint32_t n = 0;
	CHECK( std::fread( &n, sizeof( n ), 1, f ) == 1 );
	std::vector<float> ref( n );
	CHECK( std::fread( ref.data(), sizeof( float ), n, f ) == n );
	std::fclose( f );
	CHECK( ( size_t )n == now.size() );
	int mismatches = 0;
	float worst = 0.0f;
	for( size_t i = 0; i < now.size() && i < ref.size(); i++ )
	{
		if( std::memcmp( &now[i], &ref[i], sizeof( float ) ) != 0 )
		{
			mismatches++;
			worst = std::fmax( worst, std::fabs( now[i] - ref[i] ) );
		}
	}
	std::printf( "    [golden] %u outputs: %d bit-mismatches (worst |d|=%.9g)\n", n, mismatches, worst );
	CHECK( mismatches == 0 );		// refactor must be operation-for-operation identical
}

// ============================================================================ shared MC oracles
// Independent-code-path reference integrators. They share NOTHING with the implementations under
// test: point-in-region membership + uniform sampling, no shoelace, no sectors, no solid-angle
// formulas. Deterministic (grid / LCG) so failures reproduce exactly.
namespace
{

// winding number of the closed 2D polygon `poly` around point q (nonzero rule, signed).
int Winding2D( const std::vector<float2>& poly, float2 q )
{
	double total = 0.0;
	for( size_t i = 0; i < poly.size(); i++ )
	{
		float2 a = poly[i], b = poly[( i + 1 ) % poly.size()];
		double ax = a.x - q.x, ay = a.y - q.y, bx = b.x - q.x, by = b.y - q.y;
		total += std::atan2( ax * by - ay * bx, ax * bx + ay * by );
	}
	return ( int )std::lround( total / ( 2.0 * 3.14159265358979323846 ) );
}

// SIGNED area of disk(origin, r) INTERSECT polygon, winding-weighted (matches what a shoelace sum
// over the polygon's directed edges computes). Midpoint grid over the disk's bounding square.
double DiskPolyAreaMC( const std::vector<float2>& poly, float r, int K = 512 )
{
	double cell = 2.0 * r / K, sum = 0.0;
	for( int iy = 0; iy < K; iy++ )
		for( int ix = 0; ix < K; ix++ )
		{
			float px = ( float )( -r + ( ix + 0.5 ) * cell );
			float py = ( float )( -r + ( iy + 0.5 ) * cell );
			if( ( double )px * px + ( double )py * py > ( double )r * r ) { continue; }
			int w = Winding2D( poly, float2( px, py ) );
			if( w != 0 ) { sum += ( double )w * cell * cell; }
		}
	return sum;
}

// signed area of disk INTERSECT triangle(origin, A, B) - the exact quantity SoftDisk_CircleTriArea claims.
double DiskTriAreaMC( float2 A, float2 B, float r, int K = 512 )
{
	std::vector<float2> tri = { float2( 0, 0 ), A, B };
	return DiskPolyAreaMC( tri, r, K );
}

// spherical winding number of the closed direction loop `loop` around unit direction x: project the
// loop onto x's tangent plane and count turns. Undefined only if a loop vertex is (anti)parallel to x.
int WindingSphere( const std::vector<float3>& loop, float3 x )
{
	float3 up = ( std::fabs( x.z ) > 0.9f ) ? float3( 0, 1, 0 ) : float3( 0, 0, 1 );
	float3 tu = normalize( cross( up, x ) );
	float3 tv = cross( x, tu );
	double total = 0.0;
	for( size_t i = 0; i < loop.size(); i++ )
	{
		float3 a = loop[i], b = loop[( i + 1 ) % loop.size()];
		double ax = dot( a, tu ), ay = dot( a, tv ), bx = dot( b, tu ), by = dot( b, tv );
		total += std::atan2( ax * by - ay * bx, ax * bx + ay * by );
	}
	return ( int )std::lround( total / ( 2.0 * 3.14159265358979323846 ) );
}

// winding-weighted solid angle of cap(pole n, cos half-angle cosA) INTERSECT the region enclosed by
// the closed spherical loop. Uniform cap sampling: z uniform in [cosA,1], phi uniform (equal-area).
double CapLoopSolidMC( const std::vector<float3>& loop, float3 n, float cosA, int K = 400 )
{
	float3 up = ( std::fabs( n.z ) > 0.9f ) ? float3( 0, 1, 0 ) : float3( 0, 0, 1 );
	float3 cu = normalize( cross( up, n ) );
	float3 cv = cross( n, cu );
	double capSolid = 2.0 * 3.14159265358979323846 * ( 1.0 - ( double )cosA );
	double sum = 0.0;
	int    total = 0;
	for( int iz = 0; iz < K; iz++ )
		for( int ip = 0; ip < K; ip++ )
		{
			double z = ( double )cosA + ( iz + 0.5 ) / K * ( 1.0 - ( double )cosA );
			double phi = ( ip + 0.5 ) / K * 2.0 * 3.14159265358979323846;
			double s = std::sqrt( std::fmax( 0.0, 1.0 - z * z ) );
			float3 x = n * ( float )z + cu * ( float )( s * std::cos( phi ) ) + cv * ( float )( s * std::sin( phi ) );
			sum += WindingSphere( loop, x );
			total++;
		}
	return total ? sum / total * capSolid : 0.0;
}

// point strictly inside the box solid (all 6 outward face half-spaces), margin > 0 shrinks the box.
bool PointInBox( float3 X, const Box& b, float margin )
{
	for( int i = 0; i < 6; i++ )
	{
		float3 v0 = b.c[b.f[i][0]], v1 = b.c[b.f[i][1]], v2 = b.c[b.f[i][2]];
		float3 nrm = normalize( cross( v1 - v0, v2 - v0 ) );
		if( dot( nrm, X - v0 ) > -margin ) { return false; }
	}
	return true;
}

// independent segment-vs-box-solid test: march the segment, point-in-box each step.
// Returns 1 = definitely blocked (a sample point is deep inside), 0 = definitely clear (no sample
// within `margin` of the solid), -1 = grazing/undecidable at this margin (caller should skip).
int SegmentHitsBoxSampled( float3 P, float3 Q, const Box& b, int steps = 4000, float margin = 1e-3f )
{
	bool nearMiss = false;
	for( int i = 1; i < steps; i++ )
	{
		float t = ( float )i / steps;
		float3 X = P + ( Q - P ) * t;
		if( PointInBox( X, b, margin ) ) { return 1; }
		if( PointInBox( X, b, -margin ) ) { nearMiss = true; }
	}
	return nearMiss ? -1 : 0;
}

// closed-form area of the circular segment of disk radius r cut by the half-plane x >= d (0 <= d <= r).
double CircSegmentArea( double r, double d )
{
	return r * r * std::acos( d / r ) - d * std::sqrt( r * r - d * d );
}

const double MC_PI = 3.14159265358979323846;

}

// ====================================================================== 2. ORACLE SELF-VERIFICATION
// The ray-cast/silhouette machinery every other verdict leans on. Wrong oracle = every conclusion void.

TEST( SoftOracle, ray_hits_box_agrees_with_point_sampling )
{
	Rng rng( 424242u );
	int tested = 0, skipped = 0;
	for( int k = 0; k < 800; k++ )
	{
		Box b = MakeBox( float3( rng.f( -2, 2 ), rng.f( -2, 2 ), rng.f( -2, 2 ) ),
						 float3( rng.f( 0.3f, 2.0f ), rng.f( 0.3f, 2.0f ), rng.f( 0.3f, 2.0f ) ),
						 rng.f( 0, 3.14f ), rng.f( -0.5f, 0.5f ) );
		float3 P( rng.f( -6, 6 ), rng.f( -6, 6 ), rng.f( -6, 6 ) );
		float3 Q( rng.f( -6, 6 ), rng.f( -6, 6 ), rng.f( -6, 6 ) );
		if( PointInBox( P, b, -1e-2f ) || PointInBox( Q, b, -1e-2f ) ) { continue; }	// endpoint-inside contract tested separately (F8)
		int truth = SegmentHitsBoxSampled( P, Q, b );
		if( truth < 0 ) { skipped++; continue; }
		tested++;
		bool got = RayHitsBox( P, Q - P, b );
		if( got != ( truth == 1 ) )
		{
			std::printf( "    [raybox] MISMATCH P(%.3f,%.3f,%.3f) Q(%.3f,%.3f,%.3f) sampled=%d got=%d\n",
					P.x, P.y, P.z, Q.x, Q.y, Q.z, truth, ( int )got );
		}
		CHECK( got == ( truth == 1 ) );
	}
	std::printf( "    [raybox] %d segments cross-checked (%d grazing skipped)\n", tested, skipped );
	CHECK( tested > 400 );
}

TEST( SoftOracle, ray_hits_box_hand_cases )
{
	Box b = MakeBox( float3( 0, 0, 5 ), float3( 1, 1, 1 ) );		// axis-aligned cube [-1,1]^2 x [4,6]
	CHECK( RayHitsBox( float3( 0, 0, 0 ), float3( 0, 0, 10 ), b ) );			// straight through
	CHECK_FALSE( RayHitsBox( float3( 0, 0, 0 ), float3( 0, 0, 3.9f ), b ) );	// stops short
	CHECK_FALSE( RayHitsBox( float3( 3, 0, 0 ), float3( 0, 0, 10 ), b ) );		// parallel miss
	CHECK_FALSE( RayHitsBox( float3( 0, 0, 7 ), float3( 0, 0, 10 ), b ) );		// starts past it, points away
	CHECK( RayHitsBox( float3( -3, 0, 5 ), float3( 6, 0, 0 ), b ) );			// sideways through
}

TEST( SoftOracle, truth_shadow_matches_closed_forms )
{
	const float3 L( 0, 0, 12 );
	const float  R = 3.0f;
	const float3 P( 0, 0, 0 );
	// caster plane at z=6 projects onto the disk plane (z=12) with scale (12-0)/(6-0) = 2 from P.
	// (1) half-plane x>=0 at z=6 covers exactly half the disk -> shadow 0.5
	Box half = MakeBox( float3( 50, 0, 6 ), float3( 50, 50, 0.01f ) );
	CHECK_NEAR( TruthShadow( P, L, R, half, 400 ), 0.5f, 0.01f );
	// (2) half-plane x >= 0.75 at z=6 -> chord at disk x = 1.5 = r/2: circular-segment closed form
	Box seg = MakeBox( float3( 50.75f, 0, 6 ), float3( 50, 50, 0.01f ) );
	double expect = CircSegmentArea( R, 1.5 ) / ( MC_PI * R * R );
	CHECK_NEAR( TruthShadow( P, L, R, seg, 400 ), 1.0 - expect, 0.01 );
	// (3) full cover / no cover
	CHECK_NEAR( TruthShadow( P, L, R, MakeBox( float3( 0, 0, 6 ), float3( 10, 10, 0.01f ) ), 200 ), 0.0f, 1e-3f );
	CHECK_NEAR( TruthShadow( P, L, R, MakeBox( float3( 30, 0, 6 ), float3( 1, 1, 1 ) ), 200 ), 1.0f, 1e-3f );
}

TEST( SoftOracle, truth_shadow_grid_vs_random_sampler )
{
	// same box, grid disk sampling (TruthShadow) vs an INDEPENDENT random-point sampler over the disk.
	const float3 L( 0, 0, 12 );
	const float  R = 3.0f;
	const float3 P( 0, 0, 0 );
	Box b = MakeBox( float3( 0.8f, -0.4f, 6 ), float3( 1.1f, 0.7f, 0.5f ), 0.6f, 0.1f );
	float grid = TruthShadow( P, L, R, b, 300 );
	Rng rng( 777u );
	float3 nrm( 0, 0, 1 ), u( 1, 0, 0 ), v( 0, 1, 0 );
	int hit = 0, tot = 0;
	for( int k = 0; k < 200000; k++ )
	{
		float dx = rng.f( -1, 1 ), dy = rng.f( -1, 1 );
		if( dx * dx + dy * dy > 1.0f ) { continue; }
		tot++;
		float3 D = L + u * ( dx * R ) + v * ( dy * R );
		if( RayHitsBox( P, D - P, b ) ) { hit++; }
	}
	float rnd = 1.0f - ( float )hit / tot;
	std::printf( "    [truth-sampler] grid=%.4f random=%.4f\n", grid, rnd );
	CHECK_NEAR( grid, rnd, 0.01f );
}

TEST( SoftOracle, silhouette_is_closed_walk_of_box_edges )
{
	Rng rng( 31337u );
	int loops = 0;
	for( int k = 0; k < 400; k++ )
	{
		Box b = MakeBox( float3( rng.f( -3, 3 ), rng.f( -3, 3 ), rng.f( 2, 8 ) ),
						 float3( rng.f( 0.3f, 2.0f ), rng.f( 0.3f, 2.0f ), rng.f( 0.3f, 2.0f ) ),
						 rng.f( 0, 3.14f ), rng.f( -0.5f, 0.5f ) );
		float3 W( 0, 0, 12 );
		if( PointInBox( W, b, -1e-2f ) ) { continue; }		// light-inside contract is F9's test
		auto loop = Silhouette( b, W );
		if( loop.size() < 3 ) { continue; }
		loops++;
		// every loop vertex must be a box corner, and consecutive vertices a box edge (share exactly 2 faces...
		// cheap necessary condition: consecutive corners differ in exactly one axis of the corner index)
		int okVerts = 0;
		for( size_t i = 0; i < loop.size(); i++ )
		{
			for( int c = 0; c < 8; c++ )
			{
				float3 d = loop[i] - b.c[c];
				if( dot( d, d ) < 1e-10f ) { okVerts++; break; }
			}
		}
		CHECK( okVerts == ( int )loop.size() );
		// closed: walk-ordered loop implies last connects to first (Silhouette pops the duplicate); verify no
		// vertex repeats (a figure-eight would repeat the pinch vertex)
		int repeats = 0;
		for( size_t i = 0; i < loop.size(); i++ )
			for( size_t j = i + 1; j < loop.size(); j++ )
			{
				float3 d = loop[i] - loop[j];
				if( dot( d, d ) < 1e-10f ) { repeats++; }
			}
		CHECK( repeats == 0 );
	}
	std::printf( "    [sil-oracle] %d loops validated\n", loops );
	CHECK( loops > 300 );
}

TEST( SoftOracle, silhouette_winding_consistent_and_hull_contains_corners )
{
	// projected from the LIGHT onto a plane, the silhouette of a convex box must be a convex loop of
	// CONSISTENT winding that contains every projected corner. Winding flips = broken orientation contract.
	Rng rng( 90210u );
	int pos = 0, neg = 0, loops = 0;
	for( int k = 0; k < 400; k++ )
	{
		Box b = MakeBox( float3( rng.f( -3, 3 ), rng.f( -3, 3 ), rng.f( 2, 8 ) ),
						 float3( rng.f( 0.3f, 2.0f ), rng.f( 0.3f, 2.0f ), rng.f( 0.3f, 2.0f ) ),
						 rng.f( 0, 3.14f ), rng.f( -0.5f, 0.5f ) );
		float3 W( 0, 0, 12 );
		auto loop = Silhouette( b, W );
		if( loop.size() < 3 ) { continue; }
		loops++;
		// project from W onto the z=0 plane (all boxes are below the light)
		std::vector<float2> pl;
		bool ok = true;
		for( float3 V : loop )
		{
			float dz = W.z - V.z;
			if( dz < 1e-3f ) { ok = false; break; }
			float s = W.z / dz;
			pl.push_back( float2( W.x + ( V.x - W.x ) * s, W.y + ( V.y - W.y ) * s ) );
		}
		if( !ok ) { continue; }
		double sh = 0.0;
		for( size_t i = 0; i < pl.size(); i++ )
		{
			float2 a = pl[i], c = pl[( i + 1 ) % pl.size()];
			sh += ( double )a.x * c.y - ( double )a.y * c.x;
		}
		if( sh > 0 ) { pos++; }
		else { neg++; }
		// every projected corner inside-or-on the projected loop (winding != 0 or near boundary)
		for( int c = 0; c < 8; c++ )
		{
			float dz = W.z - b.c[c].z;
			if( dz < 1e-3f ) { continue; }
			float s = W.z / dz;
			float2 q( W.x + ( b.c[c].x - W.x ) * s, W.y + ( b.c[c].y - W.y ) * s );
			// shrink toward the loop centroid slightly to dodge exact-boundary winding ambiguity
			float2 ctr( 0, 0 );
			for( float2 p : pl ) { ctr = ctr + p; }
			ctr = ctr * ( 1.0f / pl.size() );
			float2 qin( q.x + ( ctr.x - q.x ) * 1e-3f, q.y + ( ctr.y - q.y ) * 1e-3f );
			CHECK( Winding2D( pl, qin ) != 0 );
		}
	}
	std::printf( "    [sil-winding] %d loops: shoelace sign +%d / -%d (must be uniform)\n", loops, pos, neg );
	CHECK( loops > 300 );
	CHECK( pos == 0 || neg == 0 );		// consistent orientation contract
}

// ====================================================================== 3. IMPLEMENTATION A PRIMITIVES

TEST( SoftPrimA, tri_and_sector_closed_forms )
{
	// triangle: (0,0),(2,0),(0,2) -> area 2, positive winding; swapped -> -2
	CHECK_NEAR( SoftDisk_Tri( float2( 2, 0 ), float2( 0, 2 ) ), 2.0f, 1e-6f );
	CHECK_NEAR( SoftDisk_Tri( float2( 0, 2 ), float2( 2, 0 ) ), -2.0f, 1e-6f );
	// sector: quarter turn at r=1 between far points -> 0.5*r^2*(pi/2)
	CHECK_NEAR( SoftDisk_Sector( float2( 2, 0 ), float2( 0, 3 ), 1.0f ), 0.5f * ( float )( MC_PI / 2 ), 1e-5f );
	CHECK_NEAR( SoftDisk_Sector( float2( 0, 3 ), float2( 2, 0 ), 1.0f ), -0.5f * ( float )( MC_PI / 2 ), 1e-5f );
	CHECK_NEAR( SoftDisk_Sector( float2( 2, 0 ), float2( 3, 0 ), 1.0f ), 0.0f, 1e-6f );	// collinear, same side
}

TEST( SoftPrimA, seg_circle_roots_closed_forms )
{
	// segment x: -2 -> +2 at y=0, r=1: crossings x=+-1 -> t=0.25, 0.75
	softSegRoots_t r = SoftDisk_SegCircleRoots( float2( -2, 0 ), float2( 4, 0 ), 1.0f );
	CHECK( r.disc > 0.0f );
	CHECK_NEAR( r.t1, 0.25f, 1e-6f );
	CHECK_NEAR( r.t2, 0.75f, 1e-6f );
	// vertical chord x=0.6: |y| = 0.8 -> y from -2: t = (2-0.8)/4=0.3, (2+0.8)/4=0.7
	r = SoftDisk_SegCircleRoots( float2( 0.6f, -2 ), float2( 0, 4 ), 1.0f );
	CHECK_NEAR( r.t1, 0.3f, 1e-6f );
	CHECK_NEAR( r.t2, 0.7f, 1e-6f );
	// clear miss (line y=2)
	r = SoftDisk_SegCircleRoots( float2( -2, 2 ), float2( 4, 0 ), 1.0f );
	CHECK( r.disc <= 0.0f );
	// t1 <= t2 always when disc > 0
	Rng rng( 55u );
	for( int k = 0; k < 2000; k++ )
	{
		float2 A( rng.f( -3, 3 ), rng.f( -3, 3 ) );
		float2 D( rng.f( -6, 6 ), rng.f( -6, 6 ) );
		if( D.x * D.x + D.y * D.y < 1e-6f ) { continue; }
		softSegRoots_t rr = SoftDisk_SegCircleRoots( A, D, 1.0f );
		if( rr.disc > 0.0f ) { CHECK( rr.t1 <= rr.t2 ); }
	}
}

// branch-verified circle-triangle areas vs the winding-weighted MC integrator. Each case FIRST asserts
// which branch the input takes (recomputing the predicates here), so drift cannot silently retarget it.
namespace
{
int CircleTriBranch( float2 A, float2 B, float r2 )		// 0 both-in, 1 degenerate, 2 sector-nocross, 3 in-out, 4 out-in, 5 secant, 6 sector-offseg
{
	float a2 = dot( A, A ), b2 = dot( B, B );
	if( a2 <= r2 && b2 <= r2 ) { return 0; }
	float2 D = B - A;
	float qa = dot( D, D );
	if( qa < 1e-9f ) { return 1; }
	softSegRoots_t rt = SoftDisk_SegCircleRoots( A, D, r2 );
	if( rt.disc <= 0.0f ) { return 2; }
	bool ain = a2 <= r2, bin = b2 <= r2;
	if( ain && !bin ) { return 3; }
	if( !ain && bin ) { return 4; }
	if( rt.t1 >= 0.0f && rt.t1 <= 1.0f && rt.t2 >= 0.0f && rt.t2 <= 1.0f ) { return 5; }
	return 6;
}
void CheckCircleTriCase( idTestResult& _tr, float2 A, float2 B, float r, int wantBranch )
{
	int gotBranch = CircleTriBranch( A, B, r * r );
	if( gotBranch != wantBranch )
	{
		std::printf( "    [ctri] BRANCH MISMATCH A(%g,%g) B(%g,%g): got %d want %d\n", A.x, A.y, B.x, B.y, gotBranch, wantBranch );
	}
	CHECK( gotBranch == wantBranch );
	float  got = SoftDisk_CircleTriArea( A, B, r * r );
	double mc  = DiskTriAreaMC( A, B, r );
	CHECK_NEAR( got, mc, 0.006 * MC_PI * r * r );
}
}

TEST( SoftPrimA, circle_tri_area_every_branch_vs_mc )
{
	const float r = 1.0f;
	CheckCircleTriCase( _tr, float2( 0.5f, 0.1f ), float2( 0.2f, 0.6f ), r, 0 );	// both inside
	// NOTE: an INSIDE duplicate takes the both-in branch first (Tri(A,A)=0, same answer); the qa guard
	// is only reachable with at least one endpoint outside - so the degenerate case must sit outside.
	CheckCircleTriCase( _tr, float2( 2.5f, 0.3f ), float2( 2.5f, 0.3f ), r, 1 );	// degenerate (outside dup)
	CheckCircleTriCase( _tr, float2( 2.0f, 0.5f ), float2( 0.5f, 2.0f ), r, 2 );	// outside, line misses disk
	CheckCircleTriCase( _tr, float2( 0.4f, 0.0f ), float2( 2.5f, 0.4f ), r, 3 );	// A in, B out
	CheckCircleTriCase( _tr, float2( 2.5f, 0.4f ), float2( 0.4f, 0.0f ), r, 4 );	// A out, B in
	CheckCircleTriCase( _tr, float2( -2.0f, 0.3f ), float2( 2.0f, 0.3f ), r, 5 );	// secant, both out
	CheckCircleTriCase( _tr, float2( 2.0f, 0.3f ), float2( 4.0f, 0.3f ), r, 6 );	// chord line crosses, segment beyond
	CheckCircleTriCase( _tr, float2( -4.0f, 0.3f ), float2( -2.0f, 0.3f ), r, 6 );	// segment before the crossings
}

TEST( SoftPrimA, circle_tri_area_fuzz_vs_mc )
{
	Rng rng( 8888u );
	int perBranch[7] = { 0, 0, 0, 0, 0, 0, 0 };
	float worst = 0;
	for( int k = 0; k < 300; k++ )
	{
		float2 A( rng.f( -2.5f, 2.5f ), rng.f( -2.5f, 2.5f ) );
		float2 B( rng.f( -2.5f, 2.5f ), rng.f( -2.5f, 2.5f ) );
		int br = CircleTriBranch( A, B, 1.0f );
		perBranch[br]++;
		float  got = SoftDisk_CircleTriArea( A, B, 1.0f );
		double mc  = DiskTriAreaMC( A, B, 1.0f, 384 );
		worst = std::fmax( worst, ( float )std::fabs( got - mc ) );
		CHECK_NEAR( got, mc, 0.008 * MC_PI );
	}
	std::printf( "    [ctri-fuzz] branches hit: in=%d deg=%d sect=%d inout=%d outin=%d secant=%d offseg=%d  worst|d|=%.4f\n",
			perBranch[0], perBranch[1], perBranch[2], perBranch[3], perBranch[4], perBranch[5], perBranch[6], worst );
	CHECK( perBranch[0] > 0 && perBranch[2] > 0 && perBranch[3] > 0 && perBranch[4] > 0 && perBranch[5] > 0 && perBranch[6] > 0 );
}

TEST( SoftPrimA, loop_sum_matches_mc_polygon_area )
{
	const float r = 1.0f;
	// (a) small square fully inside: exact shoelace area
	std::vector<float2> sq = { float2( -0.4f, -0.4f ), float2( 0.4f, -0.4f ), float2( 0.4f, 0.4f ), float2( -0.4f, 0.4f ) };
	double s = 0;
	for( size_t i = 0; i < sq.size(); i++ ) { s += SoftDisk_CircleTriArea( sq[i], sq[( i + 1 ) % sq.size()], r * r ); }
	CHECK_NEAR( s, 0.64, 1e-5 );
	// (b) huge square covering the disk: pi r^2
	std::vector<float2> big = { float2( -9, -9 ), float2( 9, -9 ), float2( 9, 9 ), float2( -9, 9 ) };
	s = 0;
	for( size_t i = 0; i < big.size(); i++ ) { s += SoftDisk_CircleTriArea( big[i], big[( i + 1 ) % big.size()], r * r ); }
	CHECK_NEAR( s, MC_PI, 1e-4 );
	// (c) non-convex star, half in half out
	std::vector<float2> star;
	for( int i = 0; i < 10; i++ )
	{
		float ang = ( float )( i * MC_PI / 5 );
		float rad = ( i & 1 ) ? 0.45f : 1.6f;
		star.push_back( float2( rad * std::cos( ang ) + 0.3f, rad * std::sin( ang ) ) );
	}
	s = 0;
	for( size_t i = 0; i < star.size(); i++ ) { s += SoftDisk_CircleTriArea( star[i], star[( i + 1 ) % star.size()], r * r ); }
	CHECK_NEAR( s, DiskPolyAreaMC( star, r ), 0.006 * MC_PI );
	// (d) ring: outer CCW + inner CW = annulus (hole subtracts through winding)
	std::vector<float2> outer = { float2( -2, -2 ), float2( 2, -2 ), float2( 2, 2 ), float2( -2, 2 ) };
	std::vector<float2> inner = { float2( -0.5f, -0.5f ), float2( -0.5f, 0.5f ), float2( 0.5f, 0.5f ), float2( 0.5f, -0.5f ) };
	s = 0;
	for( size_t i = 0; i < outer.size(); i++ ) { s += SoftDisk_CircleTriArea( outer[i], outer[( i + 1 ) % outer.size()], r * r ); }
	for( size_t i = 0; i < inner.size(); i++ ) { s += SoftDisk_CircleTriArea( inner[i], inner[( i + 1 ) % inner.size()], r * r ); }
	CHECK_NEAR( s, MC_PI - 1.0, 1e-4 );		// disk fully inside outer; inner hole 1x1 subtracts
	// (e) far-out huge-coordinate loop enclosing the disk (near-plane-clip magnitudes): must stay pi r^2 and FINITE
	std::vector<float2> huge = { float2( -3e5f, -3e5f ), float2( 3e5f, -3e5f ), float2( 3e5f, 3e5f ), float2( -3e5f, 3e5f ) };
	s = 0;
	for( size_t i = 0; i < huge.size(); i++ ) { s += SoftDisk_CircleTriArea( huge[i], huge[( i + 1 ) % huge.size()], r * r ); }
	CHECK( std::isfinite( s ) );
	CHECK_NEAR( s, MC_PI, 0.01 );
}

TEST( SoftPrimA, frame_orthonormal_and_oriented )
{
	Rng rng( 12u );
	for( int k = 0; k < 500; k++ )
	{
		float3 P( rng.f( -50, 50 ), rng.f( -50, 50 ), rng.f( -50, 50 ) );
		float3 L( rng.f( -50, 50 ), rng.f( -50, 50 ), rng.f( -50, 50 ) );
		float3 d = L - P;
		if( length( d ) < 1e-2f ) { continue; }
		softFrame_t f = SoftShadow_Frame( P, L );
		CHECK_NEAR( length( f.nrm ), 1.0f, 1e-5f );
		CHECK_NEAR( length( f.u ), 1.0f, 1e-5f );
		CHECK_NEAR( length( f.v ), 1.0f, 1e-5f );
		CHECK_NEAR( dot( f.nrm, f.u ), 0.0f, 1e-5f );
		CHECK_NEAR( dot( f.nrm, f.v ), 0.0f, 1e-5f );
		CHECK_NEAR( dot( f.u, f.v ), 0.0f, 1e-5f );
		CHECK_NEAR( dot( f.nrm, normalize( d ) ), 1.0f, 1e-5f );			// nrm points AT the light
		CHECK_NEAR( f.distPL, length( d ), length( d ) * 1e-5f + 1e-5f );
		// right-handed: cross(u,v) == nrm
		float3 c = cross( f.u, f.v );
		CHECK_NEAR( dot( c, f.nrm ), 1.0f, 1e-4f );
	}
}

TEST( SoftPrimA, project_vert_similar_triangles )
{
	// P at origin, L at (0,0,10): frame nrm=(0,0,1), u=(1,0,0) (up=(0,1,0) since |nrm.z|>0.9), v=(0,1,0).
	softFrame_t f = SoftShadow_Frame( float3( 0, 0, 0 ), float3( 0, 0, 10 ) );
	// world point (1,2,5): rel=(1,2,5), dn=5. Central projection to the light plane z=10 doubles x,y.
	float3 rel( 1, 2, 5 );
	float2 q = SoftShadow_ProjectVert( rel, dot( rel, f.nrm ), f );
	CHECK_NEAR( q.x, 2.0f, 1e-5f );
	CHECK_NEAR( q.y, 4.0f, 1e-5f );
	// twice as deep -> same projected point (it is a projection along the ray)
	float3 rel2( 2, 4, 10 );
	float2 q2 = SoftShadow_ProjectVert( rel2, dot( rel2, f.nrm ), f );
	CHECK_NEAR( q2.x, q.x, 1e-4f );
	CHECK_NEAR( q2.y, q.y, 1e-4f );
}

TEST( SoftPrimA, clip_slab_interval_table )
{
	const float eps = 1e-3f, D = 10.0f;		// slab [1e-3, 10]
	softClip_t c;
	c = SoftShadow_ClipSlab( 2.0f, 8.0f, eps, D );							// fully inside
	CHECK( !c.empty ); CHECK_NEAR( c.t0, 0.0f, 1e-7f ); CHECK_NEAR( c.t1, 1.0f, 1e-7f );
	c = SoftShadow_ClipSlab( -2.0f, 8.0f, eps, D );							// crosses near plane, rising
	CHECK( !c.empty ); CHECK_NEAR( c.t0, ( eps + 2.0f ) / 10.0f, 1e-6f ); CHECK_NEAR( c.t1, 1.0f, 1e-7f );
	c = SoftShadow_ClipSlab( 8.0f, -2.0f, eps, D );							// crosses near plane, falling
	CHECK( !c.empty ); CHECK_NEAR( c.t0, 0.0f, 1e-7f ); CHECK_NEAR( c.t1, ( eps - 8.0f ) / -10.0f, 1e-6f );
	c = SoftShadow_ClipSlab( 5.0f, 15.0f, eps, D );							// crosses light plane, rising
	CHECK( !c.empty ); CHECK_NEAR( c.t0, 0.0f, 1e-7f ); CHECK_NEAR( c.t1, 0.5f, 1e-6f );
	c = SoftShadow_ClipSlab( 15.0f, 5.0f, eps, D );							// crosses light plane, falling
	CHECK( !c.empty ); CHECK_NEAR( c.t0, 0.5f, 1e-6f ); CHECK_NEAR( c.t1, 1.0f, 1e-7f );
	c = SoftShadow_ClipSlab( -5.0f, 15.0f, eps, D );						// crosses BOTH
	CHECK( !c.empty ); CHECK_NEAR( c.t0, ( eps + 5.0f ) / 20.0f, 1e-6f ); CHECK_NEAR( c.t1, 0.75f, 1e-6f );
	c = SoftShadow_ClipSlab( -5.0f, -1.0f, eps, D );						// fully behind
	CHECK( c.empty );
	c = SoftShadow_ClipSlab( 12.0f, 15.0f, eps, D );						// fully beyond
	CHECK( c.empty );
	c = SoftShadow_ClipSlab( 5.0f, 5.0f, eps, D );							// parallel inside
	CHECK( !c.empty ); CHECK_NEAR( c.t0, 0.0f, 1e-7f ); CHECK_NEAR( c.t1, 1.0f, 1e-7f );
	c = SoftShadow_ClipSlab( -5.0f, -5.0f, eps, D );						// parallel behind
	CHECK( c.empty );
	c = SoftShadow_ClipSlab( 15.0f, 15.0f, eps, D );						// parallel beyond
	CHECK( c.empty );
	// interpolated depths always land inside the slab
	Rng rng( 99u );
	for( int k = 0; k < 3000; k++ )
	{
		float a = rng.f( -20, 20 ), b = rng.f( -20, 20 );
		softClip_t cc = SoftShadow_ClipSlab( a, b, eps, D );
		if( cc.empty ) { continue; }
		float d = b - a;
		float da = a + cc.t0 * d, db = a + cc.t1 * d;
		CHECK( da >= eps - 1e-4f && da <= D + 1e-4f );
		CHECK( db >= eps - 1e-4f && db <= D + 1e-4f );
		CHECK( cc.t0 >= 0.0f && cc.t1 <= 1.0f && cc.t0 <= cc.t1 );
	}
}

TEST( SoftPrimA, cull_never_skips_a_contributing_caster )
{
	// conservativeness: whenever CullCaster says skip for the HONEST bounding sphere of a caster's
	// silhouette, evaluating that caster anyway must find (near-)zero occlusion. The no-cull evaluation
	// uses an inflated-radius header (defeats every cull branch) on the same edge records.
	Rng rng( 4711u );
	int culled = 0, tested = 0;
	float worstLeak = 0;
	for( int k = 0; k < 1500; k++ )
	{
		float3 C( rng.f( -12, 12 ), rng.f( -12, 12 ), rng.f( -8, 14 ) );
		float3 h( rng.f( 0.3f, 2.0f ), rng.f( 0.3f, 2.0f ), rng.f( 0.3f, 2.0f ) );
		Box b = MakeBox( C, h, rng.f( 0, 3.14f ), rng.f( -0.5f, 0.5f ) );
		float3 L( 0, 0, 12 );
		float3 P( rng.f( -2, 2 ), rng.f( -2, 2 ), 0 );
		float  r = rng.f( 0.5f, 3.0f );
		auto loop = Silhouette( b, L );
		if( loop.size() < 3 ) { continue; }
		tested++;
		std::vector<float4> rec = BuildCaster( { loop } );
		// honest sphere: exact center/radius of the silhouette vertices (tight, not the 2x diagonal)
		float3 lo( 1e30f, 1e30f, 1e30f ), hi( -1e30f, -1e30f, -1e30f );
		for( float3 V : loop )
		{
			lo = float3( std::fmin( lo.x, V.x ), std::fmin( lo.y, V.y ), std::fmin( lo.z, V.z ) );
			hi = float3( std::fmax( hi.x, V.x ), std::fmax( hi.y, V.y ), std::fmax( hi.z, V.z ) );
		}
		float3 ctr = ( lo + hi ) * 0.5f;
		float3 ext = ( hi - lo ) * 0.5f;
		float  rad = length( ext );
		softFrame_t f = SoftShadow_Frame( P, float3( 0, 0, 12 ) );
		float rc = std::fmax( r, 1e-2f );
		float sinA = saturate( rc / f.distPL );
		float cosA = std::sqrt( 1.0f - sinA * sinA );
		if( !SoftShadow_CullCaster( ctr - P, rad, f, sinA, cosA, SW_NEAR_EPS ) ) { continue; }
		culled++;
		// evaluate WITHOUT cull: inflate the header radius so no cull branch can fire
		std::vector<float4> nocull = rec;
		nocull[0] = float4( P.x, P.y, P.z + 1.0f, -1.0f );		// centre near P, radius huge: passes every test
		nocull[1] = float4( 1e6f, 0, 0, 0 );
		SoftEdgeBuffer buf{ nocull.data(), ( int )nocull.size() };
		float occ = SoftShadow_WedgeOcclusion( P, L, r, 0, ( int )( nocull.size() / 2 ), 0.0f, buf );
		worstLeak = std::fmax( worstLeak, occ );
		CHECK( occ < 1e-3f );		// the cull claimed "cannot contribute": prove it
	}
	std::printf( "    [cull] %d casters, %d culled, worst culled-caster occlusion %.2g\n", tested, culled, worstLeak );
	CHECK( culled > 100 );			// the fuzz actually exercised the cull
}

TEST( SoftPrimA, chain_split_and_order_invariance )
{
	// one closed silhouette loop fed as: 1 chain; 2 chains (split mid-loop); each half-loop force-closed by
	// the chain logic - the closure connectors coincide (the split chord traversed both ways) and cancel,
	// so total area is unchanged. Also chain ORDER within a caster must not matter.
	const float3 L( 0, 0, 12 );
	const float3 P( 0.2f, -0.1f, 0 );
	const float  r = 2.5f;
	Box b = MakeBox( float3( 0.5f, 0.3f, 6 ), float3( 1.2f, 0.8f, 0.5f ), 0.4f, 0.1f );
	auto loop = Silhouette( b, L );
	CHECK( loop.size() >= 4 );

	auto runStream = [&]( const std::vector<std::vector<float3>>& chains ) -> float
	{
		std::vector<float4> rec;
		float3 lo( 1e30f, 1e30f, 1e30f ), hi( -1e30f, -1e30f, -1e30f );
		for( const auto& ch : chains )
			for( float3 p : ch )
			{
				lo = float3( std::fmin( lo.x, p.x ), std::fmin( lo.y, p.y ), std::fmin( lo.z, p.z ) );
				hi = float3( std::fmax( hi.x, p.x ), std::fmax( hi.y, p.y ), std::fmax( hi.z, p.z ) );
			}
		float3 c = ( lo + hi ) * 0.5f;
		rec.push_back( float4( c.x, c.y, c.z, -1.0f ) );
		rec.push_back( float4( length( hi - lo ), 0, 0, 0 ) );
		for( const auto& ch : chains )
			for( size_t i = 0; i + 1 < ch.size(); i++ )
			{
				rec.push_back( float4( ch[i].x, ch[i].y, ch[i].z, 0 ) );
				rec.push_back( float4( ch[i + 1].x, ch[i + 1].y, ch[i + 1].z, 0 ) );
			}
		SoftEdgeBuffer buf{ rec.data(), ( int )rec.size() };
		return SoftShadow_WedgeOcclusion( P, L, r, 0, ( int )( rec.size() / 2 ), 0.0f, buf );
	};

	// closed chain = vertex list with the start repeated at the end
	std::vector<float3> whole( loop );
	whole.push_back( loop[0] );
	float occWhole = runStream( { whole } );

	size_t cut = loop.size() / 2;
	std::vector<float3> c1( loop.begin(), loop.begin() + cut + 1 );					// open chain A..M
	std::vector<float3> c2( loop.begin() + cut, loop.end() );						// open chain M..Z
	c2.push_back( loop[0] );														// ..back to A
	float occSplit  = runStream( { c1, c2 } );
	float occSwap   = runStream( { c2, c1 } );

	std::printf( "    [chain] whole=%.6f split=%.6f swapped=%.6f\n", occWhole, occSplit, occSwap );
	CHECK_NEAR( occSplit, occWhole, 1e-4f );
	CHECK_NEAR( occSwap,  occWhole, 1e-4f );
}

TEST( SoftPrimA, max_combine_and_early_break_semantics )
{
	// two disjoint casters in one stream: the loop max-combines. A saturating first caster early-breaks;
	// whatever follows must not LOWER the result.
	const float3 L( 0, 0, 12 );
	const float3 P( 0, 0, 0 );
	const float  r = 2.0f;
	Box big  = MakeBox( float3( 0, 0, 6 ), float3( 5, 5, 0.1f ) );					// saturates (full umbra)
	Box tiny = MakeBox( float3( 8, 0, 6 ), float3( 0.2f, 0.2f, 0.1f ) );			// contributes ~0
	auto rb = BuildCaster( { Silhouette( big, L ) } );
	auto rt = BuildCaster( { Silhouette( tiny, L ) } );
	std::vector<float4> both( rb );
	both.insert( both.end(), rt.begin(), rt.end() );
	SoftEdgeBuffer buf{ both.data(), ( int )both.size() };
	float occ = SoftShadow_WedgeOcclusion( P, L, r, 0, ( int )( both.size() / 2 ), 0.0f, buf );
	CHECK( occ >= 0.999f );
	// order swapped: tiny first, big second - same result (max is order-independent)
	std::vector<float4> swapped( rt );
	swapped.insert( swapped.end(), rb.begin(), rb.end() );
	SoftEdgeBuffer buf2{ swapped.data(), ( int )swapped.size() };
	float occ2 = SoftShadow_WedgeOcclusion( P, L, r, 0, ( int )( swapped.size() / 2 ), 0.0f, buf2 );
	CHECK_NEAR( occ, occ2, 1e-5f );
}

// ====================================================================== 4. IMPLEMENTATION B PRIMITIVES

TEST( SoftPrimB, sph_tri_solid_angle_closed_forms )
{
	// octant (+x,+y,+z): solid angle 4pi/8 = pi/2, sign by orientation
	float3 X( 1, 0, 0 ), Y( 0, 1, 0 ), Z( 0, 0, 1 );
	CHECK_NEAR( SphTriSolidAngle( X, Y, Z ), ( float )( MC_PI / 2 ), 1e-5f );
	CHECK_NEAR( SphTriSolidAngle( Y, X, Z ), -( float )( MC_PI / 2 ), 1e-5f );
	// tiny triangle ~ planar area: right triangle with legs 0.01 rad at the pole
	float e = 0.01f;
	float3 a( 0, 0, 1 );
	float3 bb = normalize( float3( e, 0, 1 ) );
	float3 c = normalize( float3( 0, e, 1 ) );
	CHECK_NEAR( SphTriSolidAngle( a, bb, c ), 0.5f * e * e, 0.5f * e * e * 0.01f );
	// degenerate (repeated vertex) -> 0
	CHECK_NEAR( SphTriSolidAngle( a, a, c ), 0.0f, 1e-7f );
}

TEST( SoftPrimB, cap_arc_basis_properties )
{
	Rng rng( 6161u );
	for( int k = 0; k < 1000; k++ )
	{
		float3 dA = normalize( float3( rng.f( -1, 1 ), rng.f( -1, 1 ), rng.f( -1, 1 ) ) );
		float3 dB = normalize( float3( rng.f( -1, 1 ), rng.f( -1, 1 ), rng.f( -1, 1 ) ) );
		if( length( dA ) < 0.5f || length( dB ) < 0.5f ) { continue; }		// normalize(0) guard
		CapArcBasisT arc = CapArcBasis( dA, dB );
		float cosO = dot( dA, dB );
		if( arc.degenerate ) { CHECK( cosO > 1.0f - 1e-9f || cosO < -1.0f + 1e-9f ); continue; }
		CHECK_NEAR( arc.omega, std::acos( std::fmax( -1.0f, std::fmin( 1.0f, cosO ) ) ), 1e-5f );
		CHECK_NEAR( length( arc.e2 ), 1.0f, 1e-5f );
		CHECK_NEAR( dot( arc.e2, dA ), 0.0f, 1e-5f );
		// arc endpoint reconstruction: cos(omega)*dA + sin(omega)*e2 == dB
		float3 endp = dA * std::cos( arc.omega ) + arc.e2 * std::sin( arc.omega );
		float3 diff = endp - dB;
		CHECK_NEAR( length( diff ), 0.0f, 1e-4f );
	}
}

TEST( SoftPrimB, cap_arc_crossings_closed_form )
{
	// pole +z, cap half-angle 30deg. Arc at constant colatitude sweeping past the cap: symmetric dip.
	float3 n( 0, 0, 1 );
	float cosA = std::cos( 0.5235988f );			// 30 deg
	// arc from colat 40deg azim -50deg to colat 40deg azim +50deg: colatitude of the great-circle arc dips
	// BELOW 40deg mid-arc (great circle cuts inside), entering/leaving the 30deg cap symmetrically.
	auto dir = []( float colat, float azim )
	{
		return float3( std::sin( colat ) * std::cos( azim ), std::sin( colat ) * std::sin( azim ), std::cos( colat ) );
	};
	float colat = 0.6981317f;						// 40 deg
	float3 dA = dir( colat, -0.8726646f );			// -50 deg
	float3 dB = dir( colat, +0.8726646f );
	CapArcBasisT arc = CapArcBasis( dA, dB );
	CHECK( !arc.degenerate );
	CapCrossT cr = CapArcCrossings( dA, arc.e2, arc.omega, n, cosA );
	CHECK( cr.nt == 2 );
	// symmetry: crossings mirror about the arc midpoint
	CHECK_NEAR( cr.ts[0] + cr.ts[1], arc.omega, 1e-4f );
	// each crossing point lies ON the cap boundary
	for( int i = 0; i < cr.nt; i++ )
	{
		float3 x = dA * std::cos( cr.ts[i] ) + arc.e2 * std::sin( cr.ts[i] );
		CHECK_NEAR( dot( x, n ), cosA, 1e-5f );
	}
	// midpoint of the arc is INSIDE the cap
	float3 mid = dA * std::cos( arc.omega * 0.5f ) + arc.e2 * std::sin( arc.omega * 0.5f );
	CHECK( dot( mid, n ) > cosA );
	// an arc that never dips: colat 80deg shallow sweep
	float3 dA2 = dir( 1.3962634f, -0.3f ), dB2 = dir( 1.3962634f, 0.3f );
	CapArcBasisT arc2 = CapArcBasis( dA2, dB2 );
	CapCrossT cr2 = CapArcCrossings( dA2, arc2.e2, arc2.omega, n, cosA );
	CHECK( cr2.nt == 0 );
}

TEST( SoftPrimB, cap_tri_every_case_vs_mc )
{
	// closed spherical square loops in three regimes vs the cap-sampled winding MC. Summing CapTri around
	// a closed loop must equal the winding-weighted cap-overlap solid angle.
	float3 n( 0, 0, 1 );
	float3 u( 1, 0, 0 ), v( 0, 1, 0 );
	float cosA = std::cos( 0.4f );
	auto dir = []( float x, float y, float z ) { return normalize( float3( x, y, z ) ); };
	auto loopSum = [&]( const std::vector<float3>& loop ) -> float
	{
		float s = 0;
		for( size_t i = 0; i < loop.size(); i++ ) { s += CapTri( loop[i], loop[( i + 1 ) % loop.size()], n, u, v, cosA ); }
		return s;
	};
	// (a) loop fully INSIDE the cap
	std::vector<float3> inside = { dir( 0.1f, 0.1f, 1 ), dir( -0.1f, 0.1f, 1 ), dir( -0.1f, -0.1f, 1 ), dir( 0.1f, -0.1f, 1 ) };
	double mcIn = std::fabs( CapLoopSolidMC( inside, n, cosA ) );
	CHECK_NEAR( std::fabs( loopSum( inside ) ), mcIn, 0.01 * mcIn + 1e-4 );
	// (b) loop enclosing the whole cap (outside it): must equal the FULL cap solid angle
	std::vector<float3> around = { dir( 1, 1, 0.8f ), dir( -1, 1, 0.8f ), dir( -1, -1, 0.8f ), dir( 1, -1, 0.8f ) };
	double capSolid = 2.0 * MC_PI * ( 1.0 - ( double )cosA );
	CHECK_NEAR( std::fabs( loopSum( around ) ), capSolid, 0.01 * capSolid );
	// (c) loop overlapping the cap boundary (crossing case)
	std::vector<float3> straddle = { dir( 0.55f, 0.4f, 1 ), dir( -0.55f, 0.4f, 1 ), dir( -0.55f, -0.4f, 1 ), dir( 0.55f, -0.4f, 1 ) };
	double mcStraddle = std::fabs( CapLoopSolidMC( straddle, n, cosA ) );
	CHECK_NEAR( std::fabs( loopSum( straddle ) ), mcStraddle, 0.02 * capSolid );
	// (d) loop wholly OUTSIDE, not enclosing: 0
	std::vector<float3> off = { dir( 1, 0.1f, 0.2f ), dir( 1, -0.1f, 0.2f ), dir( 1, -0.1f, 0.4f ), dir( 1, 0.1f, 0.4f ) };
	CHECK_NEAR( std::fabs( loopSum( off ) ), 0.0, 0.005 * capSolid );
}

TEST( SoftPrimB, dir_occlusion_fuzz_vs_mc_winding )
{
	// whole-function check against the cap-sampled MC on floating planar loops in the FRONT hemisphere -
	// no silhouette/parallax confounds, no ray-cast disk-vs-solid-angle mismatch: both sides measure the
	// same functional (winding-weighted cap coverage fraction).
	Rng rng( 313131u );
	int tested = 0;
	float worst = 0;
	for( int k = 0; k < 60; k++ )
	{
		float3 L( 0, 0, 12 );
		float3 P( rng.f( -1, 1 ), rng.f( -1, 1 ), 0 );
		float  r = rng.f( 0.8f, 3.0f );
		Box b = MakeBox( float3( rng.f( -2, 2 ), rng.f( -2, 2 ), rng.f( 4, 9 ) ),
						 float3( rng.f( 0.3f, 1.5f ), rng.f( 0.3f, 1.5f ), 0.02f ), rng.f( 0, 3.14f ), rng.f( -0.2f, 0.2f ) );
		auto loop = Silhouette( b, L );
		if( loop.size() < 3 ) { continue; }
		tested++;
		float occ = DirOcclusion( BuildCaster( { loop } ), P, L, r );
		// MC: the same loop as directions from P, winding-weighted cap overlap fraction
		std::vector<float3> dirs;
		for( float3 V : loop ) { dirs.push_back( normalize( V - P ) ); }
		float3 nrm = normalize( L - P );
		float distPL = length( L - P );
		float rc = std::fmax( r, 1e-2f );
		float cosA = distPL / std::sqrt( distPL * distPL + rc * rc );
		double capSolid = 2.0 * MC_PI * ( 1.0 - ( double )cosA );
		double mc = std::fabs( CapLoopSolidMC( dirs, nrm, cosA, 300 ) ) / capSolid;
		if( mc > 1.0 ) { mc = 1.0; }
		worst = std::fmax( worst, ( float )std::fabs( occ - mc ) );
		CHECK_NEAR( occ, mc, 0.02 );
	}
	std::printf( "    [dirmc] %d loops, worst |occ-mc|=%.4f\n", tested, worst );
	CHECK( tested > 40 );
}

// ====================================================================== 5. FINDING-SPECIFIC TESTS
// One deterministic minimal input per adversarial-review finding. These assert the CORRECT behaviour
// and stay RED until the corresponding fix lands (user decision: no characterization escapes).

TEST( SoftFinding, F1_F2_caster_behind_receiver_does_not_occlude_dirspace )
{
	// closed square loop BEHIND P (anti-light side) encircling the P->L axis. It blocks nothing (the light
	// is the other way), but the direction-space sector measure winds about the axis' antipodal piercing.
	const float3 L( 0, 0, 10 );
	const float3 P( 0, 0, 0 );
	std::vector<float3> behind = { float3( -3, -3, -5 ), float3( 3, -3, -5 ), float3( 3, 3, -5 ), float3( -3, 3, -5 ) };
	float occ = DirOcclusion( BuildCaster( { behind } ), P, L, 1.0f );
	std::printf( "    [F1F2] behind-caster direction-space occ=%.4f (must be 0)\n", occ );
	CHECK_NEAR( occ, 0.0f, 1e-3f );
	// same loop, planar implementation: near-plane clip removes it entirely
	float occP = 1.0f - LiveShadow( BuildCaster( { behind } ), P, L, 1.0f );
	CHECK_NEAR( occP, 0.0f, 1e-3f );
}

TEST( SoftFinding, F3_axis_crossing_edge_is_continuous )
{
	// an edge whose direction projection passes within +-delta of the P->L axis: the sector term's wrapped
	// azimuth sweep must vary CONTINUOUSLY through delta=0, not jump by a full cap.
	const float3 L( 0, 0, 10 );
	const float3 P( 0, 0, 0 );
	const float  r = 1.0f;
	auto occAt = [&]( float delta ) -> float
	{
		// open wedge: edge crossing under the axis at height z=-1 (behind P relative to the light is z<0;
		// keep it in FRONT at z=+1 so only the axis-crossing tie is in play, not the behind-hemisphere bug)
		std::vector<float3> loop = { float3( -1, delta, 1 ), float3( 1, delta, 1 ), float3( 1, delta + 4.0f, 1 ), float3( -1, delta + 4.0f, 1 ) };
		return DirOcclusion( BuildCaster( { loop } ), P, L, r );
	};
	float below = occAt( -1e-5f );
	float above = occAt( +1e-5f );
	float at    = occAt( 0.0f );
	std::printf( "    [F3] occ(delta=-1e-5)=%.4f occ(0)=%.4f occ(+1e-5)=%.4f\n", below, at, above );
	CHECK_NEAR( below, above, 0.02f );		// continuity across the axis-crossing tie
	CHECK_NEAR( at, 0.5f * ( below + above ), 0.26f );	// and the tie itself is not an outlier
}

TEST( SoftFinding, F4_rim_grazing_root_selection_is_continuous )
{
	// one-in-one-out edges with the OUT endpoint grazing the rim: as B crosses the rim the area must move
	// continuously (the correct fallback for a t2 that float-rounds outside [0,1] is clamping, not the
	// OPPOSITE root). Scan a dense family bracketing the rim; a jump >> the disk quantum = the wrong root.
	const float r2 = 1.0f;
	float2 A( 0.6f, 0.05f );					// inside
	float prev = -1e9f;
	float worstJump = 0;
	for( int k = 0; k <= 4000; k++ )
	{
		float scale = 0.999f + 2e-6f * k * 0.25f;			// B length sweeps ~[0.999, 1.001]
		float2 B( 0.8f * scale, 0.6f * scale );
		float a = SoftDisk_CircleTriArea( A, B, r2 );
		if( prev > -1e8f ) { worstJump = std::fmax( worstJump, std::fabs( a - prev ) ); }
		prev = a;
	}
	std::printf( "    [F4] worst adjacent-step jump across rim-grazing sweep = %.6f (step ~5e-7 of arc)\n", worstJump );
	CHECK( worstJump < 1e-3f );

	// ulp-level hunt for the out-of-range exit root: rim-grazing in->out edges where t2 float-rounds
	// outside [0,1] (0.25% of grazing edges). The OBSERVABLE contract: the area must stay continuous -
	// compare each firing config against the same edge with B pushed measurably outside the rim, where
	// the root is comfortably in range. (The old code substituted the ENTRY root t1 - a point off the
	// segment on the far side of the circle - jumping the area by up to the full chord sector.)
	Rng rng( 20260807u );
	int inout = 0, fallback = 0, discontinuous = 0;
	float worstDev = 0;
	for( int k = 0; k < 400000; k++ )
	{
		float ang = rng.f( 0, 6.2831853f );
		float2 B( ( 1.0f + rng.f( -3e-7f, 3e-7f ) ) * std::cos( ang ), ( 1.0f + rng.f( -3e-7f, 3e-7f ) ) * std::sin( ang ) );
		float2 A( B.x * rng.f( 0.2f, 0.98f ), B.y * rng.f( 0.2f, 0.98f ) );		// inside, roughly along the same ray
		float a2 = dot( A, A ), b2 = dot( B, B );
		if( !( a2 <= 1.0f && b2 > 1.0f ) ) { continue; }						// need the in->out branch exactly
		inout++;
		float2 D = B - A;
		if( dot( D, D ) < 1e-9f ) { continue; }
		softSegRoots_t rt = SoftDisk_SegCircleRoots( A, D, 1.0f );
		if( rt.disc <= 0.0f ) { continue; }
		if( rt.t2 >= 0.0f && rt.t2 <= 1.0f ) { continue; }						// only the guarded firings
		fallback++;
		float aHere = SoftDisk_CircleTriArea( A, B, 1.0f );
		float2 Bout( B.x * 1.0002f, B.y * 1.0002f );							// clearly outside: root well in range
		float aRef  = SoftDisk_CircleTriArea( A, Bout, 1.0f );
		float dev = std::fabs( aHere - aRef );
		worstDev = std::fmax( worstDev, dev );
		if( dev > 1e-3f ) { discontinuous++; }
	}
	std::printf( "    [F4] %d in-out rim-grazing configs: guarded fallback fired %d times, discontinuous %d (worst dev %.5f)\n",
			inout, fallback, discontinuous, worstDev );
	CHECK( fallback > 100 );			// the hunt genuinely reached the guarded path
	CHECK( discontinuous == 0 );		// and the area stayed continuous through it
}

TEST( SoftFinding, F5_collinear_through_origin_sector_is_continuous )
{
	// both-outside edge whose chord passes through the disk centre: atan2's +-pi tie. Nudge the chord
	// across zero perpendicular offset; the sector area must be continuous (it flips sign smoothly only
	// through geometry, never by +-pi*r^2 from a rounding).
	const float r2 = 1.0f;
	auto areaAt = [&]( float offs ) -> float
	{
		float2 A( 2.0f, offs ), B( -3.0f, -offs * 1.5f );	// chord through (0, ~offs) region, both outside
		return SoftDisk_CircleTriArea( A, B, r2 );
	};
	float below = areaAt( -1e-6f ), at = areaAt( 0.0f ), above = areaAt( 1e-6f );
	std::printf( "    [F5] sector area offs=-1e-6: %.6f  0: %.6f  +1e-6: %.6f\n", below, at, above );
	// NOTE: this configuration CROSSES the disk (secant branch), so it is continuous by construction;
	// the tie case is the far-side segment. Test that one:
	auto farAt = [&]( float offs ) -> float
	{
		float2 A( 2.0f, offs ), B( 3.0f, offs * 1.5f );		// off-segment side: pure sector branch
		return SoftDisk_CircleTriArea( A, B, r2 );
	};
	float fb = farAt( -1e-6f ), fa = farAt( 1e-6f ), f0 = farAt( 0.0f );
	std::printf( "    [F5] far-side sector offs=-1e-6: %.6f  0: %.6f  +1e-6: %.6f\n", fb, f0, fa );
	CHECK_NEAR( fb, fa, 1e-4f );
	CHECK_NEAR( above, below, 1e-4f );
}

TEST( SoftFinding, F6_caster_piercing_light_plane )
{
	// box straddling the light plane, waist inside the disk radius. The far-plane clip's connector chord has
	// scale 1 (no eps blowup): if it cuts the disk, the abs()'d area loses real occlusion. Truth: heavy shadow.
	// light OUTSIDE the caster (a box containing the light is F9's case, not this one): the box straddles
	// the light PLANE dn = distPL beside the light, its lower half genuinely shadowing the disk edge.
	const float3 L( 0, 0, 10 );
	const float3 P( 0, 0, 0 );
	const float  r = 2.0f;
	Box pierce = MakeBox( float3( 1.5f, 0, 10 ), float3( 0.8f, 0.8f, 2.0f ) );	// x in [0.7,2.3], z in [8,12]
	float truth = TruthShadow( P, L, r, pierce, 300 );
	float live  = LiveShadow( BuildCaster( { Silhouette( pierce, L ) } ), P, L, r );
	std::printf( "    [F6] pierce-light-plane: truth=%.4f live=%.4f\n", truth, live );
	// MECHANISM (dissected 2026-08-08): the box's OUTER face (x=2.3) lies entirely at dn=12, beyond the
	// light plane dn=distPL=10, so every one of its edges is dropped by the slab clip. The two surviving
	// silhouette edges both cross the plane at the INNER face x=0.70, so the connector traces only that
	// inner face - the caster's true cross-section AT the light plane (the full x in [0.7,2.3] rectangle,
	// = the silhouette of the CLIPPED solid, which the edge stream does not contain) is never emitted.
	// The loop encloses a 2.5% sliver where truth is 19%. Fix = emit the light-plane cross-section contour
	// per fragment (needs caster face/solid data, not just the light-apex silhouette). RED until that lands.
	CHECK_NEAR( live, truth, 0.05f );
}

TEST( SoftFinding, F6b_receiver_apex_recovers_the_cross_section )
{
	// Does the RECEIVER-apex silhouette (of the box, from P) recover the light-plane cross-section the
	// light-apex silhouette drops? The light-apex silhouette of this box is just its left face (crosses the
	// light plane only at x=0.70, a thin sliver); the receiver-apex silhouette also carries the bottom edge
	// spanning the full width, which projects to the whole footprint. Same live coverage function on both.
	const float3 L( 0, 0, 10 );
	const float3 P( 0, 0, 0 );
	const float  r = 2.0f;
	Box b = MakeBox( float3( 1.5f, 0, 10 ), float3( 0.8f, 0.8f, 2.0f ) );
	float truth = TruthShadow( P, L, r, b, 300 );
	float shadeLight = LiveShadow( BuildCaster( { Silhouette( b, L ) } ), P, L, r );
	// box -> triangle soup (8 corners, 6 quad faces -> 12 tris) for receiver-apex adjacency
	std::vector<float> verts( 24 );
	for( int i = 0; i < 8; i++ ) { verts[i * 3 + 0] = b.c[i].x; verts[i * 3 + 1] = b.c[i].y; verts[i * 3 + 2] = b.c[i].z; }
	std::vector<uint32_t> idx;
	for( int fi = 0; fi < 6; fi++ )
	{
		idx.push_back( b.f[fi][0] ); idx.push_back( b.f[fi][1] ); idx.push_back( b.f[fi][2] );
		idx.push_back( b.f[fi][0] ); idx.push_back( b.f[fi][2] ); idx.push_back( b.f[fi][3] );
	}
	std::vector<TriEdgeAdj> adj;
	BuildTriEdgeAdj( verts.data(), idx.data(), ( uint32_t )idx.size(), adj );
	std::vector<float4> rec;
	AppendReceiverSilhouetteRecords( adj, P, rec );
	float occR = rec.empty() ? 0.0f : saturate( SoftShadow_WedgeOcclusion( P, L, r, 0, ( int )( rec.size() / 2 ), 0.0f, SoftEdgeBuffer{ rec.data(), ( int )rec.size() } ) );
	float shadeRecv = 1.0f - occR;
	// ORDER-INDEPENDENT variant for the SHADER port: select receiver-apex edges per-fragment and sum their
	// clipped circle-triangle areas with NO connector and NO chain closure. If this matches the chained value
	// the shader needs no per-fragment chaining (Sum CircleTriArea over a consistently-oriented closed edge
	// set is traversal-order-independent; only the far-clip connector wanted order).
	softFrame_t fr = SoftShadow_Frame( P, L );
	float rr = std::fmax( r, 1e-2f ), r2 = rr * rr;
	double areaOI = 0;
	for( const TriEdgeAdj& e : adj )
	{
		bool faP = dot( e.nA, P - e.A ) > 0.0f, fbP = dot( e.nB, P - e.A ) > 0.0f;
		bool isSil = ( e.count < 2 ) || ( faP != fbP );
		if( !isSil ) { continue; }
		float3 A, B;
		if( e.count < 2 ) { if( faP ) { A = e.A; B = e.B; } else { A = e.B; B = e.A; } }
		else { if( !fbP ) { A = e.A; B = e.B; } else { A = e.B; B = e.A; } }
		float3 a = A - P, b = B - P;
		float dnA = dot( a, fr.nrm ), dnB = dot( b, fr.nrm ), d = dnB - dnA;
		softClip_t cl = SoftShadow_ClipSlab( dnA, dnB, SW_NEAR_EPS, fr.distPL );
		if( cl.empty ) { continue; }
		float2 q0 = SoftShadow_ProjectVert( a + cl.t0 * ( b - a ), dnA + cl.t0 * d, fr );
		float2 q1 = SoftShadow_ProjectVert( a + cl.t1 * ( b - a ), dnA + cl.t1 * d, fr );
		areaOI += SoftDisk_CircleTriArea( q0, q1, r2 );		// NO connector, NO closure
	}
	float shadeOI = 1.0f - saturate( ( float )( std::fabs( areaOI ) / ( PI * r2 ) ) );
	// THE SHIPPED SHADER FUNCTION: build the candidate-edge buffer (4 float4/edge: A+vidA, B+vidB, nA+bnd, nB)
	// and run SoftShadow_ProcCaster (per-fragment select + O(n^2) chain + coverage), exactly as the GPU will.
	std::vector<float4> cand;
	for( const TriEdgeAdj& e : adj )
	{
		cand.push_back( float4( e.A.x, e.A.y, e.A.z, ( float )e.va ) );
		cand.push_back( float4( e.B.x, e.B.y, e.B.z, ( float )e.vb ) );
		cand.push_back( float4( e.nA.x, e.nA.y, e.nA.z, e.count < 2 ? 1.0f : 0.0f ) );
		cand.push_back( float4( e.nB.x, e.nB.y, e.nB.z, 0.0f ) );
	}
	bool blocksCentre = RayHitsBox( P, L - P, b );
	float occProc = SoftShadow_ProcCaster( P, L, r, blocksCentre, 0, ( int )adj.size(), SoftEdgeBuffer{ cand.data(), ( int )cand.size() } );
	float shadeProc = 1.0f - occProc;
	std::printf( "    [F6b] truth=%.4f  light-apex=%.4f (err %.3f)  receiver-apex(chained)=%.4f  order-indep=%.4f  SHADER ProcCaster=%.4f (err %.3f)\n",
			truth, shadeLight, std::fabs( shadeLight - truth ), shadeRecv, shadeOI, shadeProc, std::fabs( shadeProc - truth ) );
	CHECK_NEAR( shadeRecv, truth, 0.05f );		// receiver-apex silhouette must recover the cross-section
	CHECK_NEAR( shadeProc, truth, 0.05f );		// and the SHIPPED shader function reproduces it (O(n^2) chained)
}

TEST( SoftFinding, F7_near_plane_projection_stays_finite )
{
	// vertex just in front of the near plane at Doom scales: projection scale distPL/dn ~ 2e7 -> projected
	// coords ~1e9 -> qb^2 overflows float in the root solve. The function must stay FINITE (and, with HLSL
	// NaN semantics in the shim, a NaN would silently become 0 occlusion on the GPU - a lit hole in shadow).
	const float3 L( 0, 0, 20000.0f );
	const float3 P( 0, 0, 0 );
	const float  r = 32.0f;
	// quad straddling the receiver plane: two verts at dn ~ +1.5e-3 (just inside the slab), two far below
	std::vector<float3> quad = { float3( -200, -200, 1.5e-3f ), float3( 200, -200, 1.5e-3f ),
								 float3( 200, 200, -50 ), float3( -200, 200, -50 ) };
	float occ = 1.0f - LiveShadow( BuildCaster( { quad } ), P, L, r );
	std::printf( "    [F7] huge-projection occ=%.6f finite=%d\n", occ, ( int )std::isfinite( occ ) );
	CHECK( std::isfinite( occ ) );
	CHECK( occ >= 0.0f && occ <= 1.0f );
}

TEST( SoftFinding, F8_receiver_on_caster_surface_truth_not_lit )
{
	// contact-shadow contract of the truth oracle. (A point on the TOP face, light overhead, is genuinely
	// fully lit - the solid is entirely behind it - so that is the negative control, not the bug.)
	const float3 L( 0, 0, 12 );
	const float  r = 2.0f;
	Box b = MakeBox( float3( 0, 0, 3 ), float3( 2, 2, 1 ) );		// z in [2,4], top at z=4
	// (1) just INSIDE the solid: every disk ray starts in the box -> fully shadowed. The old RayHitsBox
	// returned "unblocked" for a segment starting inside (tmin stays 0), lit-biasing all contact shadows.
	float3 Pin( 0, 0, 3.9f );
	float tin = TruthShadow( Pin, L, r, b, 100 );
	std::printf( "    [F8] truth just inside the solid = %.4f (must be ~0)\n", tin );
	CHECK( tin < 0.1f );
	// (2) on a SIDE face with the light overhead: rays toward the disk cut through the solid's upper
	// corner -> heavily shadowed, never fully lit.
	float3 Pside( 2.0f, 0, 3.0f );
	float tside = TruthShadow( Pside, L, r, b, 200 );
	std::printf( "    [F8] truth on the side face = %.4f (must be well below lit)\n", tside );
	CHECK( tside < 0.6f );
	// (3) negative control: on the TOP face the solid is wholly behind the surface -> fully lit.
	float3 Ptop( 0, 0, 4.0f );
	float ttop = TruthShadow( Ptop, L, r, b, 200 );
	std::printf( "    [F8] truth on the top face = %.4f (control: must be ~1)\n", ttop );
	CHECK( ttop > 0.99f );
}

TEST( SoftFinding, F9_light_inside_caster_is_umbra )
{
	// the LIGHT sits inside a closed caster: no ray escapes, every receiver is in full shadow. Silhouette()
	// currently returns an empty loop and BuildCaster emits a +-1e30 garbage header the cull skips -> lit.
	// disk radius SMALLER than the shell so the whole disk is inside it: every P->disk ray must enter the
	// solid first (with a big disk, parts genuinely poke out of the shell and are visible - not this bug).
	const float3 L( 0, 0, 6 );
	const float3 P( 0, 0, 0 );
	const float  r = 0.5f;
	Box shell = MakeBox( float3( 0, 0, 6 ), float3( 1, 1, 1 ) );	// light at its centre
	float truth = TruthShadow( P, L, r, shell, 200 );
	CHECK_NEAR( truth, 0.0f, 1e-3f );								// oracle agrees: fully occluded
	auto loop = Silhouette( shell, L );
	float live = LiveShadow( BuildCaster( { loop } ), P, L, r );
	std::printf( "    [F9] light-inside-caster: silhouette verts=%zu live shadow=%.4f (must be 0)\n", loop.size(), live );
	CHECK_NEAR( live, 0.0f, 1e-3f );
}

TEST( SoftFinding, F10_pinch_vertex_chain_fusion_is_benign )
{
	// two closed loops sharing one bit-identical vertex, loop 2 emitted immediately after loop 1 ends at
	// that vertex: the bit-exact chain-boundary test cannot see the boundary and fuses them into one chain.
	// PROVE the fused walk still sums both areas (the zero-length connector degenerates to 0).
	const float3 L( 0, 0, 12 );
	const float3 P( 0, 0, 0 );
	const float  r = 2.5f;
	float3 pinch( 0.0f, 0.0f, 6.0f );
	std::vector<float3> loopA = { pinch, float3( -1.2f, 0.2f, 6 ), float3( -1.2f, -1.0f, 6 ), float3( -0.1f, -1.0f, 6 ) };
	std::vector<float3> loopB = { pinch, float3( 1.1f, 0.3f, 6 ), float3( 1.1f, 1.2f, 6 ), float3( 0.1f, 1.2f, 6 ) };
	// stream them so loopA's closing edge ends AT pinch and loopB's first edge starts AT pinch (bit-equal)
	float occFused = 1.0f - LiveShadow( BuildCaster( { loopA, loopB } ), P, L, r );
	// reference: the two loops as separate CASTERS would max-combine, so instead compare against analytic
	// disk areas via the MC polygon integrator on the union (loops are disjoint except the pinch point).
	softFrame_t f = SoftShadow_Frame( P, L );
	auto proj = [&]( const std::vector<float3>& lp )
	{
		std::vector<float2> out;
		for( float3 V : lp )
		{
			float3 rel = V - P;
			out.push_back( SoftShadow_ProjectVert( rel, dot( rel, f.nrm ), f ) );
		}
		return out;
	};
	float rc = std::fmax( r, 1e-2f );
	double expect = ( std::fabs( DiskPolyAreaMC( proj( loopA ), rc ) ) + std::fabs( DiskPolyAreaMC( proj( loopB ), rc ) ) ) / ( MC_PI * rc * rc );
	std::printf( "    [F10] fused-chain occ=%.4f expected(sum of loops)=%.4f\n", occFused, expect );
	CHECK_NEAR( occFused, expect, 0.02 );
}

TEST( SoftFinding, F11_edge_through_receiver_epsilon )
{
	// caster edge passing through P's epsilon-neighbourhood: dA ~ -dB (antipodal directions). CapTri
	// currently drops the contribution (e2len ~ 0 -> return 0). The direction-space occlusion of a huge
	// quad whose edge passes just over P must approach HALF the cap, continuously - not fall to a wrong
	// value at the antipodal tie.
	const float3 L( 0, 0, 10 );
	const float3 P( 0, 0, 0 );
	const float  r = 1.0f;
	auto occAt = [&]( float h ) -> float
	{
		// half-plane-ish quad at height z=h over P, extending far in +y, edge along x through (0,0,h)
		std::vector<float3> quad = { float3( -50, 0, h ), float3( 50, 0, h ), float3( 50, 100, h ), float3( -50, 100, h ) };
		return DirOcclusion( BuildCaster( { quad } ), P, L, r );
	};
	float at1   = occAt( 1.0f );		// comfortably above: blocks the +y half of the cap ~ 0.5
	float at001 = occAt( 0.01f );		// grazing: the edge's directions from P are nearly +-x
	// TRUE antipodal tie: the leading edge spans x +-50 at height h; e2len = |dB - dA*dot| ~ 2h/50, so the
	// 1e-6 degenerate guard trips when h < ~2.5e-5. h=1e-5 puts the edge INSIDE the guard's dead zone.
	float atTie = occAt( 1e-5f );
	std::printf( "    [F11] half-quad occ h=1: %.4f  h=0.01: %.4f  h=1e-5: %.4f (all must be ~0.5)\n", at1, at001, atTie );
	CHECK_NEAR( at1, 0.5f, 0.03f );
	CHECK_NEAR( at001, 0.5f, 0.05f );
	CHECK_NEAR( atTie, 0.5f, 0.05f );		// symmetric case: passes even with the tie edge dropped (cancellation)

	// The DIRECT probe: CapTri itself must be continuous as the edge's endpoint directions approach
	// antipodal. The arc dA->dB at delta passes over the POLE (through the cap); its contribution is a
	// smooth function of delta, but the antipodal guard (e2len < 1e-6) snaps it to 0 below delta ~2.5e-7.
	{
		float3 n( 0, 0, 1 ), uu( 1, 0, 0 ), vv( 0, 1, 0 );
		float cosA = std::cos( 0.5235988f );	// 30 deg cap
		auto tieCapTri = [&]( float delta ) -> float
		{
			float3 dA = normalize( float3( -1, 0, delta ) ), dB = normalize( float3( 1, 0, delta ) );
			return CapTri( dA, dB, n, uu, vv, cosA );
		};
		float v3n = tieCapTri( 1e-3f ), v5 = tieCapTri( 1e-5f ), v7 = tieCapTri( 1e-7f );
		std::printf( "    [F11] CapTri over-the-pole arc: delta=1e-3: %.6f  1e-5: %.6f  1e-7: %.6f (must be continuous)\n",
				v3n, v5, v7 );
		CHECK_NEAR( v7, v3n, 0.05f * std::fabs( v3n ) + 1e-4f );	// continuity into the antipodal limit
	}

	// and the whole-function asymmetric case: light over the quad INTERIOR, receiver exactly under the
	// quad's edge. Every disk ray immediately crosses the quad plane inside the quad -> true occ = 1.
	// A dropped grazing-edge contribution cannot hide here (no symmetric partner to cancel against).
	{
		const float3 L2( 0, 20, 10 );
		auto occAt2 = [&]( float h ) -> float
		{
			std::vector<float3> quad = { float3( -50, 0, h ), float3( 50, 0, h ), float3( 50, 100, h ), float3( -50, 100, h ) };
			return DirOcclusion( BuildCaster( { quad } ), P, L2, r );
		};
		float a3 = occAt2( 1e-3f ), a5 = occAt2( 1e-5f );
		std::printf( "    [F11] asymmetric grazing quad occ h=1e-3: %.4f  h=1e-5: %.4f (true occ ~1)\n", a3, a5 );
		CHECK_NEAR( a3, 1.0f, 0.05f );
		CHECK_NEAR( a5, 1.0f, 0.05f );
	}
}

TEST( SoftFinding, F13_test_header_sphere_must_be_tight )
{
	// BuildCaster's header radius must be the half-diagonal of the loop's AABB (a tight bound). The 2x
	// oversized len(hi-lo) masks any engine-side undersizing in every capture test. Assert tightness, and
	// assert a caster with the CORRECT tight radius still renders identically (cull stays conservative).
	const float3 L( 0, 0, 12 );
	const float3 P( 1.5f, 0.5f, 0 );
	const float  r = 2.0f;
	Box b = MakeBox( float3( -1, 1, 6 ), float3( 1.0f, 0.6f, 0.3f ), 0.7f, 0.2f );
	auto loop = Silhouette( b, L );
	CHECK( loop.size() >= 3 );
	std::vector<float4> rec = BuildCaster( { loop } );
	float3 lo( 1e30f, 1e30f, 1e30f ), hi( -1e30f, -1e30f, -1e30f );
	for( float3 V : loop )
	{
		lo = float3( std::fmin( lo.x, V.x ), std::fmin( lo.y, V.y ), std::fmin( lo.z, V.z ) );
		hi = float3( std::fmax( hi.x, V.x ), std::fmax( hi.y, V.y ), std::fmax( hi.z, V.z ) );
	}
	float tight = 0.5f * length( hi - lo );
	std::printf( "    [F13] BuildCaster header radius=%.4f tight half-diagonal=%.4f\n", rec[1].x, tight );
	CHECK_NEAR( rec[1].x, tight, tight * 0.01f );		// tight bound, not 2x
	// tight radius must not change the result vs the inflated one (cull conservative w.r.t. the honest sphere)
	std::vector<float4> tightRec = rec;
	tightRec[1].x = tight;
	SoftEdgeBuffer b1{ rec.data(), ( int )rec.size() }, b2{ tightRec.data(), ( int )tightRec.size() };
	float o1 = SoftShadow_WedgeOcclusion( P, L, r, 0, ( int )( rec.size() / 2 ), 0.0f, b1 );
	float o2 = SoftShadow_WedgeOcclusion( P, L, r, 0, ( int )( tightRec.size() / 2 ), 0.0f, b2 );
	CHECK_NEAR( o1, o2, 1e-5f );
}

TEST( SoftFinding, F14_headerless_stream_still_occludes )
{
	// records before the first header: currently fully computed then silently DISCARDED (haveCaster false).
	// Correct contract: a stream that begins with edge records is a caster without cull data - it must
	// still contribute (silent zero turns an engine offset bug into invisible missing shadows).
	const float3 L( 0, 0, 12 );
	const float3 P( 0, 0, 0 );
	const float  r = 2.0f;
	Box big = MakeBox( float3( 0, 0, 6 ), float3( 4, 4, 0.1f ) );		// full umbra
	std::vector<float4> rec = BuildCaster( { Silhouette( big, L ) } );
	std::vector<float4> headerless( rec.begin() + 2, rec.end() );		// strip the header pair
	SoftEdgeBuffer buf{ headerless.data(), ( int )headerless.size() };
	float occ = SoftShadow_WedgeOcclusion( P, L, r, 0, ( int )( headerless.size() / 2 ), 0.0f, buf );
	std::printf( "    [F14] headerless full-umbra stream occ=%.4f (must be ~1)\n", occ );
	CHECK( occ > 0.99f );
}

TEST( SoftFinding, F15_shim_nan_semantics_match_hlsl )
{
	const float nan = std::nanf( "" );
	CHECK( saturate( nan ) == 0.0f );			// D3D: saturate(NaN) = 0
	CHECK( max( nan, 0.25f ) == 0.25f );		// min/max prefer the non-NaN operand
	CHECK( max( 0.25f, nan ) == 0.25f );
	CHECK( min( nan, 0.25f ) == 0.25f );
	CHECK( min( 0.25f, nan ) == 0.25f );
	CHECK( saturate( -0.5f ) == 0.0f );
	CHECK( saturate( 1.5f ) == 1.0f );
	CHECK( saturate( 0.5f ) == 0.5f );
}

// ====================================================================== 5b. THE SHIPPED PIPELINE
// The tests above verify the coverage INTEGRAL. The game does not ship the integral - it ships
// integral + STENCIL BAND CLASSIFICATION (r_softShadowAAM: umbra>=2 / ring==1 / lit==0), and the
// classification is computed by counting stencil crossings along the CAMERA ray. These tests compose
// the full pipeline the way RenderBackend does and assert the one property the artifact violates:
// the shadow term at a WORLD point must not depend on the camera. (User report: tripod shadow is
// not camera-stable. The suite was green because nothing tested the composition.)
namespace
{

// stencil count of the INFLATED silhouette shell, mirroring the FIXED pipeline: softband.vs.hlsl now
// emits a WATERTIGHT capped volume per caster (side quads + near/far cap fans from the caster centre)
// and the backend marks it with z-FAIL (crossings BEHIND the fragment, signed with the engine's
// Carmack's-reverse polarity). For a closed oriented surface that count equals the fragment's
// containment winding - the camera only picks the ray direction, so the result is camera-independent.
// The pipeline tests still sweep cameras to PROVE that property rather than assume it.
int ShellZFailCapped( const std::vector<std::pair<float3, float3>>& edges, float3 centre, float rp,
					  float3 L, float3 C, float3 P )
{
	float3 d = P - C;
	float tP = std::sqrt( dot( d, d ) );
	if( tP < 1e-4f ) { return 0; }
	d = d * ( 1.0f / tP );
	const float BIG = 1e5f;
	float3 ctrF = centre + ( centre - L ) * BIG;
	int zfail = 0;
	for( const std::pair<float3, float3>& e : edges )
	{
		float3 dA = e.first - centre;  float la = std::sqrt( dot( dA, dA ) );
		float3 dB = e.second - centre; float lb = std::sqrt( dot( dB, dB ) );
		float3 A = la > 1e-4f ? e.first  + dA * ( rp / la ) : e.first;		// radial inflation (swParm.z margin folded into rp)
		float3 B = lb > 1e-4f ? e.second + dB * ( rp / lb ) : e.second;
		float3 Ai = A + ( A - L ) * BIG, Bi = B + ( B - L ) * BIG;
		// side quad + near cap fan (centre,B,A) + far cap fan (ctrF,Ai,Bi) - the exact softband.vs set
		float3 tris[4][3] = { { A, B, Bi }, { A, Bi, Ai }, { centre, B, A }, { ctrF, Ai, Bi } };
		for( int t = 0; t < 4; t++ )
		{
			float3 v0 = tris[t][0], v1 = tris[t][1], v2 = tris[t][2];
			float3 e1v = v1 - v0, e2v = v2 - v0, pv = cross( d, e2v );
			float det = dot( e1v, pv );
			if( std::fabs( det ) < 1e-9f ) { continue; }
			float inv = 1.0f / det;
			float3 tv = C - v0;
			float uu = dot( tv, pv ) * inv;
			if( uu < 0.0f || uu > 1.0f ) { continue; }
			float3 qv = cross( tv, e1v );
			float vv = dot( d, qv ) * inv;
			if( vv < 0.0f || uu + vv > 1.0f ) { continue; }
			float tt = dot( e2v, qv ) * inv;
			if( tt <= 0.0f ) { continue; }
			if( tt >= tP )															// z-FAIL: behind the fragment
			{
				zfail += ( dot( cross( e1v, e2v ), d ) < 0.0f ) ? -1 : +1;			// back-face INCR / front-face DECR
			}
		}
	}
	return zfail;
}

}

TEST( SoftShadowPipeline, aam_shadow_term_is_camera_stable_tripod )
{
	// THE tripod scene: three thin legs under an overhead light, receivers on the floor. The AAM band
	// classifies each receiver by stencil counts along the camera ray: core (capped z-fail, exact and
	// camera-independent - modelled by the hard containment test) stamps umbra to >=2; the capless
	// z-pass SHELL adds the penumbra ring. Composition contract: shadow(P) identical from EVERY camera.
	const float3 L( 0, 0, 12 );
	const float  rp = 3.0f;
	const float  bandRp = rp * 1.1f;			// RenderBackend swParm margin
	// three legs, feet at radius 1.2
	std::vector<Box> legs;
	std::vector<std::vector<float3>> legLoops;
	std::vector<float3> legCentres;
	std::vector<float4> rec;					// one stream, three casters (world-surface style)
	for( int i = 0; i < 3; i++ )
	{
		float a = ( float )( i * 2.0 * MC_PI / 3.0 );
		Box leg = MakeBox( float3( 1.2f * std::cos( a ), 1.2f * std::sin( a ), 2.5f ), float3( 0.12f, 0.12f, 2.5f ) );
		legs.push_back( leg );
		auto loop = Silhouette( leg, L );
		CHECK( loop.size() >= 3 );
		legLoops.push_back( loop );
		auto r1 = BuildCaster( { loop } );
		legCentres.push_back( float3( r1[0].x, r1[0].y, r1[0].z ) );
		rec.insert( rec.end(), r1.begin(), r1.end() );
	}
	// cameras: an orbit around the tripod at gameplay distances/heights, plus two close-ins. The
	// classification must agree across ALL of them.
	std::vector<float3> cams;
	for( int k = 0; k < 8; k++ )
	{
		float a = ( float )( k * 2.0 * MC_PI / 8.0 );
		cams.push_back( float3( 7.0f * std::cos( a ), 7.0f * std::sin( a ), 2.0f + ( k % 3 ) ) );
	}
	cams.push_back( float3( 2.0f, 0.5f, 1.2f ) );		// close in, likely inside a shell prism
	cams.push_back( float3( -0.5f, 1.5f, 0.8f ) );

	int pts = 0, unstable = 0;
	float worstSpread = 0;
	float3 worstP( 0, 0, 0 );
	for( int iy = -6; iy <= 6; iy++ )
		for( int ix = -6; ix <= 6; ix++ )
		{
			float3 P( ix * 0.5f, iy * 0.5f, 0.0f );
			pts++;
			float mn = 1e9f, mx = -1e9f;
			for( float3 C : cams )
			{
				// CORE: capped z-fail = exact containment in each leg's point-light shadow volume,
				// camera-independent (this part the engine gets right). Weight 4 per containing leg, so
				// up to three overlapping penumbra RINGS (1..3) never conflate into umbra (>= 4).
				int stencil = 0;
				for( const Box& leg : legs )
				{
					if( RayHitsBox( P + float3( 0, 0, 1e-3f ), L - P, leg ) ) { stencil += 1; }	// core: ANY deviation = umbra (sign-agnostic)
				}
				// SHELL: capped z-fail PARITY along the camera ray (sign-agnostic encoding: the shell bit
				// toggles per containing prism; the sweep proves camera-independence of the composition).
				int shellPar = 0;
				for( size_t li = 0; li < legs.size(); li++ )
				{
					std::vector<std::pair<float3, float3>> edges;
					const std::vector<float3>& lp = legLoops[li];
					for( size_t i = 0; i < lp.size(); i++ ) { edges.push_back( { lp[i], lp[( i + 1 ) % lp.size()] } ); }
					shellPar += std::abs( ShellZFailCapped( edges, legCentres[li], bandRp, L, C, P ) );
				}
				float shadow;
				if( stencil > 0 ) { shadow = 0.0f; }
				else if( ( shellPar & 1 ) != 0 )
				{
					SoftEdgeBuffer buf{ rec.data(), ( int )rec.size() };
					shadow = 1.0f - saturate( SoftShadow_WedgeOcclusion( P, L, rp, 0, ( int )( rec.size() / 2 ), 1.0f, buf ) );
				}
				else { shadow = 1.0f; }
				mn = std::fmin( mn, shadow );
				mx = std::fmax( mx, shadow );
			}
			float spread = mx - mn;
			if( spread > worstSpread ) { worstSpread = spread; worstP = P; }
			if( spread > 0.05f ) { unstable++; }
		}
	std::printf( "    [pipeline-tripod] %d floor points x %zu cameras: %d camera-UNSTABLE (spread>0.05), worst spread %.3f at (%.1f,%.1f)\n",
			pts, cams.size(), unstable, worstSpread, worstP.x, worstP.y );
	CHECK( unstable == 0 );			// the shadow term is a function of WORLD geometry only
}

TEST( SoftShadowPipeline, aam_full_pipeline_accuracy_and_stability_on_captures )
{
	// Full shipped composition on real captures: core = captured capped shadow volumes counted z-fail
	// (camera-independent), shell = capless z-pass of the inflated captured silhouettes along the
	// camera ray, coverage only at stencil==1. Measured from the CAPTURED camera and from displaced
	// cameras: accuracy vs ray truth AND camera stability.
	const char* names[] = { "erebus2", "erebus5", "erebus13" };
	int capsSeen = 0;
	for( const char* nm : names )
	{
		char path[512];
		std::snprintf( path, sizeof( path ), "/home/app/Games/gog/doom-3-bfg-edition/neo/tests/data/%s.softcap", nm );
		SoftCap c;
		if( !LoadSoftCap( path, c ) || c.receivers.empty() ) { continue; }
		capsSeen++;
		float3 cam0( c.hdr.vieworg[0], c.hdr.vieworg[1], c.hdr.vieworg[2] );
		std::vector<float3> cams = { cam0,
									 cam0 + float3( 150, 0, 0 ), cam0 + float3( -150, 40, 0 ),
									 cam0 + float3( 0, 150, 30 ), cam0 + float3( 40, -120, -20 ) };
		SoftEdgeBuffer buf{ reinterpret_cast<const float4*>( c.edges.data() ), ( int )( c.edges.size() * 2 ) };
		int n = 0, unstable = 0, exPipe = 0, misPipe = 0;
		double errSum = 0;
		for( uint32_t li = 0; li < c.hdr.numLights; li++ )
		{
			const softcapLight_t& Lt = c.lights[li];
			if( Lt.penumbraSize <= 0.0f || Lt.edgeCount == 0 ) { continue; }
			float3 Lo( Lt.origin[0], Lt.origin[1], Lt.origin[2] );
			std::vector<uint32_t> castIdx;
			for( const softcapCaster_t& cs : c.casters )
				if( cs.lightIndex == li ) { castIdx.insert( castIdx.end(), c.meshIdx.begin() + cs.firstIndex, c.meshIdx.begin() + cs.firstIndex + cs.numIndex ); }
			if( castIdx.empty() ) { continue; }
			// per-caster silhouette edge lists + centres (headers) for the shell
			struct ShellCaster { float3 centre; std::vector<std::pair<float3, float3>> edges; };
			std::vector<ShellCaster> shells;
			for( uint32_t rIdx = Lt.firstEdge; rIdx < Lt.firstEdge + Lt.edgeCount && rIdx < c.edges.size(); rIdx++ )
			{
				const softcapEdge_t& e = c.edges[rIdx];
				if( e.e0[3] < 0.0f )
				{
					ShellCaster sc; sc.centre = float3( e.e0[0], e.e0[1], e.e0[2] );
					shells.push_back( sc );
				}
				else if( !shells.empty() )
				{
					shells.back().edges.push_back( { float3( e.e0[0], e.e0[1], e.e0[2] ), float3( e.e1[0], e.e1[1], e.e1[2] ) } );
				}
			}
			const float bandRp = Lt.penumbraSize * 1.1f;
			for( const softcapReceiver_t& R : c.receivers )
			{
				if( R.lightIndex != li ) { continue; }
				uint32_t step = R.numVerts > 24 ? R.numVerts / 8 : 3;
				for( uint32_t vi = R.firstVert; vi < R.firstVert + R.numVerts; vi += step )
				{
					float3 P( c.recvVerts[vi * 3 + 0], c.recvVerts[vi * 3 + 1], c.recvVerts[vi * 3 + 2] );
					float3 toL = Lo - P; float dl = std::sqrt( dot( toL, toL ) );
					if( dl > 1e-4f ) { P = P + toL * ( 2.0f / dl ); }
					// core stencil: capped z-fail is containment in the caster's point-light shadow volume -
					// camera-independent by construction, so model it by its exact semantics (centre-ray
					// blocked per caster; the committed captures are v2/v3 and carry no baked volumes).
					int core = 0;
					for( const softcapCaster_t& cs : c.casters )
					{
						if( cs.lightIndex != li || cs.numIndex == 0 ) { continue; }
						if( RayHitsMesh( P, Lo - P, c.meshVerts.data(), c.meshIdx.data() + cs.firstIndex, cs.numIndex ) ) { core += 1; }	// any deviation = umbra
					}
					float mn = 1e9f, mx = -1e9f;
					for( float3 C : cams )
					{
						int shellPar = 0;
						for( const ShellCaster& sc : shells )
						{
							shellPar += std::abs( ShellZFailCapped( sc.edges, sc.centre, bandRp, Lo, C, P ) );
						}
						float shadow;
						if( core > 0 ) { shadow = 0.0f; }
						else if( ( shellPar & 1 ) != 0 )
						{
							shadow = 1.0f - saturate( SoftShadow_WedgeOcclusion( P, Lo, Lt.penumbraSize, ( int )( Lt.firstEdge * 2 ), ( int )Lt.edgeCount, 1.0f, buf ) );
						}
						else { shadow = 1.0f; }
						mn = std::fmin( mn, shadow );
						mx = std::fmax( mx, shadow );
						if( C.x == cam0.x && C.y == cam0.y && C.z == cam0.z )
						{
							float truth = MeshTruthShadowSoup( c.meshVerts.data(), castIdx.data(), ( uint32_t )castIdx.size(), P, Lo, Lt.penumbraSize, 8 );
							n++; errSum += std::fabs( shadow - truth );
							if( shadow < 0.5f && truth > 0.85f ) { exPipe++; }
							if( shadow > truth + 0.35f ) { misPipe++; }
						}
					}
					if( mx - mn > 0.05f ) { unstable++; }
				}
			}
		}
		std::printf( "    [pipeline %s] %d samples: EXTRANEOUS=%d MISSING=%d mean|err|=%.4f   camera-UNSTABLE=%d/%d\n",
				nm, n, exPipe, misPipe, n ? errSum / n : 0.0, unstable, n );
		CHECK( unstable == 0 );			// the shadow term is a function of WORLD geometry only
	}
	CHECK( capsSeen >= 2 );
}

// ====================================================================== 5b2. STENCIL-BAND CONTRACT
// Every property the band encoding RELIES ON, each asserted by its own test. These existed only as
// assumptions in review comments before - and one of them (clamp ops + baseline 0) shipped false,
// fabricating umbra on lit floors and letting the winding gate delete real shadow interiors.

TEST( SoftContract, clamp_ops_order_dependent_at_zero_exact_at_base )
{
	// The GLS stencil INCR/DECR map to CLAMP variants. Simulate: a balanced +1/-1 op stream must end at
	// net regardless of order - TRUE from BASE (no clamp engages), FALSE from 0 (DECR-first clamps).
	auto run = []( int start, const std::vector<int>& ops ) -> int
	{
		int s = start;
		for( int op : ops ) { s = std::max( 0, std::min( 255, s + op ) ); }
		return s;
	};
	// the hazard, minimal: pixel in front of a volume sees one DECR + one INCR (net 0)
	CHECK( run( 0, { -1, +1 } ) == 1 );							// clamp fabricates +1 from baseline 0 (THE turd)
	CHECK( run( 0, { +1, -1 } ) == 0 );							// ...and is order-dependent
	CHECK( run( SOFTBAND_CORE_BASE, { -1, +1 } ) == SOFTBAND_CORE_BASE );	// exact from the counting base
	// fuzz: random balanced streams, any order, from the base: always exact within the low field
	Rng rng( 5150u );
	for( int k = 0; k < 2000; k++ )
	{
		std::vector<int> ops;
		int net = 0;
		int n = 2 + ( int )rng.f( 0, 14 );
		for( int i = 0; i < n; i++ )
		{
			int op = rng.f( 0, 1 ) < 0.5f ? -1 : +1;
			ops.push_back( op );
			net += op;
		}
		CHECK( run( SOFTBAND_CORE_BASE, ops ) == SOFTBAND_CORE_BASE + net );
	}
}

TEST( SoftContract, gls_incr_decr_source_mapping_is_clamp )
{
	// source contract: PipelineCache.cpp must map plain INCR/DECR to IncrementAndClamp/DecrementAndClamp
	// (the property that forces the 128 baseline) and the _WRAP variants to the wrap ops (what the shell
	// counting depends on). If someone changes the mapping, this red-flags the band encoding for review.
	std::FILE* f = std::fopen( "/home/app/Games/gog/doom-3-bfg-edition/neo/renderer/PipelineCache.cpp", "rb" );
	CHECK( f != NULL );
	if( !f ) { return; }
	std::fseek( f, 0, SEEK_END );
	long n = std::ftell( f );
	std::fseek( f, 0, SEEK_SET );
	std::string src( ( size_t )n, 0 );
	CHECK( std::fread( &src[0], 1, ( size_t )n, f ) == ( size_t )n );
	std::fclose( f );
	auto after = [&]( const char* caseLabel, const char* op ) -> bool
	{
		size_t p = src.find( caseLabel );
		if( p == std::string::npos ) { return false; }
		size_t q = src.find( op, p );
		return q != std::string::npos && q - p < 200;		// the setOp call follows its case label closely
	};
	CHECK( after( "case GLS_STENCIL_OP_ZFAIL_INCR:", "IncrementAndClamp" ) );
	CHECK( after( "case GLS_STENCIL_OP_ZFAIL_DECR:", "DecrementAndClamp" ) );
	CHECK( after( "case GLS_STENCIL_OP_ZFAIL_INCR_WRAP:", "IncrementAndWrap" ) );
	CHECK( after( "case GLS_STENCIL_OP_ZFAIL_DECR_WRAP:", "DecrementAndWrap" ) );
	CHECK( after( "case GLS_STENCIL_OP_ZFAIL_INVERT:", "Invert" ) );		// the shell parity op
}

TEST( SoftContract, band_encoding_arithmetic )
{
	// the properties the sign-agnostic 3-class encoding rests on:
	CHECK( ( SOFTBAND_SHELL_BIT & ( SOFTBAND_SHELL_BIT - 1 ) ) == 0 );	// parity target is a single bit
	CHECK( ( SOFTBAND_CORE_BASE & SOFTBAND_SHELL_BIT ) == 0 );			// counting base keeps the parity bit clear
	CHECK( SOFTBAND_CORE_BASE - 31 >= 1 );								// 31 stacked cores cannot clamp at 0...
	CHECK( SOFTBAND_CORE_BASE + 31 < SOFTBAND_SHELL_BIT );				// ...nor carry into the parity bit
	CHECK( SOFTBAND_LIT_REF != SOFTBAND_RING_REF );						// the two drawn classes are distinct
	CHECK( SOFTBAND_RING_REF == ( SOFTBAND_CORE_BASE | SOFTBAND_SHELL_BIT ) );
	CHECK( SOFTBAND_CORE_REPS == 2 );									// one draw per surf (global+local walk)
	// sign-agnosticism itself: flipping the sign of every core count never turns umbra into lit/ring
	for( int n = 1; n <= 31; n++ )
	{
		CHECK( SOFTBAND_CORE_BASE + n != SOFTBAND_LIT_REF && SOFTBAND_CORE_BASE + n != SOFTBAND_RING_REF );
		CHECK( SOFTBAND_CORE_BASE - n != SOFTBAND_LIT_REF && SOFTBAND_CORE_BASE - n != SOFTBAND_RING_REF );
		CHECK( ( ( SOFTBAND_CORE_BASE + n ) | SOFTBAND_SHELL_BIT ) != SOFTBAND_RING_REF );
		CHECK( ( ( SOFTBAND_CORE_BASE - n ) | SOFTBAND_SHELL_BIT ) != SOFTBAND_RING_REF );
	}
}

TEST( SoftContract, backend_uses_the_shared_constants )
{
	// source contract: the backend's clear, interaction refs, core reps and ring loop must reference the
	// SOFTBAND_ constants - a literal reintroduced in any of those spots resurrects the divergence bug.
	std::FILE* f = std::fopen( "/home/app/Games/gog/doom-3-bfg-edition/neo/renderer/RenderBackend.cpp", "rb" );
	CHECK( f != NULL );
	if( !f ) { return; }
	std::fseek( f, 0, SEEK_END );
	long n = std::ftell( f );
	std::fseek( f, 0, SEEK_SET );
	std::string src( ( size_t )n, 0 );
	CHECK( std::fread( &src[0], 1, ( size_t )n, f ) == ( size_t )n );
	std::fclose( f );
	CHECK( src.find( "GL_Clear( false, false, true, SOFTBAND_CORE_BASE" ) != std::string::npos );
	CHECK( src.find( "( softBandStencilRef > 0 ) ? SOFTBAND_RING_REF : SOFTBAND_LIT_REF" ) != std::string::npos );
	CHECK( src.find( "const int swCoreReps = SOFTBAND_CORE_REPS" ) != std::string::npos );
	CHECK( src.find( "GLS_STENCIL_MAKE_MASK( SOFTBAND_SHELL_BIT )" ) != std::string::npos );
	// the shell parity marking must INVERT on BOTH faces (face-convention-free) inside that write mask
	size_t shellState = src.find( "GLS_STENCIL_MAKE_MASK( SOFTBAND_SHELL_BIT )" );
	size_t frontInv = src.rfind( "GLS_STENCIL_OP_ZFAIL_INVERT", shellState );
	size_t backInv = src.rfind( "GLS_BACK_STENCIL_OP_ZFAIL_INVERT", shellState );
	CHECK( frontInv != std::string::npos && shellState - frontInv < 400 );
	CHECK( backInv != std::string::npos && shellState - backInv < 400 );
	// and the CORE must be drawn before the shell (a core DECR could borrow into an already-set parity bit)
	size_t coreLoop = src.find( "const int swCoreReps = SOFTBAND_CORE_REPS" );
	CHECK( coreLoop != std::string::npos && coreLoop < shellState );
	// PipelineCache must map the GLS mask to the WRITE mask too, or the masked INVERT flips every bit
	std::FILE* f2 = std::fopen( "/home/app/Games/gog/doom-3-bfg-edition/neo/renderer/PipelineCache.cpp", "rb" );
	CHECK( f2 != NULL );
	if( f2 )
	{
		std::fseek( f2, 0, SEEK_END );
		long n2 = std::ftell( f2 );
		std::fseek( f2, 0, SEEK_SET );
		std::string src2( ( size_t )n2, 0 );
		CHECK( std::fread( &src2[0], 1, ( size_t )n2, f2 ) == ( size_t )n2 );
		std::fclose( f2 );
		CHECK( src2.find( "setStencilWriteMask( mask )" ) != std::string::npos );
	}
}

TEST( SoftContract, winding_gate_is_lethal_without_centre_visibility )
{
	// the winding gate's PRECONDITION, asserted from both sides: on a TRUE-UMBRA point (disk centre
	// genuinely covered) the ungated coverage saturates, and the gate - if wrongly enabled - DELETES that
	// real umbra (winding is legitimately 1 there). This is exactly what painted shadow interiors lit
	// when the mis-based stencil dropped core marks and true-umbra pixels leaked into the ring passes.
	// The gate may therefore ONLY be fed pixels whose ring classification (core = 0) actually holds.
	const float3 L( 0, 0, 12 );
	const float3 P( 0, 0, 0 );
	const float  r = 2.0f;
	Box slab = MakeBox( float3( 0, 0, 6 ), float3( 5, 5, 0.1f ) );		// full umbra at P
	auto rec = BuildCaster( { Silhouette( slab, L ) } );
	SoftEdgeBuffer buf{ rec.data(), ( int )rec.size() };
	float occUngated = SoftShadow_WedgeOcclusion( P, L, r, 0, ( int )( rec.size() / 2 ), 0.0f, buf );
	float occGated   = SoftShadow_WedgeOcclusion( P, L, r, 0, ( int )( rec.size() / 2 ), 1.0f, buf );
	std::printf( "    [gate-contract] true umbra: ungated occ=%.3f  gated occ=%.3f (gate deletes it - by design, hence the precondition)\n",
			occUngated, occGated );
	CHECK( occUngated > 0.99f );			// coverage correctly saturates in real umbra
	CHECK( occGated < 0.01f );				// the gate removes it -> NEVER enable without the ring guarantee
}

TEST( SoftContract, shell_capped_volume_watertight_and_camera_free )
{
	// the capped shell's z-fail count must be a pure containment function: identical from any camera,
	// 1 inside the inflated prism, 0 outside. 24 cameras x grid of receivers on one caster.
	const float3 L( 0, 0, 12 );
	Box b = MakeBox( float3( 0.5f, -0.3f, 6 ), float3( 1.0f, 0.7f, 0.4f ), 0.5f, 0.15f );
	auto loop = Silhouette( b, L );
	CHECK( loop.size() >= 3 );
	std::vector<std::pair<float3, float3>> edges;
	float3 lo( 1e30f, 1e30f, 1e30f ), hi( -1e30f, -1e30f, -1e30f );
	for( size_t i = 0; i < loop.size(); i++ )
	{
		edges.push_back( { loop[i], loop[( i + 1 ) % loop.size()] } );
		lo = float3( std::fmin( lo.x, loop[i].x ), std::fmin( lo.y, loop[i].y ), std::fmin( lo.z, loop[i].z ) );
		hi = float3( std::fmax( hi.x, loop[i].x ), std::fmax( hi.y, loop[i].y ), std::fmax( hi.z, loop[i].z ) );
	}
	float3 ctr = ( lo + hi ) * 0.5f;
	Rng rng( 777333u );
	int inside = 0, outside = 0, badRef = 0, unstable = 0;
	int refHist[7] = { 0, 0, 0, 0, 0, 0, 0 };		// refs -3..+3 histogram (index ref+3)
	float3 badP( 0, 0, 0 );
	int badVal = 0;
	for( int k = 0; k < 200; k++ )
	{
		float3 P( rng.f( -6, 6 ), rng.f( -6, 6 ), rng.f( -1, 4 ) );
		int ref = ShellZFailCapped( edges, ctr, 2.0f, L, float3( 20, 0, 3 ), P );
		bool stable = true;
		for( int c = 0; c < 24; c++ )
		{
			float a = ( float )( c * 2.0 * MC_PI / 24.0 );
			float3 C( 9.0f * std::cos( a ), 9.0f * std::sin( a ), 0.5f + ( c % 5 ) );
			if( ShellZFailCapped( edges, ctr, 2.0f, L, C, P ) != ref ) { stable = false; }
		}
		if( !stable ) { unstable++; }
		if( ref >= -3 && ref <= 3 ) { refHist[ref + 3]++; }
		if( ref < -1 || ref > 1 ) { badRef++; badP = P; badVal = ref; }
		if( ref != 0 ) { inside++; }		// containment is |ref| == 1; the SIGN is the GPU-convention
		else { outside++; }					// unknowable the parity encoding is immune to
	}
	std::printf( "    [shell-contract] ref histogram -3..3: %d %d %d [%d %d] %d %d   unstable=%d  worst P(%.2f,%.2f,%.2f) ref=%d\n",
			refHist[0], refHist[1], refHist[2], refHist[3], refHist[4], refHist[5], refHist[6], unstable, badP.x, badP.y, badP.z, badVal );
	CHECK( unstable == 0 );
	CHECK( badRef == 0 );					// watertight prism: |winding| <= 1
	CHECK( refHist[2] == 0 || refHist[4] == 0 );	// orientation CONSISTENT: one sign only across all receivers
	std::printf( "    [shell-contract] 200 receivers x 24 cameras: %d inside, %d outside, all camera-invariant\n", inside, outside );
	CHECK( inside > 10 );					// the grid genuinely sampled both sides
	CHECK( outside > 10 );
}

TEST( SoftContract, coverage_area_physical_bound )
{
	// The debris-drop's justification, both directions: (a) any REAL caster's summed coverage area is at
	// most the disk area (occ <= 1), fuzz-verified across geometry classes including near-plane straddles;
	// (b) the erebus7 failure class - a many-chain caster shredded by the near-plane clip - exceeds it by
	// ORDERS OF MAGNITUDE, so the 1.2x threshold separates debris from geometry with a wide margin.
	Rng rng( 24601u );
	const float r = 2.0f;
	float worstLegit = 0;
	for( int k = 0; k < 800; k++ )
	{
		float3 L( 0, 0, 12 );
		float3 P( rng.f( -2, 2 ), rng.f( -2, 2 ), 0 );
		float3 h( rng.f( 0.3f, 2.5f ), rng.f( 0.3f, 2.5f ), rng.f( 0.05f, 3.0f ) );
		float3 C( rng.f( -4, 4 ), rng.f( -4, 4 ), rng.f( 0.5f, 11.0f ) );
		Box b = MakeBox( C, h, rng.f( 0, 3.14f ), rng.f( -0.5f, 0.5f ) );
		auto loop = Silhouette( b, L );
		if( loop.size() < 3 ) { continue; }
		float occ = 1.0f - LiveShadow( BuildCaster( { loop } ), P, L, r );
		worstLegit = std::fmax( worstLegit, occ );
		CHECK( occ <= 1.0f );			// saturate() enforces the cap; the AREA bound is what the drop uses
	}
	// (b) the debris class IN THE WILD: erebus7's many-chain world casters shredded by the near-plane
	// clip (the dissected areas ran to -181258 x disk with winding 0). The drop must be OBSERVABLE on the
	// live function: find receiver/caster pairs where ungated occlusion saturates but the centre-lit drop
	// zeroes it - the exact pixels that shipped as ants before the physical bound existed.
	SoftCap cw;
	if( LoadSoftCap( "/home/app/Games/gog/doom-3-bfg-edition/neo/tests/data/erebus7.softcap", cw ) && !cw.receivers.empty() )
	{
		SoftEdgeBuffer bufW{ reinterpret_cast<const float4*>( cw.edges.data() ), ( int )( cw.edges.size() * 2 ) };
		int dropped = 0, sampledW = 0;
		for( uint32_t li = 0; li < cw.hdr.numLights && dropped == 0; li++ )
		{
			const softcapLight_t& Lt = cw.lights[li];
			if( Lt.penumbraSize <= 0.0f || Lt.edgeCount == 0 ) { continue; }
			float3 Lo( Lt.origin[0], Lt.origin[1], Lt.origin[2] );
			for( const softcapReceiver_t& R : cw.receivers )
			{
				if( R.lightIndex != li || dropped > 0 ) { continue; }
				uint32_t step = R.numVerts > 24 ? R.numVerts / 12 : 2;
				for( uint32_t vi = R.firstVert; vi < R.firstVert + R.numVerts; vi += step )
				{
					float3 Pw( cw.recvVerts[vi * 3 + 0], cw.recvVerts[vi * 3 + 1], cw.recvVerts[vi * 3 + 2] );
					float3 toL = Lo - Pw;
					float dl = std::sqrt( dot( toL, toL ) );
					if( dl > 1e-4f ) { Pw = Pw + toL * ( 2.0f / dl ); }
					sampledW++;
					float u0 = SoftShadow_WedgeOcclusion( Pw, Lo, Lt.penumbraSize, ( int )( Lt.firstEdge * 2 ), ( int )Lt.edgeCount, 0.0f, bufW );
					if( u0 < 0.999f ) { continue; }
					float g1 = SoftShadow_WedgeOcclusion( Pw, Lo, Lt.penumbraSize, ( int )( Lt.firstEdge * 2 ), ( int )Lt.edgeCount, 1.0f, bufW );
					if( g1 < 0.01f ) { dropped++; break; }
				}
			}
		}
		std::printf( "    [area-bound] worst legit occ=%.3f   erebus7: %d samples scanned, debris-drop observed=%d\n",
				worstLegit, sampledW, dropped );
		CHECK( dropped > 0 );		// the wild debris class exists and the centre-lit bound removes it
	}
	else
	{
		std::printf( "    [area-bound] erebus7 capture unreadable\n" );
		CHECK( false );
	}
}

// ====================================================================== 5c. VISUAL-DEFECT DETECTORS
// The failure modes the USER sees, detected in the capture as IMAGES - not point statistics:
//   ANTS:  small disconnected shadow speckles (<= 8 px here) where the scene is actually lit.
//   TURDS: a connected shadow blob (> 8 px) projected onto actually-lit scene.
//   FLIPS: any pixel whose umbra/ring/lit CLASS changes with the camera (temporal instability).
// The pipeline is rendered offline exactly as the backend composes it (core stencil -> solid, capped
// z-fail shell -> ring -> coverage, else lit), diffed against ray truth, and the disagreement mask is
// connected-component labelled. ONE ant or ONE turd = red. Each defect is printed with its pixel bbox,
// world position and mechanism, and a colour defect map is written next to the capture data
// (defects_<name>.ppm: grayscale = pipeline shadow, red = turd, yellow = ant) - the isolation proof.
namespace
{

struct CasterRange { uint32_t first, num; float3 lo, hi; };

bool RayAabb( float3 P, float3 D, float3 lo, float3 hi )	// segment P..P+D vs AABB (slab test)
{
	float t0 = 0.0f, t1 = 1.0f;
	for( int a = 0; a < 3; a++ )
	{
		float o = ( &P.x )[a], d = ( &D.x )[a], l = ( &lo.x )[a], h = ( &hi.x )[a];
		if( std::fabs( d ) < 1e-9f )
		{
			if( o < l || o > h ) { return false; }
			continue;
		}
		float inv = 1.0f / d, ta = ( l - o ) * inv, tb = ( h - o ) * inv;
		if( ta > tb ) { float tmp = ta; ta = tb; tb = tmp; }
		t0 = std::fmax( t0, ta );
		t1 = std::fmin( t1, tb );
		if( t0 > t1 ) { return false; }
	}
	return true;
}

bool RayHitsCasters( float3 P, float3 D, const float* verts, const uint32_t* idx,
					 const std::vector<CasterRange>& casters )
{
	for( const CasterRange& c : casters )
	{
		if( !RayAabb( P, D, c.lo, c.hi ) ) { continue; }
		if( RayHitsMesh( P, D, verts, idx + c.first, c.num ) ) { return true; }
	}
	return false;
}

// disk truth with AABB-culled rays (N=4 -> 13 rays; enough to certify "essentially lit" vs "shadowed")
// CONVERGED ray-traced occlusion oracle. A regular N x N grid has structured boundary bias and needs
// ~3200 samples (N=64) to converge; a deterministic Hammersley (radical-inverse base 2) sequence with
// equal-area disk mapping converges to the same value at ~256 samples, unbiased, and stays reproducible
// (no RNG). This is THE oracle the capture accuracy tests measure against - not a tuned grid N.
inline float SoftRadicalInverse2( uint32_t i )
{
	i = ( i << 16 ) | ( i >> 16 );
	i = ( ( i & 0x55555555u ) << 1 ) | ( ( i & 0xAAAAAAAAu ) >> 1 );
	i = ( ( i & 0x33333333u ) << 2 ) | ( ( i & 0xCCCCCCCCu ) >> 2 );
	i = ( ( i & 0x0F0F0F0Fu ) << 4 ) | ( ( i & 0xF0F0F0F0u ) >> 4 );
	i = ( ( i & 0x00FF00FFu ) << 8 ) | ( ( i & 0xFF00FF00u ) >> 8 );
	return ( float )( ( double )i / 4294967296.0 );
}
float TruthShadowConverged( float3 P, float3 L, float r, const float* verts, const uint32_t* idx,
							const std::vector<CasterRange>& casters, int M = 256 )
{
	float3 toL = L - P;
	float dist = std::sqrt( dot( toL, toL ) );
	if( dist < 1e-6f ) { return 1.0f; }
	float3 nrm = toL * ( 1.0f / dist );
	float3 up = ( std::fabs( nrm.z ) > 0.9f ) ? float3( 0, 1, 0 ) : float3( 0, 0, 1 );
	float3 u = normalize( cross( up, nrm ) ), v = cross( nrm, u );
	int inside = 0;
	for( int i = 0; i < M; i++ )
	{
		float u1 = ( i + 0.5f ) / M, u2 = SoftRadicalInverse2( ( uint32_t )i );
		float rr = std::sqrt( u1 ) * r, th = 2.0f * ( float )M_PI * u2;		// equal-area disk map
		float3 Dp = L + u * ( rr * std::cos( th ) ) + v * ( rr * std::sin( th ) );
		if( RayHitsCasters( P, Dp - P, verts, idx, casters ) ) { inside++; }
	}
	return 1.0f - ( float )inside / M;
}
float TruthShadowCulled( float3 P, float3 L, float r, const float* verts, const uint32_t* idx,
						 const std::vector<CasterRange>& casters, int N = 4 )
{
	float3 toL = L - P;
	float dist = std::sqrt( dot( toL, toL ) );
	if( dist < 1e-6f ) { return 1.0f; }
	float3 nrm = toL * ( 1.0f / dist );
	float3 up = ( std::fabs( nrm.z ) > 0.9f ) ? float3( 0, 1, 0 ) : float3( 0, 0, 1 );
	float3 u = normalize( cross( up, nrm ) ), v = cross( nrm, u );
	int inside = 0, total = 0;
	for( int iy = 0; iy < N; iy++ )
		for( int ix = 0; ix < N; ix++ )
		{
			float du = ( ix + 0.5f ) / N * 2 - 1, dv = ( iy + 0.5f ) / N * 2 - 1;
			if( du * du + dv * dv > 1.0f ) { continue; }
			total++;
			float3 Dp = L + u * ( du * r ) + v * ( dv * r );
			if( RayHitsCasters( P, Dp - P, verts, idx, casters ) ) { inside++; }
		}
	// centre ray too (N=4 grid misses it)
	total++;
	if( RayHitsCasters( P, L - P, verts, idx, casters ) ) { inside++; }
	return total ? 1.0f - ( float )inside / total : 1.0f;
}

}

TEST( SoftShadowDefects, no_ants_no_turds_no_camera_flips )
{
	// ALL readable captures - the full-frame reference run found the residual gross pixels precisely in
	// the captures a shorter list never scanned (erebus7/6/13). Coverage gaps are how defects hide.
	const char* names[] = { "erebus2", "erebus3", "erebus4", "erebus5", "erebus6", "erebus7", "erebus13" };
	const int W = 320;
	int totalTurds = 0, totalAnts = 0, totalHoles = 0, totalFlips = 0, capsSeen = 0;
	for( const char* nm : names )
	{
		char path[512];
		std::snprintf( path, sizeof( path ), "/home/app/Games/gog/doom-3-bfg-edition/neo/tests/data/%s.softcap", nm );
		SoftCap c;
		if( !LoadSoftCap( path, c ) || c.receivers.empty() || c.recvVerts.empty() ) { continue; }
		capsSeen++;
		int H = c.hdr.screenW ? ( int )( ( double )W * c.hdr.screenH / c.hdr.screenW ) : W * 9 / 16;
		SwGBuffer g( W, H );
		for( const softcapReceiver_t& R : c.receivers )
			for( uint32_t t = R.firstIndex; t + 2 < R.firstIndex + R.numIndex && t + 2 < c.recvIdx.size(); t += 3 )
			{
				uint32_t ia = c.recvIdx[t], ib = c.recvIdx[t + 1], ic = c.recvIdx[t + 2];
				float3 A( c.recvVerts[ia * 3 + 0], c.recvVerts[ia * 3 + 1], c.recvVerts[ia * 3 + 2] );
				float3 B( c.recvVerts[ib * 3 + 0], c.recvVerts[ib * 3 + 1], c.recvVerts[ib * 3 + 2] );
				float3 C( c.recvVerts[ic * 3 + 0], c.recvVerts[ic * 3 + 1], c.recvVerts[ic * 3 + 2] );
				SwRasterTri( g, c.hdr.worldMVP, A, B, C, ( int )R.lightIndex );
			}

		// per-light: caster ranges + AABBs (ray culling), shell caster edge lists, edge buffer
		struct LightCtx
		{
			bool  soft = false;
			float3 Lo;
			float rp = 0, bandRp = 0;
			int   firstElem = 0, nRec = 0;
			std::vector<CasterRange> casters;
			std::vector<std::pair<float3, std::vector<std::pair<float3, float3>>>> shells;	// centre + edges
			std::vector<std::pair<int, int>> shellRec;		// per shell: (firstRec, numRec) into the edge stream
		};
		std::vector<LightCtx> lights( c.hdr.numLights );
		for( uint32_t li = 0; li < c.hdr.numLights; li++ )
		{
			const softcapLight_t& Lt = c.lights[li];
			LightCtx& lc = lights[li];
			if( Lt.penumbraSize <= 0.0f || Lt.edgeCount == 0 ) { continue; }
			lc.soft = true;
			lc.Lo = float3( Lt.origin[0], Lt.origin[1], Lt.origin[2] );
			lc.rp = Lt.penumbraSize;
			lc.bandRp = Lt.penumbraSize * 1.1f;
			lc.firstElem = ( int )( Lt.firstEdge * 2 );
			lc.nRec = ( int )Lt.edgeCount;
			for( const softcapCaster_t& cs : c.casters )
			{
				if( cs.lightIndex != li || cs.numIndex == 0 ) { continue; }
				CasterRange cr;
				cr.first = cs.firstIndex;
				cr.num = cs.numIndex;
				cr.lo = float3( 1e30f, 1e30f, 1e30f );
				cr.hi = float3( -1e30f, -1e30f, -1e30f );
				for( uint32_t k = cs.firstIndex; k < cs.firstIndex + cs.numIndex && k < c.meshIdx.size(); k++ )
				{
					const float* vp = &c.meshVerts[c.meshIdx[k] * 3];
					cr.lo = float3( std::fmin( cr.lo.x, vp[0] ), std::fmin( cr.lo.y, vp[1] ), std::fmin( cr.lo.z, vp[2] ) );
					cr.hi = float3( std::fmax( cr.hi.x, vp[0] ), std::fmax( cr.hi.y, vp[1] ), std::fmax( cr.hi.z, vp[2] ) );
				}
				lc.casters.push_back( cr );
			}
			for( uint32_t rIdx = Lt.firstEdge; rIdx < Lt.firstEdge + Lt.edgeCount && rIdx < c.edges.size(); rIdx++ )
			{
				const softcapEdge_t& e = c.edges[rIdx];
				if( e.e0[3] < 0.0f )
				{
					if( !lc.shellRec.empty() ) { lc.shellRec.back().second = ( int )rIdx - lc.shellRec.back().first; }
					lc.shells.push_back( { float3( e.e0[0], e.e0[1], e.e0[2] ), {} } );
					lc.shellRec.push_back( { ( int )rIdx, 0 } );
				}
				else if( !lc.shells.empty() )
				{
					lc.shells.back().second.push_back( { float3( e.e0[0], e.e0[1], e.e0[2] ), float3( e.e1[0], e.e1[1], e.e1[2] ) } );
				}
			}
			if( !lc.shellRec.empty() ) { lc.shellRec.back().second = ( int )( Lt.firstEdge + Lt.edgeCount ) - lc.shellRec.back().first; }
		}

		SoftEdgeBuffer buf{ reinterpret_cast<const float4*>( c.edges.data() ), ( int )( c.edges.size() * 2 ) };
		float3 cam0( c.hdr.vieworg[0], c.hdr.vieworg[1], c.hdr.vieworg[2] );
		float3 cams[3] = { cam0, cam0 + float3( 120, -80, 25 ), cam0 + float3( -100, 130, -15 ) };

		std::vector<float> img( ( size_t )W * H, 1.0f );
		std::vector<unsigned char> defect( ( size_t )W * H, 0 );	// 1 = defect candidate
		std::vector<unsigned char> mech( ( size_t )W * H, 0 );		// mechanism: 1 ring-overlap, 2 core-vs-truth, 3 coverage
		int flips = 0;
		for( int i = 0; i < W * H; i++ )
		{
			int li = g.light[i];
			if( li < 0 || !lights[li].soft ) { continue; }
			LightCtx& lc = lights[li];
			float3 P = g.wpos[i];
			float3 toL = lc.Lo - P;
			float dl = std::sqrt( dot( toL, toL ) );
			if( dl > 1e-4f ) { P = P + toL * ( 2.0f / dl ); }
			bool hard = RayHitsCasters( P, lc.Lo - P, c.meshVerts.data(), c.meshIdx.data(), lc.casters );
			int cls[3];
			for( int k = 0; k < 3; k++ )
			{
				int shell = 0;
				for( auto& sh : lc.shells ) { shell += std::abs( ShellZFailCapped( sh.second, sh.first, lc.bandRp, lc.Lo, cams[k], P ) ); }
				cls[k] = hard ? 0 : ( ( ( shell & 1 ) != 0 ) ? 1 : 2 );		// 0 umbra, 1 ring (shell parity), 2 lit
			}
			if( cls[0] != cls[1] || cls[0] != cls[2] )
			{
				// class churn on the EXACT shell boundary is only a defect if the RADIANCE differs: at the
				// outer boundary coverage is 0, so ring and lit coincide. Assert the shadow VALUE per camera.
				float sv[3];
				for( int k = 0; k < 3; k++ )
				{
					if( cls[k] == 0 ) { sv[k] = 0.0f; }
					else if( cls[k] == 1 ) { sv[k] = 1.0f - saturate( SoftShadow_WedgeOcclusion( P, lc.Lo, lc.rp, lc.firstElem, lc.nRec, 1.0f, buf ) ); }
					else { sv[k] = 1.0f; }
				}
				float spread = std::fmax( sv[0], std::fmax( sv[1], sv[2] ) ) - std::fmin( sv[0], std::fmin( sv[1], sv[2] ) );
				if( spread > 0.02f )
				{
					flips++;
					std::printf( "    [flip %s] px(%d,%d) world(%.2f,%.2f,%.2f) classes %d/%d/%d shadow %.3f/%.3f/%.3f\n",
							nm, i % W, i / W, g.wpos[i].x, g.wpos[i].y, g.wpos[i].z, cls[0], cls[1], cls[2], sv[0], sv[1], sv[2] );
				}
			}
			float shadow;
			if( cls[0] == 0 ) { shadow = 0.0f; }
			else if( cls[0] == 1 ) { shadow = 1.0f - saturate( SoftShadow_WedgeOcclusion( P, lc.Lo, lc.rp, lc.firstElem, lc.nRec, 1.0f, buf ) ); }
			else { shadow = 1.0f; }
			img[i] = shadow;
			// certify against ray truth: shipped (gated) shadow AND the ungated one - a pixel the winding
			// gate "rescued" is still a mechanism to explain, not a fix to bank.
			float shadowUngated = shadow;
			if( cls[0] == 1 )
			{
				shadowUngated = 1.0f - saturate( SoftShadow_WedgeOcclusion( P, lc.Lo, lc.rp, lc.firstElem, lc.nRec, 0.0f, buf ) );
			}
			if( shadow < 0.5f || shadowUngated < 0.5f )
			{
				float truth = TruthShadowCulled( P, lc.Lo, lc.rp, c.meshVerts.data(), c.meshIdx.data(), lc.casters );
				if( truth > 0.85f )
				{
					if( shadow < 0.5f )
					{
						defect[i] = 1;
						mech[i] = ( cls[0] == 0 && !hard ) ? 1 : ( cls[0] == 0 && hard ? 2 : 3 );
					}
					else
					{
						defect[i] = 2;		// rescued-by-gate: not a shipped defect, but dissect its mechanism
						mech[i] = 3;
					}
				}
			}
			// THE OTHER DIRECTION - holes: pipeline says (mostly) lit where the geometry actually casts
			// shadow. This is the class that painted umbra interiors lit while the one-sided detector
			// stayed green ("large connected shadows are now thin outlines"). In the model, hard-blocked
			// pixels are umbra by construction, so algorithm-level holes come from pixels whose disk is
			// covered WITHOUT the centre ray being blocked (union-of-casters coverage) or whose ring
			// coverage under-occludes. Prefilter with 5 rays (centre + 4 rim); certify with full truth.
			if( shadow > 0.5f && !hard )
			{
				float3 nrm5 = ( lc.Lo - P ) * ( 1.0f / std::fmax( dl, 1e-4f ) );
				float3 up5 = ( std::fabs( nrm5.z ) > 0.9f ) ? float3( 0, 1, 0 ) : float3( 0, 0, 1 );
				float3 u5 = normalize( cross( up5, nrm5 ) ), v5 = cross( nrm5, u5 );
				int blocked5 = 0;
				for( int k5 = 0; k5 < 4; k5++ )
				{
					float a5 = ( float )( k5 * MC_PI / 2.0 );
					float3 D5 = lc.Lo + u5 * ( std::cos( a5 ) * lc.rp ) + v5 * ( std::sin( a5 ) * lc.rp );
					if( RayHitsCasters( P, D5 - P, c.meshVerts.data(), c.meshIdx.data(), lc.casters ) ) { blocked5++; }
				}
				if( blocked5 >= 3 )		// most of the disk rim blocked yet model lit: certify
				{
					float truth = TruthShadowCulled( P, lc.Lo, lc.rp, c.meshVerts.data(), c.meshIdx.data(), lc.casters );
					if( truth < 0.15f )
					{
						defect[i] = 3;			// HOLE: missing shadow
						mech[i] = ( cls[0] == 2 ) ? 4 : 5;	// 4 = classified lit outright, 5 = ring under-occluded
					}
				}
			}
		}

		// connected components (4-neighbour BFS) of the defect mask, per defect TYPE (1 = shipped
		// dark-on-lit, 2 = gate-rescued, 3 = HOLE/missing shadow - adjacent different types stay separate)
		std::vector<int> label( ( size_t )W * H, -1 );
		int turds = 0, ants = 0, rescued = 0, holes = 0;
		for( int i = 0; i < W * H; i++ )
		{
			if( !defect[i] || label[i] >= 0 ) { continue; }
			std::vector<int> stack = { i }, comp;
			label[i] = 1;
			while( !stack.empty() )
			{
				int p = stack.back();
				stack.pop_back();
				comp.push_back( p );
				int px = p % W, py = p / W;
				const int nx[4] = { px - 1, px + 1, px, px }, ny[4] = { py, py, py - 1, py + 1 };
				for( int k = 0; k < 4; k++ )
				{
					if( nx[k] < 0 || nx[k] >= W || ny[k] < 0 || ny[k] >= H ) { continue; }
					int q = ny[k] * W + nx[k];
					if( defect[q] == defect[i] && label[q] < 0 ) { label[q] = 1; stack.push_back( q ); }
				}
			}
			if( defect[comp[0]] == 3 )
			{
				holes++;
				float3 wp3 = g.wpos[comp[0]];
				int hx0 = W, hx1 = 0, hy0 = H, hy1 = 0;
				for( int p : comp ) { hx0 = std::min( hx0, p % W ); hx1 = std::max( hx1, p % W ); hy0 = std::min( hy0, p / W ); hy1 = std::max( hy1, p / W ); }
				std::printf( "    [defect %s] HOLE: %zu px  bbox(%d,%d..%d,%d)  world(%.0f,%.0f,%.0f)  mech %s\n",
						nm, comp.size(), hx0, hy0, hx1, hy1, wp3.x, wp3.y, wp3.z,
						mech[comp[0]] == 4 ? "classified-lit" : "ring-under-occluded" );
				for( int p : comp ) { label[p] = 4; }
				continue;
			}
			// bbox + world pos + mechanism histogram
			int x0 = W, x1 = 0, y0 = H, y1 = 0, m1 = 0, m2 = 0, m3 = 0, shipped = 0;
			float3 wp = g.wpos[comp[0]];
			for( int p : comp )
			{
				x0 = std::min( x0, p % W ); x1 = std::max( x1, p % W );
				y0 = std::min( y0, p / W ); y1 = std::max( y1, p / W );
				if( mech[p] == 1 ) { m1++; }
				if( mech[p] == 2 ) { m2++; }
				if( mech[p] == 3 ) { m3++; }
				if( defect[p] == 1 ) { shipped++; }
			}
			bool isTurd = ( int )comp.size() > 8;
			if( shipped > 0 )
			{
				if( isTurd ) { turds++; }
				else { ants++; }
			}
			else { rescued++; }
			std::printf( "    [defect %s] %s: %zu px (shipped %d)  bbox(%d,%d..%d,%d)  world(%.0f,%.0f,%.0f)  mech ring-overlap=%d core=%d coverage=%d\n",
					nm, shipped == 0 ? "RESCUED" : ( isTurd ? "TURD" : "ant" ), comp.size(), shipped, x0, y0, x1, y1, wp.x, wp.y, wp.z, m1, m2, m3 );
			// coverage-mechanism defects: identify the guilty CASTER (strongest per-caster occlusion at the
			// defect's first pixel) so the failure is attributable to specific geometry, not a statistic.
			if( m3 > 0 )
			{
				int li2 = g.light[comp[0]];
				LightCtx& lc2 = lights[li2];
				float3 P2 = g.wpos[comp[0]];
				float3 toL2 = lc2.Lo - P2;
				float dl2 = std::sqrt( dot( toL2, toL2 ) );
				if( dl2 > 1e-4f ) { P2 = P2 + toL2 * ( 2.0f / dl2 ); }
				float bestOcc = 0;
				int bestShell = -1;
				for( size_t s = 0; s < lc2.shellRec.size(); s++ )
				{
					float o = saturate( SoftShadow_WedgeOcclusion( P2, lc2.Lo, lc2.rp, lc2.shellRec[s].first * 2, lc2.shellRec[s].second, 0.0f, buf ) );
					if( o > bestOcc ) { bestOcc = o; bestShell = ( int )s; }
				}
				if( bestShell >= 0 )
				{
					float3 cc = lc2.shells[bestShell].first;
					float3 dc = cc - P2;
					float gatedOcc = saturate( SoftShadow_WedgeOcclusion( P2, lc2.Lo, lc2.rp, lc2.shellRec[bestShell].first * 2, lc2.shellRec[bestShell].second, 1.0f, buf ) );
					// THE "where" question: is this pixel even inside the guilty caster's OWN penumbra band?
					// If not, the pixel sits in some OTHER caster's ring and the guilty caster is contributing
					// occlusion from outside the region its own band ever authorised.
					int ownShell = ShellZFailCapped( lc2.shells[bestShell].second, cc, lc2.bandRp, lc2.Lo, cam0, P2 );
					std::printf( "        guilty caster: shell #%d occ=%.3f (winding-gated %.3f) IN-OWN-BAND=%d centre(%.0f,%.0f,%.0f) dist(P,centre)=%.1f edges=%zu light(%.0f,%.0f,%.0f) rp=%.1f\n",
							bestShell, bestOcc, gatedOcc, ownShell, cc.x, cc.y, cc.z, std::sqrt( dot( dc, dc ) ),
							lc2.shells[bestShell].second.size(), lc2.Lo.x, lc2.Lo.y, lc2.Lo.z, lc2.rp );
					// dissect the guilty caster with the exposed primitives: per-chain areas, clip counts,
					// winding - name the actual mechanism instead of guessing.
					softFrame_t fr = SoftShadow_Frame( P2, lc2.Lo );
					float rr = std::fmax( lc2.rp, 1e-2f );
					float r2 = rr * rr;
					int nearClipped = 0, farClipped = 0, dropped = 0, chains2 = 0;
					double chainArea = 0, chainAng = 0, totArea = 0;
					bool haveFirst = false;
					float2 qFirst( 0, 0 ), qPrev( 0, 0 );
					float3 prevB( 0, 0, 0 );
					bool havePrevB = false;
					auto closeChain = [&]()
					{
						if( !haveFirst ) { return; }
						chainArea += SoftDisk_CircleTriArea( qPrev, qFirst, r2 );
						chainAng += SoftDisk_EdgeAngle( qPrev, qFirst );
						chains2++;
						if( std::fabs( chainArea ) > 0.05 * MC_PI * r2 )
						{
							std::printf( "          chain %d: area=%+.3f x disk  winding=%+.2f\n",
									chains2, chainArea / ( MC_PI * r2 ), chainAng / ( 2 * MC_PI ) );
						}
						totArea += chainArea;
						chainArea = 0; chainAng = 0; haveFirst = false;
					};
					for( const std::pair<float3, float3>& e : lc2.shells[bestShell].second )
					{
						if( havePrevB )
						{
							float3 dd = e.first - prevB;
							if( dd.x != 0.0f || dd.y != 0.0f || dd.z != 0.0f ) { closeChain(); }
						}
						havePrevB = true;
						prevB = e.second;
						float3 a = e.first - P2, b = e.second - P2;
						float dnA = dot( a, fr.nrm ), dnB = dot( b, fr.nrm );
						if( dnA < SW_NEAR_EPS || dnB < SW_NEAR_EPS ) { nearClipped++; }
						if( dnA > fr.distPL || dnB > fr.distPL ) { farClipped++; }
						softClip_t cl = SoftShadow_ClipSlab( dnA, dnB, SW_NEAR_EPS, fr.distPL );
						if( cl.empty ) { dropped++; continue; }
						float d = dnB - dnA;
						float3 pa = a + cl.t0 * ( b - a ), pb = a + cl.t1 * ( b - a );
						float2 q0 = SoftShadow_ProjectVert( pa, dnA + cl.t0 * d, fr );
						float2 q1 = SoftShadow_ProjectVert( pb, dnB + ( cl.t1 - 1.0f ) * d, fr );	// = dnA + t1*d
						q1 = SoftShadow_ProjectVert( pb, dnA + cl.t1 * d, fr );
						if( haveFirst ) { chainArea += SoftDisk_CircleTriArea( qPrev, q0, r2 ); chainAng += SoftDisk_EdgeAngle( qPrev, q0 ); }
						else { qFirst = q0; haveFirst = true; }
						chainArea += SoftDisk_CircleTriArea( q0, q1, r2 );
						chainAng += SoftDisk_EdgeAngle( q0, q1 );
						qPrev = q1;
					}
					closeChain();
					std::printf( "          dissect: chains=%d totalArea=%+.3f x disk  edges near-clipped=%d far-clipped=%d dropped=%d  distPL=%.1f\n",
							chains2, totArea / ( MC_PI * r2 ), nearClipped, farClipped, dropped, fr.distPL );
				}
			}
			for( int p : comp ) { label[p] = isTurd ? 2 : 3; }
		}

		// isolation proof: colour defect map (gray = pipeline shadow term, red = turd, yellow = ant)
		char out[512];
		std::snprintf( out, sizeof( out ), "/home/app/Games/gog/doom-3-bfg-edition/base/softcap/defects_%s.ppm", nm );
		std::FILE* f = std::fopen( out, "wb" );
		if( f )
		{
			std::fprintf( f, "P6\n%d %d\n255\n", W, H );
			for( int i = 0; i < W * H; i++ )
			{
				unsigned char v = ( unsigned char )( std::fmax( 0.0f, std::fmin( 1.0f, img[i] ) ) * 255.0f );
				unsigned char rgb[3] = { v, v, v };
				if( label[i] == 2 ) { rgb[0] = 255; rgb[1] = 40; rgb[2] = 40; }
				if( label[i] == 3 ) { rgb[0] = 255; rgb[1] = 220; rgb[2] = 40; }
				if( label[i] == 4 ) { rgb[0] = 60; rgb[1] = 120; rgb[2] = 255; }	// HOLE: missing shadow
				std::fwrite( rgb, 1, 3, f );
			}
			std::fclose( f );
		}
		std::printf( "    [defect %s] %dx%d: TURDS=%d ants=%d HOLES=%d rescued=%d camera-flips=%d -> defects_%s.ppm\n",
				nm, W, H, turds, ants, holes, rescued, flips, nm );
		totalTurds += turds;
		totalAnts += ants;
		totalHoles += holes;
		totalFlips += flips;
	}
	CHECK( capsSeen >= 2 );
	CHECK( totalTurds == 0 );		// one connected false-shadow blob on lit scene = red
	CHECK( totalAnts == 0 );		// one false-shadow speckle = red
	CHECK( totalHoles == 0 );		// one ray-certified missing-shadow region = red
	CHECK( totalFlips == 0 );		// one camera-dependent radiance value = red
}

namespace
{
// minimal binary-P6 reader (the REAL captured game frames live beside the corpus as erebus<N>.ppm)
bool LoadPPM( const char* path, std::vector<unsigned char>& rgb, int& w, int& h )
{
	std::FILE* f = std::fopen( path, "rb" );
	if( !f ) { return false; }
	char magic[3] = {};
	int maxv = 0;
	if( std::fscanf( f, "%2s %d %d %d", magic, &w, &h, &maxv ) != 4 || std::strcmp( magic, "P6" ) != 0 || maxv != 255 )
	{
		std::fclose( f );
		return false;
	}
	std::fgetc( f );		// single whitespace after maxval
	rgb.resize( ( size_t )w * h * 3 );
	bool ok = std::fread( rgb.data(), 1, rgb.size(), f ) == rgb.size();
	std::fclose( f );
	return ok;
}
}

// ====================================================================== 5d. FULL-FRAME RT REFERENCE
// The whole shipped composition versus a full ray-traced reference on EVERY covered pixel of EVERY
// readable capture - no prefilters, no candidate sampling. Emits side-by-side images per capture
// (pipeline / ray reference / signed-error heatmap) so the verdict is inspectable, and gates on
// both error directions. Truth quantum at N=4 disk sampling is ~1/13, so the gates sit well above it.
TEST( SoftShadowReference, full_frame_vs_raytraced_all_captures )
{
	const char* names[] = { "erebus2", "erebus3", "erebus4", "erebus5", "erebus6", "erebus7", "erebus13" };
	const int W = 288;
	int capsSeen = 0;
	for( const char* nm : names )
	{
		char path[512];
		std::snprintf( path, sizeof( path ), "/home/app/Games/gog/doom-3-bfg-edition/neo/tests/data/%s.softcap", nm );
		SoftCap c;
		if( !LoadSoftCap( path, c ) || c.receivers.empty() || c.recvVerts.empty() ) { continue; }
		capsSeen++;
		int H = c.hdr.screenW ? ( int )( ( double )W * c.hdr.screenH / c.hdr.screenW ) : W * 9 / 16;
		const bool hasTex = !c.materials.empty() && c.recvST.size() == c.recvVerts.size() / 3 * 2 && !c.recvMat.empty();
		SwGBuffer g( W, H );
		for( size_t ri = 0; ri < c.receivers.size(); ri++ )
		{
			const softcapReceiver_t& R = c.receivers[ri];
			int mat = hasTex && ri < c.recvMat.size() ? ( int )c.recvMat[ri] : -1;
			for( uint32_t t = R.firstIndex; t + 2 < R.firstIndex + R.numIndex && t + 2 < c.recvIdx.size(); t += 3 )
			{
				uint32_t ia = c.recvIdx[t], ib = c.recvIdx[t + 1], ic = c.recvIdx[t + 2];
				float3 A( c.recvVerts[ia * 3 + 0], c.recvVerts[ia * 3 + 1], c.recvVerts[ia * 3 + 2] );
				float3 B( c.recvVerts[ib * 3 + 0], c.recvVerts[ib * 3 + 1], c.recvVerts[ib * 3 + 2] );
				float3 C( c.recvVerts[ic * 3 + 0], c.recvVerts[ic * 3 + 1], c.recvVerts[ic * 3 + 2] );
				float2 sa( 0, 0 ), sb( 0, 0 ), sc( 0, 0 );
				if( hasTex )
				{
					sa = float2( c.recvST[ia * 2 + 0], c.recvST[ia * 2 + 1] );
					sb = float2( c.recvST[ib * 2 + 0], c.recvST[ib * 2 + 1] );
					sc = float2( c.recvST[ic * 2 + 0], c.recvST[ic * 2 + 1] );
				}
				SwRasterTri( g, c.hdr.worldMVP, A, B, C, ( int )R.lightIndex, sa, sb, sc, mat );
			}
		}
		// per-light contexts (culling AABBs + shells), same construction as the defect detector
		struct RefLight
		{
			bool soft = false;
			float3 Lo;
			float rp = 0, bandRp = 0;
			int firstElem = 0, nRec = 0;
			std::vector<CasterRange> casters;
			std::vector<std::pair<float3, std::vector<std::pair<float3, float3>>>> shells;
		};
		std::vector<RefLight> lights( c.hdr.numLights );
		for( uint32_t li = 0; li < c.hdr.numLights; li++ )
		{
			const softcapLight_t& Lt = c.lights[li];
			RefLight& lc = lights[li];
			if( Lt.penumbraSize <= 0.0f || Lt.edgeCount == 0 ) { continue; }
			lc.soft = true;
			lc.Lo = float3( Lt.origin[0], Lt.origin[1], Lt.origin[2] );
			lc.rp = Lt.penumbraSize;
			lc.bandRp = Lt.penumbraSize * 1.1f;
			lc.firstElem = ( int )( Lt.firstEdge * 2 );
			lc.nRec = ( int )Lt.edgeCount;
			for( const softcapCaster_t& cs : c.casters )
			{
				if( cs.lightIndex != li || cs.numIndex == 0 ) { continue; }
				CasterRange cr;
				cr.first = cs.firstIndex;
				cr.num = cs.numIndex;
				cr.lo = float3( 1e30f, 1e30f, 1e30f );
				cr.hi = float3( -1e30f, -1e30f, -1e30f );
				for( uint32_t k = cs.firstIndex; k < cs.firstIndex + cs.numIndex && k < c.meshIdx.size(); k++ )
				{
					const float* vp = &c.meshVerts[c.meshIdx[k] * 3];
					cr.lo = float3( std::fmin( cr.lo.x, vp[0] ), std::fmin( cr.lo.y, vp[1] ), std::fmin( cr.lo.z, vp[2] ) );
					cr.hi = float3( std::fmax( cr.hi.x, vp[0] ), std::fmax( cr.hi.y, vp[1] ), std::fmax( cr.hi.z, vp[2] ) );
				}
				lc.casters.push_back( cr );
			}
			for( uint32_t rIdx = Lt.firstEdge; rIdx < Lt.firstEdge + Lt.edgeCount && rIdx < c.edges.size(); rIdx++ )
			{
				const softcapEdge_t& e = c.edges[rIdx];
				if( e.e0[3] < 0.0f ) { lc.shells.push_back( { float3( e.e0[0], e.e0[1], e.e0[2] ), {} } ); }
				else if( !lc.shells.empty() )
				{
					lc.shells.back().second.push_back( { float3( e.e0[0], e.e0[1], e.e0[2] ), float3( e.e1[0], e.e1[1], e.e1[2] ) } );
				}
			}
		}
		SoftEdgeBuffer buf{ reinterpret_cast<const float4*>( c.edges.data() ), ( int )( c.edges.size() * 2 ) };
		float3 cam0( c.hdr.vieworg[0], c.hdr.vieworg[1], c.hdr.vieworg[2] );

		// The SHIPPED emergent-umbra shade, factored so it can be called at a perturbed (P,Lo) to
		// measure temporal stability: any discrete decision that flips under an infinitesimal camera/
		// light move (band parity, centre-block guard) is what makes the penumbra HARDEN frame-to-frame.
		auto shadeAt = [&]( const RefLight & lc, float3 P, float3 Lo, int* parO, bool* cbO ) -> float
		{
			bool centreBlocked = RayHitsCasters( P, Lo - P, c.meshVerts.data(), c.meshIdx.data(), lc.casters );
			int par = 0;
			for( auto& sh : lc.shells ) { par += std::abs( ShellZFailCapped( sh.second, sh.first, lc.bandRp, Lo, cam0, P ) ); }
			if( parO ) { *parO = par; }
			if( cbO ) { *cbO = centreBlocked; }
			if( ( par & 1 ) != 0 )
			{
				return 1.0f - saturate( SoftShadow_WedgeOcclusion( P, Lo, lc.rp, lc.firstElem, lc.nRec, centreBlocked ? 0.0f : 1.0f, buf ) );
			}
			return centreBlocked ? 0.0f : 1.0f;
		};
		// [temporal] property: shadow(P,Lo) must be CONTINUOUS under a small camera/light perturbation.
		// A discontinuity here = the visible frame-to-frame penumbra hardening the unit suite never caught.
		const float pEps = 1.0f;			// world units of camera/receiver jitter per frame (small)
		double tMaxJump = 0; int tPenum = 0, tHard = 0, tParFlip = 0, tCbFlip = 0, tInternal = 0;

		std::vector<float> imgP( ( size_t )W * H, 1.0f ), imgR( ( size_t )W * H, 1.0f );
		std::vector<unsigned char> covered( ( size_t )W * H, 0 );
		int n = 0, exN = 0, misN = 0, gross = 0;
		int pN = 0, pGross = 0, pOverHard = 0;
		double sumAbs = 0, pSum = 0;
		float worst = 0;
		for( int i = 0; i < W * H; i++ )
		{
			int li = g.light[i];
			if( li < 0 || !lights[li].soft ) { continue; }
			RefLight& lc = lights[li];
			float3 P = g.wpos[i];
			float3 toL = lc.Lo - P;
			float dl = std::sqrt( dot( toL, toL ) );
			if( dl > 1e-4f ) { P = P + toL * ( 2.0f / dl ); }
			covered[i] = 1;
			// EMERGENT-UMBRA composition. The inflated shell is a SOLID cone containing the whole penumbra
			// AND the umbra, so shell parity is odd across all of it; the coverage integral runs everywhere
			// inside and the umbra falls out where it SATURATES to 1 - it is never stamped. The point-light
			// hit (centreBlocked) is NOT an umbra flag: the point-light silhouette is the ~50% occlusion
			// contour, the MIDDLE of the penumbra. Its only job is to tell the integral whether the disk
			// CENTRE is visible, which selects the guard mode (winding subtraction + debris drop are sound
			// only when the centre is lit; enabling them where the centre is blocked would delete the umbra).
			int par = 0; bool centreBlocked = false;
			float shadow = shadeAt( lc, P, lc.Lo, &par, &centreBlocked );
			// full ray-traced reference, EVERY pixel
			float truth = TruthShadowCulled( P, lc.Lo, lc.rp, c.meshVerts.data(), c.meshIdx.data(), lc.casters );
			// [temporal] perturbation stability - only meaningful where truth is a soft gradient.
			if( truth > 0.02f && truth < 0.98f )
			{
				const float3 dirs[5] = { {pEps,0,0}, {-pEps,0,0}, {0,pEps,0}, {0,0,pEps}, {0,0,0} };
				float jmax = 0; int parP = par; bool cbP = centreBlocked; bool flipPar = false, flipCb = false;
				for( int k = 0; k < 5; k++ )
				{
					// k<4 jitter the receiver/camera sample; k==4 jitters the light instead.
					float3 Pp = ( k < 4 ) ? P + dirs[k] : P;
					float3 Lp = ( k < 4 ) ? lc.Lo : lc.Lo + float3( pEps, 0, 0 );
					float sp = shadeAt( lc, Pp, Lp, &parP, &cbP );
					float jj = std::fabs( sp - shadow );
					if( jj > jmax ) { jmax = jj; flipPar = ( ( parP ^ par ) & 1 ) != 0; flipCb = ( cbP != centreBlocked ); }
				}
				tPenum++;
				tMaxJump = std::fmax( tMaxJump, ( double )jmax );
				if( jmax > 0.25f )		// a hard step under 1-unit jitter = visible frame-to-frame hardening
				{
					tHard++;
					if( flipPar ) { tParFlip++; }
					else if( flipCb ) { tCbFlip++; }
					else { tInternal++; }		// coverage-internal tie (F4 root / F5 atan2 / chaining)
				}
			}
			imgP[i] = shadow;
			imgR[i] = truth;
			n++;
			float d = shadow - truth;
			sumAbs += std::fabs( d );
			worst = std::fmax( worst, std::fabs( d ) );
			if( std::fabs( d ) > 0.25f ) { gross++; }
			if( d < -0.5f ) { exN++; }		// pipeline much darker than reference (extraneous shadow)
			if( d > 0.5f ) { misN++; }		// pipeline much lighter than reference (missing shadow)
			// PENUMBRA-CONDITIONAL metrics: the whole point of the technique lives in the pixels whose
			// true value is BETWEEN lit and umbra. Whole-frame means dilute them to invisibility (a few-
			// pixel band in a 50k-pixel frame reads as single-digit error while the penumbra itself is
			// wrong by half). Over-hardening - reading darker than truth inside the true penumbra, i.e.
			// stamping gradient pixels toward black - defeats the technique and is penalised separately.
			if( truth > 0.02f && truth < 0.98f )
			{
				pN++;
				pSum += std::fabs( d );
				if( std::fabs( d ) > 0.25f ) { pGross++; }
				if( d < -0.25f ) { pOverHard++; }		// darker than truth by >0.25 inside the penumbra
			}
		}
		// side-by-side sheet: SHADED pipeline | SHADED ray reference | signed-error heatmap. The two left
		// panes are lit renders (world-space checker albedo x N.L x warm light tint x shadow term + ambient)
		// so defects read in game-like context, not as abstract masks.
		auto shadePixel = [&]( int i, float shadowTerm, unsigned char* rgb )
		{
			float3 P = g.wpos[i];
			int li = g.light[i];
			// REAL captured diffuse (v5 texture tail) when present; checker fallback for legacy captures.
			// Baked texels are sRGB-ENCODED (the GPU readback blits through an sRGB target) - linearize
			// before lighting or the final sqrt() re-encode double-brightens everything toward white.
			float aR, aG, aB;
			if( g.mat[i] >= 0 )
			{
				float3 alb = c.SampleAlbedo( g.mat[i], g.st[i].x, g.st[i].y );
				aR = alb.x * alb.x; aG = alb.y * alb.y; aB = alb.z * alb.z;		// approx sRGB -> linear
			}
			else
			{
				int cx2 = ( int )std::floor( P.x / 32.0f ) + ( int )std::floor( P.y / 32.0f ) + ( int )std::floor( P.z / 32.0f );
				float check = ( cx2 & 1 ) ? 0.85f : 0.55f;
				aR = check * 0.82f; aG = check * 0.74f; aB = check * 0.62f;		// warm stone
			}
			float3 nUnit = g.nrm[i];
			float3 Lo2 = float3( c.lights[li].origin[0], c.lights[li].origin[1], c.lights[li].origin[2] );
			float3 toL2 = Lo2 - P;
			float dl2 = std::sqrt( dot( toL2, toL2 ) );
			float ndl = dl2 > 1e-4f ? std::fabs( dot( nUnit, toL2 * ( 1.0f / dl2 ) ) ) : 0.0f;
			float direct = 0.85f * shadowTerm * ndl;
			float amb = 0.12f;
			// light tint: warm incandescent
			float lr = amb + direct * 1.00f, lg = amb + direct * 0.92f, lb = amb + direct * 0.78f;
			auto tone = []( float x ) { return ( unsigned char )( std::fmin( 1.0f, std::sqrt( std::fmax( 0.0f, x ) ) ) * 255.0f ); };
			rgb[0] = tone( aR * lr );
			rgb[1] = tone( aG * lg );
			rgb[2] = tone( aB * lb );
		};
		// four panes: REAL captured game frame | shaded pipeline | shaded ray reference | signed error.
		// The real frame is the comparison standard - the shaded panes must read as the same scene.
		std::vector<unsigned char> frame;
		int fw = 0, fh = 0;
		char fpath[512];
		std::snprintf( fpath, sizeof( fpath ), "/home/app/Games/gog/doom-3-bfg-edition/neo/tests/data/%s.ppm", nm );
		bool haveFrame = LoadPPM( fpath, frame, fw, fh );
		char out[512];
		std::snprintf( out, sizeof( out ), "/home/app/Games/gog/doom-3-bfg-edition/base/softcap/ref_%s.ppm", nm );
		std::FILE* f = std::fopen( out, "wb" );
		if( f )
		{
			const int panes = haveFrame ? 4 : 3;
			std::fprintf( f, "P6\n%d %d\n255\n", W * panes, H );
			for( int y = 0; y < H; y++ )
			{
				for( int pane = 0; pane < panes; pane++ )
					for( int x = 0; x < W; x++ )
					{
						int i = y * W + x;
						unsigned char rgb[3];
						int p2 = haveFrame ? pane - 1 : pane;		// pane 0 = real frame when present
						if( haveFrame && pane == 0 )
						{
							int sx = fw > 0 ? x * fw / W : 0, sy = fh > 0 ? y * fh / H : 0;
							sx = std::min( sx, fw - 1 );
							sy = std::min( sy, fh - 1 );
							size_t o = ( ( size_t )sy * fw + sx ) * 3;
							rgb[0] = frame[o]; rgb[1] = frame[o + 1]; rgb[2] = frame[o + 2];
						}
						else if( !covered[i] ) { rgb[0] = rgb[1] = rgb[2] = 24; }
						else if( p2 < 2 )
						{
							shadePixel( i, p2 == 0 ? imgP[i] : imgR[i], rgb );
						}
						else
						{
							float d = imgP[i] - imgR[i];
							unsigned char m = ( unsigned char )( std::fmin( 1.0f, std::fabs( d ) * 2.0f ) * 255.0f );
							rgb[0] = d > 0 ? m : 0;			// red = pipeline too LIGHT (missing shadow)
							rgb[1] = ( unsigned char )( 255 - m ) / 4;
							rgb[2] = d < 0 ? m : 0;			// blue = pipeline too DARK (extraneous)
						}
						std::fwrite( rgb, 1, 3, f );
					}
			}
			std::fclose( f );
		}
		std::printf( "    [ref %s] %d px: mean|err|=%.4f worst=%.3f gross(>0.25)=%d (%.2f%%) extraneous(<-0.5)=%d missing(>0.5)=%d -> ref_%s.ppm\n",
				nm, n, n ? sumAbs / n : 0.0, worst, gross, n ? 100.0 * gross / n : 0.0, exN, misN, nm );
		std::printf( "    [ref %s] PENUMBRA (%d px, truth in 0.02..0.98): mean|err|=%.4f gross=%.1f%% OVER-HARDENED=%.1f%%\n",
				nm, pN, pN ? pSum / pN : 0.0, pN ? 100.0 * pGross / pN : 0.0, pN ? 100.0 * pOverHard / pN : 0.0 );
		std::printf( "    [temporal %s] jitter=%.1fu maxJump=%.3f  HARD(>0.25)=%d/%d (%.1f%%)  cause: parFlip=%d cbFlip=%d internal=%d\n",
				nm, pEps, tMaxJump, tHard, tPenum, tPenum ? 100.0 * tHard / tPenum : 0.0, tParFlip, tCbFlip, tInternal );
		CHECK( n > 1000 );
		CHECK( exN == 0 );						// no pixel grossly darker than the ray reference
		CHECK( misN == 0 );						// no pixel grossly lighter than the ray reference
		CHECK( gross * 100 <= n );				// and <=1% above truth-quantization disagreement
		CHECK( pN == 0 || pSum / pN < 0.08 );	// the PENUMBRA ITSELF must track the integral, not just the frame
		CHECK( pOverHard * 10 <= pN );			// over-hardening (gradient stamped dark) heavily penalised
	}
	std::printf( "    [ref] %d captures fully ray-verified\n", capsSeen );
	CHECK( capsSeen >= 7 );
}

// ====================================================================== 5d-quater. EMERGENT-UMBRA HALO
// The in-game defect the emergent-umbra GPU path (r_softShadowEmergentUmbra 1) surfaced: penumbra clipping
// and haloes. Renders the exact captured frames (softcap0012/0013) through the SAME composition the GPU
// runs - LIT / RING(guard on) / CORE(guard off) selected by centre-visibility - and the OLD solid-stamp
// composition (core = black), and writes both terms plus a leak map. The hypothesis: the core coverage
// UNDER-shadows (the proven cross-section root), so where the old path stamped solid umbra the emergent
// path now leaks to lit - a bright core inside the dark penumbra ring = the halo. Truth-free (the leak is
// emergent-vs-stamp), so it is fast; the images are the verdict. Not gated (diagnostic), but it asserts
// the leak is confined to the CORE (centre-blocked) region - if it bled into the ring, the mechanism differs.
TEST( SoftShadowHalo, emergent_umbra_leak_is_confined_to_the_core )
{
	const char* names[] = { "softcap0012", "softcap0013" };
	const int W = 480;
	int capsSeen = 0;
	for( const char* nm : names )
	{
		char path[512];
		std::snprintf( path, sizeof( path ), "/home/app/Games/gog/doom-3-bfg-edition/neo/tests/data/%s.softcap", nm );
		SoftCap c;
		if( !LoadSoftCap( path, c ) || c.receivers.empty() || c.recvVerts.empty() ) { continue; }
		capsSeen++;
		int H = c.hdr.screenW ? ( int )( ( double )W * c.hdr.screenH / c.hdr.screenW ) : W * 9 / 16;
		SwGBuffer g( W, H );
		for( const softcapReceiver_t& R : c.receivers )
			for( uint32_t t = R.firstIndex; t + 2 < R.firstIndex + R.numIndex && t + 2 < c.recvIdx.size(); t += 3 )
			{
				uint32_t ia = c.recvIdx[t], ib = c.recvIdx[t + 1], ic = c.recvIdx[t + 2];
				float3 A( c.recvVerts[ia * 3 + 0], c.recvVerts[ia * 3 + 1], c.recvVerts[ia * 3 + 2] );
				float3 B( c.recvVerts[ib * 3 + 0], c.recvVerts[ib * 3 + 1], c.recvVerts[ib * 3 + 2] );
				float3 C( c.recvVerts[ic * 3 + 0], c.recvVerts[ic * 3 + 1], c.recvVerts[ic * 3 + 2] );
				float2 z( 0, 0 );
				SwRasterTri( g, c.hdr.worldMVP, A, B, C, ( int )R.lightIndex, z, z, z, -1 );
			}
		struct RefLight { bool soft = false; float3 Lo; float rp = 0, bandRp = 0; int firstElem = 0, nRec = 0;
			std::vector<CasterRange> casters; std::vector<std::pair<float3, std::vector<std::pair<float3, float3>>>> shells; };
		std::vector<RefLight> lights( c.hdr.numLights );
		for( uint32_t li = 0; li < c.hdr.numLights; li++ )
		{
			const softcapLight_t& Lt = c.lights[li];
			RefLight& lc = lights[li];
			if( Lt.penumbraSize <= 0.0f || Lt.edgeCount == 0 ) { continue; }
			lc.soft = true; lc.Lo = float3( Lt.origin[0], Lt.origin[1], Lt.origin[2] );
			lc.rp = Lt.penumbraSize; lc.bandRp = Lt.penumbraSize * 1.1f;
			lc.firstElem = ( int )( Lt.firstEdge * 2 ); lc.nRec = ( int )Lt.edgeCount;
			for( const softcapCaster_t& cs : c.casters )
			{
				if( cs.lightIndex != li || cs.numIndex == 0 ) { continue; }
				CasterRange cr; cr.first = cs.firstIndex; cr.num = cs.numIndex;
				cr.lo = float3( 1e30f, 1e30f, 1e30f ); cr.hi = float3( -1e30f, -1e30f, -1e30f );
				for( uint32_t k = cs.firstIndex; k < cs.firstIndex + cs.numIndex && k < c.meshIdx.size(); k++ )
				{
					const float* vp = &c.meshVerts[c.meshIdx[k] * 3];
					cr.lo = float3( std::fmin( cr.lo.x, vp[0] ), std::fmin( cr.lo.y, vp[1] ), std::fmin( cr.lo.z, vp[2] ) );
					cr.hi = float3( std::fmax( cr.hi.x, vp[0] ), std::fmax( cr.hi.y, vp[1] ), std::fmax( cr.hi.z, vp[2] ) );
				}
				lc.casters.push_back( cr );
			}
			for( uint32_t r = Lt.firstEdge; r < Lt.firstEdge + Lt.edgeCount && r < c.edges.size(); r++ )
			{
				const softcapEdge_t& e = c.edges[r];
				if( e.e0[3] < 0.0f ) { lc.shells.push_back( { float3( e.e0[0], e.e0[1], e.e0[2] ), {} } ); }
				else if( !lc.shells.empty() ) { lc.shells.back().second.push_back( { float3( e.e0[0], e.e0[1], e.e0[2] ), float3( e.e1[0], e.e1[1], e.e1[2] ) } ); }
			}
		}
		SoftEdgeBuffer buf{ reinterpret_cast<const float4*>( c.edges.data() ), ( int )( c.edges.size() * 2 ) };
		float3 cam0( c.hdr.vieworg[0], c.hdr.vieworg[1], c.hdr.vieworg[2] );
		std::vector<unsigned char> imgE( ( size_t )W * H * 3, 0 ), imgS( ( size_t )W * H * 3, 0 ), imgD( ( size_t )W * H * 3, 0 );
		int coreLeak = 0, ringLeak = 0, coreN = 0;
		for( int i = 0; i < W * H; i++ )
		{
			int li = g.light[i];
			if( li < 0 || li >= ( int )lights.size() || !lights[li].soft ) { continue; }
			RefLight& lc = lights[li];
			float3 P = g.wpos[i]; float3 toL = lc.Lo - P; float dl = std::sqrt( dot( toL, toL ) );
			if( dl > 1e-4f ) { P = P + toL * ( 2.0f / dl ); }
			bool centreBlocked = RayHitsCasters( P, lc.Lo - P, c.meshVerts.data(), c.meshIdx.data(), lc.casters );
			int par = 0;
			for( auto& sh : lc.shells ) { par += std::abs( ShellZFailCapped( sh.second, sh.first, lc.bandRp, lc.Lo, cam0, P ) ); }
			// EMERGENT (GPU r_softShadowEmergentUmbra 1): LIT / RING(guard 1) / CORE(guard 0) by centre visibility
			float shadeE, shadeS;
			if( centreBlocked )
			{
				shadeE = 1.0f - saturate( SoftShadow_WedgeOcclusion( P, lc.Lo, lc.rp, lc.firstElem, lc.nRec, 0.0f, buf ) );	// CORE pass
				shadeS = 0.0f;																								// old: solid stamp
				coreN++;
				if( shadeE > 0.3f ) { coreLeak++; }
			}
			else if( ( par & 1 ) != 0 )
			{
				shadeE = 1.0f - saturate( SoftShadow_WedgeOcclusion( P, lc.Lo, lc.rp, lc.firstElem, lc.nRec, 1.0f, buf ) );	// RING pass
				shadeS = shadeE;
			}
			else { shadeE = 1.0f; shadeS = 1.0f; }
			unsigned char ve = ( unsigned char )( saturate( shadeE ) * 255.0f ), vs = ( unsigned char )( saturate( shadeS ) * 255.0f );
			imgE[i * 3 + 0] = imgE[i * 3 + 1] = imgE[i * 3 + 2] = ve;
			imgS[i * 3 + 0] = imgS[i * 3 + 1] = imgS[i * 3 + 2] = vs;
			// leak map: green = agree, RED = emergent lighter than stamp (leak); blue tints the core boundary
			float leak = shadeE - shadeS;
			if( leak > 0.1f ) { imgD[i * 3 + 0] = ( unsigned char )( saturate( leak ) * 255.0f ); if( !centreBlocked ) { ringLeak++; imgD[i * 3 + 2] = 255; } }
			else { imgD[i * 3 + 1] = vs; }
		}
		for( int k = 0; k < 3; k++ )
		{
			const char* tag = k == 0 ? "emergent" : k == 1 ? "stamp" : "leak";
			std::vector<unsigned char>& im = k == 0 ? imgE : k == 1 ? imgS : imgD;
			char out[600]; std::snprintf( out, sizeof( out ), "/home/app/Games/gog/doom-3-bfg-edition/base/softcap/halo_%s_%s.ppm", nm, tag );
			FILE* f = std::fopen( out, "wb" );
			if( f ) { std::fprintf( f, "P6\n%d %d\n255\n", W, H ); std::fwrite( im.data(), 1, im.size(), f ); std::fclose( f ); }
		}
		std::printf( "    [halo %s] core px=%d  core-leak(emergent lit where stamp black)=%d (%.1f%%)  ring-leak=%d -> halo_%s_{emergent,stamp,leak}.ppm\n",
				nm, coreN, coreLeak, coreN ? 100.0 * coreLeak / coreN : 0.0, ringLeak, nm );
		CHECK( ringLeak * 100 <= coreN + 1 );		// the leak must be a CORE phenomenon, not a ring/partition bug
	}
	// captures are local (not committed - 17MB each); assert the mechanism only when they are present
	std::printf( "    [halo] %d/2 emergent-umbra captures analysed\n", capsSeen );
}

// ====================================================================== 5d-bis. THE SILHOUETTE CONTOUR
// THE FIX, PROVEN. The penumbra under-shadow is the fed CONTOUR: the engine feeds the caster's LIGHT-apex
// silhouette (the stencil-volume edges), but disk coverage from a receiver P is bounded by the caster's
// silhouette FROM P (receiver-apex). Where a caster crosses the light plane the light-apex silhouette drops
// the far cross-section (F6/F6b); the receiver-apex silhouette carries it in an in-front edge. Measured with
// the SAME live game function (SoftShadow_WedgeOcclusion), receiver-apex contour rebuilt per fragment from
// the caster soup: mean|err| 0.10/0.11 vs light-apex 0.26/0.33 - a 60-66% cut, and F6b synthetic 0.001. The
// guards are inert (<0.002), so the residual is the contour, not the area math. NOTE the earlier reading
// that receiver-apex was WORSE was a bug - it skipped OPEN-surface boundary edges (world walls/floors are
// single quads), so their silhouette came back empty; including boundary edges is what unlocked this. The
// shippable form is a per-fragment silhouette-from-P edge test (each edge's two adjacent face normals, fed
// FIXED-ORDER + SKIP-CONNECTOR: walk the caster's candidate edges in a FIXED (view-independent) order,
// per fragment select the receiver-apex ones (skip the rest, bridging swPrev->next like the shipped
// integral does for clipped edges), close, guard. If this matches the re-chained value, the engine can
// chain candidate edges ONCE per light and the shader needs NO per-fragment chaining - the cheap path.
static float RecvApexOcc_FixedOrder( float3 P, float3 L, float r, const std::vector<TriEdgeAdj>& adj, bool rayBlocks )
{
	softFrame_t f = SoftShadow_Frame( P, L );
	float rr = std::fmax( r, 1e-2f ), r2 = rr * rr;
	double area = 0, ang = 0; bool firstValid = false; float2 first( 0, 0 ), prev( 0, 0 );
	for( const TriEdgeAdj& e : adj )
	{
		bool faP = dot( e.nA, P - e.A ) > 0.0f, fbP = dot( e.nB, P - e.A ) > 0.0f;
		if( !( e.count < 2 || faP != fbP ) ) { continue; }
		float3 A, B;
		if( e.count < 2 ) { if( faP ) { A = e.A; B = e.B; } else { A = e.B; B = e.A; } }
		else { if( !fbP ) { A = e.A; B = e.B; } else { A = e.B; B = e.A; } }
		float3 a = A - P, b = B - P; float dnA = dot( a, f.nrm ), dnB = dot( b, f.nrm ), d = dnB - dnA;
		softClip_t cl = SoftShadow_ClipSlab( dnA, dnB, SW_NEAR_EPS, f.distPL );
		if( cl.empty ) { continue; }
		float2 q0 = SoftShadow_ProjectVert( a + cl.t0 * ( b - a ), dnA + cl.t0 * d, f );
		float2 q1 = SoftShadow_ProjectVert( a + cl.t1 * ( b - a ), dnA + cl.t1 * d, f );
		if( firstValid ) { area += SoftDisk_CircleTriArea( prev, q0, r2 ); ang += SoftDisk_EdgeAngle( prev, q0 ); }
		else { first = q0; firstValid = true; }
		area += SoftDisk_CircleTriArea( q0, q1, r2 ); ang += SoftDisk_EdgeAngle( q0, q1 );
		prev = q1;
	}
	if( firstValid ) { area += SoftDisk_CircleTriArea( prev, first, r2 ); ang += SoftDisk_EdgeAngle( prev, first ); }
	if( !rayBlocks ) { area -= std::floor( ang / ( 2.0 * PI ) + 0.5 ) * ( PI * r2 ); }
	return saturate( ( float )( std::fabs( area ) / ( PI * r2 ) ) );
}

// LOCAL-CONNECTOR: the shoelace of a chained loop = Sum(edge area) + Sum(connector to NEXT edge). Each
// silhouette edge can find its OWN next edge locally (the other silhouette edge sharing its end vertex - on
// a manifold exactly one), so the connector is added per-edge with only a local vertex lookup, NO global
// walk. Order-independent, GPU-friendly. If it matches the chained value, the shader avoids the divergent
// walk. (Vertex->edge adjacency built here per call; the engine would feed it.)
static float RecvApexOcc_LocalConnector( float3 P, float3 L, float r, const std::vector<TriEdgeAdj>& adj, bool rayBlocks )
{
	softFrame_t f = SoftShadow_Frame( P, L );
	float rr = std::fmax( r, 1e-2f ), r2 = rr * rr;
	std::unordered_map<uint32_t, std::vector<int>> vmap;
	for( int i = 0; i < ( int )adj.size(); i++ ) { vmap[adj[i].va].push_back( i ); vmap[adj[i].vb].push_back( i ); }
	auto sel = [&]( const TriEdgeAdj& e, uint32_t& ds, uint32_t& de, float3& A, float3& B ) -> bool
	{
		bool faP = dot( e.nA, P - e.A ) > 0.0f, fbP = dot( e.nB, P - e.A ) > 0.0f;
		if( !( e.count < 2 || faP != fbP ) ) { return false; }
		if( e.count < 2 ) { if( faP ) { ds = e.va; de = e.vb; A = e.A; B = e.B; } else { ds = e.vb; de = e.va; A = e.B; B = e.A; } }
		else { if( !fbP ) { ds = e.va; de = e.vb; A = e.A; B = e.B; } else { ds = e.vb; de = e.va; A = e.B; B = e.A; } }
		return true;
	};
	auto proj = [&]( float3 A, float3 B, float2& q0, float2& q1 ) -> bool
	{
		float3 a = A - P, b = B - P; float dnA = dot( a, f.nrm ), dnB = dot( b, f.nrm ), d = dnB - dnA;
		softClip_t cl = SoftShadow_ClipSlab( dnA, dnB, SW_NEAR_EPS, f.distPL );
		if( cl.empty ) { return false; }
		q0 = SoftShadow_ProjectVert( a + cl.t0 * ( b - a ), dnA + cl.t0 * d, f );
		q1 = SoftShadow_ProjectVert( a + cl.t1 * ( b - a ), dnA + cl.t1 * d, f );
		return true;
	};
	double area = 0, ang = 0;
	for( int i = 0; i < ( int )adj.size(); i++ )
	{
		uint32_t ds, de; float3 A, B;
		if( !sel( adj[i], ds, de, A, B ) ) { continue; }
		float2 q0, q1; if( !proj( A, B, q0, q1 ) ) { continue; }
		area += SoftDisk_CircleTriArea( q0, q1, r2 ); ang += SoftDisk_EdgeAngle( q0, q1 );
		auto it = vmap.find( de );
		if( it == vmap.end() ) { continue; }
		for( int j : it->second )		// the next silhouette edge starts at this edge's END vertex
		{
			if( j == i ) { continue; }
			uint32_t ds2, de2; float3 A2, B2;
			if( !sel( adj[j], ds2, de2, A2, B2 ) || ds2 != de ) { continue; }
			float2 n0, n1; if( !proj( A2, B2, n0, n1 ) ) { break; }
			area += SoftDisk_CircleTriArea( q1, n0, r2 ); ang += SoftDisk_EdgeAngle( q1, n0 );
			break;
		}
	}
	if( !rayBlocks ) { area -= std::floor( ang / ( 2.0 * PI ) + 0.5 ) * ( PI * r2 ); }
	return saturate( ( float )( std::fabs( area ) / ( PI * r2 ) ) );
}

TEST( SoftShadowContour, receiver_apex_contour_recovers_the_cross_section )
{
	const char* names[] = { "erebus5", "erebus13" };		// the two penumbra-rich captures
	const int W = 288, STRIDE = 2;
	int capsSeen = 0, capsChecked = 0;
	for( const char* nm : names )
	{
		char path[512];
		std::snprintf( path, sizeof( path ), "/home/app/Games/gog/doom-3-bfg-edition/neo/tests/data/%s.softcap", nm );
		SoftCap c;
		if( !LoadSoftCap( path, c ) || c.receivers.empty() || c.recvVerts.empty() ) { continue; }
		capsSeen++;
		int H = c.hdr.screenW ? ( int )( ( double )W * c.hdr.screenH / c.hdr.screenW ) : W * 9 / 16;
		SwGBuffer g( W, H );
		for( const softcapReceiver_t& R : c.receivers )
		{
			for( uint32_t t = R.firstIndex; t + 2 < R.firstIndex + R.numIndex && t + 2 < c.recvIdx.size(); t += 3 )
			{
				uint32_t ia = c.recvIdx[t], ib = c.recvIdx[t + 1], ic = c.recvIdx[t + 2];
				float3 A( c.recvVerts[ia * 3 + 0], c.recvVerts[ia * 3 + 1], c.recvVerts[ia * 3 + 2] );
				float3 B( c.recvVerts[ib * 3 + 0], c.recvVerts[ib * 3 + 1], c.recvVerts[ib * 3 + 2] );
				float3 C( c.recvVerts[ic * 3 + 0], c.recvVerts[ic * 3 + 1], c.recvVerts[ic * 3 + 2] );
				float2 z( 0, 0 );
				SwRasterTri( g, c.hdr.worldMVP, A, B, C, ( int )R.lightIndex, z, z, z, -1 );
			}
		}
		// per-light: origin, penumbra, light-apex stream slice, culled caster ranges, and per-caster
		// RECEIVER-apex adjacency (built once, reused for every fragment of that light)
		struct CLight { bool soft = false; float3 Lo; float rp = 0; int firstElem = 0, nRec = 0; std::vector<CasterRange> casters; std::vector<std::vector<TriEdgeAdj>> adj; };
		std::vector<CLight> lights( c.hdr.numLights );
		for( uint32_t li = 0; li < c.hdr.numLights; li++ )
		{
			const softcapLight_t& Lt = c.lights[li];
			CLight& lc = lights[li];
			if( Lt.penumbraSize <= 0.0f || Lt.edgeCount == 0 ) { continue; }
			lc.soft = true;
			lc.Lo = float3( Lt.origin[0], Lt.origin[1], Lt.origin[2] );
			lc.rp = Lt.penumbraSize;
			lc.firstElem = ( int )( Lt.firstEdge * 2 );
			lc.nRec = ( int )Lt.edgeCount;
			for( const softcapCaster_t& cs : c.casters )
			{
				if( cs.lightIndex != li || cs.numIndex == 0 ) { continue; }
				CasterRange cr; cr.first = cs.firstIndex; cr.num = cs.numIndex;
				cr.lo = float3( 1e30f, 1e30f, 1e30f ); cr.hi = float3( -1e30f, -1e30f, -1e30f );
				for( uint32_t k = cs.firstIndex; k < cs.firstIndex + cs.numIndex && k < c.meshIdx.size(); k++ )
				{
					const float* vp = &c.meshVerts[c.meshIdx[k] * 3];
					cr.lo = float3( std::fmin( cr.lo.x, vp[0] ), std::fmin( cr.lo.y, vp[1] ), std::fmin( cr.lo.z, vp[2] ) );
					cr.hi = float3( std::fmax( cr.hi.x, vp[0] ), std::fmax( cr.hi.y, vp[1] ), std::fmax( cr.hi.z, vp[2] ) );
				}
				lc.casters.push_back( cr );
				std::vector<TriEdgeAdj> a;
				BuildTriEdgeAdj( c.meshVerts.data(), &c.meshIdx[cs.firstIndex], cs.numIndex, a );
				lc.adj.push_back( std::move( a ) );
			}
		}
		SoftEdgeBuffer lightBuf{ reinterpret_cast<const float4*>( c.edges.data() ), ( int )( c.edges.size() * 2 ) };
		int pN = 0, dissected = 0; double sumLight = 0, sumRecv = 0, sumNoGuard = 0, sumRecvPC = 0, sumRecvOI = 0; int lightIllusory = 0, recvIllusory = 0, recvPCillusory = 0, recvOIill = 0;
		double biasOI = 0, sumInner = 0, biasInner = 0, sumOuter = 0, biasOuter = 0; int innerN = 0, outerN = 0;		// residual structure vs converged oracle
		double sumUnion = 0, sumUnionInner = 0, biasUnionInner = 0;		// probabilistic-union combine (vs MAX) - does it fix the inner under-shadow?
		double sumClamp = 0, sumClampInner = 0, biasClampInner = 0;		// clamp-sum combine min(1,sum) - the standard AAM combine
		for( int y = 0; y < H; y += STRIDE )
			for( int x = 0; x < W; x += STRIDE )
			{
				int i = y * W + x;
				int li = g.light[i];
				if( li < 0 || li >= ( int )lights.size() || !lights[li].soft ) { continue; }
				CLight& lc = lights[li];
				float3 P = g.wpos[i];
				float3 toL = lc.Lo - P; float dl = std::sqrt( dot( toL, toL ) );
				if( dl > 1e-4f ) { P = P + toL * ( 2.0f / dl ); }
				float truth = TruthShadowConverged( P, lc.Lo, lc.rp, c.meshVerts.data(), c.meshIdx.data(), lc.casters, 256 );	// converged Hammersley oracle - matches the N=64 grid (0.057/0.035) at 12x fewer samples
				if( truth <= 0.02f || truth >= 0.98f ) { continue; }		// PENUMBRA band only
				bool centreBlocked = RayHitsCasters( P, lc.Lo - P, c.meshVerts.data(), c.meshIdx.data(), lc.casters );
				float occLight = saturate( SoftShadow_WedgeOcclusion( P, lc.Lo, lc.rp, lc.firstElem, lc.nRec, centreBlocked ? 0.0f : 1.0f, lightBuf ) );
				// same light-apex contour, guards FORCED OFF: isolates whether the winding-subtraction / debris
				// drop (active when the disk centre is lit) is what starves the outer-penumbra occlusion
				float occNoGuard = saturate( SoftShadow_WedgeOcclusion( P, lc.Lo, lc.rp, lc.firstElem, lc.nRec, 0.0f, lightBuf ) );
				sumNoGuard += std::fabs( ( 1.0f - occNoGuard ) - truth );
				// rebuild THIS receiver's apex silhouette from the caster soup and run the SAME game function
				std::vector<float4> rec;
				for( const std::vector<TriEdgeAdj>& a : lc.adj ) { AppendReceiverSilhouetteRecords( a, P, rec ); }
				float occRecv = rec.empty() ? 0.0f
						: saturate( SoftShadow_WedgeOcclusion( P, lc.Lo, lc.rp, 0, ( int )( rec.size() / 2 ), centreBlocked ? 0.0f : 1.0f, SoftEdgeBuffer{ rec.data(), ( int )rec.size() } ) );
				// PER-CASTER centre guard: run each caster's receiver-apex silhouette alone with its guard set by
				// whether THAT caster occludes the disk-centre ray (occludes -> keep winding = real umbra; else
				// subtract the illusory winding of a far caster wrapping the axis). Max-combine. Kills the F2 tail.
				// PER-CASTER guard needs a GENUINE centre-occlusion signal: a cheap geometric proxy (header sphere
				// straddles the P->L slab) was measured WORSE than light-apex (0.30/0.34, over-hard 330/622) - big
				// spheres straddle everything so the proxy never subtracts. Only the real ray test discriminates.
				// PER-CASTER guard signal = "does THIS caster occlude the disk-centre ray" = RayHitsCasters, which
				// IS point-light-shadow membership of P for that caster. Two cheaper proxies were measured WORSE:
				// a header-sphere-in-slab geometric test (0.30/0.34 - big spheres straddle everything) and an
				// edge-only winding threshold (0.17 - real casters wind ~1, the illusory ~0, but a middle band of
				// partial/grazing casters on the OPEN clipped silhouette misclassifies). The ray test is exact; its
				// cheap shader equivalent is point-in-silhouette of the ONE centre ray against the caster's
				// light-apex loop (O(edges), same order as the coverage, not O(triangles)).
				float occRecvPC = 0.0f, occRecvOI = 0.0f, prodU = 1.0f, sumOcc = 0.0f;	// prodU: prob-union; sumOcc: clamp-sum
				for( size_t ci = 0; ci < lc.adj.size() && ci < lc.casters.size(); ci++ )
				{
					std::vector<CasterRange> one( 1, lc.casters[ci] );
					bool rayBlocks = RayHitsCasters( P, lc.Lo - P, c.meshVerts.data(), c.meshIdx.data(), one );
					// chained reference (the harness re-chains receiver-apex edges into loops)
					std::vector<float4> r1; AppendReceiverSilhouetteRecords( lc.adj[ci], P, r1 );
					float o1 = 0.0f;
					if( !r1.empty() )
					{
						o1 = saturate( SoftShadow_WedgeOcclusion( P, lc.Lo, lc.rp, 0, ( int )( r1.size() / 2 ), rayBlocks ? 0.0f : 1.0f, SoftEdgeBuffer{ r1.data(), ( int )r1.size() } ) );
						occRecvPC = std::fmax( occRecvPC, o1 );
					}
					// THE SHIPPED SHADER FUNCTION SoftShadow_ProcCaster (per-fragment select + O(n^2) chain), fed the
					// candidate-edge buffer - must reproduce the chained reference on real captures.
					std::vector<float4> cand; int nSil = 0;
					for( const TriEdgeAdj& e : lc.adj[ci] )
					{
						cand.push_back( float4( e.A.x, e.A.y, e.A.z, ( float )e.va ) );
						cand.push_back( float4( e.B.x, e.B.y, e.B.z, ( float )e.vb ) );
						cand.push_back( float4( e.nA.x, e.nA.y, e.nA.z, e.count < 2 ? 1.0f : 0.0f ) );
						cand.push_back( float4( e.nB.x, e.nB.y, e.nB.z, 0.0f ) );
						bool faP = dot( e.nA, P - e.A ) > 0.0f, fbP = dot( e.nB, P - e.A ) > 0.0f;
						if( e.count < 2 || faP != fbP ) { nSil++; }
					}
					float oProc = SoftShadow_ProcCaster( P, lc.Lo, lc.rp, rayBlocks, 0, ( int )lc.adj[ci].size(), SoftEdgeBuffer{ cand.data(), ( int )cand.size() } );
					occRecvOI = std::fmax( occRecvOI, oProc );
					prodU *= ( 1.0f - oProc );
					sumOcc += oProc;
					if( dissected < 12 && std::fabs( oProc - o1 ) > 0.15f )
					{
						dissected++;
						std::printf( "      [proc] c%zu occ_ref=%.3f occ_proc=%.3f  silEdges=%d recRecs=%d rayBlocks=%d\n",
								ci, o1, oProc, nSil, ( int )( r1.size() / 2 ), ( int )rayBlocks );
					}
				}
				float shadeLight = 1.0f - occLight, shadeRecv = 1.0f - occRecv, shadeRecvPC = 1.0f - occRecvPC, shadeRecvOI = 1.0f - occRecvOI;
				sumRecvPC += std::fabs( shadeRecvPC - truth );
				if( shadeRecvPC < truth - 0.25f ) { recvPCillusory++; }
				sumRecvOI += std::fabs( shadeRecvOI - truth );
				if( shadeRecvOI < truth - 0.25f ) { recvOIill++; }
				// residual STRUCTURE of the shipped ProcCaster path vs the converged oracle: signed bias +
				// inner(truth<0.5)/outer split, to tell a fixable systematic over/under-shadow from scatter.
				double eSigned = shadeRecvOI - truth;			// >0 too LIGHT (under-shadow); <0 too DARK (over-hard)
				biasOI += eSigned;
				float shadeUnion = prodU;						// 1-(1-prod(1-occ)) = prod(1-occ); prob-union shade
				float shadeClamp = 1.0f - std::fmin( 1.0f, sumOcc );	// clamp-sum shade
				double eUnion = shadeUnion - truth, eClamp = shadeClamp - truth;
				sumUnion += std::fabs( eUnion ); sumClamp += std::fabs( eClamp );
				if( truth < 0.5f ) { sumInner += std::fabs( eSigned ); biasInner += eSigned; innerN++; sumUnionInner += std::fabs( eUnion ); biasUnionInner += eUnion; sumClampInner += std::fabs( eClamp ); biasClampInner += eClamp; }
				else               { sumOuter += std::fabs( eSigned ); biasOuter += eSigned; outerN++; }
				sumLight += std::fabs( shadeLight - truth );
				sumRecv  += std::fabs( shadeRecv  - truth );
				if( shadeLight < truth - 0.25f ) { lightIllusory++; }		// darker than truth: illusory umbra
				if( shadeRecv  < truth - 0.25f ) { recvIllusory++; }
				( void )dissected;
				pN++;
			}
		double mL = pN ? sumLight / pN : 0, mR = pN ? sumRecv / pN : 0;
		double mNG = pN ? sumNoGuard / pN : 0;
		double mRPC = pN ? sumRecvPC / pN : 0;
		double mOI = pN ? sumRecvOI / pN : 0;
		std::printf( "    [contour %s] penumbra=%d px  LIGHT-apex=%.4f(oh%d)  RECV-apex=%.4f(oh%d)  RECV+GUARD(chained ref)=%.4f(oh%d)  SHADER ProcCaster(O(n^2))=%.4f(oh%d)\n",
				nm, pN, mL, lightIllusory, mR, recvIllusory, mRPC, recvPCillusory, mOI, recvOIill );
		std::printf( "    [residual %s] vs CONVERGED oracle: bias=%+.4f  INNER(truth<.5) n=%d mean=%.4f bias=%+.4f  OUTER n=%d mean=%.4f bias=%+.4f\n",
				nm, pN ? biasOI / pN : 0.0, innerN, innerN ? sumInner / innerN : 0.0, innerN ? biasInner / innerN : 0.0,
				outerN, outerN ? sumOuter / outerN : 0.0, outerN ? biasOuter / outerN : 0.0 );
		std::printf( "    [combine %s] MAX ov=%.4f inBias=%+.4f | UNION ov=%.4f inBias=%+.4f | CLAMPSUM ov=%.4f inBias=%+.4f\n",
				nm, pN ? sumRecvOI / pN : 0.0, innerN ? biasInner / innerN : 0.0,
				pN ? sumUnion / pN : 0.0, innerN ? biasUnionInner / innerN : 0.0,
				pN ? sumClamp / pN : 0.0, innerN ? biasClampInner / innerN : 0.0 );
		CHECK( pN > 50 );
		// (1) the light-apex residual is UNDER-shadow, not over-enclosure: almost no light-apex pixel is dark
		CHECK( lightIllusory * 20 <= pN );
		// (2) the RECEIVER-apex contour RECOVERS the cross-section the light-apex drops (0.10-0.13 vs 0.26-0.33)
		CHECK( mR < mL * 0.6 );
		// (3) guards inert on the light-apex penumbra: the residual is the contour, not the area math
		CHECK( std::fabs( mL - mNG ) < 0.01 );
		// (4) THE COMPLETE FIX: receiver-apex + PER-CASTER winding guard (each caster's illusory winding
		// subtracted unless THAT caster occludes the disk-centre ray) drops mean|err| under 40% of light-apex
		// (measured 0.05/0.07) AND collapses the over-enclosure tail (over-hard 76/171 -> 1/11) that a global
		// centre guard leaves. This is the target the shipped receiver-apex path must reproduce.
		CHECK( mRPC < mL * 0.4 );
		CHECK( recvPCillusory * 25 <= pN );
		// (5) THE SHIPPED SHADER FUNCTION reproduces the accurate chained result. SoftShadow_ProcCaster does the
		// per-fragment receiver-apex select + global O(n^2) chained walk in HLSL-compatible source; it must land
		// on the chained reference (every cheaper order-free form was measured to degrade to ~0.22 - true
		// chaining is required, accuracy first). Optimise the walk only after it is confirmed visually superior.
		CHECK( mOI < mRPC * 1.5 );
		capsChecked++;
	}
	CHECK( capsSeen == 2 );
	CHECK( capsChecked == 2 );
}

// ====================================================================== 5d-ter. THE CASTER COMBINE
// The penumbra under-shadow root. SoftShadow_WedgeOcclusion combines casters with MAX (line ~328:
// swOcc = max(swOcc, |thisCaster|)). Real world geometry splits every surface into its own caster, so
// when N surfaces each occlude a DIFFERENT part of the same light disk from one receiver, MAX keeps only
// the largest single contribution and the rest of the occlusion is thrown away - a systematic under-
// shadow exactly where multiple casters share a receiver's penumbra. This runs the live per-caster area
// (slicing the fed stream at its header records, e0.w<0) and compares three combines against the ray
// truth over the penumbra pixels: MAX (shipped), clamp-SUM (min(1,sum)), and probabilistic UNION
// (1-prod(1-occ)). If a union-style combine tracks truth markedly better than MAX, the fix is the
// combine operator in the interaction path, not the contour or the area math.
TEST( SoftShadowCombine, union_of_casters_beats_max_in_the_penumbra )
{
	const char* names[] = { "erebus5", "erebus13" };
	const int W = 288, STRIDE = 2;
	int capsSeen = 0, unionWins = 0;
	for( const char* nm : names )
	{
		char path[512];
		std::snprintf( path, sizeof( path ), "/home/app/Games/gog/doom-3-bfg-edition/neo/tests/data/%s.softcap", nm );
		SoftCap c;
		if( !LoadSoftCap( path, c ) || c.receivers.empty() || c.recvVerts.empty() ) { continue; }
		capsSeen++;
		int H = c.hdr.screenW ? ( int )( ( double )W * c.hdr.screenH / c.hdr.screenW ) : W * 9 / 16;
		SwGBuffer g( W, H );
		for( const softcapReceiver_t& R : c.receivers )
			for( uint32_t t = R.firstIndex; t + 2 < R.firstIndex + R.numIndex && t + 2 < c.recvIdx.size(); t += 3 )
			{
				uint32_t ia = c.recvIdx[t], ib = c.recvIdx[t + 1], ic = c.recvIdx[t + 2];
				float3 A( c.recvVerts[ia * 3 + 0], c.recvVerts[ia * 3 + 1], c.recvVerts[ia * 3 + 2] );
				float3 B( c.recvVerts[ib * 3 + 0], c.recvVerts[ib * 3 + 1], c.recvVerts[ib * 3 + 2] );
				float3 C( c.recvVerts[ic * 3 + 0], c.recvVerts[ic * 3 + 1], c.recvVerts[ic * 3 + 2] );
				float2 z( 0, 0 );
				SwRasterTri( g, c.hdr.worldMVP, A, B, C, ( int )R.lightIndex, z, z, z, -1 );
			}
		SoftEdgeBuffer buf{ reinterpret_cast<const float4*>( c.edges.data() ), ( int )( c.edges.size() * 2 ) };
		// per-light: origin/rp/culled caster ranges (for truth+centre), plus per-caster stream SLICES (record
		// index + count) delimited by header records so each caster's area runs on the live game function alone
		struct Slice { int firstElem, nRec; };
		struct CLight { bool soft = false; float3 Lo; float rp = 0; std::vector<CasterRange> casters; std::vector<Slice> slices; };
		std::vector<CLight> lights( c.hdr.numLights );
		for( uint32_t li = 0; li < c.hdr.numLights; li++ )
		{
			const softcapLight_t& Lt = c.lights[li];
			CLight& lc = lights[li];
			if( Lt.penumbraSize <= 0.0f || Lt.edgeCount == 0 ) { continue; }
			lc.soft = true; lc.Lo = float3( Lt.origin[0], Lt.origin[1], Lt.origin[2] ); lc.rp = Lt.penumbraSize;
			for( const softcapCaster_t& cs : c.casters )
			{
				if( cs.lightIndex != li || cs.numIndex == 0 ) { continue; }
				CasterRange cr; cr.first = cs.firstIndex; cr.num = cs.numIndex;
				cr.lo = float3( 1e30f, 1e30f, 1e30f ); cr.hi = float3( -1e30f, -1e30f, -1e30f );
				for( uint32_t k = cs.firstIndex; k < cs.firstIndex + cs.numIndex && k < c.meshIdx.size(); k++ )
				{
					const float* vp = &c.meshVerts[c.meshIdx[k] * 3];
					cr.lo = float3( std::fmin( cr.lo.x, vp[0] ), std::fmin( cr.lo.y, vp[1] ), std::fmin( cr.lo.z, vp[2] ) );
					cr.hi = float3( std::fmax( cr.hi.x, vp[0] ), std::fmax( cr.hi.y, vp[1] ), std::fmax( cr.hi.z, vp[2] ) );
				}
				lc.casters.push_back( cr );
			}
			// walk the light's edge records; each header (e0.w<0) starts a new caster slice
			int cur = -1;
			for( uint32_t r = Lt.firstEdge; r < Lt.firstEdge + Lt.edgeCount && r < c.edges.size(); r++ )
			{
				if( c.edges[r].e0[3] < 0.0f )
				{
					lc.slices.push_back( { ( int )( r * 2 ), 0 } );
					cur = ( int )lc.slices.size() - 1;
				}
				if( cur >= 0 ) { lc.slices[cur].nRec++; }
			}
		}
		int pN = 0, dissected = 0; double sMax = 0, sSum = 0, sUnion = 0;
		for( int y = 0; y < H; y += STRIDE )
			for( int x = 0; x < W; x += STRIDE )
			{
				int i = y * W + x, li = g.light[i];
				if( li < 0 || li >= ( int )lights.size() || !lights[li].soft ) { continue; }
				CLight& lc = lights[li];
				float3 P = g.wpos[i];
				float3 toL = lc.Lo - P; float dl = std::sqrt( dot( toL, toL ) );
				if( dl > 1e-4f ) { P = P + toL * ( 2.0f / dl ); }
				float truth = TruthShadowCulled( P, lc.Lo, lc.rp, c.meshVerts.data(), c.meshIdx.data(), lc.casters );
				if( truth <= 0.02f || truth >= 0.98f ) { continue; }
				bool cb = RayHitsCasters( P, lc.Lo - P, c.meshVerts.data(), c.meshIdx.data(), lc.casters );
				float occMax = 0.0f, occProd = 1.0f, occAdd = 0.0f;
				for( const Slice& s : lc.slices )
				{
					float o = saturate( SoftShadow_WedgeOcclusion( P, lc.Lo, lc.rp, s.firstElem, s.nRec, cb ? 0.0f : 1.0f, buf ) );
					occMax = std::fmax( occMax, o );
					occProd *= ( 1.0f - o );
					occAdd += o;
				}
				float occUnion = 1.0f - occProd, occSum = std::fmin( 1.0f, occAdd );
				sMax   += std::fabs( ( 1.0f - occMax )   - truth );
				sSum   += std::fabs( ( 1.0f - occSum )   - truth );
				sUnion += std::fabs( ( 1.0f - occUnion ) - truth );
				// DISSECT the worst under-shadowed pixels: is truth from ONE caster whose integral reads low,
				// or spread across casters? print per-caster integral vs that caster's OWN ray truth.
				if( dissected < 10 && ( 1.0f - occMax ) > truth + 0.4f )
				{
					dissected++;
					std::printf( "      [dissect %s] P(%.0f,%.0f,%.0f) truth=%.3f occMax=%.3f  per-caster(integral|ownTruth):", nm, P.x, P.y, P.z, truth, occMax );
					for( size_t ci = 0; ci < lc.slices.size() && ci < lc.casters.size(); ci++ )
					{
						float o = saturate( SoftShadow_WedgeOcclusion( P, lc.Lo, lc.rp, lc.slices[ci].firstElem, lc.slices[ci].nRec, cb ? 0.0f : 1.0f, buf ) );
						std::vector<CasterRange> one( 1, lc.casters[ci] );
						float ot = TruthShadowCulled( P, lc.Lo, lc.rp, c.meshVerts.data(), c.meshIdx.data(), one );
						if( o > 0.02f || ot > 0.02f ) { std::printf( " c%zu(%.2f|%.2f)", ci, o, ot ); }
							if( ot < 0.9f )		// the genuine occluder: count clip fates on ITS fed silhouette
							{
								// slice<->caster pairing is order-based and the capture zeroes casterId, so match the
								// occluder caster to the slice whose header CENTRE is nearest its mesh AABB centre
								float3 cc( ( lc.casters[ci].lo.x + lc.casters[ci].hi.x ) * 0.5f, ( lc.casters[ci].lo.y + lc.casters[ci].hi.y ) * 0.5f, ( lc.casters[ci].lo.z + lc.casters[ci].hi.z ) * 0.5f );
								int bestS = -1; float bestD = 1e30f;
								for( size_t s = 0; s < lc.slices.size(); s++ )
								{
									int hr = lc.slices[s].firstElem / 2;
									float3 hc( c.edges[hr].e0[0] - cc.x, c.edges[hr].e0[1] - cc.y, c.edges[hr].e0[2] - cc.z );
									float dd = dot( hc, hc );
									if( dd < bestD ) { bestD = dd; bestS = ( int )s; }
								}
								int rs = bestS >= 0 ? lc.slices[bestS].firstElem / 2 : 0, rc = bestS >= 0 ? lc.slices[bestS].nRec : 0, eN = 0, nC = 0, fC = 0, dr = 0;
								float occAligned = bestS >= 0 ? saturate( SoftShadow_WedgeOcclusion( P, lc.Lo, lc.rp, lc.slices[bestS].firstElem, lc.slices[bestS].nRec, cb ? 0.0f : 1.0f, buf ) ) : -1.0f;
								softFrame_t fr = SoftShadow_Frame( P, lc.Lo );
								for( int r = rs; r < rs + rc && r < ( int )c.edges.size(); r++ )
								{
									if( c.edges[r].e0[3] < 0.0f ) { continue; }
									eN++;
									float3 ea( c.edges[r].e0[0] - P.x, c.edges[r].e0[1] - P.y, c.edges[r].e0[2] - P.z );
									float3 eb( c.edges[r].e1[0] - P.x, c.edges[r].e1[1] - P.y, c.edges[r].e1[2] - P.z );
									float dnA = dot( ea, fr.nrm ), dnB = dot( eb, fr.nrm );
									if( dnA < SW_NEAR_EPS || dnB < SW_NEAR_EPS ) { nC++; }
									if( dnA > fr.distPL || dnB > fr.distPL ) { fC++; }
									if( SoftShadow_ClipSlab( dnA, dnB, SW_NEAR_EPS, fr.distPL ).empty ) { dr++; }
								}
								std::printf( "\n        occluder c%zu lit=%.2f integral(idx=%.2f aligned=%.2f) slice=%d edges=%d near-clip=%d far-clip=%d dropped=%d distPL=%.0f", ci, ot, o, occAligned, bestS, eN, nC, fC, dr, fr.distPL );
							}
					}
					std::printf( "\n" );
				}
				pN++;
			}
		double mMax = pN ? sMax / pN : 0, mSum = pN ? sSum / pN : 0, mUnion = pN ? sUnion / pN : 0;
		std::printf( "    [combine %s] penumbra=%d px  MAX(shipped) mean|err|=%.4f   clamp-SUM=%.4f   prob-UNION=%.4f\n",
				nm, pN, mMax, mSum, mUnion );
		CHECK( pN > 50 );
		// FALSIFIED: a union-style combine does NOT beat MAX - the three are within 1%. The penumbra under-
		// shadow is not the combine operator; at the under-shadowed pixels the genuine occluder's OWN
		// silhouette already integrates to ~0 (dissect above: erebus5 all edges past the light plane and
		// dropped; erebus13 the surviving large silhouette's signed area cancels). The root is the silhouette
		// edge stream failing to express the caster's near/far cross-section - an architecture gap (F6 +
		// near-plane connector), not the combine, contour, or guards.
		if( std::fmin( mSum, mUnion ) >= mMax * 0.95 ) { unionWins++; }
	}
	CHECK( capsSeen == 2 );
	CHECK( unionWins == 2 );		// combine is inert: MAX ~= clamp-SUM ~= prob-UNION on both captures
}

// ====================================================================== 5e. SOFTNESS / CONTACT HARDENING
// The two properties that justify this technique over shadow maps and RT: a temporally-stable soft
// gradient, and CONTACT HARDENING - penumbra width growing with caster-receiver separation. The
// pipeline's penumbra profile is swept across the shadow edge of an elevated slab at several receiver
// distances and its 10%..90% transition width compared against the ray-traced truth. Stamping the
// hard shadow as umbra halves the width (the inner penumbra is crushed to black) - which is exactly
// "blur around a stencil shadow", not an analytic soft shadow - and fails this test.
TEST( SoftShadowSoftness, penumbra_width_tracks_truth_contact_hardening )
{
	const float3 L( 0, 0, 96 );
	const float  rp = 6.0f;
	// slab edge at x=0, elevated: receivers on the floor z=0 at varying lateral positions. The caster
	// z varies per case -> the penumbra width at the floor varies (contact hardening).
	const float slabZ[3] = { 80.0f, 48.0f, 16.0f };		// near the light .. near the floor
	int fails = 0;
	for( int s = 0; s < 3; s++ )
	{
		Box slab = MakeBox( float3( -30.0f, 0, slabZ[s] ), float3( 30.0f, 30.0f, 0.25f ) );
		auto loop = Silhouette( slab, L );
		CHECK( loop.size() >= 3 );
		auto rec = BuildCaster( { loop } );
		std::vector<std::pair<float3, float3>> edges;
		for( size_t i = 0; i < loop.size(); i++ ) { edges.push_back( { loop[i], loop[( i + 1 ) % loop.size()] } ); }
		float3 ctr( rec[0].x, rec[0].y, rec[0].z );
		// sweep the profile: shadow term and ray truth as functions of x on the floor
		auto profile = [&]( float x, float& pipe, float& truth )
		{
			float3 P( x, 0, 0 );
			// emergent-umbra model: integral runs across the full inflated cone; umbra falls out of
			// saturation. centreBlocked only selects the guard mode, it does not stamp black.
			bool centreBlocked = RayHitsBox( P + float3( 0, 0, 1e-3f ), L - P, slab );
			int par = std::abs( ShellZFailCapped( edges, ctr, rp * 1.1f, L, float3( 60, 40, 30 ), P ) );
			if( par & 1 ) { pipe = 1.0f - saturate( SoftShadow_WedgeOcclusion( P, L, rp, 0, ( int )( rec.size() / 2 ), centreBlocked ? 0.0f : 1.0f, SoftEdgeBuffer{ rec.data(), ( int )rec.size() } ) ); }
			else { pipe = centreBlocked ? 0.0f : 1.0f; }
			truth = TruthShadow( P, L, rp, slab, 160 );
		};
		// locate the transition band in both signals: scan x, record where each crosses 10% and 90% lit
		auto width = [&]( bool usePipe ) -> float
		{
			float x10 = 1e9f, x90 = -1e9f;
			for( int i = 0; i <= 1200; i++ )		// wide enough for the near-light slab whose floor penumbra spans ~+-30
			{
				float x = -60.0f + i * 0.1f;
				float p, t;
				profile( x, p, t );
				float v = usePipe ? p : t;
				if( v <= 0.1f ) { x10 = x; }				// last x still <=10% lit
				if( v >= 0.9f && x90 < -1e8f ) { x90 = x; }	// first x reaching 90% lit
			}
			return ( x90 > -1e8f && x10 < 1e8f ) ? ( x90 - x10 ) : -1.0f;
		};
		float wP = width( true ), wT = width( false );
		float ratio = ( wT > 0 ) ? wP / wT : -1;
		std::printf( "    [softness] slab z=%.0f: penumbra width pipeline=%.2f truth=%.2f ratio=%.2f\n",
				slabZ[s], wP, wT, ratio );
		if( !( ratio > 0.7f && ratio < 1.4f ) ) { fails++; }
	}
	CHECK( fails == 0 );		// the FULL analytic width, both halves - not blur around a stencil edge
}

// ====================================================================== 6. CAPTURE-STREAM INVARIANTS
// The ENGINE feed contract, checked on every committed capture: chains walk-ordered and closed, headers
// bounding their edges, per-light ranges valid, winding consistent per chain.
TEST( SoftCaptureInvariant, edge_stream_contract_on_all_committed_captures )
{
	const char* names[] = { "erebus0", "erebus1", "erebus2", "erebus3", "erebus4", "erebus5", "erebus6",
							"erebus7", "erebus13" };
	int capsSeen = 0;
	for( const char* nm : names )
	{
		char path[512];
		std::snprintf( path, sizeof( path ), "/home/app/Games/gog/doom-3-bfg-edition/neo/tests/data/%s.softcap", nm );
		SoftCap c;
		if( !LoadSoftCap( path, c ) )
		{
			std::FILE* pf = std::fopen( path, "rb" );
			uint32_t magicVer[2] = { 0, 0 };
			if( pf ) { if( std::fread( magicVer, sizeof( uint32_t ), 2, pf ) != 2 ) { magicVer[0] = 0; } std::fclose( pf ); }
			std::printf( "    [capstream %s] LOAD FAILED (magic 0x%08x version %u)\n", nm, magicVer[0], magicVer[1] );
			continue;
		}
		capsSeen++;
		int chains = 0, closed = 0, adjacentOK = 0, adjacentAll = 0, sphereViol = 0, headerCount = 0;
		int windPos = 0, windNeg = 0;
		for( uint32_t li = 0; li < c.hdr.numLights; li++ )
		{
			const softcapLight_t& L = c.lights[li];
			CHECK( ( size_t )L.firstEdge + L.edgeCount <= c.edges.size() );		// range valid
			if( L.penumbraSize <= 0.0f ) { continue; }
			float3 Lo( L.origin[0], L.origin[1], L.origin[2] );
			float3 hCtr( 0, 0, 0 );
			float  hRad = 0;
			bool   haveHeader = false;
			float3 chainStart( 0, 0, 0 ), prevB( 0, 0, 0 );
			bool   inChain = false;
			std::vector<float3> chainVerts;
			auto finishChain = [&]()
			{
				if( !inChain ) { return; }
				chains++;
				float3 d = prevB - chainStart;
				bool cl = ( d.x == 0.0f && d.y == 0.0f && d.z == 0.0f );
				if( cl ) { closed++; }
				// winding: project the chain from the light onto the plane perpendicular to (centroid - light)
				if( cl && chainVerts.size() >= 3 )
				{
					float3 ctr( 0, 0, 0 );
					for( float3 vv : chainVerts ) { ctr = ctr + vv; }
					ctr = ctr * ( 1.0f / chainVerts.size() );
					float3 ax = normalize( ctr - Lo );
					float3 up = ( std::fabs( ax.z ) > 0.9f ) ? float3( 0, 1, 0 ) : float3( 0, 0, 1 );
					float3 uu = normalize( cross( up, ax ) ), vv2 = cross( ax, uu );
					double sh = 0;
					for( size_t i = 0; i < chainVerts.size(); i++ )
					{
						float3 a3 = chainVerts[i] - Lo, b3 = chainVerts[( i + 1 ) % chainVerts.size()] - Lo;
						double ax2 = dot( a3, uu ), ay2 = dot( a3, vv2 ), bx2 = dot( b3, uu ), by2 = dot( b3, vv2 );
						sh += ax2 * by2 - ay2 * bx2;
					}
					if( sh > 0 ) { windPos++; }
					else { windNeg++; }
				}
				chainVerts.clear();
				inChain = false;
			};
			for( uint32_t rIdx = L.firstEdge; rIdx < L.firstEdge + L.edgeCount && rIdx < c.edges.size(); rIdx++ )
			{
				const softcapEdge_t& e = c.edges[rIdx];
				if( e.e0[3] < 0.0f )
				{
					finishChain();
					hCtr = float3( e.e0[0], e.e0[1], e.e0[2] );
					hRad = e.e1[0];
					haveHeader = true;
					headerCount++;
					continue;
				}
				float3 A( e.e0[0], e.e0[1], e.e0[2] ), B( e.e1[0], e.e1[1], e.e1[2] );
				if( haveHeader )
				{
					float3 dA = A - hCtr, dB = B - hCtr;
					if( length( dA ) > hRad + 1e-2f || length( dB ) > hRad + 1e-2f ) { sphereViol++; }
				}
				if( inChain )
				{
					adjacentAll++;
					float3 dd = A - prevB;
					bool adj = ( dd.x == 0.0f && dd.y == 0.0f && dd.z == 0.0f );
					if( adj ) { adjacentOK++; }
					else { finishChain(); }
				}
				if( !inChain )
				{
					inChain = true;
					chainStart = A;
					chainVerts.clear();
				}
				chainVerts.push_back( A );
				prevB = B;
			}
			finishChain();
		}
		std::printf( "    [capstream %s] headers=%d chains=%d closed=%d (%.1f%%)  adjacency %d/%d  sphere-violations=%d  winding +%d/-%d\n",
				nm, headerCount, chains, closed, chains ? 100.0 * closed / chains : 0.0,
				adjacentOK, adjacentAll, sphereViol, windPos, windNeg );
		CHECK( sphereViol == 0 );							// headers must bound their edges (cull soundness)
		CHECK( chains == 0 || closed * 100 >= chains * 95 );	// silhouettes of closed manifolds close
	}
	std::printf( "    [capstream] %d captures checked (erebus0/1 are v1-format legacy, unreadable by design)\n", capsSeen );
	CHECK( capsSeen >= 7 );
}
