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

// Analytic soft shadow volumes: penumbra wedge pixel shader. For each covered pixel it
// reconstructs the world-space receiver from the scene depth, then evaluates one silhouette
// edge's analytic occlusion of the area light and writes the SIGNED delta relative to the hard
// (point-light) shadow into the R32F accumulation buffer. Summed over the silhouette edges (plus
// the hard-shadow baseline that seeds occlusion=1 inside the projection), the accumulator holds the
// occluded fraction of the light; the resolve pass turns it into visibility = 1 - saturate(accum).

#include "global_inc.hlsl"
#include "renderParmSet13.inc.hlsl"

// the light source is stored in the spare param: xyz = world origin, w = radius (world units)
#define rpLightOriginRadius	pc.rpJitterTexScale

// *INDENT-OFF*
Texture2D		t_SceneDepth	: register( t0 VK_DESCRIPTOR_SET( 0 ) );
SamplerState	s_LinearClamp	: register( s0 VK_DESCRIPTOR_SET( 1 ) );

struct PS_IN
{
	float4 position						: SV_Position;
	nointerpolation float3 edge0		: TEXCOORD0;
	nointerpolation float3 edge1		: TEXCOORD1;
	nointerpolation float  silWeight	: TEXCOORD2;
};

struct PS_OUT
{
	float4 color : SV_Target0;
};
// *INDENT-ON*

// reconstruct the world-space position of the pixel from the depth buffer. rpModelMatrix is set to
// the view's unprojection-to-world matrix for this pass (mirrors the SSAO reconstruction).
float3 ReconstructWorldPos( float2 screenPos, float depth )
{
	float2 uv = screenPos * pc.rpWindowCoord.xy;
	float3 ndc = float3( uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth );
	float clipW = -pc.rpProjectionMatrixZ.w / ( -pc.rpProjectionMatrixZ.z - ndc.z );

	float4 clip = float4( ndc * clipW, clipW );

	float4 wP;
	wP.x = dot4( pc.rpModelMatrixX, clip );
	wP.y = dot4( pc.rpModelMatrixY, clip );
	wP.z = dot4( pc.rpModelMatrixZ, clip );
	wP.w = dot4( pc.rpModelMatrixW, clip );

	return wP.xyz / wP.w;
}

// coverage of a unit disk on the far side of a chord at signed distance t from the centre.
// t = -1 -> whole disk occluded (1); t = +1 -> nothing occluded (0). COVERAGE_MODEL will select
// among sphere / rect / LUT variants later; the sphere (circular segment) is the first cut.
float CoverageSphere( float t )
{
	t = clamp( t, -1.0, 1.0 );
	return ( acos( t ) - t * sqrt( max( 0.0, 1.0 - t * t ) ) ) / PI;
}

void main( PS_IN fragment, out PS_OUT result )
{
	float depth = t_SceneDepth.Load( int3( int2( fragment.position.xy ), 0 ) ).r;
	float3 P = ReconstructWorldPos( fragment.position.xy, depth );

	float3 L = rpLightOriginRadius.xyz;
	float  R = rpLightOriginRadius.w;
	if( R <= 0.0 )
	{
		result.color = float4( 0.0, 0.0, 0.0, 0.0 );
		return;
	}

	float3 E0 = fragment.edge0;
	float3 E1 = fragment.edge1;
	float3 edgeMid = 0.5 * ( E0 + E1 );

	// The wedge only rasterises a SCREEN footprint; the receiver reconstructed from the depth buffer
	// may be any surface behind that footprint, not one inside the shadow volume. Without the tests
	// below, every such surface gets this edge's coverage painted on it - silhouette-shaped blobs
	// anchored in world space that swim with the camera, and shadow smeared onto lit geometry.

	// Occlusion gate: the edge must lie BETWEEN the light and the receiver, i.e. the receiver is on
	// the far (shadowed) side of the edge. toShadow points from the light through the edge; a
	// receiver in this edge's shadow is farther along it than the edge is.
	float3 toShadow = normalize( edgeMid - L );
	float distER = dot( P - edgeMid, toShadow );	// receiver's distance past the edge along the shadow
	if( distER <= 0.0 )
	{
		result.color = float4( 0.0, 0.0, 0.0, 0.0 );	// receiver is beside/in front of the caster: no shadow
		return;
	}

	// Umbra boundary plane: the plane through the silhouette edge and the light centre is exactly the
	// hard (point-light) shadow boundary this edge contributes. Signed receiver distance to it, in
	// LOCAL penumbra half-widths, is the coverage argument. Winding is baked (generation orders the
	// edge by facing) so the sign is consistent around the silhouette loop.
	float3 N = cross( E1 - E0, L - E0 );
	float Nl = length( N );
	if( Nl < 1e-6 )
	{
		result.color = float4( 0.0, 0.0, 0.0, 0.0 );	// edge collinear with the light: no defined boundary
		return;
	}
	N /= Nl;
	float dP = dot( P - E0, N );

	// Penumbra half-width grows with receiver distance past the edge and shrinks with light distance
	// (contact hardening): sharp where the caster touches the receiver, soft far away.
	// Floor the light->edge distance at the light radius: an edge within R of the light would send
	// halfWidth (and thus the covered region) to infinity and make it hypersensitive to light motion
	// - the flicker pinned to light sources. At/inside the light surface the penumbra is just the
	// receiver distance.
	float distLE = max( length( L - edgeMid ), R );
	// Floor the half-width (rpJitterTexOffset.x, r_shadowPenumbraMinWidth). At contact distER->0 drives
	// the half-width toward 0, so t = dP/halfWidth explodes and the coverage swings by a full step for
	// the sub-pixel wobble in the depth-reconstructed receiver P - that is the motion churn. Flooring
	// the half-width caps the coverage gradient (1/halfWidth), trading contact sharpness for stability.
	float halfWidth = max( R * distER / distLE, max( pc.rpJitterTexOffset.x, 1e-4 ) );

	float t = dP / halfWidth;			// -1 = inner penumbra (umbra), +1 = outer (lit)
	float f = CoverageSphere( t );		// analytic occlusion of this edge
	float hard = ( t < 0.0 ) ? 1.0 : 0.0;	// point-light (R->0) step this wedge corrects

	// Continuous silhouette weight: an edge near the facing threshold contributes almost nothing and
	// grows to full as it becomes a firm silhouette, so a wedge FADES in/out over a few degrees of
	// motion instead of the binary in/out test snapping the whole penumbra on and off (the flicker).
	// pc.rpJitterTexOffset.y is the per-invocation coverage scale (0.5 two-sided = front+back nets one;
	// 1.0 single-sided). r_softShadowTwoSided picks it so the two-sided workaround is re-evaluable.
	result.color = float4( pc.rpJitterTexOffset.y * fragment.silWeight * ( f - hard ), 0.0, 0.0, 0.0 );
}
