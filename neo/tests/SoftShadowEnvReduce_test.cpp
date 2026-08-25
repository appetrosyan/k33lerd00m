/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 2026 Aleksandr Petrosyan

===========================================================================
*/

// DIRECT-SHADER unit tests for the fractional-endpoint envelope reduction (task #90). The live shader
// source (softwedge_coverage.inc.hlsl) is compiled as C++ here, so SoftScan_FillTri / SoftScan_ReduceCov
// under test ARE the GPU functions, not a CPU translation.
//
// Replicates the gate regression 181 -> 194 STEP that the FIRST port shipped, from two audited defects in
// its reduction ("RegressedReduce" below is that reduction, kept verbatim as the negative control):
//   1. CLAMP-ALIAS: envelope validity was gated on contig( grid & diskMask ), but the envelope spans the
//      RAW union. When the mask deletes one of two disjoint raw runs, the masked grid reads contiguous
//      while the envelope bridges both - the gap inside the chord counts as covered (unbounded over-shadow).
//   2. UNIT MISMATCH: the envelope contributed continuous length x SW_SCAN_BITS/2 over a denominator of
//      popcount( diskMask ), which SoftScan_Run OUT-ROUNDS by ~1 bit/chord. Discrete/discrete cancels the
//      rounding; continuous/discrete under-shadows, worst on the short extreme chords, and chords flipping
//      env<->popcount step.
// The fixed SoftScan_ReduceCov gates on RAW contiguity and contributes fraction-of-chord x that chord's
// own mask popcount. The sweep test proves it still kills the X-endpoint step (the point of task #90)
// through the REAL FillTri fill path, against an independent brute-force disk-ray truth.

#include "hlsl_compat.h"

#define SW_SCANLINE 1
#define SW_SCAN_BITS 32
#define SW_SCAN_CHORDS 16
#define inout
namespace swenvreduce
{
#include "softwedge_coverage.inc.hlsl"
swAttrib_t g_swAttrib = {};
bool       g_swAttribOn = false;
}
using namespace swenvreduce;

#include "idUnitTest.h"

#include <cmath>
#include <cstdint>
#include <cstdio>

namespace
{

// the FIRST port's reduction, verbatim (negative control - this is the code that took the gate 181->194).
float RegressedReduce( const SwGridWord grid[SW_SCAN_CHORDS], const float2 env[SW_SCAN_CHORDS], const SwGridWord mask[SW_SCAN_CHORDS] )
{
	float cov = 0.0f;
	for( int cm = 0; cm < SW_SCAN_CHORDS; cm++ )
	{
		const SwGridWord g = grid[cm] & mask[cm];
		if( SwGridContig( g ) )		// BUG 1: masked contiguity, raw envelope
		{
			const float hc = SW_SCAN_HC[cm];
			const float a = fmaxf( env[cm].x, -hc ), b = fminf( env[cm].y, hc );
			cov += fmaxf( 0.0f, b - a ) * ( SW_SCAN_BITS * 0.5f );	// BUG 2: continuous units over a discrete denominator
		}
		else
		{
			cov += ( float )SoftScan_PC( g );
		}
	}
	return cov;
}

// the SECOND port's reduction, verbatim (negative control - fixed both v1 bugs yet took the gate 194->205):
// a MIXED estimator, exact fraction on raw-contiguous chords / popcount on holey ones. Multi-occluder
// chords flip between the two estimators as the receiver moves and every flip jumps by the discrete
// out-rounding - net MORE high-frequency STEP/ANT than pure discrete.
float MixedFlipReduce( const SwGridWord grid[SW_SCAN_CHORDS], const float2 env[SW_SCAN_CHORDS], const SwGridWord mask[SW_SCAN_CHORDS] )
{
	float cov = 0.0f;
	for( int cm = 0; cm < SW_SCAN_CHORDS; cm++ )
	{
		const SwGridWord g = grid[cm] & mask[cm];
		if( SwGridContig( grid[cm] ) )
		{
			const float hc = SW_SCAN_HC[cm];
			const float a = fmaxf( env[cm].x, -hc ), b = fminf( env[cm].y, hc );
			cov += fmaxf( 0.0f, b - a ) / ( 2.0f * hc ) * ( float )SoftScan_PC( mask[cm] );
		}
		else
		{
			cov += ( float )SoftScan_PC( g );
		}
	}
	return cov;
}

void DiskMask( SwGridWord mask[SW_SCAN_CHORDS], int& diskBits )
{
	diskBits = 0;
	for( int m = 0; m < SW_SCAN_CHORDS; m++ ) { mask[m] = SoftScan_Run( -SW_SCAN_HC[m], SW_SCAN_HC[m] ); diskBits += SoftScan_PC( mask[m] ); }
}

// independent brute truth: NxN samples of the light disk, occluded iff the P->sample segment crosses the
// occluder plane z = zOcc inside the half-plane x <= xR (the sweep test's quad is that half-plane).
float BruteHalfPlaneCov( float3 P, float3 L, float R, float xR, float zOcc, int N )
{
	float3 n = normalize( L - P );
	float3 up = ( std::fabs( n.z ) > 0.9f ) ? float3( 0, 1, 0 ) : float3( 0, 0, 1 );
	float3 u = normalize( cross( up, n ) ), v = cross( n, u );
	int blocked = 0, total = 0;
	for( int iy = 0; iy < N; iy++ )
		for( int ix = 0; ix < N; ix++ )
		{
			float du = ( ix + 0.5f ) / N * 2 - 1, dv = ( iy + 0.5f ) / N * 2 - 1;
			if( du * du + dv * dv > 1.0f ) { continue; }
			total++;
			float3 S = L + u * ( du * R ) + v * ( dv * R );
			float dz = S.z - P.z;
			if( std::fabs( dz ) < 1e-9f ) { continue; }
			float t = ( zOcc - P.z ) / dz;
			if( t <= 0.0f || t >= 1.0f ) { continue; }
			float x = P.x + ( S.x - P.x ) * t;
			if( x <= xR ) { blocked++; }
		}
	return total ? ( float )blocked / total : 0.0f;
}

}

// BUG 1 replication at the grid level, using only live shader primitives. Chord 0 (hc=0.348): one run
// entirely OUTSIDE the disk chord, one inside. The mask deletes the first, the masked grid is contiguous,
// and the regressed reduction counts the clamped envelope [-hc, 0.2] - the whole gap - as covered.
TEST( SoftShadowEnvReduce, clamp_alias_two_runs_over_shadows )
{
	SwGridWord grid[SW_SCAN_CHORDS], mask[SW_SCAN_CHORDS];
	float2 env[SW_SCAN_CHORDS];
	int diskBits = 0;
	DiskMask( mask, diskBits );
	for( int m = 0; m < SW_SCAN_CHORDS; m++ ) { grid[m] = SwGridZero(); env[m] = SwEnvZero(); }

	const int m = 0;
	const float hc = SW_SCAN_HC[m];					// 0.348: intervals below -hc are outside the disk chord
	grid[m] |= SoftScan_Run( -0.9f, -0.6f );  env[m] = SwEnvAdd( env[m], -0.9f, -0.6f );	// masked away entirely
	grid[m] |= SoftScan_Run( 0.0f, 0.2f );    env[m] = SwEnvAdd( env[m], 0.0f, 0.2f );		// inside the chord

	CHECK( !SwGridContig( grid[m] ) );				// raw union genuinely has a hole
	CHECK( SwGridContig( grid[m] & mask[m] ) );		// ...that the mask hides (the alias)

	// exact truth on this chord: only [0, 0.2] covers it -> that fraction of the chord's mask bits
	const float truthBits = 0.2f / ( 2.0f * hc ) * ( float )SoftScan_PC( mask[m] );
	const float fixedCov  = SoftScan_ReduceCov( grid, env, mask );
	const float regreCov  = RegressedReduce( grid, env, mask );
	std::printf( "[envreduce] clamp-alias chord: truth=%.2f bits, fixed=%.2f, regressed=%.2f (maskPC=%d)\n",
			truthBits, fixedCov, regreCov, SoftScan_PC( mask[m] ) );
	CHECK( regreCov > truthBits + 3.0f );			// the replicated defect: gap counted as covered
	CHECK_NEAR( fixedCov, truthBits, 1.0f );		// fixed: within one column of exact
}

// BUG 2 replication: a chord fully covered by one interval must reduce to EXACTLY its mask popcount
// (coverage 1), or fully-shadowed fragments disagree with the popcount paths and seams/steps appear.
TEST( SoftShadowEnvReduce, full_cover_is_exactly_diskbits )
{
	SwGridWord grid[SW_SCAN_CHORDS], mask[SW_SCAN_CHORDS];
	float2 env[SW_SCAN_CHORDS];
	int diskBits = 0;
	DiskMask( mask, diskBits );
	for( int m = 0; m < SW_SCAN_CHORDS; m++ )
	{
		grid[m] = SoftScan_Run( -1.0f, 1.0f );
		env[m] = SwEnvZero();
		env[m] = SwEnvAdd( env[m], -1.0f, 1.0f );
	}
	const float fixedCov = SoftScan_ReduceCov( grid, env, mask );
	const float regreCov = RegressedReduce( grid, env, mask );
	std::printf( "[envreduce] full cover: diskBits=%d fixed=%.3f regressed=%.3f (regressed bias=%.3f bits)\n",
			diskBits, fixedCov, regreCov, regreCov - diskBits );
	CHECK_NEAR( fixedCov, ( float )diskBits, 1e-3f );	// unit-consistent: full chord == its popcount
	CHECK( std::fabs( regreCov - ( float )diskBits ) > 1.0f );	// the replicated bias (continuous vs out-rounded bits)
}

// The point of task #90, proven through the REAL fill path: a receiver sweeping under a straight occluder
// edge must see the X-endpoint coverage STEP collapse under the envelope reduction, while tracking the
// independent brute truth. (This is the CPU-prototype geometry, now driving the live shader FillTri.)
TEST( SoftShadowEnvReduce, endpoint_step_killed_through_fill_path )
{
	const float3 L( 0, 0, 100 );
	const float  R = 20.0f, zOcc = 50.0f, xR = 3.0f;
	// occluder: half-plane x <= xR at z = zOcc, as two large triangles
	const float3 a0( -400, -400, zOcc ), a1( xR, -400, zOcc ), a2( xR, 400, zOcc ), a3( -400, 400, zOcc );

	SwGridWord mask[SW_SCAN_CHORDS];
	int diskBits = 0;
	DiskMask( mask, diskBits );

	float prevD = -1.0f, prevF = -1.0f, maxStepD = 0.0f, maxStepF = 0.0f, maxErrF = 0.0f;
	const int NS = 201;
	for( int s = 0; s < NS; s++ )
	{
		const float3 P( -2.0f + 8.0f * s / ( NS - 1 ), 0.0f, 0.0f );
		softFrame_t F = SoftShadow_Frame( P, L );
		SwGridWord grid[SW_SCAN_CHORDS];
		float2 env[SW_SCAN_CHORDS];
		for( int m = 0; m < SW_SCAN_CHORDS; m++ ) { grid[m] = SwGridZero(); env[m] = SwEnvZero(); }
		SoftScan_FillTri( grid, env, a0, a1, a2, P, F, R, SW_NEAR_EPS );
		SoftScan_FillTri( grid, env, a0, a2, a3, P, F, R, SW_NEAR_EPS );

		int covD = 0;
		for( int m = 0; m < SW_SCAN_CHORDS; m++ ) { covD += SoftScan_PC( grid[m] & mask[m] ); }
		const float d = diskBits ? ( float )covD / diskBits : 0.0f;						// discrete baseline
		const float f = diskBits ? SoftScan_ReduceCov( grid, env, mask ) / diskBits : 0.0f;	// under test
		const float t = BruteHalfPlaneCov( P, L, R, xR, zOcc, 256 );

		if( s > 0 ) { maxStepD = fmaxf( maxStepD, std::fabs( d - prevD ) ); maxStepF = fmaxf( maxStepF, std::fabs( f - prevF ) ); }
		prevD = d; prevF = f;
		if( t > 0.02f && t < 0.98f ) { maxErrF = fmaxf( maxErrF, std::fabs( f - t ) ); }
	}
	std::printf( "[envreduce] sweep: maxStep discrete=%.5f fractional=%.5f | max|frac-truth|=%.4f\n",
			maxStepD, maxStepF, maxErrF );
	CHECK( maxStepD > 0.004f );					// the original defect exists in the discrete path
	CHECK( maxStepF < 0.5f * maxStepD );		// the envelope kills the step
	CHECK_NEAR( maxErrF, 0.0f, 0.03f );			// and stays faithful to the independent truth
}

namespace
{
bool RayTri( float3 O, float3 D, float3 a, float3 b, float3 c )
{
	float3 e1 = b - a, e2 = c - a, pv = cross( D, e2 );
	float det = dot( e1, pv );
	if( std::fabs( det ) < 1e-9f ) { return false; }
	float inv = 1.0f / det;
	float3 tv = O - a;
	float u = dot( tv, pv ) * inv; if( u < 0 || u > 1 ) { return false; }
	float3 qv = cross( tv, e1 );
	float v = dot( D, qv ) * inv; if( v < 0 || u + v > 1 ) { return false; }
	float t = dot( e2, qv ) * inv;
	return t > 1e-4f && t < 1.0f - 1e-4f;
}

struct Tri3 { float3 a, b, c; };

float BruteSoupCov( float3 P, float3 L, float R, const Tri3* tris, int nTris, int N )
{
	float3 n = normalize( L - P );
	float3 up = ( std::fabs( n.z ) > 0.9f ) ? float3( 0, 1, 0 ) : float3( 0, 0, 1 );
	float3 u = normalize( cross( up, n ) ), v = cross( n, u );
	int blocked = 0, total = 0;
	for( int iy = 0; iy < N; iy++ )
		for( int ix = 0; ix < N; ix++ )
		{
			float du = ( ix + 0.5f ) / N * 2 - 1, dv = ( iy + 0.5f ) / N * 2 - 1;
			if( du * du + dv * dv > 1.0f ) { continue; }
			total++;
			float3 D = ( L + u * ( du * R ) + v * ( dv * R ) ) - P;
			for( int t = 0; t < nTris; t++ ) { if( RayTri( P, D, tris[t].a, tris[t].b, tris[t].c ) ) { blocked++; break; } }
		}
	return total ? ( float )blocked / total : 0.0f;
}
}

// THE GATE REPLICATION (181 discrete -> 205 mixed): many small occluders so chords carry HOLES, receiver
// sweep so estimator choice can flip pixel-to-pixel. The mixed v2 reduction must show its flip-noise
// regression vs pure discrete here; the shipped SoftScan_ReduceCov must be at most as steppy as discrete
// (it differs from discrete only by continuously-shrinking outer-endpoint slivers) and at least as
// faithful to the independent ray truth.
TEST( SoftShadowEnvReduce, multi_occluder_no_flip_regression )
{
	const float3 L( 0, 0, 100 );
	const float  R = 20.0f;
	// deterministic pseudo-random small-tri soup between receiver and light (holes on most chords)
	Tri3 tris[40];
	uint32_t s = 1234u;
	auto rnd = [&s]( float lo, float hi ) { s = s * 1664525u + 1013904223u; return lo + ( hi - lo ) * ( ( s >> 8 ) & 0xffffff ) / 16777215.0f; };
	for( int i = 0; i < 40; i++ )
	{
		float cx = rnd( -8.0f, 8.0f ), cy = rnd( -6.0f, 6.0f ), cz = rnd( 30.0f, 70.0f ), sz = rnd( 1.5f, 4.0f );
		tris[i].a = float3( cx, cy, cz );
		tris[i].b = float3( cx + sz, cy, cz );
		tris[i].c = float3( cx, cy + sz, cz + rnd( -1.0f, 1.0f ) );
	}

	SwGridWord mask[SW_SCAN_CHORDS];
	int diskBits = 0;
	DiskMask( mask, diskBits );

	const int NS = 151;
	float prevD = 0, prevM = 0, prevF = 0;
	float stepD = 0, stepM = 0, stepF = 0, errD = 0, errM = 0, errF = 0;
	int holePx = 0;
	for( int si = 0; si < NS; si++ )
	{
		const float3 P( -2.0f + 4.0f * si / ( NS - 1 ), 0.3f, 0.0f );
		softFrame_t F = SoftShadow_Frame( P, L );
		SwGridWord grid[SW_SCAN_CHORDS];
		float2 env[SW_SCAN_CHORDS];
		for( int m = 0; m < SW_SCAN_CHORDS; m++ ) { grid[m] = SwGridZero(); env[m] = SwEnvZero(); }
		for( int t = 0; t < 40; t++ ) { SoftScan_FillTri( grid, env, tris[t].a, tris[t].b, tris[t].c, P, F, R, SW_NEAR_EPS ); }
		bool anyHole = false;
		for( int m = 0; m < SW_SCAN_CHORDS; m++ ) { if( !SwGridContig( grid[m] ) ) { anyHole = true; } }
		holePx += anyHole ? 1 : 0;

		int covI = 0;
		for( int m = 0; m < SW_SCAN_CHORDS; m++ ) { covI += SoftScan_PC( grid[m] & mask[m] ); }
		const float d = ( float )covI / diskBits;
		const float mx = MixedFlipReduce( grid, env, mask ) / diskBits;
		const float f = SoftScan_ReduceCov( grid, env, mask ) / diskBits;
		const float t = BruteSoupCov( P, L, R, tris, 40, 64 );

		if( si > 0 )
		{
			stepD = fmaxf( stepD, std::fabs( d - prevD ) );
			stepM = fmaxf( stepM, std::fabs( mx - prevM ) );
			stepF = fmaxf( stepF, std::fabs( f - prevF ) );
		}
		prevD = d; prevM = mx; prevF = f;
		if( t > 0.02f && t < 0.98f )
		{
			errD = fmaxf( errD, std::fabs( d - t ) );
			errM = fmaxf( errM, std::fabs( mx - t ) );
			errF = fmaxf( errF, std::fabs( f - t ) );
		}
	}
	std::printf( "[envreduce] soup sweep (%d/%d px with holey chords):\n", holePx, NS );
	std::printf( "[envreduce]   maxStep: discrete=%.5f mixed=%.5f fixed=%.5f\n", stepD, stepM, stepF );
	std::printf( "[envreduce]   max|err| vs truth: discrete=%.4f mixed=%.4f fixed=%.4f\n", errD, errM, errF );
	CHECK( holePx > NS / 2 );			// the scenario actually exercises holey chords
	CHECK( stepM > 2.0f * stepF );		// the replicated v2 regression: estimator-flip noise
	// provable bound: v3 differs from discrete by outer-endpoint slivers that move continuously; the only
	// discontinuity is a correction handing off when a NEW outermost run appears - bounded by one bit,
	// coincident with (and no larger than) the discrete jump for that same topology change.
	CHECK( stepF <= stepD + 1.0f / diskBits );
	CHECK( errF <= errD + 0.01f );		// and is at least as faithful to the independent truth
}
