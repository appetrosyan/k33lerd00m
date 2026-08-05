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

// Analytic soft shadow volumes: penumbra wedge vertex shader. Positions are pre-transformed to
// clip space on the CPU (the wedge param set has no MVP), so the VS just passes them through and
// forwards the silhouette edge endpoints (world space) flat to the pixel shader.

#include "global_inc.hlsl"
#include "renderParmSet13.inc.hlsl"	// match the DRAW_AO1 binding layout (constant buffer at b0)

// *INDENT-OFF*
struct VS_IN
{
	float4 position	: POSITION;		// pre-transformed clip space
	float4 edge0	: TEXCOORD0;		// xyz = silhouette edge endpoint 0 (world), w = silhouette weight
	float4 edge1	: TEXCOORD1;		// xyz = silhouette edge endpoint 1 (world)
};

struct VS_OUT
{
	float4 position						: SV_Position;
	nointerpolation float3 edge0		: TEXCOORD0;
	nointerpolation float3 edge1		: TEXCOORD1;
	nointerpolation float  silWeight	: TEXCOORD2;	// [0,1] fade to stop wedges popping on/off
};
// *INDENT-ON*

void main( VS_IN vertex, out VS_OUT result )
{
	result.position = vertex.position;
	result.edge0 = vertex.edge0.xyz;
	result.edge1 = vertex.edge1.xyz;
	result.silWeight = vertex.edge0.w;
}
