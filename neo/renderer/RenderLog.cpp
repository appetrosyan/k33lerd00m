/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2013-2022 Robert Beckebans

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
#include "precompiled.h"
#pragma hdrstop

#include <sys/DeviceManager.h>
extern DeviceManager* deviceManager;

idCVar r_logLevel( "r_logLevel", "0", CVAR_BOOL, "1 = Named render events in RenderDoc but is slower if enabled" );

static const int LOG_LEVEL_BLOCKS_ONLY	= 1;
static const int LOG_LEVEL_EVERYTHING	= 2;

const char* renderLogMainBlockLabels[] =
{
	ASSERT_ENUM_STRING( MRB_GPU_TIME,						0 ),
	ASSERT_ENUM_STRING( MRB_BEGIN_DRAWING_VIEW,				1 ),
	ASSERT_ENUM_STRING( MRB_FILL_DEPTH_BUFFER,				2 ),
	ASSERT_ENUM_STRING( MRB_FILL_HIZ_BUFFER,				3 ),
	ASSERT_ENUM_STRING( MRB_FILL_GEOMETRY_BUFFER,			4 ),
	ASSERT_ENUM_STRING( MRB_SSAO_PASS,						5 ),
	ASSERT_ENUM_STRING( MRB_AMBIENT_PASS,					6 ),
	ASSERT_ENUM_STRING( MRB_SHADOW_ATLAS_PASS,				7 ),
	ASSERT_ENUM_STRING( MRB_DRAW_INTERACTIONS,				8 ),
	ASSERT_ENUM_STRING( MRB_DRAW_SHADER_PASSES,				9 ),
	ASSERT_ENUM_STRING( MRB_FOG_ALL_LIGHTS,					10 ),
	ASSERT_ENUM_STRING( MRB_BLOOM,							11 ),
	ASSERT_ENUM_STRING( MRB_DRAW_SHADER_PASSES_POST,		12 ),
	ASSERT_ENUM_STRING( MRB_DRAW_DEBUG_TOOLS,				13 ),
	ASSERT_ENUM_STRING( MRB_CAPTURE_COLORBUFFER,			14 ),
	ASSERT_ENUM_STRING( MRB_MOTION_VECTORS,					15 ),
	ASSERT_ENUM_STRING( MRB_TAA,							16 ),
	ASSERT_ENUM_STRING( MRB_TONE_MAP_PASS,					17 ),
	ASSERT_ENUM_STRING( MRB_POSTPROCESS,					18 ),
	ASSERT_ENUM_STRING( MRB_DRAW_GUI,                       19 ),
	ASSERT_ENUM_STRING( MRB_CRT_POSTPROCESS,                20 ),
	ASSERT_ENUM_STRING( MRB_TOTAL,							21 )
};

extern uint64 Sys_Microseconds();

/*
========================
PC_BeginNamedEvent

FIXME: this is not thread safe on the PC
========================
*/
void PC_BeginNamedEvent( const char* szName, const idVec4& color, nvrhi::ICommandList* commandList )
{
	if( r_logLevel.GetInteger() <= 0 )
	{
		return;
	}

	if( commandList )
	{
		commandList->beginMarker( szName );
	}
}

/*
========================
PC_EndNamedEvent
========================
*/
void PC_EndNamedEvent( nvrhi::ICommandList* commandList )
{
	if( r_logLevel.GetInteger() <= 0 )
	{
		return;
	}

	if( commandList )
	{
		commandList->endMarker();
	}
}

/*
================================================================================================

idRenderLog

================================================================================================
*/

idRenderLog	renderLog;


idRenderLog::idRenderLog()
{
	frameCounter = 0;
	frameParity = 0;
}

void idRenderLog::Init()
{
	for( int i = 0; i < MRB_TOTAL * NUM_FRAME_DATA; i++ )
	{
		timerQueries.Append( deviceManager->GetDevice()->createTimerQuery() );
		timerUsed.Append( false );
	}

	for( int i = 0; i < MAX_SHADOWGEN_SEGMENTS * NUM_FRAME_DATA; i++ )
	{
		shadowGenQueries.Append( deviceManager->GetDevice()->createTimerQuery() );
		shadowGenKind.Append( 0 );
	}
	for( int i = 0; i < NUM_FRAME_DATA; i++ )
	{
		shadowGenCount[i] = 0;
	}
	shadowGenActive = -1;

	for( int i = 0; i < MAX_PASS_SEGMENTS * NUM_FRAME_DATA; i++ )
	{
		passSegQueries.Append( deviceManager->GetDevice()->createTimerQuery() );
		passSegKind.Append( 0 );
	}
	for( int i = 0; i < NUM_FRAME_DATA; i++ )
	{
		passSegCount[i] = 0;
	}
	passSegActive = -1;
	passSegDropped = 0;
}

void idRenderLog::Shutdown()
{
	commandList = nullptr;

	for( int i = 0; i < MRB_TOTAL * NUM_FRAME_DATA; i++ )
	{
		timerQueries[i].Reset();
	}

	for( int i = 0; i < MAX_SHADOWGEN_SEGMENTS * NUM_FRAME_DATA; i++ )
	{
		shadowGenQueries[i].Reset();
	}

	for( int i = 0; i < MAX_PASS_SEGMENTS * NUM_FRAME_DATA; i++ )
	{
		passSegQueries[i].Reset();
	}
}

void idRenderLog::StartFrame( nvrhi::ICommandList* _commandList )
{
	commandList = _commandList;
}

void idRenderLog::EndFrame()
{
	commandList = nullptr;
}



/*
========================
idRenderLog::OpenMainBlock
========================
*/
void idRenderLog::OpenMainBlock( renderLogMainBlock_t block )
{
	// SRS - Use glConfig.timerQueryAvailable flag to control timestamp capture for all platforms
	if( glConfig.timerQueryAvailable )
	{
		mainBlock = block;

		if( block != MRB_GPU_TIME )
		{
			// per-pass blocks go through the SUMMED segment pool: every occurrence in the frame is
			// timed and accumulated per kind (see the header - the old first-open-wins slot dropped
			// every later occurrence, untiming the whole 3D view behind a GUI view). One segment
			// open at a time; main blocks do not nest (GPU_TIME is the only enclosing block and it
			// keeps the dedicated slot below).
			if( passSegActive >= 0 )
			{
				return;			// unexpected nesting: keep the outer segment, leave this one untimed
			}
			int seg = passSegCount[frameParity];
			if( seg >= MAX_PASS_SEGMENTS )
			{
				passSegDropped++;	// pool exhausted: numbers under-report, surfaced in FetchGPUTimers
				return;
			}
			int idx = seg + frameParity * MAX_PASS_SEGMENTS;
			passSegKind[idx] = ( uint8 )block;
			commandList->beginTimerQuery( passSegQueries[idx] );
			passSegActive = idx;
			return;
		}

		int timerIndex = mainBlock + frameParity * MRB_TOTAL;

		// SRS - Only issue a new start timer query if timer slot unused
		if( !timerUsed[ timerIndex ] )
		{
			commandList->beginTimerQuery( timerQueries[ timerIndex ] );
		}
	}
}

/*
========================
idRenderLog::CloseMainBlock
========================
*/
void idRenderLog::CloseMainBlock( int _block )
{
	// SRS - Use glConfig.timerQueryAvailable flag to control timestamp capture for all platforms
	if( glConfig.timerQueryAvailable )
	{
		renderLogMainBlock_t block = mainBlock;

		if( _block != -1 )
		{
			block = renderLogMainBlock_t( _block );
		}

		if( block != MRB_GPU_TIME )
		{
			// close the open pooled per-pass segment (see OpenMainBlock)
			if( passSegActive >= 0 )
			{
				commandList->endTimerQuery( passSegQueries[passSegActive] );
				passSegCount[frameParity]++;
				passSegActive = -1;
			}
			return;
		}

		int timerIndex = block + frameParity * MRB_TOTAL;

		// SRS - Only issue a new end timer query if timer slot unused
		if( !timerUsed[ timerIndex ] )
		{
			commandList->endTimerQuery( timerQueries[ timerIndex ] );
			timerUsed[ timerIndex ] = true;
		}
	}
}

/*
========================
idRenderLog::BeginShadowGen

Open one shadow-generation timing segment at the current frame parity. Segments are
summed per kind in FetchGPUTimers. Not nestable: one segment open at a time.
========================
*/
void idRenderLog::BeginShadowGen( renderLogShadowGen_t kind )
{
	if( !glConfig.timerQueryAvailable )
	{
		return;
	}

	assert( shadowGenActive < 0 );

	int seg = shadowGenCount[frameParity];
	if( seg >= MAX_SHADOWGEN_SEGMENTS )
	{
		// pool exhausted this frame - leave the rest untimed rather than grow the pool
		return;
	}

	int idx = seg + frameParity * MAX_SHADOWGEN_SEGMENTS;
	shadowGenKind[idx] = ( uint8 )kind;
	commandList->beginTimerQuery( shadowGenQueries[idx] );
	shadowGenActive = idx;
}

/*
========================
idRenderLog::EndShadowGen
========================
*/
void idRenderLog::EndShadowGen()
{
	if( !glConfig.timerQueryAvailable || shadowGenActive < 0 )
	{
		return;
	}

	commandList->endTimerQuery( shadowGenQueries[shadowGenActive] );
	shadowGenCount[frameParity]++;
	shadowGenActive = -1;
}

/*
========================
idRenderLog::FetchGPUTimers
========================
*/
void idRenderLog::FetchGPUTimers( backEndCounters_t& pc )
{
	frameCounter++;
	frameParity = ( frameParity + 1 ) % NUM_FRAME_DATA;

	// GARBAGE-READ GUARD: a query slot at this parity is only guaranteed WRITTEN once a full
	// parity cycle has passed. Reading a never-written query returns garbage timestamps
	// (measured: a fabricated "20.9 s GPU frame" at 0.7 ms wall on the first bench frame,
	// polluting every mean). Until primed, report zeros - consumers treat gpuMicroSec==0 as
	// "timer invalid this frame", which is the truth.
	const bool primed = ( frameCounter > ( uint64 )NUM_FRAME_DATA );

	// whole-frame span: dedicated one-shot slot (the only block that encloses others)
	{
		const int timerIndex = MRB_GPU_TIME + frameParity * MRB_TOTAL;
		pc.gpuMicroSec = 0;
		if( timerUsed[timerIndex] && primed )
		{
			pc.gpuMicroSec = uint64( deviceManager->GetDevice()->getTimerQueryTime( timerQueries[ timerIndex ] ) * 1000000.0 );
		}
	}
	for( int i = 0; i < MRB_TOTAL; i++ )
	{
		timerUsed[i + frameParity * MRB_TOTAL] = false;
	}

	// per-pass blocks: sum this parity's pooled segments per kind - every occurrence in the frame
	// counts (a GUI view AND the world view, a pass that runs twice), so sum-of-passes is honestly
	// comparable against the whole-frame span and "unattributed" means exactly that.
	double passSum[MRB_TOTAL] = {};
	if( glConfig.timerQueryAvailable && primed )
	{
		for( int seg = 0; seg < passSegCount[frameParity]; seg++ )
		{
			const int idx = seg + frameParity * MAX_PASS_SEGMENTS;
			passSum[passSegKind[idx]] += deviceManager->GetDevice()->getTimerQueryTime( passSegQueries[idx] ) * 1000000.0;
		}
	}
	passSegCount[frameParity] = 0;
	passSegActive = -1;
	if( passSegDropped > 0 )
	{
		common->Warning( "idRenderLog: %d per-pass timer segments dropped (pool exhausted) - pass times under-report", passSegDropped );
		passSegDropped = 0;
	}

	pc.gpuBeginDrawingMicroSec                = uint64( passSum[MRB_BEGIN_DRAWING_VIEW] );
	pc.gpuDepthMicroSec                       = uint64( passSum[MRB_FILL_DEPTH_BUFFER] );
	pc.gpuHiZMicroSec                         = uint64( passSum[MRB_FILL_HIZ_BUFFER] );
	pc.gpuGeometryMicroSec                    = uint64( passSum[MRB_FILL_GEOMETRY_BUFFER] );
	pc.gpuScreenSpaceAmbientOcclusionMicroSec = uint64( passSum[MRB_SSAO_PASS] );
	pc.gpuAmbientPassMicroSec                 = uint64( passSum[MRB_AMBIENT_PASS] );
	pc.gpuShadowAtlasPassMicroSec             = uint64( passSum[MRB_SHADOW_ATLAS_PASS] );
	pc.gpuInteractionsMicroSec                = uint64( passSum[MRB_DRAW_INTERACTIONS] );
	pc.gpuShaderPassMicroSec                  = uint64( passSum[MRB_DRAW_SHADER_PASSES] );
	pc.gpuFogAllLightsMicroSec                = uint64( passSum[MRB_FOG_ALL_LIGHTS] );
	pc.gpuBloomMicroSec                       = uint64( passSum[MRB_BLOOM] );
	pc.gpuShaderPassPostMicroSec              = uint64( passSum[MRB_DRAW_SHADER_PASSES_POST] );
	pc.gpuMotionVectorsMicroSec               = uint64( passSum[MRB_MOTION_VECTORS] );
	pc.gpuTemporalAntiAliasingMicroSec        = uint64( passSum[MRB_TAA] );
	pc.gpuToneMapPassMicroSec                 = uint64( passSum[MRB_TONE_MAP_PASS] );
	pc.gpuPostProcessingMicroSec              = uint64( passSum[MRB_POSTPROCESS] );
	pc.gpuDrawGuiMicroSec                     = uint64( passSum[MRB_DRAW_GUI] );
	pc.gpuCrtPostProcessingMicroSec           = uint64( passSum[MRB_CRT_POSTPROCESS] );
	// sum the pooled shadow-generation segments for this parity, grouped by kind (same primed
	// guard: never-written queries read garbage)
	uint64 shadowGen[RLS_TOTAL] = {};
	if( glConfig.timerQueryAvailable && primed )
	{
		for( int seg = 0; seg < shadowGenCount[frameParity]; seg++ )
		{
			int idx = seg + frameParity * MAX_SHADOWGEN_SEGMENTS;
			double time = deviceManager->GetDevice()->getTimerQueryTime( shadowGenQueries[idx] ) * 1000000.0;
			shadowGen[shadowGenKind[idx]] += uint64( time );
		}
	}
	pc.gpuStencilShadowMicroSec	= shadowGen[RLS_STENCIL];
	pc.gpuShadowMapMicroSec		= shadowGen[RLS_SHADOWMAP];
	pc.gpuRTShadowMaskMicroSec	= shadowGen[RLS_RTMASK];
	pc.gpuSoftShadowMicroSec	= shadowGen[RLS_SOFT];

	// free the pool slots for this parity to be rewritten this frame
	shadowGenCount[frameParity] = 0;
	shadowGenActive = -1;
}


/*
========================
idRenderLog::OpenBlock
========================
*/
void idRenderLog::OpenBlock( const char* label, const idVec4& color )
{
	PC_BeginNamedEvent( label, color, commandList );
}

/*
========================
idRenderLog::CloseBlock
========================
*/
void idRenderLog::CloseBlock()
{
	PC_EndNamedEvent( commandList );
}
