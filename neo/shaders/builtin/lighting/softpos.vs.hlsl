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

// Analytic soft shadows, EXACT-POSITION G-buffer (r_softShadowCompute): re-rasterise the depth-
// prepassed opaque surfaces at depth-EQUAL and hand the PS the interpolated MODEL position, exactly
// like interactionSM.vs hands texcoord7 to the interaction PS. The clip-position math (including
// the skinning preamble and psxVertexJitter) is copied VERBATIM from interactionSM.vs so the
// rasteriser produces the same fragments with the same interpolation - that is what makes the
// compute-evaluated coverage term bit-exact against the fragment-shader integral.

#include "global_inc.hlsl"
#include "renderParmSet3.inc.hlsl"

// *INDENT-OFF*
#if USE_GPU_SKINNING
StructuredBuffer<float4> matrices: register(t11);
#endif

struct VS_IN
{
	float4 position	: POSITION;
	float2 texcoord	: TEXCOORD0;
	float4 normal	: NORMAL;
	float4 tangent	: TANGENT;
	float4 color	: COLOR0;
	float4 color2	: COLOR1;
};

struct VS_OUT
{
	float4 position		: SV_Position;
	float4 texcoord0	: TEXCOORD0_centroid;	// interpolated model position (interactionSM texcoord7)
};
// *INDENT-ON*

void main( VS_IN vertex, out VS_OUT result )
{
#if USE_GPU_SKINNING
	// identical to interactionSM.vs: same joint fetch, same weights, same dot order
	const float w0 = vertex.color2.x;
	const float w1 = vertex.color2.y;
	const float w2 = vertex.color2.z;
	const float w3 = vertex.color2.w;

	float4 matX, matY, matZ;	// must be float4 for vec4
	int joint = int( vertex.color.x * 255.1 * 3.0 );
	matX = matrices[int( joint + 0 )] * w0;
	matY = matrices[int( joint + 1 )] * w0;
	matZ = matrices[int( joint + 2 )] * w0;

	joint = int( vertex.color.y * 255.1 * 3.0 );
	matX += matrices[int( joint + 0 )] * w1;
	matY += matrices[int( joint + 1 )] * w1;
	matZ += matrices[int( joint + 2 )] * w1;

	joint = int( vertex.color.z * 255.1 * 3.0 );
	matX += matrices[int( joint + 0 )] * w2;
	matY += matrices[int( joint + 1 )] * w2;
	matZ += matrices[int( joint + 2 )] * w2;

	joint = int( vertex.color.w * 255.1 * 3.0 );
	matX += matrices[int( joint + 0 )] * w3;
	matY += matrices[int( joint + 1 )] * w3;
	matZ += matrices[int( joint + 2 )] * w3;

	float4 modelPosition;
	modelPosition.x = dot4( matX, vertex.position );
	modelPosition.y = dot4( matY, vertex.position );
	modelPosition.z = dot4( matZ, vertex.position );
	modelPosition.w = 1.0;
#else
	float4 modelPosition = vertex.position;
#endif

	result.position.x = dot4( modelPosition, pc.rpMVPmatrixX );
	result.position.y = dot4( modelPosition, pc.rpMVPmatrixY );
	result.position.z = dot4( modelPosition, pc.rpMVPmatrixZ );
	result.position.w = dot4( modelPosition, pc.rpMVPmatrixW );

	result.position.xyz = psxVertexJitter( pc.rpPSXDistortions, pc.rpProjectionMatrixW, result.position );

	result.texcoord0 = modelPosition;
}
