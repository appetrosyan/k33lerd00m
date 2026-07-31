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

#include "ReflectionsPass.h"
#include "ReflectionsPass_cb.h"

// Main toggle lives with the other GI cvars in RenderSystem_init.cpp.
extern idCVar r_useRTReflections;

// Pass-local tuning cvars (mirrors the DdgiPass convention of file-scope statics).
idCVar r_rtReflectionMaxDist( "r_rtReflectionMaxDist", "4000", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "RT reflections: maximum ray distance in world units" );
idCVar r_rtReflectionBias( "r_rtReflectionBias", "2.0", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "RT reflections: ray origin bias along the normal (self-intersection)" );
idCVar r_rtReflectionIntensity( "r_rtReflectionIntensity", "1.0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT | CVAR_NEW, "RT reflections: reflection blend intensity", 0.0f, 1.0f );
idCVar r_rtReflectionDisoccEps( "r_rtReflectionDisoccEps", "8.0", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "RT reflections: disocclusion tolerance in world units when sampling the reprojected hit" );
idCVar r_rtReflectionGateLo( "r_rtReflectionGateLo", "0.1", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "RT reflections: roughness at/below which a surface fully mirrors", 0.0f, 1.0f );
idCVar r_rtReflectionGateHi( "r_rtReflectionGateHi", "0.45", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT | CVAR_NEW, "RT reflections: roughness at/above which a surface does not reflect", 0.0f, 1.0f );
idCVar r_rtReflectionDebug( "r_rtReflectionDebug", "0", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "RT reflections: 1 = show reflections at full strength, 2 = visualise trace (red=hit, green=valid on-screen sample)", 0, 2 );

ReflectionsPass::ReflectionsPass( nvrhi::IDevice* device, CommonRenderPasses* commonPasses )
	: m_Device( device )
	, m_CommonPasses( commonPasses )
	, m_BoundTlas( nullptr )
	, m_ReflectionImage( nullptr )
	, m_ImageWidth( 0 )
	, m_ImageHeight( 0 )
	, rayTracingSupported( false )
	, loggedFirstBuild( false )
{
	m_AccelStructs.Init( device );

	// Inline ray query from a compute shader needs both RayQuery and acceleration
	// structure support; everything below is gated on this.
	rayTracingSupported =
		m_Device->queryFeatureSupport( nvrhi::Feature::RayQuery ) &&
		m_Device->queryFeatureSupport( nvrhi::Feature::RayTracingAccelStruct );

	nvrhi::BufferDesc constantBufferDesc;
	constantBufferDesc.byteSize = sizeof( ReflectionConstants );
	constantBufferDesc.debugName = "ReflectionConstants";
	constantBufferDesc.isConstantBuffer = true;
	constantBufferDesc.isVolatile = true;
	constantBufferDesc.maxVersions = c_MaxRenderPassConstantBufferVersions;
	m_ConstantBuffer = m_Device->createBuffer( constantBufferDesc );

	if( rayTracingSupported )
	{
		common->Printf( "ReflectionsPass: ray tracing supported, RT reflections available.\n" );
		CreateTracePass();
	}
	else
	{
		common->Warning( "ReflectionsPass: device has no ray query / acceleration structure support - RT reflections disabled." );
	}
}

ReflectionsPass::~ReflectionsPass()
{
	m_AccelStructs.Shutdown();
}

/*
========================
ReflectionsPass::CreateTracePass
========================
*/
void ReflectionsPass::CreateTracePass()
{
	idList<shaderMacro_t> macros;
	const int shaderIdx = renderProgManager.FindShader( "builtin/reflections/reflection_trace", SHADER_STAGE_COMPUTE, "", macros, true, LAYOUT_DRAW_VERT );
	m_TraceShader = renderProgManager.GetShader( shaderIdx );

	if( m_TraceShader == nullptr )
	{
		common->Warning( "ReflectionsPass: reflection_trace compute shader failed to load (idx %i) - RT reflections disabled.", shaderIdx );
		rayTracingSupported = false;
		return;
	}

	nvrhi::BindingLayoutDesc layoutDesc;
	layoutDesc.visibility = nvrhi::ShaderType::Compute;
	layoutDesc.bindings =
	{
		nvrhi::BindingLayoutItem::RayTracingAccelStruct( 0 ),	// t0 : world TLAS
		nvrhi::BindingLayoutItem::Texture_SRV( 1 ),			// t1 : resolved lit scene colour
		nvrhi::BindingLayoutItem::Texture_SRV( 2 ),			// t2 : hardware depth
		nvrhi::BindingLayoutItem::Texture_SRV( 3 ),			// t3 : gbuffer world normals + roughness
		nvrhi::BindingLayoutItem::Texture_SRV( 4 ),			// t4 : env radiance (off-screen fallback)
		nvrhi::BindingLayoutItem::VolatileConstantBuffer( 1 ),	// b1 : ReflectionConstants
		nvrhi::BindingLayoutItem::Texture_UAV( 0 ),			// u0 : composited output
		nvrhi::BindingLayoutItem::Sampler( 0 ),				// s0 : linear-clamp sampler
	};
	m_TraceBindingLayout = m_Device->createBindingLayout( layoutDesc );

	nvrhi::SamplerDesc samplerDesc;
	samplerDesc.setAllFilters( true );
	samplerDesc.setAllAddressModes( nvrhi::SamplerAddressMode::Clamp );
	m_LinearSampler = m_Device->createSampler( samplerDesc );

	nvrhi::ComputePipelineDesc pipelineDesc;
	pipelineDesc.bindingLayouts = { m_TraceBindingLayout };
	pipelineDesc.CS = m_TraceShader;
	m_TracePipeline = m_Device->createComputePipeline( pipelineDesc );
}

/*
========================
ReflectionsPass::EnsureReflectionImage

The composited output is a screen-sized HDR image (RGBA16F, UAV + sampleable so
the backend can blit it back over the HDR scene target). Recreated on resize.
========================
*/
void ReflectionsPass::EnsureReflectionImage( int width, int height )
{
	if( m_ReflectionImage != nullptr && m_ImageWidth == width && m_ImageHeight == height )
	{
		return;
	}

	if( m_ReflectionImage == nullptr )
	{
		m_ReflectionImage = globalImages->AllocStandaloneImage( "_rtReflection" );
	}

	m_ReflectionImage->GenerateImage( NULL, width, height, TF_LINEAR, TR_CLAMP, TD_RGBA16F, nullptr, true, true );
	m_ImageWidth = width;
	m_ImageHeight = height;

	// force the binding set to rebuild against the new image handle
	m_TraceBindingSet = nullptr;
}

/*
========================
ReflectionsPass::Render
========================
*/
bool ReflectionsPass::Render( nvrhi::ICommandList* commandList, const viewDef_t* viewDef )
{
	if( !r_useRTReflections.GetBool() || !rayTracingSupported || viewDef == NULL || m_TracePipeline == nullptr )
	{
		return false;
	}

	// (Re)build the TLAS from the visible static world geometry.
	const bool tlasReady = m_AccelStructs.RebuildFromView( commandList, viewDef );
	if( !tlasReady )
	{
		return false;
	}

	if( !loggedFirstBuild )
	{
		common->Printf( "ReflectionsPass: built TLAS with %i static instances.\n", m_AccelStructs.NumInstances() );
		loggedFirstBuild = true;
	}

	// Run at the render (supersampled) resolution, NOT native: the pass samples the
	// scene G-buffers and screen-colour copy (currentRenderImage / depth / normals),
	// all sized GetRenderWidth, and composites into the supersampled scene. Using
	// native GetWidth here made screenSize/dispatch/output native while the sampled
	// images were 2x under SSAA, so reflections read the screen colour at half scale
	// -> doubled/ghosted reflections on every reflective surface.
	const int width = renderSystem->GetRenderWidth();
	const int height = renderSystem->GetRenderHeight();
	if( width <= 0 || height <= 0 )
	{
		return false;
	}
	EnsureReflectionImage( width, height );

	ReflectionConstants constants;
	memset( &constants, 0, sizeof( constants ) );

	const idRenderMatrix& u2w = viewDef->unprojectionToWorldRenderMatrix;
	constants.unprojToWorld0 = idVec4( u2w[0][0], u2w[0][1], u2w[0][2], u2w[0][3] );
	constants.unprojToWorld1 = idVec4( u2w[1][0], u2w[1][1], u2w[1][2], u2w[1][3] );
	constants.unprojToWorld2 = idVec4( u2w[2][0], u2w[2][1], u2w[2][2], u2w[2][3] );
	constants.unprojToWorld3 = idVec4( u2w[3][0], u2w[3][1], u2w[3][2], u2w[3][3] );

	const idRenderMatrix& w2c = viewDef->worldSpace.mvp;
	constants.worldToClip0 = idVec4( w2c[0][0], w2c[0][1], w2c[0][2], w2c[0][3] );
	constants.worldToClip1 = idVec4( w2c[1][0], w2c[1][1], w2c[1][2], w2c[1][3] );
	constants.worldToClip2 = idVec4( w2c[2][0], w2c[2][1], w2c[2][2], w2c[2][3] );
	constants.worldToClip3 = idVec4( w2c[3][0], w2c[3][1], w2c[3][2], w2c[3][3] );

	const idVec3 eye = viewDef->renderView.vieworg;
	constants.eyePos = idVec4( eye.x, eye.y, eye.z, 0.0f );

	constants.params0 = idVec4( r_rtReflectionMaxDist.GetFloat(),
								r_rtReflectionBias.GetFloat(),
								r_rtReflectionIntensity.GetFloat(),
								r_rtReflectionDisoccEps.GetFloat() );
	// env-fallback mip scale: roughness -> mip over the radiance probe's LOD range
	constants.params1 = idVec4( r_rtReflectionGateLo.GetFloat(),
								r_rtReflectionGateHi.GetFloat(),
								6.0f, 0.0f );
	constants.screenSize = idVec2i( width, height );
	constants.debugFlags = idVec2i( r_rtReflectionDebug.GetInteger(), 0 );

	commandList->writeBuffer( m_ConstantBuffer, &constants, sizeof( constants ) );

	// the TLAS handle changes when it is recreated to grow; rebuild the binding
	// set (also invalidated on image resize) to point at the current resources.
	nvrhi::rt::IAccelStruct* tlas = m_AccelStructs.GetTLAS();
	if( m_TraceBindingSet == nullptr || m_BoundTlas != tlas )
	{
		nvrhi::BindingSetDesc setDesc;
		setDesc.bindings =
		{
			nvrhi::BindingSetItem::RayTracingAccelStruct( 0, tlas ),
			nvrhi::BindingSetItem::Texture_SRV( 1, globalImages->currentRenderImage->GetTextureHandle() ),
			nvrhi::BindingSetItem::Texture_SRV( 2, globalImages->currentDepthImage->GetTextureHandle() ),
			nvrhi::BindingSetItem::Texture_SRV( 3, globalImages->gbufferNormalsRoughnessImage->GetTextureHandle() ),
			nvrhi::BindingSetItem::Texture_SRV( 4, globalImages->defaultUACRadianceCube->GetTextureHandle() ),
			nvrhi::BindingSetItem::ConstantBuffer( 1, m_ConstantBuffer ),
			nvrhi::BindingSetItem::Texture_UAV( 0, m_ReflectionImage->GetTextureHandle() ),
			nvrhi::BindingSetItem::Sampler( 0, m_LinearSampler ),
		};
		m_TraceBindingSet = m_Device->createBindingSet( setDesc, m_TraceBindingLayout );
		m_BoundTlas = tlas;
	}

	nvrhi::ComputeState state;
	state.pipeline = m_TracePipeline;
	state.bindings = { m_TraceBindingSet };
	commandList->setComputeState( state );

	commandList->dispatch( ( width + 7 ) / 8, ( height + 7 ) / 8, 1 );

	return true;
}
