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

// Fixed probe volume for M2 (camera-centred). Made map-aware / cascaded later.
static const int DDGI_MAX_RAYS = 256;

// Pass-local tuning cvars (mirrors the SsaoPass convention of file-scope statics).
idCVar r_ddgiProbeSpacing( "r_ddgiProbeSpacing", "64", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "DDGI probe spacing in world units" );
idCVar r_ddgiRaysPerProbe( "r_ddgiRaysPerProbe", "128", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "rays traced per probe per frame", 32, 256 );
idCVar r_ddgiHysteresis( "r_ddgiHysteresis", "0.97", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "temporal blend weight for probe history [0..1]", 0.0f, 1.0f );
idCVar r_ddgiNormalBias( "r_ddgiNormalBias", "0.25", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "probe sampling normal bias in world units" );

DdgiPass::DdgiPass( nvrhi::IDevice* device, CommonRenderPasses* commonPasses )
	: m_Device( device )
	, m_CommonPasses( commonPasses )
	, m_TraceBoundTlas( nullptr )
	, m_FrameIndex( 0 )
	, rayTracingSupported( false )
	, loggedFirstBuild( false )
{
	m_ProbeCounts[0] = 32;
	m_ProbeCounts[1] = 8;
	m_ProbeCounts[2] = 32;

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
		nvrhi::BindingLayoutItem::VolatileConstantBuffer( 1 ),	// b1 : DdgiConstants
		nvrhi::BindingLayoutItem::StructuredBuffer_UAV( 0 ),	// u0 : per-ray radiance
	};
	m_TraceBindingLayout = m_Device->createBindingLayout( layoutDesc );

	nvrhi::ComputePipelineDesc pipelineDesc;
	pipelineDesc.bindingLayouts = { m_TraceBindingLayout };
	pipelineDesc.CS = m_TraceShader;
	m_TracePipeline = m_Device->createComputePipeline( pipelineDesc );

	const int maxProbes = m_ProbeCounts[0] * m_ProbeCounts[1] * m_ProbeCounts[2];

	nvrhi::BufferDesc bufferDesc;
	bufferDesc.byteSize = ( uint64_t )maxProbes * DDGI_MAX_RAYS * sizeof( float ) * 4;
	bufferDesc.structStride = sizeof( float ) * 4;			// float4 per (probe, ray)
	bufferDesc.canHaveUAVs = true;
	bufferDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
	bufferDesc.keepInitialState = true;
	bufferDesc.debugName = "DDGI/RayRadiance";
	m_RayRadianceBuffer = m_Device->createBuffer( bufferDesc );
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

	commandList->writeBuffer( m_ConstantBuffer, &constants, sizeof( constants ) );

	// the TLAS handle changes when it is recreated to grow; rebuild the binding
	// set to point at the current one.
	nvrhi::rt::IAccelStruct* tlas = m_AccelStructs.GetTLAS();
	if( m_TraceBindingSet == nullptr || m_TraceBoundTlas != tlas )
	{
		nvrhi::BindingSetDesc setDesc;
		setDesc.bindings =
		{
			nvrhi::BindingSetItem::RayTracingAccelStruct( 0, tlas ),
			nvrhi::BindingSetItem::ConstantBuffer( 1, m_ConstantBuffer ),
			nvrhi::BindingSetItem::StructuredBuffer_UAV( 0, m_RayRadianceBuffer ),
		};
		m_TraceBindingSet = m_Device->createBindingSet( setDesc, m_TraceBindingLayout );
		m_TraceBoundTlas = tlas;
	}

	nvrhi::ComputeState state;
	state.pipeline = m_TracePipeline;
	state.bindings = { m_TraceBindingSet };
	commandList->setComputeState( state );

	// one thread per ray (x), one row per probe (y)
	const int groupsX = ( raysPerProbe + 31 ) / 32;
	commandList->dispatch( groupsX, totalProbes, 1 );
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
