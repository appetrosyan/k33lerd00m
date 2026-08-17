/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 2026 Aleksandr Petrosyan

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").

Doom 3 BFG Edition Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

===========================================================================
*/

#include "precompiled.h"
#pragma hdrstop

#include "renderer/RenderCommon.h"
#include "SoftTileBinPass.h"

// mirrors c_TileBin in softtile_bin.cs.hlsl
struct SoftTileBinCB
{
	float	invMvp[16];
	float	lightR[4];
	int		tileRect[4];
	int		range[4];
	float	screen[4];
	int		minmax[4];
};

// mirrors c_MinMax in softtile_minmax.cs.hlsl
struct SoftTileMinMaxCB
{
	int		dims[4];
};

// per-screen-tile min/max pairs: 4K-class screens are 256x160 tiles
static const int SW_MINMAX_MAX_TILES = 256 * 160;

// 16M uints = 64 MB: ~65k tile slots at K=256. Measured on erebus1_09: a heavy frame's soft
// lights sum to ~54k tile slots, so this holds every light with headroom; lights past the cap
// fall back to the full per-fragment walk for one frame.
static const int SW_TILE_BUFFER_ELEMENTS = 48 << 20;

SoftTileBinPass::SoftTileBinPass( nvrhi::IDevice* device )
	: m_Device( device )
{
}

void SoftTileBinPass::BeginView( nvrhi::ICommandList* commandList, const viewDef_t* viewDef, nvrhi::ITexture* depthTexture )
{
	m_Cursor = 0;
	m_MinMaxValid = false;
	EnsurePipeline();
	if( m_MinMaxPipeline == nullptr || depthTexture == nullptr )
	{
		return;
	}

	// screen-tile grid covers the whole render target (absolute tile coords, like SV_Position >> 4)
	const int screenW = viewDef->viewport.x2 + 1;
	const int screenH = viewDef->viewport.y2 + 1;
	const int tilesX = ( screenW + TILE_SIZE - 1 ) / TILE_SIZE;
	const int tilesY = ( screenH + TILE_SIZE - 1 ) / TILE_SIZE;
	if( tilesX * tilesY > SW_MINMAX_MAX_TILES )
	{
		return;
	}

	SoftTileMinMaxCB cb;
	cb.dims[0] = tilesX;
	cb.dims[1] = screenW;
	cb.dims[2] = screenH;
	cb.dims[3] = 0;

	nvrhi::BindingSetDesc sd;
	sd.bindings =
	{
		nvrhi::BindingSetItem::ConstantBuffer( 0, m_MinMaxCB ),
		nvrhi::BindingSetItem::Texture_SRV( 0, depthTexture ),
		nvrhi::BindingSetItem::StructuredBuffer_UAV( 0, m_MinMaxBuffer ),
	};
	nvrhi::BindingSetHandle set = m_Device->createBindingSet( sd, m_MinMaxLayout );

	// EXPLICIT transition of the depth target out of its attachment state before the compute read:
	// nvrhi's automatic barriers do not synchronise a render-target write with a later dispatch's SRV
	// read (same hazard class the wedge-accum pass documents in RenderBackend.cpp) - without this the
	// reduce can consume the PREVIOUS frame's depth, so every bin list is built for the previous
	// camera and the shadows visibly flash one frame behind while rotating (play-test 2026-08-17).
	commandList->setTextureState( depthTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource );
	commandList->commitBarriers();

	commandList->writeBuffer( m_MinMaxCB, &cb, sizeof( cb ) );
	nvrhi::ComputeState cs;
	cs.pipeline = m_MinMaxPipeline;
	cs.bindings = { set };
	commandList->setComputeState( cs );
	commandList->dispatch( tilesX, tilesY, 1 );

	m_MinMaxTilesX = tilesX;
	m_MinMaxValid = true;
}

void SoftTileBinPass::EnsurePipeline()
{
	if( m_PipelineTried )
	{
		return;
	}
	m_PipelineTried = true;

	idList<shaderMacro_t> macros;
	m_Shader = renderProgManager.GetShader( renderProgManager.FindShader( "builtin/lighting/softtile_bin", SHADER_STAGE_COMPUTE, "", macros, true, LAYOUT_DRAW_VERT ) );
	if( m_Shader == nullptr )
	{
		common->Warning( "SoftTileBinPass: compute shader failed to load - tile binning disabled." );
		return;
	}

	nvrhi::BindingLayoutDesc ld;
	ld.visibility = nvrhi::ShaderType::Compute;
	ld.bindings =
	{
		nvrhi::BindingLayoutItem::VolatileConstantBuffer( 0 ),	// b0 : bin constants
		nvrhi::BindingLayoutItem::StructuredBuffer_SRV( 0 ),	// t0 : tri stream + caster table (joint buffer, float4)
		nvrhi::BindingLayoutItem::StructuredBuffer_SRV( 2 ),	// t2 : shared per-tile depth min/max
		nvrhi::BindingLayoutItem::StructuredBuffer_UAV( 0 ),	// u0 : tile lists
	};
	m_Layout = m_Device->createBindingLayout( ld );

	nvrhi::ComputePipelineDesc pd;
	pd.bindingLayouts = { m_Layout };
	pd.CS = m_Shader;
	m_Pipeline = m_Device->createComputePipeline( pd );

	nvrhi::BufferDesc cb;
	cb.byteSize = sizeof( SoftTileBinCB );
	cb.isConstantBuffer = true;
	cb.isVolatile = true;
	cb.maxVersions = 1024;					// dispatches per frame x frames in flight
	cb.debugName = "SoftTileBin/CB";
	m_ConstantBuffer = m_Device->createBuffer( cb );

	nvrhi::BufferDesc td;
	td.byteSize = ( uint64_t )SW_TILE_BUFFER_ELEMENTS * sizeof( uint32_t );
	td.structStride = sizeof( uint32_t );
	td.canHaveUAVs = true;
	td.initialState = nvrhi::ResourceStates::UnorderedAccess;
	td.keepInitialState = true;
	td.debugName = "SoftTileBin/Tiles";
	m_TileBuffer = m_Device->createBuffer( td );

	// --- shared depth min/max reduce (once per view; every light's bin pass reads it) ---
	m_MinMaxShader = renderProgManager.GetShader( renderProgManager.FindShader( "builtin/lighting/softtile_minmax", SHADER_STAGE_COMPUTE, "", macros, true, LAYOUT_DRAW_VERT ) );
	if( m_MinMaxShader == nullptr )
	{
		common->Warning( "SoftTileBinPass: minmax shader failed to load - tile binning disabled." );
		m_Pipeline = nullptr;
		return;
	}

	nvrhi::BindingLayoutDesc mld;
	mld.visibility = nvrhi::ShaderType::Compute;
	mld.bindings =
	{
		nvrhi::BindingLayoutItem::VolatileConstantBuffer( 0 ),	// b0 : dims
		nvrhi::BindingLayoutItem::Texture_SRV( 0 ),				// t0 : depth
		nvrhi::BindingLayoutItem::StructuredBuffer_UAV( 0 ),	// u0 : min/max pairs
	};
	m_MinMaxLayout = m_Device->createBindingLayout( mld );

	nvrhi::ComputePipelineDesc mpd;
	mpd.bindingLayouts = { m_MinMaxLayout };
	mpd.CS = m_MinMaxShader;
	m_MinMaxPipeline = m_Device->createComputePipeline( mpd );

	nvrhi::BufferDesc mcb;
	mcb.byteSize = sizeof( SoftTileMinMaxCB );
	mcb.isConstantBuffer = true;
	mcb.isVolatile = true;
	mcb.maxVersions = 256;
	mcb.debugName = "SoftTileBin/MinMaxCB";
	m_MinMaxCB = m_Device->createBuffer( mcb );

	nvrhi::BufferDesc mmd;
	mmd.byteSize = ( uint64_t )SW_MINMAX_MAX_TILES * 2 * sizeof( uint32_t );
	mmd.structStride = sizeof( uint32_t );
	mmd.canHaveUAVs = true;
	mmd.initialState = nvrhi::ResourceStates::UnorderedAccess;
	mmd.keepInitialState = true;
	mmd.debugName = "SoftTileBin/MinMax";
	m_MinMaxBuffer = m_Device->createBuffer( mmd );
}

int SoftTileBinPass::BinLight( nvrhi::ICommandList* commandList, const viewDef_t* viewDef, const viewLight_t* vLight,
							   nvrhi::IBuffer* edgeBuffer, uint32_t edgeFirstElem,
							   uint32_t casterFirstElem, int numCasters,
							   float penumbraRadius,
							   int& outTileOx, int& outTileOy, int& outTilesX )
{
	if( m_Pipeline == nullptr || numCasters <= 0 || !m_MinMaxValid )
	{
		return -1;
	}

	// tile rect over the light's scissor, in ABSOLUTE screen pixels (SV_Position space).
	// scissorRect is GL-convention (origin bottom-left); SV_Position is top-down, so Y flips
	// against viewport.y2 - the EXACT mapping RenderInteractions feeds GL_Scissor. The old
	// unflipped mapping placed the rect mirrored for every non-fullscreen light: the bins
	// landed off the light's true pixels, and the FS's out-of-rect fallback silently walked
	// the full stream (bit-exact, which is why the gate never saw it - only the bench paid).
	const int px1 = viewDef->viewport.x1 + vLight->scissorRect.x1;
	const int py1 = viewDef->viewport.y2 - vLight->scissorRect.y2;
	const int px2 = viewDef->viewport.x1 + vLight->scissorRect.x2;
	const int py2 = viewDef->viewport.y2 - vLight->scissorRect.y1;
	if( px2 < px1 || py2 < py1 )
	{
		return -1;
	}
	const int tileOx = px1 / TILE_SIZE;
	const int tileOy = py1 / TILE_SIZE;
	const int tilesX = px2 / TILE_SIZE - tileOx + 1;
	const int tilesY = py2 / TILE_SIZE - tileOy + 1;
	const int slots = tilesX * tilesY * ( TILE_K + 1 );
	extern idCVar r_rtAccelDebug;
	if( m_Cursor + slots > SW_TILE_BUFFER_ELEMENTS )
	{
		if( r_rtAccelDebug.GetBool() )
		{
			common->Printf( "SoftTileBin: BUFFER FULL (cursor %d + %d tiles*%d), light falls back to full walk\n",
							m_Cursor, tilesX * tilesY, TILE_K + 1 );
		}
		return -1;								// buffer full this view: full-walk fallback
	}
	if( r_rtAccelDebug.GetBool() )
	{
		common->Printf( "SoftTileBin: light %d tiles (%dx%d), %d casters, cursor %d/%d\n",
						tilesX * tilesY, tilesX, tilesY, numCasters, m_Cursor, SW_TILE_BUFFER_ELEMENTS );
	}
	const int outBase = m_Cursor;
	m_Cursor += slots;

	idRenderMatrix invMvp;
	if( !idRenderMatrix::Inverse( viewDef->worldSpace.mvp, invMvp ) )
	{
		return -1;
	}

	SoftTileBinCB cb;
	memcpy( cb.invMvp, invMvp[0], sizeof( cb.invMvp ) );
	cb.lightR[0] = vLight->globalLightOrigin.x;
	cb.lightR[1] = vLight->globalLightOrigin.y;
	cb.lightR[2] = vLight->globalLightOrigin.z;
	cb.lightR[3] = penumbraRadius;
	cb.tileRect[0] = tileOx;
	cb.tileRect[1] = tileOy;
	cb.tileRect[2] = tilesX;
	cb.tileRect[3] = tilesY;
	cb.range[0] = ( int )edgeFirstElem;
	cb.range[1] = numCasters;
	cb.range[2] = outBase;
	cb.range[3] = ( int )casterFirstElem;
	// viewport extents, so the pixel->NDC mapping matches this view's MVP even for subviews
	cb.screen[0] = ( float )( viewDef->viewport.x2 - viewDef->viewport.x1 + 1 );
	cb.screen[1] = ( float )( viewDef->viewport.y2 - viewDef->viewport.y1 + 1 );
	cb.screen[2] = ( float )viewDef->viewport.x1;
	cb.screen[3] = ( float )viewDef->viewport.y1;
	cb.minmax[0] = m_MinMaxTilesX;
	extern idCVar r_softShadowUmbraTiles;
	cb.minmax[1] = r_softShadowUmbraTiles.GetBool() ? 1 : 0;	// whole-tile umbra sentinel enable
	cb.minmax[2] = cb.minmax[3] = 0;

	nvrhi::BindingSetDesc sd;
	sd.bindings =
	{
		nvrhi::BindingSetItem::ConstantBuffer( 0, m_ConstantBuffer ),
		nvrhi::BindingSetItem::StructuredBuffer_SRV( 0, edgeBuffer ),
		nvrhi::BindingSetItem::StructuredBuffer_SRV( 2, m_MinMaxBuffer ),
		nvrhi::BindingSetItem::StructuredBuffer_UAV( 0, m_TileBuffer ),
	};
	nvrhi::BindingSetHandle set = m_Device->createBindingSet( sd, m_Layout );

	commandList->writeBuffer( m_ConstantBuffer, &cb, sizeof( cb ) );
	nvrhi::ComputeState cs;
	cs.pipeline = m_Pipeline;
	cs.bindings = { set };
	commandList->setComputeState( cs );
	commandList->dispatch( tilesX, tilesY, 1 );

	outTileOx = tileOx;
	outTileOy = tileOy;
	outTilesX = tilesX;
	return outBase;
}
