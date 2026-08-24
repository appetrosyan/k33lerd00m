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

// Analytic soft shadows, tile binning: SHARED per-tile depth min/max reduce, run ONCE per view over
// the whole screen. Every soft light's bin pass (softtile_bin.cs.hlsl) reads this instead of
// re-reducing the same 256 texels per (tile x light) - on a heavy frame that was ~14M redundant
// depth loads. Output: [tileIdx*2] = min depth bits, [tileIdx*2+1] = max depth bits (asuint order
// preserving for depths in [0,1)); min > max encodes "no valid depth in tile" (sky).

#define SW_TILE_SIZE 16

// *INDENT-OFF*
Texture2D<float>			t_Depth		: register( t0 );
RWStructuredBuffer<uint>	u_MinMax	: register( u0 );

cbuffer c_MinMax : register( b0 )
{
	int4	g_dims;		// screen tiles X, screen W, screen H, unused
};
// *INDENT-ON*

groupshared uint gsMin;
groupshared uint gsMax;

[numthreads( 64, 1, 1 )]
void main( uint3 groupId : SV_GroupID, uint tid : SV_GroupThreadID )
{
	if( tid == 0 )
	{
		gsMin = 0xFFFFFFFFu;
		gsMax = 0u;
	}
	GroupMemoryBarrierWithGroupSync();

	for( int s = 0; s < ( SW_TILE_SIZE * SW_TILE_SIZE + 63 ) / 64; s++ )
	{
		int t = ( int )tid + s * 64;
		if( t >= SW_TILE_SIZE * SW_TILE_SIZE ) { break; }
		int px = ( int )groupId.x * SW_TILE_SIZE + ( t % SW_TILE_SIZE );
		int py = ( int )groupId.y * SW_TILE_SIZE + ( t / SW_TILE_SIZE );
		if( px < g_dims.y && py < g_dims.z )
		{
			float z = t_Depth.Load( int3( px, py, 0 ) );
			if( z > 0.0f && z < 1.0f )
			{
				InterlockedMin( gsMin, asuint( z ) );
				InterlockedMax( gsMax, asuint( z ) );
			}
		}
	}
	GroupMemoryBarrierWithGroupSync();

	if( tid == 0 )
	{
		int tileIdx = ( int )groupId.y * g_dims.x + ( int )groupId.x;
		u_MinMax[tileIdx * 2 + 0] = gsMin;
		u_MinMax[tileIdx * 2 + 1] = gsMax;
	}
}
