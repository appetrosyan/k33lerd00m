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

#ifndef __SOFT_SHADOW_TERM_PASS_H__
#define __SOFT_SHADOW_TERM_PASS_H__

#include <nvrhi/nvrhi.h>

struct viewDef_t;
struct viewLight_t;
class SoftShadowSurfCache;

// Analytic soft shadows: per-soft-light COMPUTE evaluation of the coverage integral
// (r_softShadowCompute, softterm.cs.hlsl). One dispatch per light over its scissor rect, reading
// the EXACT receiver world positions from the softShadowPos G-buffer (softpos.{vs,ps}.hlsl at
// depth-EQUAL - never reconstructed from depth) and the SoftTileBinPass tile lists, writing the
// visibility term into an R32F atlas of screen-size slots (one per light). The interaction pixel
// shader then Loads its light's term texel (rpUser6 selects mode + slot offset) instead of running
// the integral per fragment in wave64. 32-thread workgroups (8x4) so RDNA3 picks wave32 (VOPD
// dual-issue) - VERIFIED 2026-08-17 in the final ISA (s_and_saveexec_b32 exec masks, v_dual_*
// pairs through the packed-fp16 sample loop); this vendored nvrhi exposes no
// VK_EXT_subgroup_size_control, so this rides RADV's small-workgroup heuristic rather than an
// explicit requirement - re-verify the disasm if the driver or workgroup size ever changes.
// Everything is conservative: no pipeline / atlas too large for the device / out of slots => the
// caller leaves the light on the in-shader integral (bit-exact by design either way).
class SoftShadowTermPass
{
public:
	explicit SoftShadowTermPass( nvrhi::IDevice* device );

	// Once per view, AFTER the softpos pass drew and AFTER SoftTileBinPass::BeginView/BinLight:
	// resets the slot cursor, (re)creates the term atlas for the current render size and barriers
	// the position G-buffer to ShaderResource (same explicit-transition precedent as the tile-bin
	// depth read). Returns false when the pass cannot run this view (no pipeline / atlas exceeds
	// the device texture limit).
	bool BeginView( nvrhi::ICommandList* commandList, const viewDef_t* viewDef, nvrhi::ITexture* worldPosTexture, nvrhi::ITexture* worldNormalTexture );

	// Dispatch the coverage integral for one soft light over its scissor rect into the next free
	// atlas slot. tileBase/tileOx/tileOy/tilesX are this light's SoftTileBinPass result (base -1 =
	// not binned => the shader runs the full walk - bit-exact fallback, same as the pixel shader).
	// On success fills the slot's pixel offset (add to SV_Position to address the atlas) and
	// returns true; false = out of slots (light stays on the in-shader integral).
	// falloffTex/projTex + their samplers drive the coverage early-out (the fix for the measured
	// 1.8x scissor-overcoverage loss); coverageEarlyOut false (debug shaders active, multi-stage
	// light shader, stage texture matrix) integrates the full rect - the bit-exact instrument mode.
	// surfCache non-null + active selects the SW_SURF_CACHE permutation (surface-fold cache probe):
	// cached texels short-circuit to bilerp + residual walk, misses run the shipped walk and enqueue.
	// The walk-counters permutation takes precedence (measurement mode); pass NULL for shipped.
	bool AddLight( nvrhi::ICommandList* commandList, const viewDef_t* viewDef, const viewLight_t* vLight,
				   nvrhi::IBuffer* edgeBuffer, uint32_t edgeFirstElem,
				   uint32_t casterFirstElem, int casterCount,
				   float penumbraRadius,
				   int tileBase, int tileOx, int tileOy, int tilesX,
				   int tileFaceBase,			// mode-3 face bin for the softrepair arbiter (-1 = none)
				   nvrhi::IBuffer* tileBuffer,
				   nvrhi::IBuffer* tileCullBuffer,
				   nvrhi::ITexture* falloffTex, nvrhi::ISampler* falloffSamp,
				   nvrhi::ITexture* projTex, nvrhi::ISampler* projSamp,
				   bool coverageEarlyOut,
				   SoftShadowSurfCache* surfCache,
				   int& outOfsX, int& outOfsY );

	// Walk-attribution GPU counters (r_softShadowWalkCounters; the gate bench also forces 2 untimed
	// counting frames after the timed loop, so one bench run always yields these): read the 28-uint
	// counter buffer the COUNTING permutation (SW_SCANLINE=1 = the shipped algorithm; surf/contrib
	// cache pipelines bypassed) accumulated this frame. Slots 0..6 = SW_WALKIDX_* fed by ALL plain
	// walkers - tile-list (tight+mtTri), spill/cluster (sphere->caster slots + coarse + tight +
	// mtTri), unbinned-full (all); 7 = fragments that ran the walk. Blocking readback (waitForIdle),
	// bench-only. Returns false when counters were not enabled / buffer absent.
	bool GetWalkStats( uint32_t out[34] );	// 0..7 attrib, 8-13 buckets by FINAL COVERAGE (lit cov==0 / penumbra / umbra cov>=0.99 incl. umbra-sentinel tiles; frags + FillTri survivors each), 14/15 MT hit/miss (SAMPLED walk only - stay 0 under scanline, the bench prints the FillTri outcome split instead), 16..19 tile-class census (umbra-sentinel / empty-list / spill / listed threads), 20..26 scanline FillTri attribution (fill/reject/skip/skipIter/foldIter/sweepTri/sweepIter), 27 lit-early-out skips at T>0 (path-execution proof), 28 SUM of adaptive chord count N over walked frags (task #85; mean N = slot28/slot7; 0 when r_softShadowAdaptiveChords off), 29 classifier-UMBRA skips (frags the world-cell umbra class skipped before the walk), 30..33 hybrid-whitelist census (frags with a wedge block / SUM cut casters / SUM table casters / fully-serveable frags)
	bool GetContribStats( uint32_t out[20] );	// contrib-cache header: [0] recording pool [1] serves [2] recording evals [3] claimed cells [4] verify compares [5] verify mismatches [6] max |diff| (float bits) [7] budget-blocked [8] probe-exhausted [9] BUILT-unservable [10] refinement records
	bool GetRepairStats( uint32_t out[16] );	// repair census (r_softShadowRepairStats): [0..7] phase 1 (turds), [8..15] phase 2 (holes). Slots per phase: 0 groupsDispatched / 1 groupsBailed / 2 candPixels / 3 groupsRingSurvive / 4 arbiterListedBin / 5 arbiterCluster / 6 arbiterFallback / 7 arbiterAgreed. Blocking readback.

	// once per rendered VIEW, before the AddLight loop: advances the contributor cache's own frame
	// tick (tr.frameCount is dead in minimal-init paths) for the flip-defer stamp + claim budget
	void ContribNewView()
	{
		m_ContribTick++;
	}

	// The atlas the interaction shader should Load: the BLURRED atlas when the temporal-stability blur
	// ran this view (r_softShadowTermBlur), else the raw term atlas.
	nvrhi::ITexture* GetTermTexture() const
	{
		return ( m_BlurActive && m_BlurTexture != nullptr ) ? m_BlurTexture : m_TermTexture;
	}

	// After the last AddLight: depth-proportional Gaussian blur of each packed light rect in the term
	// atlas into the blur atlas (softblur.cs.hlsl), a temporal-stability pass. No-op unless
	// r_softShadowTermBlur > 0. Call before the interaction pass Loads the term.
	void BlurView( nvrhi::ICommandList* commandList );

	// Atlas slot grid: r_softShadowTermSlotCols x r_softShadowTermSlotRows screen-size R16F slots
	// (runtime cvars, defaults 4x3; snapshotted per view in BeginView, clamped to the device texture
	// limit). R16F is BIT-EXACT for the face-coverage term, not merely tolerable: the term is always
	// popcount/SW_FACE_SAMPLES = k/16 (k=0..16), plus the early-outs writing exactly 0.0 and 1.0 -
	// every k/16 is k*2^-4, which fp16 represents exactly (<=4 mantissa bits), and the interaction
	// reads it via Load (no filtering, no bleed). So the anaTerm-hash A/B stays identical while the
	// atlas HALVES (~235 MB -> ~118 MB at 1440p) and the per-pixel term write + interaction read
	// bandwidth halve - a real win on bandwidth-bound / lower-VRAM hardware. The scissor packer
	// shelves light rects into the grid area; lights beyond the budget keep the in-shader integral.

private:
	void EnsurePipeline();

	nvrhi::DeviceHandle				m_Device;
	bool							m_PipelineTried = false;
	nvrhi::ShaderHandle				m_Shader;
	nvrhi::BindingLayoutHandle		m_Layout;
	nvrhi::ComputePipelineHandle	m_Pipeline;
	// TILE-GROUP permutation (SW_TILE_GROUP=1, task #124, r_softShadowTileGroup): 16x16 thread groups
	// + groupshared cache of the tile's wedge edge stream, selected when whitelist == 3 so the wedge
	// term walks LDS instead of SRV. Reuses m_Layout (same bindings). Byte-identical output vs the
	// SRV path when losslessness holds (fallback for pool overflow / spill / degrade).
	nvrhi::ShaderHandle				m_ShaderTG;
	nvrhi::ComputePipelineHandle	m_PipelineTG;
	// COUNTING permutation (SW_GPU_WALK_COUNTERS=1): a separate shader/layout/pipeline with an extra
	// u1 counter UAV, used only when r_softShadowWalkCounters is set so the shipped pipeline above is
	// byte-identical. m_WalkCntEnabled snapshots the cvar per view.
	nvrhi::ShaderHandle				m_ShaderCnt;
	nvrhi::BindingLayoutHandle		m_LayoutCnt;
	nvrhi::ComputePipelineHandle	m_PipelineCnt;
	// SURFACE-FOLD CACHE permutation (SW_SURF_CACHE=1, r_softShadowSurfCache): adds the texel table
	// (u2) + request queue (u3) UAVs and the residual pool SRV (t6). Separate pipeline so the shipped
	// shader stays byte-identical; used only when the probe is active.
	nvrhi::ShaderHandle				m_ShaderSurf;
	nvrhi::BindingLayoutHandle		m_LayoutSurf;
	nvrhi::ComputePipelineHandle	m_PipelineSurf;
	// SURF-CACHE GRID permutation (SW_SURF_CACHE=1 + SW_SURF_GRID=1 + SW_SCANLINE=1, r_softShadowSurfCacheGrid)
	// lives in FILE-SCOPE STATICS in the .cpp, NOT as members here: growing this class shifts its heap layout
	// and can surface a latent init-time heap fault (the idImageManager-class landmine). Reuses m_LayoutSurf.
	// FUBINI SCANLINE permutation (SW_SCANLINE=1, r_softShadowScanline): the list walk fills an 8x32
	// interval bit-grid (exact 1D union per chord) instead of the 16-sample mask. Same bindings as the
	// shipped path, so it reuses m_Layout; separate pipeline keeps the shipped one byte-identical.
	nvrhi::ShaderHandle				m_ShaderScan;
	nvrhi::ComputePipelineHandle	m_PipelineScan;
	// WEDGE-REPAIR (r_softShadowWedgeWhitelist 3): image-space ant/turd removal over the raw wedge
	// term (softrepair.cs.hlsl, REPAIR_PHASE 0 = ants / 1 = turds), dispatched after the term write.
	nvrhi::ShaderHandle				m_ShaderRepairAnts;
	nvrhi::ComputePipelineHandle	m_PipelineRepairAnts;
	nvrhi::ShaderHandle				m_ShaderRepairTurds;
	nvrhi::ComputePipelineHandle	m_PipelineRepairTurds;
	nvrhi::ShaderHandle				m_ShaderRepairHoles;	// REPAIR_PHASE 2: lit-in-umbra (a hole in the shadow)
	nvrhi::ComputePipelineHandle	m_PipelineRepairHoles;
	// REPAIR STATS permutation (SW_REPAIR_STATS=1, r_softShadowRepairStats): same three phases +
	// a u1 UAV that InterlockedAdd's 8 density/arbiter counters per phase (16 uints total). Own layout
	// (base + u1 UAV). Used only when the census cvar is set so the shipped pipeline stays byte-identical.
	nvrhi::ShaderHandle				m_ShaderRepairAntsStats;
	nvrhi::ShaderHandle				m_ShaderRepairTurdsStats;
	nvrhi::ShaderHandle				m_ShaderRepairHolesStats;
	nvrhi::BindingLayoutHandle		m_LayoutRepStats;
	nvrhi::ComputePipelineHandle	m_PipelineRepairAntsStats;
	nvrhi::ComputePipelineHandle	m_PipelineRepairTurdsStats;
	nvrhi::ComputePipelineHandle	m_PipelineRepairHolesStats;
	nvrhi::BufferHandle				m_RepairStatsBuffer;	// 16 uints (8 per phase, phases 1 & 2)
	bool							m_RepairStatsEnabled = false;
	bool							m_RepairStatsCleared = false;
	// FUSED REPAIR (r_softShadowRepairFused, softrepair_fused.cs.hlsl): single 16x16 dispatch that
	// runs ants + turds + holes with LDS barriers between stages. Shipped path stays untouched.
	nvrhi::ShaderHandle				m_ShaderRepairFused;
	nvrhi::ComputePipelineHandle	m_PipelineRepairFused;
	nvrhi::ShaderHandle				m_ShaderRepairFusedStats;
	nvrhi::ComputePipelineHandle	m_PipelineRepairFusedStats;
	// CONTRIBUTOR CACHE permutation (SW_CONTRIB_CACHE=1, scanline forced): evaluate-once union of
	// observed contributors per (world cell, light). Own layout (base + u2 table UAV); own buffer.
	nvrhi::ShaderHandle				m_ShaderContrib;
	nvrhi::BindingLayoutHandle		m_LayoutContrib;
	nvrhi::ComputePipelineHandle	m_PipelineContrib;
	nvrhi::BufferHandle				m_ContribBuffer;
	bool							m_ContribCleared = false;	// lazy first-use clear (needs a command list)
	uint64_t						m_ContribClaimFrame = ~0ull;	// last TICK the per-frame claim counter was reset
	uint64_t						m_ContribTick = 0;				// own per-view frame tick (see ContribNewView)
public:
	uint64_t						m_ContribStaticTris = 0;		// diagnosis: static-prefix tris accumulated over AddLights
	uint64_t						m_ContribTotalTris = 0;			// diagnosis: total stream tris accumulated over AddLights
	uint64_t						m_ContribActiveLights = 0;		// diagnosis: AddLight calls that took the contrib pipeline
	uint64_t						m_ContribFragments = 0;			// diagnosis: contrib-kernel threads dispatched (scissor area sum)
private:
	nvrhi::BufferHandle				m_WalkCntBuffer;	// 8 uints, cleared per view, InterlockedAdd'd by the shader
	bool							m_WalkCntEnabled = false;
	nvrhi::BufferHandle				m_ConstantBuffer;
	nvrhi::TextureHandle			m_TermTexture;
	int								m_BuiltSamples = 0;		// SW_FACE_SAMPLES the pipelines were built for (r_softShadowSamples); rebuild on change
	int								m_BuiltChords = 0;		// SW_SCAN_CHORDS the pipelines were built for (r_softShadowScanChords); rebuild on change
	int								m_BuiltAdapt = -1;		// SW_ADAPT_CHORDS the pipelines were built for (r_softShadowAdaptiveChords); rebuild on change (-1 = unbuilt)
	// TEMPORAL-STABILITY BLUR (r_softShadowTermBlur, softblur.cs.hlsl): a separate pipeline that
	// Gaussian-blurs each packed light rect of the term atlas into m_BlurTexture with a radius scaled
	// by the local penumbra width. Off by default; the interaction reads m_BlurTexture only when it ran.
	nvrhi::ShaderHandle				m_BlurShader;
	nvrhi::BindingLayoutHandle		m_BlurLayout;
	nvrhi::ComputePipelineHandle	m_BlurPipeline;
	nvrhi::BufferHandle				m_BlurCB;
	nvrhi::TextureHandle			m_BlurTexture;
	bool							m_BlurActive = false;	// snapshot per view; drives GetTermTexture
	struct BlurRect { int x, y, w, h; };
	idList<BlurRect>				m_BlurRects;			// this view's packed light rects (filled by AddLight)
	nvrhi::TextureHandle			m_WorldPos;		// this view's position G-buffer (set by BeginView)
	nvrhi::TextureHandle			m_WorldNormal;	// this view's shading-normal G-buffer (N.L early-out)
	int								m_SlotW = 0;	// screen-size extent the atlas was built for (atlas = SlotW*COLS x SlotH*ROWS)
	int								m_SlotH = 0;
	int								m_Cursor = 0;	// lights packed this view (provenance / debug only)
	// Shelf packer state (reset per view): lights are placed scissor-sized, left-to-right on shelves
	// that grow downward, so the atlas holds far more than cols*rows lights when scissors
	// are sub-screen (the measured "many small lights" case). Replaces the fixed full-screen grid.
	int								m_ShelfX = 0;	// next free x on the current shelf
	int								m_ShelfY = 0;	// current shelf top
	int								m_ShelfH = 0;	// current shelf height (tallest rect placed on it)
	bool							m_Valid = false;
};

#endif // __SOFT_SHADOW_TERM_PASS_H__
