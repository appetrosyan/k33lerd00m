/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 2026 Aleksandr Petrosyan

This file is part of the Doom 3 BFG Edition GPL Source Code.

===========================================================================
*/

// SEGMENT (fluorescent-tube / line) area-light coverage: the EXACT 1-D interval-union model
// (docs/softshadow/cylinder-light-evaluation.md, "Tube-as-segment", claim (S)). The source is the
// emitting LINE segment A->B; a fragment's shadow term is the fraction of the segment whose ray to the
// receiver is blocked = |union of the occluders' blocked t-intervals| / |AB|. Because the source is 1-D
// this is exact - no disk sampling, no per-triangle chord sweep - and it stays exact for the NON-CONVEX
// / multi-occluder case (a union of intervals), which is exactly where the doc-convicted lossy
// "two-endpoint membership + blend" (claim (T)) over-darkens the umbra and breaks monotonicity.
//
// GROUND TRUTH is INDEPENDENT of the analytic endpoint math: it samples S(t) at N points and, per point,
// asks the shared occlusion predicate SoftSeg_SegHitTri whether P->S(t) is blocked - coverage = blocked
// fraction. Truth and analytic therefore share ONLY the binary occlusion test; the analytic path adds
// the exact interval endpoints, so a match proves those endpoints, not a shared bias. The analytic
// endpoints are exact roots of linear grazing equations, so the residual vs an N-sample truth is just
// the truth's own sampling quantum (~ boundaries / N), which is why the tolerance below is TIGHT.

#include "hlsl_compat.h"
#include "softwedge_coverage.inc.hlsl"		// the LIVE shader source, compiled as C++ (SW_FUNC = inline)
#include "idUnitTest.h"

#include <vector>
#include <cmath>

namespace
{
struct Tri { float3 a, b, c; };

// axis-aligned quad in the plane z = zc, x in [x0,x1], y in [y0,y1] -> 2 triangles (an occluder slab).
void AddQuad( std::vector<Tri>& tris, float x0, float x1, float y0, float y1, float zc )
{
	float3 p00( x0, y0, zc ), p10( x1, y0, zc ), p11( x1, y1, zc ), p01( x0, y1, zc );
	tris.push_back( { p00, p10, p11 } );
	tris.push_back( { p00, p11, p01 } );
}

// ANALYTIC coverage (the primitive under test): union of every triangle's exact blocked interval.
float SegCoverage( float3 P, float3 A, float3 B, const std::vector<Tri>& tris )
{
	float2 iv[SW_SEG_MAX_IV];
	int n = 0;
	for( const Tri& t : tris )
	{
		if( n >= SW_SEG_MAX_IV ) { break; }
		iv[n++] = SoftSeg_TriInterval( t.a, t.b, t.c, P, A, B );
	}
	return SoftSeg_UnionLength( iv, n );		// t spans [0,1] so union length IS the shadow term
}

// INDEPENDENT ground truth: sample S(t) at N points, count those whose ray to P is blocked by ANY tri.
// Uses the SAME occlusion predicate as the analytic path (so only the sampling-vs-exact-endpoints
// differs), so its only error vs the exact union is its 1/N quantum.
float SegTruth( float3 P, float3 A, float3 B, const std::vector<Tri>& tris, int N )
{
	float3 d = B - A;
	int blocked = 0;
	for( int k = 0; k < N; k++ )
	{
		float t = ( k + 0.5f ) / N;
		float3 S = A + d * t;
		float3 dir = S - P;
		for( const Tri& tr : tris )
		{
			if( SoftSeg_SegHitTri( P, dir, tr.a, tr.b, tr.c ) ) { blocked++; break; }
		}
	}
	return ( float )blocked / N;
}

const int TRUTH_N = 1 << 16;		// 65536 samples: analytic-vs-truth error <= boundaries/N (~6e-5), under the 1e-4 tol
}

// The tube lies along +x at z = 100 over a receiver plane z = 0. From P = origin the ray to S(t) crosses
// the occluder plane z = 50 at lam = 0.5, x = -20 + 40t, so a slab spanning x in [x0,x1] there blocks
// t in [(x0+20)/40, (x1+20)/40] - hand-computable canonical values that the independent truth confirms.
TEST( SoftShadowSegment, coverage )
{
	const float3 P( 0, 0, 0 );
	const float3 A( -40, 0, 100 ), B( 40, 0, 100 );

	// 1. FULLY LIT: no occluder -> coverage 0.
	{
		std::vector<Tri> tris;
		float cov = SegCoverage( P, A, B, tris );
		CHECK_NEAR( cov, 0.0f, 1e-6 );
		CHECK_NEAR( cov, SegTruth( P, A, B, tris, TRUTH_N ), 1e-4 );
	}

	// 2. FULL UMBRA: a slab covering the whole crossing range x in [-20,20] -> every ray blocked -> 1.
	{
		std::vector<Tri> tris;
		AddQuad( tris, -30, 30, -60, 60, 50 );
		float cov = SegCoverage( P, A, B, tris );
		CHECK( cov > 0.999f );
		CHECK_NEAR( cov, SegTruth( P, A, B, tris, TRUTH_N ), 1e-4 );
	}

	// 3. PARTIAL PENUMBRA, single occluder -> ONE interval. x in [-4,4] -> t in [0.4,0.6], cov 0.2.
	{
		std::vector<Tri> tris;
		AddQuad( tris, -4, 4, -60, 60, 50 );
		float cov = SegCoverage( P, A, B, tris );
		CHECK_NEAR( cov, 0.2f, 2e-3 );
		CHECK_NEAR( cov, SegTruth( P, A, B, tris, TRUTH_N ), 1e-4 );
	}

	// 4. TWO occluders, DISJOINT intervals. x[-8,-4] -> t[0.3,0.4], x[4,8] -> t[0.7,0.8]; union 0.2.
	{
		std::vector<Tri> tris;
		AddQuad( tris, -8, -4, -60, 60, 50 );
		AddQuad( tris,  4,  8, -60, 60, 50 );
		float cov = SegCoverage( P, A, B, tris );
		CHECK_NEAR( cov, 0.2f, 2e-3 );
		CHECK_NEAR( cov, SegTruth( P, A, B, tris, TRUTH_N ), 1e-4 );
	}

	// 5. TWO occluders, OVERLAPPING intervals. x[-8,0] -> t[0.3,0.5], x[-4,8] -> t[0.4,0.7]; union
	//    [0.3,0.7] = 0.4 (the overlap [0.4,0.5] counted ONCE - the union, not the sum 0.5).
	{
		std::vector<Tri> tris;
		AddQuad( tris, -8, 0, -60, 60, 50 );
		AddQuad( tris, -4, 8, -60, 60, 50 );
		float cov = SegCoverage( P, A, B, tris );
		CHECK_NEAR( cov, 0.4f, 2e-3 );
		CHECK_NEAR( cov, SegTruth( P, A, B, tris, TRUTH_N ), 1e-4 );
	}

	// 6. NON-CONVEX / MONOTONICITY-BREAK (the doc's named failure of the lossy claim (T)). Slabs block
	//    NEAR BOTH ENDPOINTS while the MIDDLE stays lit: x[-24,-16] -> t[0,0.1], x[16,24] -> t[0.9,1].
	//    Both segment endpoints (t=0 and t=1) are occluded, so the two-extent intersection heuristic
	//    would label the whole segment umbra (cov ~ 1, over-dark); the EXACT union is 0.2. This test
	//    pins the exact answer, which is the whole point of building (S) instead of (T).
	{
		std::vector<Tri> tris;
		AddQuad( tris, -24, -16, -60, 60, 50 );
		AddQuad( tris,  16,  24, -60, 60, 50 );
		float cov = SegCoverage( P, A, B, tris );
		CHECK_NEAR( cov, 0.2f, 2e-3 );
		CHECK_NEAR( cov, SegTruth( P, A, B, tris, TRUTH_N ), 1e-4 );
		// sanity: both endpoints ARE blocked (what would fool the two-extent heuristic).
		CHECK( SoftSeg_SegHitTri( P, ( A ) - P, tris[0].a, tris[0].b, tris[0].c ) ||
			   SoftSeg_SegHitTri( P, ( A ) - P, tris[1].a, tris[1].b, tris[1].c ) );
	}

	// 7. GENERAL 3-D (non-coplanar) case: off-axis receiver, TILTED segment and a TILTED single triangle
	//    between them. No hand value - the independent high-N truth is the sole checker, exercising the
	//    full 3-D edge-grazing + plane-crossing root math (not the degenerate xz-planar cases above).
	{
		const float3 P2( 6, 7, 0 );
		const float3 A2( -40, -12, 100 ), B2( 35, 20, 95 );
		std::vector<Tri> tris;
		tris.push_back( { float3( -6, -8, 52 ), float3( 10, -2, 48 ), float3( 2, 9, 55 ) } );
		float cov = SegCoverage( P2, A2, B2, tris );
		CHECK( cov > 0.02f && cov < 0.98f );		// genuinely partial (guards a degenerate all/none)
		CHECK_NEAR( cov, SegTruth( P2, A2, B2, tris, TRUTH_N ), 1e-4 );
	}
}
