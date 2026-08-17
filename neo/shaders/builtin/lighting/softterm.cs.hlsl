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

// Analytic soft shadows: PER-LIGHT COMPUTE evaluation of the coverage integral
// (r_softShadowCompute). One dispatch per soft light over the light's scissor rect; each thread
// evaluates the SAME face-coverage integral the interaction pixel shader runs (the include below is
// the shared, unit-tested source), against the EXACT rasterised receiver world position from the
// softShadowPos G-buffer (written by softpos.{vs,ps}.hlsl at depth-EQUAL - NO depth
// reconstruction, so no grazing instability), and writes the visibility term into this light's
// slot of the R32F term atlas. The interaction pixel shader then just Loads its term texel
// (rpUser6) instead of running the integral in the wave64 fragment shader.
//
// Designed properties:
//   - BIT-EXACT vs the fragment path: same include, same inputs (the position G-buffer stores the
//     same interpolated model position -> world transform at float32), R32F storage (no
//     quantisation) => the anaTerm hashes must MATCH the FS path.
//   - Rate-independent: the term is computed at full pixel rate regardless of raster VRS on the
//     interaction pass, and the cost is decoupled from raster/overdraw.
//   - Wave32-friendly: 32-thread workgroups (8x4) so RDNA3 can run wave32 (VOPD dual-issue),
//     which the wave64 pixel shader forecloses. (No explicit subgroup-size control: this nvrhi
//     has no VK_EXT_subgroup_size_control plumbing; the workgroup size only encourages it.)

#define SW_TILE_SIZE	16
#define SW_TILE_K		256		// must match softtile_bin.cs.hlsl + SoftTileBinPass.h

// *INDENT-OFF*
// Declared BEFORE the include: the coverage functions read these globals directly (HLSL).
StructuredBuffer<float4>	t_SoftEdges	: register( t0 );	// softShadowEdge_t stream (float4 pairs), whole joint buffer
StructuredBuffer<uint>		t_SoftTiles	: register( t1 );	// per-tile triangle lists (softtile_bin.cs.hlsl)
#include "softwedge_coverage.inc.hlsl"

Texture2D<float4>			t_WorldPos	: register( t2 );	// exact receiver world position (softShadowPosImage)
RWTexture2D<float>			u_Term		: register( u0 );	// R32F term atlas, one screen-size slot per light

cbuffer c_Term : register( b0 )
{
	float4	g_lightR;	// light origin xyz, disk radius w
	int4	g_range;	// firstElem (edge float4 base), faceCount (records), tileBase | -1, tilesX
	int4	g_tile;		// tile origin x, y (in tiles), atlas slot offset x, y (in pixels)
	int4	g_rect;		// scissor origin x, y (absolute pixels), width, height
};
// *INDENT-ON*

[numthreads( 8, 4, 1 )]
void main( uint3 tid : SV_DispatchThreadID )
{
	if( ( int )tid.x >= g_rect.z || ( int )tid.y >= g_rect.w )
	{
		return;
	}
	const int2 px = int2( g_rect.x + ( int )tid.x, g_rect.y + ( int )tid.y );

	// EXACT receiver position (same value the interaction PS computes from texcoord7 x model
	// matrix; softpos.ps stored it at float32). Pixels never rasterised by the position pass
	// (sky, translucent-only) hold the clear value; the interaction shader never reads the term
	// there (translucent draws keep the in-shader integral, sky draws no soft interaction).
	const float3 swP = t_WorldPos.Load( int3( px, 0 ) ).xyz;

	const float3 swL = g_lightR.xyz;
	const float  swR = max( g_lightR.w, 1e-2 );

	// per-fragment sample-set rotation: the SAME world-position hash the interaction PS computes
	// (interactionSM.ps.hlsl) - swP is bit-identical by the position G-buffer design, so the angle
	// (and therefore the whole integral) stays bit-exact vs the fragment path.
	const float swRotHash = dot( swP, float3( 12.9898, 78.233, 37.719 ) );
	const float swRotAng = ( swRotHash - floor( swRotHash ) ) * 6.28318531;

	// Mirror of the interaction PS swFace branch (tile-binned walk with bit-exact full-walk
	// fallback). swCentreLit is 0: the face path ignores it (see SoftShadow_Coverage).
	float swOcc;
	const int swTx = px.x / SW_TILE_SIZE - g_tile.x;
	const int swTy = px.y / SW_TILE_SIZE - g_tile.y;
	if( g_range.z >= 0 && swTx >= 0 && swTy >= 0 )
	{
		const int  swSlot = g_range.z + ( swTy * g_range.w + swTx ) * ( SW_TILE_K + 1 );
		const uint swCnt  = t_SoftTiles[ swSlot ];
		if( swCnt != 0xFFFFFFFFu )
		{
			swOcc = SoftShadow_FaceCoverageList( swP, swL, swR, g_range.x, swSlot + 1, ( int )swCnt, swRotAng );
		}
		else
		{
			swOcc = SoftShadow_Coverage( swP, swL, swR, g_range.x, g_range.y, 0.0, true, swRotAng );
		}
	}
	else
	{
		swOcc = SoftShadow_Coverage( swP, swL, swR, g_range.x, g_range.y, 0.0, true, swRotAng );
	}

	u_Term[ uint2( px + g_tile.zw ) ] = 1.0 - saturate( swOcc );
}
