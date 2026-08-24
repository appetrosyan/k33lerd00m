/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2013-2020 Robert Beckebans

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").

Doom 3 BFG Edition Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 BFG Edition Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 BFG Edition Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 BFG Edition Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 BFG Edition Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/
#ifndef __RENDERLOG_H__
#define __RENDERLOG_H__

/*
================================================================================================
Contains the RenderLog declaration.
================================================================================================
*/


enum renderLogMainBlock_t
{
	// each block will require to allocate 2 GPU query timestamps
	MRB_GPU_TIME,
	MRB_BEGIN_DRAWING_VIEW,
	MRB_FILL_DEPTH_BUFFER,
	MRB_FILL_HIZ_BUFFER,
	MRB_FILL_GEOMETRY_BUFFER,
	MRB_SSAO_PASS,
	MRB_AMBIENT_PASS,
	MRB_SHADOW_ATLAS_PASS,
	MRB_DRAW_INTERACTIONS,
	MRB_DRAW_SHADER_PASSES,
	MRB_FOG_ALL_LIGHTS,
	MRB_BLOOM,
	MRB_DRAW_SHADER_PASSES_POST,
	MRB_DRAW_DEBUG_TOOLS,
	MRB_CAPTURE_COLORBUFFER,
	MRB_MOTION_VECTORS,
	MRB_TAA,
	MRB_TONE_MAP_PASS,
	MRB_POSTPROCESS,
	MRB_DRAW_GUI,
	MRB_CRT_POSTPROCESS,
	MRB_TOTAL,

	MRB_TOTAL_QUERIES = MRB_TOTAL * 2,
};

// Shadow-generation sub-timers. Unlike the main blocks these are issued many times per
// frame (once per shadow-casting light, and interleaved with lighting for stencil), so a
// single begin/end pair can't measure them - we pool a fixed number of query objects and
// sum the resolved deltas per kind. See idRenderLog::BeginShadowGen.
enum renderLogShadowGen_t
{
	RLS_STENCIL,		// stencil shadow-volume clear + extrusion
	RLS_SHADOWMAP,		// per-light shadow map (only when the shadow atlas is off)
	RLS_RTMASK,			// ray-traced shadow visibility mask
	RLS_SOFT,			// analytic soft-shadow interaction draw (per-fragment silhouette coverage)
	RLS_TOTAL,
};

// ponytail: fixed pool, capped per frame. 48 segments covers ~24 stencil lights (2 each);
// beyond that the extra shadow work goes untimed (undercount) rather than growing the pool.
static const int MAX_SHADOWGEN_SEGMENTS = 48;



/*
================================================
idRenderLog

// Performance Events abstraction layer for OpenGL, Vulkan, DX12
// see https://devblogs.nvidia.com/best-practices-gpu-performance-events/
================================================
*/
class idRenderLog
{
private:
	renderLogMainBlock_t mainBlock;

	nvrhi::CommandListHandle		commandList;

	uint64							frameCounter;
	uint32							frameParity;

	idStaticList<nvrhi::TimerQueryHandle, MRB_TOTAL* NUM_FRAME_DATA> timerQueries;
	idStaticList<bool, MRB_TOTAL* NUM_FRAME_DATA> timerUsed;

	// pooled accumulating timers for interleaved shadow-generation work (see renderLogShadowGen_t)
	idStaticList<nvrhi::TimerQueryHandle, MAX_SHADOWGEN_SEGMENTS* NUM_FRAME_DATA> shadowGenQueries;
	idStaticList<uint8, MAX_SHADOWGEN_SEGMENTS* NUM_FRAME_DATA> shadowGenKind;
	int							shadowGenCount[NUM_FRAME_DATA];		// segments issued this cycle, per parity
	int							shadowGenActive;					// index of the open segment, or -1

	// pooled accumulating timers for the per-pass MAIN blocks (all MRB_* except MRB_GPU_TIME, which
	// stays a dedicated whole-frame span). The old one-query-per-block "first open wins" scheme
	// silently dropped every later occurrence of a block in the frame - with a GUI/2D view rendered
	// before the world view, the trivial first view claimed the slot and the ENTIRE 3D view's passes
	// went untimed (measured: depth "0.03 ms" at 1440p, interactions "0.24 ms" at 220 lights, while
	// 85-148 ms/frame sat unattributed). Segments are summed per block kind in FetchGPUTimers, so
	// every occurrence is counted - same proven mechanism as the shadow-gen pool above.
	static const int MAX_PASS_SEGMENTS = 64;
	idStaticList<nvrhi::TimerQueryHandle, MAX_PASS_SEGMENTS* NUM_FRAME_DATA> passSegQueries;
	idStaticList<uint8, MAX_PASS_SEGMENTS* NUM_FRAME_DATA> passSegKind;	// renderLogMainBlock_t of each segment
	int							passSegCount[NUM_FRAME_DATA];		// segments issued this cycle, per parity
	int							passSegActive;						// index of the open segment, or -1
	int							passSegDropped;						// segments lost to pool exhaustion (whole run)

public:
	idRenderLog();

	void		Init();
	void		Shutdown();

	void		StartFrame( nvrhi::ICommandList* _commandList );
	void		EndFrame();
	void		Close() {}
	int			Active()
	{
		return 0;
	}

	void		OpenBlock( const char* label, const idVec4& color = colorBlack );
	void		CloseBlock();
	void		OpenMainBlock( renderLogMainBlock_t block );
	void		CloseMainBlock( int block = -1 );

	// bracket a single shadow-generation segment; nesting is not supported (one open at a time)
	void		BeginShadowGen( renderLogShadowGen_t kind );
	void		EndShadowGen();

	void		Printf( VERIFY_FORMAT_STRING const char* fmt, ... ) {}

	void		FetchGPUTimers( backEndCounters_t& pc );
};

extern idRenderLog renderLog;

#endif // !__RENDERLOG_H__
