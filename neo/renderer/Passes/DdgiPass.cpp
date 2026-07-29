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

// Pass-local tuning cvars (mirrors the SsaoPass convention of file-scope statics).
idCVar r_ddgiProbeSpacing( "r_ddgiProbeSpacing", "64", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "DDGI probe spacing in world units" );
idCVar r_ddgiRaysPerProbe( "r_ddgiRaysPerProbe", "128", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "rays traced per probe per frame", 32, 256 );
idCVar r_ddgiHysteresis( "r_ddgiHysteresis", "0.97", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "temporal blend weight for probe history [0..1]", 0.0f, 1.0f );
idCVar r_ddgiNormalBias( "r_ddgiNormalBias", "0.25", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "probe sampling normal bias in world units" );

DdgiPass::DdgiPass( nvrhi::IDevice* device, CommonRenderPasses* commonPasses )
	: m_Device( device )
	, m_CommonPasses( commonPasses )
	, rayTracingSupported( false )
	, loggedFirstBuild( false )
{
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
	// M3: integrate rays into the octahedral irradiance + distance atlases and
	//     temporally blend with r_ddgiHysteresis.
}
