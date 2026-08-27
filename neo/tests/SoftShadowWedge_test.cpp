/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 2026 Aleksandr Petrosyan

This file is part of the Doom 3 BFG Edition GPL Source Code.

===========================================================================
*/

// PENUMBRA-WEDGE characterization vs the ray-cast area-light truth. The method under test is
// SoftShadow_WedgeOcclusion (softwedge_coverage.inc.hlsl): it walks a caster's silhouette EDGE loop,
// projects each vertex through the RECEIVER onto the light disk, and sums per-edge SIGNED
// circle-triangle areas (SoftDisk_CircleTriArea) around the closed loop. occ = |sum| / disk-area.
//
// SUSPECTED failure (the task hypothesis): at silhouette VERTICES adjacent edge-wedges leave a GAP at
// a convex corner (under-shadow) and OVERLAP at a concave corner (over-shadow), the error scaling with
// corner turning-angle / sharpness.
//
// This TU MEASURES that. It isolates the corner integral with a PLANAR CARD occluder (a flat polygon
// whose outline is view-independent, so the silhouette handed to the wedge is EXACTLY the true occluding
// contour - no 3D silhouette-selection error contaminates the corner measurement). Ground truth is an
// independent ray-cast of the disk against the card's plane + point-in-polygon (crossing number), which
// shares NO math with the signed-area integral. A faithful C++ re-mirror of the wedge loop (calling the
// SAME shader primitives) is validated bit-close against the live shader, then a candidate CORNER FAN
// fix is bolted on to see whether it drives the corner error to zero or injects it.
//
// A second sweep drives a real 3D BOX through the LIVE shader vs the box ray-truth to locate where the
// wedge actually diverges (the light-picked / receiver-projected silhouette-selection error) and to
// confirm it stays accurate in the LARGE-light (large sinA) regime where the static-silhouette method
// fails - the composition value of the wedge covering the silhouette method's blind spot.

#include "hlsl_compat.h"
#include "softwedge_coverage.inc.hlsl"		// the LIVE shader source, compiled as C++ (SW_FUNC = inline)
#include "SoftShadowBox.h"					// MakeBox / Silhouette / TruthShadow / BuildCaster / LiveShadow
#include "idUnitTest.h"

#include <vector>
#include <cmath>
#include <cstdio>

using namespace swtest;

namespace
{

// ---------------------------------------------------------------- planar-card ground truth (independent)
// A card = closed loop of world verts in the plane z = zc (facing +z toward the light). Ground-truth
// occlusion of the disk (centre L, radius r, facing P): fraction of disk-sample rays P->Dp that pierce
// the card polygon. Crossing-number point-in-polygon over the loop's XY - shares no math with the wedge.
bool PtInPolyXY( const std::vector<float3>& poly, float x, float y )
{
	bool in = false;
	int n = ( int )poly.size();
	for( int a = 0, b = n - 1; a < n; b = a++ )
	{
		if( ( ( poly[a].y > y ) != ( poly[b].y > y ) ) &&
				( x < ( poly[b].x - poly[a].x ) * ( y - poly[a].y ) / ( poly[b].y - poly[a].y ) + poly[a].x ) )
		{
			in = !in;
		}
	}
	return in;
}

float CardTruthOcc( const std::vector<float3>& loop, float3 P, float3 L, float r, int N )
{
	float3 toL = L - P;
	float dist = len3( toL );
	float3 nrm = toL * ( 1.0f / dist );
	float3 up = ( std::fabs( nrm.z ) > 0.9f ) ? v3( 0, 1, 0 ) : v3( 0, 0, 1 );
	float3 u = normalize( cross( up, nrm ) );
	float3 v = cross( nrm, u );
	const float zc = loop[0].z;					// card plane (all verts share z)
	int inside = 0, total = 0;
	for( int iy = 0; iy < N; iy++ )
		for( int ix = 0; ix < N; ix++ )
		{
			float du = ( ix + 0.5f ) / N * 2 - 1, dv = ( iy + 0.5f ) / N * 2 - 1;
			if( du * du + dv * dv > 1.0f ) { continue; }
			total++;
			float3 Dp = L + u * ( du * r ) + v * ( dv * r );
			float dz = Dp.z - P.z;
			if( std::fabs( dz ) < 1e-9f ) { continue; }
			float t = ( zc - P.z ) / dz;			// ray P->Dp meets card plane
			if( t <= 0.0f || t >= 1.0f ) { continue; }
			float3 X = P + ( Dp - P ) * t;
			if( PtInPolyXY( loop, X.x, X.y ) ) { inside++; }
		}
	return total ? ( float )inside / total : 0.0f;	// OCCLUSION (0 = lit)
}

// ------------------------------------------------------------ faithful re-mirror of the wedge inner loop
// Single closed loop, one caster, swCentreLit = 0. Calls the SAME shader primitives (SoftShadow_Frame,
// SoftShadow_ClipSlab, SoftShadow_ProjectVert, SoftDisk_CircleTriArea) as SoftShadow_WedgeOcclusion, so
// (cornerMode 0) it must reproduce the live shader. cornerMode 1 adds the candidate CORNER FAN: a signed
// sector 0.5*r2*turn at each projected vertex (what a "fix" would add if adjacent edges were independent
// shadow fins that gap/overlap at corners by the turning angle). edgeCount returns edges walked.
float WedgeOccLoop( const std::vector<float3>& loop, float3 P, float3 L, float r, int cornerMode, long* edgeCount )
{
	r = max( r, 1e-2f );
	softFrame_t F = SoftShadow_Frame( P, L );
	float r2 = r * r;
	float invA = 1.0f / ( PI * r2 );
	const int m = ( int )loop.size();

	float area = 0.0f;
	bool firstValid = false;
	float2 first( 0, 0 ), prev( 0, 0 );
	std::vector<float2> proj;					// kept clipped-projected verts (for the corner-fan pass)
	proj.reserve( m );

	for( int i = 0; i < m; i++ )
	{
		float3 A = loop[i], B = loop[( i + 1 ) % m];
		float3 a = A - P, b = B - P;
		float dnA = dot( a, F.nrm ), dnB = dot( b, F.nrm ), d = dnB - dnA;
		softClip_t cl = SoftShadow_ClipSlab( dnA, dnB, SW_NEAR_EPS, F.distPL );
		if( cl.empty ) { continue; }
		float2 q0 = SoftShadow_ProjectVert( a + cl.t0 * ( b - a ), dnA + cl.t0 * d, F );
		float2 q1 = SoftShadow_ProjectVert( a + cl.t1 * ( b - a ), dnA + cl.t1 * d, F );
		if( firstValid ) { area += SoftDisk_CircleTriArea( prev, q0, r2 ); }
		else             { first = q0; firstValid = true; }
		area += SoftDisk_CircleTriArea( q0, q1, r2 );
		prev = q1;
		proj.push_back( q0 );
		if( edgeCount ) { ( *edgeCount )++; }
	}
	if( firstValid ) { area += SoftDisk_CircleTriArea( prev, first, r2 ); }

	if( cornerMode == 1 )
	{
		// CANDIDATE CORNER FAN: at each projected vertex add a signed circular sector spanning the
		// TURNING angle between the incoming and outgoing edge directions. This is the correction that
		// WOULD be required if the per-edge terms were union-of-fins that gap (convex) / overlap
		// (concave) at the corner. Measured effect: it moves the result AWAY from truth (the base sum
		// already tiles the corner exactly), which is itself the evidence.
		int k = ( int )proj.size();
		for( int i = 0; i < k; i++ )
		{
			float2 vi = proj[i];
			float2 vPrev = proj[( i + k - 1 ) % k];
			float2 vNext = proj[( i + 1 ) % k];
			float2 din = vi - vPrev;		// incoming edge direction
			float2 dout = vNext - vi;		// outgoing edge direction
			float turn = SoftDisk_EdgeAngle( din, dout );		// signed turning angle at the corner
			area += 0.5f * r2 * turn;								// naive per-corner sector
		}
	}
	return saturate( std::fabs( area ) * invA );
}

// --------------------------------------------------------------------------------- card shape factory
// All cards live in z = zc, wound CCW, roughly centred at origin, "scale" = characteristic half-extent.
std::vector<float3> CardSquare( float zc, float s )
{
	return { v3( -s, -s, zc ), v3( s, -s, zc ), v3( s, s, zc ), v3( -s, s, zc ) };
}
std::vector<float3> CardNgon( float zc, float s, int n )		// convex regular n-gon (corner sharpness ~ 1/n)
{
	std::vector<float3> l;
	for( int i = 0; i < n; i++ )
	{
		float a = 2.0f * PI * i / n;
		l.push_back( v3( s * std::cos( a ), s * std::sin( a ), zc ) );
	}
	return l;
}
std::vector<float3> CardTriangle( float zc, float s )			// sharp convex corners (60 deg)
{
	return CardNgon( zc, s, 3 );
}
std::vector<float3> CardL( float zc, float s )					// L-notch: ONE concave (reflex) corner
{
	// unit L in [0,2]x[0,2] minus the [1,2]x[1,2] quadrant, recentred and scaled
	std::vector<float3> u = { v3( 0, 0, 0 ), v3( 2, 0, 0 ), v3( 2, 1, 0 ), v3( 1, 1, 0 ), v3( 1, 2, 0 ), v3( 0, 2, 0 ) };
	std::vector<float3> l;
	for( float3 p : u ) { l.push_back( v3( ( p.x - 1.0f ) * s, ( p.y - 1.0f ) * s, zc ) ); }
	return l;											// the vertex at (0,0)->(-s? ) index 3 is the reflex corner
}
std::vector<float3> CardStar( float zc, float s, int points )	// star: alternating sharp convex + concave corners
{
	std::vector<float3> l;
	for( int i = 0; i < points * 2; i++ )
	{
		float a = PI * i / points;
		float rr = ( i & 1 ) ? s * 0.42f : s;		// inner radius -> deep reflex corners
		l.push_back( v3( rr * std::cos( a ), rr * std::sin( a ), zc ) );
	}
	return l;
}

// live-shader occlusion for a single card loop (via BuildCaster + SoftShadow_WedgeOcclusion)
float CardLiveOcc( const std::vector<float3>& loop, float3 P, float3 L, float r )
{
	std::vector<std::vector<float3>> loops{ loop };
	std::vector<float4> rec = BuildCaster( loops );
	SoftEdgeBuffer buf{ rec.data(), ( int )rec.size() };
	return saturate( SoftShadow_WedgeOcclusion( P, L, r, 0, ( int )( rec.size() / 2 ), 0.0f, buf ) );
}

struct ErrStat { double maxAbs = 0, sumSgn = 0; int n = 0; float3 worstP = float3( 0, 0, 0 ); };
void Accum( ErrStat& e, float ana, float truth, float3 P )
{
	double s = ( double )ana - truth;
	e.sumSgn += s; e.n++;
	if( std::fabs( s ) > e.maxAbs ) { e.maxAbs = std::fabs( s ); e.worstP = P; }
}

} // namespace

// ===================================================================================================
// STEP 1+2+3: corner isolation on planar cards. Faithful mirror validated == live shader; then the error
// surface (shape x light radius x off-axis x sharpness) vs ray truth, testing the corner hypothesis.
TEST( SoftShadowWedge, corner_isolation_planar_cards )
{
	const float zc = 0.0f;			// card plane
	const float Pz = -30.0f;		// receiver plane (below)
	const int   NT = 160;			// disk ray-truth samples per axis (2D card truth is cheap -> low noise)

	struct Shape { const char* name; std::vector<float3> loop; int concaveCorners; };
	std::vector<Shape> shapes =
	{
		{ "square(4,convex)",     CardSquare( zc, 6.0f ),        0 },
		{ "triangle(3,sharp cx)", CardTriangle( zc, 7.0f ),      0 },
		{ "octagon(8,soft cx)",   CardNgon( zc, 6.0f, 8 ),       0 },
		{ "16gon(smooth cx)",     CardNgon( zc, 6.0f, 16 ),      0 },
		{ "L-notch(1 concave)",   CardL( zc, 6.0f ),             1 },
		{ "star5(5 concave)",     CardStar( zc, 7.0f, 5 ),       5 },
		{ "star8(8 concave)",     CardStar( zc, 7.0f, 8 ),       8 },
	};
	// light radius (penumbra size) and receiver off-axis sweeps
	const float radii[]   = { 2.0f, 6.0f, 14.0f, 28.0f };			// small .. large (sinA up to ~0.46 at dist 60)
	const float offAxis[] = { 0.0f, 8.0f, 20.0f, 40.0f };			// 0 .. grazing

	std::printf( "\n  [WEDGE corner isolation | planar card, ray-truth N=%d]\n", NT );
	std::printf( "  %-22s %6s %7s | %-28s | %-28s | mirror==shader\n",
				 "shape", "R", "offX", "BASE err (maxAbs, meanSgn)", "with CORNER-FAN (maxAbs,meanSgn)" );

	double gMirror = 0.0, gBaseMax = 0.0, gFanMax = 0.0;
	double baseConvexMax = 0.0, baseConcaveMax = 0.0;
	for( Shape& sh : shapes )
	{
		for( float R : radii )
		{
			ErrStat base, fan; double mirrorMax = 0.0;
			for( float ox : offAxis )
			{
				float3 P( ox, 0.0f, Pz );
				float3 L( 0.0f, 0.0f, 60.0f );
				float truth = CardTruthOcc( sh.loop, P, L, R, NT );
				float live  = CardLiveOcc( sh.loop, P, L, R );
				float mine  = WedgeOccLoop( sh.loop, P, L, R, 0, nullptr );
				float mineFan = WedgeOccLoop( sh.loop, P, L, R, 1, nullptr );
				mirrorMax = std::fmax( mirrorMax, std::fabs( ( double )live - mine ) );
				Accum( base, mine, truth, P );
				Accum( fan, mineFan, truth, P );
			}
			double baseMean = base.n ? base.sumSgn / base.n : 0.0;
			double fanMean  = fan.n ? fan.sumSgn / fan.n : 0.0;
			std::printf( "  %-22s %6.0f %7s | max %.4f  mean %+.4f       | max %.4f  mean %+.4f       | d=%.2e\n",
						 sh.name, R, "sweep", base.maxAbs, baseMean, fan.maxAbs, fanMean, mirrorMax );
			gMirror = std::fmax( gMirror, mirrorMax );
			gBaseMax = std::fmax( gBaseMax, base.maxAbs );
			gFanMax  = std::fmax( gFanMax, fan.maxAbs );
			if( sh.concaveCorners == 0 ) { baseConvexMax = std::fmax( baseConvexMax, base.maxAbs ); }
			else                         { baseConcaveMax = std::fmax( baseConcaveMax, base.maxAbs ); }
		}
	}
	std::printf( "  ---- worst mirror|live-mine|=%.2e  base maxAbs convex=%.4f concave=%.4f  corner-fan maxAbs=%.4f\n",
				 gMirror, baseConvexMax, baseConcaveMax, gFanMax );

	// (1) The C++ re-mirror reproduces the LIVE shader (faithfulness of the measurement instrument).
	CHECK( gMirror < 2e-3 );
	// (2) CORNER HYPOTHESIS: if adjacent edge-wedges gapped/overlapped at corners, the concave shapes
	//     (star8 = 8 reflex corners) would carry a large corner-scaled error. They do not - the base
	//     circle-polygon integral is exact for BOTH convex and concave loops to the ray-truth noise floor.
	CHECK( gBaseMax < 0.02 );			// exact everywhere (only disk-sampling quantum remains)
	CHECK( baseConcaveMax < 0.02 );		// concave corners are NOT an over-shadow source (refutes the hypothesis)
	// (3) The candidate CORNER FAN does NOT reduce the (already ~zero) error - it injects it. A corner
	//     fix has no headroom because there is no corner error: red-until-green converges trivially.
	CHECK( gFanMax > gBaseMax );		// the "fix" only makes it worse -> nothing to fix
}

// ===================================================================================================
// STEP 3(cont): the REAL wedge error surface on a 3D box (light-picked silhouette, receiver-projected),
// and the LARGE-light composition value vs the static-silhouette sinA failure.
TEST( SoftShadowWedge, box3d_error_and_large_light )
{
	std::printf( "\n  [WEDGE 3D box vs ray-truth | error vs light angular size]\n" );
	std::printf( "  %-14s %6s %6s | %-26s\n", "config", "R", "sinA", "wedge err (maxAbs, meanSgn)" );

	struct Cfg { const char* name; float3 C, h; float3 Lc; };
	const Cfg cfgs[] =
	{
		{ "cube",      float3( 0, 0, 0 ), float3( 6, 6, 6 ),  float3( 0, 0, 60 ) },
		{ "slab(thin)",float3( 0, 0, 0 ), float3( 10, 10, 2 ),float3( 0, 0, 60 ) },
		{ "tall",      float3( 0, 0, 0 ), float3( 5, 5, 14 ), float3( 0, 0, 60 ) },
	};
	const float radii[] = { 2.0f, 6.0f, 14.0f, 28.0f, 44.0f };		// sinA up to ~0.7
	const float Pz = -30.0f;
	double smallRmax = 0.0;				// worst |err| at small light (sinA < 0.10)
	double largeRworstMean = 0.0;		// most-negative signed mean at large light (undershoot magnitude)
	for( const Cfg& cf : cfgs )
	{
		for( float R : radii )
		{
			Box b = MakeBox( cf.C, cf.h, 0.0f, 0.0f );
			ErrStat e;
			float dist = 0;
			for( float ox : { 0.0f, 6.0f, 14.0f, 26.0f } )
			{
				float3 P( ox, 0.0f, Pz );
				float3 L = cf.Lc;
				dist = len3( L - P );
				std::vector<float3> loop = Silhouette( b, L );
				if( loop.size() < 3 ) { continue; }
				std::vector<std::vector<float3>> loops{ loop };
				std::vector<float4> rec = BuildCaster( loops );
				SoftEdgeBuffer buf{ rec.data(), ( int )rec.size() };
				float ana = saturate( SoftShadow_WedgeOcclusion( P, L, R, 0, ( int )( rec.size() / 2 ), 0.0f, buf ) );
				float truth = 1.0f - TruthShadow( P, L, R, b, 96 );		// TruthShadow returns lit-fraction
				Accum( e, ana, truth, P );
			}
			float sinA = R / dist; if( sinA > 1 ) { sinA = 1; }
			double mean = e.n ? e.sumSgn / e.n : 0.0;
			std::printf( "  %-14s %6.0f %6.3f | max %.4f  mean %+.4f\n", cf.name, R, sinA, e.maxAbs, mean );
			if( sinA < 0.10 ) { smallRmax = std::fmax( smallRmax, e.maxAbs ); }
			if( R >= 28.0f )  { largeRworstMean = std::fmin( largeRworstMean, mean ); }
		}
	}
	// MEASURED characterization (not a forced bound):
	//  - the wedge is EXACT (ray-truth noise floor) at small light for 3D boxes too;
	//  - at LARGE light the wedge UNDER-shadows 3D casters that have DEPTH (the tall box worst): it
	//    projects ONE silhouette (picked at the light CENTRE) through the receiver, but the true blocking
	//    contour parallax-shifts across the extended disk. The residual is signed NEGATIVE (undershoot),
	//    magnitude scaling with caster depth x light angular size - a silhouette-SELECTION error, NOT a
	//    corner error and NOT the static-silhouette sinA offset (planar cards showed none).
	std::printf( "  ---- small-light maxAbs=%.4f   large-light worst signed mean=%+.4f (undershoot)\n",
				 smallRmax, largeRworstMean );
	CHECK( smallRmax < 0.02 );				// exact in the small-light regime
	CHECK( largeRworstMean < -0.05 );		// large-light 3D error is a real UNDERSHOOT (direction confirmed)
}

// ===================================================================================================
// STEP 5: cost. Per-fragment wedge work = #silhouette EDGES; Fubini baseline = #triangles x #samples.
TEST( SoftShadowWedge, cost_collapse_estimate )
{
	std::printf( "\n  [WEDGE cost / collapse vs Fubini triangle-sample baseline]\n" );
	// measured edge count the wedge actually walks for a box silhouette
	Box b = MakeBox( float3( 0, 0, 0 ), float3( 6, 6, 6 ), 0.3f, 0.2f );
	std::vector<float3> loop = Silhouette( b, float3( 0, 0, 60 ) );
	long edges = 0;
	WedgeOccLoop( loop, float3( 3, 2, -30 ), float3( 0, 0, 60 ), 8.0f, 0, &edges );
	std::printf( "  box silhouette: walked %ld edges (loop verts=%zu)\n", edges, loop.size() );

	// representative character: surface tris vs silhouette-loop edges. Silhouette perimeter ~ O(sqrt(area))
	// so edges ~ O(sqrt(tris)); a mid character ~5000 tris projects a silhouette of ~120-180 edges.
	struct M { const char* name; long tris; long silEdges; };
	const M models[] =
	{
		{ "small prop (box-ish)", 12,   6 },
		{ "monster (mid)",        5000, 150 },
		{ "hero (dense)",         20000, 320 },
	};
	const int fubSamples = 16;			// Fubini: #triangles x #chords/samples (r_softShadowScanChords default 16)
	std::printf( "  %-22s %8s %8s %14s %10s\n", "model", "tris", "silEdges", "Fubini(tris*16)", "collapse" );
	for( const M& m : models )
	{
		long fub = m.tris * fubSamples;
		double collapse = ( double )fub / ( double )m.silEdges;
		std::printf( "  %-22s %8ld %8ld %14ld %9.0fx\n", m.name, m.tris, m.silEdges, fub, collapse );
	}
	std::printf( "  (per-edge wedge op ~ 1 clip + 1 project + 1 circle-tri area w/ 1 fast-atan;\n"
				 "   per Fubini op ~ 1 Moller-Trumbore point test. edge count is O(sqrt(tris)).)\n" );
	CHECK( edges > 0 );
}

// ===================================================================================================
// WEDGE-WHITELIST safety outputs (SoftShadow_WedgeOcclusionEx). The per-fragment selector offloads to the
// cheap wedge only where it is PROVABLY the exact union; the two outputs are its evidence:
//   nContrib = number of casters that occlude here (1 => max IS the union => wedge exact),
//   gap      = min(1, Sum solo) - max solo = the largest amount the max-combine can UNDER-shadow.
// This asserts the accumulation algebra directly (no ray truth): the concat of two casters must report
// nContrib 2, occ == max(solo0, solo1), and gap == min(1, solo0+solo1) - max - the identity the whitelist
// relies on. Two concentric cards of different size both occlude, so max-combine is actually exact here yet
// gap flags a nonzero risk: the whitelist is CONSERVATIVE (over-flags overlap, never under-flags), which is
// the safe direction - scanline is exact, so a false "unsafe" costs perf, never correctness.
TEST( SoftShadowWedge, whitelist_gap_and_contrib )
{
	const float3 L( 0, 0, 60 ), P( 0, 0, -30 );
	const float  R = 8.0f;
	// two concentric square cards of different size, both on the P->L axis so both actually shadow P
	std::vector<float3> c0 = CardSquare( 0.0f, 1.5f );		// small => PARTIAL disk coverage (solo < 1, gap != 0)
	std::vector<float3> c1 = CardSquare( 0.0f, 1.0f );

	auto runEx = [&]( const std::vector<float4>& rec, float& gap, int& nc ) -> float
	{
		SoftEdgeBuffer buf{ rec.data(), ( int )rec.size() };
		int clip = 0;
		return saturate( SoftShadow_WedgeOcclusionEx( P, L, R, 0, ( int )( rec.size() / 2 ), 0.0f, gap, nc, clip, buf ) );
	};

	std::vector<float4> r0 = BuildCaster( { c0 } );
	std::vector<float4> r1 = BuildCaster( { c1 } );
	float g0 = -1, g1 = -1; int n0 = -1, n1 = -1;
	float s0 = runEx( r0, g0, n0 );
	float s1 = runEx( r1, g1, n1 );
	std::printf( "  [whitelist] solo0 %.3f (nc %d, gap %.4f)  solo1 %.3f (nc %d, gap %.4f)\n", s0, n0, g0, s1, n1, g1 );
	// single contributor => max IS the union => gap exactly 0, one contributor counted
	CHECK( n0 == 1 && n1 == 1 );
	CHECK_NEAR( g0, 0.0f, 1e-6f );
	CHECK_NEAR( g1, 0.0f, 1e-6f );
	CHECK( s0 > 0.02f && s1 > 0.02f );		// both cards actually shadow

	std::vector<float4> rBoth = r0;
	rBoth.insert( rBoth.end(), r1.begin(), r1.end() );		// concat = two casters (two headers)
	float gB = -1; int nB = -1;
	float occB = runEx( rBoth, gB, nB );
	const float expOcc = std::fmax( s0, s1 );
	const float expGap = std::fmax( std::fmin( 1.0f, s0 + s1 ) - expOcc, 0.0f );
	std::printf( "  [whitelist] both: occ %.3f (exp %.3f) nContrib %d gap %.4f (exp %.4f)\n", occB, expOcc, nB, gB, expGap );
	CHECK( nB == 2 );										// two casters contribute
	CHECK_NEAR( occB, expOcc, 1e-4f );						// wedge returns the max-combine
	CHECK_NEAR( gB, expGap, 1e-4f );						// gap == min(1,sum) - max: the whitelist identity
	CHECK( gB > 0.02f );									// disjoint casters => a genuine under-shadow the wedge misses
}
