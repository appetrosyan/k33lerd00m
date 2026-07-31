/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").

Doom 3 BFG Edition Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 BFG Edition Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 BFG Edition Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 BFG Edition Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 BFG Edition Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/

#include "global_inc.hlsl"
#include "renderParmSet4.inc.hlsl"


// *INDENT-OFF*
Texture2D	 t_BaseColor	: register( t0 VK_DESCRIPTOR_SET( 1 ) );
SamplerState s_Sampler		: register( s0 VK_DESCRIPTOR_SET( 2 ) );

struct PS_IN {
	float4 position		: SV_POSITION;
	float3 texcoord0	: TEXCOORD0_centroid;
	float4 color		: COLOR0;
};

struct PS_OUT {
	float4 color : SV_Target0;
};
// *INDENT-ON*

void main( in PS_IN fragment, out PS_OUT result )
{
	float2 uv = fragment.texcoord0.xy;

	// PSX affine texture mapping
	if( pc.rpPSXDistortions.z > 0.0 )
	{
		uv /= fragment.texcoord0.z;
	}

	float4 color = t_BaseColor.Sample( s_Sampler, uv ) * fragment.color;
	clip( color.a - pc.rpAlphaTest.x );

	// SRS - sRGBAToLinearRGBA clamps to [0,1], which would flatten emissive/additive
	// stages that intentionally exceed white (r_emissiveScale). Linearize the in-gamut
	// part exactly as before and add any over-bright back as a linear tail, so the [0,1]
	// range is bit-identical to the old path while HDR glow above white is preserved.
	result.color = sRGBAToLinearRGBA( min( color, 1.0 ) ) + float4( max( color.rgb - 1.0, 0.0 ), 0.0 );
}
