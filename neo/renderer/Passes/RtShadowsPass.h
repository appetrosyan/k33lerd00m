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
#ifndef RENDERER_PASSES_RTSHADOWSPASS_H_
#define RENDERER_PASSES_RTSHADOWSPASS_H_

#include "DdgiAccelStructures.h"

/*
================================================================================

	RtShadowsPass - ray-traced hard shadows.

	An alternative to shadow maps. For each shadow-casting light the pass runs one
	compute thread per screen pixel: it reconstructs the pixel's world position from
	the depth buffer, offsets along the gbuffer normal, and traces an inline
	visibility ray toward the light against the world TLAS. A hit means occluded; the
	visibility fraction is written into a screen-space R8 mask (globalImages->
	rtShadowMaskImage). The forward interaction shader then Load()s that mask in place
	of the shadow-map PCF term - the light-space projection / atlas machinery is
	bypassed entirely.

	Pixel-exact hard edges (like the stencil shadow volumes the game shipped with),
	no depth bias acne, no shadow-map resolution shimmer, and temporally stable
	(deterministic single-frame, no reprojection). Cost scales with pixels + BVH
	traversal, not shadow-volume overdraw.

	Self-contained: owns its own DdgiAccelStructures so it works with DDGI / RT
	reflections off. When several RT passes are enabled the TLAS is built more than
	once per frame - accepted, matching ReflectionsPass.

	Gated on r_useRTShadows and on device ray query support. Parallel (sun) lights
	are left on the shadow-map path for now (their world direction is not plumbed
	here); point and projected/spot lights use ray-traced visibility.

================================================================================
*/

class idImage;
struct viewDef_t;
struct viewLight_t;

class RtShadowsPass
{
public:
	RtShadowsPass( nvrhi::IDevice* device, CommonRenderPasses* commonPasses );
	~RtShadowsPass();

	bool			IsSupported() const
	{
		return rayTracingSupported;
	}

	// Rebuild the acceleration structure from the visible static world. Call once per
	// view before the light loop; returns true when the TLAS is ready to trace against.
	bool			BeginView( nvrhi::ICommandList* commandList, const viewDef_t* viewDef );

	// Trace visibility for one light into the shadow mask. Requires a successful
	// BeginView this frame. Returns false (mask untouched) when unsupported / not ready.
	bool			RenderLight( nvrhi::ICommandList* commandList, const viewDef_t* viewDef, const viewLight_t* vLight );

private:
	void			CreateTracePass();
	void			CreateDenoisePass();

	nvrhi::DeviceHandle		m_Device;
	CommonRenderPasses*		m_CommonPasses;

	nvrhi::BufferHandle		m_ConstantBuffer;

	// self-contained TLAS builder (shared class with DdgiPass, own instance)
	DdgiAccelStructures		m_AccelStructs;

	nvrhi::ShaderHandle				m_TraceShader;
	nvrhi::BindingLayoutHandle		m_TraceBindingLayout;
	nvrhi::ComputePipelineHandle	m_TracePipeline;

	// Two binding sets sharing one pipeline/layout (see r_rtShadowCoarse / RenderLight). The legacy
	// single dispatch (passMode 0) and the refine dispatch (passMode 2) both write the FINAL mask
	// through u0 and have the coarse image bound at t3 (legacy never reads it - harmless dummy;
	// refine does) - identical bindings, so one set covers both. The coarse dispatch (passMode 1)
	// writes the coarse image through u0 instead, with the final mask as its (unread) dummy SRV -
	// a resource cannot be bound as both UAV and SRV in the same set, hence the swap.
	nvrhi::BindingSetHandle			m_MaskBindingSet;
	nvrhi::BindingSetHandle			m_CoarseBindingSet;

	// soft-shadow penumbra denoise: the trace writes the RAW noisy mask, this pass filters
	// it into the final mask the interaction reads (per light, over its scissor rect).
	idImage*						m_RawMaskImage;
	nvrhi::BindingSetHandle			m_RawMaskBindingSet;	// trace variant: u0 = raw mask
	nvrhi::ShaderHandle				m_DenoiseShader;
	nvrhi::BindingLayoutHandle		m_DenoiseBindingLayout;
	nvrhi::ComputePipelineHandle	m_DenoisePipeline;
	nvrhi::BindingSetHandle			m_DenoiseBindingSet;
	nvrhi::BufferHandle				m_DenoiseConstantBuffer;

	// the binding sets are rebuilt when the TLAS grows or an image handle changes
	nvrhi::rt::IAccelStruct*	m_BoundTlas;
	idImage*					m_BoundMask;
	idImage*					m_BoundCoarse;
	idImage*					m_BoundRawMask;

	bool					m_ViewReady;
	bool					rayTracingSupported;
	bool					loggedFirstBuild;
};

#endif
