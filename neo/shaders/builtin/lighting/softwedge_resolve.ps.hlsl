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

// Analytic soft shadow volumes: resolve pass. Reads the signed coverage accumulator (baseline hard
// occlusion + summed wedge deltas) and writes the light visibility the interaction multiplies in:
// visibility = 1 - saturate(occlusion). No umbra is stamped anywhere - it emerges wherever the
// accumulated occlusion saturates to 1.

#include "global_inc.hlsl"
#include "renderParmSet13.inc.hlsl"	// match the DRAW_AO1 binding layout (constant buffer at b0)

// *INDENT-OFF*
Texture2D		t_Accum			: register( t0 VK_DESCRIPTOR_SET( 0 ) );
SamplerState	s_PointClamp	: register( s0 VK_DESCRIPTOR_SET( 1 ) );	// declared to match the layout (Load used, sampler unreferenced)

struct PS_IN
{
	float4 position : SV_Position;
};

struct PS_OUT
{
	float4 color : SV_Target0;
};
// *INDENT-ON*

void main( PS_IN fragment, out PS_OUT result )
{
	float occlusion = t_Accum.Load( int3( int2( fragment.position.xy ), 0 ) ).r;
	result.color = float4( saturate( 1.0 - occlusion ), 0.0, 0.0, 0.0 );
}
