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

// The two penumbra CONFINES as exact point-membership predicates, for validating the hybrid (solid-umbra +
// analytic-penumbra) in C++ before any GPU stencil is written. A box silhouette seen from one light is
// CONVEX, so a receiver P is:
//   * UMBRA  (fully occluded)  iff the light disk projects fully INSIDE the silhouette as seen from P
//   * LIT    (fully unblocked) iff the disk projects fully OUTSIDE at least one silhouette edge
//   * PENUMBRA otherwise.
// Per edge (A,B) the deciding plane is plane(A,B,P); the light disk (centre c, radius r, facing P) is on the
// interior side, the exterior side, or straddling it. This is the exact classification the two shippable
// shadow VOLUMES (umbra = edges tangent to the disk's far side; outer = tangent to the near side) rasterise;
// validating it here validates that geometry. Uses the light DISK, not an apex approximation, so it is exact
// for any r (the earlier c +/- r*n apex was only first-order in r/D and failed at r ~ D).
// Depends on hlsl_compat.h + SoftShadowBox.h (float3 ops, Box, Silhouette) included first.

#ifndef __SOFTSHADOWCONFINE_H__
#define __SOFTSHADOWCONFINE_H__

#include <vector>
#include <cmath>

namespace swtest
{

inline float3 LoopCentre( const std::vector<float3>& loop )
{
	float3 s( 0, 0, 0 );
	for( float3 v : loop ) { s = s + v; }
	return s * ( 1.0f / std::max( 1, ( int )loop.size() ) );
}

// Which side of the edge (A,B) does the whole light disk fall on, as seen from receiver P?
//   +1 = disk entirely on the caster-INTERIOR side  (this edge fully blocks that flank)
//   -1 = disk entirely on the EXTERIOR side          (light leaks past this edge => P is lit)
//    0 = disk straddles the edge plane               (penumbra contribution from this edge)
// g = caster centroid, used only to orient "interior". The plane is plane(A,B,P); the disk half-width along
// the plane normal is r * sin(angle between normal and disk axis) since the disk faces P.
inline int EdgeDiskSide( float3 P, float3 A, float3 B, float3 g, float3 c, float r )
{
	float3 nu = cross( B - A, P - A );
	float  ln = length( nu );
	if( ln < 1e-8f ) { return 0; }							// degenerate (P on the edge line): no opinion
	nu = nu * ( 1.0f / ln );
	if( dot( nu, g - A ) < 0.0f ) { nu = nu * -1.0f; }		// +nu now points to the caster-interior side
	float  dc = dot( nu, c - A );							// light-centre signed distance (interior positive)
	float3 dn = normalize( P - c );							// disk axis (the disk faces the receiver)
	float  cosw = dot( nu, dn );
	float  hw = r * std::sqrt( std::fmax( 0.0f, 1.0f - cosw * cosw ) );	// disk half-width projected onto nu
	if( dc >  hw ) { return +1; }
	if( dc < -hw ) { return -1; }
	return 0;
}

// UMBRA: every edge fully blocks => disk fully inside the silhouette => fully occluded. Conservative SUBSET
// of the true umbra (convex-exact; a concave caster would need winding, not a plain AND).
inline bool InUmbraConfine( float3 P, const std::vector<float3>& loop, float3 c, float r )
{
	int n = ( int )loop.size();
	if( n < 3 ) { return false; }
	float3 g = LoopCentre( loop );
	for( int i = 0; i < n; i++ )
	{
		if( EdgeDiskSide( P, loop[i], loop[( i + 1 ) % n], g, c, r ) != +1 ) { return false; }
	}
	return true;
}

// OUTER: no edge lets the whole disk leak past => at least partially occluded. Conservative SUPERSET of the
// penumbra+umbra (never classifies a truly-occluded fragment as lit).
inline bool InOuterConfine( float3 P, const std::vector<float3>& loop, float3 c, float r )
{
	int n = ( int )loop.size();
	if( n < 3 ) { return false; }
	float3 g = LoopCentre( loop );
	for( int i = 0; i < n; i++ )
	{
		if( EdgeDiskSide( P, loop[i], loop[( i + 1 ) % n], g, c, r ) == -1 ) { return false; }
	}
	return true;
}

} // namespace swtest

#endif // __SOFTSHADOWCONFINE_H__
