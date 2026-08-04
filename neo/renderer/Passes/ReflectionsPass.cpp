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
idCVar r_rtReflectionShadeBias( "r_rtReflectionShadeBias", "1.0", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "RT reflections: shadow-ray origin bias (world units) for the re-shaded hit" );
idCVar r_rtReflectionReshade( "r_rtReflectionReshade", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "RT reflections: 1 = re-shade hit from material+lights, 0 = flat average albedo only (diagnostic: isolates bindless/shadow-ray cost)" );
idCVar r_rtReflectionShadows( "r_rtReflectionShadows", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "RT reflections: 1 = trace a shadow ray per light in the re-shade, 0 = skip (diagnostic / perf)" );

// direct light gathered from the view for the world-space hit re-shade. Layout must
// match RtLight in reflection_trace.cs.hlsl (and DdgiLight in DdgiPass.cpp).
static const int RT_REFL_MAX_LIGHTS = 64;
struct ReflLight
{
	float	projectS[4];
	float	projectT[4];
	float	projectQ[4];
	float	projectFalloff[4];
	float	color[4];		// rgb (+ pad)
	float	origin[4];		// world origin xyz (+ pad)
};

extern idCVar r_lightScale;

/*
========================
R_GatherReflectionLights

Collect the view's real illuminating lights into RtLight records for the hit
re-shade. Mirrors DdgiPass's gather (interaction colour = sum of light-shader
stage colours * lightScale; the four texgen planes drive the projected shape),
minus the DDGI dynamic/static classification the reflection trace does not need.
========================
*/
static int R_GatherReflectionLights( const viewDef_t* viewDef, ReflLight* out, int maxLights )
{
	const float lightScale = r_lightScale.GetFloat();
	int numLights = 0;
	for( const viewLight_t* vLight = viewDef->viewLights; vLight != NULL && numLights < maxLights; vLight = vLight->next )
	{
		const idMaterial* lightShader = vLight->lightShader;
		if( lightShader == NULL || vLight->shaderRegisters == NULL )
		{
			continue;
		}
		if( lightShader->IsFogLight() || lightShader->IsBlendLight() )
		{
			continue;
		}

		const float* lightRegs = vLight->shaderRegisters;
		idVec3 color( 0.0f, 0.0f, 0.0f );
		for( int s = 0; s < lightShader->GetNumStages(); s++ )
		{
			const shaderStage_t* stage = lightShader->GetStage( s );
			if( !lightRegs[ stage->conditionRegister ] )
			{
				continue;
			}
			color.x += lightScale * lightRegs[ stage->color.registers[0] ];
			color.y += lightScale * lightRegs[ stage->color.registers[1] ];
			color.z += lightScale * lightRegs[ stage->color.registers[2] ];
		}
		if( color.LengthSqr() <= 0.0f )
		{
			continue;
		}

		ReflLight& L = out[numLights++];
		for( int p = 0; p < 4; p++ )
		{
			L.projectS[p]       = vLight->lightProject[0][p];
			L.projectT[p]       = vLight->lightProject[1][p];
			L.projectQ[p]       = vLight->lightProject[2][p];
			L.projectFalloff[p] = vLight->lightProject[3][p];
		}
		L.color[0] = color.x;
		L.color[1] = color.y;
		L.color[2] = color.z;
		L.color[3] = 0.0f;
		L.origin[0] = vLight->globalLightOrigin.x;
		L.origin[1] = vLight->globalLightOrigin.y;
		L.origin[2] = vLight->globalLightOrigin.z;
		L.origin[3] = 0.0f;
	}
	return numLights;
}

ReflectionsPass::ReflectionsPass( nvrhi::IDevice* device, CommonRenderPasses* commonPasses )
	: m_Device( device )
	, m_CommonPasses( commonPasses )
	, m_BoundTlas( nullptr )
	, m_BoundInstanceData( nullptr )
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
		// opt in to the bindless material table BEFORE CreateTracePass so the reflection
		// pipeline can reference its layout; the trace re-shades hits from real textures.
		m_AccelStructs.EnableBindlessMaterials();
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
		nvrhi::BindingLayoutItem::Texture_SRV( 1 ),			// t1 : resolved lit scene colour (base/passthrough)
		nvrhi::BindingLayoutItem::Texture_SRV( 2 ),			// t2 : hardware depth
		nvrhi::BindingLayoutItem::Texture_SRV( 3 ),			// t3 : gbuffer world normals + roughness
		nvrhi::BindingLayoutItem::Texture_SRV( 4 ),			// t4 : env radiance (miss fallback)
		nvrhi::BindingLayoutItem::StructuredBuffer_SRV( 5 ),	// t5 : per-instance shading data
		nvrhi::BindingLayoutItem::RawBuffer_SRV( 6 ),			// t6 : static idDrawVert cache
		nvrhi::BindingLayoutItem::RawBuffer_SRV( 7 ),			// t7 : static R16 index cache
		nvrhi::BindingLayoutItem::StructuredBuffer_SRV( 8 ),	// t8 : projected lights
		nvrhi::BindingLayoutItem::VolatileConstantBuffer( 1 ),	// b1 : ReflectionConstants
		nvrhi::BindingLayoutItem::Texture_UAV( 0 ),			// u0 : composited output
		nvrhi::BindingLayoutItem::Sampler( 0 ),				// s0 : linear-clamp (env / gbuffers)
		nvrhi::BindingLayoutItem::Sampler( 1 ),				// s1 : linear-wrap (material textures)
	};
	m_TraceBindingLayout = m_Device->createBindingLayout( layoutDesc );

	nvrhi::SamplerDesc samplerDesc;
	samplerDesc.setAllFilters( true );
	samplerDesc.setAllAddressModes( nvrhi::SamplerAddressMode::Clamp );
	m_LinearSampler = m_Device->createSampler( samplerDesc );

	nvrhi::SamplerDesc matSamplerDesc;
	matSamplerDesc.setAllFilters( true );
	matSamplerDesc.setAllAddressModes( nvrhi::SamplerAddressMode::Wrap );
	matSamplerDesc.setMaxAnisotropy( 8 );
	m_MaterialSampler = m_Device->createSampler( matSamplerDesc );

	// per-frame projected-light buffer for the hit re-shade
	nvrhi::BufferDesc lightDesc;
	lightDesc.byteSize = ( uint64_t )RT_REFL_MAX_LIGHTS * sizeof( ReflLight );
	lightDesc.structStride = sizeof( ReflLight );
	lightDesc.initialState = nvrhi::ResourceStates::ShaderResource;
	lightDesc.keepInitialState = true;
	lightDesc.debugName = "RTReflections/Lights";
	m_LightBuffer = m_Device->createBuffer( lightDesc );

	nvrhi::ComputePipelineDesc pipelineDesc;
	// second layout: the bindless material-texture table (register space 1)
	pipelineDesc.bindingLayouts = { m_TraceBindingLayout, m_AccelStructs.GetBindlessLayout() };
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

	// Skip subviews (mirrors / remote cameras / window portals): world-position
	// reconstruction + reprojection go through the subview's mirrored projection while the
	// TLAS and scene-colour are real-space, so the result thrashes. Same reason RT shadows
	// skip subviews (RenderBackend.cpp). The subview keeps its non-RT (SSR / env) path.
	if( viewDef->isSubview )
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

	// gather the view's direct lights for the world-space hit re-shade
	ReflLight lights[RT_REFL_MAX_LIGHTS];
	const int numLights = R_GatherReflectionLights( viewDef, lights, RT_REFL_MAX_LIGHTS );
	if( numLights > 0 )
	{
		commandList->writeBuffer( m_LightBuffer, lights, ( size_t )numLights * sizeof( ReflLight ) );
	}

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
	// eyePos.w carries the shadow-ray toggle for the re-shade (0 = skip shadow rays)
	constants.eyePos = idVec4( eye.x, eye.y, eye.z, r_rtReflectionShadows.GetBool() ? 1.0f : 0.0f );

	// params0.w was the (now-removed) screen-reprojection disocclusion eps; it now
	// carries the re-shade toggle (0 = flat average albedo, no bindless / no lights)
	constants.params0 = idVec4( r_rtReflectionMaxDist.GetFloat(),
								r_rtReflectionBias.GetFloat(),
								r_rtReflectionIntensity.GetFloat(),
								r_rtReflectionReshade.GetBool() ? 1.0f : 0.0f );
	// env-fallback mip scale: roughness -> mip over the radiance probe's LOD range;
	// w = shadow-ray bias for the re-shade
	constants.params1 = idVec4( r_rtReflectionGateLo.GetFloat(),
								r_rtReflectionGateHi.GetFloat(),
								6.0f, r_rtReflectionShadeBias.GetFloat() );
	constants.screenSize = idVec2i( width, height );
	constants.debugFlags = idVec2i( r_rtReflectionDebug.GetInteger(), numLights );

	commandList->writeBuffer( m_ConstantBuffer, &constants, sizeof( constants ) );

	// the TLAS handle changes when it is recreated to grow; rebuild the binding
	// set (also invalidated on image resize) to point at the current resources.
	nvrhi::rt::IAccelStruct* tlas = m_AccelStructs.GetTLAS();
	nvrhi::IBuffer* instanceData = m_AccelStructs.GetInstanceDataBuffer();
	if( m_TraceBindingSet == nullptr || m_BoundTlas != tlas || m_BoundInstanceData != instanceData )
	{
		nvrhi::BindingSetDesc setDesc;
		setDesc.bindings =
		{
			nvrhi::BindingSetItem::RayTracingAccelStruct( 0, tlas ),
			nvrhi::BindingSetItem::Texture_SRV( 1, globalImages->currentRenderImage->GetTextureHandle() ),
			nvrhi::BindingSetItem::Texture_SRV( 2, globalImages->currentDepthImage->GetTextureHandle() ),
			nvrhi::BindingSetItem::Texture_SRV( 3, globalImages->gbufferNormalsRoughnessImage->GetTextureHandle() ),
			nvrhi::BindingSetItem::Texture_SRV( 4, globalImages->defaultUACRadianceCube->GetTextureHandle() ),
			nvrhi::BindingSetItem::StructuredBuffer_SRV( 5, instanceData ),
			nvrhi::BindingSetItem::RawBuffer_SRV( 6, m_AccelStructs.GetStaticVertexBuffer() ),
			nvrhi::BindingSetItem::RawBuffer_SRV( 7, m_AccelStructs.GetStaticIndexBuffer() ),
			nvrhi::BindingSetItem::StructuredBuffer_SRV( 8, m_LightBuffer ),
			nvrhi::BindingSetItem::ConstantBuffer( 1, m_ConstantBuffer ),
			nvrhi::BindingSetItem::Texture_UAV( 0, m_ReflectionImage->GetTextureHandle() ),
			nvrhi::BindingSetItem::Sampler( 0, m_LinearSampler ),
			nvrhi::BindingSetItem::Sampler( 1, m_MaterialSampler ),
		};
		m_TraceBindingSet = m_Device->createBindingSet( setDesc, m_TraceBindingLayout );
		m_BoundTlas = tlas;
		m_BoundInstanceData = instanceData;
	}

	nvrhi::ComputeState state;
	state.pipeline = m_TracePipeline;
	state.bindings = { m_TraceBindingSet, m_AccelStructs.GetBindlessTable() };
	commandList->setComputeState( state );

	commandList->dispatch( ( width + 7 ) / 8, ( height + 7 ) / 8, 1 );

	return true;
}
