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

// Ember sprite shading: a soft, round, additive emissive dot. Procedural (no
// texture) so there is no asset to miss, and bright enough to punch past
// paper-white on the HDR output.
//
// Self-contained (no global_inc.hlsl).

#pragma pack_matrix( row_major )

struct PSIn
{
	float4	position	: SV_Position;
	float2	uv			: TEXCOORD0;
	float3	color		: TEXCOORD1;
	float	fade		: TEXCOORD2;
};

float4 main( PSIn i ) : SV_Target0
{
	const float r2 = dot( i.uv, i.uv );
	if( r2 > 1.0f )
	{
		discard;
	}

	// soft falloff with a small hot core; additive blend so alpha is unused
	const float soft = pow( saturate( 1.0f - r2 ), 3.0f );
	const float core = pow( saturate( 1.0f - r2 ), 10.0f );
	const float3 rgb = i.color * ( soft * 0.6f + core ) * i.fade;

	return float4( rgb, 1.0f );
}
