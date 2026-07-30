/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 2026 Aleksandr Petrosyan

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG
Edition Source Code").

Doom 3 BFG Edition Source Code is free software: you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation, either version 3 of the License,
or (at your option) any later version.

Doom 3 BFG Edition Source Code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
Public License for more details.

You should have received a copy of the GNU General Public License along with
Doom 3 BFG Edition Source Code.  If not, see <http://www.gnu.org/licenses/>.

===========================================================================
*/
#include <precompiled.h>
#pragma hdrstop

#include "renderer/RenderCommon.h"

#include "HdrGuiCompositePass.h"

// UI white luminance, separate from the scene paper-white: scaling the 2D UI all
// the way to the scene paper-white (e.g. 200 nits) blows out the HUD, so the UI
// gets its own, lower, runtime-tunable level. 80 nits == scRGB 1.0 == the old
// (pre-composite) UI white brightness, but now correctly linearised.
idCVar r_hdrGuiPaperWhiteNits( "r_hdrGuiPaperWhiteNits", "80", CVAR_ARCHIVE | CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "HDR output: 2D UI (HUD/menu) white luminance in nits", 40.0f, 400.0f );

// Matches HdrGuiConstants in builtin/post/hdr_gui_composite.cs.hlsl.
struct HdrGuiConstants
{
	float	paperScale;
	int		width;
	int		height;
	int		pad;
};

HdrGuiCompositePass::HdrGuiCompositePass( nvrhi::IDevice* device )
	: m_Device( device )
	, m_BoundGui( nullptr )
	, m_BoundLdr( nullptr )
	, m_Valid( false )
{
	idList<shaderMacro_t> macros;
	const int shaderIdx = renderProgManager.FindShader( "builtin/post/hdr_gui_composite", SHADER_STAGE_COMPUTE, "", macros, true, LAYOUT_DRAW_VERT );
	m_Shader = renderProgManager.GetShader( shaderIdx );
	if( m_Shader == nullptr )
	{
		common->Warning( "HdrGuiCompositePass: hdr_gui_composite compute shader failed to load (idx %i) - HDR 2D composite disabled.", shaderIdx );
		return;
	}

	nvrhi::BufferDesc constantBufferDesc;
	constantBufferDesc.byteSize = sizeof( HdrGuiConstants );
	constantBufferDesc.debugName = "HdrGuiConstants";
	constantBufferDesc.isConstantBuffer = true;
	constantBufferDesc.isVolatile = true;
	constantBufferDesc.maxVersions = c_MaxRenderPassConstantBufferVersions;
	m_ConstantBuffer = m_Device->createBuffer( constantBufferDesc );

	nvrhi::BindingLayoutDesc layoutDesc;
	layoutDesc.visibility = nvrhi::ShaderType::Compute;
	layoutDesc.bindings =
	{
		nvrhi::BindingLayoutItem::Texture_SRV( 0 ),			// t0 : isolated 2D UI (premultiplied sRGB)
		nvrhi::BindingLayoutItem::VolatileConstantBuffer( 0 ),	// b0 : HdrGuiConstants
		nvrhi::BindingLayoutItem::Texture_UAV( 0 ),			// u0 : ldrImage (linear scene in / composited out)
	};
	m_BindingLayout = m_Device->createBindingLayout( layoutDesc );

	nvrhi::ComputePipelineDesc pipelineDesc;
	pipelineDesc.bindingLayouts = { m_BindingLayout };
	pipelineDesc.CS = m_Shader;
	m_Pipeline = m_Device->createComputePipeline( pipelineDesc );

	m_Valid = true;
}

HdrGuiCompositePass::~HdrGuiCompositePass()
{
}

void HdrGuiCompositePass::Render( nvrhi::ICommandList* commandList )
{
	if( !m_Valid )
	{
		return;
	}

	idImage* gui = globalImages->guiCompositeImage;
	idImage* ldr = globalImages->ldrImage;
	if( gui == NULL || ldr == NULL )
	{
		return;
	}

	const int width = ldr->GetUploadWidth();
	const int height = ldr->GetUploadHeight();
	if( width <= 0 || height <= 0 )
	{
		return;
	}

	HdrGuiConstants constants;
	constants.paperScale = Max( 1.0f, r_hdrGuiPaperWhiteNits.GetFloat() ) / 80.0f;
	constants.width = width;
	constants.height = height;
	constants.pad = 0;
	commandList->writeBuffer( m_ConstantBuffer, &constants, sizeof( constants ) );

	// (re)build the binding set when the backing images change (resize / reload)
	if( m_BindingSet == nullptr || m_BoundGui != gui || m_BoundLdr != ldr )
	{
		nvrhi::BindingSetDesc setDesc;
		setDesc.bindings =
		{
			nvrhi::BindingSetItem::Texture_SRV( 0, gui->GetTextureHandle() ),
			nvrhi::BindingSetItem::ConstantBuffer( 0, m_ConstantBuffer ),
			nvrhi::BindingSetItem::Texture_UAV( 0, ldr->GetTextureHandle() ),
		};
		m_BindingSet = m_Device->createBindingSet( setDesc, m_BindingLayout );
		m_BoundGui = gui;
		m_BoundLdr = ldr;
	}

	nvrhi::ComputeState state;
	state.pipeline = m_Pipeline;
	state.bindings = { m_BindingSet };
	commandList->setComputeState( state );

	commandList->dispatch( ( width + 7 ) / 8, ( height + 7 ) / 8, 1 );
}
