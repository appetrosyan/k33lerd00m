/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 2026 Aleksandr Petrosyan

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").

Doom 3 BFG Edition Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

===========================================================================
*/

#include "precompiled.h"
#pragma hdrstop

#include "renderer/RenderCommon.h"
#include "SoftShadowTermPass.h"
#include "SoftShadowSurfCache.h"

// mirrors c_Term in softterm.cs.hlsl
struct SoftTermCB
{
	float	lightR[4];		// light origin xyz, disk radius w
	int		range[4];		// firstElem (tri stream float4 base), casterCount, tileBase | -1, tilesX
	int		tile[4];		// tile origin x, y, atlas slot offset x, y
	int		rect[4];		// scissor origin x, y (absolute pixels), width, height
	float	falloffS[4];	// WORLD-space falloff plane (vLight->lightProject[3])
	float	projS[4];		// WORLD-space projection planes (vLight->lightProject[0..2])
	float	projT[4];
	float	projQ[4];
	int		flags[4];		// x: coverage early-outs enabled
	float	classAabbCell[4];	// lit classifier grid: origin xyz + cellSize
	int		classDims[4];		// dims xyz + BASE (float4 elem offset into the joint buffer); base < 0 = disabled
	float	surfParams[4];	// surface-fold cache: texel G, viz mode, unused, unused
	int		surfA[4];		// surface-fold cache: table cap (slots), queue cap (uints), static caster count, light key
	float	aa[4];			// x = r_softShadowAA per-sample analytic-AA half-width (0 = off); y/z/w unused
	int		surfCost[4];	// x = surf-cache cost gate (r_softShadowSurfCacheMinCost): min tile occluders to cache
	float	misc[4];		// x = r_softShadowMinDnRatio: projection dn-clamp (grazing grain fix);
							// yzw = view origin (cache-economics tiers 1/3)
	float	econ[4];		// cache economics (task #112): x = near-exact radius SQUARED, y = tier-1
							// gate base margin, z = persistent static-stream segment tri base (float4
							// elems, uint BIT-CAST into the lane - audit finding #4), w reserved.
							// Mirrors g_econ in softterm.cs.hlsl.
	unsigned int areaMask[4];	// PORTAL-AREA CULL (r_softShadowAreaCull): 128-bit mask, bit areaNum set
							// iff the light's flood reached the area; ALL-ONES = cull inert (cvar off /
							// unqualified light - conservative). Mirrors g_areaMask in softterm.cs.hlsl.
};

// mirrors c_Blur in softblur.cs.hlsl
struct SoftBlurCB
{
	int		rect[4];	// this light's atlas rect: origin x, y + width, height
	float	blur[4];	// max radius (px), width->radius scale, penumbra lo, penumbra hi
};

// conservative common limit; RDNA3 reports 16384. Exceeding it just disables the pass for the view.
static const int SW_TERM_MAX_TEX_DIM = 16384;

// Atlas grid snapshot (r_softShadowTermSlotCols/Rows, clamped to SW_TERM_MAX_TEX_DIM): file-scope
// statics, NOT class members - growing SoftShadowTermPass shifts its heap layout (the
// idImageManager-class landmine, see SwTermGridStatics below). BeginView snapshots the cvars once
// per view; AddLight reads the same snapshot. 0 = no atlas built yet.
static int swTermAtlasCols = 0;
static int swTermAtlasRows = 0;

// TERM-LIGHT IDENTITY accumulator (bench instrument): unique surf-permutation light indices seen
// since the last Take, with whether the cache was ON for them at CB-fill time. Names the by-light
// rings (ring = index & 15), whose collisions made miss attribution ambiguous at >6 lights.
static int  s_swSurfTermLightIdx[64];
static bool s_swSurfTermLightOn[64];
static int  s_swSurfTermLightCount = 0;
void R_SoftSurfTermLightSeen( int index, bool cacheOn )
{
	for( int i = 0; i < s_swSurfTermLightCount; i++ )
	{
		if( s_swSurfTermLightIdx[i] == index )
		{
			s_swSurfTermLightOn[i] = cacheOn;
			return;
		}
	}
	if( s_swSurfTermLightCount < 64 )
	{
		s_swSurfTermLightIdx[s_swSurfTermLightCount] = index;
		s_swSurfTermLightOn[s_swSurfTermLightCount] = cacheOn;
		s_swSurfTermLightCount++;
	}
}
int R_SoftSurfTermLightsTake( int* outIdx, bool* outOn, int cap )
{
	const int n = ( s_swSurfTermLightCount < cap ) ? s_swSurfTermLightCount : cap;
	for( int i = 0; i < n; i++ )
	{
		outIdx[i] = s_swSurfTermLightIdx[i];
		outOn[i] = s_swSurfTermLightOn[i];
	}
	s_swSurfTermLightCount = 0;
	return n;
}

// SURF-CACHE GRID permutation kept OFF the class layout (not members): growing SoftShadowTermPass shifts
// its heap layout and can surface a latent init-time heap fault (the idImageManager-class landmine). They
// must ALSO outlive normal static teardown: a file-scope nvrhi handle runs its destructor at PROGRAM EXIT,
// after the Vulkan device is gone, and ComputePipeline::~ -> vkDestroyPipeline on a dead device SIGSEGVs
// (confirmed backtrace 2026-08-23). So hold them in a heap struct that is INTENTIONALLY LEAKED (accessor's
// local static pointer, never freed) - the handle destructors never run; device teardown reclaims the GPU
// objects. Single term-pass instance.
struct SwTermGridStatics
{
	nvrhi::ShaderHandle				shader;
	nvrhi::ComputePipelineHandle	pipeline;
};
static SwTermGridStatics& swTermGrid()
{
	static SwTermGridStatics* g = new SwTermGridStatics();	// leaked on purpose - see above; never delete
	return *g;
}

SoftShadowTermPass::SoftShadowTermPass( nvrhi::IDevice* device )
	: m_Device( device )
{
}

void SoftShadowTermPass::EnsurePipeline()
{
	if( m_PipelineTried )
	{
		return;
	}
	m_PipelineTried = true;

	// SW_FACE_SAMPLES permutation (r_softShadowSamples): the disk ray count is compiled into the walk
	// (fp16-packed at whatever count), so 8/16/32 stay the fast path and cost is monotonic. 0 disables
	// in-shader (built as 16). FindShader dedups by name+suffix and IGNORES macros, so each count needs
	// a DISTINCT nameOutSuffix ("s8"/"s16"/"s32") or it returns the first-built entry.
	extern idCVar r_softShadowSamples;
	const int sv = r_softShadowSamples.GetInteger();
	m_BuiltSamples = ( sv == 8 || sv == 32 ) ? sv : 16;
	const char* samplesStr = ( m_BuiltSamples == 8 ) ? "8" : ( ( m_BuiltSamples == 32 ) ? "32" : "16" );
	// SW_SCAN_CHORDS permutation (r_softShadowScanChords): Fubini penumbra chord count. Distinct
	// nameOutSuffix per chord count (FindShader dedups by name+suffix, ignores macros - see samples).
	extern idCVar r_softShadowScanChords;
	const int cv = r_softShadowScanChords.GetInteger();
	m_BuiltChords = ( cv == 4 || cv == 8 || cv == 32 ) ? cv : 16;
	const char* chordsStr = ( m_BuiltChords == 4 ) ? "4" : ( ( m_BuiltChords == 8 ) ? "8" : ( ( m_BuiltChords == 32 ) ? "32" : "16" ) );
	const idStr sfx = idStr( "s" ) + samplesStr + "c" + chordsStr;

	idList<shaderMacro_t> macros;
	macros.Append( shaderMacro_t( "SW_GPU_WALK_COUNTERS", "0" ) );	// shipped permutation (must be explicit now the cfg declares {0,1})
	macros.Append( shaderMacro_t( "SW_SURF_CACHE", "0" ) );			// same rule for the surf-cache axis
	macros.Append( shaderMacro_t( "SW_SURF_GRID", "0" ) );			// and the grid axis (order MUST match shaders.cfg: after SURF_CACHE)
	macros.Append( shaderMacro_t( "SW_SCANLINE", "0" ) );			// and the scanline axis
	macros.Append( shaderMacro_t( "SW_CONTRIB_CACHE", "0" ) );		// and the contributor-cache axis
	macros.Append( shaderMacro_t( "SW_FACE_SAMPLES", samplesStr ) );
		macros.Append( shaderMacro_t( "SW_SCAN_CHORDS", chordsStr ) );
	m_Shader = renderProgManager.GetShader( renderProgManager.FindShader( "builtin/lighting/softterm", SHADER_STAGE_COMPUTE, sfx.c_str(), macros, true, LAYOUT_DRAW_VERT ) );
	if( m_Shader == nullptr )
	{
		common->Warning( "SoftShadowTermPass: compute shader failed to load - compute soft-shadow term disabled." );
		return;
	}

	nvrhi::BindingLayoutDesc ld;
	ld.visibility = nvrhi::ShaderType::Compute;
	ld.bindings =
	{
		nvrhi::BindingLayoutItem::VolatileConstantBuffer( 0 ),	// b0 : term constants
		nvrhi::BindingLayoutItem::StructuredBuffer_SRV( 0 ),	// t0 : edge records
		nvrhi::BindingLayoutItem::StructuredBuffer_SRV( 1 ),	// t1 : tile lists
		nvrhi::BindingLayoutItem::Texture_SRV( 2 ),				// t2 : exact world position G-buffer
		nvrhi::BindingLayoutItem::Texture_SRV( 3 ),				// t3 : light falloff (coverage early-out)
		nvrhi::BindingLayoutItem::Texture_SRV( 4 ),				// t4 : light projection (coverage early-out)
		nvrhi::BindingLayoutItem::Texture_SRV( 5 ),				// t5 : world shading normal (N.L early-out)
		nvrhi::BindingLayoutItem::StructuredBuffer_SRV( 7 ),	// t7 : cull-before-load (centroid, triRad) per tile slot
		nvrhi::BindingLayoutItem::Sampler( 0 ),					// s0 : falloff sampler
		nvrhi::BindingLayoutItem::Sampler( 1 ),					// s1 : projection sampler
		nvrhi::BindingLayoutItem::Texture_UAV( 0 ),				// u0 : term atlas
	};
	m_Layout = m_Device->createBindingLayout( ld );

	nvrhi::ComputePipelineDesc pd;
	pd.bindingLayouts = { m_Layout };
	pd.CS = m_Shader;
	m_Pipeline = m_Device->createComputePipeline( pd );

	// WALK-ATTRIBUTION counting permutation: same shader with -D SW_GPU_WALK_COUNTERS=1 (adds a u1
	// counter UAV). Separate layout+pipeline so the shipped pipeline stays byte-identical; used only
	// when r_softShadowWalkCounters is set. Failure here is non-fatal (counters just unavailable).
	{
		idList<shaderMacro_t> cntMacros;
		cntMacros.Append( shaderMacro_t( "SW_GPU_WALK_COUNTERS", "1" ) );
		cntMacros.Append( shaderMacro_t( "SW_SURF_CACHE", "0" ) );
		cntMacros.Append( shaderMacro_t( "SW_SURF_GRID", "0" ) );
		// SCANLINE=1: the counting permutation must instrument the SHIPPED path. It compiled the dead
		// sampled walk (=0) until 2026-08-25, so every "walk counter" run measured a path the game
		// never executes - the counters now attribute FillTri (fill/reject/skip/fold/sweep, slots 20-26).
		cntMacros.Append( shaderMacro_t( "SW_SCANLINE", "1" ) );
		cntMacros.Append( shaderMacro_t( "SW_CONTRIB_CACHE", "0" ) );
		cntMacros.Append( shaderMacro_t( "SW_FACE_SAMPLES", samplesStr ) );
		cntMacros.Append( shaderMacro_t( "SW_SCAN_CHORDS", chordsStr ) );
		// DISTINCT nameOutSuffix: FindShader dedups by name+stage+suffix and IGNORES macros, so without a
		// distinct suffix the counting call returns the shipped (=0) entry. The suffix does not change the
		// blob path (LoadShader keys the .bin on shader.name only) - it forces a separate entry whose
		// macros make FindPermutationInBlob select the =1 variant.
		m_ShaderCnt = renderProgManager.GetShader( renderProgManager.FindShader( "builtin/lighting/softterm", SHADER_STAGE_COMPUTE, ( idStr( "walkcnt" ) + sfx ).c_str(), cntMacros, true, LAYOUT_DRAW_VERT ) );
		if( m_ShaderCnt != nullptr )
		{
			nvrhi::BindingLayoutDesc lc = ld;
			lc.bindings.push_back( nvrhi::BindingLayoutItem::StructuredBuffer_UAV( 1 ) );	// u1 : walk counters
			m_LayoutCnt = m_Device->createBindingLayout( lc );
			nvrhi::ComputePipelineDesc pc;
			pc.bindingLayouts = { m_LayoutCnt };
			pc.CS = m_ShaderCnt;
			m_PipelineCnt = m_Device->createComputePipeline( pc );

			nvrhi::BufferDesc wc;
			wc.byteSize = 28 * sizeof( uint32_t );	// 0-7 attrib, 8-13 buckets, 14-15 hit/miss, 16-19 tile-class census, 20-26 scanline FillTri attribution (task #106)
			wc.structStride = sizeof( uint32_t );		// RWStructuredBuffer<uint> (matches u_SpillCnt pattern)
			wc.canHaveUAVs = true;
			wc.initialState = nvrhi::ResourceStates::UnorderedAccess;
			wc.keepInitialState = true;
			wc.debugName = "SoftShadowTerm/WalkCounters";
			m_WalkCntBuffer = m_Device->createBuffer( wc );
		}
	}

	// SURFACE-FOLD CACHE permutation (SW_SURF_CACHE=1, r_softShadowSurfCache): adds the texel-table +
	// request-queue UAVs and the residual-pool SRV. Separate layout/pipeline (same isolation reasoning
	// as the counting permutation); failure is non-fatal (the probe just stays off).
	{
		idList<shaderMacro_t> surfMacros;
		surfMacros.Append( shaderMacro_t( "SW_GPU_WALK_COUNTERS", "0" ) );	// blob keys carry ALL axes
		surfMacros.Append( shaderMacro_t( "SW_SURF_CACHE", "1" ) );
		surfMacros.Append( shaderMacro_t( "SW_SURF_GRID", "0" ) );
		// SW_SCANLINE 1 (task #102): the surf permutation historically compiled SAMPLED, silently
		// reverting every surf-routed fragment (hit residual walk AND miss/walk-always/anchor-reject)
		// to the legacy 16-sample walk - banding + grain the plain path had already fixed. The
		// scanline+envelope serve keeps ONE exact algorithm per frame; the sampled body remains under
		// #else in SoftShadow_FaceCoverageSurfResidual as the A/B baseline.
		surfMacros.Append( shaderMacro_t( "SW_SCANLINE", "1" ) );
		surfMacros.Append( shaderMacro_t( "SW_CONTRIB_CACHE", "0" ) );
		surfMacros.Append( shaderMacro_t( "SW_FACE_SAMPLES", samplesStr ) );
		surfMacros.Append( shaderMacro_t( "SW_SCAN_CHORDS", chordsStr ) );
		m_ShaderSurf = renderProgManager.GetShader( renderProgManager.FindShader( "builtin/lighting/softterm", SHADER_STAGE_COMPUTE, ( idStr( "surfcache" ) + sfx ).c_str(), surfMacros, true, LAYOUT_DRAW_VERT ) );
		if( m_ShaderSurf != nullptr )
		{
			nvrhi::BindingLayoutDesc ls = ld;
			ls.bindings.push_back( nvrhi::BindingLayoutItem::StructuredBuffer_UAV( 2 ) );	// u2 : texel table
			ls.bindings.push_back( nvrhi::BindingLayoutItem::StructuredBuffer_SRV( 6 ) );	// t6 : residual pool
			// t8 : persistent static stream (audit finding #4) - the pool's 1-uint residual indices
			// resolve here. Declared only by the SCALAR surf shader; the grid shader (same layout)
			// leaves it unreferenced, which Vulkan permits (the desync landmine is the OPPOSITE case:
			// a shader-declared binding missing from the layout).
			ls.bindings.push_back( nvrhi::BindingLayoutItem::StructuredBuffer_SRV( 8 ) );
			// NOTE: no u3 request-queue - the read-only term never enqueues (the burst seeds), and a
			// declared-but-stripped u3 desynced the reflected layout and crashed at r_softShadowSamples 32.
			m_LayoutSurf = m_Device->createBindingLayout( ls );
			nvrhi::ComputePipelineDesc ps;
			ps.bindingLayouts = { m_LayoutSurf };
			ps.CS = m_ShaderSurf;
			m_PipelineSurf = m_Device->createComputePipeline( ps );
		}
	}

	// SURF-CACHE GRID permutation (SW_SURF_CACHE=1 + SW_SURF_GRID=1 + SW_SCANLINE=1,
	// r_softShadowSurfCacheGrid): the cached hit reads the frozen static Fubini grid (parallel buffer,
	// SRV t7) and ORs the live dynamic grid instead of the scalar fold. Surf bindings + t7. Separate
	// pipeline; failure is non-fatal (grid mode falls back to the scalar surf pipeline / exact walk).
	{
		idList<shaderMacro_t> gridMacros;
		gridMacros.Append( shaderMacro_t( "SW_GPU_WALK_COUNTERS", "0" ) );
		gridMacros.Append( shaderMacro_t( "SW_SURF_CACHE", "1" ) );
		gridMacros.Append( shaderMacro_t( "SW_SURF_GRID", "1" ) );
		gridMacros.Append( shaderMacro_t( "SW_SCANLINE", "1" ) );
		gridMacros.Append( shaderMacro_t( "SW_CONTRIB_CACHE", "0" ) );
		gridMacros.Append( shaderMacro_t( "SW_FACE_SAMPLES", samplesStr ) );
		gridMacros.Append( shaderMacro_t( "SW_SCAN_CHORDS", chordsStr ) );
		swTermGrid().shader = renderProgManager.GetShader( renderProgManager.FindShader( "builtin/lighting/softterm", SHADER_STAGE_COMPUTE, ( idStr( "surfgrid" ) + sfx ).c_str(), gridMacros, true, LAYOUT_DRAW_VERT ) );
		if( swTermGrid().shader != nullptr && m_LayoutSurf != nullptr )
		{
			// GRID reuses the surf binding LAYOUT (u2 table + t6): the grid buffer binds to t6 in place of
			// the residual pool (excluded in grid mode), so no extra slot desyncs the reflected layout.
			nvrhi::ComputePipelineDesc psg;
			psg.bindingLayouts = { m_LayoutSurf };
			psg.CS = swTermGrid().shader;
			swTermGrid().pipeline = m_Device->createComputePipeline( psg );
		}
	}

	// FUBINI SCANLINE permutation (SW_SCANLINE=1, r_softShadowScanline): the tile-list walk fills an 8x32
	// interval bit-grid (exact 1D union per chord) instead of the 16-sample mask. Same bindings as the
	// shipped path (no new buffers), so it reuses ld/m_Layout. Separate pipeline keeps the shipped one
	// byte-identical; failure is non-fatal (the toggle just stays on the sampled path).
	{
		idList<shaderMacro_t> scanMacros;
		scanMacros.Append( shaderMacro_t( "SW_GPU_WALK_COUNTERS", "0" ) );
		scanMacros.Append( shaderMacro_t( "SW_SURF_CACHE", "0" ) );
		scanMacros.Append( shaderMacro_t( "SW_SURF_GRID", "0" ) );
		scanMacros.Append( shaderMacro_t( "SW_SCANLINE", "1" ) );
		scanMacros.Append( shaderMacro_t( "SW_CONTRIB_CACHE", "0" ) );
		scanMacros.Append( shaderMacro_t( "SW_FACE_SAMPLES", samplesStr ) );
		scanMacros.Append( shaderMacro_t( "SW_SCAN_CHORDS", chordsStr ) );
		m_ShaderScan = renderProgManager.GetShader( renderProgManager.FindShader( "builtin/lighting/softterm", SHADER_STAGE_COMPUTE, ( idStr( "scanline" ) + sfx ).c_str(), scanMacros, true, LAYOUT_DRAW_VERT ) );
		if( m_ShaderScan != nullptr )
		{
			nvrhi::ComputePipelineDesc pn;
			pn.bindingLayouts = { m_Layout };		// identical bindings to the shipped path
			pn.CS = m_ShaderScan;
			m_PipelineScan = m_Device->createComputePipeline( pn );
		}
	}

	// CONTRIBUTOR CACHE permutation (SW_CONTRIB_CACHE=1, r_softShadowContribCache; scanline is forced
	// inside the shader). Evaluate-once union: the first K fragments per (world cell, light) record
	// their solo contributors, later fragments walk the recorded 7-17-triangle union + live dynamics.
	// Base bindings + the table UAV at u2; separate layout/pipeline keeps every other path untouched.
	{
		idList<shaderMacro_t> conMacros;
		conMacros.Append( shaderMacro_t( "SW_GPU_WALK_COUNTERS", "0" ) );
		conMacros.Append( shaderMacro_t( "SW_SURF_CACHE", "0" ) );
		conMacros.Append( shaderMacro_t( "SW_SURF_GRID", "0" ) );
		conMacros.Append( shaderMacro_t( "SW_SCANLINE", "1" ) );
		conMacros.Append( shaderMacro_t( "SW_CONTRIB_CACHE", "1" ) );
		conMacros.Append( shaderMacro_t( "SW_FACE_SAMPLES", samplesStr ) );
		conMacros.Append( shaderMacro_t( "SW_SCAN_CHORDS", chordsStr ) );
		m_ShaderContrib = renderProgManager.GetShader( renderProgManager.FindShader( "builtin/lighting/softterm", SHADER_STAGE_COMPUTE, ( idStr( "contrib" ) + sfx ).c_str(), conMacros, true, LAYOUT_DRAW_VERT ) );
		if( m_ShaderContrib != nullptr )
		{
			nvrhi::BindingLayoutDesc lc2 = ld;
			lc2.bindings.push_back( nvrhi::BindingLayoutItem::StructuredBuffer_UAV( 2 ) );	// u2 : contributor table
			m_LayoutContrib = m_Device->createBindingLayout( lc2 );
			nvrhi::ComputePipelineDesc pc2;
			pc2.bindingLayouts = { m_LayoutContrib };
			pc2.CS = m_ShaderContrib;
			m_PipelineContrib = m_Device->createComputePipeline( pc2 );

			extern idCVar r_softShadowContribCap;
			nvrhi::BufferDesc cbd;
			// header (24 uints: pool occupancy + stats) + cap slots x (4 + 64 entries + 4 words: tile
			// live/pending/pend-frame + generation) uints - must match SW_CONTRIB_STRIDE/HEADER in
			// softterm.cs.hlsl
			cbd.byteSize = ( uint64_t )( 24 + ( uint64_t )r_softShadowContribCap.GetInteger() * 72 ) * sizeof( uint32_t );
			cbd.structStride = sizeof( uint32_t );
			cbd.canHaveUAVs = true;
			cbd.initialState = nvrhi::ResourceStates::UnorderedAccess;
			cbd.keepInitialState = true;
			cbd.debugName = "SoftShadowTerm/ContribTable";
			m_ContribBuffer = m_Device->createBuffer( cbd );
		}
	}

	nvrhi::BufferDesc cb;
	cb.byteSize = sizeof( SoftTermCB );
	cb.isConstantBuffer = true;
	cb.isVolatile = true;
	cb.maxVersions = 1024;					// dispatches per frame x frames in flight
	cb.debugName = "SoftShadowTerm/CB";
	m_ConstantBuffer = m_Device->createBuffer( cb );

	// TEMPORAL-STABILITY BLUR pipeline (softblur.cs.hlsl): reads the term atlas (t0), writes the blur
	// atlas (u0), one dispatch per packed light rect. No macros. Failure is non-fatal (blur stays off).
	{
		m_BlurShader = renderProgManager.GetShader( renderProgManager.FindShader( "builtin/lighting/softblur", SHADER_STAGE_COMPUTE, "", idList<shaderMacro_t>(), true, LAYOUT_DRAW_VERT ) );
		if( m_BlurShader != nullptr )
		{
			nvrhi::BindingLayoutDesc lb;
			lb.visibility = nvrhi::ShaderType::Compute;
			lb.bindings =
			{
				nvrhi::BindingLayoutItem::VolatileConstantBuffer( 0 ),	// b0 : blur constants
				nvrhi::BindingLayoutItem::Texture_SRV( 0 ),				// t0 : term atlas (read)
				nvrhi::BindingLayoutItem::Texture_UAV( 0 ),				// u0 : blur atlas (write)
			};
			m_BlurLayout = m_Device->createBindingLayout( lb );
			nvrhi::ComputePipelineDesc pb;
			pb.bindingLayouts = { m_BlurLayout };
			pb.CS = m_BlurShader;
			m_BlurPipeline = m_Device->createComputePipeline( pb );

			nvrhi::BufferDesc bc;
			bc.byteSize = sizeof( SoftBlurCB );
			bc.isConstantBuffer = true;
			bc.isVolatile = true;
			bc.maxVersions = 1024;
			bc.debugName = "SoftShadowTerm/BlurCB";
			m_BlurCB = m_Device->createBuffer( bc );
		}
	}
}

bool SoftShadowTermPass::BeginView( nvrhi::ICommandList* commandList, const viewDef_t* viewDef, nvrhi::ITexture* worldPosTexture, nvrhi::ITexture* worldNormalTexture )
{
	m_Cursor = 0;
	m_ShelfX = m_ShelfY = m_ShelfH = 0;	// reset the scissor packer for this view
	m_Valid = false;
	m_WorldPos = worldPosTexture;
	// walk-attribution counting: snapshot the cvar; clear the counter buffer so this view's totals
	// start at zero. Only active when the counting pipeline built (m_PipelineCnt).
	extern idCVar r_softShadowWalkCounters;
	m_WalkCntEnabled = r_softShadowWalkCounters.GetBool() && m_PipelineCnt != nullptr && m_WalkCntBuffer != nullptr;
	if( m_WalkCntEnabled )
	{
		commandList->clearBufferUInt( m_WalkCntBuffer, 0 );		// same as the spill counter's proven clear
	}
	m_WorldNormal = worldNormalTexture;
	// r_softShadowSamples changed -> rebuild the pipelines at the new SW_FACE_SAMPLES count (rare; a tuning
	// toggle). Nulling the handles + m_PipelineTried forces EnsurePipeline to recompile the count variant.
	{
		extern idCVar r_softShadowSamples;
		extern idCVar r_softShadowScanChords;
		const int sv = r_softShadowSamples.GetInteger();
		const int want = ( sv == 8 || sv == 32 ) ? sv : 16;
		const int cvc = r_softShadowScanChords.GetInteger();
		const int wantC = ( cvc == 4 || cvc == 8 || cvc == 32 ) ? cvc : 16;
		if( ( m_BuiltSamples != 0 && m_BuiltSamples != want ) || ( m_BuiltChords != 0 && m_BuiltChords != wantC ) )
		{
			m_PipelineTried = false;
			m_Pipeline = m_PipelineCnt = m_PipelineSurf = m_PipelineScan = m_PipelineContrib = swTermGrid().pipeline = nullptr;
			m_Shader = m_ShaderCnt = m_ShaderSurf = m_ShaderScan = m_ShaderContrib = swTermGrid().shader = nullptr;
		}
	}
	EnsurePipeline();
	if( m_Pipeline == nullptr || worldPosTexture == nullptr )
	{
		return false;
	}

	// screen-size slots cover the whole render target so the atlas is addressed by SV_Position +
	// slot offset (absolute pixels, like the tile bins)
	const int screenW = viewDef->viewport.x2 + 1;
	const int screenH = viewDef->viewport.y2 + 1;
	if( screenW <= 0 || screenH <= 0 || screenW > SW_TERM_MAX_TEX_DIM || screenH > SW_TERM_MAX_TEX_DIM )
	{
		return false;						// exotic resolution: leave every light on the in-shader integral
	}

	// snapshot the grid cvars for this view, clamped so the atlas never exceeds the device limit
	{
		extern idCVar r_softShadowTermSlotCols;
		extern idCVar r_softShadowTermSlotRows;
		int cols = r_softShadowTermSlotCols.GetInteger();
		int rows = r_softShadowTermSlotRows.GetInteger();
		const int maxCols = SW_TERM_MAX_TEX_DIM / screenW;
		const int maxRows = SW_TERM_MAX_TEX_DIM / screenH;
		if( cols > maxCols || rows > maxRows )
		{
			static bool warned = false;
			if( !warned )
			{
				warned = true;
				common->Warning( "SoftShadowTerm: slot grid %dx%d clamped to %dx%d (%dx%d screen, %d max texture dim)",
								 cols, rows, ( cols > maxCols ) ? maxCols : cols, ( rows > maxRows ) ? maxRows : rows,
								 screenW, screenH, SW_TERM_MAX_TEX_DIM );
			}
			cols = ( cols > maxCols ) ? maxCols : cols;
			rows = ( rows > maxRows ) ? maxRows : rows;
		}
		if( cols != swTermAtlasCols || rows != swTermAtlasRows )
		{
			// grid changed mid-run: force the atlas (and matching blur atlas) to rebuild below
			m_TermTexture = nullptr;
			m_BlurTexture = nullptr;
			swTermAtlasCols = cols;
			swTermAtlasRows = rows;
		}
	}

	// GROW-only: subviews (mirrors) have smaller viewports than the main view; resizing per view
	// would thrash the allocation every frame. A larger-than-needed slot is harmless - offsets are
	// multiples of the stored slot extents and every dispatch stays inside its slot.
	if( m_TermTexture == nullptr || m_SlotW < screenW || m_SlotH < screenH )
	{
		const int newW = ( m_SlotW > screenW ) ? m_SlotW : screenW;
		const int newH = ( m_SlotH > screenH ) ? m_SlotH : screenH;
		nvrhi::TextureDesc td;
		td.width = newW * swTermAtlasCols;
		td.height = newH * swTermAtlasRows;
		td.format = nvrhi::Format::R16_FLOAT;	// term is k/16, exactly representable in fp16 (see header)
		td.isUAV = true;
		td.initialState = nvrhi::ResourceStates::UnorderedAccess;
		td.keepInitialState = true;
		td.debugName = "SoftShadowTerm/Atlas";
		m_TermTexture = m_Device->createTexture( td );
		m_SlotW = newW;
		m_SlotH = newH;
		m_BlurTexture = nullptr;		// term atlas resized -> the blur atlas must match; rebuilt below
	}

	// TEMPORAL-STABILITY BLUR: snapshot the cvar for this view and (re)create the blur atlas to match
	// the term atlas. m_BlurActive gates GetTermTexture() and BlurView(); m_BlurRects collects the
	// packed light rects as AddLight places them.
	extern idCVar r_softShadowTermBlur;
	m_BlurActive = ( r_softShadowTermBlur.GetFloat() > 0.0f ) && ( m_BlurPipeline != nullptr );
	m_BlurRects.SetNum( 0 );
	if( m_BlurActive && m_BlurTexture == nullptr )
	{
		nvrhi::TextureDesc td;
		td.width = m_SlotW * swTermAtlasCols;
		td.height = m_SlotH * swTermAtlasRows;
		td.format = nvrhi::Format::R16_FLOAT;
		td.isUAV = true;
		td.initialState = nvrhi::ResourceStates::UnorderedAccess;
		td.keepInitialState = true;
		td.debugName = "SoftShadowTerm/BlurAtlas";
		m_BlurTexture = m_Device->createTexture( td );
	}

	// The softpos raster pass wrote this texture earlier in the frame; force it through
	// ShaderResource so the compute reads are ordered after the raster writes (same explicit
	// barrier precedent as the SoftTileBinPass depth read).
	commandList->setTextureState( worldPosTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource );
	if( worldNormalTexture != nullptr )
	{
		commandList->setTextureState( worldNormalTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource );
	}

	m_Valid = true;
	return true;
}

bool SoftShadowTermPass::AddLight( nvrhi::ICommandList* commandList, const viewDef_t* viewDef, const viewLight_t* vLight,
								   nvrhi::IBuffer* edgeBuffer, uint32_t edgeFirstElem,
								   uint32_t casterFirstElem, int casterCount,
								   float penumbraRadius,
								   int tileBase, int tileOx, int tileOy, int tilesX,
								   nvrhi::IBuffer* tileBuffer,
								   nvrhi::IBuffer* tileCullBuffer,
								   nvrhi::ITexture* falloffTex, nvrhi::ISampler* falloffSamp,
								   nvrhi::ITexture* projTex, nvrhi::ISampler* projSamp,
								   bool coverageEarlyOut,
								   SoftShadowSurfCache* surfCache,
								   int& outOfsX, int& outOfsY )
{
	if( !m_Valid || casterCount <= 0 )
	{
		return false;
	}
	// light scissor in ABSOLUTE screen pixels (SV_Position space) - same mapping as BinLight:
	// scissorRect is GL-convention (bottom-up), SV_Position is top-down => Y flips against
	// viewport.y2. The old unflipped mapping wrote the term into a MIRRORED rect for every
	// non-fullscreen light; the FS then Loaded zeros from the unwritten true rect (all-black
	// light). Fullscreen lights are flip-invariant, which is how the first A/B missed it.
	const int px1 = viewDef->viewport.x1 + vLight->scissorRect.x1;
	const int py1 = viewDef->viewport.y2 - vLight->scissorRect.y2;
	const int px2 = viewDef->viewport.x1 + vLight->scissorRect.x2;
	const int py2 = viewDef->viewport.y2 - vLight->scissorRect.y1;
	if( px2 < px1 || py2 < py1 )
	{
		return false;
	}

	// SCISSOR-PACK the atlas (replaces the fixed full-screen grid): place this light's scissor-sized
	// rect on a shelf. Total lit area across a frame's soft lights is ~3-7x screen (measured), so a
	// COLS*ROWS-screen atlas holds far more than COLS*ROWS lights when scissors are sub-screen - the
	// "many small lights" rooms that previously spilled the excess onto the wave64 in-shader integral.
	const int atlasW = m_SlotW * swTermAtlasCols;
	const int atlasH = m_SlotH * swTermAtlasRows;
	const int rw = px2 - px1 + 1;
	const int rh = py2 - py1 + 1;
	if( rw > atlasW || rh > atlasH )
	{
		return false;						// a single rect larger than the whole atlas: keep the integral
	}
	if( m_ShelfX + rw > atlasW )			// no room on the current shelf: start a new one below
	{
		m_ShelfX = 0;
		m_ShelfY += m_ShelfH;
		m_ShelfH = 0;
	}
	if( m_ShelfY + rh > atlasH )
	{
		extern idCVar r_rtAccelDebug;
		if( r_rtAccelDebug.GetBool() )
		{
			common->Printf( "SoftShadowTerm: atlas full (%d lights packed), light keeps the in-shader integral\n", m_Cursor );
		}
		return false;						// atlas genuinely full: the light stays on the in-shader integral
	}
	const int atlasX = m_ShelfX;
	const int atlasY = m_ShelfY;
	m_ShelfX += rw;
	if( rh > m_ShelfH )
	{
		m_ShelfH = rh;
	}
	m_Cursor++;
	if( m_BlurActive )		// record the packed rect for BlurView (atlas-local coords)
	{
		BlurRect br = { atlasX, atlasY, rw, rh };
		m_BlurRects.Append( br );
	}
	// the CS writes u_Term[px + offset] and the interaction PS reads term.Load(SV_Position + offset);
	// px == SV_Position == absolute screen pixel, so offset = atlasOrigin - scissorOrigin lands both
	// at packed-local coords. Both consumers are UNCHANGED - only the offset value differs.
	const int slotOfsX = atlasX - px1;
	const int slotOfsY = atlasY - py1;

	SoftTermCB cb;
	cb.lightR[0] = vLight->globalLightOrigin.x;
	cb.lightR[1] = vLight->globalLightOrigin.y;
	cb.lightR[2] = vLight->globalLightOrigin.z;
	cb.lightR[3] = penumbraRadius;
	cb.range[0] = ( int )edgeFirstElem;
	cb.range[1] = casterCount;
	cb.range[2] = tileBase;
	cb.range[3] = tilesX;
	cb.tile[0] = tileOx;
	cb.tile[1] = tileOy;
	cb.tile[2] = slotOfsX;
	cb.tile[3] = slotOfsY;
	cb.rect[0] = px1;
	cb.rect[1] = py1;
	cb.rect[2] = px2 - px1 + 1;
	cb.rect[3] = py2 - py1 + 1;
	// WORLD-space light planes for the coverage early-out (the VS folds these into model space
	// per surface; plane . worldPos reproduces the PS's idtex2Dproj coordinates)
	for( int i = 0; i < 4; i++ )
	{
		cb.projS[i]    = vLight->lightProject[0][i];
		cb.projT[i]    = vLight->lightProject[1][i];
		cb.projQ[i]    = vLight->lightProject[2][i];
		cb.falloffS[i] = vLight->lightProject[3][i];
	}
	cb.flags[0] = ( coverageEarlyOut && falloffTex != NULL && projTex != NULL ) ? 1 : 0;
	cb.flags[1] = ( int )casterFirstElem;	// caster table base (float4 elements) - stream v2
	extern idCVar r_softShadowBackfaceCull;
	cb.flags[2] = r_softShadowBackfaceCull.GetBool() ? 1 : 0;	// N.L<=0 early-out enable
	extern idCVar r_softShadowTileK;
	cb.flags[3] = idMath::ClampInt( 1, 512, r_softShadowTileK.GetInteger() );	// tile-buffer stride (must match bin CS)

	// surface-fold cache probe (r_softShadowSurfCache): the SW_SURF_CACHE permutation is selected only
	// when the probe is active AND the counting permutation is not (counters = measurement mode, wins).
	// surfA[0] <= 0 keeps the cache block dead even on the surf pipeline (light with no static prefix).
	// PER-LIGHT pipeline selection (r_softShadowSurfCacheMinLight): the surf permutation carries a fixed
	// ~0.8ms/frame occupancy tax (extra UAV/SRV bindings lower the wave count) that caching only recoups on
	// EXPENSIVE lights. A cheap light gains nothing from the cache but still pays the tax, so route it to the
	// shipped (cheap) pipeline instead - we don't pay the permutation cost where it can't be recouped. Gate on
	// the light's STATIC caster count (what the cache removes). 0 = off (every warmed light uses surf).
	// PLAYER-PROXIMITY exactness (r_softShadowNearRadius): a light near the view origin is forced off
	// every cache (contrib + surf) onto the plain exact scanline atlas fill. The caches can serve a
	// stale/under-covered union on a moving camera (the "area fully lit / skipped shadow" defect); the
	// atlas walk is always exact and, if the atlas overflows, the in-shader integral is exact too - so a
	// near light can never be starved. Far lights keep the cache. 0 = off.
	extern idCVar r_softShadowNearRadius;
	const float swNearR = r_softShadowNearRadius.GetFloat();
	const bool nearPlayer = ( swNearR > 0.0f ) && ( vLight != NULL ) &&
							( ( vLight->globalLightOrigin - viewDef->renderView.vieworg ).LengthSqr() <= swNearR * swNearR );
	extern idCVar r_softShadowSurfCacheMinLight;
	const int swLightStatic = ( vLight != NULL ) ? vLight->softStaticCasterCount : 0;
	const bool surf = ( surfCache != NULL ) && surfCache->IsActive() && m_PipelineSurf != nullptr && !m_WalkCntEnabled
					  && !nearPlayer
					  && swLightStatic >= r_softShadowSurfCacheMinLight.GetInteger();
	// FUBINI SCANLINE (r_softShadowScanline): replaces the sampled walk with the interval bit-grid. Loses to
	// the counting (measurement) and surf-cache permutations; A/B lever against the shipped 16-sample path.
	extern idCVar r_softShadowScanline;
	const bool scan = r_softShadowScanline.GetBool() && m_PipelineScan != nullptr && !m_WalkCntEnabled && !surf;
	// CONTRIBUTOR CACHE (r_softShadowContribCache): scanline-only (the record/serve paths fill the
	// Fubini grid; the sampled walk's skip-on-hit loop cannot observe solo contributions). Loses to
	// the counting and surf permutations; requires a static prefix (something to cache) and a light
	// key. Every fall-through inside the permutation is the exact walk.
	extern idCVar r_softShadowContribCache;
	// generation source: the per-light surfCache param is NULL for surf-cold lights (the surf gate),
	// but the invalidation bookkeeping lives on the ALWAYS-constructed global cache object
	SoftShadowSurfCache* swGenSrc = ( surfCache != NULL ) ? surfCache : backEnd.GetSoftShadowSurfCache();
	const bool contrib = r_softShadowContribCache.GetBool() && scan && m_PipelineContrib != nullptr
						 && !nearPlayer
						 && m_ContribBuffer != nullptr && vLight->softStaticCasterCount > 0
						 && vLight->lightDef != NULL && swGenSrc != NULL;
	// SURF-CACHE GRID (r_softShadowSurfCacheGrid): route the cached-hit path to the Fubini bit-grid pipeline.
	// Only when the surf permutation is already selected AND the grid buffer exists (built in grid mode).
	extern idCVar r_softShadowSurfCacheGrid;
	const bool surfGrid = surf && r_softShadowSurfCacheGrid.GetBool() && swTermGrid().pipeline != nullptr
						  && surfCache != nullptr && surfCache->GetGridBuffer() != nullptr	// 2026-08-26: the grid ALLOC can fail (2GB worst-case sized buffer in a loaded-game VRAM context); serving the grid permutation against a null SRV while the build fell back to scalar = garbage terms = black world. The build checks the buffer; the serve must too.
						  && surfCache != NULL && surfCache->GetGridBuffer() != NULL;
	extern idCVar r_softShadowLitEarlyOut, r_softShadowRotGrid;
	cb.surfParams[0] = 0.0f;
	cb.surfParams[1] = 0.0f;
	// z = intensity lit early-out threshold: skip the walk where the light's falloff*projection is
	// below this (-> term 1.0), cutting the far penumbra the light barely reaches. 0 = exact. Forced
	// exact (0) for near-player lights so the guarantee can never blank their far penumbra.
	cb.surfParams[2] = nearPlayer ? 0.0f : r_softShadowLitEarlyOut.GetFloat();
	// w = rotation-hash world grid: snap swP to this grid before the sample-rotation hash so TAA
	// jitter can't flip the 1/16 quantum per frame (temporal-stability fix). 0 = exact per-position.
	cb.surfParams[3] = r_softShadowRotGrid.GetFloat();
	extern idCVar r_softShadowSamples;
	cb.aa[0] = ( float )r_softShadowSamples.GetInteger();	// disk ray count (0 = off, 16 = shipped, else runtime-N)
	cb.aa[1] = cb.aa[2] = cb.aa[3] = 0.0f;
	extern idCVar r_softShadowSurfCacheMinCost;
	cb.surfCost[0] = r_softShadowSurfCacheMinCost.GetInteger();	// cost gate: min tile occluders to engage the cache
	extern idCVar r_softShadowCullBeforeLoad;
	cb.surfCost[1] = r_softShadowCullBeforeLoad.GetBool() ? 1 : 0;	// cull-before-load runtime toggle (walk reads g_surfCost.y)
	extern idCVar r_softShadowTermLevels;
	cb.surfCost[2] = r_softShadowTermLevels.GetInteger();	// DIAGNOSTIC: quantize the stored term to N levels (0 = off)
	extern idCVar r_softShadowScanRotate;
	cb.surfCost[3] = r_softShadowScanRotate.GetInteger();	// GATE POSITIVE CONTROL: 1 re-injects the scanline rotation grain (ants) so GateGrain can be validated
	extern idCVar r_softShadowMinDnRatio;
	cb.misc[0] = r_softShadowMinDnRatio.GetFloat();			// projection dn-clamp: grazing-grain fix (distPL/dn near-contact amplification)
	// cache economics (task #112): view origin + per-fragment near-exact radius + tier-1 gate margin.
	// Both cvars default 0 = the whole block is inert (byte-identical serve behavior).
	cb.misc[1] = viewDef->renderView.vieworg.x;
	cb.misc[2] = viewDef->renderView.vieworg.y;
	cb.misc[3] = viewDef->renderView.vieworg.z;
	{
		extern idCVar r_softShadowNearRadius, r_softShadowSurfCacheGateMargin;
		const float swNearR = r_softShadowNearRadius.GetFloat();
		cb.econ[0] = swNearR * swNearR;
		cb.econ[1] = r_softShadowSurfCacheGateMargin.GetFloat();
		cb.econ[2] = cb.econ[3] = 0.0f;
	}
	// PORTAL-AREA DEAD-WORK CULL mask (r_softShadowAreaCull): bit areaNum set iff the light's portal
	// flood reached that area (lightDef->references chain). A world fragment in an unreached area has no
	// interaction draw under this light, so its term is provably never read - the CS skips the walk.
	// All-ones = cull inert: cvar off, NULL lightDef, empty reference list, or any areaNum >= 128
	// (conservative - never skip on doubt). Fog/blend lights never reach AddLight.
	cb.areaMask[0] = cb.areaMask[1] = cb.areaMask[2] = cb.areaMask[3] = 0xFFFFFFFFu;
	{
		extern idCVar r_softShadowAreaCull;
		if( r_softShadowAreaCull.GetInteger() > 0 && vLight->lightDef != NULL && vLight->lightDef->references != NULL )
		{
			unsigned int swAreaM[4] = { 0u, 0u, 0u, 0u };
			bool swAreaOk = true;
			for( const areaReference_t* ref = vLight->lightDef->references; ref != NULL; ref = ref->ownerNext )
			{
				const int a = ( ref->area != NULL ) ? ref->area->areaNum : -1;
				if( a < 0 || a >= 128 )
				{
					swAreaOk = false;
					break;
				}
				swAreaM[a >> 5] |= 1u << ( a & 31 );
			}
			if( swAreaOk )
			{
				cb.areaMask[0] = swAreaM[0];
				cb.areaMask[1] = swAreaM[1];
				cb.areaMask[2] = swAreaM[2];
				cb.areaMask[3] = swAreaM[3];
			}
		}
	}
	cb.surfA[0] = 0;
	cb.surfA[1] = cb.surfA[2] = cb.surfA[3] = 0;
	if( surf )
	{
		extern idCVar r_softShadowSurfCacheViz;
		cb.surfParams[0] = surfCache->GetTexel();
		cb.surfParams[1] = ( float )r_softShadowSurfCacheViz.GetInteger();
		cb.surfA[0] = ( vLight->softStaticCasterCount > 0 && vLight->softSurfHash != 0 ) ? surfCache->GetTableCap() : 0;
		cb.surfA[1] = surfCache->GetQueueWords();
		cb.surfA[2] = vLight->softStaticCasterCount;
		cb.surfA[3] = ( vLight->lightDef != NULL ) ? ( vLight->lightDef->index & 0x1FFF ) : 0;
		// TERM-LIGHT IDENTITY (bench instrument): record each surf-permutation light's index so the
		// bench can NAME the by-light rings (ring = index & 15; collisions past 16 lights made ring
		// attribution ambiguous exactly when it mattered). One-shot list, reset by the bench reader.
		if( vLight->lightDef != NULL )
		{
			extern void R_SoftSurfTermLightSeen( int index, bool cacheOn );
			R_SoftSurfTermLightSeen( vLight->lightDef->index, cb.surfA[0] != 0 );
		}
		// econ.z = this light's persistent static-stream segment TRI BASE (float4 elems), BIT-CAST into
		// the float lane (asuint on the shader side) so any base stays exact - audit finding #4. The
		// pool's 1-uint residual indices resolve against t_SurfStream at this base. A warmed light
		// always has a segment; if it somehow doesn't (scalar mode), disable the cache for this light
		// rather than serve indices into garbage. Grid mode never reads the stream.
		{
			const int swSegBase = ( vLight->lightDef != NULL ) ? surfCache->GetLightStreamBase( vLight->lightDef->index ) : -1;
			if( swSegBase >= 0 )
			{
				const uint32_t swSegBits = ( uint32_t )swSegBase;
				memcpy( &cb.econ[2], &swSegBits, sizeof( swSegBits ) );
			}
			else if( !surfGrid )
			{
				cb.surfA[0] = 0;
			}
		}
		// per-light generation: the term CS treats a slot whose stored generation != this as stale and
		// reclaims it (a set change bumped the generation instead of wiping the whole table)
		cb.aa[1] = ( vLight->lightDef != NULL ) ? ( float )surfCache->GetLightGeneration( vLight->lightDef->index ) : 0.0f;
		extern idCVar r_softShadowSurfCacheForceWalk;
		cb.aa[2] = r_softShadowSurfCacheForceWalk.GetFloat();	// DEBUG probe-tax isolation -> g_aa.z (1 = force-walk, 2 = skip-probe)
		extern idCVar r_softShadowSurfCacheTileDyn;
		cb.aa[3] = r_softShadowSurfCacheTileDyn.GetFloat();		// tile the hit-path dynamic walk -> g_aa.w (0 = old untiled)
	}
	else if( contrib )
	{
		// contributor-cache fields (the contrib permutation reuses the surf CB slots; the two
		// permutations are mutually exclusive, so the meanings cannot collide at runtime)
		extern idCVar r_softShadowContribG, r_softShadowContribPool, r_softShadowContribCap;
		cb.surfParams[0] = r_softShadowContribG.GetFloat();							// cell size G
		cb.surfParams[1] = ( r_softShadowContribCache.GetInteger() == 2 ) ? 1.0f : 0.0f;	// SERVE-VERIFY mode
		cb.surfCost[3] = r_softShadowContribCap.GetInteger();						// table capacity (slots)
		cb.surfA[1] = ( int )( m_ContribTick & 0x7FFF );							// flip-frame stamp (serve-defer race fix; own tick - tr.frameCount is dead in minimal-init)
		cb.surfA[2] = vLight->softStaticCasterCount;								// dynamic caster suffix start
		cb.surfA[3] = vLight->lightDef->index & 0x1FFF;								// light key
		cb.aa[1] = ( float )swGenSrc->GetLightGeneration( vLight->lightDef->index );	// per-light generation (invalidation)
		cb.aa[2] = ( float )r_softShadowContribPool.GetInteger();					// recording-pool budget (incremental warm-up)
		extern idCVar r_softShadowContribRefine;
		cb.aa[3] = ( float )r_softShadowContribRefine.GetInteger();					// refinement divisor (1-in-N; 0 = off)
		// diagnosis accumulators: the cache can only accelerate the STATIC prefix - the static share
		// of the stream bounds its ceiling (measure-first before widening the classification)
		m_ContribStaticTris += ( uint64_t )vLight->softStaticTriCount;
		m_ContribTotalTris  += ( uint64_t )( vLight->softEdgeCount * 2 / 3 );	// 3 float4 per tri, 2 per edge record
		m_ContribActiveLights++;
		m_ContribFragments  += ( uint64_t )cb.rect[2] * ( uint64_t )cb.rect[3];	// dispatched contrib-kernel threads
	}

	// lit classifier grid (r_softShadowClassify): built at flatten into the joint buffer. base < 0 = none.
	cb.classDims[3] = -1;
	if( vLight->softClassifyDims[3] != 0 && vLight->softClassifyCache != 0 )
	{
		const uint clsOfs = ( uint )( ( vLight->softClassifyCache >> VERTCACHE_OFFSET_SHIFT ) & VERTCACHE_OFFSET_MASK );
		cb.classAabbCell[0] = vLight->softClassifyAabbCell[0];	cb.classAabbCell[1] = vLight->softClassifyAabbCell[1];
		cb.classAabbCell[2] = vLight->softClassifyAabbCell[2];	cb.classAabbCell[3] = vLight->softClassifyAabbCell[3];
		cb.classDims[0] = vLight->softClassifyDims[0];	cb.classDims[1] = vLight->softClassifyDims[1];
		cb.classDims[2] = vLight->softClassifyDims[2];	cb.classDims[3] = ( int )( clsOfs / 16u );	// float4 element base
	}

	// t1 must bind SOMETHING even when this light was not binned (layout demands a resource);
	// tileBase -1 keeps the shader from reading it - mirrors the pixel-shader t13 handling.
	nvrhi::IBuffer* tiles = ( tileBuffer != nullptr ) ? tileBuffer : edgeBuffer;
	// cull-before-load (t7): bind SOMETHING always (layout demands it). Only read on the binned list path,
	// which is inactive when tileBuffer is null, so the edgeBuffer stand-in is never dereferenced there.
	nvrhi::IBuffer* cull = ( tileCullBuffer != nullptr ) ? tileCullBuffer : edgeBuffer;
	// same rule for t3/t4/s0/s1 when the early-out is off (flags.x 0 keeps the shader from
	// sampling them): the caller passes black + any sampler in that case, but guard anyway.
	nvrhi::ITexture* fallT = ( falloffTex != nullptr ) ? falloffTex : m_WorldPos.Get();
	nvrhi::ITexture* projT = ( projTex != nullptr ) ? projTex : m_WorldPos.Get();
	nvrhi::ISampler* fallS = ( falloffSamp != nullptr ) ? falloffSamp : projSamp;
	nvrhi::ISampler* projS = ( projSamp != nullptr ) ? projSamp : falloffSamp;
	if( fallS == nullptr || projS == nullptr )
	{
		return false;							// no sampler at all: caller must supply one
	}

	nvrhi::BindingSetDesc sd;
	sd.bindings =
	{
		nvrhi::BindingSetItem::ConstantBuffer( 0, m_ConstantBuffer ),
		nvrhi::BindingSetItem::StructuredBuffer_SRV( 0, edgeBuffer ),
		nvrhi::BindingSetItem::StructuredBuffer_SRV( 1, tiles ),
		nvrhi::BindingSetItem::Texture_SRV( 2, m_WorldPos ),
		nvrhi::BindingSetItem::Texture_SRV( 3, fallT ),
		nvrhi::BindingSetItem::Texture_SRV( 4, projT ),
		nvrhi::BindingSetItem::Texture_SRV( 5, m_WorldNormal ? m_WorldNormal.Get() : m_WorldPos.Get() ),
		nvrhi::BindingSetItem::StructuredBuffer_SRV( 7, cull ),
		nvrhi::BindingSetItem::Sampler( 0, fallS ),
		nvrhi::BindingSetItem::Sampler( 1, projS ),
		nvrhi::BindingSetItem::Texture_UAV( 0, m_TermTexture ),
	};
	// WALK-COUNTERS: the counting permutation needs u1 in its binding set + layout; the shipped path
	// uses neither. Everything else is identical. The SURF permutation instead adds the cache
	// table/queue UAVs + residual pool SRV (counters and surf are mutually exclusive, counters win).
	const bool cnt = m_WalkCntEnabled;
	if( cnt )
	{
		sd.bindings.push_back( nvrhi::BindingSetItem::StructuredBuffer_UAV( 1, m_WalkCntBuffer ) );
	}
	else if( surf )
	{
		sd.bindings.push_back( nvrhi::BindingSetItem::StructuredBuffer_UAV( 2, surfCache->GetTable() ) );
		/* u3 request-queue binding removed: the read-only term never enqueues (the burst seeds it) */;
		// t6 = residual pool (scalar) OR the static bit-grid (grid mode); same register, same layout.
		sd.bindings.push_back( nvrhi::BindingSetItem::StructuredBuffer_SRV( 6, surfGrid ? surfCache->GetGridBuffer() : surfCache->GetPool() ) );
		// t8 = persistent static stream (audit finding #4): residual indices resolve here. The layout
		// demands a resource even in grid mode / before any warm - edgeBuffer stands in (never read then).
		nvrhi::IBuffer* swStream = surfCache->GetStaticStream();
		sd.bindings.push_back( nvrhi::BindingSetItem::StructuredBuffer_SRV( 8, ( swStream != nullptr ) ? swStream : edgeBuffer ) );
	}
	else if( contrib )
	{
		if( !m_ContribCleared )
		{
			// lazy one-time clear: empty keys are the probe's vacancy sentinel, so the table must
			// start zeroed. Generations handle set-change invalidation from then on.
			commandList->clearBufferUInt( m_ContribBuffer, 0 );
			m_ContribCleared = true;
		}
		if( m_ContribClaimFrame != m_ContribTick )
		{
			// per-FRAME claim budget: header[0] counts claims THIS FRAME and resets here. The old
			// held-until-flip ticket model clogged (sub-K cells held tickets forever, pool pinned at
			// budget, warm-up never settled and probe pairs kept straddling transitions). A per-frame
			// budget keeps warm-up incremental with no clog; steady recording load stays bounded by
			// how many cells are below K, which geometry bounds.
			m_ContribClaimFrame = m_ContribTick;
			const uint32_t zero = 0;
			commandList->writeBuffer( m_ContribBuffer, &zero, sizeof( zero ), 0 );
		}
		sd.bindings.push_back( nvrhi::BindingSetItem::StructuredBuffer_UAV( 2, m_ContribBuffer ) );
	}
	nvrhi::BindingSetHandle set = m_Device->createBindingSet( sd, cnt ? m_LayoutCnt : ( surf ? m_LayoutSurf : ( contrib ? m_LayoutContrib : m_Layout ) ) );

	commandList->writeBuffer( m_ConstantBuffer, &cb, sizeof( cb ) );
	nvrhi::ComputeState cs;
	cs.pipeline = cnt ? m_PipelineCnt : ( surfGrid ? swTermGrid().pipeline : ( surf ? m_PipelineSurf : ( contrib ? m_PipelineContrib : ( scan ? m_PipelineScan : m_Pipeline ) ) ) );
	cs.bindings = { set };
	commandList->setComputeState( cs );
	commandList->dispatch( ( cb.rect[2] + 7 ) / 8, ( cb.rect[3] + 3 ) / 4, 1 );

	outOfsX = slotOfsX;
	outOfsY = slotOfsY;
	return true;
}

void SoftShadowTermPass::BlurView( nvrhi::ICommandList* commandList )
{
	if( !m_Valid || !m_BlurActive || m_BlurTexture == nullptr || m_BlurPipeline == nullptr || m_BlurRects.Num() == 0 )
	{
		return;
	}
	extern idCVar r_softShadowTermBlur, r_softShadowTermBlurScale;
	// the term dispatches wrote m_TermTexture as UAV; read it as SRV for the blur, write the blur atlas.
	commandList->setTextureState( m_TermTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource );
	commandList->setTextureState( m_BlurTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess );
	commandList->commitBarriers();

	for( int i = 0; i < m_BlurRects.Num(); i++ )
	{
		const BlurRect& r = m_BlurRects[i];
		SoftBlurCB cb;
		cb.rect[0] = r.x; cb.rect[1] = r.y; cb.rect[2] = r.w; cb.rect[3] = r.h;
		cb.blur[0] = r_softShadowTermBlur.GetFloat();		// max radius (pixels)
		cb.blur[1] = r_softShadowTermBlurScale.GetFloat();	// penumbra-width -> radius scale
		cb.blur[2] = 0.02f;									// penumbra band lo (skip deep umbra)
		cb.blur[3] = 0.98f;									// penumbra band hi (skip full lit)
		commandList->writeBuffer( m_BlurCB, &cb, sizeof( cb ) );

		nvrhi::BindingSetDesc bsd;
		bsd.bindings =
		{
			nvrhi::BindingSetItem::ConstantBuffer( 0, m_BlurCB ),
			nvrhi::BindingSetItem::Texture_SRV( 0, m_TermTexture ),
			nvrhi::BindingSetItem::Texture_UAV( 0, m_BlurTexture ),
		};
		nvrhi::BindingSetHandle bs = m_Device->createBindingSet( bsd, m_BlurLayout );

		nvrhi::ComputeState st;
		st.pipeline = m_BlurPipeline;
		st.bindings = { bs };
		commandList->setComputeState( st );
		commandList->dispatch( ( r.w + 7 ) / 8, ( r.h + 7 ) / 8, 1 );
	}
	// hand the blur atlas to the interaction pass as an SRV.
	commandList->setTextureState( m_BlurTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource );
	commandList->commitBarriers();
}

bool SoftShadowTermPass::GetContribStats( uint32_t out[20] )
{
	if( m_ContribBuffer == nullptr || !m_ContribCleared )
	{
		return false;
	}
	nvrhi::BufferDesc sbd;
	sbd.byteSize = 20 * sizeof( uint32_t );
	sbd.cpuAccess = nvrhi::CpuAccessMode::Read;
	sbd.debugName = "SoftShadowTerm/ContribStatsReadback";
	nvrhi::BufferHandle staging = m_Device->createBuffer( sbd );
	nvrhi::CommandListHandle cl = m_Device->createCommandList();
	cl->open();
	cl->copyBuffer( staging, 0, m_ContribBuffer, 0, 20 * sizeof( uint32_t ) );
	cl->close();
	m_Device->executeCommandList( cl );
	m_Device->waitForIdle();
	void* p = m_Device->mapBuffer( staging, nvrhi::CpuAccessMode::Read );
	if( p == nullptr )
	{
		return false;
	}
	memcpy( out, p, 20 * sizeof( uint32_t ) );
	m_Device->unmapBuffer( staging );
	return true;
}

bool SoftShadowTermPass::GetWalkStats( uint32_t out[28] )
{
	if( !m_WalkCntEnabled || m_WalkCntBuffer == nullptr )
	{
		return false;
	}
	nvrhi::BufferDesc sbd;
	sbd.byteSize = 28 * sizeof( uint32_t );
	sbd.cpuAccess = nvrhi::CpuAccessMode::Read;
	sbd.debugName = "SoftShadowTerm/WalkCountersReadback";
	nvrhi::BufferHandle staging = m_Device->createBuffer( sbd );
	nvrhi::CommandListHandle cl = m_Device->createCommandList();
	cl->open();
	cl->copyBuffer( staging, 0, m_WalkCntBuffer, 0, 28 * sizeof( uint32_t ) );
	cl->close();
	m_Device->executeCommandList( cl );
	m_Device->waitForIdle();
	void* p = m_Device->mapBuffer( staging, nvrhi::CpuAccessMode::Read );
	if( p == nullptr )
	{
		return false;
	}
	memcpy( out, p, 28 * sizeof( uint32_t ) );
	m_Device->unmapBuffer( staging );
	return true;
}
