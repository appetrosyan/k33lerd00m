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

// Analytic soft shadows - penumbra CONFINE volume (vertex shader).
//
// Builds one of the two geometric confines that bracket the penumbra, so the expensive per-fragment
// coverage shader (interactionSM.ps.hlsl USE_SOFT_WEDGE) runs ONLY between them. The literature
// (Akenine-Möller & Assarsson, "Approximate Soft Shadows using Penumbra Wedges", section 4.1) brackets
// the penumbra with a per-EDGE apex f = c - r*n (outer) / b = c + r*n (umbra). But a per-edge apex makes
// a shared silhouette vertex extrude to two different far points, so neighbouring prism walls gap at
// convex corners (unfilled shadow) and flap if the edges are lengthened to close the gap.
//
// Instead we keep the apex SHARED (the light centre c, as in an ordinary point-light shadow volume ->
// watertight, no gaps/flaps) and get the confine by inflating the silhouette RADIALLY about the caster
// centre by r' before extruding: V' = V + (-apexSign*r')*normalize(V-centre). apexSign -1 inflates ->
// the point-light shadow of the enlarged silhouette is a conservative superset of the combined
// umbra+penumbra region (OUTER confine); apexSign +1 deflates -> a conservative subset (UMBRA confine).
// V' depends only on V and the caster centre, so a shared vertex inflates identically in both its edges.
//
// This VS emits, per soft silhouette edge, ONLY the side quad (inflated edge swept to infinity from c) =
// the prism walls. Summed over a caster's closed silhouette loop they form the (open) infinite shadow
// prism, a valid CAPLESS z-PASS shadow volume (Carmack's original): z-pass counts front-minus-back walls
// IN FRONT of each fragment, so containment is correct and nothing bleeds through world geometry behind
// it. No caps: a valid cap needs the caster's interior triangles the edge buffer does not carry, and
// capless z-FAIL would mark the whole infinite far prism (bleed). z-pass is correct while the eye is
// outside the shadow; the eye-inside case is handled later.
//
// Procedural: no vertex buffer. Reads the per-light silhouette edges from t12 (softShadowEdge_t = 2
// float4/record: e0.xyz/e1.xyz = endpoints; header records have e0.w<0, carry the caster centre in
// e0.xyz, and each edge stashes its caster's header index in e1.w). rpGlobalLightOrigin = c;
// rpJitterTexScale = (r, apexSign, marginScale, -); rpJitterTexOffset.x = first element index.

#include "global_inc.hlsl"
#include "renderParmSet11.inc.hlsl"

// *INDENT-OFF*
StructuredBuffer<float4> t_SoftEdges : register( t12 VK_DESCRIPTOR_SET( 0 ) );

struct VS_OUT
{
	float4 position : SV_Position;
};

// 12 verts = 4 triangles per silhouette edge: the side quad (prism wall) + a near-cap fan triangle at
// the caster + a far-cap fan triangle at infinity. Corner ids:
//   0 = A' (w1)   1 = B' (w1)   2 = A'_inf (w0)   3 = B'_inf (w0)   4 = centre (w1)   5 = centre_inf (w0)
// The caps close each caster's prism into a WATERTIGHT oriented volume (fan apex = the caster centre from
// the header record, which inflates onto itself; the far fan apex is its extrusion). Watertightness is what
// lets the backend mark the shell with z-FAIL (Carmack's reverse): the count then depends only on the
// FRAGMENT's containment, not on the camera. The previous capless z-PASS marking counted crossings of the
// camera->fragment segment, which is wrong whenever the camera sits inside a penumbra prism - fat, infinite
// volumes the camera is inside all the time - producing camera-dependent penumbra classification (the
// tripod shadow that changed as the camera moved; reproduced by SoftShadowPipeline tests).
// Orientation: each cap triangle traverses its shared boundary edge OPPOSITE to the side quad (near cap
// B'->A', far cap A'_inf->B'_inf), so the closed surface is consistently oriented; holes (loops wound the
// other way) count negative and carve out of the shell, as they should.
static const uint volIdx[12] =
{
	0, 1, 3,	// side quad tri 1  (A', B', B'_inf)
	0, 3, 2,	// side quad tri 2  (A', B'_inf, A'_inf)
	4, 1, 0,	// near-cap fan     (centre, B', A')
	5, 2, 3		// far-cap fan      (centre_inf, A'_inf, B'_inf)
};

VS_OUT main( uint vertexId : SV_VertexID )
{
	VS_OUT result;

	const uint rec = vertexId / 12u;
	const uint c   = volIdx[ vertexId % 12u ];

	const uint first = uint( pc.rpJitterTexOffset.x );
	const float4 e0 = t_SoftEdges[ first + rec * 2u + 0u ];
	const float4 e1 = t_SoftEdges[ first + rec * 2u + 1u ];

	// header record (e0.w < 0) is a caster boundary, not an edge: collapse to a degenerate point.
	if( e0.w < 0.0 )
	{
		result.position = float4( 0.0, 0.0, 0.0, 1.0 );
		return result;
	}

	float3 A = e0.xyz;
	float3 B = e1.xyz;
	const float3 L = pc.rpGlobalLightOrigin.xyz;

	const float r        = max( pc.rpJitterTexScale.x, 1e-2 );	// light radius = penumbra size
	const float apexSign = pc.rpJitterTexScale.y;				// -1 = outer (inflate), +1 = umbra (deflate)
	const float margin   = max( pc.rpJitterTexScale.z, 1.0 );	// conservative over/under-inflate factor

	const float rp = r * margin;								// inflate distance r' = penumbra half-width * margin

	// A per-edge apex (f = c - r'n) makes a shared silhouette vertex extrude to TWO different far points, so
	// neighbouring prism walls gap at convex corners (unfilled shadow) and flap if lengthened. Instead inflate
	// the silhouette RADIALLY about the caster centre by r', then extrude from the light centre c: the apex is
	// SHARED across all edges (watertight, no gaps/flaps) and V' depends only on V and the centre, so a shared
	// vertex inflates identically in both its edges. Inflating (apexSign -1) grows the shadow -> conservative
	// OUTER confine (>= combined region); deflating (+1) shrinks it -> conservative UMBRA confine. The caster
	// centre is the header record (e0.xyz) whose index this edge stashes in e1.w.
	const uint hdr = uint( e1.w );
	const float3 ctr = t_SoftEdges[ first + hdr * 2u + 0u ].xyz;
	const float3 dA = A - ctr;
	const float3 dB = B - ctr;
	A += ( -apexSign * rp ) * ( length( dA ) > 1e-4 ? normalize( dA ) : float3( 0.0, 0.0, 0.0 ) );
	B += ( -apexSign * rp ) * ( length( dB ) > 1e-4 ? normalize( dB ) : float3( 0.0, 0.0, 0.0 ) );

	// corner positions. *_inf are extruded to infinity from the light centre L (w=0 direction) - a point-light
	// shadow of the inflated silhouette. Shared apex L keeps the summed prism watertight; the cap fans use the
	// caster centre (inflation maps it onto itself) and its extrusion.
	float3 posW;
	float  w;
	switch( c )
	{
		case 0u:  posW = A;         w = 1.0; break;	// A'
		case 1u:  posW = B;         w = 1.0; break;	// B'
		case 2u:  posW = A - L;     w = 0.0; break;	// A'_inf
		case 3u:  posW = B - L;     w = 0.0; break;	// B'_inf
		case 4u:  posW = ctr;       w = 1.0; break;	// near-cap fan apex
		default:  posW = ctr - L;   w = 0.0; break;	// far-cap fan apex
	}

	// world -> clip (rpMVPmatrix set to world->clip for this pass).
	const float4 mp = float4( posW, w );
	result.position.x = dot4( mp, pc.rpMVPmatrixX );
	result.position.y = dot4( mp, pc.rpMVPmatrixY );
	result.position.z = dot4( mp, pc.rpMVPmatrixZ );
	result.position.w = dot4( mp, pc.rpMVPmatrixW );

	return result;
}
