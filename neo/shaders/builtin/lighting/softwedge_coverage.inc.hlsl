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

// Project a world-space silhouette vertex A onto the light-disk plane (centre L, unit normal n facing the
// receiver P), returning its 2D coordinate in the disk frame (u,v). Returns false when A is not toward the
// light from P (the receiver is in front of that vertex) - such a caster cannot cleanly shadow P.
bool SoftDisk_Project( float3 A, float3 P, float3 L, float3 n, float3 u, float3 v, out float2 outUV )
{
	float3 dir = A - P;
	float  dn  = dot( dir, n );
	if( dn <= 1e-4 )
	{
		outUV = float2( 0.0, 0.0 );
		return false;
	}
	float  s   = dot( L - P, n ) / dn;
	float3 Q   = P + s * dir;
	float3 rel = Q - L;
	outUV = float2( dot( rel, u ), dot( rel, v ) );
	return true;
}

// Fast path for a vertex that the caller has ALREADY slab-clipped: its depth dn = dot(A-P,n) is known and
// guaranteed in [swEps, distPL], so the projection can never fail (no bool) and never produce NaN/Inf. Two
// identities collapse the general Project: (1) dot(L-P,n) is exactly distPL because n = (L-P)/|L-P|, so
// s = distPL/dn - no re-derivation; (2) the disk basis (u,v) is perpendicular to n, so dot(L-P,u)=dot(L-P,v)=0
// and the disk-plane offset is simply s*(dot(dir,u), dot(dir,v)) - no Q/rel reconstruction. Algebraically
// identical to SoftDisk_Project, just far fewer ops on the hot per-edge path.
float2 SoftDisk_ProjClipped( float3 A, float3 P, float dn, float distPL, float3 u, float3 v )
{
	float3 dir = A - P;
	float  s   = distPL / dn;
	return s * float2( dot( dir, u ), dot( dir, v ) );
}

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

#endif // __SOFTWEDGE_COVERAGE_INC__
