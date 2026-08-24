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

#ifndef __SOFT_TILE_BIN_PASS_H__
#define __SOFT_TILE_BIN_PASS_H__

#include <nvrhi/nvrhi.h>

struct viewDef_t;
struct viewLight_t;

// Analytic soft shadows: per-light, per-16x16-tile TRIANGLE BINNING compute prepass
// (softtile_bin.cs.hlsl). For each tile of the light's scissor it culls the light's face-stream
// triangles against the tile's depth-bounded receiver volume and writes the surviving pair-start
// indices; the interaction pixel shader then walks only its tile's list (t13, rpUser7). The
// measured per-fragment record walk + triangle culls were 69% of the soft-shadow frame cost.
// Everything is conservative: tile overflow and out-of-buffer lights fall back to the full walk.
class SoftTileBinPass
{
public:
	explicit SoftTileBinPass( nvrhi::IDevice* device );

	// Once per view, BEFORE any BinLight: resets the tile-buffer cursor and dispatches the SHARED
	// per-screen-tile depth min/max reduce every light's bin pass reads (one reduce instead of one
	// per light x tile). Requires the depth prepass to be complete.
	void BeginView( nvrhi::ICommandList* commandList, const viewDef_t* viewDef, nvrhi::ITexture* depthTexture );

	// Dispatch binning for one soft light (stream v2: pure tri stream + caster table, both float4
	// element offsets into the same joint buffer). Returns the tile-list base (uint element index
	// into the tile buffer) and fills the tile rect the pixel shader needs, or -1 when binning is
	// unavailable (no pipeline, buffer full, degenerate scissor) - callers then use the full walk.
	int BinLight( nvrhi::ICommandList* commandList, const viewDef_t* viewDef, const viewLight_t* vLight,
				  nvrhi::IBuffer* edgeBuffer, uint32_t edgeFirstElem,
				  uint32_t casterFirstElem, int numCasters,
				  float penumbraRadius,
				  int& outTileOx, int& outTileOy, int& outTilesX );

	nvrhi::IBuffer* GetTileBuffer() const
	{
		return m_TileBuffer;
	}

	// CULL-BEFORE-LOAD parallel buffer (float4 centroid+triRad per tile-list slot); term walk reads it
	// to cull before scatter-loading verts. See SoftTileBinPass.cpp for the rationale.
	nvrhi::IBuffer* GetTileCullBuffer() const
	{
		return m_TileCullBuffer;
	}

	// blocking readback of the spill allocator/stats: [0] total demand (uint elements),
	// [1] overflow-tile count, [2] max per-tile survivor count. Bench/diagnostic only.
	bool GetSpillStats( uint32_t out[4] )
	{
		return DebugReadSpillStats( out );
	}

	static const int TILE_SIZE = 16;			// 16x16. History below - read it before "trying 8x8".
	static const int TILE_K = 512;			// MAX indices per tile (slot cap); must match softtile_bin.cs.hlsl + interactionSM.ps.hlsl. Runtime STRIDE is r_softShadowTileK (default 256 at 16x16).
	// TILE-SIZE HISTORY (candid, both reversals): 16x16 -> 8x8 (2026-08-24, "net win -12..-31%") ->
	// BACK to 16x16 (same day). The 8x8 "win" was measured on a bench whose multi-cap runs were
	// CORRUPTED by cross-capture state leakage (the gate reused the render world; re-added lights fell
	// off the interactionTable to per-frame dynamic interactions - up to 14x fabricated cost, see
	// RenderCapture.cpp per-cap isolation). On the FIXED per-cap-isolated bench, 16x16 at K=256 beats
	// 8x8 at its best K (128) on 4 of 5 heavy caps (cap0006 52.1->37.7, cap0008 48.2->37.1 ms net
	// soft; cap0009 equal) and pays 2.5-5 ms tilebin instead of 6.5-14. The honest 16x16 K sweep:
	// 256 best, 512 worse on most heavy caps, 128 worse. LESSON: a documented negative OR positive is
	// only as good as the instrument that produced it; re-measure both after any instrument fix.
	// SPILL region (2026-08-17): tiles denser than K no longer fall back to the O(all-casters) full
	// walk (measured ~12 ms/frame at cap0061 live density - K cannot chase it: K=1024 recovered
	// only ~4 ms at +320 MB). Instead the bin CS bump-allocates a span from the buffer TAIL and
	// writes the tile's FULL index list there (SW_TILE_SPILL sentinel + span descriptor in the tile
	// slot); consumers walk the span like a normal list. Exhausted region -> old sentinel fallback.
	// Spill partition of the SAME 48M buffer (no extra VRAM). STREAM V3: overflowed tiles spill
	// CLUSTER records (k-d leaves of <=32 tris), so the measured cap0061 1080p demand collapsed
	// from 19.4M tri entries to 1.45M cluster entries (7670 overflow tiles, worst 334 clusters) -
	// 24M is now deep headroom even for 4K. Both halves degrade gracefully (slots: light falls to
	// full walk; spill: tile falls to full walk) and the gate bench prints the spill demand so
	// exhaustion is never silent.
	static const int SPILL_ELEMENTS = 56 << 20;	// sized generously (worst measured demand: ~47 M at the 8x8/K128 experiment; 16x16/K256 demands far less) so dense scenes never EXHAUST the region and fall back to the full walk. Main region keeps 40 M - at 16x16 K256 a full-screen 1440p light needs ~3.7 M slots, so ~10 lights of headroom before the graceful full-walk fallback.

private:
	void EnsurePipeline();
	bool DebugReadSpillStats( uint32_t out[4] );

	nvrhi::DeviceHandle				m_Device;
	bool							m_PipelineTried = false;
	nvrhi::ShaderHandle				m_Shader;
	nvrhi::BindingLayoutHandle		m_Layout;
	nvrhi::ComputePipelineHandle	m_Pipeline;
	nvrhi::BufferHandle				m_ConstantBuffer;
	nvrhi::BufferHandle				m_TileBuffer;
	nvrhi::BufferHandle				m_TileCullBuffer;	// parallel float4 (centroid, triRad) per tile-list slot for cull-before-load
	nvrhi::BufferHandle				m_SpillCounter;	// 1-uint global bump allocator for the spill tail (cleared per view)
	nvrhi::ShaderHandle				m_MinMaxShader;
	nvrhi::BindingLayoutHandle		m_MinMaxLayout;
	nvrhi::ComputePipelineHandle	m_MinMaxPipeline;
	nvrhi::BufferHandle				m_MinMaxCB;
	nvrhi::BufferHandle				m_MinMaxBuffer;
	int								m_MinMaxTilesX = 0;	// screen-tile row stride of the last reduce
	bool							m_MinMaxValid = false;
	int								m_Cursor = 0;	// uint elements allocated this view
};

#endif // __SOFT_TILE_BIN_PASS_H__
