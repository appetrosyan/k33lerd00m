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

// Ember sprite expansion. No vertex buffer: SV_VertexID drives 6 verts per ember
// (two triangles), read from the simulation buffer and billboarded to face the
// camera. Inactive/undetached embers collapse to a degenerate off-screen triangle.
//
// Self-contained (no global_inc.hlsl).

#pragma pack_matrix( row_major )

struct Ember
{
	float3	pos;		float	age;
	float3	vel;		float	emitDelay;
	float3	dir;		float	seed;
};

// *INDENT-OFF*
StructuredBuffer<Ember>		t_Embers	: register( t0 );

cbuffer c_Render : register( b0 )
{
	float4	g_vp0;			// world->clip (id row-major idRenderMatrix rows)
	float4	g_vp1;
	float4	g_vp2;
	float4	g_vp3;
	float3	g_right;		float	g_spriteSize;	// camera right (world) + base sprite radius
	float3	g_up;			float	g_lifetime;
	float3	g_colorWarm;	float	g_intensity;
	float3	g_colorCool;	float	g_growth;		// sprite growth over life
	uint	g_slotBase;		float3	g_pad;			// first ember slot of this block
};
// *INDENT-ON*

struct VSOut
{
	float4	position	: SV_Position;
	float2	uv			: TEXCOORD0;
	float3	color		: TEXCOORD1;
	float	fade		: TEXCOORD2;
};

static const float2 CORNER[6] =
{
	float2( -1, -1 ), float2( 1, -1 ), float2( 1, 1 ),
	float2( -1, -1 ), float2( 1, 1 ), float2( -1, 1 ),
};

VSOut main( uint vid : SV_VertexID )
{
	VSOut o;

	const uint idx = g_slotBase + vid / 6u;
	const uint corner = vid % 6u;

	Ember e = t_Embers[idx];
	const float localT = e.age - e.emitDelay;

	if( e.age < 0.0f || localT < 0.0f )
	{
		// degenerate: push outside clip space so nothing rasterises
		o.position = float4( 2, 2, 2, 1 );
		o.uv = float2( 0, 0 );
		o.color = float3( 0, 0, 0 );
		o.fade = 0.0f;
		return o;
	}

	const float lifeFrac = saturate( localT / max( 0.001f, g_lifetime ) );
	const float2 c = CORNER[corner];
	const float size = g_spriteSize * ( 1.0f + g_growth * lifeFrac );

	const float3 wp = e.pos + g_right * ( c.x * size ) + g_up * ( c.y * size );
	const float4 wp4 = float4( wp, 1.0f );

	float4 clip;
	clip.x = dot( g_vp0, wp4 );
	clip.y = dot( g_vp1, wp4 );
	clip.z = dot( g_vp2, wp4 );
	clip.w = dot( g_vp3, wp4 );

	o.position = clip;
	o.uv = c;
	// hot young embers cool as they age; a brief flash at birth
	o.color = lerp( g_colorWarm, g_colorCool, lifeFrac ) * g_intensity;
	o.fade = ( 1.0f - lifeFrac ) * saturate( localT * 6.0f );
	return o;
}
