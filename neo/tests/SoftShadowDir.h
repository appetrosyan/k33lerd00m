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

// DIRECTION-SPACE soft-shadow coverage (C++ reference under development). The shipped coverage
// (softwedge_coverage.inc.hlsl) projects the silhouette onto the flat light disk by central projection,
// which divides by dn = depth-along-P->L and blows up as a vertex nears the receiver near-plane. That
// singularity forces the near-plane clip + straight connector, whose chord cuts the disk to a half - the
// deep-umbra semicircle / "ant" artifact on pillars.
//
// Here every world vertex V maps to a DIRECTION d = normalize(V - P) on the unit sphere, finite for every
// V (behind-P vertices just point into the back hemisphere). The light disk becomes a spherical CAP about
// pole nL = normalize(L - P), half-angle alpha with cos(alpha) = distPL / sqrt(distPL^2 + r^2). Occlusion =
// signed solid angle( cap INTERSECT silhouette-region ) / cap solid angle, summed per edge and winding-
// correct like the planar version - but with no 1/dn, no clip, no chord. A pillar wrapping around P maps to
// a loop that encloses the cap => full solid angle => solid umbra, automatically.
//
// Per-edge contribution CapTri(dA,dB) = signed solid angle of cap INTERSECT spherical-triangle(nL,dA,dB):
//   both dirs INSIDE cap  -> Van Oosterom-Strackee signed solid angle of triangle(nL,dA,dB)
//   both OUTSIDE, no dip   -> cap sector (1-cosAlpha)*dphi, dphi = signed azimuth sweep about nL (finite)
//   crossing               -> split at the cap-boundary intersection; triangle part + sector part
// This is the reference; once green it ports to HLSL (single-component only).

#ifndef __SOFTSHADOWDIR_H__
#define __SOFTSHADOWDIR_H__

#include <vector>
#include <cmath>

namespace swtest
{

static const float SW_PI = 3.14159265358979323846f;

// signed solid angle of the spherical triangle (O, a, b, c) subtended at the origin (unit vectors).
// Van Oosterom & Strackee: tan(Omega/2) = (a . (b x c)) / (1 + a.b + b.c + c.a).
inline float SphTriSolidAngle( float3 a, float3 b, float3 c )
{
	float num = dot( a, cross( b, c ) );
	float den = 1.0f + dot( a, b ) + dot( b, c ) + dot( c, a );
	return 2.0f * std::atan2( num, den );
}

// azimuth of direction d about pole n, in the (u,v) tangent frame (orthographic projection onto n's plane).
inline float SwAzimuth( float3 d, float3 u, float3 v )
{
	return std::atan2( dot( d, v ), dot( d, u ) );
}

// wrap x into (-PI, PI]
inline float SwWrapPi( float x )
{
	while( x > SW_PI )  { x -= 2.0f * SW_PI; }
	while( x <= -SW_PI ) { x += 2.0f * SW_PI; }
	return x;
}

// ---------------------------------------------------------- decomposed intermediates (unit-testable)

// great-circle arc basis: along the arc x(t) = cos(t)*dA + sin(t)*e2, t in [0, omega].
struct CapArcBasisT
{
	float  omega;		// arc length dA..dB (radians)
	float3 e2;			// unit vector completing the arc-plane basis with dA
	bool   degenerate;	// endpoints coincide (or are antipodal: e2 undefined)
};
inline CapArcBasisT CapArcBasis( float3 dA, float3 dB )
{
	CapArcBasisT b;
	b.omega = std::acos( std::fmax( -1.0f, std::fmin( 1.0f, dot( dA, dB ) ) ) );
	b.e2 = float3( 0, 0, 0 );
	b.degenerate = true;
	if( b.omega < 1e-6f )
	{
		return b;						// degenerate edge (endpoints coincide)
	}
	float3 e2 = dB - dA * dot( dA, dB );
	float  e2len = length( e2 );
	if( e2len < 1e-6f )
	{
		return b;						// antipodal: the arc plane is undefined
	}
	b.e2 = e2 * ( 1.0f / e2len );
	b.degenerate = false;
	return b;
}

// parameters t strictly inside (0, omega) where the arc crosses the cap boundary dot(x,n) = cosA:
//   (dA.n) cos t + (e2.n) sin t = cosA  ->  R cos(t - psi) = cosA
struct CapCrossT
{
	int   nt;			// 0, 1 or 2 crossings inside the open arc
	float ts[2];		// sorted ascending
};
inline CapCrossT CapArcCrossings( float3 dA, float3 e2, float omega, float3 n, float cosA )
{
	CapCrossT c;
	c.nt = 0;
	c.ts[0] = 0.0f;
	c.ts[1] = 0.0f;
	float en1 = dot( dA, n );
	float en2 = dot( e2, n );
	float R = std::sqrt( en1 * en1 + en2 * en2 );
	if( R < 1e-6f || cosA / R > 1.0f )
	{
		return c;						// great circle never reaches the cap boundary
	}
	float psi = std::atan2( en2, en1 );
	float da  = std::acos( std::fmax( -1.0f, std::fmin( 1.0f, cosA / R ) ) );
	float t1 = psi - da;
	float t2 = psi + da;
	if( t1 > 1e-6f && t1 < omega - 1e-6f ) { c.ts[c.nt++] = t1; }
	if( t2 > 1e-6f && t2 < omega - 1e-6f ) { c.ts[c.nt++] = t2; }
	if( c.nt == 2 && c.ts[0] > c.ts[1] ) { float tmp = c.ts[0]; c.ts[0] = c.ts[1]; c.ts[1] = tmp; }
	return c;
}

// walk the arc [0..omega] split at the crossings, accumulating: inside-cap sub-arcs as triangle solid
// angle (apex at the pole), outside-cap sub-arcs as cap-boundary sector (azimuth sweep * (1-cosA)).
inline float CapSubArcWalk( float3 dA, float3 e2, float omega, const CapCrossT& cross,
							float3 n, float3 u, float3 v, float cosA )
{
	float bounds[4]; int nb = 0;
	bounds[nb++] = 0.0f;
	for( int i = 0; i < cross.nt; i++ ) { bounds[nb++] = cross.ts[i]; }
	bounds[nb++] = omega;

	float total = 0.0f;
	for( int i = 0; i + 1 < nb; i++ )
	{
		float t0 = bounds[i], t1b = bounds[i + 1];
		float tmid = 0.5f * ( t0 + t1b );
		float3 p0 = dA * std::cos( t0 )  + e2 * std::sin( t0 );
		float3 p1 = dA * std::cos( t1b ) + e2 * std::sin( t1b );
		float3 pm = dA * std::cos( tmid ) + e2 * std::sin( tmid );
		if( dot( pm, n ) >= cosA )
		{
			total += SphTriSolidAngle( n, p0, p1 );		// this sub-arc is inside the cap
		}
		else
		{
			float dp = SwWrapPi( SwAzimuth( p1, u, v ) - SwAzimuth( p0, u, v ) );
			total += ( 1.0f - cosA ) * dp;				// outside: cap-boundary sector
		}
	}
	return total;
}

// signed solid angle of cap(pole n, half-angle alpha) INTERSECT spherical triangle (n, dA, dB).
// u,v complete an orthonormal frame with n; cosA = cos(alpha).
inline float CapTri( float3 dA, float3 dB, float3 n, float3 u, float3 v, float cosA )
{
	float cA = dot( dA, n );			// cos(angle from pole) for each endpoint
	float cB = dot( dB, n );
	bool  inA = ( cA >= cosA );
	bool  inB = ( cB >= cosA );

	float phiA = SwAzimuth( dA, u, v );
	float phiB = SwAzimuth( dB, u, v );
	float dphi = SwWrapPi( phiB - phiA );	// signed azimuth sweep dA->dB about the pole (the sector measure)
	float sector = ( 1.0f - cosA ) * dphi;	// solid angle of the cap boundary swept through dphi

	if( inA && inB )
	{
		// the whole edge is inside the cap: the triangle (n,dA,dB) lies within the cap -> exact solid angle
		return SphTriSolidAngle( n, dA, dB );
	}

	CapArcBasisT arc = CapArcBasis( dA, dB );
	if( arc.degenerate )
	{
		return 0.0f;
	}
	CapCrossT cross = CapArcCrossings( dA, arc.e2, arc.omega, n, cosA );
	if( cross.nt == 0 )
	{
		// arc never dips inside the cap (endpoints outside, no interior crossing) -> pure sector
		return sector;
	}
	return CapSubArcWalk( dA, arc.e2, arc.omega, cross, n, u, v, cosA );
}

// CapTri on a WORLD edge wA->wB seen from swP. When the endpoint directions are near-antipodal (edge
// passing close to the receiver) the great-circle plane degenerates and CapTri dropped the whole
// contribution (finding F11: half the cap lost at a grazing edge). The world midpoint resolves the
// plane exactly - subdividing the edge is an identity on the direction path - so split and recurse
// once per level until the halves are well-conditioned. An edge passing exactly THROUGH swP has no
// defined silhouette contribution; that residual degenerate returns 0.
inline float CapTriWorld( float3 wA, float3 wB, float3 swP, float3 n, float3 u, float3 v, float cosA, int depth = 8 )
{
	float3 dA = normalize( wA - swP );
	float3 dB = normalize( wB - swP );
	if( dot( dA, dB ) > -0.99f || depth <= 0 )
	{
		return CapTri( dA, dB, n, u, v, cosA );
	}
	float3 wM( ( wA.x + wB.x ) * 0.5f, ( wA.y + wB.y ) * 0.5f, ( wA.z + wB.z ) * 0.5f );
	float3 rel = wM - swP;
	if( dot( rel, rel ) < 1e-12f )
	{
		return 0.0f;						// edge passes through the receiver point itself
	}
	return CapTriWorld( wA, wM, swP, n, u, v, cosA, depth - 1 )
		 + CapTriWorld( wM, wB, swP, n, u, v, cosA, depth - 1 );
}

// full per-fragment occlusion in [0,1] (1 = umbra), same record layout as SoftShadow_WedgeOcclusion.
inline float DirOcclusion( const std::vector<float4>& rec, float3 swP, float3 swL, float swR )
{
	swR = std::fmax( swR, 1e-2f );
	float3 toL = swL - swP;
	float  distPL = std::fmax( length( toL ), 1e-4f );
	float3 n = toL * ( 1.0f / distPL );
	float3 up = ( std::fabs( n.z ) > 0.9f ) ? float3( 0, 1, 0 ) : float3( 0, 0, 1 );
	float3 u = normalize( cross( up, n ) );
	float3 v = cross( n, u );
	float  cosA = distPL / std::sqrt( distPL * distPL + swR * swR );		// cap half-angle
	float  capSolid = 2.0f * SW_PI * ( 1.0f - cosA );

	float occ = 0.0f, sum = 0.0f;
	bool  have = false, skip = false;
	bool  firstValid = false; float3 wFirst( 0, 0, 0 ), wPrev( 0, 0, 0 );	// WORLD chain endpoints (CapTriWorld)
	bool  havePrev = false; float3 prevBw( 0, 0, 0 );
	int   nrec = ( int )( rec.size() / 2 );
	for( int se = 0; se < nrec; se++ )
	{
		float4 e0 = rec[se * 2 + 0], e1 = rec[se * 2 + 1];
		if( e0.w < 0.0f )
		{
			if( have && firstValid ) { sum += CapTriWorld( wPrev, wFirst, swP, n, u, v, cosA ); }
			if( have ) { occ = std::fmax( occ, std::fmin( std::fabs( sum ) / capSolid, 1.0f ) ); }
			have = true; sum = 0.0f; skip = false; firstValid = false; havePrev = false;
			// caster bounding-sphere cull (centre e0.xyz, radius e1.x). Without it, a caster wholly BEHIND
			// the receiver winds the azimuth around the axis' antipodal piercing and reads as FULL
			// occlusion (findings F1/F2): the sector measure cannot tell +n winding from -n winding, so
			// geometry that can contribute nothing must never enter the sum.
			float3 dCv = float3( e0.x, e0.y, e0.z ) - swP;
			float  dCn = dot( dCv, n );
			if( dCn + e1.x < 0.0f || dCn - e1.x > distPL ) { skip = true; }
			continue;
		}
		if( skip ) { continue; }
		float3 A( e0.x, e0.y, e0.z ), B( e1.x, e1.y, e1.z );
		if( havePrev && ( A.x != prevBw.x || A.y != prevBw.y || A.z != prevBw.z ) && firstValid )
		{
			sum += CapTriWorld( wPrev, wFirst, swP, n, u, v, cosA );
			firstValid = false;
		}
		havePrev = true; prevBw = B;
		if( firstValid ) { sum += CapTriWorld( wPrev, A, swP, n, u, v, cosA ); }
		else             { wFirst = A; firstValid = true; }
		sum += CapTriWorld( A, B, swP, n, u, v, cosA );
		wPrev = B;
	}
	if( have && firstValid ) { sum += CapTriWorld( wPrev, wFirst, swP, n, u, v, cosA ); }
	if( have ) { occ = std::fmax( occ, std::fmin( std::fabs( sum ) / capSolid, 1.0f ) ); }
	return occ;
}

inline float DirShadow( const std::vector<float4>& rec, float3 P, float3 L, float r )
{
	return 1.0f - std::fmin( DirOcclusion( rec, P, L, r ), 1.0f );
}

} // namespace swtest

#endif // __SOFTSHADOWDIR_H__
