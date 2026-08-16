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

// Analytic soft shadows: PER-TILE TRIANGLE BINNING (r_softShadowTileBin). One threadgroup per 16x16
// screen tile of one soft light's scissor. The group reduces the tile's depth min/max, unprojects the
// tile's 8 corner points into a world-space receiver AABB, and then tests every caster TRIANGLE of the
// light's face stream against the sample cone of that AABB - the SAME cone/slab cull the interaction
// fragment shader runs per fragment, made conservative for the whole tile by inflating with the AABB
// radius (translating the cone apex by d changes any point-cone distance by at most d, and the
// receiver->light distance by at most d, so radius + 2d covers every receiver in the tile). Surviving
// pair-start record indices are appended to this tile's list; the fragment shader then walks ONLY that
// list (SoftShadow_FaceCoverageList). Measured motivation: the per-fragment record walk + triangle
// culls are 69% of the soft-shadow cost and are near-identical across a tile's 256 fragments.
//
// CONSERVATIVE by construction - a tile list can only OVER-include:
//   - depth-degenerate tiles (huge z range) inflate the AABB, keeping more triangles;
//   - a tile with more than SW_TILE_K survivors writes the overflow sentinel and the fragment shader
//     falls back to the full walk;
//   - sky/invalid-depth texels are excluded from the reduce; an all-sky tile bins nothing (no receiver
//     fragments will read it).
// The triangle list layout per tile: [count | idx0 .. idx(K-1)], count == 0xFFFFFFFF => overflow.

#pragma pack_matrix( row_major )

#define SW_TILE_SIZE	16
#define SW_TILE_K		256		// indices per tile; must match interactionSM.ps.hlsl + SoftTileBinPass.h

// *INDENT-OFF*
StructuredBuffer<float4>	t_Edges		: register( t0 );	// softShadowEdge_t stream (float4 pairs), whole joint buffer
StructuredBuffer<uint>		t_PairIdx	: register( t1 );	// pair-start record indices, one per triangle
StructuredBuffer<uint>		t_MinMax	: register( t2 );	// per-SCREEN-tile depth min/max bits (softtile_minmax.cs.hlsl)
RWStructuredBuffer<uint>	u_Tiles		: register( u0 );	// [count | K indices] per tile

cbuffer c_TileBin : register( b0 )
{
	float4	g_invMvp0;		// world MVP inverse, rows (clip -> world, homogeneous)
	float4	g_invMvp1;
	float4	g_invMvp2;
	float4	g_invMvp3;
	float4	g_lightR;		// light origin xyz, disk radius w
	int4	g_tileRect;		// tile origin x, y (in tiles), tilesX, tilesY
	int4	g_range;		// firstElem (edge float4 base), numPairs, outBase (uint elements), pairBase (uint elements)
	float4	g_screen;		// viewport W, H, viewport origin x, y
	int4	g_minmax;		// screen tiles X (t_MinMax row stride), unused x3
};

#define SW_NEAR_EPS 1e-3f
// *INDENT-ON*

groupshared uint gsCount;
groupshared float3 gsCentre;
groupshared float  gsRad;
groupshared float3 gsNrm;
groupshared float  gsDistPL;

float3 TileUnproject( float px, float py, float depth )
{
	float uvx = ( px - g_screen.z ) / g_screen.x;
	float uvy = ( py - g_screen.w ) / g_screen.y;
	float4 clip = float4( uvx * 2.0f - 1.0f, 1.0f - uvy * 2.0f, depth, 1.0f );
	float wx = dot( g_invMvp0, clip );
	float wy = dot( g_invMvp1, clip );
	float wz = dot( g_invMvp2, clip );
	float ww = dot( g_invMvp3, clip );
	float inv = ( abs( ww ) > 1e-12f ) ? 1.0f / ww : 0.0f;
	return float3( wx * inv, wy * inv, wz * inv );
}

[numthreads( 64, 1, 1 )]
void main( uint3 groupId : SV_GroupID, uint tid : SV_GroupThreadID )
{
	const int tileX = g_tileRect.x + ( int )groupId.x;
	const int tileY = g_tileRect.y + ( int )groupId.y;
	const int tileIdx = ( int )groupId.y * g_tileRect.z + ( int )groupId.x;
	const int outSlot = g_range.z + tileIdx * ( SW_TILE_K + 1 );

	if( tid == 0 )
	{
		gsCount = 0u;
	}

	// ---- tile depth min/max from the SHARED once-per-view reduce (softtile_minmax.cs.hlsl) ----
	const uint zMinBits = t_MinMax[ ( tileY * g_minmax.x + tileX ) * 2 + 0 ];
	const uint zMaxBits = t_MinMax[ ( tileY * g_minmax.x + tileX ) * 2 + 1 ];
	if( zMinBits > zMaxBits )	// no valid depth in the tile: nothing will shade here
	{
		if( tid == 0 )
		{
			u_Tiles[outSlot] = 0u;
		}
		return;
	}
	GroupMemoryBarrierWithGroupSync();

	// ---- receiver AABB from the tile's 8 unprojected corners ----
	if( tid == 0 )
	{
		float zmn = asfloat( zMinBits );
		float zmx = asfloat( zMaxBits );
		float x0 = ( float )( tileX * SW_TILE_SIZE );
		float y0 = ( float )( tileY * SW_TILE_SIZE );
		float x1 = min( x0 + ( float )SW_TILE_SIZE, g_screen.z + g_screen.x );
		float y1 = min( y0 + ( float )SW_TILE_SIZE, g_screen.w + g_screen.y );
		float3 mn = float3( 1e30f, 1e30f, 1e30f );
		float3 mx = float3( -1e30f, -1e30f, -1e30f );
		for( int c = 0; c < 8; c++ )
		{
			float3 w = TileUnproject( ( c & 1 ) != 0 ? x1 : x0, ( c & 2 ) != 0 ? y1 : y0, ( c & 4 ) != 0 ? zmx : zmn );
			mn = min( mn, w );
			mx = max( mx, w );
		}
		float3 cen = ( mn + mx ) * 0.5f;
		float3 he  = ( mx - mn ) * 0.5f;
		gsCentre = cen;
		gsRad    = sqrt( dot( he, he ) );
		float3 toL = float3( g_lightR.x, g_lightR.y, g_lightR.z ) - cen;
		gsDistPL = max( length( toL ), 1e-4f );
		gsNrm    = toL * ( 1.0f / gsDistPL );
	}
	GroupMemoryBarrierWithGroupSync();

	const float3 Pc     = gsCentre;
	const float  tR     = gsRad;
	const float3 nrm    = gsNrm;
	const float  distPL = gsDistPL;
	const float  swR    = max( g_lightR.w, 1e-2f );
	const float  eps    = SW_NEAR_EPS;

	// ---- bin: same cone/slab cull as the fragment walk, inflated by the tile radius ----
	for( int p = ( int )tid; p < g_range.y; p += 64 )
	{
		int se = ( int )t_PairIdx[ g_range.w + p ];
		float4 e0 = t_Edges[ g_range.x + se * 2 + 0 ];
		float4 e1 = t_Edges[ g_range.x + se * 2 + 1 ];
		float4 g1 = t_Edges[ g_range.x + ( se + 1 ) * 2 + 1 ];
		float3 tcen = ( float3( e0.x, e0.y, e0.z ) + float3( e1.x, e1.y, e1.z ) + float3( g1.x, g1.y, g1.z ) ) * ( 1.0f / 3.0f );
		float  triRad = e0.w + tR;								// apex may sit anywhere in the tile AABB
		float3 rc = tcen - Pc;
		float  cd = dot( rc, nrm );
		if( cd + triRad < eps ) { continue; }					// wholly behind every receiver in the tile
		if( cd - triRad > distPL + tR ) { continue; }			// wholly beyond the light for every receiver
		float3 perp = rc - cd * nrm;
		float  coneR = swR * ( cd + triRad ) / max( distPL - tR, 1e-4f );
		if( sqrt( dot( perp, perp ) ) - triRad > coneR ) { continue; }
		uint slot;
		InterlockedAdd( gsCount, 1u, slot );
		if( slot < SW_TILE_K )
		{
			u_Tiles[ outSlot + 1 + ( int )slot ] = ( uint )se;
		}
	}
	GroupMemoryBarrierWithGroupSync();

	if( tid == 0 )
	{
		u_Tiles[outSlot] = ( gsCount > SW_TILE_K ) ? 0xFFFFFFFFu : gsCount;
	}
}
