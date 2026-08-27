/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 2026 Aleksandr Petrosyan

This file is part of the Doom 3 BFG Edition GPL Source Code.

===========================================================================
*/

// RADIAL-MAX soft-shadow coverage primitive + parity/eligibility proof.
//
// MOTIVATION. Soft shadow at a fragment = 1 - |union of casters' blocked disk regions| / (pi R^2).
// SoftShadow_WedgeOcclusion computes a per-caster analytic AREA and MAX-combines casters - which is NOT a
// set union: two casters covering different halves of the disk each read ~0.5, max stays 0.5, but the real
// union is ~1.0. The RADIAL-MAX representation unions correctly WHEN every caster's blocked region is
// star-shaped about the disk centre O and contains O (proof: a convex silhouette containing O is star-shaped
// from O with ray-coverage [0, r_i(theta)]; the union of star-shaped-from-O sets is star-shaped from O with
// ray-coverage [0, max_i r_i(theta)]). Coverage is then an exact 1-D angular integral of the outer radius.
//
// PRIMITIVE. rad[K] = outer blocked radius per angular bin over [0,2pi). Per projected silhouette edge
// (q0,q1 in light-disk coords of radius swR, projected EXACTLY as SoftShadow_WedgeOcclusion does -
// SoftShadow_ClipSlab -> SoftShadow_ProjectVert), the ray from O in each bin direction d=(cos,sin theta)
// that crosses the segment forward contributes r = dot(hit,d); rad[bin] = max(rad[bin], r) unions all
// casters. Coverage = sum_k min(rad[k],R)^2 / (K R^2). min_k rad[k] >= R => full disk => coverage 1 (umbra).
//
// This file compiles the LIVE shader source (softwedge_coverage.inc.hlsl) as C++ for the projection math, so
// the eligible-case parity is against the same SoftShadow_ProjectVert the GPU runs. The radial primitive
// below is written in single-component (.x/.y) style so it can be lifted verbatim into the .inc.hlsl as
// SoftShadow_RadialOcclusion; it is standalone here to avoid touching the shipped 3433-line shader.

#include "hlsl_compat.h"
#include "softwedge_coverage.inc.hlsl"		// SW_SCANLINE defaults 0: pulls Frame / ClipSlab / ProjectVert / WedgeOcclusion
#include "SoftShadowBox.h"
#include "idUnitTest.h"

#include <vector>
#include <cstdio>
#include <cmath>

using namespace swtest;

namespace
{
// ------------------------------------------------------------------ the RADIAL-MAX primitive
#define RAD_K 32			// angular bins over [0,2pi); array size. Parity plateau picks 16 vs 32 (see kPlateau test).
const double RAD_2PI = 6.283185307179586;

// Accumulate ONE projected silhouette edge (q0->q1 in disk coords, units of swR) into rad[K]. For every bin
// whose ray from O crosses the segment forward (s in [0,1], r>0), rad[bin] = max(rad[bin], r). Scanning all K
// bins per edge is the register-safe (bin-outer) form written as a per-edge helper; the arc-limited form
// (only the ceil(K*|span|/2pi) bins the edge subtends) does the same work with fewer intersection solves - see
// the COST/REGISTER note in radialCostNote. rad[] is dynamically indexed (rad[bin] write) - the scratch risk.
void RadialAccumEdge( float rad[RAD_K], float2 q0, float2 q1, int K )
{
	float2 e = q1 - q0;
	for( int k = 0; k < K; k++ )
	{
		double th = ( k + 0.5 ) * ( RAD_2PI / K );
		float  dx = ( float )std::cos( th ), dy = ( float )std::sin( th );
		float  den = e.x * dy - e.y * dx;					// cross(q1-q0, d)
		if( abs( den ) < 1e-20f ) { continue; }				// edge parallel to the ray: no crossing
		float  s = -( q0.x * dy - q0.y * dx ) / den;		// -cross(q0,d)/cross(q1-q0,d)
		if( s < 0.0f || s > 1.0f ) { continue; }			// crossing is off the segment
		float  hx = q0.x + s * e.x, hy = q0.y + s * e.y;
		float  r = hx * dx + hy * dy;						// dot(hit, d)
		if( r > rad[k] ) { rad[k] = r; }					// forward (r>0) outer radius; max unions casters
	}
}

// Finalize rad[K] -> occluded fraction in [0,1]. Clamp each radius to R (blocked region past the disk edge is
// outside the light and contributes nothing); umbra iff every bin reaches R.
float RadialFinalize( const float rad[RAD_K], float R, int K, bool* umbra )
{
	double sum = 0.0; bool full = true;
	for( int k = 0; k < K; k++ )
	{
		float rc = rad[k] < R ? rad[k] : R;
		if( rad[k] < R ) { full = false; }
		sum += ( double )rc * rc;
	}
	if( umbra ) { *umbra = full; }
	return saturate( ( float )( sum / ( ( double )K * R * R ) ) );
}

// Full driver: walk a set of caster silhouette loops (world verts, closed), project each edge EXACTLY as
// SoftShadow_WedgeOcclusion (Frame/ClipSlab/ProjectVert), union into rad[K], finalize. This is the radial
// analogue of SoftShadow_WedgeOcclusion, and the union across loops is a real set union (the wedge's flaw).
float RadialOcclusion( const std::vector<std::vector<float3>>& loops, float3 P, float3 L, float R, int K, bool* umbra = nullptr )
{
	R = max( R, 1e-2f );
	softFrame_t F = SoftShadow_Frame( P, L );
	const float eps = SW_NEAR_EPS;
	float rad[RAD_K]; for( int k = 0; k < K; k++ ) { rad[k] = 0.0f; }
	for( const auto& loop : loops )
	{
		int m = ( int )loop.size();
		for( int i = 0; i < m; i++ )
		{
			float3 A = loop[i], B = loop[( i + 1 ) % m];
			float3 a = A - P, b = B - P;
			float  dnA = dot( a, F.nrm ), dnB = dot( b, F.nrm );
			softClip_t cl = SoftShadow_ClipSlab( dnA, dnB, eps, F.distPL );
			if( cl.empty ) { continue; }
			float  d = dnB - dnA;
			float3 pa = a + cl.t0 * ( b - a ), pb = a + cl.t1 * ( b - a );
			float  dna = dnA + cl.t0 * d, dnb = dnA + cl.t1 * d;
			float2 q0 = SoftShadow_ProjectVert( pa, dna, F );
			float2 q1 = SoftShadow_ProjectVert( pb, dnb, F );
			RadialAccumEdge( rad, q0, q1, K );
		}
	}
	return RadialFinalize( rad, R, K, umbra );
}

// ------------------------------------------------------------------ independent oracles
// 2D crossing-number point-in-polygon (borrowed from SoftShadowDivergence_test).
bool PtInPoly( const std::vector<float2>& p, float x, float y )
{
	bool in = false; int n = ( int )p.size();
	for( int a = 0, bpt = n - 1; a < n; bpt = a++ )
	{
		if( ( ( p[a].y > y ) != ( p[bpt].y > y ) ) &&
				( x < ( p[bpt].x - p[a].x ) * ( y - p[a].y ) / ( p[bpt].y - p[a].y ) + p[a].x ) )
		{
			in = !in;
		}
	}
	return in;
}

// project a world loop to light-disk coords (units of swR) exactly as the primitive does (no clip: callers
// keep casters fully in front of the receiver). This is the radial primitive's INPUT, so a disk-integral over
// these polygons is an oracle that isolates the radial approximation (angular binning + star-shape) from any
// 3D projection error - independent of the coverage math under test.
std::vector<float2> ProjectLoop( const std::vector<float3>& loop, float3 P, softFrame_t F )
{
	std::vector<float2> q;
	for( float3 V : loop )
	{
		float3 rel = V - P;
		q.push_back( SoftShadow_ProjectVert( rel, dot( rel, F.nrm ), F ) );
	}
	return q;
}

// exact 2D disk occlusion: fraction of the disk (radius R about O) inside the UNION of the projected polygons.
// This is the ground truth for whatever the projected silhouettes actually block, convex or concave. N*N grid.
float Poly2DDiskOcclusion( const std::vector<std::vector<float2>>& polys, float R, int N = 400 )
{
	long inside = 0, total = 0;
	for( int iy = 0; iy < N; iy++ )
		for( int ix = 0; ix < N; ix++ )
		{
			float x = ( ( ix + 0.5f ) / N * 2 - 1 ) * R, y = ( ( iy + 0.5f ) / N * 2 - 1 ) * R;
			if( x * x + y * y > R * R ) { continue; }
			total++;
			bool hit = false;
			for( const auto& p : polys ) { if( PtInPoly( p, x, y ) ) { hit = true; break; } }
			if( hit ) { inside++; }
		}
	return total ? ( float )inside / total : 0.0f;
}

// 3D ray-cast disk occlusion over MANY boxes (blocked if ANY box hit) - the multi-caster analogue of
// TruthShadow. Returns occlusion (1 = umbra), matching RadialOcclusion's sign.
float TruthShadowMultiOcc( float3 P, float3 L, float R, const std::vector<Box>& boxes, int N = 96 )
{
	float3 toL = L - P; float dist = len3( toL ); float3 nrm = toL * ( 1.0f / dist );
	float3 up = ( std::fabs( nrm.z ) > 0.9f ) ? v3( 0, 1, 0 ) : v3( 0, 0, 1 );
	float3 u = normalize( cross( up, nrm ) ), v = cross( nrm, u );
	int inside = 0, total = 0;
	for( int iy = 0; iy < N; iy++ )
		for( int ix = 0; ix < N; ix++ )
		{
			float du = ( ix + 0.5f ) / N * 2 - 1, dv = ( iy + 0.5f ) / N * 2 - 1;
			if( du * du + dv * dv > 1.0f ) { continue; }
			total++;
			float3 Dp = L + u * ( du * R ) + v * ( dv * R );
			for( const Box& b : boxes ) { if( RayHitsBox( P, Dp - P, b ) ) { inside++; break; } }
		}
	return total ? ( float )inside / total : 0.0f;
}

// wedge occlusion over multiple casters: concatenate each caster's (header+edge) records and MAX-combine, the
// shipped multi-caster behaviour. This is what radial must BEAT on overlapping casters.
float WedgeOccMulti( const std::vector<std::vector<float3>>& sils, float3 P, float3 L, float R )
{
	std::vector<float4> rec;
	for( const auto& s : sils )
	{
		std::vector<float4> one = BuildCaster( { s } );
		rec.insert( rec.end(), one.begin(), one.end() );
	}
	SoftEdgeBuffer buf{ rec.data(), ( int )rec.size() };
	return saturate( SoftShadow_WedgeOcclusion( P, L, R, 0, ( int )( rec.size() / 2 ), 0.0f, buf ) );
}
}

// ============================================================================================= ELIGIBLE
TEST( SoftShadowRadial, convexParityEligible )
{
	// A convex box silhouette that CONTAINS O (centre ray blocked): radial coverage is EXACT. Swept over light
	// radius and receiver offset. Three references: (a) 2D disk-integral of the projected silhouette (isolates
	// the radial binning error), (b) TruthShadow 3D ray oracle (adds projection error), (c) the analytic wedge.
	struct Cfg { float3 C, h; float yaw, pitch; float3 P, L; float r; };
	const Cfg cfgs[] =
	{
		{ float3( 0, 0, 20 ), float3( 6, 6, 4 ), 0.0f, 0.0f, float3( 0, 0, -30 ), float3( 0, 0, 60 ),  4.0f },
		{ float3( 0, 0, 20 ), float3( 6, 6, 4 ), 0.0f, 0.0f, float3( 2, 1, -30 ), float3( 0, 0, 60 ),  8.0f },
		{ float3( 0, 0, 18 ), float3( 8, 5, 3 ), 0.4f, 0.2f, float3( 3, 2, -28 ), float3( 1, 1, 58 ), 10.0f },
		{ float3( 0, 0, 22 ), float3( 7, 7, 5 ), 0.7f, 0.3f, float3( 4, 3, -26 ), float3( 2, 1, 62 ),  6.0f },
		{ float3( 0, 0, 20 ), float3( 9, 6, 4 ), 0.0f, 0.0f, float3( 3, 2, -30 ), float3( 1, 1, 60 ),  9.0f },
	};
	double worst2D = 0, worstRT = 0, worstWedge = 0;
	for( int ci = 0; ci < ( int )( sizeof( cfgs ) / sizeof( cfgs[0] ) ); ci++ )
	{
		const Cfg& g = cfgs[ci];
		Box b = MakeBox( g.C, g.h, g.yaw, g.pitch );
		std::vector<float3> sil = Silhouette( b, g.L );
		softFrame_t F = SoftShadow_Frame( g.P, g.L );
		std::vector<float2> loop2d = ProjectLoop( sil, g.P, F );

		// eligibility: projected silhouette contains O, and the centre ray is truly blocked
		CHECK( PtInPoly( loop2d, 0.0f, 0.0f ) );
		CHECK( RayHitsBox( g.P, g.L - g.P, b ) );

		float rad = RadialOcclusion( { sil }, g.P, g.L, g.r, RAD_K );
		float o2d = Poly2DDiskOcclusion( { loop2d }, g.r );
		float rt  = 1.0f - TruthShadow( g.P, g.L, g.r, b );		// TruthShadow returns 1=lit; convert to occlusion
		std::vector<float4> rec = BuildCaster( { sil } );
		SoftEdgeBuffer buf{ rec.data(), ( int )rec.size() };
		float wedge = saturate( SoftShadow_WedgeOcclusion( g.P, g.L, g.r, 0, ( int )( rec.size() / 2 ), 0.0f, buf ) );

		worst2D = std::fmax( worst2D, std::fabs( rad - o2d ) );
		worstRT = std::fmax( worstRT, std::fabs( rad - rt ) );
		worstWedge = std::fmax( worstWedge, std::fabs( rad - wedge ) );

		CHECK_NEAR( rad, o2d, 0.02 );		// vs exact 2D union of the projected silhouette: binning error only
		CHECK_NEAR( rad, rt, 0.06 );		// vs 3D ray oracle: same tolerance FillBox/FillPoly are held to
		CHECK_NEAR( rad, wedge, 0.06 );		// single convex caster: radial and the analytic wedge agree
		CHECK( o2d > 0.05f );				// non-trivial (actually shadows)
	}
	std::printf( "  [eligible] worst |radial-2Doracle|=%.4f  |radial-RT|=%.4f  |radial-wedge|=%.4f\n",
			worst2D, worstRT, worstWedge );
}

TEST( SoftShadowRadial, multiCasterUnionBeatsWedge )
{
	// Two convex casters, both containing O, projections OVERLAPPING. Radial unions correctly (matches truth);
	// the wedge MAX-combines and cannot union - so on the disjoint-ish parts it UNDER-shadows. We prove radial
	// tracks truth within eps while the wedge's error is materially larger.
	struct Cfg { float3 Ca, ha, Cb, hb; float3 P, L; float r; };
	const Cfg cfgs[] =
	{
		// two slabs offset on +/-x, each covering somewhat past centre: union > either alone
		{ float3( -5, 0, 20 ), float3( 6, 10, 4 ), float3( 5, 0, 20 ), float3( 6, 10, 4 ), float3( 0, 0, -30 ), float3( 0, 0, 60 ), 10.0f },
		{ float3( 0, -5, 18 ), float3( 10, 6, 4 ), float3( 0, 5, 18 ), float3( 10, 6, 4 ), float3( 1, 0, -28 ), float3( 0, 0, 58 ),  9.0f },
	};
	for( int ci = 0; ci < ( int )( sizeof( cfgs ) / sizeof( cfgs[0] ) ); ci++ )
	{
		const Cfg& g = cfgs[ci];
		Box ba = MakeBox( g.Ca, g.ha ), bb = MakeBox( g.Cb, g.hb );
		std::vector<float3> sa = Silhouette( ba, g.L ), sb = Silhouette( bb, g.L );

		float rad   = RadialOcclusion( { sa, sb }, g.P, g.L, g.r, RAD_K );
		float rt    = TruthShadowMultiOcc( g.P, g.L, g.r, { ba, bb } );
		float wedge = WedgeOccMulti( { sa, sb }, g.P, g.L, g.r );

		std::printf( "  [multi] radial=%.4f  RT=%.4f  wedge=%.4f  |radial-RT|=%.4f  |wedge-RT|=%.4f\n",
				rad, rt, wedge, std::fabs( rad - rt ), std::fabs( wedge - rt ) );

		CHECK_NEAR( rad, rt, 0.06 );					// radial unions correctly
		CHECK( std::fabs( wedge - rt ) > std::fabs( rad - rt ) + 0.05 );	// wedge is materially worse (cannot union)
	}
}

// ============================================================================================ SATURATION
TEST( SoftShadowRadial, saturatesToUmbraAfterTwoCasters )
{
	// Two big slabs on opposite sides that JOINTLY fill the disk. One alone: min rad < R (not umbra), coverage
	// < 1. Both: min rad >= R => umbra signal => coverage exactly 1. Proves the union saturates after ~2 casters.
	const float3 P( 0, 0, -30 ), L( 0, 0, 60 ); const float R = 8.0f;
	Box left  = MakeBox( float3( -8, 0, 20 ), float3( 10, 12, 4 ) );	// covers the -x half (and past centre)
	Box right = MakeBox( float3(  8, 0, 20 ), float3( 10, 12, 4 ) );	// covers the +x half
	std::vector<float3> sl = Silhouette( left, L ), sr = Silhouette( right, L );

	bool umbra1 = false, umbra2 = false;
	float cov1 = RadialOcclusion( { sl }, P, L, R, RAD_K, &umbra1 );
	float cov2 = RadialOcclusion( { sl, sr }, P, L, R, RAD_K, &umbra2 );

	std::printf( "  [saturate] one: cov=%.4f umbra=%d   two: cov=%.4f umbra=%d\n", cov1, umbra1, cov2, umbra2 );

	CHECK( !umbra1 );					// one caster does not fill the disk
	CHECK( cov1 < 0.95f );
	CHECK( umbra2 );					// the union fills it: min_k rad[k] >= R
	CHECK_NEAR( cov2, 1.0f, 1e-6 );		// umbra early-out signal: coverage exactly 1
}

// ========================================================================================== NOT ELIGIBLE
TEST( SoftShadowRadial, ineligibleErrorCharacterised )
{
	// Where the star-shape-about-O precondition fails, radial OVER-fills (it keeps only the OUTER radius, so any
	// hole between O and the blocked annulus is spuriously filled). We characterise the magnitude so the gate
	// that restricts radial to eligible casters is justified by a number, not a hunch.

	// (a) CONVEX caster NOT containing O: a grazing slab whose projection is a sliver off to one side. O is
	// outside it, so the disk is only PARTIALLY shadowed - but if a ray from O passes through the sliver, radial
	// fills [0, r_outer], inventing coverage between O and the sliver.
	{
		const float3 P( 0, 0, -30 ), L( 0, 0, 60 ); const float R = 12.0f;
		Box graze = MakeBox( float3( 16, 0, 20 ), float3( 4, 10, 4 ) );	// well off +x: silhouette misses O
		std::vector<float3> s = Silhouette( graze, L );
		softFrame_t F = SoftShadow_Frame( P, L );
		std::vector<float2> loop2d = ProjectLoop( s, P, F );

		CHECK( !PtInPoly( loop2d, 0.0f, 0.0f ) );		// confirm O is OUTSIDE (ineligible)
		float rad = RadialOcclusion( { s }, P, L, R, RAD_K );
		float o2d = Poly2DDiskOcclusion( { loop2d }, R );
		std::printf( "  [ineligible-grazing] radial=%.4f  truth2D=%.4f  over-fill=%.4f\n", rad, o2d, rad - o2d );
		CHECK( rad - o2d > 0.05f );		// radial OVER-shadows by a characterised margin
	}

	// (b) CONCAVE caster (L-shape flat poly, one reflex vertex) with O in the notch region: rays through the
	// notch hit the boundary twice, radial keeps the outer hit and fills the notch gap.
	{
		const float3 O( 0, 0, 20 ), U( 1, 0, 0 ), V( 0, 1, 0 );
		auto W = [&]( float x, float y ) { return O + U * x + V * y; };
		// L-shape: [-10,10]^2 minus the [0,10]x[0,10] quadrant, reflex vertex at (0,0)
		std::vector<float3> Lshape = { W( -10, -10 ), W( 10, -10 ), W( 10, 0 ), W( 0, 0 ), W( 0, 10 ), W( -10, 10 ) };
		const float3 P( 3, 3, -30 ), Lgt( 3, 3, 60 ); const float R = 12.0f;	// receiver under the notch cut
		softFrame_t F = SoftShadow_Frame( P, Lgt );
		std::vector<float2> loop2d = ProjectLoop( Lshape, P, F );

		float rad = RadialOcclusion( { Lshape }, P, Lgt, R, RAD_K );
		float o2d = Poly2DDiskOcclusion( { loop2d }, R );		// exact area of the concave poly ∩ disk
		std::printf( "  [ineligible-concave] radial=%.4f  truth2D=%.4f  over-fill=%.4f\n", rad, o2d, rad - o2d );
		CHECK( rad - o2d > 0.02f );		// radial fills the concave notch it should have left lit
	}
}

// ========================================================================================= K / COST NOTE
TEST( SoftShadowRadial, kPlateauAndCost )
{
	// Pick K by the parity plateau: run the eligible corpus at K=16 and K=32 against the 2D oracle (binning
	// error only) and report the worst error at each. Also emit the cost/register note.
	struct Cfg { float3 C, h; float yaw, pitch; float3 P, L; float r; };
	const Cfg cfgs[] =
	{
		{ float3( 0, 0, 20 ), float3( 6, 6, 4 ), 0.0f, 0.0f, float3( 0, 0, -30 ), float3( 0, 0, 60 ),  4.0f },
		{ float3( 0, 0, 20 ), float3( 6, 6, 4 ), 0.0f, 0.0f, float3( 2, 1, -30 ), float3( 0, 0, 60 ),  8.0f },
		{ float3( 0, 0, 18 ), float3( 8, 5, 3 ), 0.4f, 0.2f, float3( 3, 2, -28 ), float3( 1, 1, 58 ), 10.0f },
		{ float3( 0, 0, 22 ), float3( 7, 7, 5 ), 0.7f, 0.3f, float3( 4, 3, -26 ), float3( 2, 1, 62 ),  6.0f },
		{ float3( 0, 0, 20 ), float3( 9, 6, 4 ), 0.0f, 0.0f, float3( 3, 2, -30 ), float3( 1, 1, 60 ),  9.0f },
	};
	const int Ks[] = { 8, 16, 32 };
	double worst[3] = { 0, 0, 0 };
	for( int ci = 0; ci < ( int )( sizeof( cfgs ) / sizeof( cfgs[0] ) ); ci++ )
	{
		const Cfg& g = cfgs[ci];
		Box b = MakeBox( g.C, g.h, g.yaw, g.pitch );
		std::vector<float3> sil = Silhouette( b, g.L );
		softFrame_t F = SoftShadow_Frame( g.P, g.L );
		float o2d = Poly2DDiskOcclusion( { ProjectLoop( sil, g.P, F ) }, g.r );
		for( int ki = 0; ki < 3; ki++ )
		{
			float rad = RadialOcclusion( { sil }, g.P, g.L, g.r, Ks[ki] );
			worst[ki] = std::fmax( worst[ki], std::fabs( rad - o2d ) );
		}
	}
	std::printf( "  [K plateau] worst |radial-2Doracle|:  K=8: %.4f   K=16: %.4f   K=32: %.4f\n",
			worst[0], worst[1], worst[2] );
	std::printf( "  [cost] rad[K] = K floats (K=32 -> 128B) dynamically indexed on the max-write (rad[bin]) -\n" );
	std::printf( "         the same scratch-spill risk that killed FillPolyConcave. Bin-outer reformulation\n" );
	std::printf( "         (rad_k a scalar, loop edges inside) removes the dynamic index at O(K*E) intersections.\n" );
	std::printf( "         Per edge: arc-limited = ceil(K*|span|/2pi) intersection solves; all-bin = K. The\n" );
	std::printf( "         analytic wedge is O(1) area accumulation per edge - radial trades that O(1) for O(K).\n" );

	CHECK( worst[2] <= 0.02 );		// K=32 within the 2D-oracle binning tolerance
	CHECK( worst[1] <= 0.03 );		// K=16 close behind: the plateau starts by 16
}
