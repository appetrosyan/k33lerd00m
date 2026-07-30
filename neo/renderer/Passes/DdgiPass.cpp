/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 2026 Robert Beckebans (RBDOOM-3-BFG contributors)

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

#include "DdgiPass.h"
#include "DdgiPass_cb.h"

// Main toggle lives with the other GI cvars in RenderSystem_init.cpp.
extern idCVar r_useDDGI;
extern idCVar r_lightScale;

// Fixed probe volume for M2 (camera-centred). Made map-aware / cascaded later.
static const int DDGI_MAX_RAYS = 256;

// Upper bound on projected lights fed to the probe trace each frame (M4 stage 2).
static const int DDGI_MAX_LIGHTS = 64;

// Per-light shading data for the probe trace. Layout must match DdgiLight in
// probe_trace.cs.hlsl. The four planes are Doom's classic light-projection
// texgen planes (S, T, Q-divide, falloff); dot(plane, float4(worldPos,1)).
struct DdgiLight
{
	float	projectS[4];
	float	projectT[4];
	float	projectQ[4];
	float	projectFalloff[4];
	float	color[4];		// rgb (+ pad)
	float	origin[4];		// world origin xyz (+ pad)
};

// Pass-local tuning cvars (mirrors the SsaoPass convention of file-scope statics).
idCVar r_ddgiProbeSpacing( "r_ddgiProbeSpacing", "64", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "DDGI probe spacing in world units" );
idCVar r_ddgiRaysPerProbe( "r_ddgiRaysPerProbe", "128", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "rays traced per probe per frame", 32, 256 );
idCVar r_ddgiHysteresis( "r_ddgiHysteresis", "0.97", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "temporal blend weight for probe history [0..1]", 0.0f, 1.0f );
idCVar r_ddgiNormalBias( "r_ddgiNormalBias", "0.25", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "probe sampling normal bias in world units" );
idCVar r_ddgiDebug( "r_ddgiDebug", "0", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "overlay DDGI probe atlas: 1 = irradiance atlas", 0, 1 );
idCVar r_ddgiBounceGain( "r_ddgiBounceGain", "0.95", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "DDGI multi-bounce feedback gain (0 = single bounce)", 0.0f, 2.0f );
idCVar r_ddgiUpdateScope( "r_ddgiUpdateScope", "1", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "which probes trace each frame: 0 = visible only, 1 = visible + neighbours, 2 = full", 0, 2 );

DdgiPass::DdgiPass( nvrhi::IDevice* device, CommonRenderPasses* commonPasses )
	: m_Device( device )
	, m_CommonPasses( commonPasses )
	, m_TraceBoundTlas( nullptr )
	, m_TraceBoundInstanceData( nullptr )
	, m_IrradianceImage( nullptr )
	, m_DistanceImage( nullptr )
	, m_FrameIndex( 0 )
	, rayTracingSupported( false )
	, loggedFirstBuild( false )
{
	// Camera-anchored volume. X*Z folds into the atlas width (countX*countZ*SIZE),
	// which is texture-dimension limited (~16384) - not memory - so keep X*Z within
	// ~1024. countY is cheap. r_ddgiUpdateScope culls which probes trace per frame.
	m_ProbeCounts[0] = 24;
	m_ProbeCounts[1] = 16;
	m_ProbeCounts[2] = 24;
	m_VolumeSpacing = 64.0f;

	m_AccelStructs.Init( device );
	// DDGI traces probe rays with inline ray queries from a compute shader, so
	// it needs both RayQuery and acceleration-structure support. Everything the
	// later milestones create (accel structs, RT binding sets) is gated on this.
	rayTracingSupported =
		m_Device->queryFeatureSupport( nvrhi::Feature::RayQuery ) &&
		m_Device->queryFeatureSupport( nvrhi::Feature::RayTracingAccelStruct );

	nvrhi::BufferDesc constantBufferDesc;
	constantBufferDesc.byteSize = sizeof( DdgiConstants );
	constantBufferDesc.debugName = "DdgiConstants";
	constantBufferDesc.isConstantBuffer = true;
	constantBufferDesc.isVolatile = true;
	constantBufferDesc.maxVersions = c_MaxRenderPassConstantBufferVersions;
	m_ConstantBuffer = m_Device->createBuffer( constantBufferDesc );

	if( rayTracingSupported )
	{
		common->Printf( "DdgiPass: ray tracing supported, dynamic diffuse GI available.\n" );
		CreateTracePass();
		if( rayTracingSupported )
		{
			CreateIntegratePass();
		}
	}
	else
	{
		common->Warning( "DdgiPass: device has no ray query / acceleration structure support - DDGI disabled, falling back to baked light grid." );
	}
}

DdgiPass::~DdgiPass()
{
	m_AccelStructs.Shutdown();
}

/*
========================
DdgiPass::CreateTracePass

Builds the probe trace compute pipeline (inline ray query) and the per-ray
radiance output buffer. Called once, only when ray tracing is supported.
========================
*/
void DdgiPass::CreateTracePass()
{
	idList<shaderMacro_t> macros;
	const int shaderIdx = renderProgManager.FindShader( "builtin/ddgi/probe_trace", SHADER_STAGE_COMPUTE, "", macros, true, LAYOUT_DRAW_VERT );
	m_TraceShader = renderProgManager.GetShader( shaderIdx );

	if( m_TraceShader == nullptr )
	{
		common->Warning( "DdgiPass: probe_trace compute shader failed to load (idx %i) - DDGI trace disabled.", shaderIdx );
		rayTracingSupported = false;
		return;
	}

	nvrhi::BindingLayoutDesc layoutDesc;
	layoutDesc.visibility = nvrhi::ShaderType::Compute;
	layoutDesc.bindings =
	{
		nvrhi::BindingLayoutItem::RayTracingAccelStruct( 0 ),	// t0 : world TLAS
		nvrhi::BindingLayoutItem::StructuredBuffer_SRV( 1 ),	// t1 : per-instance data
		nvrhi::BindingLayoutItem::RawBuffer_SRV( 2 ),			// t2 : static vertex cache
		nvrhi::BindingLayoutItem::RawBuffer_SRV( 3 ),			// t3 : static index cache
		nvrhi::BindingLayoutItem::StructuredBuffer_SRV( 4 ),	// t4 : projected lights
		nvrhi::BindingLayoutItem::Texture_SRV( 5 ),			// t5 : prev irradiance atlas (multi-bounce)
		nvrhi::BindingLayoutItem::Texture_SRV( 6 ),			// t6 : prev distance atlas (Chebyshev)
		nvrhi::BindingLayoutItem::VolatileConstantBuffer( 1 ),	// b1 : DdgiConstants
		nvrhi::BindingLayoutItem::StructuredBuffer_UAV( 0 ),	// u0 : per-ray radiance
		nvrhi::BindingLayoutItem::Sampler( 0 ),				// s0 : linear-clamp atlas sampler
	};
	m_TraceBindingLayout = m_Device->createBindingLayout( layoutDesc );

	// linear-clamp sampler for reading the octahedral atlases back for bounce
	nvrhi::SamplerDesc samplerDesc;
	samplerDesc.setAllFilters( true );
	samplerDesc.setAllAddressModes( nvrhi::SamplerAddressMode::Clamp );
	m_LinearSampler = m_Device->createSampler( samplerDesc );

	nvrhi::ComputePipelineDesc pipelineDesc;
	pipelineDesc.bindingLayouts = { m_TraceBindingLayout };
	pipelineDesc.CS = m_TraceShader;
	m_TracePipeline = m_Device->createComputePipeline( pipelineDesc );

	const int maxProbes = m_ProbeCounts[0] * m_ProbeCounts[1] * m_ProbeCounts[2];

	nvrhi::BufferDesc bufferDesc;
	bufferDesc.byteSize = ( uint64_t )maxProbes * DDGI_MAX_RAYS * sizeof( float ) * 4;
	bufferDesc.structStride = sizeof( float ) * 4;			// float4 per (probe, ray)
	bufferDesc.canHaveUAVs = true;
	// keepInitialState seeds the state tracker (else first use = "Unknown prior
	// state" fatal); nvrhi still auto-inserts the UAV->SRV barrier between the
	// trace (writes it) and the integrate pass (reads it) during the list.
	bufferDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
	bufferDesc.keepInitialState = true;
	bufferDesc.debugName = "DDGI/RayRadiance";
	m_RayRadianceBuffer = m_Device->createBuffer( bufferDesc );

	// per-frame projected-light buffer (filled in DispatchProbeTrace)
	nvrhi::BufferDesc lightDesc;
	lightDesc.byteSize = ( uint64_t )DDGI_MAX_LIGHTS * sizeof( DdgiLight );
	lightDesc.structStride = sizeof( DdgiLight );
	lightDesc.initialState = nvrhi::ResourceStates::ShaderResource;
	lightDesc.keepInitialState = true;
	lightDesc.debugName = "DDGI/Lights";
	m_LightBuffer = m_Device->createBuffer( lightDesc );
	m_LightCapacity = DDGI_MAX_LIGHTS;
}

/*
========================
DdgiPass::CreateIntegratePass

Creates the octahedral irradiance + distance atlases and the compute pipeline
that folds the per-ray radiance buffer into them (with temporal blending).
========================
*/
void DdgiPass::CreateIntegratePass()
{
	const int probeSize = LIGHTGRID_IRRADIANCE_SIZE;
	const int atlasCols = m_ProbeCounts[0] * m_ProbeCounts[2];	// tile = (px + pz*countX, py)
	const int atlasRows = m_ProbeCounts[1];
	const int atlasW = atlasCols * probeSize;
	const int atlasH = atlasRows * probeSize;

	// idImages (RT+UAV, linear-clamp, keepInitialState) so the ambient pass can
	// Bind() them in place of the baked grid. Same compute-write + sample pattern
	// as the SSAO image. TF_LINEAR gives the bilinear read the border copy needs.
	m_IrradianceImage = globalImages->AllocStandaloneImage( "_ddgiIrradianceAtlas" );
	m_IrradianceImage->GenerateImage( NULL, atlasW, atlasH, TF_LINEAR, TR_CLAMP, TD_RGBA16F, nullptr, true, true );

	m_DistanceImage = globalImages->AllocStandaloneImage( "_ddgiDistanceAtlas" );
	m_DistanceImage->GenerateImage( NULL, atlasW, atlasH, TF_LINEAR, TR_CLAMP, TD_RG16F, nullptr, true, true );

	idList<shaderMacro_t> macros;
	const int shaderIdx = renderProgManager.FindShader( "builtin/ddgi/probe_integrate", SHADER_STAGE_COMPUTE, "", macros, true, LAYOUT_DRAW_VERT );
	m_IntegrateShader = renderProgManager.GetShader( shaderIdx );

	if( m_IntegrateShader == nullptr )
	{
		common->Warning( "DdgiPass: probe_integrate compute shader failed to load (idx %i) - DDGI disabled.", shaderIdx );
		rayTracingSupported = false;
		return;
	}

	nvrhi::BindingLayoutDesc layoutDesc;
	layoutDesc.visibility = nvrhi::ShaderType::Compute;
	layoutDesc.bindings =
	{
		nvrhi::BindingLayoutItem::StructuredBuffer_SRV( 0 ),	// t0 : per-ray radiance
		nvrhi::BindingLayoutItem::VolatileConstantBuffer( 1 ),	// b1 : DdgiConstants
		nvrhi::BindingLayoutItem::Texture_UAV( 0 ),			// u0 : irradiance atlas
		nvrhi::BindingLayoutItem::Texture_UAV( 1 ),			// u1 : distance atlas
	};
	m_IntegrateBindingLayout = m_Device->createBindingLayout( layoutDesc );

	nvrhi::ComputePipelineDesc pipelineDesc;
	pipelineDesc.bindingLayouts = { m_IntegrateBindingLayout };
	pipelineDesc.CS = m_IntegrateShader;
	m_IntegratePipeline = m_Device->createComputePipeline( pipelineDesc );

	nvrhi::BindingSetDesc setDesc;
	setDesc.bindings =
	{
		nvrhi::BindingSetItem::StructuredBuffer_SRV( 0, m_RayRadianceBuffer ),
		nvrhi::BindingSetItem::ConstantBuffer( 1, m_ConstantBuffer ),
		nvrhi::BindingSetItem::Texture_UAV( 0, m_IrradianceImage->GetTextureHandle() ),
		nvrhi::BindingSetItem::Texture_UAV( 1, m_DistanceImage->GetTextureHandle() ),
	};
	m_IntegrateBindingSet = m_Device->createBindingSet( setDesc, m_IntegrateBindingLayout );

	// octahedral border copy pass: shares the atlas UAV layout, no ray radiance
	const int borderIdx = renderProgManager.FindShader( "builtin/ddgi/probe_border", SHADER_STAGE_COMPUTE, "", macros, true, LAYOUT_DRAW_VERT );
	m_BorderShader = renderProgManager.GetShader( borderIdx );
	if( m_BorderShader == nullptr )
	{
		common->Warning( "DdgiPass: probe_border compute shader failed to load (idx %i) - DDGI disabled.", borderIdx );
		rayTracingSupported = false;
		return;
	}

	nvrhi::BindingLayoutDesc borderLayoutDesc;
	borderLayoutDesc.visibility = nvrhi::ShaderType::Compute;
	borderLayoutDesc.bindings =
	{
		nvrhi::BindingLayoutItem::VolatileConstantBuffer( 1 ),	// b1 : DdgiConstants
		nvrhi::BindingLayoutItem::Texture_UAV( 0 ),			// u0 : irradiance atlas
		nvrhi::BindingLayoutItem::Texture_UAV( 1 ),			// u1 : distance atlas
	};
	m_BorderBindingLayout = m_Device->createBindingLayout( borderLayoutDesc );

	nvrhi::ComputePipelineDesc borderPipelineDesc;
	borderPipelineDesc.bindingLayouts = { m_BorderBindingLayout };
	borderPipelineDesc.CS = m_BorderShader;
	m_BorderPipeline = m_Device->createComputePipeline( borderPipelineDesc );

	nvrhi::BindingSetDesc borderSetDesc;
	borderSetDesc.bindings =
	{
		nvrhi::BindingSetItem::ConstantBuffer( 1, m_ConstantBuffer ),
		nvrhi::BindingSetItem::Texture_UAV( 0, m_IrradianceImage->GetTextureHandle() ),
		nvrhi::BindingSetItem::Texture_UAV( 1, m_DistanceImage->GetTextureHandle() ),
	};
	m_BorderBindingSet = m_Device->createBindingSet( borderSetDesc, m_BorderBindingLayout );
}

/*
========================
DdgiPass::DispatchProbeTrace

Positions a camera-centred probe volume, uploads the DDGI constants and casts
r_ddgiRaysPerProbe rays per probe against the world TLAS.
========================
*/
void DdgiPass::DispatchProbeTrace( nvrhi::ICommandList* commandList, const viewDef_t* viewDef )
{
	if( m_TracePipeline == nullptr )
	{
		return;
	}

	const float spacing = Max( 1.0f, r_ddgiProbeSpacing.GetFloat() );
	int raysPerProbe = r_ddgiRaysPerProbe.GetInteger();
	raysPerProbe = idMath::ClampInt( 32, DDGI_MAX_RAYS, raysPerProbe );

	const int totalProbes = m_ProbeCounts[0] * m_ProbeCounts[1] * m_ProbeCounts[2];

	// centre the volume on the camera and snap the origin to the probe spacing
	// so the grid does not swim as the camera moves fractions of a cell.
	const idVec3 viewOrg = viewDef->renderView.vieworg;
	idVec3 origin;
	for( int i = 0; i < 3; i++ )
	{
		const float half = m_ProbeCounts[i] * spacing * 0.5f;
		origin[i] = idMath::Floor( ( viewOrg[i] - half ) / spacing ) * spacing;
	}

	// publish the volume so the ambient pass can sample this frame's atlas
	m_VolumeOrigin = origin;
	m_VolumeSpacing = spacing;

	DdgiConstants constants;
	memset( &constants, 0, sizeof( constants ) );
	constants.probeGridOrigin = idVec4( origin.x, origin.y, origin.z, 0.0f );
	constants.probeGridSpacing = idVec4( spacing, spacing, spacing, 0.0f );
	constants.probeGridCountsXY = idVec2i( m_ProbeCounts[0], m_ProbeCounts[1] );
	constants.probeGridCountZ_total = idVec2i( m_ProbeCounts[2], totalProbes );
	constants.irradianceProbe = idVec2i( LIGHTGRID_IRRADIANCE_SIZE, LIGHTGRID_IRRADIANCE_BORDER_SIZE );
	constants.distanceProbe = idVec2i( LIGHTGRID_IRRADIANCE_SIZE, LIGHTGRID_IRRADIANCE_BORDER_SIZE );
	constants.raysPerProbe = raysPerProbe;
	constants.hysteresis = r_ddgiHysteresis.GetFloat();
	constants.normalBias = r_ddgiNormalBias.GetFloat();
	constants.viewBias = 0.1f;
	constants.rayRotation = idVec4( 0.0f, 0.0f, 0.0f, 1.0f );	// identity for M2; jittered later
	constants.frameIndex = m_FrameIndex++;
	constants.bounceGain = r_ddgiBounceGain.GetFloat();

	// update scope: cull which probes trace this frame (the rest keep history).
	// world->clip for the frustum test, plus the volume centre + neighbour radius.
	constants.updateScope = r_ddgiUpdateScope.GetInteger();
	const idRenderMatrix& worldToClip = viewDef->worldSpace.mvp;
	constants.worldToClip0 = idVec4( worldToClip[0][0], worldToClip[0][1], worldToClip[0][2], worldToClip[0][3] );
	constants.worldToClip1 = idVec4( worldToClip[1][0], worldToClip[1][1], worldToClip[1][2], worldToClip[1][3] );
	constants.worldToClip2 = idVec4( worldToClip[2][0], worldToClip[2][1], worldToClip[2][2], worldToClip[2][3] );
	constants.worldToClip3 = idVec4( worldToClip[3][0], worldToClip[3][1], worldToClip[3][2], worldToClip[3][3] );
	const idVec3 center = origin + idVec3( m_ProbeCounts[0], m_ProbeCounts[1], m_ProbeCounts[2] ) * ( spacing * 0.5f );
	constants.volumeCenter = idVec4( center.x, center.y, center.z, 4.0f * spacing );

	// Gather visible projected lights for hit shading (M4 stage 2). Mirrors the
	// interaction pass: colour = sum over light-shader stages of lightScale *
	// shaderRegisters; the four texgen planes (S, T, Q-divide, falloff) drive the
	// projected light shape so DDGI bounce matches Doom's lights.
	const float lightScale = r_lightScale.GetFloat();
	DdgiLight lights[DDGI_MAX_LIGHTS];
	int numLights = 0;
	for( const viewLight_t* vLight = viewDef->viewLights; vLight != NULL && numLights < DDGI_MAX_LIGHTS; vLight = vLight->next )
	{
		const idMaterial* lightShader = vLight->lightShader;
		if( lightShader == NULL || vLight->shaderRegisters == NULL )
		{
			continue;
		}
		// only real illuminating lights bounce - skip fog and blend lights
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

		DdgiLight& L = lights[numLights++];
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
	constants.numLights = numLights;

	commandList->writeBuffer( m_ConstantBuffer, &constants, sizeof( constants ) );
	if( numLights > 0 )
	{
		commandList->writeBuffer( m_LightBuffer, lights, ( size_t )numLights * sizeof( DdgiLight ) );
	}

	// the TLAS handle changes when it is recreated to grow; rebuild the binding
	// set to point at the current one.
	nvrhi::rt::IAccelStruct* tlas = m_AccelStructs.GetTLAS();
	nvrhi::IBuffer* instanceData = m_AccelStructs.GetInstanceDataBuffer();
	if( m_TraceBindingSet == nullptr || m_TraceBoundTlas != tlas || m_TraceBoundInstanceData != instanceData )
	{
		nvrhi::BindingSetDesc setDesc;
		setDesc.bindings =
		{
			nvrhi::BindingSetItem::RayTracingAccelStruct( 0, tlas ),
			nvrhi::BindingSetItem::StructuredBuffer_SRV( 1, instanceData ),
			nvrhi::BindingSetItem::RawBuffer_SRV( 2, m_AccelStructs.GetStaticVertexBuffer() ),
			nvrhi::BindingSetItem::RawBuffer_SRV( 3, m_AccelStructs.GetStaticIndexBuffer() ),
			nvrhi::BindingSetItem::StructuredBuffer_SRV( 4, m_LightBuffer ),
			nvrhi::BindingSetItem::Texture_SRV( 5, m_IrradianceImage->GetTextureHandle() ),
			nvrhi::BindingSetItem::Texture_SRV( 6, m_DistanceImage->GetTextureHandle() ),
			nvrhi::BindingSetItem::ConstantBuffer( 1, m_ConstantBuffer ),
			nvrhi::BindingSetItem::StructuredBuffer_UAV( 0, m_RayRadianceBuffer ),
			nvrhi::BindingSetItem::Sampler( 0, m_LinearSampler ),
		};
		m_TraceBindingSet = m_Device->createBindingSet( setDesc, m_TraceBindingLayout );
		m_TraceBoundTlas = tlas;
		m_TraceBoundInstanceData = instanceData;
	}

	nvrhi::ComputeState state;
	state.pipeline = m_TracePipeline;
	state.bindings = { m_TraceBindingSet };
	commandList->setComputeState( state );

	// one thread per ray (x), one row per probe (y)
	const int groupsX = ( raysPerProbe + 31 ) / 32;
	commandList->dispatch( groupsX, totalProbes, 1 );

	// M3: integrate the per-ray radiance into the octahedral irradiance/distance
	// atlases, temporally blended. nvrhi transitions the ray-radiance buffer from
	// UAV to SRV automatically (it is state-tracked).
	if( m_IntegratePipeline != nullptr )
	{
		nvrhi::ComputeState integrateState;
		integrateState.pipeline = m_IntegratePipeline;
		integrateState.bindings = { m_IntegrateBindingSet };
		commandList->setComputeState( integrateState );

		const int probeSize = LIGHTGRID_IRRADIANCE_SIZE;
		commandList->dispatch( ( probeSize + 7 ) / 8, ( probeSize + 7 ) / 8, totalProbes );

		// M4 stage 3: octahedral border copy so the atlases sample seamlessly with
		// bilinear filtering. nvrhi inserts the UAV barrier after the integrate pass.
		if( m_BorderPipeline != nullptr )
		{
			nvrhi::ComputeState borderState;
			borderState.pipeline = m_BorderPipeline;
			borderState.bindings = { m_BorderBindingSet };
			commandList->setComputeState( borderState );
			commandList->dispatch( ( probeSize + 7 ) / 8, ( probeSize + 7 ) / 8, totalProbes );
		}
	}
}

void DdgiPass::Render( nvrhi::ICommandList* commandList, const viewDef_t* viewDef )
{
	if( !r_useDDGI.GetBool() || !rayTracingSupported || viewDef == NULL )
	{
		return;
	}

	// M1: (re)build the ray tracing acceleration structures from the visible
	// static world geometry. The TLAS is what the probe trace (M2) rays against.
	const bool tlasReady = m_AccelStructs.RebuildFromView( commandList, viewDef );

	if( tlasReady && !loggedFirstBuild )
	{
		common->Printf( "DdgiPass: built TLAS with %i static instances.\n", m_AccelStructs.NumInstances() );
		loggedFirstBuild = true;
	}

	// M2: trace r_ddgiRaysPerProbe rays/probe via inline ray query against the TLAS.
	if( tlasReady )
	{
		DispatchProbeTrace( commandList, viewDef );
	}

	// M3: integrate rays into the octahedral irradiance + distance atlases and
	//     temporally blend with r_ddgiHysteresis.
}
