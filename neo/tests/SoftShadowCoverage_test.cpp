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

// Reference + tests for the analytic soft-shadow light-disk coverage integral
// (shaders/builtin/lighting/interactionSM.ps.hlsl USE_SOFT_WEDGE +
//  shaders/builtin/lighting/softwedge_coverage.inc.hlsl).
//
// SoftShadowCoverageRef below is a LINE-FOR-LINE C++ mirror of the HLSL: it consumes the same
// flattened t_SoftEdges record layout (2 float4/record; header records have e0.w<0 and carry the
// caster bounding sphere; edge records hold world endpoints e0.xyz/e1.xyz and stash the caster's
// header index in e1.w) and runs the same per-fragment loop. Keep the two in sync: any edit to the
// coverage math must land in BOTH this mirror and the .hlsl, and this test must stay green.
//
// The property under test is the near/far-plane closure. A silhouette that dips behind the receiver
// near-plane (or pokes beyond the light plane) must stay a CLOSED loop on the light disk; the fix is
// Sutherland-Hodgman slab clipping with the near-plane connector kept as a straight edge of the
// clipped polygon (a sector/chord shortcut there is what over-darkens lit fragments = the halo).
// Ground truth is an independent ray cast (occluder must lie strictly between P and the disk point),
// so the test does not presuppose the projection math it validates.

#include "idUnitTest.h"

#include <vector>
#include <cmath>

namespace
{

// ------------------------------------------------------------------ tiny vec types (HLSL-like)
struct v2 { float x, y; };
struct v3 { float x, y, z; };
struct v4 { float x, y, z, w; };

static inline v3   sub( v3 a, v3 b )   { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
static inline v3   add( v3 a, v3 b )   { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
static inline v3   mul( v3 a, float s ){ return { a.x * s, a.y * s, a.z * s }; }
static inline float dot( v3 a, v3 b )  { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline v3   cross( v3 a, v3 b ) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
static inline float len( v3 a )        { return std::sqrt( dot( a, a ) ); }
static inline v3   normalize( v3 a )   { float l = len( a ); return l > 0 ? mul( a, 1.0f / l ) : a; }

// ================================================================= HLSL MIRROR (keep in sync) =====

// softwedge_coverage.inc.hlsl : SoftDisk_CircleTriArea
static float SoftDisk_CircleTriArea( v2 A, v2 B, float r2 )
{
	float a2 = A.x * A.x + A.y * A.y;
	float b2 = B.x * B.x + B.y * B.y;
	if( a2 <= r2 && b2 <= r2 )
	{
		return 0.5f * ( A.x * B.y - A.y * B.x );
	}
	v2 D = { B.x - A.x, B.y - A.y };
	float qa = D.x * D.x + D.y * D.y;
	if( qa < 1e-9f )
	{
		return 0.0f;
	}
	float qb = 2.0f * ( A.x * D.x + A.y * D.y );
	float qc = a2 - r2;
	float disc = qb * qb - 4.0f * qa * qc;
	bool ain = ( a2 <= r2 );
	bool bin = ( b2 <= r2 );
	if( disc <= 0.0f )
	{
		return 0.5f * r2 * std::atan2( A.x * B.y - A.y * B.x, A.x * B.x + A.y * B.y );
	}
	float sq = std::sqrt( disc );
	float t1 = ( -qb - sq ) / ( 2.0f * qa );
	float t2 = ( -qb + sq ) / ( 2.0f * qa );
	if( ain && !bin )
	{
		float t = ( t2 >= 0.0f && t2 <= 1.0f ) ? t2 : t1;
		v2 X = { A.x + t * D.x, A.y + t * D.y };
		return 0.5f * ( A.x * X.y - A.y * X.x )
			   + 0.5f * r2 * std::atan2( X.x * B.y - X.y * B.x, X.x * B.x + X.y * B.y );
	}
	if( bin && !ain )
	{
		float t = ( t1 >= 0.0f && t1 <= 1.0f ) ? t1 : t2;
		v2 X = { A.x + t * D.x, A.y + t * D.y };
		return 0.5f * r2 * std::atan2( A.x * X.y - A.y * X.x, A.x * X.x + A.y * X.y )
			   + 0.5f * ( X.x * B.y - X.y * B.x );
	}
	if( t1 >= 0.0f && t1 <= 1.0f && t2 >= 0.0f && t2 <= 1.0f )
	{
		v2 P1 = { A.x + t1 * D.x, A.y + t1 * D.y };
		v2 P2 = { A.x + t2 * D.x, A.y + t2 * D.y };
		return 0.5f * r2 * std::atan2( A.x * P1.y - A.y * P1.x, A.x * P1.x + A.y * P1.y )
			   + 0.5f * ( P1.x * P2.y - P1.y * P2.x )
			   + 0.5f * r2 * std::atan2( P2.x * B.y - P2.y * B.x, P2.x * B.x + P2.y * B.y );
	}
	return 0.5f * r2 * std::atan2( A.x * B.y - A.y * B.x, A.x * B.x + A.y * B.y );
}

// interactionSM.ps.hlsl USE_SOFT_WEDGE coverage loop. `rec` = flattened t_SoftEdges (2 v4/record).
// Returns shadow in [0,1] (1 = fully lit). swP = receiver world pos, swL = light origin, swR = radius.
static float SoftShadowCoverageRef( const std::vector<v4>& rec, v3 swP, v3 swL, float swR )
{
	swR = std::fmax( swR, 1e-2f );
	v3    swToL    = sub( swL, swP );
	float swDistPL = std::fmax( len( swToL ), 1e-4f );
	v3    swNrm    = mul( swToL, 1.0f / swDistPL );
	v3    swUp     = ( std::fabs( swNrm.z ) > 0.9f ) ? v3{ 0, 1, 0 } : v3{ 0, 0, 1 };
	v3    swU      = normalize( cross( swUp, swNrm ) );
	v3    swV      = cross( swNrm, swU );
	float swR2     = swR * swR;
	float swInvDiskArea = 1.0f / ( 3.14159265358979f * swR2 );
	const float swEps = 1e-3f;

	float swOcc = 0.0f;
	float swArea = 0.0f;
	bool  haveCaster = false;
	bool  swSkip = false;
	// per-chain streaming shoelace state (Sutherland-Hodgman clipped contour)
	bool  swFirstValid = false;
	v2    swFirst = { 0, 0 };
	v2    swPrev  = { 0, 0 };
	bool  havePrevE1 = false;
	v3    prevE1w = { 0, 0, 0 };

	int swN = ( int )( rec.size() / 2 );
	for( int se = 0; se < swN; se++ )
	{
		v4 e0 = rec[se * 2 + 0];
		v4 e1 = rec[se * 2 + 1];

		if( e0.w < 0.0f )   // header record = caster boundary
		{
			// close the current chain of the previous caster, then finalize it
			if( haveCaster && swFirstValid )
			{
				swArea += SoftDisk_CircleTriArea( swPrev, swFirst, swR2 );
			}
			if( haveCaster )
			{
				swOcc = std::fmax( swOcc, std::fmin( std::fabs( swArea ) * swInvDiskArea, 1.0f ) );
				if( swOcc >= 0.999f ) { break; }
			}
			haveCaster = true;
			swArea = 0.0f;
			swSkip = false;
			swFirstValid = false;
			havePrevE1 = false;

			v3    dCv  = sub( v3{ e0.x, e0.y, e0.z }, swP );
			float cRad = e1.x;
			float dCn  = dot( dCv, swNrm );
			if( dCn + cRad < swEps || dCn - cRad > swDistPL )
			{
				swSkip = true;
			}
			continue;
		}
		if( swSkip ) { continue; }

		v3 A = { e0.x, e0.y, e0.z };
		v3 B = { e1.x, e1.y, e1.z };

		// chain boundary: this edge's E0 world vertex not equal to the previous edge's E1 (bit-exact
		// for shared silhouette vertices within one walk-ordered chain). Close the finished chain so
		// its clipped contour never connects to the next chain (holes / multi-surface caster parts).
		bool newChain = !havePrevE1 || A.x != prevE1w.x || A.y != prevE1w.y || A.z != prevE1w.z;
		if( newChain && swFirstValid )
		{
			swArea += SoftDisk_CircleTriArea( swPrev, swFirst, swR2 );
			swFirstValid = false;
		}
		havePrevE1 = true;
		prevE1w = B;

		v3 a = sub( A, swP );
		v3 b = sub( B, swP );
		float dnA = dot( a, swNrm );
		float dnB = dot( b, swNrm );
		float d   = dnB - dnA;

		// clip the edge's parameter interval to the depth slab [swEps, swDistPL] (Sutherland-Hodgman
		// against the two parallel planes). Empty => whole edge outside the slab; its endpoints are
		// bridged by the connector formed against swPrev on the next kept edge / chain close.
		float t0 = 0.0f, t1 = 1.0f;
		bool empty = false;
		if( std::fabs( d ) < 1e-12f )
		{
			if( dnA < swEps ) { empty = true; }
		}
		else
		{
			float tc = ( swEps - dnA ) / d;
			if( d > 0.0f ) { t0 = std::fmax( t0, tc ); }
			else           { t1 = std::fmin( t1, tc ); }
		}
		if( !empty )
		{
			if( std::fabs( d ) < 1e-12f )
			{
				if( dnA > swDistPL ) { empty = true; }
			}
			else
			{
				float tc = ( swDistPL - dnA ) / d;
				if( d > 0.0f ) { t1 = std::fmin( t1, tc ); }
				else           { t0 = std::fmax( t0, tc ); }
			}
		}
		if( empty || t0 > t1 ) { continue; }

		v3 pa = add( a, mul( sub( b, a ), t0 ) );
		v3 pb = add( a, mul( sub( b, a ), t1 ) );
		float dna = dnA + t0 * d;
		float dnb = dnA + t1 * d;
		v2 q0 = { ( swDistPL / dna ) * dot( pa, swU ), ( swDistPL / dna ) * dot( pa, swV ) };
		v2 q1 = { ( swDistPL / dnb ) * dot( pb, swU ), ( swDistPL / dnb ) * dot( pb, swV ) };

		if( swFirstValid ) { swArea += SoftDisk_CircleTriArea( swPrev, q0, swR2 ); }
		else               { swFirst = q0; swFirstValid = true; }
		swArea += SoftDisk_CircleTriArea( q0, q1, swR2 );
		swPrev = q1;
	}
	if( haveCaster && swFirstValid )
	{
		swArea += SoftDisk_CircleTriArea( swPrev, swFirst, swR2 );
	}
	if( haveCaster )
	{
		swOcc = std::fmax( swOcc, std::fmin( std::fabs( swArea ) * swInvDiskArea, 1.0f ) );
	}
	return 1.0f - std::fmin( swOcc, 1.0f );
}

// The OLD (buggy) per-edge sum: each edge summed independently, endpoints behind the receiver skipped.
// Kept only to prove the new path is bit-identical where nothing clips, and diverges where it does.
static float SoftShadowCoverageOld( const std::vector<v4>& rec, v3 swP, v3 swL, float swR )
{
	swR = std::fmax( swR, 1e-2f );
	v3 swToL = sub( swL, swP );
	float swDistPL = std::fmax( len( swToL ), 1e-4f );
	v3 swNrm = mul( swToL, 1.0f / swDistPL );
	v3 swUp = ( std::fabs( swNrm.z ) > 0.9f ) ? v3{ 0, 1, 0 } : v3{ 0, 0, 1 };
	v3 swU = normalize( cross( swUp, swNrm ) );
	v3 swV = cross( swNrm, swU );
	float swR2 = swR * swR;
	float swInvDiskArea = 1.0f / ( 3.14159265358979f * swR2 );
	float swOcc = 0.0f, swArea = 0.0f;
	bool haveCaster = false, swSkip = false;
	const float swEps = 1e-3f;
	int swN = ( int )( rec.size() / 2 );
	for( int se = 0; se < swN; se++ )
	{
		v4 e0 = rec[se * 2 + 0], e1 = rec[se * 2 + 1];
		if( e0.w < 0.0f )
		{
			if( haveCaster ) { swOcc = std::fmax( swOcc, std::fmin( std::fabs( swArea ) * swInvDiskArea, 1.0f ) ); }
			haveCaster = true; swArea = 0.0f; swSkip = false; continue;
		}
		if( swSkip ) { continue; }
		v3 a = sub( v3{ e0.x, e0.y, e0.z }, swP );
		v3 b = sub( v3{ e1.x, e1.y, e1.z }, swP );
		float dnA = dot( a, swNrm ), dnB = dot( b, swNrm );
		if( ( dnA < swEps && dnB < swEps ) || ( dnA > swDistPL && dnB > swDistPL ) ) { continue; }
		if( dnA < swEps ) { a = add( a, mul( sub( b, a ), ( swEps - dnA ) / ( dnB - dnA ) ) ); dnA = swEps; }
		if( dnB < swEps ) { b = add( b, mul( sub( a, b ), ( swEps - dnB ) / ( dnA - dnB ) ) ); dnB = swEps; }
		if( dnA > swDistPL ) { a = add( a, mul( sub( b, a ), ( swDistPL - dnA ) / ( dnB - dnA ) ) ); dnA = swDistPL; }
		if( dnB > swDistPL ) { b = add( b, mul( sub( a, b ), ( swDistPL - dnB ) / ( dnA - dnB ) ) ); dnB = swDistPL; }
		v2 qa = { ( swDistPL / dnA ) * dot( a, swU ), ( swDistPL / dnA ) * dot( a, swV ) };
		v2 qb = { ( swDistPL / dnB ) * dot( b, swU ), ( swDistPL / dnB ) * dot( b, swV ) };
		swArea += SoftDisk_CircleTriArea( qa, qb, swR2 );
	}
	if( haveCaster ) { swOcc = std::fmax( swOcc, std::fmin( std::fabs( swArea ) * swInvDiskArea, 1.0f ) ); }
	return 1.0f - std::fmin( swOcc, 1.0f );
}

// ================================================================= test scaffolding ==============

// build flattened records for one caster from a set of closed, walk-ordered world loops.
static std::vector<v4> BuildCaster( const std::vector<std::vector<v3>>& loops )
{
	// bounding sphere over all loop verts (permissive, so the depth cull never falsely skips in tests)
	v3 lo = { 1e30f, 1e30f, 1e30f }, hi = { -1e30f, -1e30f, -1e30f };
	for( const auto& loop : loops )
		for( v3 p : loop )
		{
			lo = { std::fmin( lo.x, p.x ), std::fmin( lo.y, p.y ), std::fmin( lo.z, p.z ) };
			hi = { std::fmax( hi.x, p.x ), std::fmax( hi.y, p.y ), std::fmax( hi.z, p.z ) };
		}
	v3 ctr = mul( add( lo, hi ), 0.5f );
	float rad = len( sub( hi, lo ) );
	std::vector<v4> rec;
	rec.push_back( { ctr.x, ctr.y, ctr.z, -1.0f } );   // header e0 (centre, marker)
	rec.push_back( { rad, 0, 0, 0 } );                 // header e1 (bounding radius)
	for( const auto& loop : loops )
	{
		int m = ( int )loop.size();
		for( int i = 0; i < m; i++ )
		{
			v3 A = loop[i], B = loop[( i + 1 ) % m];
			rec.push_back( { A.x, A.y, A.z, 0.0f } );
			rec.push_back( { B.x, B.y, B.z, 0.0f } );
		}
	}
	return rec;
}

// independent ground truth: fraction of the light disk whose ray P->Dp is blocked by a caster loop
// STRICTLY between P and the disk (0<t<1). loops are planar convex polygons (triangle fan).
static bool RayHitsLoopBetween( v3 P, v3 dir, const std::vector<v3>& poly )
{
	for( size_t i = 1; i + 1 < poly.size(); i++ )
	{
		v3 A = poly[0], B = poly[i], C = poly[i + 1];
		v3 e1 = sub( B, A ), e2 = sub( C, A );
		v3 pv = cross( dir, e2 );
		float det = dot( e1, pv );
		if( std::fabs( det ) < 1e-12f ) { continue; }
		float inv = 1.0f / det;
		v3 tv = sub( P, A );
		float w = dot( tv, pv ) * inv;
		if( w < -1e-6f || w > 1 + 1e-6f ) { continue; }
		v3 qv = cross( tv, e1 );
		float vv = dot( dir, qv ) * inv;
		if( vv < -1e-6f || w + vv > 1 + 1e-6f ) { continue; }
		float t = dot( e2, qv ) * inv;
		if( t > 1e-6f && t < 1.0f - 1e-6f ) { return true; }
	}
	return false;
}

static float TruthCoverage( v3 P, v3 L, float r, const std::vector<std::vector<v3>>& loops, int N = 200 )
{
	v3 toL = sub( L, P ); float dist = len( toL ); v3 nrm = mul( toL, 1.0f / dist );
	v3 up = ( std::fabs( nrm.z ) > 0.9f ) ? v3{ 0, 1, 0 } : v3{ 0, 0, 1 };
	v3 u = normalize( cross( up, nrm ) ); v3 v = cross( nrm, u );
	int inside = 0, total = 0;
	for( int iy = 0; iy < N; iy++ )
		for( int ix = 0; ix < N; ix++ )
		{
			float du = ( ix + 0.5f ) / N * 2 - 1, dv = ( iy + 0.5f ) / N * 2 - 1;
			if( du * du + dv * dv > 1.0f ) { continue; }
			total++;
			v3 Dp = add( L, add( mul( u, du * r ), mul( v, dv * r ) ) );
			v3 dir = sub( Dp, P );
			for( const auto& loop : loops )
				if( RayHitsLoopBetween( P, dir, loop ) ) { inside++; break; }
		}
	return total ? ( float )inside / total : 0.0f;
}

// axis-aligned / tilted quad helpers (planar, convex)
static std::vector<v3> QuadXY( float cx, float cy, float cz, float sx, float sy )
{
	return { { cx - sx, cy - sy, cz }, { cx + sx, cy - sy, cz }, { cx + sx, cy + sy, cz }, { cx - sx, cy + sy, cz } };
}
static std::vector<v3> QuadTiltYZ( float cx, float cy, float cz, float s, float dy )   // straddles near/far in z
{
	return { { cx - s, cy - s + dy, cz - s }, { cx + s, cy - s + dy, cz - s },
			 { cx + s, cy + s + dy, cz + s }, { cx - s, cy + s + dy, cz + s } };
}

// small deterministic LCG so the fuzz is reproducible without <random> platform variance
struct Rng
{
	unsigned s;
	Rng( unsigned seed ) : s( seed ) {}
	float f( float lo, float hi ) { s = s * 1664525u + 1013904223u; return lo + ( hi - lo ) * ( ( s >> 8 ) & 0xFFFFFF ) / float( 0x1000000 ); }
	int   i( int lo, int hi ) { s = s * 1664525u + 1013904223u; return lo + int( ( s >> 8 ) % ( unsigned )( hi - lo + 1 ) ); }
};

const v3 P0 = { 0, 0, 0 };
const v3 L0 = { 0, 0, 10 };
const float R0 = 3.0f;

} // namespace

// ===================================================================== tests ======================

TEST( SoftShadowCoverage, fully_lit_when_caster_off_to_the_side )
{
	auto rec = BuildCaster( { QuadXY( 9, 0, 5, 1, 1 ) } );
	CHECK_NEAR( SoftShadowCoverageRef( rec, P0, L0, R0 ), 1.0f, 1e-4f );   // shadow==1, no occlusion
}

TEST( SoftShadowCoverage, partial_penumbra_over_center )
{
	auto rec = BuildCaster( { QuadXY( 0, 0, 5, 1, 1 ) } );
	float shadow = SoftShadowCoverageRef( rec, P0, L0, R0 );
	float truth  = 1.0f - TruthCoverage( P0, L0, R0, { QuadXY( 0, 0, 5, 1, 1 ) } );
	CHECK_NEAR( shadow, truth, 0.01f );
}

TEST( SoftShadowCoverage, near_plane_straddle_does_not_over_darken )
{
	// the halo/scene-darkening bug: a caster whose silhouette dips behind the receiver near-plane.
	// truth is 0 occlusion here (edge-on / off to the side); the loop must stay closed and return lit.
	for( float dy : { -1.0f, 0.0f, 0.3f, 1.0f, 2.0f } )
	{
		auto loop = QuadTiltYZ( 3, 0, 0, 2, dy );
		auto rec = BuildCaster( { loop } );
		float shadow = SoftShadowCoverageRef( rec, P0, L0, R0 );
		float truth  = 1.0f - TruthCoverage( P0, L0, R0, { loop } );
		CHECK_NEAR( shadow, truth, 0.02f );
	}
}

TEST( SoftShadowCoverage, near_plane_straddle_full_block_and_miss )
{
	// centered slab straddling the near plane: fully blocks (truth coverage 1) vs fully misses (0)
	auto blockLoop = QuadTiltYZ( 0, 0, 0, 2, -1.0f );
	auto missLoop  = QuadTiltYZ( 0, 0, 0, 2,  2.0f );
	CHECK_NEAR( SoftShadowCoverageRef( BuildCaster( { blockLoop } ), P0, L0, R0 ),
			1.0f - TruthCoverage( P0, L0, R0, { blockLoop } ), 0.02f );
	CHECK_NEAR( SoftShadowCoverageRef( BuildCaster( { missLoop } ), P0, L0, R0 ),
			1.0f - TruthCoverage( P0, L0, R0, { missLoop } ), 0.02f );
}

TEST( SoftShadowCoverage, beyond_the_light_does_not_occlude )
{
	// a caster entirely beyond the light plane cannot block light travelling disk->P: far clip -> lit.
	auto rec = BuildCaster( { QuadXY( 0, 0, 14, 1, 1 ) } );
	CHECK_NEAR( SoftShadowCoverageRef( rec, P0, L0, R0 ), 1.0f, 1e-4f );
}

TEST( SoftShadowCoverage, hole_subtracts_via_opposite_winding )
{
	// outer loop + inner loop wound the other way: the inner hole must reduce coverage, not add to it.
	std::vector<v3> outer = QuadXY( 0, 0, 5, 2.0f, 2.0f );
	std::vector<v3> hole  = QuadXY( 0, 0, 5, 0.7f, 0.7f );
	std::vector<v3> holeRev( hole.rbegin(), hole.rend() );
	auto rec = BuildCaster( { outer, holeRev } );
	float shadow = SoftShadowCoverageRef( rec, P0, L0, R0 );
	// ground truth: blocked by outer AND NOT by hole
	v3 toL = sub( L0, P0 ); float dist = len( toL ); v3 nrm = mul( toL, 1.0f / dist );
	v3 up = { 0, 1, 0 }; v3 u = normalize( cross( up, nrm ) ); v3 vv = cross( nrm, u );
	int inside = 0, total = 0, N = 200;
	for( int iy = 0; iy < N; iy++ )
		for( int ix = 0; ix < N; ix++ )
		{
			float du = ( ix + 0.5f ) / N * 2 - 1, dv = ( iy + 0.5f ) / N * 2 - 1;
			if( du * du + dv * dv > 1.0f ) { continue; }
			total++;
			v3 Dp = add( L0, add( mul( u, du * R0 ), mul( vv, dv * R0 ) ) );
			v3 dir = sub( Dp, P0 );
			if( RayHitsLoopBetween( P0, dir, outer ) && !RayHitsLoopBetween( P0, dir, hole ) ) { inside++; }
		}
	float truth = 1.0f - ( float )inside / total;
	CHECK_NEAR( shadow, truth, 0.02f );
}

TEST( SoftShadowCoverage, unclipped_is_bit_identical_to_old_path )
{
	// where nothing clips (caster fully inside the depth slab), the new streaming path must produce the
	// EXACT same value as the old per-edge sum - losslessness for lit/umbra/in-slab-penumbra fragments.
	Rng rng( 12345 );
	float worst = 0.0f;
	for( int k = 0; k < 400; k++ )
	{
		std::vector<std::vector<v3>> loops;
		int nl = rng.i( 1, 3 );
		for( int j = 0; j < nl; j++ )
		{
			// keep every vertex well inside the slab (z in [2,8], small) so no edge is clipped
			loops.push_back( QuadXY( rng.f( -6, 6 ), rng.f( -6, 6 ), rng.f( 3, 7 ), rng.f( 0.3f, 1.0f ), rng.f( 0.3f, 1.0f ) ) );
		}
		auto rec = BuildCaster( loops );
		float a = SoftShadowCoverageRef( rec, P0, L0, R0 );
		float b = SoftShadowCoverageOld( rec, P0, L0, R0 );
		worst = std::fmax( worst, std::fabs( a - b ) );
	}
	CHECK( worst < 1e-6f );
}

TEST( SoftShadowCoverage, fuzz_matches_raycast_ground_truth )
{
	// single-loop casters across the whole slab range (behind receiver .. beyond light) vs ray truth.
	Rng rng( 777 );
	int bad = 0; float worst = 0.0f;
	for( int k = 0; k < 300; k++ )
	{
		auto loop = QuadTiltYZ( rng.f( -5, 5 ), rng.f( -5, 5 ), rng.f( -3, 14 ), rng.f( 0.4f, 2.5f ), rng.f( -2, 3 ) );
		auto rec = BuildCaster( { loop } );
		float shadow = SoftShadowCoverageRef( rec, P0, L0, R0 );
		float truth  = 1.0f - TruthCoverage( P0, L0, R0, { loop }, 160 );
		float d = std::fabs( shadow - truth );
		worst = std::fmax( worst, d );
		if( d > 0.03f ) { bad++; }
	}
	CHECK( bad == 0 );
	CHECK( worst < 0.03f );
}

TEST( SoftShadowCoverage, fuzz_disjoint_multichain_caster )
{
	// two spatially separated loops as ONE caster (multi-surface entity / holes): the per-chain closure
	// must keep them from cross-connecting. Loops kept disjoint on the disk so signed sum == union.
	Rng rng( 555 );
	int bad = 0; float worst = 0.0f;
	for( int k = 0; k < 200; k++ )
	{
		auto A = QuadTiltYZ( rng.f( -5, -3.5f ), rng.f( -4, 4 ), rng.f( -2, 9 ), rng.f( 0.4f, 1.0f ), rng.f( -1, 3 ) );
		auto B = QuadTiltYZ( rng.f( 3.5f, 5 ), rng.f( -4, 4 ), rng.f( -2, 9 ), rng.f( 0.4f, 1.0f ), rng.f( -1, 3 ) );
		auto rec = BuildCaster( { A, B } );
		float shadow = SoftShadowCoverageRef( rec, P0, L0, R0 );
		float truth  = 1.0f - TruthCoverage( P0, L0, R0, { A, B }, 150 );
		float d = std::fabs( shadow - truth );
		worst = std::fmax( worst, d );
		if( d > 0.03f ) { bad++; }
	}
	CHECK( bad == 0 );
	CHECK( worst < 0.03f );
}
