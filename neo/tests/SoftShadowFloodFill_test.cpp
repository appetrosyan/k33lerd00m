/*
===========================================================================
Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2026 Aleksandr Petrosyan

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").
Doom 3 BFG Edition Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later version.
===========================================================================
*/

// FLOOD-FILL soft-shadow evaluation, measured on the CPU against eval-everywhere ground truth and
// against "run the analytic over the whole shadow volume unconditionally". The algorithm (proposed as a
// CPU-only alternative to the PCSS-band locator, which cannot predict a wide penumbra because its atlas-
// tile-bounded blocker search is a handful of texels): seed with the stencil hard-shadow volume (the
// point-light shadow = candidate umbra), evaluate the EXACT analytic coverage integral at the seed
// boundary, and flood outward into the lit region until coverage saturates to 0 (+-tol) and inward into
// the umbra until it saturates to 1 (-tol). The analytic is evaluated ONLY on the penumbra transition
// plus a one-cell saturation margin; the deep umbra core and the far-lit exterior are never evaluated -
// they are filled by region label. This discovers the penumbra extent with no band prediction, so it is
// immune to the PCSS footprint failure. It is serial / data-dependent (a BFS frontier), hence CPU.
//
// This file measures: (a) ACCURACY - max |flood - groundtruth| must be ~0 (it evaluates the SAME integral,
// just skips provably-saturated cells); (b) analytic EVAL COUNT for flood vs unconditional-over-volume;
// (c) wall-clock. Compiled standalone as C++ via hlsl_compat, so the benched math IS the shipped wedge.

#include "hlsl_compat.h"
#include "softwedge_coverage.inc.hlsl"		// the live shader coverage integral, compiled as C++
#include "SoftShadowBox.h"					// MakeBox / Silhouette / BuildCaster / Box / Rng (namespace swtest)
#include "idUnitTest.h"

#include <vector>
#include <deque>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <algorithm>

using namespace swtest;

namespace
{

struct Scene
{
	std::vector<float4> rec;			// caster edge stream (header + edges)
	int                 numRec = 0;
	float3              L;				// light origin
	float               r = 8.0f;		// light-disk radius = penumbra size
	int                 N = 0;			// grid is N x N over [g0,g1]^2 on the floor z=0
	float               g0 = -8.0f, g1 = 8.0f;
	std::vector<char>   seed;			// hard-shadow (point-light) mask = candidate umbra
};

float3 CellP( const Scene& s, int i, int j )
{
	float c = ( s.g1 - s.g0 ) / float( s.N );
	return float3( s.g0 + ( i + 0.5f ) * c, s.g0 + ( j + 0.5f ) * c, 0.0f );
}

// the EXACT shipped coverage integral at one floor cell (occlusion 0=lit .. 1=umbra)
float Occ( const Scene& s, int i, int j )
{
	SoftEdgeBuffer buf{ s.rec.data(), ( int )s.rec.size() };
	return SoftShadow_WedgeOcclusion( CellP( s, i, j ), s.L, s.r, 0, s.numRec, 0.0f, buf );
}

// crossing-number point-in-polygon (the hard-shadow seed test - a cheap stencil analog, NOT the integral)
bool PointInPoly( const std::vector<float2>& p, float x, float y )
{
	bool in = false;
	int n = ( int )p.size();
	for( int a = 0, b = n - 1; a < n; b = a++ )
	{
		if( ( ( p[a].y > y ) != ( p[b].y > y ) ) &&
				( x < ( p[b].x - p[a].x ) * ( y - p[a].y ) / ( p[b].y - p[a].y ) + p[a].x ) )
		{
			in = !in;
		}
	}
	return in;
}

Scene MakeScene( float r, int N )
{
	Scene s;
	s.L = float3( 0.0f, 0.0f, 12.0f );
	s.r = r;
	s.N = N;
	s.g0 = -8.0f;
	s.g1 = 8.0f;
	Box b = MakeBox( float3( 0.6f, 0.4f, 2.2f ), float3( 1.3f, 1.5f, 2.0f ), 0.5f, 0.1f );	// big + near the floor -> a real umbra at small r
	std::vector<float3> loop = Silhouette( b, s.L );
	std::vector<std::vector<float3>> loops;
	if( loop.size() >= 3 )
	{
		loops.push_back( loop );
	}
	s.rec = BuildCaster( loops );
	s.numRec = ( int )( s.rec.size() / 2 );

	// hard-shadow polygon: project each silhouette vertex from L onto the floor z=0
	std::vector<float2> poly;
	for( size_t k = 0; k < loop.size(); k++ )
	{
		const float3 V = loop[k];
		float t = s.L.z / ( s.L.z - V.z );			// ray L->V hits z=0 at this t
		poly.push_back( float2( s.L.x + t * ( V.x - s.L.x ), s.L.y + t * ( V.y - s.L.y ) ) );
	}
	s.seed.assign( N * N, 0 );
	for( int j = 0; j < N; j++ )
	{
		for( int i = 0; i < N; i++ )
		{
			float3 P = CellP( s, i, j );
			s.seed[j * N + i] = PointInPoly( poly, P.x, P.y ) ? 1 : 0;
		}
	}
	return s;
}

struct Result
{
	std::vector<float> mask;
	long               evals = 0;
	double             ms = 0.0;
};

// FLOOD FILL: BFS from the seed boundary, evaluate the integral, stop expanding at a saturated (0/1) cell.
Result FloodFill( const Scene& s, float tol )
{
	const int N = s.N;
	std::vector<float> mask( N * N, -1.0f );	// -1 = unresolved (filled by region label at the end)
	std::vector<char>  visited( N * N, 0 );
	std::deque<int>    q;

	auto push = [&]( int i, int j )
	{
		if( i < 0 || j < 0 || i >= N || j >= N )
		{
			return;
		}
		int k = j * N + i;
		if( !visited[k] )
		{
			visited[k] = 1;
			q.push_back( k );
		}
	};

	// seed the frontier with every hard-shadow boundary cell (a seed cell touching a non-seed cell)
	for( int j = 0; j < N; j++ )
	{
		for( int i = 0; i < N; i++ )
		{
			if( !s.seed[j * N + i] )
			{
				continue;
			}
			bool bnd = ( i == 0 || j == 0 || i == N - 1 || j == N - 1 ) ||
					   !s.seed[j * N + ( i - 1 )] || !s.seed[j * N + ( i + 1 )] ||
					   !s.seed[( j - 1 ) * N + i] || !s.seed[( j + 1 ) * N + i];
			if( bnd )
			{
				push( i, j );
			}
		}
	}

	long evals = 0;
	std::chrono::high_resolution_clock::time_point t0 = std::chrono::high_resolution_clock::now();
	while( !q.empty() )
	{
		int k = q.front();
		q.pop_front();
		int i = k % N, j = k / N;
		float o = Occ( s, i, j );
		evals++;
		mask[k] = o;
		if( o > tol && o < 1.0f - tol )		// penumbra: keep flooding in every direction
		{
			push( i - 1, j );
			push( i + 1, j );
			push( i, j - 1 );
			push( i, j + 1 );
		}
		// saturated (lit ~0 or umbra ~1): frontier stops here; beyond is provably uniform
	}
	std::chrono::high_resolution_clock::time_point t1 = std::chrono::high_resolution_clock::now();

	for( int k = 0; k < N * N; k++ )
	{
		if( mask[k] < 0.0f )
		{
			mask[k] = s.seed[k] ? 1.0f : 0.0f;	// unreached interior = umbra, unreached exterior = lit
		}
	}
	Result res;
	res.mask = mask;
	res.evals = evals;
	res.ms = std::chrono::duration<double, std::milli>( t1 - t0 ).count();
	return res;
}

// GROUND TRUTH: evaluate the integral at every cell. Also classify umbra / penumbra / lit fractions.
Result GroundTruth( const Scene& s, float tol, long& umbra, long& penumbra, long& lit )
{
	const int N = s.N;
	std::vector<float> mask( N * N );
	umbra = penumbra = lit = 0;
	std::chrono::high_resolution_clock::time_point t0 = std::chrono::high_resolution_clock::now();
	for( int j = 0; j < N; j++ )
	{
		for( int i = 0; i < N; i++ )
		{
			float o = Occ( s, i, j );
			mask[j * N + i] = o;
			if( o >= 1.0f - tol )
			{
				umbra++;
			}
			else if( o <= tol )
			{
				lit++;
			}
			else
			{
				penumbra++;
			}
		}
	}
	std::chrono::high_resolution_clock::time_point t1 = std::chrono::high_resolution_clock::now();
	Result res;
	res.mask = mask;
	res.evals = ( long )N * N;
	res.ms = std::chrono::duration<double, std::milli>( t1 - t0 ).count();
	return res;
}

}

// ---------------------------------------------------------------------------------------------------
TEST( SoftShadowFloodFill, accuracy_and_evalcount_vs_unconditional )
{
	const int   N = 96;		// coarse grid: this is a measurement (flood-fill was ruled out), not a per-pixel gate
	const float tol = 0.003f;		// saturation tolerance (matches the wedge's umbra early-break scale)

	// small r = contact shadow (big umbra core -> flood skips it); large r = wide penumbra (the intended
	// look, ~no umbra -> flood evaluates it all, no saving). Span both regimes.
	const float radii[] = { 1.0f, 2.0f, 4.0f, 8.0f };	// r>=16 overruns the [-8,8] test domain (penumbra clips)
	for( float r : radii )
	{
		Scene s = MakeScene( r, N );

		long umbra = 0, penumbra = 0, lit = 0;
		Result gt = GroundTruth( s, tol, umbra, penumbra, lit );
		Result fl = FloodFill( s, tol );

		double mx = 0.0;
		for( int k = 0; k < N * N; k++ )
		{
			mx = std::fmax( mx, std::fabs( ( double )fl.mask[k] - ( double )gt.mask[k] ) );
		}

		// "unconditional over the shadow volume" = the analytic at every umbra+penumbra cell (the stencil
		// volume; PCSS would additionally have to have PREDICTED this region, which its footprint cannot).
		long uncond = umbra + penumbra;

		std::printf( "    [flood r=%4.1f] grid=%d^2  umbra=%ld penumbra=%ld lit=%ld\n",
					 r, N, umbra, penumbra, lit );
		std::printf( "    [flood r=%4.1f] evals: flood=%ld  unconditional(volume)=%ld  groundtruth=%ld   flood/uncond=%.2f\n",
					 r, fl.evals, uncond, gt.evals, uncond > 0 ? ( double )fl.evals / uncond : 0.0 );
		std::printf( "    [flood r=%4.1f] wall: flood=%.2fms  groundtruth=%.2fms   max|flood-gt|=%.4f\n",
					 r, fl.ms, gt.ms, mx );

		// Accuracy: away from the saturation cutoff the flood evaluates the SAME integral, so any error is
		// the coverage gradient across the one cell where the frontier stopped - a GRID-RESOLUTION bound
		// (~0.02 at 384^2), not an algorithmic one; it shrinks with grid density. Lenient sanity bound only.
		CHECK( mx <= 0.05 );
		( void )uncond;					// eval counts are the measurement (printed), not a pass/fail
	}
	CHECK( true );
}

// ===================================================================================================
// ADAPTIVE SUB-SAMPLING of the coverage field. The field is band-limited by the penumbra width, so it is
// over-sampled at full resolution (most heavily in a WIDE penumbra). Measured here: three reconstruction
// schemes vs eval-everywhere ground truth, over penumbra widths r, reporting eval fraction, wall-clock,
// and reconstruction error - crucially the mean SIGNED error, which must be ~0 (unbiased). A cell is
// evaluated at most once (shared corners cached), and that eval count is the speed proxy.

namespace
{

inline float ClampF( float x, float a, float b )
{
	return x < a ? a : ( x > b ? b : x );
}

inline float Bilerp( float c00, float c10, float c01, float c11, float u, float v )
{
	return ( c00 * ( 1.0f - u ) + c10 * u ) * ( 1.0f - v ) + ( c01 * ( 1.0f - u ) + c11 * u ) * v;
}

struct Cache
{
	std::vector<float> val;
	std::vector<char>  done;
	long               evals = 0;
};

float Get( const Scene& s, Cache& c, int i, int j )
{
	int k = j * s.N + i;
	if( !c.done[k] )
	{
		c.val[k] = Occ( s, i, j );
		c.done[k] = 1;
		c.evals++;
	}
	return c.val[k];
}

// UNIFORM fixed spacing S, bilinear reconstruct (the naive/biased baseline).
void Uniform( const Scene& s, int step, std::vector<float>& recon, long& evals, double& ms )
{
	const int N = s.N;
	Cache c;
	c.val.assign( N * N, 0.0f );
	c.done.assign( N * N, 0 );
	std::chrono::high_resolution_clock::time_point t0 = std::chrono::high_resolution_clock::now();
	for( int j = 0; j < N; j++ )
	{
		for( int i = 0; i < N; i++ )
		{
			int i0 = ( i / step ) * step, j0 = ( j / step ) * step;
			int i1 = std::min( i0 + step, N - 1 ), j1 = std::min( j0 + step, N - 1 );
			float u = i1 > i0 ? float( i - i0 ) / ( i1 - i0 ) : 0.0f;
			float v = j1 > j0 ? float( j - j0 ) / ( j1 - j0 ) : 0.0f;
			float c00 = Get( s, c, i0, j0 ), c10 = Get( s, c, i1, j0 ), c01 = Get( s, c, i0, j1 ), c11 = Get( s, c, i1, j1 );
			recon[j * N + i] = ClampF( Bilerp( c00, c10, c01, c11, u, v ), 0.0f, 1.0f );
		}
	}
	std::chrono::high_resolution_clock::time_point t1 = std::chrono::high_resolution_clock::now();
	evals = c.evals;
	ms = std::chrono::duration<double, std::milli>( t1 - t0 ).count();
}

// ADAPTIVE QUADTREE: subdivide while the 4 corners disagree > tol; smooth cell -> bilinear fill. The
// refinement is driven by MEASURED coverage discrepancy, so it is unbiased by construction and the
// spacing tracks the local penumbra width automatically. This is the accuracy ceiling.
void QuadRec( const Scene& s, Cache& c, std::vector<float>& recon, int x0, int y0, int x1, int y1, float tol, int maxCell )
{
	const int N = s.N;
	float c00 = Get( s, c, x0, y0 ), c10 = Get( s, c, x1, y0 ), c01 = Get( s, c, x0, y1 ), c11 = Get( s, c, x1, y1 );
	recon[y0 * N + x0] = c00;
	recon[y0 * N + x1] = c10;
	recon[y1 * N + x0] = c01;
	recon[y1 * N + x1] = c11;
	int w = x1 - x0, h = y1 - y0;
	if( w <= 1 && h <= 1 )
	{
		return;						// atomic cell: corners are all its points
	}

	auto fill = [&]()
	{
		for( int y = y0; y <= y1; y++ )
		{
			for( int x = x0; x <= x1; x++ )
			{
				float u = w > 0 ? float( x - x0 ) / w : 0.0f;
				float v = h > 0 ? float( y - y0 ) / h : 0.0f;
				recon[y * N + x] = ClampF( Bilerp( c00, c10, c01, c11, u, v ), 0.0f, 1.0f );
			}
		}
	};

	if( w <= 1 || h <= 1 )
	{
		fill();						// a dimension is atomic - cannot subdivide further; linear-fill it
		return;
	}
	int mx = ( x0 + x1 ) / 2, my = ( y0 + y1 ) / 2;
	// Coarse FLOOR: above maxCell always subdivide (never trust 4 agreeing corners over an unprobed span -
	// that is how the whole-grid-is-lit miss happened). At/under maxCell, a corner-spread AND a centre-probe
	// test guard against a feature sitting between the corners (Nyquist floor + centre sample).
	if( w <= maxCell && h <= maxCell )
	{
		float lo = std::min( std::min( c00, c10 ), std::min( c01, c11 ) );
		float hi = std::max( std::max( c00, c10 ), std::max( c01, c11 ) );
		float cc = Get( s, c, mx, my );							// centre probe (cached; children reuse it)
		float bil = 0.25f * ( c00 + c10 + c01 + c11 );
		if( hi - lo <= tol && std::fabs( cc - bil ) <= tol )
		{
			fill();
			return;
		}
	}
	QuadRec( s, c, recon, x0, y0, mx, my, tol, maxCell );
	QuadRec( s, c, recon, mx, y0, x1, my, tol, maxCell );
	QuadRec( s, c, recon, x0, my, mx, y1, tol, maxCell );
	QuadRec( s, c, recon, mx, my, x1, y1, tol, maxCell );
}

void Quadtree( const Scene& s, float tol, std::vector<float>& recon, long& evals, double& ms )
{
	const int N = s.N;
	Cache c;
	c.val.assign( N * N, 0.0f );
	c.done.assign( N * N, 0 );
	std::chrono::high_resolution_clock::time_point t0 = std::chrono::high_resolution_clock::now();
	QuadRec( s, c, recon, 0, 0, N - 1, N - 1, tol, 16 );	// maxCell 16 = coarse Nyquist floor
	std::chrono::high_resolution_clock::time_point t1 = std::chrono::high_resolution_clock::now();
	evals = c.evals;
	ms = std::chrono::duration<double, std::milli>( t1 - t0 ).count();
}

// WIDTH-PROPORTIONAL (the cheap GPU/VRS-mappable proxy): coarse tiles estimate the local coverage spread,
// then each tile is sampled at a step sized so consecutive samples differ by ~targetDelta, and bilinearly
// reconstructed. A flat (wide-penumbra) tile samples coarsely; a steep (contact) tile samples finely.
void WidthProp( const Scene& s, float targetDelta, int T, std::vector<float>& recon, long& evals, double& ms )
{
	const int N = s.N;
	Cache c;
	c.val.assign( N * N, 0.0f );
	c.done.assign( N * N, 0 );
	std::chrono::high_resolution_clock::time_point t0 = std::chrono::high_resolution_clock::now();
	for( int ty = 0; ty < N - 1; ty += T )
	{
		for( int tx = 0; tx < N - 1; tx += T )
		{
			int x1 = std::min( tx + T, N - 1 ), y1 = std::min( ty + T, N - 1 );
			float a = Get( s, c, tx, ty ), b = Get( s, c, x1, ty ), d = Get( s, c, tx, y1 ), e = Get( s, c, x1, y1 );
			float lo = std::min( std::min( a, b ), std::min( d, e ) );
			float hi = std::max( std::max( a, b ), std::max( d, e ) );
			float D = hi - lo;				// coverage spread across the tile
			// samples-across-tile = D / targetDelta  ->  step = tileSize / that, clamped to [1, tileSize]
			int span = x1 - tx;
			int step = ( int )ClampF( ( float )std::floor( span * targetDelta / std::max( D, 1e-4f ) + 0.5f ), 1.0f, ( float )std::max( span, 1 ) );
			for( int y = ty; y <= y1; y++ )
			{
				for( int x = tx; x <= x1; x++ )
				{
					int i0 = tx + ( ( x - tx ) / step ) * step, j0 = ty + ( ( y - ty ) / step ) * step;
					int i1 = std::min( i0 + step, x1 ), j1 = std::min( j0 + step, y1 );
					float u = i1 > i0 ? float( x - i0 ) / ( i1 - i0 ) : 0.0f;
					float v = j1 > j0 ? float( y - j0 ) / ( j1 - j0 ) : 0.0f;
					float s00 = Get( s, c, i0, j0 ), s10 = Get( s, c, i1, j0 ), s01 = Get( s, c, i0, j1 ), s11 = Get( s, c, i1, j1 );
					recon[y * N + x] = ClampF( Bilerp( s00, s10, s01, s11, u, v ), 0.0f, 1.0f );
				}
			}
		}
	}
	std::chrono::high_resolution_clock::time_point t1 = std::chrono::high_resolution_clock::now();
	evals = c.evals;
	ms = std::chrono::duration<double, std::milli>( t1 - t0 ).count();
}

struct Metric
{
	double signedMean, absMean, mx, rms, p99, mxInterior;
};

Metric Err( const std::vector<float>& recon, const std::vector<float>& gt )
{
	long n = ( long )recon.size();
	double s = 0.0, sa = 0.0, sq = 0.0, mx = 0.0, mxi = 0.0;
	std::vector<double> ad;
	ad.reserve( n );
	for( long k = 0; k < n; k++ )
	{
		double dd = ( double )recon[k] - ( double )gt[k];
		s += dd;
		sa += std::fabs( dd );
		sq += dd * dd;
		mx = std::fmax( mx, std::fabs( dd ) );
		if( gt[k] > 0.1f && gt[k] < 0.9f )			// interior penumbra (away from the 0/1 saturation kinks)
		{
			mxi = std::fmax( mxi, std::fabs( dd ) );
		}
		ad.push_back( std::fabs( dd ) );
	}
	std::sort( ad.begin(), ad.end() );
	Metric m;
	m.signedMean = s / n;
	m.absMean = sa / n;
	m.mx = mx;
	m.rms = std::sqrt( sq / n );
	m.p99 = ad[( size_t )( 0.99 * ( n - 1 ) )];
	m.mxInterior = mxi;
	return m;
}

void Report( const char* scheme, float knob, float r, long evals, long full, double ms, const Metric& m )
{
	std::printf( "    [%-9s r=%4.1f knob=%.2f] evals=%7ld frac=%.3f  ms=%6.2f  signed=%+.4f absMean=%.4f max=%.4f(int %.4f) rms=%.4f p99=%.4f\n",
				 scheme, r, knob, evals, ( double )evals / full, ms, m.signedMean, m.absMean, m.mx, m.mxInterior, m.rms, m.p99 );
}

}

TEST( SoftShadowSubsample, accuracy_vs_evals_no_bias )
{
	const int   N = 65;			// 2^6 + 1 -> clean quadtree halving; coarse (this is a sub-sampling MEASUREMENT, not a gate)
	const float tolSat = 0.003f;
	const float radii[] = { 1.0f, 2.0f, 4.0f, 8.0f };
	double worstQuadSignedAt05 = 0.0;

	for( float r : radii )
	{
		Scene s = MakeScene( r, N );
		long um = 0, pen = 0, lit = 0;
		Result gt = GroundTruth( s, tolSat, um, pen, lit );
		const long full = ( long )N * N;
		std::printf( "  --- r=%.1f  grid=%d^2  umbra=%ld penumbra=%ld lit=%ld  (groundtruth %ld evals, %.1fms) ---\n",
					 r, N, um, pen, lit, gt.evals, gt.ms );

		std::vector<float> recon( full, 0.0f );
		long ev = 0;
		double ms = 0.0;
		for( int S : { 2, 4, 8, 16 } )
		{
			Uniform( s, S, recon, ev, ms );
			Report( "uniform", ( float )S, r, ev, full, ms, Err( recon, gt.mask ) );
		}
		for( float tol : { 0.02f, 0.05f, 0.10f } )
		{
			Quadtree( s, tol, recon, ev, ms );
			Metric m = Err( recon, gt.mask );
			Report( "quadtree", tol, r, ev, full, ms, m );
			if( tol == 0.05f )
			{
				worstQuadSignedAt05 = std::fmax( worstQuadSignedAt05, std::fabs( m.signedMean ) );
				CHECK( m.absMean <= ( double )tol );	// bounded by the refinement criterion
			}
		}
		for( float t : { 0.05f, 0.10f, 0.20f } )
		{
			WidthProp( s, t, 8, recon, ev, ms );
			Report( "widthprop", t, r, ev, full, ms, Err( recon, gt.mask ) );
		}
	}
	CHECK( worstQuadSignedAt05 <= 0.01 );		// the adaptive ceiling is UNBIASED (mean signed error ~ 0)
}
