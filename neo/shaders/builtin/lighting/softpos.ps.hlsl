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
struct PS_IN
{
	float4 position		: SV_Position;
	float4 texcoord0	: TEXCOORD0_centroid;	// interpolated model position
};

struct PS_OUT
{
	float4 color : SV_Target0;
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
}
