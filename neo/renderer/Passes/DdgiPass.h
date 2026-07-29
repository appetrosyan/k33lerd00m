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
#ifndef RENDERER_PASSES_DDGIPASS_H_
#define RENDERER_PASSES_DDGIPASS_H_

#include "DdgiAccelStructures.h"

/*
================================================================================

	DdgiPass - Dynamic Diffuse Global Illumination (Majercik/McGuire 2019).

	Ray-traced irradiance probes that replace the offline-baked light grid with
	a GPU update every frame. The probe volume reuses the existing octahedral
	irradiance atlas layout (LIGHTGRID_IRRADIANCE_SIZE) so the ambient sampling
	shader (builtin/lighting/ambient_lightgrid_IBL.ps.hlsl) can read it without
	change once M4 lands.

	See DdgiPass_DESIGN.md for the milestone plan. This is M0: scaffold only -
	no acceleration structures, no ray dispatch, no behavior change. Render()
	early-outs until the RT path is built in M1-M3.

================================================================================
*/

class idRenderBackend;
struct viewDef_t;

class DdgiPass
{
public:
	DdgiPass( nvrhi::IDevice* device, CommonRenderPasses* commonPasses );
	~DdgiPass();

	// True only when the device advertises ray tracing / ray query support.
	// M1+ needs this before creating any acceleration structure or RT pipeline.
	bool			IsSupported() const
	{
		return rayTracingSupported;
	}

	// Per-frame entry point. Rebuilds the acceleration structures and, once RT
	// is available, traces the probe rays. Later: integrate + blend atlases.
	void			Render( nvrhi::ICommandList* commandList, const viewDef_t* viewDef );

private:
	// Build the probe trace compute pipeline + ray-radiance buffer (M2).
	void			CreateTracePass();
	// Fill the constants for a camera-centred probe volume and dispatch the trace.
	void			DispatchProbeTrace( nvrhi::ICommandList* commandList, const viewDef_t* viewDef );

	nvrhi::DeviceHandle		m_Device;
	CommonRenderPasses*		m_CommonPasses;

	nvrhi::BufferHandle		m_ConstantBuffer;

	DdgiAccelStructures		m_AccelStructs;

	// probe trace compute pass (M2)
	nvrhi::BufferHandle				m_RayRadianceBuffer;
	nvrhi::ShaderHandle				m_TraceShader;
	nvrhi::BindingLayoutHandle		m_TraceBindingLayout;
	nvrhi::BindingSetHandle			m_TraceBindingSet;
	nvrhi::ComputePipelineHandle	m_TracePipeline;
	nvrhi::rt::IAccelStruct*		m_TraceBoundTlas;		// TLAS the binding set was built against

	int						m_ProbeCounts[3];
	int						m_FrameIndex;

	bool					rayTracingSupported;
	bool					loggedFirstBuild;
};

#endif
