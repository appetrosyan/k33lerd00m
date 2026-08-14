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

// BENCHMARK + DEVIATION REGRESSION for the analytic soft-shadow wedge integral
// (SoftShadow_WedgeOcclusion, softwedge_coverage.inc.hlsl). Purpose: before optimising the hot loop
// (fast atan2, culled-caster jump-skip, tiled edge lists, ...) we need
//   (a) a per-evaluation LATENCY BASELINE to measure any speedup against, and
//   (b) a DEVIATION-FROM-ANCHOR gate that says HOW MUCH an optimisation changed the output - passing
//       only while the change stays within a stated tolerance, and always printing max/mean/rms/p99.
// Compiled standalone as C++ (ID_UNIT_TEST_STANDALONE) via the same hlsl_compat shim the correctness
// suite uses, so the benched math IS the shipped math. The bit-exact refactor guard lives in
// SoftShadowPrimitives_test.cpp (SoftShadowGolden); this file is the perf + approximation side.

#include "hlsl_compat.h"
#include "softwedge_coverage.inc.hlsl"		// the live shader source, compiled as C++ (SW_FUNC=inline)
#include "SoftShadowBox.h"					// MakeBox / Silhouette / BuildCaster / Box / Rng (namespace swtest)
#include "idUnitTest.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
#include <algorithm>
#include <cmath>

using namespace swtest;

namespace
{

// A representative "light": its record stream accumulates many box silhouettes (a busy static scene ->
// hundreds of edge records, like a real point light), evaluated over a grid of floor receiver points
// spanning lit / penumbra / umbra - the same shape the pixel shader hits (every fragment loops the
// whole light's records). numCasters drives E (records/evaluation); gridN^2 drives the call count.
struct Workload
{
	std::vector<float4> rec;
	int                 numRec = 0;		// rec.size()/2
	float3              L;
	float               r = 0.0f;
	std::vector<float3> P;				// receiver points
};

Workload MakeWorkload( int numCasters, int gridN, unsigned seed )
{
	Workload w;
	w.L = float3( 0, 0, 12 );
	w.r = 2.0f;
	Rng rng( seed );
	std::vector<std::vector<float3>> loops;
	for( int c = 0; c < numCasters; c++ )
	{
		float3 C( rng.f( -4, 4 ), rng.f( -4, 4 ), rng.f( 3.0f, 8.0f ) );
		float3 h( rng.f( 0.3f, 1.5f ), rng.f( 0.3f, 1.5f ), rng.f( 0.2f, 1.5f ) );
		Box b = MakeBox( C, h, rng.f( 0, 3.14f ), rng.f( -0.3f, 0.3f ) );
		std::vector<float3> loop = Silhouette( b, w.L );
		if( loop.size() >= 3 )
		{
			loops.push_back( loop );
		}
	}
	w.rec = BuildCaster( loops );		// one stream, many casters (headers + edges)
	w.numRec = ( int )( w.rec.size() / 2 );
	for( int i = 0; i < gridN; i++ )
	{
		for( int j = 0; j < gridN; j++ )
		{
			float x = -5.0f + 10.0f * i / float( gridN - 1 );
			float y = -5.0f + 10.0f * j / float( gridN - 1 );
			w.P.push_back( float3( x, y, 0.0f ) );
		}
	}
	return w;
}

// evaluate the wedge over every receiver point once; accumulate to defeat dead-code elimination.
double RunOnce( const Workload& w )
{
	SoftEdgeBuffer buf{ w.rec.data(), ( int )w.rec.size() };
	double s = 0.0;
	for( size_t i = 0; i < w.P.size(); i++ )
	{
		s += SoftShadow_WedgeOcclusion( w.P[i], w.L, w.r, 0, w.numRec, 0.0f, buf );
	}
	return s;
}

const char* ANCHOR_PATH = "/home/app/Games/gog/doom-3-bfg-edition/neo/tests/data/wedge_bench_anchor.bin";

// deviation an optimisation is allowed to introduce vs the exact anchor before it counts as "broken".
// 0 today (nothing approximated yet); raise CONSCIOUSLY when accepting an approximation (e.g. fast atan2).
const double DEVIATION_TOL = 0.0;

}

// -------------------------------------------------------------------------- latency baseline
TEST( SoftShadowBench, wedge_throughput )
{
	Workload w = MakeWorkload( 30, 64, 20260815u );	// ~hundreds of records; 4096 receiver calls
	volatile double sink = RunOnce( w );			// warm caches / pull code in

	const int REPS = 20;
	std::chrono::high_resolution_clock::time_point t0 = std::chrono::high_resolution_clock::now();
	double acc = 0.0;
	for( int r = 0; r < REPS; r++ )
	{
		acc += RunOnce( w );
	}
	std::chrono::high_resolution_clock::time_point t1 = std::chrono::high_resolution_clock::now();
	sink = acc;

	double ns = std::chrono::duration<double, std::nano>( t1 - t0 ).count();
	long calls = ( long )w.P.size() * REPS;
	long recUpper = calls * ( long )w.numRec;	// upper bound; per-caster cull skips some at runtime
	std::printf( "    [bench] records/light=%d  receiver-calls/rep=%zu  reps=%d\n", w.numRec, w.P.size(), REPS );
	std::printf( "    [bench] %.1f ns/call   %.3f ns/record(upper-bound)   %.2f Mcalls/s   (sink=%.4g)\n",
				 ns / calls, ns / recUpper, calls / ( ns / 1e3 ), ( double )sink );
	CHECK( acc == acc );	// a benchmark only needs to be finite/non-NaN; it is a measurement, not a pass/fail
}

// -------------------------------------------------------------------------- deviation-from-anchor gate
TEST( SoftShadowBench, deviation_vs_anchor )
{
	Workload w = MakeWorkload( 24, 48, 20260901u );		// fixed deterministic corpus (stable anchor)
	SoftEdgeBuffer buf{ w.rec.data(), ( int )w.rec.size() };
	std::vector<float> now;
	now.reserve( w.P.size() );
	for( size_t i = 0; i < w.P.size(); i++ )
	{
		now.push_back( SoftShadow_WedgeOcclusion( w.P[i], w.L, w.r, 0, w.numRec, 0.0f, buf ) );
	}

	std::FILE* f = std::fopen( ANCHOR_PATH, "rb" );
	if( !f )
	{
		f = std::fopen( ANCHOR_PATH, "wb" );
		CHECK( f != NULL );
		if( f )
		{
			uint32_t n = ( uint32_t )now.size();
			std::fwrite( &n, sizeof( n ), 1, f );
			std::fwrite( now.data(), sizeof( float ), now.size(), f );
			std::fclose( f );
			std::printf( "    [anchor] GENERATED %zu outputs -> wedge_bench_anchor.bin (rerun to compare)\n", now.size() );
		}
		return;
	}
	uint32_t n = 0;
	CHECK( std::fread( &n, sizeof( n ), 1, f ) == 1 );
	std::vector<float> ref( n );
	CHECK( std::fread( ref.data(), sizeof( float ), n, f ) == n );
	std::fclose( f );
	CHECK( ( size_t )n == now.size() );

	double sum = 0.0, sq = 0.0, mx = 0.0;
	int over = 0;
	std::vector<double> devs;
	devs.reserve( now.size() );
	for( size_t i = 0; i < now.size() && i < ref.size(); i++ )
	{
		double d = std::fabs( ( double )now[i] - ( double )ref[i] );
		sum += d;
		sq += d * d;
		mx = std::fmax( mx, d );
		if( d > DEVIATION_TOL )
		{
			over++;
		}
		devs.push_back( d );
	}
	std::sort( devs.begin(), devs.end() );
	double mean = devs.empty() ? 0.0 : sum / devs.size();
	double rms = devs.empty() ? 0.0 : std::sqrt( sq / devs.size() );
	double p99 = devs.empty() ? 0.0 : devs[( size_t )( 0.99 * ( devs.size() - 1 ) )];
	std::printf( "    [deviation vs anchor] n=%u  max=%.6f  mean=%.6f  rms=%.6f  p99=%.6f  over-tol(%.4f)=%d\n",
				 n, mx, mean, rms, p99, DEVIATION_TOL, over );
	CHECK( mx <= DEVIATION_TOL );	// an optimisation must stay within tol of the exact anchor; raise tol consciously
}
