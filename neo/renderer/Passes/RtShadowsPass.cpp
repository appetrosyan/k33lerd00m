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

#include "RtShadowsPass.h"
#include "RtShadowsPass_cb.h"

// Main toggle lives with the other renderer cvars in RenderSystem_init.cpp.
extern idCVar r_useRTShadows;

// Pass-local tuning cvars (mirrors the ReflectionsPass convention of file-scope statics).
idCVar r_rtShadowBias( "r_rtShadowBias", "1.5", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "RT shadows: ray origin bias along the surface normal (self-intersection)" );
idCVar r_rtShadowRays( "r_rtShadowRays", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER | CVAR_NEW, "RT shadows: visibility rays per pixel (1 = hard shadow; >1 = brute-force soft, spatial-denoised)", 1, 16 );
idCVar r_rtShadowDenoise( "r_rtShadowDenoise", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL | CVAR_NEW, "RT shadows: spatial edge-aware denoise of the soft-shadow penumbra (only active with r_rtShadowRays > 1)" );
idCVar r_rtShadowDenoiseRadius( "r_rtShadowDenoiseRadius", "6", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT | CVAR_NEW, "RT shadows: soft-shadow penumbra denoise radius in pixels (0 = off)", 0.0f, 16.0f );
idCVar r_rtShadowDenoiseDepthSigma( "r_rtShadowDenoiseDepthSigma", "0.001", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "RT shadows: penumbra denoise depth edge-stop sigma (hardware-depth units)" );

// C++ side of the shadow-denoise constant buffer; must match ShadowDenoiseConstants in rt_shadow_denoise.cs.hlsl.
struct ShadowDenoiseConstants
{
	idVec2i	screenSize;
	idVec2i	scissorMin;
	idVec2i	scissorMax;
	idVec2i	pad0;
	idVec4	params;		// x = radius, y = depthSigma
};
idCVar r_rtShadowSoftRadius( "r_rtShadowSoftRadius", "12.0", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "RT shadows: light radius in world units used for soft penumbra when r_rtShadowRays > 1" );
idCVar r_rtShadowBackfaceCull( "r_rtShadowBackfaceCull", "0", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "RT shadows ray face culling (debug). 0 = two-sided, occlude regardless of facing (default; matches stencil, no facing seam on single-sided world geometry); 1 = cull back faces; 2 = cull front faces. NOT archived - two-sided is the correct default.", 0, 2 );
// Keep lights that portal-culling would drop (a light reachable only through a hidden
// window). Default OFF: it also pulls in lights that then over-illuminate rooms they
// should not reach, which read as "bright where it should be dark". Gate for tr_frontend.
idCVar r_rtShadowExtraLights( "r_rtShadowExtraLights", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "RT shadows: keep shadow-casting lights in view even when their frustum misses the visible portal chain. 0 = original light culling (no phantom illumination); 1 = keep them (fixes shadows from lights only reachable through a window, but can over-light)" );
idCVar r_rtShadowUmbra( "r_rtShadowUmbra", "0.15", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT | CVAR_NEW, "RT shadows: minimum shadow term (umbra light floor). RT visibility is binary and pitch-black; a small floor gives back the shipped stencil shadows' gentler darkening without softening the edge. 0 = hard black umbra", 0.0f, 1.0f );
idCVar r_rtShadowForce( "r_rtShadowForce", "0", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "RT shadows debug: 0 = normal, 1 = force the whole mask fully shadowed (black), 2 = force fully lit, 3 = hit distance, 4 = primary-ray TLAS coverage probe (green=surface in TLAS, blue=occluded, red=MISSING). If forcing 1 does NOT darken an area, that area is not lit by an RT-shadowed light - its brightness comes from ambient / DDGI / a non-RT light. 5/6 = bisection: plain binary occlusion, generous t-range, 5 toward light / 6 straight up.", 0, 6 );
idCVar r_rtShadowShowMask( "r_rtShadowShowMask", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "RT shadows debug: blit the last light's visibility mask over the scene (red = lit, dark = shadowed) to check its spatial registration against the lit geometry" );
// Build the shadow TLAS from real shadow casters only (honour noShadows, drop translucent
// glass). Default ON so RT shadows match the stencil caster set. Toggle OFF to confirm the
// mechanism: the stipple / spurious shadows from grates, railings, decals and window glass
// come back, because those surfaces then re-enter the shadow acceleration structure.
idCVar r_rtShadowCasterFilter( "r_rtShadowCasterFilter", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "RT shadows: restrict the shadow TLAS to real casters (skip noShadows + translucent surfaces). 0 = add all static geometry (spurious detail shadows / glass blocks light)" );
// Perf: the shadow TLAS omits the wide 94-area frontend flood-occluder gather (thousands of
// instances/frame). The per-light shadow-caster chains (vLight->globalShadows/localShadows)
// already carry every occluder that shadows a visible light, incl. off-frustum ones, so for
// shadows the flood is redundant - dropping it shrinks the TLAS from thousands to hundreds
// (faster build AND faster rays). 0 restores the flood if a missing off-view shadow appears.
idCVar r_rtShadowSkipWorldFlood( "r_rtShadowSkipWorldFlood", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "RT shadows: skip the wide 94-area flood-occluder gather in the shadow TLAS (per-light casters cover shadows). 0 = keep the flood (slower; only needed if an off-view shadow is missing)" );
// Perf: dispatch the visibility trace over only the light's screen-space scissor rect
// instead of the whole framebuffer. A light that touches a small screen region then traces
// rays for just those pixels (its interactions are scissored to the same rect anyway).
idCVar r_rtShadowScissor( "r_rtShadowScissor", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "RT shadows: trace only each light's screen scissor rect (perf). 0 = full-screen dispatch per light" );
// Perf: the shadow mask is piecewise-constant (flat lit/umbra interiors + thin boundaries), so
// trace it COARSE first, then refine with a full-res ray only where the coarse mask disagrees
// (a boundary is nearby) - interiors just upsample, no ray. This decouples the shadow ray budget
// from render (SSAA) resolution: the coarse divisor is scaled by R_SSAAScale() so it stays a fixed
// fraction of DISPLAY resolution regardless of supersampling (see r_rtShadowCoarseDiv). Forced off
// (legacy single dispatch) whenever a debug force mode or soft shadows (rays > 1) are active - see
// RenderLight.
idCVar r_rtShadowCoarse( "r_rtShadowCoarse", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER | CVAR_NEW, "RT shadows: 0 = single full-res dispatch (default); 1 = coarse-only upsample (soft edges, tests the coarse plumbing); 2 = coarse + full-res edge refine (perf, sharp edges). Forced to 0 when r_rtShadowForce != 0 or r_rtShadowRays > 1.", 0, 2 );
idCVar r_rtShadowCoarseDiv( "r_rtShadowCoarseDiv", "4", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER | CVAR_NEW, "RT shadows: coarse trace is renderRes / round(this * SSAAScale) - e.g. 4 gives 1/4 res at SSAA 1x, 1/6 at 1.5x, 1/8 at 2x, so the shadow ray budget stays a fixed fraction of DISPLAY res. Live - no vid_restart needed.", 2, 16 );
// Lossless ray-count cut: skip the visibility ray for receivers outside the light's projection
// volume (baseLightProject [0,1] cube). The interaction's falloff/cookie is zero there, so the
// shadow value is discarded regardless - tracing it was pure waste. Culls the scissor-rect corners
// a point light's sphere / a spot light's cone never fill. Archived on; toggle to A/B visually.
idCVar r_rtShadowVolumeCull( "r_rtShadowVolumeCull", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL | CVAR_NEW, "RT shadows: skip rays for receivers outside the light's projection volume (lossless - the interaction gives zero light there). 0 = trace the whole scissor rect." );
// TLAS trim: let RT honour the same shadow-bounds-vs-frustum cull stencil uses (tr_frontend_addmodels),
// instead of keeping EVERY off-view caster. A caster that shadows an in-view receiver still has that
// receiver inside its shadowBounds -> intersects the frustum -> kept (incl. behind-camera casters), so
// this should be lossless while dropping casters whose shadow lands nowhere visible. 0 = keep all
// off-view casters (the previous behaviour) if a missing off-view shadow shows up.
idCVar r_rtShadowCullOffView( "r_rtShadowCullOffView", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL | CVAR_NEW, "RT shadows: cull casters whose shadow-bounds miss the view frustum (shrinks the shadow TLAS). 0 = keep every off-view caster." );
// Skip the ray for receivers whose (bumped) shading normal faces away from the light: diffuse is
// max(0,N.L)=0 there and specular ~0, so the interaction's light term is zero regardless of the
// shadow value. Uses the gbuffer normal (same one the interaction lights with) so it matches - the
// only caveat is materials that write explicit vertexColor (the gbuffer stores normal*vertexColor),
// a rare minority. Toggle off if such a surface shows missing light.
idCVar r_rtShadowFacingCull( "r_rtShadowFacingCull", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL | CVAR_NEW, "RT shadows: skip rays for receivers facing away from the light (zero diffuse there). 0 = trace regardless of facing." );
extern idCVar r_useScissor;

RtShadowsPass::RtShadowsPass( nvrhi::IDevice* device, CommonRenderPasses* commonPasses )
	: m_Device( device )
	, m_CommonPasses( commonPasses )
	, m_RawMaskImage( nullptr )
	, m_BoundTlas( nullptr )
	, m_BoundMask( nullptr )
	, m_BoundCoarse( nullptr )
	, m_BoundRawMask( nullptr )
	, m_ViewReady( false )
	, rayTracingSupported( false )
	, loggedFirstBuild( false )
{
	m_AccelStructs.Init( device );

	// Inline ray query from a compute shader needs both RayQuery and acceleration
	// structure support; everything below is gated on this.
	rayTracingSupported =
		m_Device->queryFeatureSupport( nvrhi::Feature::RayQuery ) &&
		m_Device->queryFeatureSupport( nvrhi::Feature::RayTracingAccelStruct );

	// Unlike the other RT passes, RenderLight writes this volatile buffer ONCE (legacy/coarse-only)
	// or TWICE (coarse+refine, r_rtShadowCoarse 2) PER LIGHT (each shadow-casting point/spot re-
	// fills the light origin before each of its dispatches), so the version ring must cover every
	// such write in a frame - not just one per light. 16 (c_MaxRenderPassConstantBufferVersions)
	// overflows on busy maps and nvrhi fatals; 2048 keeps the original ~1024-light headroom now
	// that coarse+refine can consume two versions per light instead of one.
	nvrhi::BufferDesc constantBufferDesc;
	constantBufferDesc.byteSize = sizeof( RtShadowConstants );
	constantBufferDesc.debugName = "RtShadowConstants";
	constantBufferDesc.isConstantBuffer = true;
	constantBufferDesc.isVolatile = true;
	constantBufferDesc.maxVersions = 2048;
	m_ConstantBuffer = m_Device->createBuffer( constantBufferDesc );

	if( rayTracingSupported )
	{
		common->Printf( "RtShadowsPass: ray tracing supported, RT shadows available.\n" );
		CreateTracePass();
		CreateDenoisePass();
	}
	else
	{
		common->Warning( "RtShadowsPass: device has no ray query / acceleration structure support - RT shadows disabled." );
	}
}

RtShadowsPass::~RtShadowsPass()
{
	m_AccelStructs.Shutdown();
}

/*
========================
RtShadowsPass::CreateTracePass
========================
*/
void RtShadowsPass::CreateTracePass()
{
	idList<shaderMacro_t> macros;
	const int shaderIdx = renderProgManager.FindShader( "builtin/reflections/rt_shadows", SHADER_STAGE_COMPUTE, "", macros, true, LAYOUT_DRAW_VERT );
	m_TraceShader = renderProgManager.GetShader( shaderIdx );

	if( m_TraceShader == nullptr )
	{
		common->Warning( "RtShadowsPass: rt_shadows compute shader failed to load (idx %i) - RT shadows disabled.", shaderIdx );
		rayTracingSupported = false;
		return;
	}

	nvrhi::BindingLayoutDesc layoutDesc;
	layoutDesc.visibility = nvrhi::ShaderType::Compute;
	layoutDesc.bindings =
	{
		nvrhi::BindingLayoutItem::RayTracingAccelStruct( 0 ),	// t0 : world TLAS
		nvrhi::BindingLayoutItem::Texture_SRV( 1 ),			// t1 : hardware depth
		nvrhi::BindingLayoutItem::Texture_SRV( 2 ),			// t2 : gbuffer world normals + roughness
		nvrhi::BindingLayoutItem::VolatileConstantBuffer( 1 ),	// b1 : RtShadowConstants
		nvrhi::BindingLayoutItem::Texture_UAV( 0 ),			// u0 : write target (final mask, or the coarse image for passMode 1)
		nvrhi::BindingLayoutItem::Texture_SRV( 3 ),			// t3 : coarse+refine - the OTHER of {mask,coarse} not bound at u0 (dummy/unread except in the refine pass)
	};
	m_TraceBindingLayout = m_Device->createBindingLayout( layoutDesc );

	nvrhi::ComputePipelineDesc pipelineDesc;
	pipelineDesc.bindingLayouts = { m_TraceBindingLayout };
	pipelineDesc.CS = m_TraceShader;
	m_TracePipeline = m_Device->createComputePipeline( pipelineDesc );
}

/*
========================
RtShadowsPass::CreateDenoisePass

Spatial edge-aware scalar denoise of the soft-shadow penumbra. Filters the raw noisy
mask the trace writes into the final mask (per light, over its scissor rect). Non-temporal.
========================
*/
void RtShadowsPass::CreateDenoisePass()
{
	idList<shaderMacro_t> macros;
	const int shaderIdx = renderProgManager.FindShader( "builtin/reflections/rt_shadow_denoise", SHADER_STAGE_COMPUTE, "", macros, true, LAYOUT_DRAW_VERT );
	m_DenoiseShader = renderProgManager.GetShader( shaderIdx );
	if( m_DenoiseShader == nullptr )
	{
		common->Warning( "RtShadowsPass: rt_shadow_denoise compute shader failed to load (idx %i) - soft-shadow denoise disabled.", shaderIdx );
		return;	// hard/soft shadows still work; the penumbra denoise just won't run
	}

	nvrhi::BufferDesc cbDesc;
	cbDesc.byteSize = sizeof( ShadowDenoiseConstants );
	cbDesc.debugName = "RtShadowDenoiseConstants";
	cbDesc.isConstantBuffer = true;
	cbDesc.isVolatile = true;
	cbDesc.maxVersions = 2048;	// one per light per frame, like the trace CB
	m_DenoiseConstantBuffer = m_Device->createBuffer( cbDesc );

	nvrhi::BindingLayoutDesc layoutDesc;
	layoutDesc.visibility = nvrhi::ShaderType::Compute;
	layoutDesc.bindings =
	{
		nvrhi::BindingLayoutItem::Texture_SRV( 0 ),			// t0 : raw mask
		nvrhi::BindingLayoutItem::Texture_SRV( 1 ),			// t1 : depth
		nvrhi::BindingLayoutItem::Texture_SRV( 2 ),			// t2 : gbuffer normals
		nvrhi::BindingLayoutItem::VolatileConstantBuffer( 1 ),	// b1
		nvrhi::BindingLayoutItem::Texture_UAV( 0 ),			// u0 : denoised mask
	};
	m_DenoiseBindingLayout = m_Device->createBindingLayout( layoutDesc );

	nvrhi::ComputePipelineDesc pipelineDesc;
	pipelineDesc.bindingLayouts = { m_DenoiseBindingLayout };
	pipelineDesc.CS = m_DenoiseShader;
	m_DenoisePipeline = m_Device->createComputePipeline( pipelineDesc );
}

/*
========================
RtShadowsPass::BeginView

Rebuild the TLAS from the visible static world once for the whole light loop.
========================
*/
bool RtShadowsPass::BeginView( nvrhi::ICommandList* commandList, const viewDef_t* viewDef )
{
	m_ViewReady = false;

	if( !r_useRTShadows.GetBool() || !rayTracingSupported || viewDef == NULL || m_TracePipeline == nullptr )
	{
		return false;
	}

	// NOTE: do NOT force-generate the coarse image here. Materialising a texture mid-frame
	// (during command-list recording) opens a second command list and crashes - the same
	// anti-pattern the raw-mask image was fixed for. If the coarse image is not resident,
	// coarse+refine simply stays dormant (coarseImageReady is false in RenderLight -> legacy
	// dispatch); it is disabled at r_rtShadowRays > 1 anyway. Re-enabling coarse+refine needs
	// the image materialised safely at init/resize, not here.

	m_AccelStructs.SetShadowCastersOnly( r_rtShadowCasterFilter.GetBool() );
	m_AccelStructs.SetSkipFrontendOccluders( r_rtShadowSkipWorldFlood.GetBool() );
	m_ViewReady = m_AccelStructs.RebuildFromView( commandList, viewDef );

	if( m_ViewReady && !loggedFirstBuild )
	{
		common->Printf( "RtShadowsPass: built TLAS with %i static instances.\n", m_AccelStructs.NumInstances() );
		loggedFirstBuild = true;
	}

	return m_ViewReady;
}

/*
========================
RtShadowsPass::RenderLight
========================
*/
bool RtShadowsPass::RenderLight( nvrhi::ICommandList* commandList, const viewDef_t* viewDef, const viewLight_t* vLight )
{
	if( !m_ViewReady || vLight == NULL )
	{
		return false;
	}

	idImage* mask = globalImages->rtShadowMaskImage;
	if( mask == NULL || mask->GetTextureHandle() == nullptr )
	{
		return false;
	}

	// Coarse image may be briefly unavailable right after a resolution change (image function
	// callbacks run lazily); if so, silently fall back to the legacy single dispatch below rather
	// than failing the light entirely.
	idImage* coarse = globalImages->rtShadowCoarseImage;
	const bool coarseImageReady = ( coarse != NULL && coarse->GetTextureHandle() != nullptr );

	// Run at the render (supersampled) resolution: the mask lines up 1:1 with the
	// depth / gbuffer it reconstructs from and with the interaction pass's SV_Position.
	const int width = renderSystem->GetRenderWidth();
	const int height = renderSystem->GetRenderHeight();
	if( width <= 0 || height <= 0 )
	{
		return false;
	}

	RtShadowConstants constants;
	memset( &constants, 0, sizeof( constants ) );

	const idRenderMatrix& u2w = viewDef->unprojectionToWorldRenderMatrix;
	constants.unprojToWorld0 = idVec4( u2w[0][0], u2w[0][1], u2w[0][2], u2w[0][3] );
	constants.unprojToWorld1 = idVec4( u2w[1][0], u2w[1][1], u2w[1][2], u2w[1][3] );
	constants.unprojToWorld2 = idVec4( u2w[2][0], u2w[2][1], u2w[2][2], u2w[2][3] );
	constants.unprojToWorld3 = idVec4( u2w[3][0], u2w[3][1], u2w[3][2], u2w[3][3] );

	const idVec3 lightOrigin = vLight->globalLightOrigin;
	constants.lightOrigin = idVec4( lightOrigin.x, lightOrigin.y, lightOrigin.z, r_rtShadowSoftRadius.GetFloat() );
	constants.params = idVec4( r_rtShadowBias.GetFloat(),
							   r_rtShadowUmbra.GetFloat(),
							   ( float )r_rtShadowRays.GetInteger(),
							   ( float )( tr.frameCount & 1023 ) );
	constants.screenSize = idVec2i( width, height );
	constants.pad = idVec2i( r_rtShadowBackfaceCull.GetInteger(), r_rtShadowForce.GetInteger() );

	// Light-volume cull: pass baseLightProject rows so the shader can skip rays for receivers
	// outside the light's projection box (see r_rtShadowVolumeCull). Disabled by the debug force
	// modes, which want full-screen coverage. Row-major: c[i] = row_i . (worldP,1).
	const idRenderMatrix& lp = vLight->baseLightProject;
	constants.lightProject0 = idVec4( lp[0][0], lp[0][1], lp[0][2], lp[0][3] );
	constants.lightProject1 = idVec4( lp[1][0], lp[1][1], lp[1][2], lp[1][3] );
	constants.lightProject2 = idVec4( lp[2][0], lp[2][1], lp[2][2], lp[2][3] );
	constants.lightProject3 = idVec4( lp[3][0], lp[3][1], lp[3][2], lp[3][3] );
	const bool debugForce = ( r_rtShadowForce.GetInteger() != 0 );
	constants.pad2 = idVec2i( ( r_rtShadowVolumeCull.GetBool() && !debugForce ) ? 1 : 0,
							  ( r_rtShadowFacingCull.GetBool() && !debugForce ) ? 1 : 0 );

	// Dispatch the trace over ONLY this light's screen-space scissor rect instead of the whole
	// framebuffer - a light that touches 5% of the screen then costs 5% of the rays. The rect is
	// mapped to the mask's pixel space EXACTLY as RenderInteractions maps it for its GL_Scissor and
	// SV_Position mask Load: top-left = ( viewport.x1 + rect.x1, viewport.y2 - rect.y2 ), size =
	// ( rect.x2+1-rect.x1, rect.y2+1-rect.y1 ). The shader writes u_ShadowMask[dispatchID+scissorMin],
	// so those pixels line up 1:1 with the fragments that sample them. Using viewport.y2 - rect.y2
	// (NOT the old height-1-y2, which assumed a full-height viewport) is what makes it correct when
	// the viewport is offset/supersampled - the misalignment that sank the earlier attempt. Safe
	// with the shared mask because trace and draw are interleaved per light: each light traces its
	// rect, then draws its (equally scissored) interactions, before the next light overwrites.
	int dispX = 0, dispY = 0, dispW = width, dispH = height;
	if( r_rtShadowScissor.GetBool() && !vLight->scissorRect.IsEmpty() )
	{
		const idScreenRect& s = vLight->scissorRect;
		const idScreenRect& vp = viewDef->viewport;
		dispX = vp.x1 + s.x1;
		dispY = vp.y2 - s.y2;
		dispW = s.x2 + 1 - s.x1;
		dispH = s.y2 + 1 - s.y1;

		// clamp into the mask so a stray rect never dispatches negative/out-of-bounds threads
		// (the shader also drops pixel >= screenSize, but scissorMin must not go negative).
		if( dispX < 0 )		{	dispW += dispX; dispX = 0;	}
		if( dispY < 0 )		{	dispH += dispY; dispY = 0;	}
		if( dispX + dispW > width )	{	dispW = width - dispX;	}
		if( dispY + dispH > height )	{	dispH = height - dispY;	}
		if( dispW <= 0 || dispH <= 0 )
		{
			return false;	// rect fully off the mask - nothing to trace
		}
	}
	constants.scissorMin = idVec2i( dispX, dispY );
	const idVec3& eye = viewDef->renderView.vieworg;
	constants.cameraOrigin = idVec4( eye.x, eye.y, eye.z, 0.0f );

	// Depth-bounds cull: skip rays for pixels outside this light's volume depth extent - they get
	// zero light from its falloff, so the shadow mask there is unused. r_rtShadowScissor gates it
	// with the scissor (same "trace only what matters" optimisation); zmin>=zmax disables in-shader.
	if( r_rtShadowScissor.GetBool() )
	{
		constants.lightDepthBounds = idVec4( vLight->scissorRect.zmin, vLight->scissorRect.zmax, 0.0f, 0.0f );
	}

	// the TLAS handle changes when it is recreated to grow; rebuild the binding sets
	// (also invalidated when either image handle changes) to track current resources.
	nvrhi::rt::IAccelStruct* tlas = m_AccelStructs.GetTLAS();
	if( tlas == nullptr )
	{
		// An empty TLAS (no instances built this view) leaves a null accel structure;
		// creating a binding set with it fatals ("binding set slot 0 does not match the
		// layout"). Skip the dispatch instead of crashing, and say so once.
		static bool warned = false;
		if( !warned )
		{
			common->Warning( "RtShadowsPass: TLAS is empty (no shadow-caster instances) - skipping RT shadow dispatch. Check r_rtAccelDebug / r_rtShadowCasterFilter." );
			warned = true;
		}
		return false;
	}

	// Two binding sets share the same TLAS/depth/normal/CB bindings and only swap which of
	// {mask, coarse} is the UAV write target vs. the (possibly unread) SRV - see RtShadowsPass.h.
	// coarseTexture may be null right after a resolution change; bind the mask itself as the dummy
	// SRV in that case (never read by the shader when the coarse image isn't ready - two-pass mode
	// is disabled below whenever !coarseImageReady, so m_CoarseBindingSet is simply left unused).
	if( m_MaskBindingSet == nullptr || m_BoundTlas != tlas || m_BoundMask != mask || m_BoundCoarse != coarse )
	{
		nvrhi::TextureHandle coarseSRV = coarseImageReady ? coarse->GetTextureHandle() : mask->GetTextureHandle();

		nvrhi::BindingSetDesc maskSetDesc;
		maskSetDesc.bindings =
		{
			nvrhi::BindingSetItem::RayTracingAccelStruct( 0, tlas ),
			nvrhi::BindingSetItem::Texture_SRV( 1, globalImages->currentDepthImage->GetTextureHandle() ),
			nvrhi::BindingSetItem::Texture_SRV( 2, globalImages->gbufferNormalsRoughnessImage->GetTextureHandle() ),
			nvrhi::BindingSetItem::ConstantBuffer( 1, m_ConstantBuffer ),
			nvrhi::BindingSetItem::Texture_UAV( 0, mask->GetTextureHandle() ),
			nvrhi::BindingSetItem::Texture_SRV( 3, coarseSRV ),
		};
		m_MaskBindingSet = m_Device->createBindingSet( maskSetDesc, m_TraceBindingLayout );

		if( coarseImageReady )
		{
			nvrhi::BindingSetDesc coarseSetDesc;
			coarseSetDesc.bindings =
			{
				nvrhi::BindingSetItem::RayTracingAccelStruct( 0, tlas ),
				nvrhi::BindingSetItem::Texture_SRV( 1, globalImages->currentDepthImage->GetTextureHandle() ),
				nvrhi::BindingSetItem::Texture_SRV( 2, globalImages->gbufferNormalsRoughnessImage->GetTextureHandle() ),
				nvrhi::BindingSetItem::ConstantBuffer( 1, m_ConstantBuffer ),
				nvrhi::BindingSetItem::Texture_UAV( 0, coarse->GetTextureHandle() ),
				nvrhi::BindingSetItem::Texture_SRV( 3, mask->GetTextureHandle() ),
			};
			m_CoarseBindingSet = m_Device->createBindingSet( coarseSetDesc, m_TraceBindingLayout );
		}
		else
		{
			m_CoarseBindingSet = nullptr;
		}

		// soft-shadow penumbra denoise: (re)create the raw mask + the trace-to-raw and
		// raw-to-final binding sets. The raw image is only regenerated on a resolution
		// change (mask handle change), not on every TLAS growth.
		if( m_DenoisePipeline != nullptr )
		{
			// raw mask is a global image (created by the image system at init/resize, NOT
			// mid-frame - generating a texture during command-list recording opens a second
			// command list and crashes). Its handle changes with the mask handle on resize,
			// which is exactly when this rebuild block runs (m_BoundMask != mask).
			idImage* const rawMask = globalImages->rtShadowMaskRawImage;

			// trace variant: same bindings as the mask set but writes the RAW mask at u0
			nvrhi::BindingSetDesc rawSetDesc;
			rawSetDesc.bindings =
			{
				nvrhi::BindingSetItem::RayTracingAccelStruct( 0, tlas ),
				nvrhi::BindingSetItem::Texture_SRV( 1, globalImages->currentDepthImage->GetTextureHandle() ),
				nvrhi::BindingSetItem::Texture_SRV( 2, globalImages->gbufferNormalsRoughnessImage->GetTextureHandle() ),
				nvrhi::BindingSetItem::ConstantBuffer( 1, m_ConstantBuffer ),
				nvrhi::BindingSetItem::Texture_UAV( 0, rawMask->GetTextureHandle() ),
				nvrhi::BindingSetItem::Texture_SRV( 3, mask->GetTextureHandle() ),	// coarse dummy (unread in the soft/legacy path)
			};
			m_RawMaskBindingSet = m_Device->createBindingSet( rawSetDesc, m_TraceBindingLayout );

			// denoise: raw mask -> final mask
			nvrhi::BindingSetDesc dnSetDesc;
			dnSetDesc.bindings =
			{
				nvrhi::BindingSetItem::Texture_SRV( 0, rawMask->GetTextureHandle() ),
				nvrhi::BindingSetItem::Texture_SRV( 1, globalImages->currentDepthImage->GetTextureHandle() ),
				nvrhi::BindingSetItem::Texture_SRV( 2, globalImages->gbufferNormalsRoughnessImage->GetTextureHandle() ),
				nvrhi::BindingSetItem::ConstantBuffer( 1, m_DenoiseConstantBuffer ),
				nvrhi::BindingSetItem::Texture_UAV( 0, mask->GetTextureHandle() ),
			};
			m_DenoiseBindingSet = m_Device->createBindingSet( dnSetDesc, m_DenoiseBindingLayout );
			m_BoundRawMask = rawMask;
		}

		m_BoundTlas = tlas;
		m_BoundMask = mask;
		m_BoundCoarse = coarse;
	}

	// Coarse + edge-refine (see r_rtShadowCoarse). Disabled - falls back to the legacy single
	// dispatch - whenever: the coarse image isn't ready yet; a debug force mode is active (they
	// assume full-screen single-pass coverage, see rt_shadows.cs.hlsl); or soft shadows are active
	// (rays > 1 makes the coarse mask continuous almost everywhere, so the refine pass would trace
	// nearly every pixel anyway - pure overhead with no savings).
	const int coarseMode = r_rtShadowCoarse.GetInteger();
	const bool debugForceActive = ( r_rtShadowForce.GetInteger() != 0 );
	const bool softShadowsActive = ( r_rtShadowRays.GetInteger() > 1 );
	const bool useTwoPass = coarseImageReady && coarseMode != 0 && !debugForceActive && !softShadowsActive;

	// One-shot gate diagnostic: if coarse+refine "does nothing", this line names the failing
	// condition (coarseImageReady 0 = coarse mask never generated; rays>1 = soft shadows force
	// legacy; force!=0 = a debug mode forces legacy).
	{
		static bool loggedGate = false;
		if( !loggedGate )
		{
			common->Printf( "RtShadowsPass gate: coarseMode=%i coarseImageReady=%i force=%i rays=%i -> useTwoPass=%i\n",
							 coarseMode, ( int )coarseImageReady, r_rtShadowForce.GetInteger(), r_rtShadowRays.GetInteger(), ( int )useTwoPass );
			loggedGate = true;
		}
	}

	if( !useTwoPass )
	{
		// legacy path: constants.coarseParams is already (0,0) from the memset above.
		commandList->writeBuffer( m_ConstantBuffer, &constants, sizeof( constants ) );

		// soft shadows: trace the raw (noisy) penumbra to m_RawMaskImage, then spatially
		// denoise it into the final mask. Hard shadows (rays == 1) are pixel-exact and
		// skip the denoise entirely (trace straight to the final mask).
		const bool doDenoise = softShadowsActive && m_DenoisePipeline != nullptr &&
							   m_RawMaskBindingSet != nullptr && m_DenoiseBindingSet != nullptr &&
							   r_rtShadowDenoise.GetBool() && r_rtShadowDenoiseRadius.GetFloat() > 0.0f;

		nvrhi::ComputeState state;
		state.pipeline = m_TracePipeline;
		state.bindings = { doDenoise ? m_RawMaskBindingSet : m_MaskBindingSet };
		commandList->setComputeState( state );
		commandList->dispatch( ( dispW + 7 ) / 8, ( dispH + 7 ) / 8, 1 );

		if( doDenoise )
		{
			ShadowDenoiseConstants dc;
			memset( &dc, 0, sizeof( dc ) );
			dc.screenSize = constants.screenSize;
			dc.scissorMin = constants.scissorMin;
			dc.scissorMax = idVec2i( constants.scissorMin.x + dispW - 1, constants.scissorMin.y + dispH - 1 );
			dc.params = idVec4( r_rtShadowDenoiseRadius.GetFloat(), r_rtShadowDenoiseDepthSigma.GetFloat(), 0.0f, 0.0f );
			commandList->writeBuffer( m_DenoiseConstantBuffer, &dc, sizeof( dc ) );

			nvrhi::ComputeState dnState;
			dnState.pipeline = m_DenoisePipeline;
			dnState.bindings = { m_DenoiseBindingSet };
			commandList->setComputeState( dnState );
			commandList->dispatch( ( dispW + 7 ) / 8, ( dispH + 7 ) / 8, 1 );
		}

		return true;
	}

	// Coarse resolution: a fixed fraction of DISPLAY (not render) resolution, so SSAA supersampling
	// does not also multiply the shadow ray budget. ratio = render texels per coarse texel.
	const int ratio = idMath::ClampInt( 1, 64, idMath::Ftoi( idMath::Rint( ( float )r_rtShadowCoarseDiv.GetInteger() * R_SSAAScale() ) ) );
	const int coarseW = ( width + ratio - 1 ) / ratio;
	const int coarseH = ( height + ratio - 1 ) / ratio;

	// Coarse rect: the render-res dispatch rect mapped into coarse-texel space, expanded by 1 texel
	// on every side and clamped to the coarse image. The refine pass's bilinear bracket can read one
	// texel beyond a pixel's own coarse cell, so under-expanding here leaves a stale seam at the
	// scissor border (a previous light's leftover coarse value, since the subregion is shared).
	int cMinX = dispX / ratio - 1;
	int cMinY = dispY / ratio - 1;
	int cMaxX = ( dispX + dispW + ratio - 1 ) / ratio + 1;
	int cMaxY = ( dispY + dispH + ratio - 1 ) / ratio + 1;
	cMinX = idMath::ClampInt( 0, coarseW, cMinX );
	cMinY = idMath::ClampInt( 0, coarseH, cMinY );
	cMaxX = idMath::ClampInt( 0, coarseW, cMaxX );
	cMaxY = idMath::ClampInt( 0, coarseH, cMaxY );
	const int coarseDispW = cMaxX - cMinX;
	const int coarseDispH = cMaxY - cMinY;
	if( coarseDispW <= 0 || coarseDispH <= 0 )
	{
		// Degenerate coarse rect (should not happen for a non-empty render rect) - fall back to
		// the legacy single dispatch for this light rather than dropping its shadow entirely.
		commandList->writeBuffer( m_ConstantBuffer, &constants, sizeof( constants ) );

		nvrhi::ComputeState state;
		state.pipeline = m_TracePipeline;
		state.bindings = { m_MaskBindingSet };
		commandList->setComputeState( state );

		commandList->dispatch( ( dispW + 7 ) / 8, ( dispH + 7 ) / 8, 1 );
		return true;
	}

	constants.coarseSize = idVec2i( coarseW, coarseH );
	constants.coarseScissorMin = idVec2i( cMinX, cMinY );

	// One-shot dims log: confirms the coarse-rect mapping/expansion math (r_rtShadowCoarse) without
	// needing a visually-correct frame - coarseScissorMin must stay >= 0 and coarseScissorMin +
	// coarseDisp must stay <= coarseSize, and the coarse rect must cover ceil(render rect / ratio).
	{
		static bool loggedCoarseDims = false;
		if( !loggedCoarseDims )
		{
			common->Printf( "RtShadowsPass: coarse dims ratio=%i coarseSize=(%i,%i) coarseRect=(%i,%i)+(%i,%i) renderRect=(%i,%i)+(%i,%i)\n",
							 ratio, coarseW, coarseH, cMinX, cMinY, coarseDispW, coarseDispH, dispX, dispY, dispW, dispH );
			loggedCoarseDims = true;
		}
	}

	// Coarse dispatch: trace one ray per coarse texel over the (over-expanded) coarse rect, write
	// the coarse image. Strict write -> setState -> dispatch order preserved between the two
	// dispatches below (do not hoist the writeBuffer calls) - the volatile CB versions on the
	// bound set at each dispatch, and binding the coarse image as an SRV in the refine dispatch's
	// set transitions it out of the UAV state this dispatch leaves it in.
	constants.coarseParams = idVec2i( 1, 0 );
	commandList->writeBuffer( m_ConstantBuffer, &constants, sizeof( constants ) );
	{
		nvrhi::ComputeState state;
		state.pipeline = m_TracePipeline;
		state.bindings = { m_CoarseBindingSet };
		commandList->setComputeState( state );
		commandList->dispatch( ( coarseDispW + 7 ) / 8, ( coarseDispH + 7 ) / 8, 1 );
	}

	// Refine dispatch: full render-res over the light's normal scissor rect. coarseMode == 1 is the
	// "coarse-only" diagnostic (force-upsample, never trace - see the shader); coarseMode == 2 is
	// the real edge-refine (trace only where the coarse mask disagrees).
	constants.coarseParams = idVec2i( 2, coarseMode == 1 ? 1 : 0 );
	commandList->writeBuffer( m_ConstantBuffer, &constants, sizeof( constants ) );
	{
		nvrhi::ComputeState state;
		state.pipeline = m_TracePipeline;
		state.bindings = { m_MaskBindingSet };
		commandList->setComputeState( state );
		commandList->dispatch( ( dispW + 7 ) / 8, ( dispH + 7 ) / 8, 1 );
	}

	return true;
}
