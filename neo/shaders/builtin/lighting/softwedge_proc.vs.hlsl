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

// Analytic soft shadows - penumbra WEDGE-COVERAGE vertex shader (procedural).
//
// Fuses two existing shaders so the disabled screen-space accumulate pass can be fed from the per-light
// silhouette-edge cache instead of dead CPU-only wedge surfs:
//   - geometry: softband.vs.hlsl's procedural expansion (12 verts = 4 tris per edge, watertight prism,
//     read from t12, no vertex buffer). apexSign is FIXED to -1 (outer/inflate) so the prism conservatively
//     COVERS the whole penumbra region where this edge's coverage is nonzero; the PS then computes the
//     analytic coverage per covered fragment.
//   - varyings: softwedge.vs.hlsl's flat edge0/edge1/silWeight the PS (softwedge.ps.hlsl) consumes. edge0/
//     edge1 are the ORIGINAL (un-inflated) endpoints - the PS needs the true silhouette edge, not the
//     inflated prism wall. silWeight = e0.w (the per-edge grazing fade populated by R_CollectPenumbraEdges).
//
// Unified onto renderParmSet11 (a VS and PS in one pipeline share the single b0 CB). The two overloaded
// jitter vectors are repacked so both stages read one packing: rpJitterTexScale = (lightOrigin.xyz, radius),
// rpJitterTexOffset = (minWidth, coverageScale, firstElemIndex, -). The VS reads L,r from rpJitterTexScale
// and the first element index from rpJitterTexOffset.z; the PS reads the same vectors for its own fields.

#include "global_inc.hlsl"
#include "renderParmSet11.inc.hlsl"

// *INDENT-OFF*
StructuredBuffer<float4> t_SoftEdges : register( t12 VK_DESCRIPTOR_SET( 0 ) );

struct VS_OUT
{
	float4 position						: SV_Position;
	nointerpolation float3 edge0		: TEXCOORD0;
	nointerpolation float3 edge1		: TEXCOORD1;
	nointerpolation float  silWeight	: TEXCOORD2;
};

// 12 verts = 4 triangles per silhouette edge: side quad (prism wall) + near-cap fan (caster centre) +
// far-cap fan (its extrusion). Identical corner ids to softband.vs.hlsl.
static const uint volIdx[12] =
{
	0, 1, 3,	// side quad tri 1  (A', B', B'_inf)
	0, 3, 2,	// side quad tri 2  (A', B'_inf, A'_inf)
	4, 1, 0,	// near-cap fan     (centre, B', A')
	5, 2, 3		// far-cap fan      (centre_inf, A'_inf, B'_inf)
};
// *INDENT-ON*

VS_OUT main( uint vertexId : SV_VertexID )
{
	VS_OUT result;
	result.edge0 = float3( 0.0, 0.0, 0.0 );
	result.edge1 = float3( 0.0, 0.0, 0.0 );
	result.silWeight = 0.0;

	const uint rec = vertexId / 12u;
	const uint c   = volIdx[ vertexId % 12u ];

	const uint first = uint( pc.rpJitterTexOffset.z );			// first element index (repacked into .z)
	const float4 e0 = t_SoftEdges[ first + rec * 2u + 0u ];
	const float4 e1 = t_SoftEdges[ first + rec * 2u + 1u ];

	// header record (e0.w < 0) is a caster boundary, not an edge: collapse to a degenerate point.
	if( e0.w < 0.0 )
	{
		result.position = float4( 0.0, 0.0, 0.0, 1.0 );
		return result;
	}

	// original (un-inflated) silhouette edge + its fade weight, forwarded flat to the coverage PS.
	const float3 A0 = e0.xyz;
	const float3 B0 = e1.xyz;
	result.edge0 = A0;
	result.edge1 = B0;
	result.silWeight = e0.w;

	// repacked: light origin.xyz + radius.w (shared with the PS). apexSign/margin are constants here - the
	// coverage prism always inflates (outer confine) so it covers every penumbra pixel of this edge.
	const float3 L        = pc.rpJitterTexScale.xyz;
	const float  r        = max( pc.rpJitterTexScale.w, 1e-2 );	// light radius = penumbra size
	const float  apexSign = -1.0;								// outer confine (inflate)
	const float  margin   = 1.1;								// conservative over-inflate

	const float rp = r * margin;								// inflate distance r' = penumbra half-width * margin

	// inflate the silhouette RADIALLY about the caster centre by r', then extrude from the light centre L:
	// the apex is SHARED (watertight, no gaps/flaps) and V' depends only on V and the centre. The caster
	// centre is the header record (e0.xyz) whose index this edge stashes in e1.w.
	float3 A = A0;
	float3 B = B0;
	const uint hdr = uint( e1.w );
	const float3 ctr = t_SoftEdges[ first + hdr * 2u + 0u ].xyz;
	const float3 dA = A - ctr;
	const float3 dB = B - ctr;
	A += ( -apexSign * rp ) * ( length( dA ) > 1e-4 ? normalize( dA ) : float3( 0.0, 0.0, 0.0 ) );
	B += ( -apexSign * rp ) * ( length( dB ) > 1e-4 ? normalize( dB ) : float3( 0.0, 0.0, 0.0 ) );

	// corner positions. *_inf are extruded to infinity from L (w=0 direction); the cap fans use the caster
	// centre (inflation maps it onto itself) and its extrusion. Shared apex L keeps the summed prism watertight.
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
