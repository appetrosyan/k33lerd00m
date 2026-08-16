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

	// Dispatch binning for one soft light. Returns the tile-list base (uint element index into the
	// tile buffer) and fills the tile rect the pixel shader needs, or -1 when binning is
	// unavailable (no pipeline, buffer full, degenerate scissor) - callers then use the full walk.
	int BinLight( nvrhi::ICommandList* commandList, const viewDef_t* viewDef, const viewLight_t* vLight,
				  nvrhi::IBuffer* edgeBuffer, uint32_t edgeFirstElem,
				  nvrhi::IBuffer* pairBuffer, uint32_t pairFirstElem, int numPairs,
				  float penumbraRadius,
				  int& outTileOx, int& outTileOy, int& outTilesX );

	nvrhi::IBuffer* GetTileBuffer() const
	{
		return m_TileBuffer;
	}

	static const int TILE_SIZE = 16;
	static const int TILE_K = 256;			// indices per tile; must match softtile_bin.cs.hlsl + interactionSM.ps.hlsl.
	// Measured (erebus1_05/07/09): K=64 overflowed the DENSE tiles - exactly the expensive ones -
	// back to the full walk, erasing the win on heavy scenes; K=256 resolves every overflow there
	// and K=512 changes nothing further (the residual cost is real per-tile sample work).
	// 8x8 tiles MEASURED WORSE (17.9/22.1/23.8 vs 16.7/19.6/20.1 ms on erebus1_05/07/09): 4x the
	// prepass and per-tile list overhead, while the dense tiles' relevant sets barely shrink - a
	// triangle near one tile is near its neighbours too. Do not retry without a new idea.

private:
	void EnsurePipeline();

	nvrhi::DeviceHandle				m_Device;
	bool							m_PipelineTried = false;
	nvrhi::ShaderHandle				m_Shader;
	nvrhi::BindingLayoutHandle		m_Layout;
	nvrhi::ComputePipelineHandle	m_Pipeline;
	nvrhi::BufferHandle				m_ConstantBuffer;
	nvrhi::BufferHandle				m_TileBuffer;
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
