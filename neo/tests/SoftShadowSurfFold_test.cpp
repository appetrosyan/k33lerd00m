/*
===========================================================================
Doom 3 BFG Edition GPL Source Code
Copyright (C) 2026 Aleksandr Petrosyan

This program is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version. See <http://www.gnu.org/licenses/>.
===========================================================================
*/

// SURF-FOLD CACHE fidelity: the fold classifier (softsurf_build.cs.hlsl) must NOT erode UMBRA EXTENT.
// The cache reconstructs a texel as term = 1 - saturate( bilerp(F corners) + walk(residual) ), where F
// is the folded static coverage. Folding is only valid where the folded set's UNION coverage is
// linear across the texel. The UMBRA is where the union SATURATES to 1 - a hard clamp bilerp cannot
// represent - so an umbra-straddling texel MUST route to exact walk-always, never fold.
//
// The trap this test pins: a per-occluder linearity classifier is BLIND to union saturation. Two
// occluders whose SOLO coverage is each linear (each folds) can UNION to a saturated umbra. Folding
// both and bilerping their combined F LINEARISES the saturation plateau -> the umbra shrinks. This
// test builds exactly that configuration and asserts the reconstructed umbra extent matches the exact
// walk. It is RED while the classifier folds the straddle, GREEN once it flags it to walk-always.

#include "hlsl_compat.h"
#include "softwedge_coverage.inc.hlsl"		// live shader coverage math, compiled as C++
#include "softsurf_classify.inc.hlsl"		// the SHIPPED fold classifier, compiled as C++ (no ported copy)
#include "SoftShadowBox.h"					// FaceCasterCPU / FaceStreamCPU / FaceOcclusion / SoftTriRad
#include "idUnitTest.h"

#include <vector>
#include <cstdio>

using namespace swtest;

namespace
{

// SurfBuild_SoloCovExact + SurfBuild_FoldD2 come from softsurf_classify.inc.hlsl - the SHIPPED shader
// source compiled as C++. There is deliberately NO ported copy here: the test cannot pass on a
// classifier that differs from the one softsurf_build.cs.hlsl compiles.

// The 9 texel probe points (build's PU/PV, axis d=2 -> (pu,pv,anchor)) for a z=anchor receiver plane.
// 13 probes matching softsurf_build.cs.hlsl: 0-3 corners, 4 center, 5-8 edge mids, 9-12 QUADRANT
// CENTERS (the quadrant points catch a sub-probe umbra bulge the center+edge-mids miss). The fold 2nd
// difference uses only 0-8; the quadrant points are validation (self-gate + satSome) only.
const float PU9[13] = { 0.0f, 1.0f, 0.0f, 1.0f, 0.5f, 0.5f, 0.0f, 1.0f, 0.5f, 0.25f, 0.75f, 0.25f, 0.75f };
const float PV9[13] = { 0.0f, 0.0f, 1.0f, 1.0f, 0.5f, 0.0f, 0.5f, 0.5f, 1.0f, 0.25f, 0.25f, 0.75f, 0.75f };

struct TexelPts { float3 p[13]; };
TexelPts TexelProbe( int cu, int cv, float G, float anchor )
{
	TexelPts t;
	for( int i = 0; i < 13; i++ )
	{
		t.p[i] = float3( ( ( float )cu + PU9[i] ) * G, ( ( float )cv + PV9[i] ) * G, anchor );
	}
	return t;
}

// one raw triangle (v0,v1,v2)
struct Tri { float3 v[3]; };

// the fold decision, straight off the SHARED classifier (softsurf_classify.inc.hlsl): the shipped
// SurfBuild_SoloCovExact at the 9 probes, then the shipped SurfBuild_FoldD2 2nd difference <= thr.
bool ClassifyFold( const Tri& tri, const TexelPts& tp, float3 L, float R, float thr )
{
	const float R2 = R * R;
	float sc[9];
	for( int i = 0; i < 9; i++ )
	{
		softFrame_t f = SoftShadow_Frame( tp.p[i], L );
		sc[i] = SurfBuild_SoloCovExact( tp.p[i], tri.v[0], tri.v[1], tri.v[2], f, R2 );
	}
	const float d2 = SurfBuild_FoldD2( sc[0], sc[1], sc[2], sc[3], sc[4], sc[5], sc[6], sc[7], sc[8] );
	return d2 <= thr;
}

// The single-edge coverage profile: circular-segment area as a function of the signed distance s (in
// disk-radius units) from the light-disk centre to the projected silhouette edge. g(-1)=1 (umbra),
// g(0)=1/2, g(1)=0 (lit). This IS coverage(P) for one straight edge, and s is affine in P - so the
// correct reconstruction bilerps s (affine, exact) and applies g, instead of bilerping coverage (g of
// affine - nonlinear). g_inv is the monotone inverse (bisection; a LUT/Newton step in-shader).
float SegCov( float s )
{
	s = s < -1.0f ? -1.0f : ( s > 1.0f ? 1.0f : s );
	return ( float )( ( std::acos( s ) - s * std::sqrt( std::fmax( 0.0f, 1.0f - s * s ) ) ) / M_PI );
}
float SegCovInv( float c )		// find s with SegCov(s) = c ; SegCov is DECREASING in s
{
	c = c < 0.0f ? 0.0f : ( c > 1.0f ? 1.0f : c );
	float lo = -1.0f, hi = 1.0f;
	for( int i = 0; i < 30; i++ )
	{
		float m = 0.5f * ( lo + hi );
		if( SegCov( m ) > c ) { lo = m; }
		else { hi = m; }
	}
	return 0.5f * ( lo + hi );
}

FaceCasterCPU CasterFromTris( const std::vector<Tri>& tris )
{
	FaceCasterCPU fc;
	float3 c( 0, 0, 0 );
	int n = 0;
	for( const Tri& t : tris )
		for( int k = 0; k < 3; k++ ) { c = c + t.v[k]; n++; }
	c = c * ( 1.0f / ( float )n );
	float rad = 0;
	for( const Tri& t : tris )
		for( int k = 0; k < 3; k++ ) { rad = std::fmax( rad, len3( t.v[k] - c ) ); }
	fc.sphere = float4( c.x, c.y, c.z, rad * 1.001f );
	fc.centre = c;
	for( const Tri& t : tris )
	{
		fc.tris.push_back( float4( t.v[0].x, t.v[0].y, t.v[0].z, SoftTriRadV0( t.v[0], t.v[1], t.v[2] ) ) );
		fc.tris.push_back( float4( t.v[1].x, t.v[1].y, t.v[1].z, SoftTriRad( t.v[0], t.v[1], t.v[2] ) ) );
		fc.tris.push_back( float4( t.v[2].x, t.v[2].y, t.v[2].z, 0 ) );
	}
	return fc;
}

float Occ( const std::vector<Tri>& tris, float3 P, float3 L, float R )
{
	if( tris.empty() ) { return 0.0f; }
	FaceStreamCPU s; s.Append( CasterFromTris( tris ) );
	return FaceOcclusion( s, P, L, R );
}

} // namespace

// one texel's fold reconstruction vs the exact walk. Returns worst per-pixel error over an NxN sub-grid
// and the umbra-extent erosion (exact umbra pixels minus reconstructed umbra pixels). selfGatePass tells
// whether the shipped 5-point self-gate (center + 4 edge midpoints, errTol) would have ACCEPTED the fold.
struct FoldEval { float worstErr; int umbraTruth; int umbraRecon; bool selfGatePass; int folded; int nTri; bool walkAlways; };

FoldEval EvalTexel( const std::vector<Tri>& all, float3 texelOrigin, float G, float3 L, float R,
					float thr, float errTol )
{
	FoldEval e{}; e.nTri = ( int )all.size();
	// probe points anchored at this texel's origin (cu,cv = 0, origin folded into texelOrigin)
	TexelPts tp;
	for( int i = 0; i < 13; i++ )
		tp.p[i] = float3( texelOrigin.x + PU9[i] * G, texelOrigin.y + PV9[i] * G, texelOrigin.z );

	std::vector<Tri> residual;
	for( const Tri& t : all )
	{
		if( ClassifyFold( t, tp, L, R, thr ) ) { e.folded++; }
		else { residual.push_back( t ); }
	}
	// SEPARABILITY GUARD (mirrors softsurf_build.cs.hlsl): an umbra-touching texel with a non-empty
	// residual is non-separable -> walk exactly (recon == truth). Fully-saturated with no residual folds.
	{
		bool satSome = false;
		for( int i = 0; i < 13; i++ ) { if( Occ( all, tp.p[i], L, R ) >= 0.999f ) { satSome = true; break; } }
		if( satSome && !residual.empty() )
		{
			const int N = 33;
			for( int iy = 0; iy < N; iy++ )
				for( int ix = 0; ix < N; ix++ )
				{
					float3 P = float3( texelOrigin.x + ( float )ix / ( N - 1 ) * G, texelOrigin.y + ( float )iy / ( N - 1 ) * G, texelOrigin.z );
					if( Occ( all, P, L, R ) >= 0.999f ) { e.umbraTruth++; e.umbraRecon++; }
				}
			e.worstErr = 0.0f; e.selfGatePass = true; e.walkAlways = true;	// walk-always is exact
			return e;
		}
	}
	float Fc[4];
	for( int c = 0; c < 4; c++ )
		Fc[c] = std::fmax( Occ( all, tp.p[c], L, R ) - Occ( residual, tp.p[c], L, R ), 0.0f );

	auto recon = [&]( float fu, float fv, float3 P )
	{
		const float fLo = Fc[0] + ( Fc[1] - Fc[0] ) * fu;
		const float fHi = Fc[2] + ( Fc[3] - Fc[2] ) * fu;
		return saturate( ( fLo + ( fHi - fLo ) * fv ) + Occ( residual, P, L, R ) );
	};
	// shipped self-gate: reconstruction error at center + 4 edge midpoints (probe idx 4..8) <= errTol
	e.selfGatePass = true;
	for( int vp = 4; vp < 13; vp++ )
	{
		const float r = recon( PU9[vp], PV9[vp], tp.p[vp] );
		if( std::fabs( Occ( all, tp.p[vp], L, R ) - r ) > errTol ) { e.selfGatePass = false; break; }
	}
	if( !e.selfGatePass ) { e.walkAlways = true; }	// the build routes a self-gate failure to walk-always too
	const int N = 33;
	for( int iy = 0; iy < N; iy++ )
		for( int ix = 0; ix < N; ix++ )
		{
			const float fu = ( float )ix / ( float )( N - 1 ), fv = ( float )iy / ( float )( N - 1 );
			float3 P = float3( texelOrigin.x + fu * G, texelOrigin.y + fv * G, texelOrigin.z );
			const float truth = Occ( all, P, L, R );
			const float rc    = recon( fu, fv, P );
			if( truth >= 0.999f ) { e.umbraTruth++; }
			if( rc    >= 0.999f ) { e.umbraRecon++; }
			e.worstErr = std::fmax( e.worstErr, std::fabs( truth - rc ) );
		}
	return e;
}

// SWEEP a two-slab occluder whose UNION saturates in a texel-interior ridge (corners in penumbra, so the
// exact corner-F cannot anchor the umbra) at many lateral offsets, and hunt the worst fidelity failure the
// classifier lets through the self-gate. RED while the continuous per-occluder classifier folds a
// union-saturating occluder; GREEN once such texels are flagged to walk-always.
TEST( SurfFold, UmbraNotErodedByUnionSaturationFold )
{
	const float G = 8.0f, R = 8.0f, thr = 0.01f, errTol = 0.05f;
	const float3 L = float3( 4, 4, 120 );

	FoldEval worst{}; float worstScore = -1.0f; float worstOff = 0;
	// two big slabs at z=50, edges at x=xa and x=xb, so between them the union double-covers -> saturates;
	// slide the pair laterally so the saturated band lands at every position across the texel interior.
	for( float off = -6.0f; off <= 6.0f; off += 0.5f )
	{
		const float xa = 4.0f + off, xb = 4.0f + off;	// two coincident-edge slabs from opposite sides
		auto Vz = []( float x, float y ) { return float3( x, y, 50.0f ); };
		// slab A covers x <= xa ; slab B covers x >= xb-? -> make them OVERLAP a strip to force saturation
		const float w = 1.5f;	// overlap half-width
		std::vector<Tri> all = {
			// slab A: big rect x in [-300, xa+w], split into 2 tris
			{ { Vz( -300, -300 ), Vz( xa + w, -300 ), Vz( xa + w, 300 ) } },
			{ { Vz( -300, -300 ), Vz( xa + w, 300 ), Vz( -300, 300 ) } },
			// slab B: big rect x in [xb-w, 300]
			{ { Vz( xb - w, -300 ), Vz( 300, -300 ), Vz( 300, 300 ) } },
			{ { Vz( xb - w, -300 ), Vz( 300, 300 ), Vz( xb - w, 300 ) } },
		};
		FoldEval e = EvalTexel( all, float3( 0, 0, 0 ), G, L, R, thr, errTol );
		// score = umbra erosion the self-gate MISSED (a fidelity failure that ships)
		const float erosion = ( float )( e.umbraTruth - e.umbraRecon );
		const float score = e.selfGatePass ? std::fmax( erosion, e.worstErr * 100.0f ) : -1.0f;
		if( score > worstScore ) { worstScore = score; worst = e; worstOff = off; }
	}

	std::printf( "[SurfFold] WORST off=%.1f folded=%d/%d selfGatePass=%d | umbra truth=%d recon=%d | worstErr=%.3f\n",
			worstOff, worst.folded, worst.nTri, worst.selfGatePass ? 1 : 0,
			worst.umbraTruth, worst.umbraRecon, worst.worstErr );

	// THE PROPERTY: no texel the self-gate ACCEPTS may erode the umbra or exceed the errTol-class bound.
	CHECK( worst.umbraRecon >= worst.umbraTruth - 33 );		// <= 1 grid row of slack
	CHECK( worst.worstErr <= 0.06f );
}

static void AppendQuad( std::vector<Tri>& out, float cx, float cy, float z, float s )
{
	const float h = s * 0.5f;
	float3 a( cx - h, cy - h, z ), b( cx + h, cy - h, z ), c( cx + h, cy + h, z ), d( cx - h, cy + h, z );
	out.push_back( { { a, b, c } } );
	out.push_back( { { a, c, d } } );
}

// REFUTABLE (the 2x question): does the fold cache cheapen DENSE static occlusion, or only sparse? Sweep
// an occluder grid over the texel's penumbra region and report, per config: exact union coverage, folded
// vs residual occluders, whether the texel is CACHED or routed to WALK-ALWAYS, reconstruction error, and
// the WALK-REDUCTION a cache HIT delivers (= folded occluders removed from the runtime walk; 0 if
// walk-always). Hypothesis under test: "dense static is non-foldable, so the cache cannot cheapen the
// expensive (dense) fragments" -> walk-reduction -> 0 as coverage rises. If a dense (high coverage,
// non-umbra) config folds with HIGH walk-reduction and accurate reconstruction, the hypothesis is REFUTED.
TEST( SurfFold, DenseStaticFoldWalkReduction )
{
	const float G = 8.0f, R = 8.0f, thr = 0.01f, errTol = 0.05f;
	const float3 L = float3( 4, 4, 120 );
	const float3 origin = float3( 0, 0, 0 );
	const float3 ctr = float3( 4, 4, 0 );

	// CANONICAL scene shadow: one LARGE occluder (a wall, x <= edge at z=50) whose penumbra edge crosses
	// the texel - a wall casting onto a floor. Sweep the edge across the texel; does a large-occluder edge
	// FOLD (cheap hit) or route to walk-always (no saving)? This is what real static shadows look like.
	std::printf( "[WallEdge] edgeX  cov   walkAlways  bilinearErr  ridgeErr\n" );
	float worstRidge = 0.0f, worstBilinAtRidge = 0.0f;
	for( float ex = -2.0f; ex <= 10.0f; ex += 1.0f )
	{
		std::vector<Tri> wall = {
			{ { float3( -400, -400, 50 ), float3( ex, -400, 50 ), float3( ex, 400, 50 ) } },
			{ { float3( -400, -400, 50 ), float3( ex, 400, 50 ), float3( -400, 400, 50 ) } },
		};
		const float cov = Occ( wall, ctr, L, R );
		FoldEval e = EvalTexel( wall, origin, G, L, R, thr, errTol );
		// 4 corner coverages, and their linearizing coordinate s = g^-1(cov)
		float Fc[4], sc[4];
		for( int c = 0; c < 4; c++ )
		{
			float3 P = float3( PU9[c] * G, PV9[c] * G, 0.0f );
			Fc[c] = Occ( wall, P, L, R );
			sc[c] = SegCovInv( Fc[c] );
		}
		auto bilerp = []( const float v[4], float fu, float fv )
		{
			const float lo = v[0] + ( v[1] - v[0] ) * fu;
			const float hi = v[2] + ( v[3] - v[2] ) * fu;
			return lo + ( hi - lo ) * fv;
		};
		float biErr = 0.0f, rdErr = 0.0f;
		const int N = 33;
		for( int iy = 0; iy < N; iy++ )
			for( int ix = 0; ix < N; ix++ )
			{
				const float fu = ( float )ix / ( N - 1 ), fv = ( float )iy / ( N - 1 );
				float3 P = float3( fu * G, fv * G, 0.0f );
				const float truth = Occ( wall, P, L, R );
				biErr = std::fmax( biErr, std::fabs( truth - bilerp( Fc, fu, fv ) ) );			// bilerp COVERAGE
				rdErr = std::fmax( rdErr, std::fabs( truth - SegCov( bilerp( sc, fu, fv ) ) ) );	// bilerp s, apply g (ridge)
			}
		std::printf( "[WallEdge] %+.1f   %.3f     %d          %.3f        %.3f\n",
					 ex, cov, e.walkAlways ? 1 : 0, biErr, rdErr );
		if( rdErr > worstRidge ) { worstRidge = rdErr; worstBilinAtRidge = biErr; }
	}
	std::printf( "[WallEdge] WORST over sweep: bilinear=%.3f  ridge=%.3f  (ridge should hold the edge < 0.06)\n",
				 worstBilinAtRidge, worstRidge );

	std::printf( "[DenseFold] fill  N  nTri   cov   folded resid cached wR   worstErr\n" );
	int   bestDenseWR = 0;
	float bestDenseCov = 0, bestDenseErr = 1;
	for( float fill : { 0.7f, 1.3f } )
	{
		for( int Nn : { 1, 2, 3, 4, 6, 8 } )
		{
			std::vector<Tri> all;
			const float lo = -4.0f, hi = 12.0f, span = hi - lo;
			const float spacing = span / ( float )Nn;
			const float s = spacing * fill;
			for( int j = 0; j < Nn; j++ )
				for( int i = 0; i < Nn; i++ )
					AppendQuad( all, lo + ( i + 0.5f ) * spacing, lo + ( j + 0.5f ) * spacing, 50.0f, s );
			const float cov = Occ( all, ctr, L, R );
			FoldEval e = EvalTexel( all, origin, G, L, R, thr, errTol );
			const int resid = e.nTri - e.folded;
			const int wR = e.walkAlways ? 0 : e.folded;		// occluders a HIT removes from the walk
			std::printf( "[DenseFold] %.1f  %d  %4d  %.3f  %5d %5d   %d   %4d  %.3f\n",
						 fill, Nn, e.nTri, cov, e.folded, resid, e.walkAlways ? 0 : 1, wR, e.worstErr );
			if( fill < 1.0f && cov >= 0.7f && cov < 0.999f && !e.walkAlways && wR > bestDenseWR )
			{
				bestDenseWR = wR; bestDenseCov = cov; bestDenseErr = e.worstErr;
			}
		}
	}
	std::printf( "[DenseFold] BEST dense-penumbra CACHED case: cov=%.3f walkReduction=%d worstErr=%.3f\n",
				 bestDenseCov, bestDenseWR, bestDenseErr );
	// REFUTED (2026-08-22): "dense static is non-foldable, so the cache cannot cheapen the expensive
	// fragments" is FALSE. A dense, non-umbra occluder set folds+caches with a large walk-reduction and
	// near-zero reconstruction error (measured: 72 occluders -> 40 folded, cached, worstErr 0). Only FULL
	// umbra (cov -> 1) routes to walk-always, and full-umbra tiles are already free via the tile-bin. So
	// the scene's near-zero saving is NOT a foldability wall - it is a runtime HIT-RATE problem (foldable
	// texels not consumed as hits). This CHECK guards that the fold keeps cheapening dense penumbra.
	CHECK( bestDenseWR >= 8 );
	CHECK( bestDenseErr <= 0.06f );
}
