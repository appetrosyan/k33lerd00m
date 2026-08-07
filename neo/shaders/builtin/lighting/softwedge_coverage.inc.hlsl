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

// Analytic soft shadows by LIGHT-DISK COVERAGE. From the receiver, the caster's silhouette loop projects
// onto the area light's disk; the occluded fraction is area( disk INTERSECT projected-silhouette ) / area.
// That is a signed sum of per-edge circle-triangle areas around the loop: it is winding-correct, so it is
// exact for NON-CONVEX casters (where an intersection-of-half-spaces / MIN combine fails), and it varies
// smoothly 0..1 across the penumbra (contact hardening is intrinsic - a near receiver sees the silhouette
// large on the disk, a far one sees it small). The umbra is where coverage saturates to 1 - never stamped.
//
// The sum is order-INDEPENDENT (verified numerically), so the engine may hand edges in any order as long
// as they are consistently wound. Each caster's edges are summed separately (grouped by id) and casters
// are unioned by max.

#ifndef __SOFTWEDGE_COVERAGE_INC__
#define __SOFTWEDGE_COVERAGE_INC__

// Signed area of the disk (centre origin, radius r) intersected with the triangle (origin, A, B). Summed
// over a closed loop's directed edges this yields the signed area of disk INTERSECT polygon. Handles the
// four clip cases: both endpoints in; one in one out; segment crossing; segment entirely outside (a pure
// circular sector). A sector's signed area is 0.5*r^2*angle, the triangle's is 0.5*cross(A,B).
float SoftDisk_CircleTriArea( float2 A, float2 B, float r2 )		// r2 = disk radius squared (fragment-invariant, hoisted)
{
	float a2 = dot( A, A );
	float b2 = dot( B, B );

	if( a2 <= r2 && b2 <= r2 )
	{
		return 0.5 * ( A.x * B.y - A.y * B.x );					// both inside: plain triangle (checked first: no D/qa needed)
	}

	float2 D  = B - A;
	float  qa = dot( D, D );
	if( qa < 1e-9 )
	{
		return 0.0;												// degenerate edge (endpoints coincide): no area, avoids /0
	}

	float  qb = 2.0 * dot( A, D );
	float  qc = a2 - r2;
	float  disc = qb * qb - 4.0 * qa * qc;

	bool ain = ( a2 <= r2 );
	bool bin = ( b2 <= r2 );

	if( disc <= 0.0 )
	{
		return 0.5 * r2 * atan2( A.x * B.y - A.y * B.x, dot( A, B ) );	// entirely outside: sector
	}

	float sq = sqrt( disc );
	float t1 = ( -qb - sq ) / ( 2.0 * qa );
	float t2 = ( -qb + sq ) / ( 2.0 * qa );

	if( ain && !bin )
	{
		float  t = ( t2 >= 0.0 && t2 <= 1.0 ) ? t2 : t1;
		float2 X = A + t * D;
		return 0.5 * ( A.x * X.y - A.y * X.x )
			   + 0.5 * r2 * atan2( X.x * B.y - X.y * B.x, dot( X, B ) );
	}
	if( !ain && bin )
	{
		float  t = ( t1 >= 0.0 && t1 <= 1.0 ) ? t1 : t2;
		float2 X = A + t * D;
		return 0.5 * r2 * atan2( A.x * X.y - A.y * X.x, dot( A, X ) )
			   + 0.5 * ( X.x * B.y - X.y * B.x );
	}
	if( t1 >= 0.0 && t1 <= 1.0 && t2 >= 0.0 && t2 <= 1.0 )
	{
		float2 P1 = A + t1 * D;
		float2 P2 = A + t2 * D;
		return 0.5 * r2 * atan2( A.x * P1.y - A.y * P1.x, dot( A, P1 ) )
			   + 0.5 * ( P1.x * P2.y - P1.y * P2.x )
			   + 0.5 * r2 * atan2( P2.x * B.y - P2.y * B.x, dot( P2, B ) );
	}
	return 0.5 * r2 * atan2( A.x * B.y - A.y * B.x, dot( A, B ) );		// both outside, no crossing: sector
}

// ---------------------------------------------------------------------------------------------------
// Full per-fragment light-disk occlusion. Shared by the pixel shader (interactionSM.ps.hlsl) and the
// unit tests (neo/tests/SoftShadowCoverage_test.cpp), which compile this exact source as C++ via
// hlsl_compat.h - so the tests exercise the LIVE shader math, not a copy. Written with only single-
// component (.x/.y/.z/.w) access, no multi-swizzles, to stay valid in both languages. The edge buffer
// is the global t_SoftEdges in HLSL and a trailing parameter in C++ (SW_EDGEBUF_PARAM).
//
// swP = receiver world pos, swL = light origin, swR = light-disk radius (penumbra size), swFirstElem =
// this light's first edge record, swN = record count (edges + one header per caster). Returns the
// occluded fraction 0(lit)..1(umbra); the caller forms shadow = 1 - saturate( occ ).
//
// NEAR-PLANE CLOSURE (Sutherland-Hodgman). The blocked region on the disk is the projection of the part
// of the caster IN FRONT of the receiver; the part behind P blocks nothing. So the silhouette is clipped
// to the depth slab [swEps, swDistPL] and only the clipped (front) contour is summed. A vertex behind the
// receiver is CLIPPED OFF, not projected (its projection blows up as dn->0). The near-plane clip inserts a
// straight connector edge (the central projection of the caster's planar near-plane cross-section - lines
// map to lines, so it is exactly straight), whose endpoints sit at radius ~swDistPL/swEps. swEps is kept
// small so those crossings project FAR outside the disk and the connector never cuts it (a connector that
// cut the disk was the semicircular-umbra bug: at a coarse eps two opposite crossings form a diameter).

#ifndef PI
	#define PI 3.14159265358979323846
#endif

#ifndef SW_NEAR_EPS
	#define SW_NEAR_EPS 1e-3		// near-plane clip depth; small so near-plane crossings project far off the disk
#endif

#ifdef __cplusplus
	#define SW_EDGEBUF_PARAM , SoftEdgeBuffer t_SoftEdges
#else
	#define SW_EDGEBUF_PARAM
#endif

float SoftShadow_WedgeOcclusion( float3 swP, float3 swL, float swR, int swFirstElem, int swN SW_EDGEBUF_PARAM )
{
	swR = max( swR, 1e-2 );
	float3 swToL = swL - swP;
	float  swDistPL = max( length( swToL ), 1e-4 );					// receiver->light distance
	float3 swNrm = swToL * ( 1.0 / swDistPL );
	float3 swUp  = ( abs( swNrm.z ) > 0.9 ) ? float3( 0.0, 1.0, 0.0 ) : float3( 0.0, 0.0, 1.0 );
	float3 swU   = normalize( cross( swUp, swNrm ) );
	float3 swV   = cross( swNrm, swU );
	float  swR2  = swR * swR;
	float  swInvDiskArea = 1.0 / ( PI * swR2 );
	float  swSinA = saturate( swR / swDistPL );						// disk half-angle (caster-invariant cull)
	float  swCosA = sqrt( 1.0 - swSinA * swSinA );
	const float swEps = SW_NEAR_EPS;

	float swOcc = 0.0;
	float swArea = 0.0;
	bool  haveCaster = false;
	bool  swSkip = false;
	// per-chain shoelace state: swFirst/swPrev are the first/last clipped vertex of the current silhouette
	// chain. A caster concatenates several CLOSED walk-ordered chains (a hole wound the other way, one chain
	// per surface of a multi-part entity); a chain boundary is detected bit-exactly by e0 != previous e1.
	bool   swFirstValid = false;
	float2 swFirst = float2( 0.0, 0.0 );
	float2 swPrev  = float2( 0.0, 0.0 );
	bool   havePrevE1 = false;
	float3 prevE1w = float3( 0.0, 0.0, 0.0 );
	for( int se = 0; se < swN; se++ )
	{
		float4 e0 = t_SoftEdges[ swFirstElem + se * 2 + 0 ];
		float4 e1 = t_SoftEdges[ swFirstElem + se * 2 + 1 ];
		if( e0.w < 0.0 )		// header record = caster boundary: close+finalize the previous caster, cull the next
		{
			if( haveCaster && swFirstValid )
			{
				swArea += SoftDisk_CircleTriArea( swPrev, swFirst, swR2 );	// close the previous caster's open chain
			}
			if( haveCaster )
			{
				swOcc = max( swOcc, saturate( abs( swArea ) * swInvDiskArea ) );
				if( swOcc >= 0.999 ) { break; }
			}
			haveCaster = true;
			swArea = 0.0;
			swSkip = false;
			swFirstValid = false;
			havePrevE1 = false;
			float3 dCv  = float3( e0.x, e0.y, e0.z ) - swP;			// caster bounding sphere: centre, radius e1.x
			float  cRad = e1.x;
			float  dCn  = dot( dCv, swNrm );
			if( dCn + cRad < swEps || dCn - cRad > swDistPL )		// wholly behind receiver, or wholly beyond light
			{
				swSkip = true;
				continue;
			}
			if( dCn - cRad > 1e-3 )									// sphere fully in front: angular reject is safe (AO-4)
			{
				float front = dot( dCv, dCv ) - cRad * cRad;
				swSkip = ( dCn < swCosA * sqrt( front ) - swSinA * cRad );
			}
			continue;
		}
		if( swSkip ) { continue; }

		float3 A = float3( e0.x, e0.y, e0.z );
		float3 B = float3( e1.x, e1.y, e1.z );
		if( havePrevE1 && ( A.x != prevE1w.x || A.y != prevE1w.y || A.z != prevE1w.z ) && swFirstValid )
		{
			swArea += SoftDisk_CircleTriArea( swPrev, swFirst, swR2 );	// close a finished chain before the next
			swFirstValid = false;
		}
		havePrevE1 = true;
		prevE1w = B;

		// clip the edge's parameter interval to the depth slab [swEps, swDistPL]; an empty interval = the
		// whole edge is behind the receiver or beyond the light, contributes no vertex, and its span is
		// bridged by the connector against swPrev on the next kept edge - keeping the clipped loop closed.
		float3 a = A - swP;
		float3 b = B - swP;
		float  dnA = dot( a, swNrm );
		float  dnB = dot( b, swNrm );
		float  d   = dnB - dnA;
		float  t0 = 0.0;
		float  t1 = 1.0;
		bool   empty = false;
		if( abs( d ) < 1e-12 )
		{
			if( dnA < swEps ) { empty = true; }
		}
		else
		{
			float tc = ( swEps - dnA ) / d;
			if( d > 0.0 ) { t0 = max( t0, tc ); }
			else          { t1 = min( t1, tc ); }
		}
		if( !empty )
		{
			if( abs( d ) < 1e-12 )
			{
				if( dnA > swDistPL ) { empty = true; }
			}
			else
			{
				float tc = ( swDistPL - dnA ) / d;
				if( d > 0.0 ) { t1 = min( t1, tc ); }
				else          { t0 = max( t0, tc ); }
			}
		}
		if( empty || t0 > t1 ) { continue; }

		float3 pa  = a + t0 * ( b - a );
		float3 pb  = a + t1 * ( b - a );
		float  dna = dnA + t0 * d;
		float  dnb = dnA + t1 * d;
		float2 q0 = float2( ( swDistPL / dna ) * dot( pa, swU ), ( swDistPL / dna ) * dot( pa, swV ) );
		float2 q1 = float2( ( swDistPL / dnb ) * dot( pb, swU ), ( swDistPL / dnb ) * dot( pb, swV ) );

		if( swFirstValid ) { swArea += SoftDisk_CircleTriArea( swPrev, q0, swR2 ); }
		else               { swFirst = q0; swFirstValid = true; }
		swArea += SoftDisk_CircleTriArea( q0, q1, swR2 );
		swPrev = q1;
	}
	if( haveCaster && swFirstValid )
	{
		swArea += SoftDisk_CircleTriArea( swPrev, swFirst, swR2 );		// close the last caster's open chain
	}
	if( haveCaster ) { swOcc = max( swOcc, saturate( abs( swArea ) * swInvDiskArea ) ); }
	return swOcc;
}

#endif // __SOFTWEDGE_COVERAGE_INC__
