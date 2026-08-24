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
	int		tune[4];
};

// mirrors c_MinMax in softtile_minmax.cs.hlsl
struct SoftTileMinMaxCB
{
	int		dims[4];
};

// per-screen-tile min/max pairs: 4K-class screens are 256x160 tiles
static const int SW_MINMAX_MAX_TILES = 512 * 288;	// covers 8x8 tiles up to 4K (480x270); 16x16 used 256x160

// 48M uints = 192 MB: ~94k tile slots at K=512 (a heavy frame's soft lights measured ~54k slots
// on erebus1_09, so this holds every light with headroom; lights past the cap fall back to the
// full per-fragment walk for one frame). The TAIL of the buffer (SPILL_ELEMENTS, see the header)
// is reserved for overflowed tiles' spilled full lists - tile-slot allocation stops short of it.
static const int SW_TILE_BUFFER_ELEMENTS = 96 << 20;	// 384 MB. 8x8 tiles (TILE_SIZE 8) allocate ~2x the 16x16 layout at tileK 256 (main region ~48 MB of slots) + the spill tail below; sized so heavy multi-light scenes do not hit the full-walk fallback. Scale with resolution/scene if BUFFER FULL warns (r_rtAccelDebug).

SoftTileBinPass::SoftTileBinPass( nvrhi::IDevice* device )
	: m_Device( device )
{
}

void SoftTileBinPass::BeginView( nvrhi::ICommandList* commandList, const viewDef_t* viewDef, nvrhi::ITexture* depthTexture )
{
	m_Cursor = 0;
	m_MinMaxValid = false;
	// the clip->world unprojection matrix is VIEW-constant: invert once here instead of once per
	// light in BinLight (pure CPU saving, ~20 lights/view on the heavy scenes)
	idRenderMatrix invMvp;
	m_InvMvpValid = idRenderMatrix::Inverse( viewDef->worldSpace.mvp, invMvp );
	if( m_InvMvpValid )
	{
		memcpy( m_InvMvp, invMvp[0], sizeof( m_InvMvp ) );
	}
	EnsurePipeline();
	if( m_MinMaxPipeline == nullptr || depthTexture == nullptr )
	{
		return;
	}

	// reset the spill-region bump allocator: every view re-allocates the tail from zero
	commandList->clearBufferUInt( m_SpillCounter, 0 );

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
		nvrhi::BindingLayoutItem::StructuredBuffer_UAV( 1 ),	// u1 : spill-region bump allocator (1 uint)
		nvrhi::BindingLayoutItem::StructuredBuffer_UAV( 2 ),	// u2 : per-slot cull data (centroid + triRad)
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

	// CULL-BEFORE-LOAD parallel buffer: one float4 (centroid.xyz, tight triRad) per tile-list slot,
	// same index as m_TileBuffer. The term walk reads it SEQUENTIALLY to run the per-fragment cone
	// cull and only scatter-loads the 3 verts for survivors - the culled majority skips the vertex
	// gather. Written by the bin CS (u2); read by softterm (SW_CULL_BEFORE_LOAD). Full float32 so the
	// stored centroid is bit-identical to the walk's (v0+v1+v2)/3, keeping the surviving set exact.
	nvrhi::BufferDesc cd4;
	cd4.byteSize = ( uint64_t )SW_TILE_BUFFER_ELEMENTS * 2 * sizeof( uint32_t );	// uint2: centroid.xyz + triRad as 4 fp16
	cd4.structStride = 2 * sizeof( uint32_t );
	cd4.canHaveUAVs = true;
	cd4.initialState = nvrhi::ResourceStates::UnorderedAccess;
	cd4.keepInitialState = true;
	cd4.debugName = "SoftTileBin/Cull";
	m_TileCullBuffer = m_Device->createBuffer( cd4 );

	nvrhi::BufferDesc scd;
	scd.byteSize = 4 * sizeof( uint32_t );		// [0] bump cursor (= total demand), [1] overflow tiles, [2] max per-tile count
	scd.structStride = sizeof( uint32_t );
	scd.canHaveUAVs = true;
	scd.initialState = nvrhi::ResourceStates::UnorderedAccess;
	scd.keepInitialState = true;
	scd.debugName = "SoftTileBin/SpillCounter";
	m_SpillCounter = m_Device->createBuffer( scd );

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
	extern idCVar r_softShadowTileK;
	// active per-tile index capacity = the tile-buffer STRIDE. Smaller K packs the tile slots denser,
	// so the common ~80-survivor tiles' count/index words land in cache for the consumer walk; the few
	// tiles that overflow K spill to the cluster region. Crossover is GPU-specific (cache/bandwidth) -
	// a calibration knob, default TILE_K (identical to the old fixed layout).
	const int activeK = idMath::ClampInt( 1, TILE_K, r_softShadowTileK.GetInteger() );
	const int slots = tilesX * tilesY * ( activeK + 1 );
	extern idCVar r_rtAccelDebug;
	// tile-slot allocation stops short of the spill tail (overflowed tiles' full lists live there)
	if( m_Cursor + slots > SW_TILE_BUFFER_ELEMENTS - SPILL_ELEMENTS )
	{
		if( r_rtAccelDebug.GetBool() )
		{
			common->Printf( "SoftTileBin: BUFFER FULL (cursor %d + %d tiles*%d), light falls back to full walk\n",
							m_Cursor, tilesX * tilesY, activeK + 1 );
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

	if( !m_InvMvpValid )						// inverted once per view in BeginView
	{
		return -1;
	}

	SoftTileBinCB cb;
	memcpy( cb.invMvp, m_InvMvp, sizeof( cb.invMvp ) );
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
	cb.minmax[2] = SW_TILE_BUFFER_ELEMENTS - SPILL_ELEMENTS;	// spill region base (uint elements)
	cb.minmax[3] = SW_TILE_BUFFER_ELEMENTS;						// spill region end
	cb.tune[0] = activeK;	// unified active tile-K: stride + write cap + spill threshold
	extern idCVar r_softShadowCullBeforeLoad;
	cb.tune[1] = r_softShadowCullBeforeLoad.GetBool() ? 1 : 0;	// gate the parallel cull-buffer write (g_tune.y)
	cb.tune[2] = cb.tune[3] = 0;

	// binding-set CACHE: of the six bindings only t0 (the frame's joint buffer) can vary, and it is
	// constant across every light in a view (the pointer rotates once per frame with frameData) -
	// so one createBindingSet per frame instead of one per light (~20/view on heavy scenes).
	if( m_CachedSet == nullptr || m_CachedSetEdgeBuffer != edgeBuffer )
	{
		nvrhi::BindingSetDesc sd;
		sd.bindings =
		{
			nvrhi::BindingSetItem::ConstantBuffer( 0, m_ConstantBuffer ),
			nvrhi::BindingSetItem::StructuredBuffer_SRV( 0, edgeBuffer ),
			nvrhi::BindingSetItem::StructuredBuffer_SRV( 2, m_MinMaxBuffer ),
			nvrhi::BindingSetItem::StructuredBuffer_UAV( 0, m_TileBuffer ),
			nvrhi::BindingSetItem::StructuredBuffer_UAV( 1, m_SpillCounter ),
			nvrhi::BindingSetItem::StructuredBuffer_UAV( 2, m_TileCullBuffer ),
		};
		m_CachedSet = m_Device->createBindingSet( sd, m_Layout );
		m_CachedSetEdgeBuffer = edgeBuffer;
	}
	nvrhi::BindingSetHandle set = m_CachedSet;

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

// blocking debug readback of the spill allocator/stats ([0] demand, [1] overflow tiles, [2] max
// per-tile count) - bench/diagnostic instrument: SILENT spill exhaustion is a perf leak the
// correctness gate can never see, so the gate bench prints these after its timing loop.
bool SoftTileBinPass::DebugReadSpillStats( uint32_t out[4] )
{
	if( m_SpillCounter == nullptr )
	{
		return false;
	}
	nvrhi::BufferDesc sbd;
	sbd.byteSize = 4 * sizeof( uint32_t );
	sbd.cpuAccess = nvrhi::CpuAccessMode::Read;
	sbd.debugName = "SoftTileBin/SpillStatsReadback";
	nvrhi::BufferHandle staging = m_Device->createBuffer( sbd );
	nvrhi::CommandListHandle cl = m_Device->createCommandList();
	cl->open();
	cl->copyBuffer( staging, 0, m_SpillCounter, 0, 4 * sizeof( uint32_t ) );
	cl->close();
	m_Device->executeCommandList( cl );
	m_Device->waitForIdle();
	void* p = m_Device->mapBuffer( staging, nvrhi::CpuAccessMode::Read );
	if( p == nullptr )
	{
		return false;
	}
	memcpy( out, p, 4 * sizeof( uint32_t ) );
	m_Device->unmapBuffer( staging );
	return true;
}
