/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2014 Robert Beckebans

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

#include "RenderCommon.h"
#include "RenderCapture.h"

/*
==========================================================================================

FRAME MEMORY ALLOCATION

==========================================================================================
*/

static const unsigned int FRAME_ALLOC_ALIGNMENT = 128;
static const unsigned int MAX_FRAME_MEMORY = 64 * 1024 * 1024;	// larger so that we can noclip on PC for dev purposes
// NOTE: exhausting this on a JOB thread (idLib::Error mid-job) kills the job and leaves the frontend
// spinning forever in idParallelJobList::Wait - keep per-frame stream growth well inside it.

idFrameData		smpFrameData[NUM_FRAME_DATA];
idFrameData* 	frameData;
unsigned int	smpFrame;

//#define TRACK_FRAME_ALLOCS

#if defined( TRACK_FRAME_ALLOCS )
	idSysInterlockedInteger frameAllocTypeCount[FRAME_ALLOC_MAX];
	int frameHighWaterTypeCount[FRAME_ALLOC_MAX];
#endif

/*
====================
R_ToggleSmpFrame
====================
*/
void R_ToggleSmpFrame()
{
	// update the highwater mark
	if( frameData->frameMemoryAllocated.GetValue() > frameData->highWaterAllocated )
	{
		frameData->highWaterAllocated = frameData->frameMemoryAllocated.GetValue();
#if defined( TRACK_FRAME_ALLOCS )
		frameData->highWaterUsed = frameData->frameMemoryUsed.GetValue();
		for( int i = 0; i < FRAME_ALLOC_MAX; i++ )
		{
			frameHighWaterTypeCount[i] = frameAllocTypeCount[i].GetValue();
		}
#endif
	}

	// switch to the next frame
	smpFrame++;
	frameData = &smpFrameData[smpFrame % NUM_FRAME_DATA];

	// reset the memory allocation

	// RB: 64 bit fixes, changed unsigned int to uintptr_t
	const uintptr_t bytesNeededForAlignment = FRAME_ALLOC_ALIGNMENT - ( ( uintptr_t )frameData->frameMemory & ( FRAME_ALLOC_ALIGNMENT - 1 ) );
	// RB end

	frameData->frameMemoryAllocated.SetValue( bytesNeededForAlignment );
	frameData->frameMemoryUsed.SetValue( 0 );

#if defined( TRACK_FRAME_ALLOCS )
	for( int i = 0; i < FRAME_ALLOC_MAX; i++ )
	{
		frameAllocTypeCount[i].SetValue( 0 );
	}
#endif

	// clear the command chain and make a RC_NOP command the only thing on the list
	frameData->cmdHead = frameData->cmdTail = ( emptyCommand_t* )R_FrameAlloc( sizeof( *frameData->cmdHead ), FRAME_ALLOC_DRAW_COMMAND );
	frameData->cmdHead->commandId = RC_NOP;
	frameData->cmdHead->next = NULL;
}

/*
=====================
R_ShutdownFrameData
=====================
*/
void R_ShutdownFrameData()
{
	frameData = NULL;
	for( int i = 0; i < NUM_FRAME_DATA; i++ )
	{
		Mem_Free16( smpFrameData[i].frameMemory );
		smpFrameData[i].frameMemory = NULL;
	}
}

/*
=====================
R_InitFrameData
=====================
*/
void R_InitFrameData()
{
	R_ShutdownFrameData();

	for( int i = 0; i < NUM_FRAME_DATA; i++ )
	{
		smpFrameData[i].frameMemory = ( byte* ) Mem_Alloc16( MAX_FRAME_MEMORY, TAG_RENDER );
	}

	// must be set before calling R_ToggleSmpFrame()
	frameData = &smpFrameData[ 0 ];

	R_ToggleSmpFrame();
}

/*
================
R_FrameAlloc

This data will be automatically freed when the
current frame's back end completes.

This should only be called by the front end.  The
back end shouldn't need to allocate memory.

All temporary data, like dynamic tesselations
and local spaces are allocated here.

All memory is cache-line-cleared for the best performance.
================
*/
void* R_FrameAlloc( int bytes, frameAllocType_t type )
{
#if defined( TRACK_FRAME_ALLOCS )
	frameData->frameMemoryUsed.Add( bytes );
	frameAllocTypeCount[type].Add( bytes );
#endif

	bytes = ( bytes + FRAME_ALLOC_ALIGNMENT - 1 ) & ~( FRAME_ALLOC_ALIGNMENT - 1 );

	// thread safe add
	int	end = frameData->frameMemoryAllocated.Add( bytes );
	if( end > MAX_FRAME_MEMORY )
	{
		idLib::Error( "R_FrameAlloc ran out of memory. bytes = %d, end = %d, highWaterAllocated = %d\n", bytes, end, frameData->highWaterAllocated );
	}

	byte* ptr = frameData->frameMemory + end - bytes;

	// cache line clear the memory
	for( int offset = 0; offset < bytes; offset += CACHE_LINE_SIZE )
	{
		ZeroCacheLine( ptr, offset );
	}

	return ptr;
}

/*
==================
R_ClearedFrameAlloc
==================
*/
void* R_ClearedFrameAlloc( int bytes, frameAllocType_t type )
{
	// NOTE: every allocation is cache line cleared
	return R_FrameAlloc( bytes, type );
}

/*
==========================================================================================

FONT-END STATIC MEMORY ALLOCATION

==========================================================================================
*/

/*
=================
R_StaticAlloc
=================
*/
void* R_StaticAlloc( int bytes, const memTag_t tag )
{
	tr.pc.c_alloc++;

	void* buf = Mem_Alloc( bytes, tag );

	// don't exit on failure on zero length allocations since the old code didn't
	if( buf == NULL && bytes != 0 )
	{
		common->FatalError( "R_StaticAlloc failed on %i bytes", bytes );
	}
	return buf;
}

/*
=================
R_ClearedStaticAlloc
=================
*/
void* R_ClearedStaticAlloc( int bytes )
{
	void* buf = R_StaticAlloc( bytes );
	memset( buf, 0, bytes );
	return buf;
}

/*
=================
R_StaticFree
=================
*/
void R_StaticFree( void* data )
{
	tr.pc.c_free++;
	Mem_Free( data );
}

/*
==========================================================================================

FONT-END RENDERING

==========================================================================================
*/

/*
=================
R_SortDrawSurfs
=================
*/
static void R_SortDrawSurfs( drawSurf_t** drawSurfs, const int numDrawSurfs )
{
#if 1

	uint64* indices = ( uint64* ) _alloca16( numDrawSurfs * sizeof( indices[0] ) );

	// sort the draw surfs based on:
	// 1. sort value (largest first)
	// 2. depth (smallest first)
	// 3. index (largest first)
	assert( numDrawSurfs <= 0xFFFF );
	for( int i = 0; i < numDrawSurfs; i++ )
	{
		float sort = SS_POST_PROCESS - drawSurfs[i]->sort;
		assert( sort >= 0.0f );

		uint64 dist = 0;
		if( drawSurfs[i]->frontEndGeo != NULL )
		{
			float min = 0.0f;
			float max = 1.0f;
			idRenderMatrix::DepthBoundsForBounds( min, max, drawSurfs[i]->space->mvp, drawSurfs[i]->frontEndGeo->bounds );
			dist = idMath::Ftoui16( min * 0xFFFF );
		}

		indices[i] = ( ( numDrawSurfs - i ) & 0xFFFF ) | ( dist << 16 ) | ( ( uint64 )( *( uint32* )&sort ) << 32 );
	}

	const int64 MAX_LEVELS = 128;
	int64 lo[MAX_LEVELS];
	int64 hi[MAX_LEVELS];

	// Keep the top of the stack in registers to avoid load-hit-stores.
	int64 st_lo = 0;
	int64 st_hi = numDrawSurfs - 1;
	int64 level = 0;

	for( ; ; )
	{
		int64 i = st_lo;
		int64 j = st_hi;
		if( j - i >= 4 && level < MAX_LEVELS - 1 )
		{
			uint64 pivot = indices[( i + j ) / 2];
			do
			{
				while( indices[i] > pivot )
				{
					i++;
				}
				while( indices[j] < pivot )
				{
					j--;
				}
				if( i > j )
				{
					break;
				}
				uint64 h = indices[i];
				indices[i] = indices[j];
				indices[j] = h;
			}
			while( ++i <= --j );

			// No need for these iterations because we are always sorting unique values.
			//while ( indices[j] == pivot && st_lo < j ) j--;
			//while ( indices[i] == pivot && i < st_hi ) i++;

			assert( level < MAX_LEVELS - 1 );
			lo[level] = i;
			hi[level] = st_hi;
			st_hi = j;
			level++;
		}
		else
		{
			for( ; i < j; j-- )
			{
				int64 m = i;
				for( int64 k = i + 1; k <= j; k++ )
				{
					if( indices[k] < indices[m] )
					{
						m = k;
					}
				}
				uint64 h = indices[m];
				indices[m] = indices[j];
				indices[j] = h;
			}
			if( --level < 0 )
			{
				break;
			}
			st_lo = lo[level];
			st_hi = hi[level];
		}
	}

	drawSurf_t** newDrawSurfs = ( drawSurf_t** ) indices;
	for( int i = 0; i < numDrawSurfs; i++ )
	{
		newDrawSurfs[i] = drawSurfs[numDrawSurfs - ( indices[i] & 0xFFFF )];
	}
	memcpy( drawSurfs, newDrawSurfs, numDrawSurfs * sizeof( drawSurfs[0] ) );

#else

	struct local_t
	{
		static int R_QsortSurfaces( const void* a, const void* b )
		{
			const drawSurf_t* ea = *( drawSurf_t** )a;
			const drawSurf_t* eb = *( drawSurf_t** )b;
			if( ea->sort < eb->sort )
			{
				return -1;
			}
			if( ea->sort > eb->sort )
			{
				return 1;
			}
			return 0;
		}
	};

	// Add a sort offset so surfaces with equal sort orders still deterministically
	// draw in the order they were added, at least within a given model.
	float sorfOffset = 0.0f;
	for( int i = 0; i < numDrawSurfs; i++ )
	{
		drawSurf[i]->sort += sorfOffset;
		sorfOffset += 0.000001f;
	}

	// sort the drawsurfs
	qsort( drawSurfs, numDrawSurfs, sizeof( drawSurfs[0] ), local_t::R_QsortSurfaces );

#endif
}

// RB begin
static void R_SetupSplitFrustums( viewDef_t* viewDef )
{
	idVec3			planeOrigin;

	const float zNearStart = ( viewDef->renderView.cramZNear ) ? ( r_znear.GetFloat() * 0.25f ) : r_znear.GetFloat();
	float zFarEnd = 10000;

	float zNear = zNearStart;
	float zFar = zFarEnd;

	float lambda = r_shadowMapSplitWeight.GetFloat();
	float ratio = zFarEnd / zNearStart;

	for( int i = 0; i < 6; i++ )
	{
		tr.viewDef->frustumSplitDistances[i] = idMath::INFINITUM;
	}

	for( int i = 1; i <= ( r_shadowMapSplits.GetInteger() + 1 ) && i < MAX_FRUSTUMS; i++ )
	{
		float si = i / ( float )( r_shadowMapSplits.GetInteger() + 1 );

		if( i > FRUSTUM_CASCADE1 )
		{
			zNear = zFar - ( zFar * 0.005f );
		}

		zFar = 1.005f * lambda * ( zNearStart * powf( ratio, si ) ) + ( 1 - lambda ) * ( zNearStart + ( zFarEnd - zNearStart ) * si );

		if( i <= r_shadowMapSplits.GetInteger() )
		{
			tr.viewDef->frustumSplitDistances[i - 1] = zFar;
		}

		float projectionMatrix[16];
		R_SetupProjectionMatrix2( tr.viewDef, zNear, zFar, projectionMatrix );

		// setup render matrices for faster culling
		idRenderMatrix projectionRenderMatrix;
		idRenderMatrix::Transpose( *( idRenderMatrix* )projectionMatrix, projectionRenderMatrix );
		idRenderMatrix viewRenderMatrix;
		idRenderMatrix::Transpose( *( idRenderMatrix* )tr.viewDef->worldSpace.modelViewMatrix, viewRenderMatrix );
		idRenderMatrix::Multiply( projectionRenderMatrix, viewRenderMatrix, tr.viewDef->frustumMVPs[i] );

		// the planes of the view frustum are needed for portal visibility culling
		idRenderMatrix::GetFrustumPlanes( tr.viewDef->frustums[i], tr.viewDef->frustumMVPs[i], false, true );

		// the DOOM 3 frustum planes point outside the frustum
		for( int j = 0; j < 6; j++ )
		{
			tr.viewDef->frustums[i][j] = - tr.viewDef->frustums[i][j];
		}

		// remove the Z-near to avoid portals from being near clipped
		if( i == FRUSTUM_CASCADE1 )
		{
			tr.viewDef->frustums[i][4][3] -= r_znear.GetFloat();
		}
	}
}

class idSort_CompareEnvprobe : public idSort_Quick< RenderEnvprobeLocal*, idSort_CompareEnvprobe >
{
	idVec3	viewOrigin;

public:
	idSort_CompareEnvprobe( const idVec3& origin )
	{
		viewOrigin = origin;
	}

	int Compare( RenderEnvprobeLocal* const& a, RenderEnvprobeLocal* const& b ) const
	{
		float adist = ( viewOrigin - a->parms.origin ).LengthSqr();
		float bdist = ( viewOrigin - b->parms.origin ).LengthSqr();

		if( adist < bdist )
		{
			return -1;
		}

		if( adist > bdist )
		{
			return 1;
		}

		return 0;
	}
};

// Env-probe selection hysteresis. The 3 nearest probes are picked by CAMERA distance,
// so crossing a probe-cell boundary reshuffles the set and the whole specular/diffuse
// reflection swaps in one step (the "teleport"). This discount makes a probe already
// selected last frame "stick": a challenger must be closer than incumbent*(1-frac) to
// displace it, so a small camera move keeps the same triangle and the barycentric blend
// varies continuously instead of snapping. 0 = off (original behaviour).
idCVar r_envProbeHysteresis( "r_envProbeHysteresis", "0.5", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE | CVAR_NEW, "env-probe selection stickiness [0..0.9]: challenger must be this fraction closer to replace a selected probe (reduces the one-step reflection teleport)", 0.0f, 0.9f );

// NUKE the teleport: 0 = skip per-view env-probe selection entirely and keep the stable
// global cubes, so the specular/diffuse reflection can NEVER swap as the camera crosses
// a probe cell (zero teleport, at the cost of local reflection detail). 1 = select local
// probes per view (original behaviour; use r_envProbeHysteresis to stabilise them).
idCVar r_envProbeReflections( "r_envProbeReflections", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_ARCHIVE | CVAR_NEW, "0 = disable per-view env-probe selection (stable global cube, no reflection teleport); 1 = local probes per view" );

static void R_FindClosestEnvironmentProbes()
{
	// set safe defaults
	tr.viewDef->globalProbeBounds.Clear();

	tr.viewDef->irradianceImage = globalImages->defaultUACIrradianceCube;
	tr.viewDef->radianceImageBlends.Set( 1, 0, 0, 0 );
	for( int i = 0; i < 3; i++ )
	{
		tr.viewDef->radianceImages[i] = globalImages->defaultUACRadianceCube;
	}
	// .w = 0 tells the shader there is no valid probe triangle -> use the camera blend
	for( int i = 0; i < 3; i++ )
	{
		tr.viewDef->probePositions[i].Set( 0.0f, 0.0f, 0.0f, 0.0f );
	}

	// NUKE the reflection teleport (default): keep the stable global cubes set above and
	// skip per-view probe selection entirely, so the reflection never swaps between
	// probes as the camera moves. r_envProbeReflections 1 restores local probes.
	if( !r_envProbeReflections.GetBool() )
	{
		return;
	}

	// early out
	if( tr.viewDef->areaNum == -1 || tr.viewDef->isSubview )
	{
		return;
	}

	idList<RenderEnvprobeLocal*, TAG_RENDER_ENVPROBE> viewEnvprobes;
	for( int i = 0; i < tr.primaryWorld->envprobeDefs.Num(); i++ )
	{
		RenderEnvprobeLocal* vProbe = tr.primaryWorld->envprobeDefs[i];
		if( vProbe )
		{
			// check for being closed off behind a door
			if( r_useLightAreaCulling.GetBool() && vProbe->areaNum != -1 && !tr.viewDef->connectedAreas[ vProbe->areaNum ] )
			{
				continue;
			}

			viewEnvprobes.AddUnique( vProbe );
		}
	}

	if( viewEnvprobes.Num() == 0 )
	{
		return;
	}

	idVec3 testOrigin = tr.viewDef->renderView.vieworg;

	// sort by distance
	// RB: each Doom 3 level has ~50 - 150 probes so this should be ok for each frame
	viewEnvprobes.SortWithTemplate( idSort_CompareEnvprobe( testOrigin ) );

	// Hysteresis (see r_envProbeHysteresis): keep the probes selected last frame in the
	// top 3 so a small camera move does not reshuffle the set and teleport the whole
	// reflection. Compared by origin (not pointer) so it survives map reloads. Only the
	// primary view reaches here (subviews early-out above), so a static is safe.
	static idVec3 s_lastProbeOrigins[3];
	static bool s_lastProbesValid = false;
	const float hyst = r_envProbeHysteresis.GetFloat();
	if( hyst > 0.0f && s_lastProbesValid && viewEnvprobes.Num() > 3 )
	{
		const float d3 = ( viewEnvprobes[2]->parms.origin - testOrigin ).Length();
		const float keepDist = d3 / Max( 1e-3f, 1.0f - hyst );

		// promote each incumbent still near enough back into the top 3
		for( int inc = 0; inc < 3; inc++ )
		{
			int found = -1;
			for( int j = 3; j < viewEnvprobes.Num(); j++ )
			{
				if( ( viewEnvprobes[j]->parms.origin - s_lastProbeOrigins[inc] ).LengthSqr() < 1.0f )
				{
					found = j;
					break;
				}
			}
			if( found < 0 )
			{
				continue;	// incumbent already in the top 3, or gone from view
			}
			if( ( viewEnvprobes[found]->parms.origin - testOrigin ).Length() <= keepDist )
			{
				RenderEnvprobeLocal* tmp = viewEnvprobes[2];
				viewEnvprobes[2] = viewEnvprobes[found];
				viewEnvprobes[found] = tmp;
			}
		}

		// keep the single-nearest (diffuse irradiance) sticky too, if still in the top 3
		for( int j = 1; j < 3; j++ )
		{
			if( ( viewEnvprobes[j]->parms.origin - s_lastProbeOrigins[0] ).LengthSqr() < 1.0f &&
					( viewEnvprobes[j]->parms.origin - testOrigin ).Length() <= keepDist )
			{
				RenderEnvprobeLocal* tmp = viewEnvprobes[0];
				viewEnvprobes[0] = viewEnvprobes[j];
				viewEnvprobes[j] = tmp;
				break;
			}
		}
	}
	for( int i = 0; i < 3; i++ )
	{
		s_lastProbeOrigins[i] = viewEnvprobes[ ( i < viewEnvprobes.Num() ) ? i : ( viewEnvprobes.Num() - 1 ) ]->parms.origin;
	}
	s_lastProbesValid = true;

	RenderEnvprobeLocal* nearest = viewEnvprobes[0];
	tr.viewDef->globalProbeBounds = nearest->globalProbeBounds;

	if( nearest->irradianceImage->IsLoaded() && !nearest->irradianceImage->IsDefaulted() )
	{
		tr.viewDef->irradianceImage = nearest->irradianceImage;
	}

	// form a triangle of the 3 closest probes
	idVec3 verts[3];
	for( int i = 0; i < 3; i++ )
	{
		verts[i] = viewEnvprobes[0]->parms.origin;
	}

	for( int i = 0; i < viewEnvprobes.Num() && i < 3; i++ )
	{
		RenderEnvprobeLocal* vProbe = viewEnvprobes[i];

		verts[i] = vProbe->parms.origin;
	}

	tr.viewDef->probePositions->Set( verts[0].x, verts[0].y, verts[0].z, 1 );
	tr.viewDef->probePositions->Set( verts[1].x, verts[1].y, verts[1].z, 1 );
	tr.viewDef->probePositions->Set( verts[2].x, verts[2].y, verts[2].z, 1 );

	idVec3 closest = R_ClosestPointPointTriangle( testOrigin, verts[0], verts[1], verts[2] );
	idVec3 bary;

	// find the barycentric coordinates
	float denom = idWinding::TriangleArea( verts[0], verts[1], verts[2] );
	if( denom == 0 )
	{
		// triangle is line
		float t;

		R_ClosestPointOnLineSegment( testOrigin, verts[0], verts[1], t );

		bary.Set( 1.0f - t, t, 0 );
	}
	else
	{
		float	a, b, c;

		a = idWinding::TriangleArea( closest, verts[1], verts[2] ) / denom;
		b = idWinding::TriangleArea( closest, verts[2], verts[0] ) / denom;
		c = idWinding::TriangleArea( closest, verts[0], verts[1] ) / denom;

		bary.Set( a, b, c );
	}

	tr.viewDef->radianceImageBlends.Set( bary.x, bary.y, bary.z, 0.0f );

	for( int i = 0; i < viewEnvprobes.Num() && i < 3; i++ )
	{
		if( !viewEnvprobes[i]->radianceImage->IsDefaulted() )
		{
			tr.viewDef->radianceImages[i] = viewEnvprobes[i]->radianceImage;
		}
	}

	if( tr.viewDef->radianceImages[0] == globalImages->defaultUACRadianceCube &&
			tr.viewDef->radianceImages[1] == globalImages->defaultUACRadianceCube &&
			tr.viewDef->radianceImages[2] == globalImages->defaultUACRadianceCube )
	{
		// this didn't work so this is the way to tell the backend and avoid blood reflections
		tr.viewDef->globalProbeBounds.Clear();
		tr.viewDef->probePositions[0].w = 0.0f;	// invalidate the per-pixel probe blend too
	}
}

// this one tries to interpolate between probes over time
static void R_FindClosestEnvironmentProbes2()
{
	// set safe defaults
	tr.viewDef->globalProbeBounds.Clear();

	tr.viewDef->irradianceImage = globalImages->defaultUACIrradianceCube;
	tr.viewDef->radianceImageBlends.Set( 1, 0, 0, 0 );
	for( int i = 0; i < 3; i++ )
	{
		tr.viewDef->radianceImages[i] = globalImages->defaultUACRadianceCube;
	}
	// .w = 0 tells the shader there is no valid probe triangle -> use the camera blend
	for( int i = 0; i < 3; i++ )
	{
		tr.viewDef->probePositions[i].Set( 0.0f, 0.0f, 0.0f, 0.0f );
	}

	// NUKE the reflection teleport (default): keep the stable global cubes set above and
	// skip per-view probe selection entirely, so the reflection never swaps between
	// probes as the camera moves. r_envProbeReflections 1 restores local probes.
	if( !r_envProbeReflections.GetBool() )
	{
		return;
	}

	// early out
	if( tr.viewDef->areaNum == -1 || tr.viewDef->isSubview )
	{
		return;
	}

	idList<RenderEnvprobeLocal*, TAG_RENDER_ENVPROBE> viewEnvprobes;
	for( int i = 0; i < tr.primaryWorld->envprobeDefs.Num(); i++ )
	{
		RenderEnvprobeLocal* vProbe = tr.primaryWorld->envprobeDefs[i];
		if( vProbe )
		{
			// check for being closed off behind a door
			if( r_useLightAreaCulling.GetBool() && vProbe->areaNum != -1 && !tr.viewDef->connectedAreas[ vProbe->areaNum ] )
			{
				continue;
			}

			viewEnvprobes.AddUnique( vProbe );
		}
	}

	if( viewEnvprobes.Num() == 0 )
	{
		return;
	}

	idVec3 testOrigin = tr.viewDef->renderView.vieworg;

	// sort by distance
	// RB: each Doom 3 level has ~50 - 150 probes so this should be ok for each frame
	viewEnvprobes.SortWithTemplate( idSort_CompareEnvprobe( testOrigin ) );

	// Hysteresis (see r_envProbeHysteresis): keep the probes selected last frame in the
	// top 3 so a small camera move does not reshuffle the set and teleport the whole
	// reflection. Compared by origin (not pointer) so it survives map reloads. Only the
	// primary view reaches here (subviews early-out above), so a static is safe.
	static idVec3 s_lastProbeOrigins[3];
	static bool s_lastProbesValid = false;
	const float hyst = r_envProbeHysteresis.GetFloat();
	if( hyst > 0.0f && s_lastProbesValid && viewEnvprobes.Num() > 3 )
	{
		const float d3 = ( viewEnvprobes[2]->parms.origin - testOrigin ).Length();
		const float keepDist = d3 / Max( 1e-3f, 1.0f - hyst );

		// promote each incumbent still near enough back into the top 3
		for( int inc = 0; inc < 3; inc++ )
		{
			int found = -1;
			for( int j = 3; j < viewEnvprobes.Num(); j++ )
			{
				if( ( viewEnvprobes[j]->parms.origin - s_lastProbeOrigins[inc] ).LengthSqr() < 1.0f )
				{
					found = j;
					break;
				}
			}
			if( found < 0 )
			{
				continue;	// incumbent already in the top 3, or gone from view
			}
			if( ( viewEnvprobes[found]->parms.origin - testOrigin ).Length() <= keepDist )
			{
				RenderEnvprobeLocal* tmp = viewEnvprobes[2];
				viewEnvprobes[2] = viewEnvprobes[found];
				viewEnvprobes[found] = tmp;
			}
		}

		// keep the single-nearest (diffuse irradiance) sticky too, if still in the top 3
		for( int j = 1; j < 3; j++ )
		{
			if( ( viewEnvprobes[j]->parms.origin - s_lastProbeOrigins[0] ).LengthSqr() < 1.0f &&
					( viewEnvprobes[j]->parms.origin - testOrigin ).Length() <= keepDist )
			{
				RenderEnvprobeLocal* tmp = viewEnvprobes[0];
				viewEnvprobes[0] = viewEnvprobes[j];
				viewEnvprobes[j] = tmp;
				break;
			}
		}
	}
	for( int i = 0; i < 3; i++ )
	{
		s_lastProbeOrigins[i] = viewEnvprobes[ ( i < viewEnvprobes.Num() ) ? i : ( viewEnvprobes.Num() - 1 ) ]->parms.origin;
	}
	s_lastProbesValid = true;

	RenderEnvprobeLocal* nearest = viewEnvprobes[0];
	tr.viewDef->globalProbeBounds = nearest->globalProbeBounds;

	if( nearest->irradianceImage->IsLoaded() && !nearest->irradianceImage->IsDefaulted() )
	{
		tr.viewDef->irradianceImage = nearest->irradianceImage;
	}

	static float oldBarycentricWeights[3] = {0};
	static int oldIndexes[3] = {0};
	static int timeInterpolateStart = 0;

	// form a triangle of the 3 closest probes
	int triIndexes[3];
	idVec3 verts[3];
	for( int i = 0; i < 3; i++ )
	{
		verts[i] = viewEnvprobes[0]->parms.origin;
		triIndexes[i] =  viewEnvprobes[0]->index;
	}

	bool triChanged = false;
	for( int i = 0; i < viewEnvprobes.Num() && i < 3; i++ )
	{
		RenderEnvprobeLocal* vProbe = viewEnvprobes[i];

		verts[i] = vProbe->parms.origin;
		triIndexes[i] = vProbe->index;
	}

	tr.viewDef->probePositions->Set( verts[0].x, verts[0].y, verts[0].z, 1 );
	tr.viewDef->probePositions->Set( verts[1].x, verts[1].y, verts[1].z, 1 );
	tr.viewDef->probePositions->Set( verts[2].x, verts[2].y, verts[2].z, 1 );

	// don't assume tri changed if we just moved inside a triangle and only the indixes switched
	// because one vertex is closer than before

	static int numInterpolantsDuringChange = 0;
	int numInterpolants = 0;
	int interpolants[3];
	int mapIndexes[3] = {0, 1, 2};

	for( int i = 0; i < 3; i++ )
	{
		for( int j = 0; j < 3; j++ )
		{
			if( oldIndexes[i] == triIndexes[j] && numInterpolants < 3 )
			{
				interpolants[numInterpolants] = i;
				mapIndexes[numInterpolants] = j;
				numInterpolants++;
			}
		}
	}

	if( numInterpolants != 3 )
	{
		triChanged = true;
		timeInterpolateStart = Sys_Milliseconds();
		numInterpolantsDuringChange = numInterpolants;

		//idLib::Printf( "env_probe triangle changed!\n" );
	}

	const int c_interpolationTimeframe = 2000.0f;

	idVec3 closest = R_ClosestPointPointTriangle( testOrigin, verts[0], verts[1], verts[2] );
	idVec3 barycentricWeights;

	int time = Sys_Milliseconds();

	// find the barycentric coordinates
	float denom = idWinding::TriangleArea( verts[0], verts[1], verts[2] );
	if( denom == 0 )
	{
		// triangle is a line
		// this can be the case in long corridors
		float t;

		R_ClosestPointOnLineSegment( testOrigin, verts[0], verts[1], t );

		barycentricWeights.Set( 1.0f - t, t, 0 );

		oldBarycentricWeights[0] = barycentricWeights[0];
		oldBarycentricWeights[1] = barycentricWeights[1];
		oldBarycentricWeights[2] = barycentricWeights[2];
	}
	else
	{
		float	a, b, c;

		a = idWinding::TriangleArea( closest, verts[1], verts[2] ) / denom;
		b = idWinding::TriangleArea( closest, verts[2], verts[0] ) / denom;
		c = idWinding::TriangleArea( closest, verts[0], verts[1] ) / denom;

		barycentricWeights.Set( a, b, c );

		// are there at least 2 old matching indices then interpolate from the old barycentrics over time
		if( numInterpolantsDuringChange == 2 && ( time < ( timeInterpolateStart + c_interpolationTimeframe ) ) )
		{
			float t = -float( timeInterpolateStart - time ) / c_interpolationTimeframe;

			t = idMath::ClampFloat( 0.0f, 1.0f, t );

			barycentricWeights[mapIndexes[0]] = Lerp( oldBarycentricWeights[interpolants[0]], barycentricWeights[mapIndexes[0]], t );
			barycentricWeights[mapIndexes[1]] = Lerp( oldBarycentricWeights[interpolants[1]], barycentricWeights[mapIndexes[1]], t );
			barycentricWeights.z = 1.0f - idMath::Sqrt( idMath::Fabs( barycentricWeights.x * barycentricWeights.x + barycentricWeights.y * barycentricWeights.y ) );

#if 0
			idLib::Printf( "start %i time %i lerp %.2f old[ %.2f %.2f %.2f] new [ %.2f %.2f %.2f]\n", timeInterpolateStart, time, t,
						   oldBarycentricWeights[0], oldBarycentricWeights[1], oldBarycentricWeights[2],
						   barycentricWeights.x, barycentricWeights.y, barycentricWeights.z );
#endif
		}
		else
		{
			oldBarycentricWeights[0] = barycentricWeights[0];
			oldBarycentricWeights[1] = barycentricWeights[1];
			oldBarycentricWeights[2] = barycentricWeights[2];
		}
	}

	oldIndexes[0] = triIndexes[0];
	oldIndexes[1] = triIndexes[1];
	oldIndexes[2] = triIndexes[2];

	tr.viewDef->radianceImageBlends.Set( barycentricWeights.x, barycentricWeights.y, barycentricWeights.z, 0.0f );

	for( int i = 0; i < viewEnvprobes.Num() && i < 3; i++ )
	{
		if( !viewEnvprobes[i]->radianceImage->IsDefaulted() )
		{
			tr.viewDef->radianceImages[i] = viewEnvprobes[i]->radianceImage;
		}
	}

	if( tr.viewDef->radianceImages[0] == globalImages->defaultUACRadianceCube &&
			tr.viewDef->radianceImages[1] == globalImages->defaultUACRadianceCube &&
			tr.viewDef->radianceImages[2] == globalImages->defaultUACRadianceCube )
	{
		// this didn't work so this is the way to tell the backend and avoid blood reflections
		tr.viewDef->globalProbeBounds.Clear();
		tr.viewDef->probePositions[0].w = 0.0f;	// invalidate the per-pixel probe blend too
	}
}
// RB end

// RT occluder gather (feeds the ray-tracing TLAS for shadows + reflections). Master
// switch lives with the accel-struct code; radius bounds the gather to nearby areas.
extern idCVar r_rtWorldOccluders;
idCVar r_rtOccluderAreaHops( "r_rtOccluderAreaHops", "6", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "RT occluders: portal hops out from the camera area to gather static world geometry (0 = camera area only). Larger = fewer off-view shadow/reflection dropouts, more TLAS cost", 0, 64 );
idCVar r_rtOccluderDebug( "r_rtOccluderDebug", "1", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "1 = print gather stats; 2 = also dump suspect-surface BLAS inputs; 3 = gather ONLY world area models (debug isolation)", 0, 3 );

/*
================
R_GatherRTOccluders

Snapshot the static world geometry of the camera's portal-connected areas (frustum-
independent) into viewDef->rtOccluders, for the ray-tracing TLAS. Runs in the single-
threaded frontend after FindViewLightsAndEntities (so connectedAreas is known and the
render world is safe to read), producing a frozen frame-allocated array the backend can
consume without touching the live world. This is the occluder source that keeps shadows
and reflections of off-view geometry alive when the camera rotates in place.

Each portal area's static "_area%i" world model is entityDefs[area]; its surface caches
are made static-resident at load, so we just read the handles (no allocation).
================
*/
static void R_GatherRTOccluders( viewDef_t* viewDef )
{
	viewDef->rtOccluders = NULL;
	viewDef->numRtOccluders = 0;

	if( !r_rtWorldOccluders.GetBool() )
	{
		return;
	}

	idRenderWorldLocal* world = static_cast<idRenderWorldLocal*>( viewDef->renderWorld );
	if( world == NULL || world->numPortalAreas <= 0 )
	{
		return;
	}

	// Collect nearby areas by flooding portals out from the camera area (hop-capped),
	// through any portal not sealed by a closed door (PS_BLOCK_VIEW). This is the set of
	// areas the camera could see into by moving/rotating. viewDef->connectedAreas is NOT
	// this set - it collapses to just the camera area in an enclosed room (measured 1/94
	// at the delta1 spawn), which starved the gather. Outside the world (areaNum < 0)
	// gather every area.
	bool* areaVisited = ( bool* )R_ClearedFrameAlloc( world->numPortalAreas * sizeof( bool ) );
	const int camArea = viewDef->areaNum;
	// RT shadows: occlusion-cull NOTHING. Portal flooding (hop cap + PS_BLOCK_VIEW) drops
	// occluders that stencil still shadows with, so a shadow ray finds no blocker and the
	// area reads unshadowed. Gather EVERY static occluder in the map into the TLAS when RT is
	// active; areas where this proves too expensive can be re-culled later, one at a time.
	extern idCVar r_useRTShadows;
	const bool gatherEveryArea = ( camArea < 0 || camArea >= world->numPortalAreas ) || r_useRTShadows.GetBool();
	if( gatherEveryArea )
	{
		for( int a = 0; a < world->numPortalAreas; a++ )
		{
			areaVisited[a] = true;
		}
	}
	else
	{
		const int maxHops = r_rtOccluderAreaHops.GetInteger();
		int* frontier = ( int* )R_FrameAlloc( world->numPortalAreas * sizeof( int ) );
		int* nextFrontier = ( int* )R_FrameAlloc( world->numPortalAreas * sizeof( int ) );
		int frontierNum = 0;
		areaVisited[camArea] = true;
		frontier[frontierNum++] = camArea;
		for( int hop = 0; hop < maxHops && frontierNum > 0; hop++ )
		{
			int nextNum = 0;
			for( int f = 0; f < frontierNum; f++ )
			{
				for( portal_t* p = world->portalAreas[frontier[f]].portals; p != NULL; p = p->next )
				{
					if( ( p->doublePortal->blockingBits & PS_BLOCK_VIEW ) != 0 )
					{
						continue;	// sealed by a closed door
					}
					const int na = p->intoArea;
					if( na >= 0 && na < world->numPortalAreas && !areaVisited[na] )
					{
						areaVisited[na] = true;
						nextFrontier[nextNum++] = na;
					}
				}
			}
			int* tmp = frontier;
			frontier = nextFrontier;
			nextFrontier = tmp;
			frontierNum = nextNum;
		}
	}

	// Collect the unique static-model entities in the flooded areas: the per-area world
	// model AND static props (func_static etc.), deduped by entity index. This is what
	// the earlier area-model-only gather missed - an off-view shadow/reflection occluder
	// is often a prop, not BSP world geometry.
	const int numEntityDefs = world->entityDefs.Num();
	bool* entVisited = ( bool* )R_ClearedFrameAlloc( ( numEntityDefs > 0 ? numEntityDefs : 1 ) * sizeof( bool ) );
	idRenderEntityLocal** ents = ( idRenderEntityLocal** )R_FrameAlloc( ( numEntityDefs > 0 ? numEntityDefs : 1 ) * sizeof( idRenderEntityLocal* ) );
	int numEnts = 0;
	for( int a = 0; a < world->numPortalAreas; a++ )
	{
		if( !areaVisited[a] )
		{
			continue;
		}
		const portalArea_t& pa = world->portalAreas[a];
		for( areaReference_t* ref = pa.entityRefs.areaNext; ref != &pa.entityRefs; ref = ref->areaNext )
		{
			idRenderEntityLocal* ent = ref->entity;
			if( ent == NULL || ent->index < 0 || ent->index >= numEntityDefs || entVisited[ent->index] )
			{
				continue;
			}
			entVisited[ent->index] = true;
			if( ent->parms.hModel == NULL )
			{
				continue;
			}
			// static geometry only: skip dynamic / procedural / skinned models - calling
			// Surface() on those is unsafe, and they are not static occluders.
			if( ent->parms.hModel->IsDynamicModel() != DM_STATIC )
			{
				continue;
			}
			// debug (r_rtOccluderDebug 3): gather ONLY the world area models - combined with
			// r_rtAccelDebug 2 (flood-only TLAS) this isolates whether world-brush geometry
			// registers ray hits at all.
			if( r_rtOccluderDebug.GetInteger() == 3 && !ent->parms.hModel->IsStaticWorldModel() )
			{
				continue;
			}
			ents[numEnts++] = ent;
		}
	}

	// Two passes over the collected entities' surfaces: count, allocate, fill.
	int count = 0;
	for( int pass = 0; pass < 2; pass++ )
	{
		rtOccluderSurf_t* list = NULL;
		int n = 0;
		if( pass == 1 )
		{
			if( count == 0 )
			{
				return;
			}
			list = ( rtOccluderSurf_t* )R_FrameAlloc( count * sizeof( rtOccluderSurf_t ), FRAME_ALLOC_UNKNOWN );
		}

		for( int e = 0; e < numEnts; e++ )
		{
			idRenderEntityLocal* ent = ents[e];
			const idRenderModel* model = ent->parms.hModel;
			const int numSurfaces = model->NumSurfaces();
			for( int s = 0; s < numSurfaces; s++ )
			{
				const modelSurface_t* msurf = model->Surface( s );
				if( msurf == NULL || msurf->geometry == NULL )
				{
					continue;
				}
				const srfTriangles_t* tri = msurf->geometry;
				if( tri->numIndexes <= 0 )
				{
					continue;
				}
				if( !vertexCache.CacheIsStatic( tri->ambientCache ) || !vertexCache.CacheIsStatic( tri->indexCache ) )
				{
					continue;
				}

				if( pass == 0 )
				{
					count++;
				}
				else
				{
					rtOccluderSurf_t& o = list[n++];
					o.ambientCache = tri->ambientCache;
					o.indexCache = tri->indexCache;
					o.numVerts = tri->numVerts;
					o.numIndexes = tri->numIndexes;
					o.material = msurf->shader;
					memcpy( o.modelMatrix, ent->modelMatrix, sizeof( o.modelMatrix ) );

					// diagnostics (r_rtOccluderDebug 2): dump the BLAS inputs of suspect surfaces so a
					// surface whose rays inexplicably miss can be compared against one that works.
					if( r_rtOccluderDebug.GetInteger() == 2 && msurf->shader != NULL &&
							( idStr::FindText( msurf->shader->GetName(), "stetile4" ) >= 0 ||
							  idStr::FindText( msurf->shader->GetName(), "common/shadow" ) >= 0 ||
							  idStr::FindText( msurf->shader->GetName(), "deltakiosk" ) >= 0 ) )
					{
						const float* m = ent->modelMatrix;
						common->Printf( "rtOccluder surf model '%s' shader '%s': verts %i idx %i vtxOfs %u idxOfs %u bounds (%.0f %.0f %.0f)-(%.0f %.0f %.0f) mat[row0 %.2f %.2f %.2f %.2f | row1 %.2f %.2f %.2f %.2f | row2 %.2f %.2f %.2f %.2f]\n",
										ent->parms.hModel->Name(), msurf->shader->GetName(),
										tri->numVerts, tri->numIndexes,
										( unsigned )( ( tri->ambientCache >> VERTCACHE_OFFSET_SHIFT ) & VERTCACHE_OFFSET_MASK ),
										( unsigned )( ( tri->indexCache >> VERTCACHE_OFFSET_SHIFT ) & VERTCACHE_OFFSET_MASK ),
										tri->bounds[0].x, tri->bounds[0].y, tri->bounds[0].z,
										tri->bounds[1].x, tri->bounds[1].y, tri->bounds[1].z,
										m[0], m[4], m[8], m[12], m[1], m[5], m[9], m[13], m[2], m[6], m[10], m[14] );
					}
				}
			}
		}

		if( pass == 1 )
		{
			viewDef->rtOccluders = list;
			viewDef->numRtOccluders = n;
		}
	}

	// diagnostic: print on camera-area change so we can read the numbers wherever the
	// dropout is seen, not just at spawn. r_rtOccluderDebug 0 silences it.
	extern idCVar r_rtOccluderDebug;
	static int s_rtLastLoggedArea = -2;
	if( r_rtOccluderDebug.GetBool() && camArea != s_rtLastLoggedArea )
	{
		s_rtLastLoggedArea = camArea;
		int flooded = 0;
		for( int a = 0; a < world->numPortalAreas; a++ )
		{
			if( areaVisited[a] )
			{
				flooded++;
			}
		}
		common->Printf( "R_GatherRTOccluders: camArea %i, %i/%i flooded areas (%i hops), %i static ents, gathered %i occluder surfaces\n",
						camArea, flooded, world->numPortalAreas, r_rtOccluderAreaHops.GetInteger(), numEnts, viewDef->numRtOccluders );
	}
}

/*
================
R_RenderView

A view may be either the actual camera view,
a mirror / remote location, or a 3D view on a gui surface.

Parms will typically be allocated with R_FrameAlloc
================
*/
void R_RenderView( viewDef_t* parms )
{
	// save view in case we are a subview
	viewDef_t* oldView = tr.viewDef;

	tr.viewDef = parms;

	// use this same frame index for the projection matrix jittering here and in the backend!
	tr.viewDef->taaFrameCount = tr.frameCount;

	// setup the matrix for world space to eye space
	R_SetupViewMatrix( tr.viewDef );

	// we need to set the projection matrix before doing
	// portal-to-screen scissor calculations
	R_SetupProjectionMatrix( tr.viewDef, true );
	R_SetupProjectionMatrix( tr.viewDef, false );

	// RB: we need a unprojection matrix to calculate the vertex position based on the depth image value
	// for some post process shaders
	R_SetupUnprojection( tr.viewDef );

	// setup render matrices for faster culling
	idRenderMatrix::Transpose( *( idRenderMatrix* )tr.viewDef->projectionMatrix, tr.viewDef->projectionRenderMatrix );
	idRenderMatrix viewRenderMatrix;
	idRenderMatrix::Transpose( *( idRenderMatrix* )tr.viewDef->worldSpace.modelViewMatrix, viewRenderMatrix );
	idRenderMatrix::Multiply( tr.viewDef->projectionRenderMatrix, viewRenderMatrix, tr.viewDef->worldSpace.mvp );

	idRenderMatrix::Transpose( *( idRenderMatrix* )tr.viewDef->unjitteredProjectionMatrix, tr.viewDef->unjitteredProjectionRenderMatrix );
	idRenderMatrix::Multiply( tr.viewDef->unjitteredProjectionRenderMatrix, viewRenderMatrix, tr.viewDef->worldSpace.unjitteredMVP );

	// the planes of the view frustum are needed for portal visibility culling
	idRenderMatrix::GetFrustumPlanes( tr.viewDef->frustums[FRUSTUM_PRIMARY], tr.viewDef->worldSpace.mvp, false, true );

	// the DOOM 3 frustum planes point outside the frustum
	for( int i = 0; i < 6; i++ )
	{
		tr.viewDef->frustums[FRUSTUM_PRIMARY][i] = - tr.viewDef->frustums[FRUSTUM_PRIMARY][i];
	}
	// remove the Z-near to avoid portals from being near clipped
	tr.viewDef->frustums[FRUSTUM_PRIMARY][4][3] -= r_znear.GetFloat();

	// RB: prepare subfrustums for cascaded shadow mapping of sun lights
	R_SetupSplitFrustums( tr.viewDef );

	// identify all the visible portal areas, and create view lights and view entities
	// for all the the entityDefs and lightDefs that are in the visible portal areas
	static_cast<idRenderWorldLocal*>( parms->renderWorld )->FindViewLightsAndEntities();

	// snapshot static world occluders from the camera's connected areas for the RT TLAS
	// (frustum-independent, so shadows / reflections of off-view geometry don't drop out)
	R_GatherRTOccluders( tr.viewDef );

	// wait for any shadow volume jobs from the previous frame to finish
	tr.frontEndJobList->Wait();

	// RB: render worldspawn geometry to the software culling buffer
	R_FillMaskedOcclusionBufferWithModels( tr.viewDef );

	// make sure that interactions exist for all light / entity combinations that are visible
	// add any pre-generated light shadows, and calculate the light shader values
	R_AddLights();

	// adds ambient surfaces and create any necessary interaction surfaces to add to the light lists
	R_AddModels();

	// build up the GUIs on world surfaces
	R_AddInGameGuis( tr.viewDef->drawSurfs, tr.viewDef->numDrawSurfs );

	// any viewLight that didn't have visible surfaces can have it's shadows removed
	R_OptimizeViewLightsList();

	// sort all the ambient surfaces for translucency ordering
	R_SortDrawSurfs( tr.viewDef->drawSurfs, tr.viewDef->numDrawSurfs );

	// generate any subviews (mirrors, cameras, etc) before adding this view
	if( R_GenerateSubViews( tr.viewDef->drawSurfs, tr.viewDef->numDrawSurfs ) )
	{
		// if we are debugging subviews, allow the skipping of the main view draw
		if( r_subviewOnly.GetBool() )
		{
			return;
		}
	}

	// RB: find closest environment probes so we can interpolate between them in the ambient shaders
	R_FindClosestEnvironmentProbes();

	// soft-shadow scene capture: snapshot the main view's lights/edges/caster-meshes (frontend half) before
	// the view is handed to the backend, which finishes the capture with the screenshot.
	if( R_SoftShadowCaptureArmed() && !parms->isSubview )
	{
		R_CaptureFrontendView( tr.viewDef );
	}

	// add the rendering commands for this viewDef
	R_AddDrawViewCmd( parms, false );

	// restore view in case we are a subview
	tr.viewDef = oldView;
}

/*
================
R_RenderPostProcess

Because R_RenderView may be called by subviews we have to make sure the post process
pass happens after the active view and its subviews is done rendering.
================
*/
void R_RenderPostProcess( viewDef_t* parms )
{
	viewDef_t* oldView = tr.viewDef;

	if( !( parms->renderView.rdflags & RDF_IRRADIANCE ) )
	{
		R_AddDrawPostProcess( parms );
	}

	tr.viewDef = oldView;
}
