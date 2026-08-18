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
#include "SoftShadowTermPass.h"

// mirrors c_Term in softterm.cs.hlsl
struct SoftTermCB
{
	float	lightR[4];		// light origin xyz, disk radius w
	int		range[4];		// firstElem (tri stream float4 base), casterCount, tileBase | -1, tilesX
	int		tile[4];		// tile origin x, y, atlas slot offset x, y
	int		rect[4];		// scissor origin x, y (absolute pixels), width, height
	float	falloffS[4];	// WORLD-space falloff plane (vLight->lightProject[3])
	float	projS[4];		// WORLD-space projection planes (vLight->lightProject[0..2])
	float	projT[4];
	float	projQ[4];
	int		flags[4];		// x: coverage early-outs enabled
};

// conservative common limit; RDNA3 reports 16384. Exceeding it just disables the pass for the view.
static const int SW_TERM_MAX_TEX_DIM = 16384;

SoftShadowTermPass::SoftShadowTermPass( nvrhi::IDevice* device )
	: m_Device( device )
{
}

void SoftShadowTermPass::EnsurePipeline()
{
	if( m_PipelineTried )
	{
		return;
	}
	m_PipelineTried = true;

	idList<shaderMacro_t> macros;
	macros.Append( shaderMacro_t( "SW_GPU_WALK_COUNTERS", "0" ) );	// shipped permutation (must be explicit now the cfg declares {0,1})
	m_Shader = renderProgManager.GetShader( renderProgManager.FindShader( "builtin/lighting/softterm", SHADER_STAGE_COMPUTE, "", macros, true, LAYOUT_DRAW_VERT ) );
	if( m_Shader == nullptr )
	{
		common->Warning( "SoftShadowTermPass: compute shader failed to load - compute soft-shadow term disabled." );
		return;
	}

	nvrhi::BindingLayoutDesc ld;
	ld.visibility = nvrhi::ShaderType::Compute;
	ld.bindings =
	{
		nvrhi::BindingLayoutItem::VolatileConstantBuffer( 0 ),	// b0 : term constants
		nvrhi::BindingLayoutItem::StructuredBuffer_SRV( 0 ),	// t0 : edge records
		nvrhi::BindingLayoutItem::StructuredBuffer_SRV( 1 ),	// t1 : tile lists
		nvrhi::BindingLayoutItem::Texture_SRV( 2 ),				// t2 : exact world position G-buffer
		nvrhi::BindingLayoutItem::Texture_SRV( 3 ),				// t3 : light falloff (coverage early-out)
		nvrhi::BindingLayoutItem::Texture_SRV( 4 ),				// t4 : light projection (coverage early-out)
		nvrhi::BindingLayoutItem::Texture_SRV( 5 ),				// t5 : world shading normal (N.L early-out)
		nvrhi::BindingLayoutItem::Sampler( 0 ),					// s0 : falloff sampler
		nvrhi::BindingLayoutItem::Sampler( 1 ),					// s1 : projection sampler
		nvrhi::BindingLayoutItem::Texture_UAV( 0 ),				// u0 : term atlas
	};
	m_Layout = m_Device->createBindingLayout( ld );

	nvrhi::ComputePipelineDesc pd;
	pd.bindingLayouts = { m_Layout };
	pd.CS = m_Shader;
	m_Pipeline = m_Device->createComputePipeline( pd );

	// WALK-ATTRIBUTION counting permutation: same shader with -D SW_GPU_WALK_COUNTERS=1 (adds a u1
	// counter UAV). Separate layout+pipeline so the shipped pipeline stays byte-identical; used only
	// when r_softShadowWalkCounters is set. Failure here is non-fatal (counters just unavailable).
	{
		idList<shaderMacro_t> cntMacros;
		cntMacros.Append( shaderMacro_t( "SW_GPU_WALK_COUNTERS", "1" ) );
		// DISTINCT nameOutSuffix: FindShader dedups by name+stage+suffix and IGNORES macros, so without a
		// distinct suffix the counting call returns the shipped (=0) entry. The suffix does not change the
		// blob path (LoadShader keys the .bin on shader.name only) - it forces a separate entry whose
		// macros make FindPermutationInBlob select the =1 variant.
		m_ShaderCnt = renderProgManager.GetShader( renderProgManager.FindShader( "builtin/lighting/softterm", SHADER_STAGE_COMPUTE, "walkcnt", cntMacros, true, LAYOUT_DRAW_VERT ) );
		if( m_ShaderCnt != nullptr )
		{
			nvrhi::BindingLayoutDesc lc = ld;
			lc.bindings.push_back( nvrhi::BindingLayoutItem::StructuredBuffer_UAV( 1 ) );	// u1 : walk counters
			m_LayoutCnt = m_Device->createBindingLayout( lc );
			nvrhi::ComputePipelineDesc pc;
			pc.bindingLayouts = { m_LayoutCnt };
			pc.CS = m_ShaderCnt;
			m_PipelineCnt = m_Device->createComputePipeline( pc );

			nvrhi::BufferDesc wc;
			wc.byteSize = 8 * sizeof( uint32_t );
			wc.structStride = sizeof( uint32_t );		// RWStructuredBuffer<uint> (matches u_SpillCnt pattern)
			wc.canHaveUAVs = true;
			wc.initialState = nvrhi::ResourceStates::UnorderedAccess;
			wc.keepInitialState = true;
			wc.debugName = "SoftShadowTerm/WalkCounters";
			m_WalkCntBuffer = m_Device->createBuffer( wc );
		}
	}

	nvrhi::BufferDesc cb;
	cb.byteSize = sizeof( SoftTermCB );
	cb.isConstantBuffer = true;
	cb.isVolatile = true;
	cb.maxVersions = 1024;					// dispatches per frame x frames in flight
	cb.debugName = "SoftShadowTerm/CB";
	m_ConstantBuffer = m_Device->createBuffer( cb );
}

bool SoftShadowTermPass::BeginView( nvrhi::ICommandList* commandList, const viewDef_t* viewDef, nvrhi::ITexture* worldPosTexture, nvrhi::ITexture* worldNormalTexture )
{
	m_Cursor = 0;
	m_ShelfX = m_ShelfY = m_ShelfH = 0;	// reset the scissor packer for this view
	m_Valid = false;
	m_WorldPos = worldPosTexture;
	// walk-attribution counting: snapshot the cvar; clear the counter buffer so this view's totals
	// start at zero. Only active when the counting pipeline built (m_PipelineCnt).
	extern idCVar r_softShadowWalkCounters;
	m_WalkCntEnabled = r_softShadowWalkCounters.GetBool() && m_PipelineCnt != nullptr && m_WalkCntBuffer != nullptr;
	if( m_WalkCntEnabled )
	{
		commandList->clearBufferUInt( m_WalkCntBuffer, 0 );		// same as the spill counter's proven clear
	}
	m_WorldNormal = worldNormalTexture;
	EnsurePipeline();
	if( m_Pipeline == nullptr || worldPosTexture == nullptr )
	{
		return false;
	}

	// screen-size slots cover the whole render target so the atlas is addressed by SV_Position +
	// slot offset (absolute pixels, like the tile bins)
	const int screenW = viewDef->viewport.x2 + 1;
	const int screenH = viewDef->viewport.y2 + 1;
	if( screenW <= 0 || screenH <= 0
			|| screenW * SLOT_COLS > SW_TERM_MAX_TEX_DIM
			|| screenH * SLOT_ROWS > SW_TERM_MAX_TEX_DIM )
	{
		return false;						// exotic resolution: leave every light on the in-shader integral
	}

	// GROW-only: subviews (mirrors) have smaller viewports than the main view; resizing per view
	// would thrash the allocation every frame. A larger-than-needed slot is harmless - offsets are
	// multiples of the stored slot extents and every dispatch stays inside its slot.
	if( m_TermTexture == nullptr || m_SlotW < screenW || m_SlotH < screenH )
	{
		const int newW = ( m_SlotW > screenW ) ? m_SlotW : screenW;
		const int newH = ( m_SlotH > screenH ) ? m_SlotH : screenH;
		nvrhi::TextureDesc td;
		td.width = newW * SLOT_COLS;
		td.height = newH * SLOT_ROWS;
		td.format = nvrhi::Format::R16_FLOAT;	// term is k/16, exactly representable in fp16 (see header)
		td.isUAV = true;
		td.initialState = nvrhi::ResourceStates::UnorderedAccess;
		td.keepInitialState = true;
		td.debugName = "SoftShadowTerm/Atlas";
		m_TermTexture = m_Device->createTexture( td );
		m_SlotW = newW;
		m_SlotH = newH;
	}

	// The softpos raster pass wrote this texture earlier in the frame; force it through
	// ShaderResource so the compute reads are ordered after the raster writes (same explicit
	// barrier precedent as the SoftTileBinPass depth read).
	commandList->setTextureState( worldPosTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource );
	if( worldNormalTexture != nullptr )
	{
		commandList->setTextureState( worldNormalTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource );
	}

	m_Valid = true;
	return true;
}

bool SoftShadowTermPass::AddLight( nvrhi::ICommandList* commandList, const viewDef_t* viewDef, const viewLight_t* vLight,
								   nvrhi::IBuffer* edgeBuffer, uint32_t edgeFirstElem,
								   uint32_t casterFirstElem, int casterCount,
								   float penumbraRadius,
								   int tileBase, int tileOx, int tileOy, int tilesX,
								   nvrhi::IBuffer* tileBuffer,
								   nvrhi::ITexture* falloffTex, nvrhi::ISampler* falloffSamp,
								   nvrhi::ITexture* projTex, nvrhi::ISampler* projSamp,
								   bool coverageEarlyOut,
								   int& outOfsX, int& outOfsY )
{
	if( !m_Valid || casterCount <= 0 )
	{
		return false;
	}
	// light scissor in ABSOLUTE screen pixels (SV_Position space) - same mapping as BinLight:
	// scissorRect is GL-convention (bottom-up), SV_Position is top-down => Y flips against
	// viewport.y2. The old unflipped mapping wrote the term into a MIRRORED rect for every
	// non-fullscreen light; the FS then Loaded zeros from the unwritten true rect (all-black
	// light). Fullscreen lights are flip-invariant, which is how the first A/B missed it.
	const int px1 = viewDef->viewport.x1 + vLight->scissorRect.x1;
	const int py1 = viewDef->viewport.y2 - vLight->scissorRect.y2;
	const int px2 = viewDef->viewport.x1 + vLight->scissorRect.x2;
	const int py2 = viewDef->viewport.y2 - vLight->scissorRect.y1;
	if( px2 < px1 || py2 < py1 )
	{
		return false;
	}

	// SCISSOR-PACK the atlas (replaces the fixed full-screen grid): place this light's scissor-sized
	// rect on a shelf. Total lit area across a frame's soft lights is ~3-7x screen (measured), so a
	// COLS*ROWS-screen atlas holds far more than COLS*ROWS lights when scissors are sub-screen - the
	// "many small lights" rooms that previously spilled the excess onto the wave64 in-shader integral.
	const int atlasW = m_SlotW * SLOT_COLS;
	const int atlasH = m_SlotH * SLOT_ROWS;
	const int rw = px2 - px1 + 1;
	const int rh = py2 - py1 + 1;
	if( rw > atlasW || rh > atlasH )
	{
		return false;						// a single rect larger than the whole atlas: keep the integral
	}
	if( m_ShelfX + rw > atlasW )			// no room on the current shelf: start a new one below
	{
		m_ShelfX = 0;
		m_ShelfY += m_ShelfH;
		m_ShelfH = 0;
	}
	if( m_ShelfY + rh > atlasH )
	{
		extern idCVar r_rtAccelDebug;
		if( r_rtAccelDebug.GetBool() )
		{
			common->Printf( "SoftShadowTerm: atlas full (%d lights packed), light keeps the in-shader integral\n", m_Cursor );
		}
		return false;						// atlas genuinely full: the light stays on the in-shader integral
	}
	const int atlasX = m_ShelfX;
	const int atlasY = m_ShelfY;
	m_ShelfX += rw;
	if( rh > m_ShelfH )
	{
		m_ShelfH = rh;
	}
	m_Cursor++;
	// the CS writes u_Term[px + offset] and the interaction PS reads term.Load(SV_Position + offset);
	// px == SV_Position == absolute screen pixel, so offset = atlasOrigin - scissorOrigin lands both
	// at packed-local coords. Both consumers are UNCHANGED - only the offset value differs.
	const int slotOfsX = atlasX - px1;
	const int slotOfsY = atlasY - py1;

	SoftTermCB cb;
	cb.lightR[0] = vLight->globalLightOrigin.x;
	cb.lightR[1] = vLight->globalLightOrigin.y;
	cb.lightR[2] = vLight->globalLightOrigin.z;
	cb.lightR[3] = penumbraRadius;
	cb.range[0] = ( int )edgeFirstElem;
	cb.range[1] = casterCount;
	cb.range[2] = tileBase;
	cb.range[3] = tilesX;
	cb.tile[0] = tileOx;
	cb.tile[1] = tileOy;
	cb.tile[2] = slotOfsX;
	cb.tile[3] = slotOfsY;
	cb.rect[0] = px1;
	cb.rect[1] = py1;
	cb.rect[2] = px2 - px1 + 1;
	cb.rect[3] = py2 - py1 + 1;
	// WORLD-space light planes for the coverage early-out (the VS folds these into model space
	// per surface; plane . worldPos reproduces the PS's idtex2Dproj coordinates)
	for( int i = 0; i < 4; i++ )
	{
		cb.projS[i]    = vLight->lightProject[0][i];
		cb.projT[i]    = vLight->lightProject[1][i];
		cb.projQ[i]    = vLight->lightProject[2][i];
		cb.falloffS[i] = vLight->lightProject[3][i];
	}
	cb.flags[0] = ( coverageEarlyOut && falloffTex != NULL && projTex != NULL ) ? 1 : 0;
	cb.flags[1] = ( int )casterFirstElem;	// caster table base (float4 elements) - stream v2
	extern idCVar r_softShadowBackfaceCull;
	cb.flags[2] = r_softShadowBackfaceCull.GetBool() ? 1 : 0;	// N.L<=0 early-out enable
	cb.flags[3] = 0;

	// t1 must bind SOMETHING even when this light was not binned (layout demands a resource);
	// tileBase -1 keeps the shader from reading it - mirrors the pixel-shader t13 handling.
	nvrhi::IBuffer* tiles = ( tileBuffer != nullptr ) ? tileBuffer : edgeBuffer;
	// same rule for t3/t4/s0/s1 when the early-out is off (flags.x 0 keeps the shader from
	// sampling them): the caller passes black + any sampler in that case, but guard anyway.
	nvrhi::ITexture* fallT = ( falloffTex != nullptr ) ? falloffTex : m_WorldPos.Get();
	nvrhi::ITexture* projT = ( projTex != nullptr ) ? projTex : m_WorldPos.Get();
	nvrhi::ISampler* fallS = ( falloffSamp != nullptr ) ? falloffSamp : projSamp;
	nvrhi::ISampler* projS = ( projSamp != nullptr ) ? projSamp : falloffSamp;
	if( fallS == nullptr || projS == nullptr )
	{
		return false;							// no sampler at all: caller must supply one
	}

	nvrhi::BindingSetDesc sd;
	sd.bindings =
	{
		nvrhi::BindingSetItem::ConstantBuffer( 0, m_ConstantBuffer ),
		nvrhi::BindingSetItem::StructuredBuffer_SRV( 0, edgeBuffer ),
		nvrhi::BindingSetItem::StructuredBuffer_SRV( 1, tiles ),
		nvrhi::BindingSetItem::Texture_SRV( 2, m_WorldPos ),
		nvrhi::BindingSetItem::Texture_SRV( 3, fallT ),
		nvrhi::BindingSetItem::Texture_SRV( 4, projT ),
		nvrhi::BindingSetItem::Texture_SRV( 5, m_WorldNormal ? m_WorldNormal.Get() : m_WorldPos.Get() ),
		nvrhi::BindingSetItem::Sampler( 0, fallS ),
		nvrhi::BindingSetItem::Sampler( 1, projS ),
		nvrhi::BindingSetItem::Texture_UAV( 0, m_TermTexture ),
	};
	// WALK-COUNTERS: the counting permutation needs u1 in its binding set + layout; the shipped path
	// uses neither. Everything else is identical.
	const bool cnt = m_WalkCntEnabled;
	if( cnt )
	{
		sd.bindings.push_back( nvrhi::BindingSetItem::StructuredBuffer_UAV( 1, m_WalkCntBuffer ) );
	}
	nvrhi::BindingSetHandle set = m_Device->createBindingSet( sd, cnt ? m_LayoutCnt : m_Layout );

	commandList->writeBuffer( m_ConstantBuffer, &cb, sizeof( cb ) );
	nvrhi::ComputeState cs;
	cs.pipeline = cnt ? m_PipelineCnt : m_Pipeline;
	cs.bindings = { set };
	commandList->setComputeState( cs );
	commandList->dispatch( ( cb.rect[2] + 7 ) / 8, ( cb.rect[3] + 3 ) / 4, 1 );

	outOfsX = slotOfsX;
	outOfsY = slotOfsY;
	return true;
}

bool SoftShadowTermPass::GetWalkStats( uint32_t out[8] )
{
	if( !m_WalkCntEnabled || m_WalkCntBuffer == nullptr )
	{
		return false;
	}
	nvrhi::BufferDesc sbd;
	sbd.byteSize = 8 * sizeof( uint32_t );
	sbd.cpuAccess = nvrhi::CpuAccessMode::Read;
	sbd.debugName = "SoftShadowTerm/WalkCountersReadback";
	nvrhi::BufferHandle staging = m_Device->createBuffer( sbd );
	nvrhi::CommandListHandle cl = m_Device->createCommandList();
	cl->open();
	cl->copyBuffer( staging, 0, m_WalkCntBuffer, 0, 8 * sizeof( uint32_t ) );
	cl->close();
	m_Device->executeCommandList( cl );
	m_Device->waitForIdle();
	void* p = m_Device->mapBuffer( staging, nvrhi::CpuAccessMode::Read );
	if( p == nullptr )
	{
		return false;
	}
	memcpy( out, p, 8 * sizeof( uint32_t ) );
	m_Device->unmapBuffer( staging );
	return true;
}
