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
	int		range[4];		// firstElem, faceCount, tileBase | -1, tilesX
	int		tile[4];		// tile origin x, y, atlas slot offset x, y
	int		rect[4];		// scissor origin x, y (absolute pixels), width, height
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
		nvrhi::BindingLayoutItem::Texture_UAV( 0 ),				// u0 : term atlas
	};
	m_Layout = m_Device->createBindingLayout( ld );

	nvrhi::ComputePipelineDesc pd;
	pd.bindingLayouts = { m_Layout };
	pd.CS = m_Shader;
	m_Pipeline = m_Device->createComputePipeline( pd );

	nvrhi::BufferDesc cb;
	cb.byteSize = sizeof( SoftTermCB );
	cb.isConstantBuffer = true;
	cb.isVolatile = true;
	cb.maxVersions = 1024;					// dispatches per frame x frames in flight
	cb.debugName = "SoftShadowTerm/CB";
	m_ConstantBuffer = m_Device->createBuffer( cb );
}

bool SoftShadowTermPass::BeginView( nvrhi::ICommandList* commandList, const viewDef_t* viewDef, nvrhi::ITexture* worldPosTexture )
{
	m_Cursor = 0;
	m_Valid = false;
	m_WorldPos = worldPosTexture;
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
		td.format = nvrhi::Format::R32_FLOAT;
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

	m_Valid = true;
	return true;
}

bool SoftShadowTermPass::AddLight( nvrhi::ICommandList* commandList, const viewDef_t* viewDef, const viewLight_t* vLight,
								   nvrhi::IBuffer* edgeBuffer, uint32_t edgeFirstElem, int faceCount,
								   float penumbraRadius,
								   int tileBase, int tileOx, int tileOy, int tilesX,
								   nvrhi::IBuffer* tileBuffer,
								   int& outOfsX, int& outOfsY )
{
	if( !m_Valid || faceCount <= 0 )
	{
		return false;
	}
	if( m_Cursor >= SLOT_COLS * SLOT_ROWS )
	{
		extern idCVar r_rtAccelDebug;
		if( r_rtAccelDebug.GetBool() )
		{
			common->Printf( "SoftShadowTerm: OUT OF SLOTS (%d), light keeps the in-shader integral\n", m_Cursor );
		}
		return false;						// out of slots: the light stays on the in-shader integral
	}

	// light scissor in ABSOLUTE screen pixels (SV_Position space) - same mapping as BinLight
	const int px1 = viewDef->viewport.x1 + vLight->scissorRect.x1;
	const int py1 = viewDef->viewport.y1 + vLight->scissorRect.y1;
	const int px2 = viewDef->viewport.x1 + vLight->scissorRect.x2;
	const int py2 = viewDef->viewport.y1 + vLight->scissorRect.y2;
	if( px2 < px1 || py2 < py1 )
	{
		return false;
	}

	const int slot = m_Cursor++;
	const int slotOfsX = ( slot % SLOT_COLS ) * m_SlotW;
	const int slotOfsY = ( slot / SLOT_COLS ) * m_SlotH;

	SoftTermCB cb;
	cb.lightR[0] = vLight->globalLightOrigin.x;
	cb.lightR[1] = vLight->globalLightOrigin.y;
	cb.lightR[2] = vLight->globalLightOrigin.z;
	cb.lightR[3] = penumbraRadius;
	cb.range[0] = ( int )edgeFirstElem;
	cb.range[1] = faceCount;
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

	// t1 must bind SOMETHING even when this light was not binned (layout demands a resource);
	// tileBase -1 keeps the shader from reading it - mirrors the pixel-shader t13 handling.
	nvrhi::IBuffer* tiles = ( tileBuffer != nullptr ) ? tileBuffer : edgeBuffer;

	nvrhi::BindingSetDesc sd;
	sd.bindings =
	{
		nvrhi::BindingSetItem::ConstantBuffer( 0, m_ConstantBuffer ),
		nvrhi::BindingSetItem::StructuredBuffer_SRV( 0, edgeBuffer ),
		nvrhi::BindingSetItem::StructuredBuffer_SRV( 1, tiles ),
		nvrhi::BindingSetItem::Texture_SRV( 2, m_WorldPos ),
		nvrhi::BindingSetItem::Texture_UAV( 0, m_TermTexture ),
	};
	nvrhi::BindingSetHandle set = m_Device->createBindingSet( sd, m_Layout );

	commandList->writeBuffer( m_ConstantBuffer, &cb, sizeof( cb ) );
	nvrhi::ComputeState cs;
	cs.pipeline = m_Pipeline;
	cs.bindings = { set };
	commandList->setComputeState( cs );
	commandList->dispatch( ( cb.rect[2] + 7 ) / 8, ( cb.rect[3] + 3 ) / 4, 1 );

	outOfsX = slotOfsX;
	outOfsY = slotOfsY;
	return true;
}
