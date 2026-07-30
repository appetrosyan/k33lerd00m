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
class idImage;
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
	// Build the octahedral irradiance/distance atlases + integrate pipeline (M3).
	void			CreateIntegratePass();
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
	nvrhi::IBuffer*					m_TraceBoundInstanceData;	// instance-data buffer the set was built against

	// per-frame projected-light buffer (M4 stage 2), bound as a StructuredBuffer SRV
	nvrhi::BufferHandle				m_LightBuffer;
	int								m_LightCapacity;

	// linear-clamp sampler for reading the irradiance/distance atlases back into
	// the trace pass for multi-bounce (M4 stage 3)
	nvrhi::SamplerHandle			m_LinearSampler;

public:
	// The octahedral irradiance atlas as an idImage, so the ambient pass can Bind()
	// it in place of the baked light grid and the debug overlay can blit it.
	idImage*		GetIrradianceImage() const
	{
		return m_IrradianceImage;
	}

	// Camera-anchored probe volume for this frame, so the ambient pass can sample
	// the DDGI atlas in place of the baked light grid (M4 stage 4).
	const idVec3&	GetVolumeOrigin() const
	{
		return m_VolumeOrigin;
	}
	float			GetVolumeSpacing() const
	{
		return m_VolumeSpacing;
	}
	int				GetProbeCount( int axis ) const
	{
		return m_ProbeCounts[axis];
	}

private:
	// probe integrate pass (M3): octahedral atlases the trace radiance folds into.
	// idImages so the ambient pass can sample them through the normal bind path.
	idImage*						m_IrradianceImage;		// rgb irradiance per probe texel
	idImage*						m_DistanceImage;		// mean, mean^2 distance
	nvrhi::ShaderHandle				m_IntegrateShader;
	nvrhi::BindingLayoutHandle		m_IntegrateBindingLayout;
	nvrhi::BindingSetHandle			m_IntegrateBindingSet;
	nvrhi::ComputePipelineHandle	m_IntegratePipeline;

	// octahedral border copy pass (M4 stage 3): seamless bilinear across tile edges
	nvrhi::ShaderHandle				m_BorderShader;
	nvrhi::BindingLayoutHandle		m_BorderBindingLayout;
	nvrhi::BindingSetHandle			m_BorderBindingSet;
	nvrhi::ComputePipelineHandle	m_BorderPipeline;

	int						m_ProbeCounts[3];
	idVec3					m_VolumeOrigin;		// snapped camera-anchored origin this frame
	float					m_VolumeSpacing;	// world units between probes
	int						m_FrameIndex;

	bool					rayTracingSupported;
	bool					loggedFirstBuild;
};

#endif
