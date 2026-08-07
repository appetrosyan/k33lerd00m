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

	// Does the great-circle arc dA->dB dip inside the cap? Closest approach of the great circle to n.
	float omega = std::acos( std::fmax( -1.0f, std::fmin( 1.0f, dot( dA, dB ) ) ) );	// arc length dA..dB
	if( omega < 1e-6f )
	{
		return 0.0f;						// degenerate edge
	}
	// orthonormal basis of the great-circle plane: e1 = dA, e2 = component of dB perpendicular to dA.
	float3 e2 = dB - dA * dot( dA, dB );
	float  e2len = length( e2 );
	if( e2len < 1e-6f )
	{
		return 0.0f;
	}
	e2 = e2 * ( 1.0f / e2len );
	// along the arc x(t) = cos t * e1 + sin t * e2, t in [0, omega]. dot(x,n) = cosA at the cap boundary:
	//   (e1.n) cos t + (e2.n) sin t = cosA  ->  R cos(t - psi) = cosA
	float en1 = dot( dA, n );
	float en2 = dot( e2, n );
	float R = std::sqrt( en1 * en1 + en2 * en2 );
	if( R < 1e-6f || cosA / R > 1.0f )
	{
		// great circle never reaches the cap (its closest approach angle > alpha): pure sector
		return sector;
	}
	float psi = std::atan2( en2, en1 );
	float da  = std::acos( std::fmax( -1.0f, std::fmin( 1.0f, cosA / R ) ) );
	float t1 = psi - da;
	float t2 = psi + da;
	// collect the crossings that fall strictly inside the arc (0, omega)
	float ts[2]; int nt = 0;
	if( t1 > 1e-6f && t1 < omega - 1e-6f ) { ts[nt++] = t1; }
	if( t2 > 1e-6f && t2 < omega - 1e-6f ) { ts[nt++] = t2; }
	if( nt == 0 )
	{
		// arc stays entirely outside the cap (endpoints outside, no interior crossing) -> sector
		return sector;
	}
	if( nt == 2 && ts[0] > ts[1] ) { float tmp = ts[0]; ts[0] = ts[1]; ts[1] = tmp; }

	// walk the arc [0..omega], accumulating: inside-cap sub-arcs as triangle solid angle, outside-cap
	// sub-arcs as sector (their azimuth sweep * (1-cosA)). Boundaries are 0, crossings..., omega.
	float bounds[4]; int nb = 0;
	bounds[nb++] = 0.0f;
	for( int i = 0; i < nt; i++ ) { bounds[nb++] = ts[i]; }
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
	bool  firstValid = false; float3 dFirst( 0, 0, 0 ), dPrev( 0, 0, 0 );
	bool  havePrev = false; float3 prevBw( 0, 0, 0 );
	int   nrec = ( int )( rec.size() / 2 );
	for( int se = 0; se < nrec; se++ )
	{
		float4 e0 = rec[se * 2 + 0], e1 = rec[se * 2 + 1];
		if( e0.w < 0.0f )
		{
			if( have && firstValid ) { sum += CapTri( dPrev, dFirst, n, u, v, cosA ); }
			if( have ) { occ = std::fmax( occ, std::fmin( std::fabs( sum ) / capSolid, 1.0f ) ); }
			have = true; sum = 0.0f; skip = false; firstValid = false; havePrev = false;
			continue;
		}
		if( skip ) { continue; }
		float3 A( e0.x, e0.y, e0.z ), B( e1.x, e1.y, e1.z );
		if( havePrev && ( A.x != prevBw.x || A.y != prevBw.y || A.z != prevBw.z ) && firstValid )
		{
			sum += CapTri( dPrev, dFirst, n, u, v, cosA );
			firstValid = false;
		}
		havePrev = true; prevBw = B;
		float3 dA = normalize( A - swP );
		float3 dB = normalize( B - swP );
		if( firstValid ) { sum += CapTri( dPrev, dA, n, u, v, cosA ); }
		else             { dFirst = dA; firstValid = true; }
		sum += CapTri( dA, dB, n, u, v, cosA );
		dPrev = dB;
	}
	if( have && firstValid ) { sum += CapTri( dPrev, dFirst, n, u, v, cosA ); }
	if( have ) { occ = std::fmax( occ, std::fmin( std::fabs( sum ) / capSolid, 1.0f ) ); }
	return occ;
}

inline float DirShadow( const std::vector<float4>& rec, float3 P, float3 L, float r )
{
	return 1.0f - std::fmin( DirOcclusion( rec, P, L, r ), 1.0f );
}

} // namespace swtest

#endif // __SOFTSHADOWDIR_H__
