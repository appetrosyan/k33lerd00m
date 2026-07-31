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

// Marks a block of ember slots inactive (age < 0) before an impact-append seed,
// so slots the append does not fill are skipped by the sim and the sprite VS.
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
RWStructuredBuffer<Ember>	u_Embers	: register( u0 );

cbuffer c_Clear : register( b0 )
{
	uint	g_slotBase;
	uint	g_count;
	uint	g_pad0;
	uint	g_pad1;
};
// *INDENT-ON*

[numthreads( 64, 1, 1 )]
void main( uint3 dispatchID : SV_DispatchThreadID )
{
	if( dispatchID.x >= g_count )
	{
		return;
	}
	u_Embers[g_slotBase + dispatchID.x].age = -1.0f;
}
