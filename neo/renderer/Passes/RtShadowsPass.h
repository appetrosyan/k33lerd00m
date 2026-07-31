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

	nvrhi::DeviceHandle		m_Device;
	CommonRenderPasses*		m_CommonPasses;

	nvrhi::BufferHandle		m_ConstantBuffer;

	// self-contained TLAS builder (shared class with DdgiPass, own instance)
	DdgiAccelStructures		m_AccelStructs;

	nvrhi::ShaderHandle				m_TraceShader;
	nvrhi::BindingLayoutHandle		m_TraceBindingLayout;
	nvrhi::BindingSetHandle			m_TraceBindingSet;
	nvrhi::ComputePipelineHandle	m_TracePipeline;

	// the binding set is rebuilt when the TLAS grows or the mask image resizes
	nvrhi::rt::IAccelStruct*	m_BoundTlas;
	idImage*					m_BoundMask;

	bool					m_ViewReady;
	bool					rayTracingSupported;
	bool					loggedFirstBuild;
};

#endif
