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
#include "renderer/RenderWorld_local.h"		// idRenderWorldLocal::lightDefs (warm-queue resolution)
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

SoftShadowSurfCache::SoftShadowSurfCache( nvrhi::IDevice* device )
	: m_Device( device )
{
}

void SoftShadowSurfCache::EnsurePipeline()
{
	if( m_PipelineTried )
	{
		return;
	}
	m_PipelineTried = true;

	idList<shaderMacro_t> macros;
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
	cb.maxVersions = 1024;
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
	const int wantQueue = Max( budget + 1, warmBudget + 1 );	// per-light queue holds one light's claimed texels
	const float texel = ( float )r_softShadowSurfCacheTexel.GetInteger();
	const float thr = r_softShadowSurfCacheSecondThr.GetFloat();
	const float errTol = r_softShadowSurfCacheErrTol.GetFloat();
	if( m_Table == nullptr || capP2 != m_TableCap || poolCap != m_PoolCap || budget != m_Budget
			|| wantQueue != m_QueueWords || texel != m_Texel || thr != m_Thr || errTol != m_ErrTol )
	{
		m_TableCap = capP2;
		m_PoolCap = poolCap;
		m_Budget = budget;
		m_QueueWords = wantQueue;	// must hold the warm build's per-light SW_WARM_BUDGET enqueued texels
		m_Texel = texel;
		m_Thr = thr;
		m_ErrTol = errTol;		// build semantics changed: drop + reseed

		nvrhi::BufferDesc bd;
		bd.structStride = sizeof( uint32_t );
		bd.canHaveUAVs = true;
		bd.initialState = nvrhi::ResourceStates::UnorderedAccess;
		bd.keepInitialState = true;

		bd.byteSize = ( ( uint64_t )m_TableCap * 8 + 4 ) * sizeof( uint32_t );	// +4: per-frame path counters
		bd.debugName = "SoftShadowSurfCache/Table";
		m_Table = m_Device->createBuffer( bd );

		bd.byteSize = ( uint64_t )m_PoolCap * sizeof( uint32_t );
		bd.debugName = "SoftShadowSurfCache/Pool";
		m_Pool = m_Device->createBuffer( bd );

		bd.byteSize = ( uint64_t )m_QueueWords * sizeof( uint32_t );
		bd.debugName = "SoftShadowSurfCache/Queue";
		m_Queue = m_Device->createBuffer( bd );

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
	commandList->clearBufferUInt( m_Pool, 0 );
	commandList->clearBufferUInt( m_Queue, 0 );
	m_NeedClear = false;
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

bool SoftShadowSurfCache::WarmLight( nvrhi::ICommandList* commandList, const idRenderLightLocal* light )
{
	if( light == NULL || m_Pipeline == nullptr || m_Table == nullptr )
	{
		return false;
	}
	extern idCVar r_shadowPenumbraSize, r_softShadowSurfCacheErrTol;
	extern bool R_BuildLightStaticSoftStream( const idRenderLightLocal* light, float penumbraSize,
			idList<idVec4>& outTris, idList<idVec4>& outCasters, idList<idVec4>& outRecvTris,
			int& outStaticCasters, int& outStaticTris, int& outRecvTriCount, uint64_t& outFingerprint );
	extern uint64_t R_LightStaticChainSig( const idRenderLightLocal* light, float penumbraSize );

	const float penumbra = r_shadowPenumbraSize.GetFloat();

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
		return false;
	}

	lightState_t& st = m_LightHash[ light->index ];
	if( st.hash != 0 && st.hash != fp )
	{
		st.generation++;		// static set changed: orphan this light's stale slots
		m_GenBumps++;
	}
	st.hash = fp;

	// pack [tris][casters] into the reused warm-stream buffer (float4 elements)
	const int triF4 = tris.Num();		// 3 per tri
	const int casF4 = casters.Num();	// 2 per caster
	EnsureWarmStream( triF4 + casF4 );
	if( m_WarmStream == nullptr )
	{
		return false;
	}
	commandList->writeBuffer( m_WarmStream, tris.Ptr(), ( size_t )triF4 * sizeof( idVec4 ), 0 );
	commandList->writeBuffer( m_WarmStream, casters.Ptr(), ( size_t )casF4 * sizeof( idVec4 ), ( size_t )triF4 * sizeof( idVec4 ) );

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

	// QUEUE-mode warm: the seed ENQUEUES this light's claimed texels; the build consumes only those
	// (bounded by SW_WARM_BUDGET), NOT the whole 1M-slot table - a whole-table sweep x this light's
	// casters is what hung the GPU (device removed). One dispatch warms up to SW_WARM_BUDGET texels;
	// larger lights cache their first SW_WARM_BUDGET texels and the rest fall to the exact miss walk.
	extern idCVar r_softShadowSurfCacheWarmBudget;
	const int SW_WARM_BUDGET = Min( r_softShadowSurfCacheWarmBudget.GetInteger(), m_QueueWords - 1 );
	commandList->clearBufferUInt( m_Queue, 0 );		// reset the queue count for this light's seed

	SoftSurfBuildCB cb;
	cb.lightR[0] = light->globalLightOrigin.x;
	cb.lightR[1] = light->globalLightOrigin.y;
	cb.lightR[2] = light->globalLightOrigin.z;
	cb.lightR[3] = penumbra;
	cb.range[0] = 0;					// triBase (float4 elems)
	cb.range[1] = nCas;
	cb.range[2] = triF4;				// casterBase (float4 elems) = right after the tris
	cb.range[3] = light->index & 0x1FFF;
	cb.caps[0] = m_TableCap;
	cb.caps[1] = m_PoolCap;
	cb.caps[2] = SW_WARM_BUDGET;		// build dispatch bound (queue mode processes queue[0..budget))
	cb.caps[3] = SW_SURF_MAX_RESIDUAL;
	cb.params[0] = m_Texel;
	cb.params[1] = m_Thr;
	cb.params[2] = ( float )m_QueueWords;
	cb.params[3] = r_softShadowSurfCacheErrTol.GetFloat();
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
		commandList->writeBuffer( m_ConstantBuffer, &cb, sizeof( cb ) );
		nvrhi::ComputeState scs;
		scs.pipeline = m_SeedPipeline;
		scs.bindings = { seedSet };
		commandList->setComputeState( scs );
		commandList->dispatch( ( nRecv + 63 ) / 64, 1, 1 );
	}

	// BUILD: consume the enqueued texels (bounded), filling F for each
	nvrhi::BindingSetDesc sd;
	sd.bindings =
	{
		nvrhi::BindingSetItem::ConstantBuffer( 0, m_ConstantBuffer ),
		nvrhi::BindingSetItem::StructuredBuffer_SRV( 0, m_WarmStream ),
		nvrhi::BindingSetItem::StructuredBuffer_SRV( 1, m_WarmStream ),	// dummy t1 (include requirement)
		nvrhi::BindingSetItem::StructuredBuffer_UAV( 0, m_Table ),
		nvrhi::BindingSetItem::StructuredBuffer_UAV( 1, m_Pool ),
		nvrhi::BindingSetItem::StructuredBuffer_UAV( 2, m_Queue ),
	};
	nvrhi::BindingSetHandle set = m_Device->createBindingSet( sd, m_Layout );
	commandList->writeBuffer( m_ConstantBuffer, &cb, sizeof( cb ) );
	nvrhi::ComputeState cs;
	cs.pipeline = m_Pipeline;
	cs.bindings = { set };
	commandList->setComputeState( cs );
	commandList->dispatch( ( SW_WARM_BUDGET + 63 ) / 64, 1, 1 );

	st.seedPending = false;
	st.needSweep = false;		// warmed (up to SW_WARM_BUDGET texels) in this one dispatch
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

void SoftShadowSurfCache::EndBuilds( nvrhi::ICommandList* commandList )
{
	if( !m_Active )
	{
		return;
	}
	// reset the request queue: this frame's term dispatches claim a fresh budget's worth of texels.
	// Claims that found the queue full reverted their slot to empty, so nothing is ever lost - the
	// texel just re-claims on a later frame.
	commandList->clearBufferUInt( m_Queue, 0 );
	// HUD readback (non-blocking): snapshot THIS frame's counters into the ring, then map back the
	// OLDEST ring entry - copied SW_STATS_RING-1 frames ago, so the GPU is long done and mapBuffer
	// never stalls the pipe (unlike GetStats, which waitForIdle's). Feeds com_showFPS's cache line.
	const uint64_t swCounterOff = ( uint64_t )m_TableCap * 8 * sizeof( uint32_t );
	if( m_StatsRing[0] != nullptr )
	{
		commandList->copyBuffer( m_StatsRing[m_StatsRingWrite], 0, m_Table, swCounterOff, 4 * sizeof( uint32_t ) );
		const int oldest = ( m_StatsRingWrite + 1 ) % SW_STATS_RING;
		void* p = m_Device->mapBuffer( m_StatsRing[oldest], nvrhi::CpuAccessMode::Read );
		if( p != nullptr )
		{
			memcpy( m_HudStats, p, 4 * sizeof( uint32_t ) );
			m_Device->unmapBuffer( m_StatsRing[oldest] );
		}
		m_StatsRingWrite = ( m_StatsRingWrite + 1 ) % SW_STATS_RING;
	}
	// reset the per-frame path counters (the 4 words after the table) so each frame's term
	// dispatches accumulate a fresh hit/miss/walkalways/anchor-reject sample
	const uint32_t swZero[4] = { 0, 0, 0, 0 };
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
	// Build AT MOST ONE light per frame: each WarmLight is a bounded GPU dispatch, and more than one
	// per frame can trip the GPU watchdog (device removed) on many-caster lights. Explicit queue first,
	// then a bounded cheap-skip scan of the whole map. WarmLight returns true only when it actually
	// dispatched a (re)build, so the map warms one light per frame (~a few seconds), stably.
	bool builtOne = false;
	size_t qi = 0;
	for( ; qi < warm.size() && !builtOne; qi++ )
	{
		const int idx = warm[qi];
		if( idx >= 0 && idx < world->lightDefs.Num() )
		{
			const idRenderLightLocal* light = world->lightDefs[idx];
			if( light != NULL && WarmLight( commandList, light ) )
			{
				builtOne = true;
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
		std::lock_guard<std::mutex> lock( m_WarmMutex );
		m_WarmWorld = world;
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
	const int n = world->lightDefs.Num();
	int warmed = 0;
	// drive the LOADING BAR: this burst runs from ExecuteMapChange (behind the load screen), so report
	// its progress like any other load step. Increment per light advances the bar AND renders the load
	// GUI (LoadPacifierProgressIncrement -> UpdateLevelLoadPacifier), which self-guards to a no-op when
	// not inside a map change (the view-path fallback), so this is safe from either caller.
	common->LoadPacifierProgressTotal( n );
	for( int i = 0; i < n; i++ )
	{
		common->LoadPacifierProgressIncrement( 1 );
		const idRenderLightLocal* light = world->lightDefs[i];
		if( light == NULL )
		{
			continue;
		}
		cl->open();
		const bool built = WarmLight( cl, light );
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
}
