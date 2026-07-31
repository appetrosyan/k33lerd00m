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

#include "renderParmSet1.inc.hlsl"


// *INDENT-OFF*
struct VS_IN
{
	float4 position	: POSITION;
	float2 texcoord	: TEXCOORD0;
	float4 normal	: NORMAL;
	float4 tangent	: TANGENT;
	float4 color	: COLOR0;
	float4 color2	: COLOR1;
};

struct VS_OUT {
	float4 position : SV_Position;
	float2 texcoord0 : TEXCOORD0_centroid;
};
// *INDENT-ON*

void main( VS_IN vertex, out VS_OUT result )
{
	result.position = vertex.position;
	result.position.y = -result.position.y;
	result.texcoord0 = vertex.texcoord;
}
