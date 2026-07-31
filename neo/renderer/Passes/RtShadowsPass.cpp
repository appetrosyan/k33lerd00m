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

#include "RtShadowsPass.h"
#include "RtShadowsPass_cb.h"

// Main toggle lives with the other renderer cvars in RenderSystem_init.cpp.
extern idCVar r_useRTShadows;

// Pass-local tuning cvars (mirrors the ReflectionsPass convention of file-scope statics).
idCVar r_rtShadowBias( "r_rtShadowBias", "1.5", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "RT shadows: ray origin bias along the surface normal (self-intersection)" );
idCVar r_rtShadowRays( "r_rtShadowRays", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER | CVAR_NEW, "RT shadows: visibility rays per pixel (1 = hard shadow; >1 = brute-force soft, no denoiser)", 1, 16 );
idCVar r_rtShadowSoftRadius( "r_rtShadowSoftRadius", "12.0", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "RT shadows: light radius in world units used for soft penumbra when r_rtShadowRays > 1" );
idCVar r_rtShadowBackfaceCull( "r_rtShadowBackfaceCull", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL | CVAR_NEW, "RT shadows: cull back-facing triangles as occluders so coplanar/thin back-side surfaces don't wrongly shadow (fixes flashlight-cone z-fighting); 0 if it light-leaks on one-sided geometry" );

RtShadowsPass::RtShadowsPass( nvrhi::IDevice* device, CommonRenderPasses* commonPasses )
	: m_Device( device )
	, m_CommonPasses( commonPasses )
	, m_BoundTlas( nullptr )
	, m_BoundMask( nullptr )
	, m_ViewReady( false )
	, rayTracingSupported( false )
	, loggedFirstBuild( false )
{
	m_AccelStructs.Init( device );

	// Inline ray query from a compute shader needs both RayQuery and acceleration
	// structure support; everything below is gated on this.
	rayTracingSupported =
		m_Device->queryFeatureSupport( nvrhi::Feature::RayQuery ) &&
		m_Device->queryFeatureSupport( nvrhi::Feature::RayTracingAccelStruct );

	// Unlike the other RT passes, RenderLight writes this volatile buffer ONCE PER LIGHT
	// (each shadow-casting point/spot re-fills the light origin before its dispatch), so
	// the version ring must cover every such light in a frame - not just one write. 16
	// (c_MaxRenderPassConstantBufferVersions) overflows on busy maps and nvrhi fatals.
	nvrhi::BufferDesc constantBufferDesc;
	constantBufferDesc.byteSize = sizeof( RtShadowConstants );
	constantBufferDesc.debugName = "RtShadowConstants";
	constantBufferDesc.isConstantBuffer = true;
	constantBufferDesc.isVolatile = true;
	constantBufferDesc.maxVersions = 1024;
	m_ConstantBuffer = m_Device->createBuffer( constantBufferDesc );

	if( rayTracingSupported )
	{
		common->Printf( "RtShadowsPass: ray tracing supported, RT shadows available.\n" );
		CreateTracePass();
	}
	else
	{
		common->Warning( "RtShadowsPass: device has no ray query / acceleration structure support - RT shadows disabled." );
	}
}

RtShadowsPass::~RtShadowsPass()
{
	m_AccelStructs.Shutdown();
}

/*
========================
RtShadowsPass::CreateTracePass
========================
*/
void RtShadowsPass::CreateTracePass()
{
	idList<shaderMacro_t> macros;
	const int shaderIdx = renderProgManager.FindShader( "builtin/reflections/rt_shadows", SHADER_STAGE_COMPUTE, "", macros, true, LAYOUT_DRAW_VERT );
	m_TraceShader = renderProgManager.GetShader( shaderIdx );

	if( m_TraceShader == nullptr )
	{
		common->Warning( "RtShadowsPass: rt_shadows compute shader failed to load (idx %i) - RT shadows disabled.", shaderIdx );
		rayTracingSupported = false;
		return;
	}

	nvrhi::BindingLayoutDesc layoutDesc;
	layoutDesc.visibility = nvrhi::ShaderType::Compute;
	layoutDesc.bindings =
	{
		nvrhi::BindingLayoutItem::RayTracingAccelStruct( 0 ),	// t0 : world TLAS
		nvrhi::BindingLayoutItem::Texture_SRV( 1 ),			// t1 : hardware depth
		nvrhi::BindingLayoutItem::Texture_SRV( 2 ),			// t2 : gbuffer world normals + roughness
		nvrhi::BindingLayoutItem::VolatileConstantBuffer( 1 ),	// b1 : RtShadowConstants
		nvrhi::BindingLayoutItem::Texture_UAV( 0 ),			// u0 : visibility mask
	};
	m_TraceBindingLayout = m_Device->createBindingLayout( layoutDesc );

	nvrhi::ComputePipelineDesc pipelineDesc;
	pipelineDesc.bindingLayouts = { m_TraceBindingLayout };
	pipelineDesc.CS = m_TraceShader;
	m_TracePipeline = m_Device->createComputePipeline( pipelineDesc );
}

/*
========================
RtShadowsPass::BeginView

Rebuild the TLAS from the visible static world once for the whole light loop.
========================
*/
bool RtShadowsPass::BeginView( nvrhi::ICommandList* commandList, const viewDef_t* viewDef )
{
	m_ViewReady = false;

	if( !r_useRTShadows.GetBool() || !rayTracingSupported || viewDef == NULL || m_TracePipeline == nullptr )
	{
		return false;
	}

	m_ViewReady = m_AccelStructs.RebuildFromView( commandList, viewDef );

	if( m_ViewReady && !loggedFirstBuild )
	{
		common->Printf( "RtShadowsPass: built TLAS with %i static instances.\n", m_AccelStructs.NumInstances() );
		loggedFirstBuild = true;
	}

	return m_ViewReady;
}

/*
========================
RtShadowsPass::RenderLight
========================
*/
bool RtShadowsPass::RenderLight( nvrhi::ICommandList* commandList, const viewDef_t* viewDef, const viewLight_t* vLight )
{
	if( !m_ViewReady || vLight == NULL )
	{
		return false;
	}

	idImage* mask = globalImages->rtShadowMaskImage;
	if( mask == NULL || mask->GetTextureHandle() == nullptr )
	{
		return false;
	}

	// Run at the render (supersampled) resolution: the mask lines up 1:1 with the
	// depth / gbuffer it reconstructs from and with the interaction pass's SV_Position.
	const int width = renderSystem->GetRenderWidth();
	const int height = renderSystem->GetRenderHeight();
	if( width <= 0 || height <= 0 )
	{
		return false;
	}

	RtShadowConstants constants;
	memset( &constants, 0, sizeof( constants ) );

	const idRenderMatrix& u2w = viewDef->unprojectionToWorldRenderMatrix;
	constants.unprojToWorld0 = idVec4( u2w[0][0], u2w[0][1], u2w[0][2], u2w[0][3] );
	constants.unprojToWorld1 = idVec4( u2w[1][0], u2w[1][1], u2w[1][2], u2w[1][3] );
	constants.unprojToWorld2 = idVec4( u2w[2][0], u2w[2][1], u2w[2][2], u2w[2][3] );
	constants.unprojToWorld3 = idVec4( u2w[3][0], u2w[3][1], u2w[3][2], u2w[3][3] );

	const idVec3 lightOrigin = vLight->globalLightOrigin;
	constants.lightOrigin = idVec4( lightOrigin.x, lightOrigin.y, lightOrigin.z, r_rtShadowSoftRadius.GetFloat() );
	constants.params = idVec4( r_rtShadowBias.GetFloat(),
							   0.0f,
							   ( float )r_rtShadowRays.GetInteger(),
							   ( float )( tr.frameCount & 1023 ) );
	constants.screenSize = idVec2i( width, height );
	constants.pad = idVec2i( r_rtShadowBackfaceCull.GetBool() ? 1 : 0, 0 );

	commandList->writeBuffer( m_ConstantBuffer, &constants, sizeof( constants ) );

	// the TLAS handle changes when it is recreated to grow; rebuild the binding set
	// (also invalidated when the mask image handle changes) to track current resources.
	nvrhi::rt::IAccelStruct* tlas = m_AccelStructs.GetTLAS();
	if( m_TraceBindingSet == nullptr || m_BoundTlas != tlas || m_BoundMask != mask )
	{
		nvrhi::BindingSetDesc setDesc;
		setDesc.bindings =
		{
			nvrhi::BindingSetItem::RayTracingAccelStruct( 0, tlas ),
			nvrhi::BindingSetItem::Texture_SRV( 1, globalImages->currentDepthImage->GetTextureHandle() ),
			nvrhi::BindingSetItem::Texture_SRV( 2, globalImages->gbufferNormalsRoughnessImage->GetTextureHandle() ),
			nvrhi::BindingSetItem::ConstantBuffer( 1, m_ConstantBuffer ),
			nvrhi::BindingSetItem::Texture_UAV( 0, mask->GetTextureHandle() ),
		};
		m_TraceBindingSet = m_Device->createBindingSet( setDesc, m_TraceBindingLayout );
		m_BoundTlas = tlas;
		m_BoundMask = mask;
	}

	nvrhi::ComputeState state;
	state.pipeline = m_TracePipeline;
	state.bindings = { m_TraceBindingSet };
	commandList->setComputeState( state );

	commandList->dispatch( ( width + 7 ) / 8, ( height + 7 ) / 8, 1 );

	return true;
}
