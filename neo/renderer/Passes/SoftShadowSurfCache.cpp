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
#include <algorithm>	// std::sort in DumpTableHistogram
#pragma hdrstop

#include "renderer/RenderCommon.h"
#include "renderer/RenderWorld_local.h"		// idRenderWorldLocal::lightDefs (warm-queue resolution)
#include "framework/Common_local.h"			// commonLocal.GetRendererGPUMicroseconds (emergency-FPS gate)
#include "SoftShadowSurfCache.h"

// mirrors c_SurfBuild in softsurf_build.cs.hlsl
struct SoftSurfBuildCB
{
	float	lightR[4];		// light origin xyz, disk radius w
	int		range[4];		// triBase (float4 elems), staticCasterCount, casterBase (float4 elems), lightKey
	int		caps[4];		// tableCap (slots), poolCap (uints), budget (threads), max residual per texel
	float	params[4];		// texel G, 2nd-derivative threshold, queue capacity (uints), unused
	int		seed[4];		// mode (0 queue / 1 table-scan), scan start slot, static tri count (seed CS), unused
};

// hard per-texel residual ceiling: a texel needing more than this many exact-walked occluders is too
// curved to be worth caching (WALK-ALWAYS, exact) and would bloat the pool.
static const int SW_SURF_MAX_RESIDUAL = 1024;

// RUNTIME (post-burst) drain diagnostics: rebuilds dispatched + GC table-wipes since the last read.
// Read+reset by the com_softShadowFrameProbe summary to confirm whether a cutscene re-warm storm exists.
static int s_runtimeBuilds = 0, s_gcClears = 0;
void R_SoftCacheWarmDebugCounters( int& builds, int& clears )
{
	builds = s_runtimeBuilds;
	clears = s_gcClears;
	s_runtimeBuilds = 0;
	s_gcClears = 0;
}

// IN-FLIGHT runtime drain (audit #7b): a non-fullDrain WarmLight seeds the WHOLE light but builds only
// the first budget window; the remaining windows must keep draining on later frames or the light stays
// part-warm forever - its queue tail is REQUESTED and the read-only term never re-requests, and a
// same-generation re-seed enqueues NOTHING (softsurf_seed only enqueues empty/stale-gen claims), so
// "just re-warm it" can never finish the tail (InvalidateLight on a big light hit exactly this).
// The seed queue holds ONE light's seed at a time, so the in-flight drain is a single (light,
// next-window) pair plus the parking warm's CB. File-scope static, not a member (growing the class
// layout is the known init-fault landmine, see the grid statics below). Render-thread only.
struct SwDrainState
{
	int				light = -1;		// lightDef index mid-drain; -1 = none
	int				window = 0;		// next unbuilt build window of the parked seed queue
	bool			grid = false;	// pipeline the parking warm ran on
	SoftSurfBuildCB	cb;				// the parking warm's CB (caps[2] = its budget, seed[2] = MinROI)
};
static SwDrainState s_swDrain;

SoftShadowSurfCache::SoftShadowSurfCache( nvrhi::IDevice* device )
	: m_Device( device )
{
}

// GRID-mode GPU resources kept OFF the class layout (not members): growing SoftShadowSurfCache shifts its
// heap layout and surfaces an init-time fault in the render backend (same reason the reduced-set snapshot
// below uses statics). They must ALSO outlive normal static teardown: a file-scope nvrhi handle runs its
// destructor at PROGRAM EXIT, after the Vulkan device is already gone, and ComputePipeline::~ - >
// vkDestroyPipeline on a dead device SIGSEGVs (confirmed backtrace 2026-08-23). So hold them in a
// heap-allocated struct that is INTENTIONALLY LEAKED (accessor's local static pointer, never freed): the
// handle destructors never run, and the GPU memory is reclaimed by device teardown. Single cache instance.
struct SwGridStatics
{
	nvrhi::ShaderHandle				shader;
	nvrhi::ComputePipelineHandle	pipeline;
	nvrhi::BufferHandle				buffer;			// parallel per-slot Fubini grid (SW_SCAN_CHORDS uints/slot)
	int								bufferCap = 0;	// slot capacity the grid buffer was sized for
};
static SwGridStatics& swGrid()
{
	static SwGridStatics* g = new SwGridStatics();	// leaked on purpose - see above; never delete
	return *g;
}

nvrhi::IBuffer* SoftShadowSurfCache::GetGridBuffer() const
{
	return swGrid().buffer;
}

// PERSISTENT static-caster stream (audit finding #4): each warmed light's [tris][casters] float4
// stream APPENDED at a per-light base and kept for the cache's lifetime, so the residual pool can
// store 1-uint tri indices into it (12x pool capacity vs the old self-contained 12-uint records)
// with nothing to dangle - unlike the reused m_WarmStream or the per-frame view stream. Same
// leaked-statics pattern (and reasons) as SwGridStatics above. Re-warming a light appends a FRESH
// segment and the old one leaks until the next full clear - the same accepted leak model as the
// pool's tombstoned reservations (DoClearIfNeeded reclaims both wholesale).
struct SwStreamStatics
{
	nvrhi::BufferHandle				buffer;
	int								capF4 = 0;		// capacity in float4 elements
	int								cursorF4 = 0;	// append cursor (float4 elements)
	std::unordered_map<int, int>	baseF4;			// lightDef->index -> segment base (float4 elements)
};
static SwStreamStatics& swStream()
{
	static SwStreamStatics* g = new SwStreamStatics();	// leaked on purpose - see SwGridStatics; never delete
	return *g;
}

nvrhi::IBuffer* SoftShadowSurfCache::GetStaticStream() const
{
	return swStream().buffer;
}

int SoftShadowSurfCache::GetLightStreamBase( int lightIndex ) const
{
	auto it = swStream().baseF4.find( lightIndex );
	return it != swStream().baseF4.end() ? it->second : -1;
}

void SoftShadowSurfCache::EnsurePipeline()
{
	if( m_PipelineTried )
	{
		return;
	}
	m_PipelineTried = true;

	idList<shaderMacro_t> macros;
	macros.Append( shaderMacro_t( "SW_SURF_GRID", "0" ) );	// scalar fold permutation (blob keys carry all axes)
	m_Shader = renderProgManager.GetShader( renderProgManager.FindShader( "builtin/lighting/softsurf_build", SHADER_STAGE_COMPUTE, "", macros, true, LAYOUT_DRAW_VERT ) );
	if( m_Shader == nullptr )
	{
		common->Warning( "SoftShadowSurfCache: build compute shader failed to load - surface-fold cache disabled." );
		return;
	}

	nvrhi::BindingLayoutDesc ld;
	ld.visibility = nvrhi::ShaderType::Compute;
	ld.bindings =
	{
		nvrhi::BindingLayoutItem::VolatileConstantBuffer( 0 ),	// b0 : build constants
		nvrhi::BindingLayoutItem::StructuredBuffer_SRV( 0 ),	// t0 : tri stream + caster table (joint)
		nvrhi::BindingLayoutItem::StructuredBuffer_SRV( 1 ),	// t1 : dummy (include requirement)
		nvrhi::BindingLayoutItem::StructuredBuffer_UAV( 0 ),	// u0 : texel table
		nvrhi::BindingLayoutItem::StructuredBuffer_UAV( 1 ),	// u1 : residual pool
		nvrhi::BindingLayoutItem::StructuredBuffer_UAV( 2 ),	// u2 : request queue
	};
	m_Layout = m_Device->createBindingLayout( ld );

	nvrhi::ComputePipelineDesc pd;
	pd.bindingLayouts = { m_Layout };
	pd.CS = m_Shader;
	m_Pipeline = m_Device->createComputePipeline( pd );

	// GRID build permutation (SW_SURF_GRID=1, r_softShadowSurfCacheGrid): rasterises the Fubini bit-grid
	// into the parallel grid buffer, bound at u1 in place of the residual pool (unused in grid mode). REUSES
	// the scalar build layout m_Layout (identical u0 table, u1, u2 queue); separate shader + pipeline only.
	{
		idList<shaderMacro_t> gridMacros;
		gridMacros.Append( shaderMacro_t( "SW_SURF_GRID", "1" ) );
		swGrid().shader = renderProgManager.GetShader( renderProgManager.FindShader( "builtin/lighting/softsurf_build", SHADER_STAGE_COMPUTE, "grid", gridMacros, true, LAYOUT_DRAW_VERT ) );
		if( swGrid().shader != nullptr )
		{
			nvrhi::ComputePipelineDesc pg;
			pg.bindingLayouts = { m_Layout };
			pg.CS = swGrid().shader;
			swGrid().pipeline = m_Device->createComputePipeline( pg );
		}
	}

	// PREWARM seed pass (one thread per static tri -> texel claims with triangle-plane anchors).
	// Failure is non-fatal: the cache just warms lazily via the fragment-claim path instead.
	{
		idList<shaderMacro_t> seedMacros;
		m_SeedShader = renderProgManager.GetShader( renderProgManager.FindShader( "builtin/lighting/softsurf_seed", SHADER_STAGE_COMPUTE, "", seedMacros, true, LAYOUT_DRAW_VERT ) );
		if( m_SeedShader != nullptr )
		{
			nvrhi::BindingLayoutDesc sl;
			sl.visibility = nvrhi::ShaderType::Compute;
			sl.bindings =
			{
				nvrhi::BindingLayoutItem::VolatileConstantBuffer( 0 ),	// b0 : shared build/seed constants
				nvrhi::BindingLayoutItem::StructuredBuffer_SRV( 0 ),	// t0 : tri stream (joint)
				nvrhi::BindingLayoutItem::StructuredBuffer_UAV( 0 ),	// u0 : texel table
				nvrhi::BindingLayoutItem::StructuredBuffer_UAV( 1 ),	// u1 : request queue (seed enqueues claimed slots)
			};
			m_SeedLayout = m_Device->createBindingLayout( sl );
			nvrhi::ComputePipelineDesc sp;
			sp.bindingLayouts = { m_SeedLayout };
			sp.CS = m_SeedShader;
			m_SeedPipeline = m_Device->createComputePipeline( sp );
		}
	}

	nvrhi::BufferDesc cb;
	cb.byteSize = sizeof( SoftSurfBuildCB );
	cb.isConstantBuffer = true;
	cb.isVolatile = true;
	// 16384 (was 1024): the full-drain warm writes the CB once per build WINDOW (up to 16/light with
	// the table-sized queue) plus the seed write - a 220-light burst frame versions past 1024 and
	// Sys_Errors. The version ring is CPU-side staging; at ~200 B/version this is ~3 MB. (2026-08-26)
	cb.maxVersions = 16384;
	cb.debugName = "SoftShadowSurfCache/CB";
	m_ConstantBuffer = m_Device->createBuffer( cb );
}

// Ensure pipelines + GPU buffers exist and match the current cvar sizing. Callable BEFORE any view
// (the warm-queue drain in GL_StartFrame uses it), so the camera-independent build path can run even
// on the very first frame. Returns true when ready to dispatch.
bool SoftShadowSurfCache::EnsureResources()
{
	extern idCVar r_softShadowSurfCacheTexel, r_softShadowSurfCacheSecondThr;
	extern idCVar r_softShadowSurfCacheBudget, r_softShadowSurfCacheCap, r_softShadowSurfCachePoolCap;
	extern idCVar r_softShadowSurfCacheErrTol, r_softShadowSurfCacheWarmBudget;
	extern idCVar r_softShadowSurfCacheReduced, r_softShadowSurfCacheReducedK, r_softShadowSurfCacheReducedCutoff;
	extern idCVar r_softShadowSurfCacheGrid;
	EnsurePipeline();
	if( m_Pipeline == nullptr )
	{
		return false;
	}
	// snapshot the params; capacity is rounded DOWN to a power of two (the shader masks with cap-1)
	int cap = r_softShadowSurfCacheCap.GetInteger();
	int capP2 = 65536;
	while( capP2 * 2 <= cap )
	{
		capP2 *= 2;
	}
	const int poolCap = r_softShadowSurfCachePoolCap.GetInteger();
	const int budget = r_softShadowSurfCacheBudget.GetInteger();
	const int warmBudget = r_softShadowSurfCacheWarmBudget.GetInteger();
	// queue must hold EVERY texel a light can seed (task #87 full drain): sizing it to the warm budget
	// silently truncated big lights at the seed - capacity = table cap + count word (4 MB at 1M slots)
	const int wantQueue = Max( Max( budget + 1, warmBudget + 1 ), capP2 + 1 );
	const float texel = ( float )r_softShadowSurfCacheTexel.GetInteger();
	const float thr = r_softShadowSurfCacheSecondThr.GetFloat();
	const float errTol = r_softShadowSurfCacheErrTol.GetFloat();
	const int reduced = r_softShadowSurfCacheReduced.GetBool() ? 1 : 0;
	const int reducedK = r_softShadowSurfCacheReducedK.GetInteger();
	const float reducedCut = r_softShadowSurfCacheReducedCutoff.GetFloat();
	const int grid = r_softShadowSurfCacheGrid.GetBool() ? 1 : 0;
	// reduced-set + grid-mode snapshots kept as function statics, not class members: growing SoftShadowSurfCache
	// shifts its heap layout and surfaces an init-time fault in the render backend (see the .h note). Toggling
	// grid mode changes what BUILD writes (bit-grid vs scalar fold), so it must DROP+rebuild the cache.
	static int s_reduced = -1, s_reducedK = -1, s_gridMode = -1;
	static float s_reducedCut = -1.0f;
	if( m_Table == nullptr || capP2 != m_TableCap || poolCap != m_PoolCap || budget != m_Budget
			|| wantQueue != m_QueueWords || texel != m_Texel || thr != m_Thr || errTol != m_ErrTol
			|| reduced != s_reduced || reducedK != s_reducedK || reducedCut != s_reducedCut || grid != s_gridMode )
	{
		m_TableCap = capP2;
		m_PoolCap = poolCap;
		m_Budget = budget;
		m_QueueWords = wantQueue;	// must hold the warm build's per-light SW_WARM_BUDGET enqueued texels
		m_Texel = texel;
		m_Thr = thr;
		m_ErrTol = errTol;		// build semantics changed: drop + reseed
		s_reduced = reduced;
		s_reducedK = reducedK;
		s_reducedCut = reducedCut;
		s_gridMode = grid;

		nvrhi::BufferDesc bd;
		bd.structStride = sizeof( uint32_t );
		bd.canHaveUAVs = true;
		bd.initialState = nvrhi::ResourceStates::UnorderedAccess;
		bd.keepInitialState = true;

		bd.byteSize = ( ( uint64_t )m_TableCap * 8 + 8 + 32 ) * sizeof( uint32_t );	// +8: 4 path counters + 4 miss sub-reason counters; +32: per-light {hit,miss} pairs keyed lightKey&15 (bench attribution)
		bd.debugName = "SoftShadowSurfCache/Table";
		m_Table = m_Device->createBuffer( bd );

		bd.byteSize = ( uint64_t )m_PoolCap * sizeof( uint32_t );
		bd.debugName = "SoftShadowSurfCache/Pool";
		m_Pool = m_Device->createBuffer( bd );

		bd.byteSize = ( uint64_t )m_QueueWords * sizeof( uint32_t );
		bd.debugName = "SoftShadowSurfCache/Queue";
		m_Queue = m_Device->createBuffer( bd );

		// GRID mode: parallel per-slot Fubini bit-grid, allocated only when r_softShadowSurfCacheGrid is
		// set. The scalar path never touches it. Written by the grid build (u3), read by the surfgrid term
		// (t7). No clear needed: the build writes the grid before marking the slot BUILT, and the term
		// reads it only for BUILT slots. SIZED for the MAX config the shader can compile: SW_SCAN_CHORDS up
		// to 32 x SwGridWord up to uint2 (2 words). The shader indexes at its COMPILED chord stride, so a
		// larger allocation only leaves unused tail - never an overflow.
		if( grid && swGrid().pipeline != nullptr )
		{
			bd.byteSize = ( uint64_t )m_TableCap * 32 /*max chords*/ * 2 /*max words (uint2)*/ * sizeof( uint32_t );
			bd.debugName = "SoftShadowSurfCache/Grid";
			swGrid().buffer = m_Device->createBuffer( bd );
			swGrid().bufferCap = m_TableCap;
		}

		// HUD counter readback ring (non-blocking): CPU-readable staging, one per in-flight frame
		nvrhi::BufferDesc rbd;
		rbd.byteSize = 4 * sizeof( uint32_t );
		rbd.cpuAccess = nvrhi::CpuAccessMode::Read;
		for( int i = 0; i < SW_STATS_RING; i++ )
		{
			rbd.debugName = "SoftShadowSurfCache/StatsRing";
			m_StatsRing[i] = m_Device->createBuffer( rbd );
		}
		m_StatsRingWrite = 0;

		m_NeedClear = true;
		m_LightHash.clear();
		m_ScanCursor = m_TableCap;
	}
	return m_Table != nullptr;
}

// Wipe the whole table/pool/queue when flagged (buffer recreate or GC). Every known light must then
// re-warm; queue them so the camera-independent drain rebuilds them.
void SoftShadowSurfCache::DoClearIfNeeded( nvrhi::ICommandList* commandList )
{
	if( !m_NeedClear )
	{
		return;
	}
	commandList->clearBufferUInt( m_Table, 0xFFFFFFFFu );	// empty sentinel everywhere
	// ...but the 8 PATH/MISS counter words (base tableCap*8) must be ZERO, not the 0xFFFFFFFF sentinel:
	// the per-frame stats readback was reporting 0xFFFFFFFF (a bogus even 25/25/25/25 split) because this
	// clear left them at the sentinel and the first ring copy captured it before EndBuilds' reset ran.
	{
		const uint32_t swZeroC[8 + 32] = { 0 };		// 8 path/miss counters + 32 per-light {hit,miss} pairs
		commandList->writeBuffer( m_Table, swZeroC, sizeof( swZeroC ), ( uint64_t )m_TableCap * 8 * sizeof( uint32_t ) );
	}
	commandList->clearBufferUInt( m_Pool, 0 );
	commandList->clearBufferUInt( m_Queue, 0 );
	// the wipe just emptied the seed queue: a parked part-warm drain (audit #7b) has nothing left to
	// consume, and every light re-warms from scratch below anyway.
	s_swDrain.light = -1;
	s_swDrain.window = 0;
	// persistent static stream: reset the append cursor + segment map. Every leaked segment (re-warmed
	// lights) is reclaimed here, exactly like the pool's leaked reservations; the buffer itself is kept
	// (contents are garbage until lights re-warm, and nothing serves before a re-warm rebuilds records).
	swStream().cursorF4 = 0;
	swStream().baseF4.clear();
	m_NeedClear = false;
	// a wholesale wipe reclaims every orphaned slot by definition - the bump counter's debt is spent.
	// NOT resetting it here was harness-audit finding #3: the counter survived the wipe, so the FIRST
	// BeginView after a warm burst crossed the >=64 threshold again and erased the entire just-warmed
	// table, then re-warm dripped at 1 light/frame - the caps caught by the wipe benched ~0% hit.
	m_GenBumps = 0;
	m_StatsRingFilled = 0;		// ring slots are stale after a wipe; don't read them until re-filled
	s_gcClears++;
	m_ScanCursor = m_TableCap;
	std::lock_guard<std::mutex> lock( m_WarmMutex );
	for( auto& kv : m_LightHash )
	{
		kv.second.seedPending = true;
		kv.second.needSweep = true;
		m_WarmQueue.push_back( kv.first );		// re-warm everyone the wipe cleared
	}
}

bool SoftShadowSurfCache::BeginView( nvrhi::ICommandList* commandList, const viewDef_t* viewDef, uint64_t lastGpuMicros )
{
	( void )viewDef;
	( void )lastGpuMicros;
	m_Active = false;
	extern idCVar r_softShadowSurfCache;
	if( !r_softShadowSurfCache.GetBool() )
	{
		return false;
	}
	if( !EnsureResources() )
	{
		return false;
	}

	// GC: enough per-light generation bumps have orphaned slots -> reclaim wholesale (rare).
	const int SW_SURF_GC_BUMPS = 64;
	if( m_GenBumps >= SW_SURF_GC_BUMPS )
	{
		m_NeedClear = true;
		m_GenBumps = 0;
	}
	DoClearIfNeeded( commandList );

	// The runtime term pass is READ-ONLY: it serves warm-cache HITs and walks the view-culled stream on
	// a MISS. ALL building is camera-independent via the warm queue (DrainWarmQueue, run before the
	// view), so camera motion never triggers a build - no per-view seeding / governor / sweep here.
	m_SeededThisFrame = false;
	m_WantRestart = false;
	m_DispatchedLights.clear();
	m_ActiveBudget = m_Budget;
	m_Active = true;
	return true;
}

void SoftShadowSurfCache::BuildLight( nvrhi::ICommandList* commandList, const viewLight_t* vLight,
									  nvrhi::IBuffer* jointBuffer, uint32_t triBase, uint32_t casterBase,
									  float penumbraRadius )
{
	// READ-ONLY runtime (plan noble-sniffing-rose): the per-view path no longer seeds or builds. ALL
	// building is camera-independent via the warm queue (WarmLight / DrainWarmQueue), so camera motion
	// cannot trigger a build and cannot spike frame time. The term pass reads the warm table for HITs;
	// a MISS walks the view-culled stream (jointBuffer) exactly as the uncached path would.
	( void )commandList;
	( void )vLight;
	( void )jointBuffer;
	( void )triBase;
	( void )casterBase;
	( void )penumbraRadius;
}

// Returns true if it dispatched a (re)build this call (so the drain does one per frame), false if it
// cheap-skipped an already-warm light or found nothing to warm.
// accumulators for the one-shot warm-at-load summary line (reset in WarmMapBurst)
static int s_warmRecvTotal = 0, s_warmCasterTotal = 0, s_warmStreamedLights = 0;

bool SoftShadowSurfCache::WarmLight( nvrhi::ICommandList* commandList, const idRenderLightLocal* light, bool fullDrain )
{
	if( light == NULL || m_Pipeline == nullptr || m_Table == nullptr )
	{
		return false;
	}
	// harness-audit guard: a direct WarmLight call while a wipe is PENDING would seed into a table
	// about to be cleared (the gate's per-probe warm was exposed to exactly this). No-op otherwise.
	DoClearIfNeeded( commandList );
	// EMERGENCY build (r_softShadowSurfCacheEmergencyFPS): a frame slower than the threshold is
	// unshippable regardless - full-drain the build so the cache CONVERGES instead of dripping.
	{
		extern idCVar r_softShadowSurfCacheEmergencyFPS;
		const float swEmFps = r_softShadowSurfCacheEmergencyFPS.GetFloat();
		if( !fullDrain && swEmFps > 0.0f
				&& commonLocal.GetRendererGPUMicroseconds() > ( uint64 )( 1000000.0f / swEmFps ) )
		{
			fullDrain = true;
		}
	}
	extern idCVar r_shadowPenumbraSize, r_softShadowSurfCacheErrTol;
	extern bool R_BuildLightStaticSoftStream( const idRenderLightLocal* light, float penumbraSize,
			idList<idVec4>& outTris, idList<idVec4>& outCasters, idList<idVec4>& outRecvTris,
			int& outStaticCasters, int& outStaticTris, int& outRecvTriCount, uint64_t& outFingerprint );
	extern uint64_t R_LightStaticChainSig( const idRenderLightLocal* light, float penumbraSize );
	extern float R_SoftPenumbraRadius( const idRenderLightLocal* lightDef );	// per-light emitter radius (task #105)

	const float penumbra = R_SoftPenumbraRadius( light );

	// CHEAP skip: static-set signature unchanged and this light is fully warm -> nothing to do (no
	// face collection). This is what makes the per-light scan affordable.
	const uint64_t sig = R_LightStaticChainSig( light, penumbra );
	{
		auto sit = m_LightHash.find( light->index );
		if( sig != 0 && sit != m_LightHash.end() && sit->second.hash == sig
				&& !sit->second.seedPending && !sit->second.needSweep )
		{
			return false;
		}
	}

	// parked-drain helpers (audit #7b), shared by the CONTINUATION just below and the pre-seed FLUSH:
	// build windows [w0,w1) of the parked seed queue against the stored CB - identical dispatches to
	// the fresh path's windowed build (each stays at the parking warm's proven-safe budget bound).
	auto swDrainTotalWindows = [this]() -> int
	{
		return ( m_QueueWords - 1 + s_swDrain.cb.caps[2] - 1 ) / s_swDrain.cb.caps[2];
	};
	auto swDrainDispatch = [&]( int w0, int w1 )
	{
		if( swStream().buffer == nullptr )
		{
			return;		// no persistent stream = nothing was ever warmed = nothing parked to drain
		}
		nvrhi::BindingSetDesc sd;
		sd.bindings =
		{
			nvrhi::BindingSetItem::ConstantBuffer( 0, m_ConstantBuffer ),
			// PERSISTENT static stream (audit #4 merge): the parking warm's CB range bases are
			// segment-relative into this buffer - the reused m_WarmStream is dead/bypassed.
			nvrhi::BindingSetItem::StructuredBuffer_SRV( 0, swStream().buffer ),
			nvrhi::BindingSetItem::StructuredBuffer_SRV( 1, swStream().buffer ),	// dummy t1 (include requirement)
			nvrhi::BindingSetItem::StructuredBuffer_UAV( 0, m_Table ),
			nvrhi::BindingSetItem::StructuredBuffer_UAV( 1, s_swDrain.grid ? swGrid().buffer.Get() : m_Pool.Get() ),
			nvrhi::BindingSetItem::StructuredBuffer_UAV( 2, m_Queue ),
		};
		nvrhi::BindingSetHandle set = m_Device->createBindingSet( sd, m_Layout );
		nvrhi::ComputeState cs;
		cs.pipeline = s_swDrain.grid ? swGrid().pipeline : m_Pipeline;
		cs.bindings = { set };
		for( int wb = w0; wb < w1; wb++ )
		{
			s_swDrain.cb.seed[1] = wb * s_swDrain.cb.caps[2];	// queue window base (build CS: qi = g_seed.y + i)
			commandList->writeBuffer( m_ConstantBuffer, &s_swDrain.cb, sizeof( s_swDrain.cb ) );
			commandList->setComputeState( cs );
			commandList->dispatch( ( s_swDrain.cb.caps[2] + 63 ) / 64, 1, 1 );
		}
	};

	// DRAIN CONTINUATION (audit #7b): a previous non-fullDrain warm of THIS light built only the first
	// budget window and parked the rest; the seed queue and warm stream are untouched since (EndBuilds
	// no longer clears the queue, and any other light's warm flushes the park before clobbering them),
	// so just build the next window - no face re-collection, no re-seed. Converges: one window per
	// frame via the warm queue until every seeded texel is built (empty tail windows retire on the
	// queue-count check), fullDrain finishes in one call.
	if( s_swDrain.light == light->index && sig != 0 )
	{
		auto sit = m_LightHash.find( light->index );
		if( sit != m_LightHash.end() && sit->second.hash == sig && !sit->second.seedPending )
		{
			const int swTotal = swDrainTotalWindows();
			const int wEnd = fullDrain ? swTotal : s_swDrain.window + 1;
			swDrainDispatch( s_swDrain.window, wEnd );
			s_swDrain.window = wEnd;
			if( wEnd >= swTotal )
			{
				s_swDrain.light = -1;
				sit->second.needSweep = false;	// drained: every seeded texel is built
			}
			else
			{
				EnqueueWarm( light );			// next frame continues the drain (one window/frame)
			}
			return true;
		}
	}

	idList<idVec4> tris, casters, recvTris;
	int nCas = 0, nTris = 0, nRecv = 0;
	uint64_t fp = 0;
	if( !R_BuildLightStaticSoftStream( light, penumbra, tris, casters, recvTris, nCas, nTris, nRecv, fp ) )
	{
		// no static casters (or moved): remember it so the scan does not re-collect it every pass.
		lightState_t& se = m_LightHash[ light->index ];
		se.hash = ( sig != 0 ) ? sig : 1;
		se.seedPending = false;
		se.needSweep = false;
		if( s_swDrain.light == light->index )
		{
			s_swDrain.light = -1;	// its static set is gone: the parked queue tail is stale, drop it
		}
		return false;
	}

	lightState_t& st = m_LightHash[ light->index ];
	if( st.hash != 0 && st.hash != fp )
	{
		st.generation++;		// static set changed: orphan this light's stale slots
		m_GenBumps++;
	}
	st.hash = fp;

	// FLUSH a parked drain (audit #7b): the seed queue written below is a shared single buffer -
	// seeding this light over it would strand the part-drained light's REQUESTED tail as a
	// permanent miss. Finish it now: the same proven-safe per-window dispatches, and windows past the
	// actually-enqueued count retire on the queue-count check (near-free).
	if( s_swDrain.light >= 0 )
	{
		swDrainDispatch( s_swDrain.window, swDrainTotalWindows() );
		auto dit = m_LightHash.find( s_swDrain.light );
		if( dit != m_LightHash.end() && !dit->second.seedPending )
		{
			dit->second.needSweep = false;		// drained: every seeded texel is built
		}
		s_swDrain.light = -1;
	}

	// pack [tris][casters] into the PERSISTENT static stream at this light's appended base (audit
	// finding #4): the build walks it AND the residual pool's 1-uint tri indices resolve against it at
	// serve, so the segment must outlive the warm (the old reused m_WarmStream could not). Grow on
	// demand with a COPY (other lights' live segments must survive the realloc). A re-warmed light's
	// old segment leaks until the next full clear - same leak model as the pool reservations.
	const int triF4 = tris.Num();		// 3 per tri
	const int casF4 = casters.Num();	// 2 per caster
	const int needF4 = triF4 + casF4;
	if( needF4 <= 0 )
	{
		return false;		// nothing to stream (mirrors the old EnsureWarmStream(0) -> null early-out)
	}
	if( swStream().buffer == nullptr || swStream().cursorF4 + needF4 > swStream().capF4 )
	{
		int want = swStream().capF4 > 0 ? swStream().capF4 : 65536;
		while( want < swStream().cursorF4 + needF4 )
		{
			want *= 2;
		}
		nvrhi::BufferDesc bd;
		bd.byteSize = ( uint64_t )want * sizeof( idVec4 );
		bd.structStride = sizeof( idVec4 );		// float4 stride (matches t_SoftEdges/t_SurfStream StructuredBuffer<float4>)
		bd.canHaveUAVs = false;
		bd.initialState = nvrhi::ResourceStates::ShaderResource;
		bd.keepInitialState = true;
		bd.debugName = "SoftShadowSurfCache/StaticStream";
		nvrhi::BufferHandle grown = m_Device->createBuffer( bd );
		if( grown == nullptr )
		{
			return false;
		}
		if( swStream().buffer != nullptr && swStream().cursorF4 > 0 )
		{
			commandList->copyBuffer( grown, 0, swStream().buffer, 0, ( uint64_t )swStream().cursorF4 * sizeof( idVec4 ) );
		}
		swStream().buffer = grown;
		swStream().capF4 = want;
	}
	const int segBase = swStream().cursorF4;
	swStream().cursorF4 += needF4;
	swStream().baseF4[ light->index ] = segBase;
	commandList->writeBuffer( swStream().buffer, tris.Ptr(), ( size_t )triF4 * sizeof( idVec4 ), ( uint64_t )segBase * sizeof( idVec4 ) );
	commandList->writeBuffer( swStream().buffer, casters.Ptr(), ( size_t )casF4 * sizeof( idVec4 ), ( uint64_t )( segBase + triF4 ) * sizeof( idVec4 ) );

	// RECEIVER stream for the seed (separate buffer): the seed rasterises RECEIVER tris to claim the
	// texels the term reads; the build walks the CASTER stream above. Keying them to the same geometry
	// is what makes reads hit (the old seed rasterised the casters -> receiver reads missed).
	s_warmRecvTotal += nRecv;
	s_warmCasterTotal += nTris;
	s_warmStreamedLights++;
	const int recvF4 = recvTris.Num();		// 3 per receiver tri
	if( recvF4 > 0 )
	{
		EnsureRecvStream( recvF4 );
		if( m_WarmRecvStream != nullptr )
		{
			commandList->writeBuffer( m_WarmRecvStream, recvTris.Ptr(), ( size_t )recvF4 * sizeof( idVec4 ), 0 );
		}
	}

	// QUEUE-mode warm: the seed ENQUEUES this light's claimed texels; the build consumes those in
	// SW_WARM_BUDGET-bounded windows, NOT via a whole-table sweep - a whole-table sweep x this light's
	// casters is what hung the GPU (device removed). fullDrain loops windows over the entire queue
	// (task #87 - the old single window left big lights permanently part-warm = permanent misses);
	// the non-emergency runtime keeps one window per frame.
	extern idCVar r_softShadowSurfCacheWarmBudget;
	const int SW_WARM_BUDGET = Min( r_softShadowSurfCacheWarmBudget.GetInteger(), m_QueueWords - 1 );
	commandList->clearBufferUInt( m_Queue, 0 );		// reset the queue count for this light's seed

	SoftSurfBuildCB cb;
	cb.lightR[0] = light->globalLightOrigin.x;
	cb.lightR[1] = light->globalLightOrigin.y;
	cb.lightR[2] = light->globalLightOrigin.z;
	cb.lightR[3] = penumbra;
	cb.range[0] = segBase;				// triBase (float4 elems) = this light's persistent segment base
	cb.range[1] = nCas;
	cb.range[2] = segBase + triF4;		// casterBase (float4 elems) = right after the tris
	cb.range[3] = light->index & 0x1FFF;
	cb.caps[0] = m_TableCap;
	cb.caps[1] = m_PoolCap;
	cb.caps[2] = SW_WARM_BUDGET;		// build dispatch bound (queue mode processes queue[0..budget))
	// REDUCED-SET mode: signal via a NEGATIVE caps[3] (magnitude = the top-K cap) and pass the solo cutoff
	// in the errTol slot params[3] (the fold errTol gate is skipped in reduced mode). No new cbuffer field.
	extern idCVar r_softShadowSurfCacheReduced, r_softShadowSurfCacheReducedK, r_softShadowSurfCacheReducedCutoff;
	const bool reducedMode = r_softShadowSurfCacheReduced.GetBool();
	cb.caps[3] = reducedMode ? -Max( 1, r_softShadowSurfCacheReducedK.GetInteger() ) : SW_SURF_MAX_RESIDUAL;
	cb.params[0] = m_Texel;
	cb.params[1] = m_Thr;
	cb.params[2] = ( float )m_QueueWords;
	cb.params[3] = reducedMode ? r_softShadowSurfCacheReducedCutoff.GetFloat() : r_softShadowSurfCacheErrTol.GetFloat();
	cb.seed[0] = 0;						// QUEUE mode (consume the seed's enqueued slots)
	cb.seed[1] = 0;
	cb.seed[2] = nRecv;					// seed rasterises RECEIVER tris (one thread per tri)
	cb.seed[3] = ( int )GetLightGeneration( light->index );

	// SEED: one thread per static RECEIVER tri claims + ENQUEUES this light's receiver texels
	if( m_SeedPipeline != nullptr && nRecv > 0 && m_WarmRecvStream != nullptr )
	{
		nvrhi::BindingSetDesc ss;
		ss.bindings =
		{
			nvrhi::BindingSetItem::ConstantBuffer( 0, m_ConstantBuffer ),
			nvrhi::BindingSetItem::StructuredBuffer_SRV( 0, m_WarmRecvStream ),
			nvrhi::BindingSetItem::StructuredBuffer_UAV( 0, m_Table ),
			nvrhi::BindingSetItem::StructuredBuffer_UAV( 1, m_Queue ),
		};
		nvrhi::BindingSetHandle seedSet = m_Device->createBindingSet( ss, m_SeedLayout );
		// SEED CB version: g_range.x is the SEED's tri base into the RECV stream, whose data is always
		// at offset 0 - NOT the build's persistent-stream segment base. The finding-#4 merge left
		// segBase in range[0] here, so every light past the first rasterised garbage float4s from
		// [segBase..) and claimed near-nothing under garbage keys (2026-08-26 0%-hit regression: claims
		// collapsed 2.5M -> 317k, all unmatchable). The build windows below restore range[0] = segBase.
		cb.range[0] = 0;
		commandList->writeBuffer( m_ConstantBuffer, &cb, sizeof( cb ) );
		nvrhi::ComputeState scs;
		scs.pipeline = m_SeedPipeline;
		scs.bindings = { seedSet };
		commandList->setComputeState( scs );
		commandList->dispatch( ( nRecv + 63 ) / 64, 1, 1 );
	}

	// BUILD: consume the enqueued texels (bounded), filling F (scalar) or the bit-grid (grid mode) for each.
	// GRID mode routes to the SW_SURF_GRID pipeline/layout and binds the parallel grid buffer as UAV u3.
	extern idCVar r_softShadowSurfCacheGrid;
	const bool gridMode = r_softShadowSurfCacheGrid.GetBool() && swGrid().pipeline != nullptr && swGrid().buffer != nullptr;
	nvrhi::BindingSetDesc sd;
	sd.bindings =
	{
		nvrhi::BindingSetItem::ConstantBuffer( 0, m_ConstantBuffer ),
		nvrhi::BindingSetItem::StructuredBuffer_SRV( 0, swStream().buffer ),	// persistent stream (build walks this light's segment)
		nvrhi::BindingSetItem::StructuredBuffer_SRV( 1, swStream().buffer ),	// dummy t1 (include requirement)
		nvrhi::BindingSetItem::StructuredBuffer_UAV( 0, m_Table ),
		nvrhi::BindingSetItem::StructuredBuffer_UAV( 1, gridMode ? swGrid().buffer.Get() : m_Pool.Get() ),	// u1 : pool (scalar) or grid (grid mode)
		nvrhi::BindingSetItem::StructuredBuffer_UAV( 2, m_Queue ),
	};
	nvrhi::BindingSetHandle set = m_Device->createBindingSet( sd, m_Layout );
	// WINDOWED build (task #87): fullDrain loops bounded windows over the whole queue - each dispatch
	// stays at the proven-safe SW_WARM_BUDGET bound, empty tail windows retire on the count check.
	// Single-window (the old behavior) remains the cheap runtime default above the emergency line;
	// the unbuilt tail is PARKED below (audit #7b) so later frames keep draining it.
	const int swTotalWindows = ( m_QueueWords - 1 + SW_WARM_BUDGET - 1 ) / SW_WARM_BUDGET;
	const int swWindows = fullDrain ? swTotalWindows : 1;
	nvrhi::ComputeState cs;
	cs.pipeline = gridMode ? swGrid().pipeline : m_Pipeline;
	cs.bindings = { set };
	// TIER-2 ECONOMICS (task #112): the BUILD windows repurpose g_seed.z as the min-ROI threshold
	// (the seed dispatch above already consumed its own CB version with seed[2] = nRecv). Without
	// this overwrite the build would read nRecv as the threshold and free nearly every texel.
	{
		extern idCVar r_softShadowSurfCacheMinROI;
		cb.seed[2] = r_softShadowSurfCacheMinROI.GetInteger();
	}
	cb.range[0] = segBase;		// restore the BUILD's persistent-stream segment tri base (the seed CB version above zeroed it)
	for( int wb = 0; wb < swWindows; wb++ )
	{
		cb.seed[1] = wb * SW_WARM_BUDGET;	// queue window base (build CS: qi = g_seed.y + i)
		commandList->writeBuffer( m_ConstantBuffer, &cb, sizeof( cb ) );
		commandList->setComputeState( cs );
		commandList->dispatch( ( SW_WARM_BUDGET + 63 ) / 64, 1, 1 );
	}

	st.seedPending = false;
	if( swWindows < swTotalWindows )
	{
		// PART-WARM (audit #7b): windows remain unbuilt - park the drain (CB + pipeline choice) and
		// keep needSweep pending so the warm queue continues it one window per frame until every
		// seeded texel is built. The old unconditional clear here declared the light warm after ONE
		// window, stranding the queue tail REQUESTED forever (re-warm could never recover it: a
		// same-generation re-seed enqueues nothing).
		s_swDrain.light = light->index;
		s_swDrain.window = swWindows;
		s_swDrain.grid = gridMode;
		s_swDrain.cb = cb;
		st.needSweep = true;
		EnqueueWarm( light );
	}
	else
	{
		st.needSweep = false;	// warmed: every seeded texel built
	}
	return true;
}

// (re)create the reused warm-stream buffer when a light's stream needs more room than it holds
void SoftShadowSurfCache::EnsureWarmStream( int float4Count )
{
	if( float4Count <= 0 || ( m_WarmStream != nullptr && m_WarmStreamF4 >= float4Count ) )
	{
		return;
	}
	int want = m_WarmStreamF4 > 0 ? m_WarmStreamF4 : 4096;
	while( want < float4Count )
	{
		want *= 2;
	}
	nvrhi::BufferDesc bd;
	bd.byteSize = ( uint64_t )want * sizeof( idVec4 );
	bd.structStride = sizeof( idVec4 );		// float4 stride (matches t_SoftEdges StructuredBuffer<float4>)
	bd.canHaveUAVs = false;
	bd.initialState = nvrhi::ResourceStates::ShaderResource;
	bd.keepInitialState = true;
	bd.debugName = "SoftShadowSurfCache/WarmStream";
	m_WarmStream = m_Device->createBuffer( bd );
	m_WarmStreamF4 = want;
}

// (re)create the reused receiver-tri seed buffer when a light's receiver stream needs more room
void SoftShadowSurfCache::EnsureRecvStream( int float4Count )
{
	if( float4Count <= 0 || ( m_WarmRecvStream != nullptr && m_WarmRecvStreamF4 >= float4Count ) )
	{
		return;
	}
	int want = m_WarmRecvStreamF4 > 0 ? m_WarmRecvStreamF4 : 4096;
	while( want < float4Count )
	{
		want *= 2;
	}
	nvrhi::BufferDesc bd;
	bd.byteSize = ( uint64_t )want * sizeof( idVec4 );
	bd.structStride = sizeof( idVec4 );		// float4 stride (matches t_SoftEdges StructuredBuffer<float4>)
	bd.canHaveUAVs = false;
	bd.initialState = nvrhi::ResourceStates::ShaderResource;
	bd.keepInitialState = true;
	bd.debugName = "SoftShadowSurfCache/WarmRecvStream";
	m_WarmRecvStream = m_Device->createBuffer( bd );
	m_WarmRecvStreamF4 = want;
}

bool SoftShadowSurfCache::GetStats( uint32_t out[4] )
{
	if( m_Table == nullptr || m_TableCap <= 0 )
	{
		return false;
	}
	nvrhi::BufferDesc sbd;
	sbd.byteSize = 4 * sizeof( uint32_t );
	sbd.cpuAccess = nvrhi::CpuAccessMode::Read;
	sbd.debugName = "SoftShadowSurfCache/StatsReadback";
	nvrhi::BufferHandle staging = m_Device->createBuffer( sbd );
	nvrhi::CommandListHandle cl = m_Device->createCommandList();
	cl->open();
	cl->copyBuffer( staging, 0, m_Table, ( uint64_t )m_TableCap * 8 * sizeof( uint32_t ), 4 * sizeof( uint32_t ) );
	cl->close();
	m_Device->executeCommandList( cl );
	m_Device->waitForIdle();
	void* p = m_Device->mapBuffer( staging, nvrhi::CpuAccessMode::Read );
	if( p == nullptr )
	{
		return false;
	}
	memcpy( out, p, 4 * sizeof( uint32_t ) );
	m_Device->unmapBuffer( staging );
	return true;
}

// blocking readback of the 4 MISS sub-reason counters (0 stale-gen, 1 requested-unbuilt, 2 empty-slot,
// 3 probe-overflow) that ride the 4 words after the class counters (base = tableCap*8 + 4). Diagnostic:
// splits WHERE the miss% comes from - a warm/key defect (empty/overflow) vs a build-budget lag (requested)
// vs invalidation churn (stale-gen). LAST rendered frame's sample.
bool SoftShadowSurfCache::GetMissReasons( uint32_t out[4] )
{
	if( m_Table == nullptr || m_TableCap <= 0 )
	{
		return false;
	}
	nvrhi::BufferDesc sbd;
	sbd.byteSize = 4 * sizeof( uint32_t );
	sbd.cpuAccess = nvrhi::CpuAccessMode::Read;
	sbd.debugName = "SoftShadowSurfCache/MissReasonReadback";
	nvrhi::BufferHandle staging = m_Device->createBuffer( sbd );
	nvrhi::CommandListHandle cl = m_Device->createCommandList();
	cl->open();
	cl->copyBuffer( staging, 0, m_Table, ( uint64_t )( m_TableCap * 8 + 4 ) * sizeof( uint32_t ), 4 * sizeof( uint32_t ) );
	cl->close();
	m_Device->executeCommandList( cl );
	m_Device->waitForIdle();
	void* p = m_Device->mapBuffer( staging, nvrhi::CpuAccessMode::Read );
	if( p == nullptr )
	{
		return false;
	}
	memcpy( out, p, 4 * sizeof( uint32_t ) );
	m_Device->unmapBuffer( staging );
	return true;
}

// blocking readback of the 16 per-light {hit,miss} ring pairs (base = tableCap*8 + 8, ring = lightKey&15).
// Bench attribution: names WHICH lights own the miss bucket - an all-or-nothing light (never seeded, 0% hit)
// reads as a ring with hits 0 / misses large. LAST rendered frame's sample.
bool SoftShadowSurfCache::GetPerLightStats( uint32_t out[32] )
{
	if( m_Table == nullptr || m_TableCap <= 0 )
	{
		return false;
	}
	nvrhi::BufferDesc sbd;
	sbd.byteSize = 32 * sizeof( uint32_t );
	sbd.cpuAccess = nvrhi::CpuAccessMode::Read;
	sbd.debugName = "SoftShadowSurfCache/PerLightReadback";
	nvrhi::BufferHandle staging = m_Device->createBuffer( sbd );
	nvrhi::CommandListHandle cl = m_Device->createCommandList();
	cl->open();
	cl->copyBuffer( staging, 0, m_Table, ( uint64_t )( m_TableCap * 8 + 8 ) * sizeof( uint32_t ), 32 * sizeof( uint32_t ) );
	cl->close();
	m_Device->executeCommandList( cl );
	m_Device->waitForIdle();
	void* p = m_Device->mapBuffer( staging, nvrhi::CpuAccessMode::Read );
	if( p == nullptr )
	{
		return false;
	}
	memcpy( out, p, 32 * sizeof( uint32_t ) );
	m_Device->unmapBuffer( staging );
	return true;
}

// TRUSTWORTHY table census (blocking full scan). The per-frame path counters read via GetHudStats proved
// unreliable (a GPU-atomic mislabel: idx-1 misses landed in the walk-always word), so for the frame-probe
// summary read the ACTUAL slot states instead - built/requested/empty is exact and shows table FILL, the
// oversubscription that overflows the term's 16-slot probe and starves the hit rate.
bool SoftShadowSurfCache::GetTableCensus( uint32_t out[4] )
{
	out[0] = out[1] = out[2] = out[3] = 0;
	if( m_Table == nullptr || m_TableCap <= 0 )
	{
		return false;
	}
	const uint64_t words = ( uint64_t )m_TableCap * 8;
	nvrhi::BufferDesc sbd;
	sbd.byteSize = words * sizeof( uint32_t );
	sbd.cpuAccess = nvrhi::CpuAccessMode::Read;
	sbd.debugName = "SoftShadowSurfCache/CensusReadback";
	nvrhi::BufferHandle staging = m_Device->createBuffer( sbd );
	nvrhi::CommandListHandle cl = m_Device->createCommandList();
	cl->open();
	cl->copyBuffer( staging, 0, m_Table, 0, words * sizeof( uint32_t ) );
	cl->close();
	m_Device->executeCommandList( cl );
	m_Device->waitForIdle();
	const uint32_t* t = ( const uint32_t* )m_Device->mapBuffer( staging, nvrhi::CpuAccessMode::Read );
	if( t == nullptr )
	{
		return false;
	}
	for( uint64_t s = 0; s < ( uint64_t )m_TableCap; s++ )
	{
		const uint32_t w0 = t[s * 8 + 0];
		if( w0 == 0xFFFFFFFFu )
		{
			out[2]++;    // empty
			continue;
		}
		if( w0 == 0xFFFFFFFEu )
		{
			out[3]++;    // TOMBSTONE (build-freed, chain-preserving): its stale state word must not read as requested/built
			continue;
		}
		const uint32_t code = t[s * 8 + 2] & 3u;
		if( code == 2u )
		{
			out[0]++;    // built
		}
		else if( code == 1u )
		{
			out[1]++;    // requested-unbuilt
		}
		else
		{
			out[3]++;    // other (walk-always / partial)
		}
	}
	m_Device->unmapBuffer( staging );
	return true;
}

// MEASUREMENT one-shot (r_softShadowSurfCacheDump, called at the end of WarmMapBurst): read the whole
// warmed table back to the CPU and print the per-texel SAVING distribution - the answer to "how much
// would be saved if all that can be cached were cached, and where". Per BUILT slot the saving is word 4
// hi = foldedCount (static occluders removed from the runtime walk); residual = word 4 lo (still walked).
// Attributes Sum(foldedCount) per light (owning light key = word 1 bits 19..31) and cross-references each
// light's static caster/tri counts (m_WarmEntries) so a light warmed for little return (many casters,
// low folded) is visible. Blocking waitForIdle - fine for a one-shot offline dump.
void SoftShadowSurfCache::DumpTableHistogram()
{
	if( m_Table == nullptr || m_TableCap <= 0 )
	{
		return;
	}
	const uint64_t words = ( uint64_t )m_TableCap * 8;
	nvrhi::BufferDesc sbd;
	sbd.byteSize = words * sizeof( uint32_t );
	sbd.cpuAccess = nvrhi::CpuAccessMode::Read;
	sbd.debugName = "SoftShadowSurfCache/TableDump";
	nvrhi::BufferHandle staging = m_Device->createBuffer( sbd );
	nvrhi::CommandListHandle cl = m_Device->createCommandList();
	cl->open();
	cl->copyBuffer( staging, 0, m_Table, 0, words * sizeof( uint32_t ) );
	cl->close();
	m_Device->executeCommandList( cl );
	m_Device->waitForIdle();
	const uint32_t* t = ( const uint32_t* )m_Device->mapBuffer( staging, nvrhi::CpuAccessMode::Read );
	if( t == nullptr )
	{
		return;
	}

	// per-light accumulation keyed by the 13-bit light key stored in the slot
	struct lightAgg_t { uint64_t fold = 0; int built = 0; int walk = 0; uint32_t maxFold = 0; };
	std::unordered_map<uint32_t, lightAgg_t> perLight;
	// foldedCount histogram buckets: 0, 1-2, 3-4, 5-8, 9-16, 17-32, 33-64, 65+
	const uint32_t bEdge[8] = { 0, 2, 4, 8, 16, 32, 64, 0xFFFFFFFFu };
	uint64_t hist[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
	uint64_t builtSlots = 0, walkSlots = 0, reqSlots = 0, emptySlots = 0;
	uint64_t totalFold = 0, totalRes = 0;
	uint32_t maxFold = 0, maxFoldKeyLo = 0, maxFoldKeyHi = 0;
	for( uint64_t s = 0; s < ( uint64_t )m_TableCap; s++ )
	{
		const uint32_t w0 = t[s * 8 + 0];
		if( w0 == 0xFFFFFFFFu )
		{
			emptySlots++;
			continue;
		}
		const uint32_t w1 = t[s * 8 + 1];
		const uint32_t code = t[s * 8 + 2] & 3u;
		const uint32_t key = ( w1 >> 19 ) & 0x1FFFu;
		if( code == 2u )		// BUILT
		{
			builtSlots++;
			const uint32_t w4 = t[s * 8 + 4];
			const uint32_t folded = w4 >> 16;
			totalFold += folded;
			totalRes  += ( w4 & 0xFFFFu );
			int bk = 0;
			while( bk < 7 && folded > bEdge[bk] ) { bk++; }
			hist[bk]++;
			lightAgg_t& a = perLight[key];
			a.fold += folded; a.built++;
			if( folded > a.maxFold ) { a.maxFold = folded; }
			if( folded > maxFold ) { maxFold = folded; maxFoldKeyLo = w0; maxFoldKeyHi = w1; }
		}
		else if( code == 3u )
		{
			walkSlots++;
			perLight[key].walk++;
		}
		else
		{
			reqSlots++;		// code 1 = requested-but-unbuilt (seeded, never completed)
		}
	}
	m_Device->unmapBuffer( staging );

	// index&0x1FFF -> static caster/tri counts (usefulness vs warm cost)
	std::unordered_map<uint32_t, std::pair<int, int>> keyCost;
	for( const auto& kv : m_WarmEntries )
	{
		keyCost[( uint32_t )( kv.first & 0x1FFF )] = { kv.second.nCas, kv.second.nTris };
	}

	common->Printf( "[softdump] table %d slots: built %llu, walk-always %llu, requested-unbuilt %llu, empty %llu\n",
					m_TableCap, ( unsigned long long )builtSlots, ( unsigned long long )walkSlots,
					( unsigned long long )reqSlots, ( unsigned long long )emptySlots );
	const double meanFold = builtSlots ? ( double )totalFold / ( double )builtSlots : 0.0;
	common->Printf( "[softdump] SAVING: Sum folded = %llu walk-units over %llu built texels (mean %.2f folded/texel, residual still walked %llu)\n",
					( unsigned long long )totalFold, ( unsigned long long )builtSlots, meanFold, ( unsigned long long )totalRes );
	// decode the biggest-saving texel's world cell (mirror softterm.cs.hlsl key derivation)
	{
		const int cu = ( int )( maxFoldKeyLo & 0xFFFFu ) - 32768;
		const int cv = ( int )( maxFoldKeyLo >> 16 ) - 32768;
		const int cw = ( int )( maxFoldKeyHi & 0xFFFFu ) - 32768;
		const uint32_t axis = ( maxFoldKeyHi >> 16 ) & 7u;
		const uint32_t lk = ( maxFoldKeyHi >> 19 ) & 0x1FFFu;
		common->Printf( "[softdump] biggest saving = %u folded @ cell(%d,%d,%d) axis %u light %u\n",
						maxFold, cu, cv, cw, axis, lk );
	}
	common->Printf( "[softdump] folded histogram: [0]=%llu [1-2]=%llu [3-4]=%llu [5-8]=%llu [9-16]=%llu [17-32]=%llu [33-64]=%llu [65+]=%llu\n",
					( unsigned long long )hist[0], ( unsigned long long )hist[1], ( unsigned long long )hist[2], ( unsigned long long )hist[3],
					( unsigned long long )hist[4], ( unsigned long long )hist[5], ( unsigned long long )hist[6], ( unsigned long long )hist[7] );

	// rank lights by Sum folded (warm usefulness); print the top 20 with their static caster/tri cost
	std::vector<std::pair<uint32_t, lightAgg_t>> ranked( perLight.begin(), perLight.end() );
	std::sort( ranked.begin(), ranked.end(), []( const std::pair<uint32_t, lightAgg_t>& a, const std::pair<uint32_t, lightAgg_t>& b )
	{
		return a.second.fold > b.second.fold;
	} );
	common->Printf( "[softdump] %llu lights hold built texels. TOP by Sum-folded (usefulness) | key: builtTexels SumFold meanFold walkAlways | nCas nTris:\n",
					( unsigned long long )ranked.size() );
	const int topN = ( int )( ranked.size() < 20 ? ranked.size() : 20 );
	for( int r = 0; r < topN; r++ )
	{
		const uint32_t key = ranked[r].first;
		const lightAgg_t& a = ranked[r].second;
		const double mf = a.built ? ( double )a.fold / ( double )a.built : 0.0;
		auto ci = keyCost.find( key );
		const int nc = ( ci != keyCost.end() ) ? ci->second.first : -1;
		const int nt = ( ci != keyCost.end() ) ? ci->second.second : -1;
		common->Printf( "[softdump]   light %5u: %6d texels  fold %8llu  mean %6.2f  walk %6d | nCas %5d nTris %6d\n",
						key, a.built, ( unsigned long long )a.fold, mf, a.walk, nc, nt );
	}
}

void SoftShadowSurfCache::EndBuilds( nvrhi::ICommandList* commandList )
{
	if( !m_Active )
	{
		return;
	}
	// NO per-frame queue clear (audit #9a): the runtime term is READ-ONLY - softterm's claim/enqueue
	// path is dead (prev = 0u) and it does not even bind u_SurfQueue - and GetQueue() has no callers,
	// so the only queue writers are this file's seed/build warms and the GC wipe, each of which does
	// its own clear. The multi-MB clearBufferUInt here every frame was pure dead work; persisting the
	// queue across frames is also what lets a parked part-warm drain (audit #7b) keep consuming it.
	// HUD readback (non-blocking): snapshot THIS frame's counters into the ring, then map back the
	// OLDEST ring entry - copied SW_STATS_RING-1 frames ago, so the GPU is long done and mapBuffer
	// never stalls the pipe (unlike GetStats, which waitForIdle's). Feeds com_showFPS's cache line.
	const uint64_t swCounterOff = ( uint64_t )m_TableCap * 8 * sizeof( uint32_t );
	if( m_StatsRing[0] != nullptr )
	{
		commandList->copyBuffer( m_StatsRing[m_StatsRingWrite], 0, m_Table, swCounterOff, 4 * sizeof( uint32_t ) );
		const int oldest = ( m_StatsRingWrite + 1 ) % SW_STATS_RING;
		// only read a ring slot once it has actually been WRITTEN (a full ring's worth of frames since the
		// last wipe); before that the staging buffer is uninitialised and read back as garbage (0xFFFFFFFF).
		if( m_StatsRingFilled >= SW_STATS_RING )
		{
			void* p = m_Device->mapBuffer( m_StatsRing[oldest], nvrhi::CpuAccessMode::Read );
			if( p != nullptr )
			{
				memcpy( m_HudStats, p, 4 * sizeof( uint32_t ) );
				m_Device->unmapBuffer( m_StatsRing[oldest] );
			}
		}
		m_StatsRingWrite = ( m_StatsRingWrite + 1 ) % SW_STATS_RING;
		if( m_StatsRingFilled < SW_STATS_RING + 1 ) { m_StatsRingFilled++; }
	}
	// reset the per-frame path counters (the 4 words after the table) so each frame's term
	// dispatches accumulate a fresh hit/miss/walkalways/anchor-reject sample
	const uint32_t swZero[8 + 32] = { 0 };	// 4 class counters + 4 miss sub-reason counters + 32 per-light {hit,miss} pairs
	commandList->writeBuffer( m_Table, swZero, sizeof( swZero ), swCounterOff );

	// prewarm sweep bookkeeping: a fresh seed (or an in-view light that missed the last sweep)
	// restarts the table scan from slot 0 next frame; otherwise the cursor advances one budget
	// window per frame - but only when at least one light actually dispatched over the window
	// (no dispatch = window not scanned = must not be skipped). On wrap, the lights that were
	// dispatched this frame are marked swept (approximation: a light that entered mid-sweep and
	// still has REQUESTED slots re-triggers a restart via needSweep next time it goes idle).
	if( m_SeededThisFrame || m_WantRestart )
	{
		m_ScanCursor = 0;
	}
	else if( m_ScanCursor < m_TableCap && !m_DispatchedLights.empty() )
	{
		m_ScanCursor += m_ActiveBudget;
		if( m_ScanCursor >= m_TableCap )
		{
			for( int li : m_DispatchedLights )
			{
				auto it = m_LightHash.find( li );
				if( it != m_LightHash.end() && !it->second.seedPending )
				{
					it->second.needSweep = false;
				}
			}
		}
	}
}

// GAME/LOAD thread: cheap dedup-push of a light index. No cache/GPU state touched here.
void SoftShadowSurfCache::EnqueueWarm( const idRenderLightLocal* light )
{
	if( light == NULL )
	{
		return;
	}
	std::lock_guard<std::mutex> lock( m_WarmMutex );
	m_WarmWorld = light->world;		// queued indices resolve against this world at drain time
	const int idx = light->index;
	for( size_t i = 0; i < m_WarmQueue.size(); i++ )
	{
		if( m_WarmQueue[i] == idx )
		{
			return;
		}
	}
	m_WarmQueue.push_back( idx );
}

// GAME thread: geometry under this light changed (caster spawned/freed/moved, or the light moved).
// Just record the index; the render thread bumps the generation and rebuilds during the drain.
void SoftShadowSurfCache::InvalidateLight( const idRenderLightLocal* light )
{
	if( light == NULL )
	{
		return;
	}
	std::lock_guard<std::mutex> lock( m_WarmMutex );
	m_WarmWorld = light->world;
	const int idx = light->index;
	for( size_t i = 0; i < m_InvalidateQueue.size(); i++ )
	{
		if( m_InvalidateQueue[i] == idx )
		{
			return;
		}
	}
	m_InvalidateQueue.push_back( idx );
}

// RENDER thread (GL_StartFrame, before the view): drain the queues and build camera-independently.
// A partially drained queue just leaves those lights briefly uncached (miss = exact walk, no spike).
void SoftShadowSurfCache::DrainWarmQueue( nvrhi::ICommandList* commandList, int maxLights )
{
	extern idCVar r_softShadowSurfCache;
	std::vector<int> warm, inval;
	idRenderWorldLocal* world = nullptr;
	{
		std::lock_guard<std::mutex> lock( m_WarmMutex );
		if( !r_softShadowSurfCache.GetBool() )
		{
			m_WarmQueue.clear();
			m_InvalidateQueue.clear();
			return;
		}
		warm.swap( m_WarmQueue );
		inval.swap( m_InvalidateQueue );
		world = m_WarmWorld;
	}
	// EVENT-DRIVEN runtime drain: the whole map is warmed ONCE at load by WarmMapBurst (behind the load
	// fade), so this per-frame path only re-warms lights whose STATIC geometry actually changed (the
	// invalidate queue) plus any explicit enqueues. It used to also cheap-skip-scan the ENTIRE map every
	// frame - but R_LightStaticChainSig walks each light's whole interaction chain with a per-surface
	// shadow-cast + light-bounds test (world-model lights = thousands of surfaces), 48 lights/frame,
	// forever: that scan was the dominant runtime cost (measured 37->12 fps in the erebus1 cutscene) and
	// is fully redundant after the load burst. Nothing queued => nothing to do.
	if( world == NULL )
	{
		world = tr.primaryWorld;
	}
	if( world == NULL || ( warm.empty() && inval.empty() ) || !EnsureResources() )
	{
		return;
	}
	DoClearIfNeeded( commandList );

	// invalidated lights: bump generation FIRST (orphans stale slots even if a moved light produces no
	// new static stream), then fall through to warm. Merge into the warm list (invalidations first).
	for( size_t i = 0; i < inval.size(); i++ )
	{
		auto it = m_LightHash.find( inval[i] );
		if( it != m_LightHash.end() && it->second.hash != 0 )
		{
			it->second.generation++;
			it->second.seedPending = true;
			it->second.needSweep = true;
			m_GenBumps++;
		}
		warm.push_back( inval[i] );
	}

	( void )maxLights;
	// Build up to r_softShadowSurfCacheWarmLightsPerFrame lights per frame (default 1). Each WarmLight is a
	// bounded GPU dispatch; more than one per frame historically risked the GPU watchdog on many-caster
	// lights, but now that the term/permutation is cheap this budget can be flexible - a higher value clears
	// the post-invalidation / post-GC re-warm backlog (149 lights, otherwise one/frame = 149 frames of
	// stale misses) far faster, which is where the cinematic mean hit rate is dragged down. Explicit queue
	// first, then a bounded cheap-skip scan of the whole map. WarmLight returns true only when it actually
	// dispatched a (re)build.
	extern idCVar r_softShadowSurfCacheWarmLightsPerFrame;
	const int swMaxWarm = idMath::ClampInt( 1, 64, r_softShadowSurfCacheWarmLightsPerFrame.GetInteger() );
	// EXPENSIVE-FIRST (task #87 / #83): under the per-frame budget, re-warm the heaviest queued lights
	// first (same interaction-chain-length proxy as the load burst) - queue arrival order was blind to
	// cost, so a cheap mover could starve a heavy room light's re-warm for many frames.
	if( warm.size() > 1 )
	{
		std::sort( warm.begin(), warm.end(), [world]( int a, int b )
		{
			auto chainLen = [world]( int li ) -> int
			{
				if( li < 0 || li >= world->lightDefs.Num() || world->lightDefs[li] == NULL )
				{
					return -1;
				}
				int c = 0;
				for( const idInteraction* inter = world->lightDefs[li]->firstInteraction; inter != NULL; inter = inter->lightNext )
				{
					c++;
				}
				return c;
			};
			return chainLen( a ) > chainLen( b );
		} );
	}
	int builtCount = 0;
	size_t qi = 0;
	for( ; qi < warm.size() && builtCount < swMaxWarm; qi++ )
	{
		const int idx = warm[qi];
		if( idx >= 0 && idx < world->lightDefs.Num() )
		{
			const idRenderLightLocal* light = world->lightDefs[idx];
			if( light != NULL && WarmLight( commandList, light ) )
			{
				builtCount++;
				s_runtimeBuilds++;
			}
		}
	}
	// re-queue explicit-queue entries not reached this frame (they get a turn next frame)
	if( qi < warm.size() )
	{
		std::lock_guard<std::mutex> lock( m_WarmMutex );
		for( ; qi < warm.size(); qi++ )
		{
			m_WarmQueue.push_back( warm[qi] );
		}
	}
}

// RENDER thread: fire once per new world (pointer identity). The backend calls this on the first
// soft-frame of a map; a true return means "burst the whole map warm now, behind the load fade".
bool SoftShadowSurfCache::TakeLoadWarm( idRenderWorldLocal* world )
{
	extern idCVar r_softShadowSurfCache;
	if( world == NULL || !r_softShadowSurfCache.GetBool() || world == m_LastBurstWorld )
	{
		return false;
	}
	// ponytail: pointer-identity new-map signal. A world address reused after FreeRenderWorld would
	// skip the burst and fall back to the one-light-per-frame scan (correct, just not pre-warmed).
	m_LastBurstWorld = world;
	return true;
}

// RENDER thread, called with NO immediate command list open (the backend closes its frame list first).
// Warm every light of the world in this single load-transition frame, one light per submit+wait so no
// single bounded dispatch can trip the GPU watchdog. Heavy by design - it replaces the ~40 visible
// startup spikes (one light per gameplay frame) with one slow frame the player never sees moving.
void SoftShadowSurfCache::WarmMapBurst( nvrhi::IDevice* device, idRenderWorldLocal* world )
{
	if( device == NULL || world == NULL || !EnsureResources() )
	{
		return;
	}
	{
		// fresh world: the previous world's CPU-side state is ALL stale (harness-audit findings #3 +
		// use-after-free): m_LightHash keys are old light indices (their re-warm queue entries resolve
		// NULL forever and inflate "pending"; index collisions with the new world's lights bump
		// generations spuriously), m_WarmEntries streams belong to freed lights, and queued indices
		// dereference m_WarmWorld at drain time - clear everything under the lock, then adopt.
		std::lock_guard<std::mutex> lock( m_WarmMutex );
		m_WarmWorld = world;
		m_WarmQueue.clear();
		m_InvalidateQueue.clear();
		m_LightHash.clear();
		m_WarmEntries.clear();
		m_GenBumps = 0;
	}
	// fresh map: force a clear so stale slots from the previous world cannot alias into a hit.
	m_NeedClear = true;
	nvrhi::CommandListHandle cl = device->createCommandList();
	cl->open();
	DoClearIfNeeded( cl );
	cl->close();
	device->executeCommandList( cl );
	device->waitForIdle();

	s_warmRecvTotal = 0;
	s_warmCasterTotal = 0;
	s_warmStreamedLights = 0;
	{
		extern void R_SoftWarmDiagReset();
		R_SoftWarmDiagReset();		// one-shot warm-miss attribution (task #87 "instrument the misses")
	}
	const int n = world->lightDefs.Num();
	int warmed = 0;
	// drive the LOADING BAR: this burst runs from ExecuteMapChange (behind the load screen), so report
	// its progress like any other load step. Increment per light advances the bar AND renders the load
	// GUI (LoadPacifierProgressIncrement -> UpdateLevelLoadPacifier), which self-guards to a no-op when
	// not inside a map change (the view-path fallback), so this is safe from either caller.
	common->LoadPacifierProgressTotal( n );
	// EXPENSIVE-FIRST (task #87 / #83): warm the heaviest lights first so a truncated or interrupted
	// burst spends its time where the runtime walk is dearest. Cost proxy = interaction-chain length
	// (cheap: pointer walk, no per-surface tests); map-parse index order was blind to cost.
	std::vector<std::pair<int, int>> warmOrder;	// ( -chainLen, lightIndex ): sort ascending = heaviest first
	warmOrder.reserve( n );
	for( int oi = 0; oi < n; oi++ )
	{
		const idRenderLightLocal* ol = world->lightDefs[oi];
		if( ol == NULL )
		{
			continue;
		}
		int chainLen = 0;
		for( const idInteraction* inter = ol->firstInteraction; inter != NULL; inter = inter->lightNext )
		{
			chainLen++;
		}
		warmOrder.emplace_back( -chainLen, oi );
	}
	std::sort( warmOrder.begin(), warmOrder.end() );
	for( size_t wi = 0; wi < warmOrder.size(); wi++ )
	{
		const int i = warmOrder[wi].second;
		common->LoadPacifierProgressIncrement( 1 );
		const idRenderLightLocal* light = world->lightDefs[i];
		if( light == NULL )
		{
			continue;
		}
		cl->open();
		const bool built = WarmLight( cl, light, true );	// load burst: FULL drain (partial warm = permanent miss)
		cl->close();
		device->executeCommandList( cl );
		device->waitForIdle();		// one bounded light per submit -> watchdog-safe
		if( built )
		{
			warmed++;
		}
	}
	common->Printf( "[softsurf] warm-at-load: %d of %d lights warmed | %d recv-tris, %d caster-tris over %d streamed lights\n",
					warmed, n, s_warmRecvTotal, s_warmCasterTotal, s_warmStreamedLights );
	{
		extern void R_SoftWarmDiagPrint();
		R_SoftWarmDiagPrint();		// one-shot: where the collector rejected everything (task #87)
	}
	extern idCVar r_softShadowSurfCacheDump;
	if( r_softShadowSurfCacheDump.GetBool() )
	{
		DumpTableHistogram();
	}
}
