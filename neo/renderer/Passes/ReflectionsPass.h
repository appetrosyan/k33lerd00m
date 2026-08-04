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
#ifndef RENDERER_PASSES_REFLECTIONSPASS_H_
#define RENDERER_PASSES_REFLECTIONSPASS_H_

#include "DdgiAccelStructures.h"

/*
================================================================================

	ReflectionsPass - hybrid ray-traced mirror reflections.

	Reuses the DDGI ray tracing acceleration structures (as a pure TLAS builder)
	to trace one reflection ray per screen pixel. The ray finds the hit geometry
	- including geometry the camera cannot see - and the hit is reprojected to the
	screen so the already-lit scene colour can be sampled there ("hybrid": the
	reflection colour comes from the shaded framebuffer, not a re-shade). The pass
	composites in place and writes the blended result to its own image; the backend
	blits that back over the HDR scene target.

	Self-contained: owns its own DdgiAccelStructures so it works with r_useDDGI off
	(DDGI is shelved). When both are enabled the TLAS is built twice - accepted, as
	they are not used together in practice.

	Gated on r_useRTReflections and on device ray query support.

================================================================================
*/

class idImage;
struct viewDef_t;

class ReflectionsPass
{
public:
	ReflectionsPass( nvrhi::IDevice* device, CommonRenderPasses* commonPasses );
	~ReflectionsPass();

	bool			IsSupported() const
	{
		return rayTracingSupported;
	}

	// Per-frame entry point. Rebuilds the acceleration structures from the static
	// world, then traces + composites reflections into m_ReflectionImage. Returns
	// true when the image holds a valid composited frame to blit back.
	bool			Render( nvrhi::ICommandList* commandList, const viewDef_t* viewDef );

	// The composited (base lit colour + denoised reflection) image, blitted back over
	// the HDR scene target by the backend. This is the denoise pass output.
	idImage*		GetReflectionImage() const
	{
		return m_DenoisedImage;
	}

	// Ember dissolve seeds from the posed-vertex pool this pass just built.
	DdgiAccelStructures&	GetAccelStructures()
	{
		return m_AccelStructs;
	}

private:
	void			CreateTracePass();
	void			CreateDenoisePass();
	// (Re)create the screen-sized output images when the render resolution changes.
	void			EnsureReflectionImage( int width, int height );

	nvrhi::DeviceHandle		m_Device;
	CommonRenderPasses*		m_CommonPasses;

	nvrhi::BufferHandle		m_ConstantBuffer;

	// self-contained TLAS builder (shared class with DdgiPass, own instance)
	DdgiAccelStructures		m_AccelStructs;

	nvrhi::ShaderHandle				m_TraceShader;
	nvrhi::BindingLayoutHandle		m_TraceBindingLayout;
	nvrhi::BindingSetHandle			m_TraceBindingSet;
	nvrhi::ComputePipelineHandle	m_TracePipeline;
	nvrhi::SamplerHandle			m_LinearSampler;	// clamp: env probe + gbuffers
	nvrhi::SamplerHandle			m_MaterialSampler;	// wrap+aniso: bindless material textures

	// per-frame projected-light buffer for the world-space hit re-shade (mirrors
	// DdgiPass's light buffer; reflections build their own so they work with DDGI off).
	nvrhi::BufferHandle				m_LightBuffer;

	// spatial denoise + composite (reads the raw reflection m_ReflectionImage, writes the
	// composited m_DenoisedImage that the backend blits)
	nvrhi::ShaderHandle				m_DenoiseShader;
	nvrhi::BindingLayoutHandle		m_DenoiseBindingLayout;
	nvrhi::BindingSetHandle			m_DenoiseBindingSet;
	nvrhi::ComputePipelineHandle	m_DenoisePipeline;
	nvrhi::BufferHandle				m_DenoiseConstantBuffer;
	idImage*						m_DenoisedImage;

	// the binding set is rebuilt when any of these change (TLAS grows / image resizes /
	// instance-data buffer grows)
	nvrhi::rt::IAccelStruct*	m_BoundTlas;
	nvrhi::IBuffer*				m_BoundInstanceData;
	idImage*					m_ReflectionImage;
	int							m_ImageWidth;
	int							m_ImageHeight;

	bool					rayTracingSupported;
	bool					loggedFirstBuild;
};

#endif
