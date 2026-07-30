/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 2026 Robert Beckebans (RBDOOM-3-BFG contributors)

This program is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version. See <http://www.gnu.org/licenses/>.

===========================================================================
*/

// DDGI octahedral border copy (M4 stage 3). Runs after probe_integrate. Each
// probe tile carries a one-texel border so bilinear sampling across the octahedral
// seam is continuous; this pass fills that border from the interior texels using
// the standard octahedral wrap (edges mirror the perpendicular axis, corners map
// to the diagonally-opposite interior corner).
//
// Self-contained (no global_inc.hlsl) - compiled by the ShadersRT target.

#pragma pack_matrix(row_major)

struct DdgiConstants
{
	float4	probeGridOrigin;
	float4	probeGridSpacing;
	int2	probeGridCountsXY;
	int2	probeGridCountZ_total;

	int2	irradianceProbe;
	int2	distanceProbe;

	int		raysPerProbe;
	float	hysteresis;
	float	normalBias;
	float	viewBias;

	float4	rayRotation;

	int		frameIndex;
	int		numLights;
	float	bounceGain;
	int		pad2;
};

// *INDENT-OFF*
RWTexture2D<float4>			u_IrradianceAtlas	: register(u0);
RWTexture2D<float2>			u_DistanceAtlas		: register(u1);

cbuffer c_Ddgi : register(b1)
{
	DdgiConstants g_Ddgi;
};
// *INDENT-ON*

[numthreads( 8, 8, 1 )]
void main( uint3 dispatchID : SV_DispatchThreadID )
{
	const int probeIndex = int( dispatchID.z );
	if( probeIndex >= g_Ddgi.probeGridCountZ_total.y )
	{
		return;
	}

	const int probeSize = g_Ddgi.irradianceProbe.x;
	const int last = probeSize - 1;
	const int x = int( dispatchID.x );
	const int y = int( dispatchID.y );
	if( x >= probeSize || y >= probeSize )
	{
		return;
	}

	const bool isBorder = ( x == 0 || y == 0 || x == last || y == last );
	if( !isBorder )
	{
		return;
	}

	// octahedral wrap: find the interior source texel for this border texel
	int2 src;
	if( ( x == 0 || x == last ) && ( y == 0 || y == last ) )
	{
		// corner -> diagonally opposite interior corner
		src.x = ( x == 0 ) ? ( probeSize - 2 ) : 1;
		src.y = ( y == 0 ) ? ( probeSize - 2 ) : 1;
	}
	else if( x == 0 || x == last )
	{
		// left/right edge -> adjacent interior column, y mirrored
		src.x = ( x == 0 ) ? 1 : ( probeSize - 2 );
		src.y = last - y;
	}
	else
	{
		// top/bottom edge -> adjacent interior row, x mirrored
		src.y = ( y == 0 ) ? 1 : ( probeSize - 2 );
		src.x = last - x;
	}

	// atlas tile for this probe (matches probe_integrate)
	const int cx = g_Ddgi.probeGridCountsXY.x;
	const int cy = g_Ddgi.probeGridCountsXY.y;
	const int px = probeIndex % cx;
	const int py = ( probeIndex / cx ) % cy;
	const int pz = probeIndex / ( cx * cy );
	const int2 base = int2( ( px + pz * cx ) * probeSize, py * probeSize );

	u_IrradianceAtlas[base + int2( x, y )] = u_IrradianceAtlas[base + src];
	u_DistanceAtlas[base + int2( x, y )] = u_DistanceAtlas[base + src];
}
