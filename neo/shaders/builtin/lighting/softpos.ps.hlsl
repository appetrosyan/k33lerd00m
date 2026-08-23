/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 2026 Aleksandr Petrosyan

This program is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version. See <http://www.gnu.org/licenses/>.

===========================================================================
*/

// Analytic soft shadows, EXACT-POSITION G-buffer (r_softShadowCompute): store the receiver WORLD
// position at RGBA32F. The transform is the same three dot4s the interaction PS runs on its
// interpolated texcoord7 (interactionSM.ps.hlsl ~line 137), on the same interpolated model
// position, so the stored value is bit-identical to the position the fragment integral would use.

#include "global_inc.hlsl"
#include "renderParmSet3.inc.hlsl"

// *INDENT-OFF*
Texture2D	 t_NormalMap	: register( t0 VK_DESCRIPTOR_SET( 1 ) );	// per-surface bump map (softterm N.L early-out)
SamplerState s_Sampler		: register( s0 VK_DESCRIPTOR_SET( 2 ) );

struct PS_IN
{
	float4 position		: SV_Position;
	float4 texcoord0	: TEXCOORD0_centroid;	// interpolated model position
	float4 texcoord1	: TEXCOORD1_centroid;	// bump texcoord (.xy), PSX warp (.z)
	float3 texcoord2	: TEXCOORD2_centroid;	// world tangent-frame rows
	float3 texcoord3	: TEXCOORD3_centroid;
	float3 texcoord4	: TEXCOORD4_centroid;
};

struct PS_OUT
{
	float4 color  : SV_Target0;		// RGBA32F exact world position (.w = 1 valid)
	float4 normal : SV_Target1;		// RGBA16F world SHADING normal (raw signed; only its direction is used)
};
// *INDENT-ON*

void main( PS_IN fragment, out PS_OUT result )
{
	float4 modelPosition = float4( fragment.texcoord0.xyz, 1.0 );
	float3 worldPosition;
	worldPosition.x = dot4( pc.rpModelMatrixX, modelPosition );
	worldPosition.y = dot4( pc.rpModelMatrixY, modelPosition );
	worldPosition.z = dot4( pc.rpModelMatrixZ, modelPosition );
	result.color = float4( worldPosition, 1.0 );

	// WORLD SHADING NORMAL for the term's back-facing early-out. The tangent-space decode is IDENTICAL
	// to interactionSM.ps (same bump sample, same .wy-0.5 / z=sqrt(|dot-0.25|) / normalize), so the
	// world N.L the term computes matches the interaction's ldotN - and at the ldotN=0 prune boundary
	// the light contribution vanishes, so any residual difference costs nothing. Rotated to world by
	// the interpolated tangent frame (gbuffer.ps convention).
	float2 bumpUV = fragment.texcoord1.xy;
	if( pc.rpPSXDistortions.z > 0.0 )
	{
		bumpUV /= fragment.texcoord1.z;
	}
	float4 bumpMap = t_NormalMap.Sample( s_Sampler, bumpUV );
	float3 localNormal;
#if USE_NORMAL_FMT_RGB8
	localNormal.xy = bumpMap.rg - 0.5;
#else
	localNormal.xy = bumpMap.wy - 0.5;
#endif
	localNormal.z = sqrt( abs( dot( localNormal.xy, localNormal.xy ) - 0.25 ) );
	localNormal = normalize( localNormal );

	float3 worldNormal;
	worldNormal.x = dot3( localNormal, fragment.texcoord2 );
	worldNormal.y = dot3( localNormal, fragment.texcoord3 );
	worldNormal.z = dot3( localNormal, fragment.texcoord4 );

	// GEOMETRIC dominant axis for the surf-cache texel key (result.normal.w). The term used to derive the
	// key axis from the normal-MAPPED shading normal above, but the surf-cache SEED keys texels off the flat
	// GEOMETRIC triangle normal - so on any normal-mapped surface whose bump flips the dominant axis the two
	// disagreed and every warm read missed (measured: hit ~4% on the cinematic). The flat geometric normal
	// is the screen-space derivative of world position (per-triangle, sign-agnostic dominant axis == the
	// seed's cross(edge,edge) axis). Same tie-break as softsurf_seed / softterm. Depth-edge pixels get a
	// garbage derivative and simply miss (thin, harmless).
	const float3 gn = cross( ddx( worldPosition ), ddy( worldPosition ) );
	const float3 ga = abs( gn );
	const float  gaxis = ( ga.x >= ga.y && ga.x >= ga.z ) ? 0.0 : ( ( ga.y >= ga.z ) ? 1.0 : 2.0 );
	result.normal = float4( worldNormal, gaxis );
}
