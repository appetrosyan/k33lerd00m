/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").

Doom 3 BFG Edition Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

===========================================================================
*/

#include "global_inc.hlsl"
#include "renderParmSet1.inc.hlsl"


// *INDENT-OFF*
Texture2D	 t_BaseColor	: register( t0 VK_DESCRIPTOR_SET( 1 ) );
SamplerState s_Sampler		: register( s0 VK_DESCRIPTOR_SET( 2 ) );

struct PS_IN
{
	float4 position		: SV_Position;
	float2 texcoord0	: TEXCOORD0_centroid;
};

struct PS_OUT
{
	float4 color : SV_Target0;
};
// *INDENT-ON*

// SSAA downsample resolve.
//
// The 3D scene is rendered at nativeRes * scale; this pass reduces it back to native.
// It must NOT be a plain box average: in an HDR pipeline a single very bright sub-pixel
// sample would dominate the average and erase the edge, so each tap is weighted by the
// Karis reversible tonemap 1/(1+luma) (GPUOpen / Karis / MJP). A separable tent over the
// native pixel's source footprint feathers the taps; a fixed 4x4 bilinear-tap grid covers
// scale 1.5 and 2.0 uniformly. Correct for HDR-linear (essential) and benign for SDR-gamma.
void main( PS_IN fragment, out PS_OUT result )
{
	// rpScreenCorrectionFactor = ( 1/srcWidth, 1/srcHeight, scale, 0 )
	const float2 invSrc = pc.rpScreenCorrectionFactor.xy;
	const float scale = pc.rpScreenCorrectionFactor.z;

	// 4 tap positions across the footprint: -0.5..+0.5 of (scale) source texels
	const float pos[4] = { -1.5, -0.5, 0.5, 1.5 };
	const float tw[4]  = {  0.25, 0.75, 0.75, 0.25 }; // linear tent weights

	float3 sum = _float3( 0.0 );
	float wsum = 0.0;

	[unroll]
	for( int y = 0; y < 4; y++ )
	{
		[unroll]
		for( int x = 0; x < 4; x++ )
		{
			float2 off = float2( pos[x], pos[y] ) * ( scale / 3.0 );
			float2 uv = fragment.texcoord0 + off * invSrc;

			float3 c = t_BaseColor.SampleLevel( s_Sampler, uv, 0 ).rgb;
			float luma = dot( c, float3( 0.2126, 0.7152, 0.0722 ) );
			float w = ( tw[x] * tw[y] ) / ( 1.0 + max( luma, 0.0 ) );

			sum += c * w;
			wsum += w;
		}
	}

	result.color = float4( sum / max( wsum, 1e-5 ), 1.0 );
}
