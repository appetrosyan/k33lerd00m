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

#ifndef __SOFT_SHADOW_SURF_CACHE_H__
#define __SOFT_SHADOW_SURF_CACHE_H__

#include <nvrhi/nvrhi.h>
#include <unordered_map>
#include <vector>
#include <mutex>

struct viewDef_t;
struct viewLight_t;
class idRenderLightLocal;

// SURFACE-FOLD CACHE probe (r_softShadowSurfCache, plan noble-sniffing-rose): a persistent,
// world-anchored per-texel cache of the STATIC part of the analytic soft-shadow coverage term.
// Per texel: 4 corner values of F = coverage(all static) - coverage(residual) plus the RESIDUAL
// occluder index list (occluders whose solo coverage curves across the texel). Cached fragments
// compute bilerp(F) + exact walk(residual + dynamic); everything else stays on the exact shipped
// walk. LAZY build: the term CS enqueues missing texels (bounded by the queue = the budget), a
// budgeted build CS fills them next frame. GPU state is three global uint buffers (all lights
// share them; the light index is part of the texel key):
//   table: open-addressing hash, 8 uints/slot (layout doc in softterm.cs.hlsl)
//   pool:  [0] alloc counter, then residual triangle indices (static-prefix stream order)
//   queue: [0] count, then claimed slot indices (capacity == the per-frame build budget)
// INVALIDATION is wholesale: any soft light's static-set fingerprint (viewLight_t::softSurfHash,
// computed at flatten) changing - a mover settling, a cvar change - clears the entire cache and it
// re-warms lazily. Probe simplicity; the miss path is always exact, so a clear only costs perf.
// Buffers are standalone nvrhi handles owned here (never idImageManager members - the known layout
// landmine), self-healing per frame, freed with the object.
class SoftShadowSurfCache
{
public:
	explicit SoftShadowSurfCache( nvrhi::IDevice* device );

	// Once per view, BEFORE any build/term dispatch: snapshot the cvars, (re)create the buffers on
	// capacity/param changes, compare every soft light's static-set hash and clear the cache if any
	// changed. lastGpuMicros = the previous frame's GPU time: while PREWARMING, headroom (< the
	// 120-FPS budget cvar) escalates the per-frame build budget so the cache fills during load /
	// idle GPU time instead of dripping. Returns false when the probe is off / cannot run.
	bool BeginView( nvrhi::ICommandList* commandList, const viewDef_t* viewDef, uint64_t lastGpuMicros );

	// Per soft light, before the term dispatches. Three duties: (1) if this light's static set is
	// newly fingerprinted, dispatch the PREWARM SEED pass (one thread per static tri, claims every
	// texel of the light's static geometry with a triangle-plane anchor - exact and deterministic);
	// (2) while the prewarm sweep is active, dispatch the build CS in table-SCAN mode over this
	// frame's slot window; (3) otherwise consume last frame's lazy request queue. The build CS
	// skips other lights' entries. No-op when the light has no static prefix.
	void BuildLight( nvrhi::ICommandList* commandList, const viewLight_t* vLight,
					 nvrhi::IBuffer* jointBuffer, uint32_t triBase, uint32_t casterBase,
					 float penumbraRadius );

	// After ALL BuildLight calls, before the term dispatches: reset the request queue so this
	// frame's term CS can claim a fresh budget's worth of texels, and advance (or restart, after a
	// seed) the prewarm scan cursor.
	void EndBuilds( nvrhi::ICommandList* commandList );

	bool IsActive() const
	{
		return m_Active;
	}
	// blocking readback of the per-frame path counters (0 hit / 1 miss / 2 walk-always /
	// 3 anchor-reject) - LAST rendered frame's sample (EndBuilds clears them each frame).
	// Bench/diagnostic only (waitForIdle).
	bool GetStats( uint32_t out[4] );
	// blocking readback of the 4 MISS sub-reason counters (0 stale-gen, 1 requested-unbuilt, 2 empty-slot,
	// 3 probe-overflow) - splits WHERE the miss% comes from. Diagnostic (waitForIdle).
	bool GetMissReasons( uint32_t out[4] );
	// TRUSTWORTHY table census (blocking full scan): out = [built, requested-unbuilt, empty, other]. Unlike
	// the per-frame path counters (whose GPU-atomic breakdown proved unreliable), this reads the actual slot
	// states, so it honestly shows warm progress + table FILL (oversubscription = probe-overflow = no hits).
	bool GetTableCensus( uint32_t out[4] );
	// MEASUREMENT one-shot: read the whole warmed table back and print the per-texel saving distribution
	// (foldedCount histogram, biggest saving + its cell, per-light Sum-folded vs static caster cost).
	// r_softShadowSurfCacheDump gates the call at the end of WarmMapBurst. Blocking; diagnostic only.
	void DumpTableHistogram();
	// NON-BLOCKING per-frame counters for the HUD (com_showFPS): [hit, miss, walkalways, anchor-rej],
	// read back from a staging ring several frames deep so the GPU is already done - no waitForIdle,
	// no pipe stall. Plain memory copy; safe to call from the main thread.
	void GetHudStats( uint32_t out[4] ) const
	{
		out[0] = m_HudStats[0];
		out[1] = m_HudStats[1];
		out[2] = m_HudStats[2];
		out[3] = m_HudStats[3];
	}
	// prewarm-state snapshot for the bench line: scan cursor (>= cap = idle), table cap, and how
	// many known lights still have seedPending/needSweep - diagnoses a starved lazy-build ratchet
	void GetPrewarmState( int& cursor, int& cap, int& pending ) const
	{
		cursor = m_ScanCursor;
		cap = m_TableCap;
		pending = 0;
		for( const auto& kv : m_LightHash )
		{
			if( kv.second.seedPending || kv.second.needSweep )
			{
				pending++;
			}
		}
	}
	// fully prewarmed light: seeded, carried through a complete table sweep, sweep idle. Warm cached
	// fragments never read the tile lists, so the backend may skip this light's tile-bin dispatch
	// (r_softShadowSurfCacheSkipBin); misses fall back to the bounded full walk.
	bool IsWarmLight( int lightIndex ) const
	{
		if( !m_Active || m_ScanCursor < m_TableCap )
		{
			return false;
		}
		auto it = m_LightHash.find( lightIndex );
		return it != m_LightHash.end() && it->second.hash != 0
			   && !it->second.seedPending && !it->second.needSweep;
	}
	nvrhi::IBuffer* GetTable() const
	{
		return m_Table;
	}
	nvrhi::IBuffer* GetPool() const
	{
		return m_Pool;
	}
	nvrhi::IBuffer* GetQueue() const
	{
		return m_Queue;
	}
	int GetTableCap() const
	{
		return m_TableCap;
	}
	int GetQueueWords() const
	{
		return m_QueueWords;
	}
	float GetTexel() const
	{
		return m_Texel;
	}

	// ---- camera-independent WARM path (plan noble-sniffing-rose) ----
	// Build a light's static cache COMPLETELY and camera-independently from its interaction chain
	// (seed + build-to-completion). Never called from the per-view render path, so camera motion
	// cannot trigger a build. No-op if the light moved or has no static casters.
	bool WarmLight( nvrhi::ICommandList* commandList, const idRenderLightLocal* light );
	// Queue a light to (re)warm; deduped, drained by DrainWarmQueue. Cheap - safe to call from the
	// engine's interaction/spawn hooks (GenerateAllInteractions, UpdateEntityDef, lightHasMoved).
	// Captures the light's world so the drain can resolve indices fresh (no dangling pointers).
	void EnqueueWarm( const idRenderLightLocal* light );
	// Drain up to maxLights (<=0 = all) queued lights on this command list. Runs before the view; a
	// partially drained queue just leaves those lights briefly uncached (miss = exact walk, no spike).
	void DrainWarmQueue( nvrhi::ICommandList* commandList, int maxLights );
	// Geometry changed under a light (caster moved/spawned/freed, or the light moved): bump its
	// generation (orphans its stale GPU slots) and queue a re-warm. Camera motion never calls this.
	void InvalidateLight( const idRenderLightLocal* light );

	// WARM-AT-LOAD: true exactly once per new world (render thread). The backend calls this on the
	// first soft-frame of a map and, if true, bursts the whole map warm behind the load fade instead
	// of dripping one light per gameplay frame (the visible startup spikes).
	bool TakeLoadWarm( class idRenderWorldLocal* world );
	// Warm EVERY light of the world synchronously, one light per command-list submit+wait (bounded,
	// watchdog-safe). Heavy - one load-transition frame - so the map is fully warm before gameplay.
	// Must be called with NO immediate command list open (the backend closes its list first).
	void WarmMapBurst( nvrhi::IDevice* device, class idRenderWorldLocal* world );
	// Test hook: forget which world was load-warmed so the next TakeLoadWarm re-fires (the motion bench
	// rebuilds its scene mid-run and needs a fresh burst). No layout change - inline.
	void ResetLoadWarm()
	{
		m_LastBurstWorld = nullptr;
	}

private:
	void EnsurePipeline();
	bool EnsureResources();								// cvars + pipelines + buffers; ready to dispatch
	void DoClearIfNeeded( nvrhi::ICommandList* commandList );
	void EnsureWarmStream( int float4Count );			// (re)create the reused caster warm-stream buffer
	void EnsureRecvStream( int float4Count );			// (re)create the reused receiver-tri seed buffer

	nvrhi::DeviceHandle				m_Device;
	bool							m_PipelineTried = false;
	nvrhi::ShaderHandle				m_Shader;
	nvrhi::BindingLayoutHandle		m_Layout;
	nvrhi::ComputePipelineHandle	m_Pipeline;
	nvrhi::ShaderHandle				m_SeedShader;
	nvrhi::BindingLayoutHandle		m_SeedLayout;
	nvrhi::ComputePipelineHandle	m_SeedPipeline;
	nvrhi::BufferHandle				m_ConstantBuffer;
	nvrhi::BufferHandle				m_Table;
	nvrhi::BufferHandle				m_Pool;
	nvrhi::BufferHandle				m_Queue;
	nvrhi::BufferHandle				m_WarmStream;		// reused camera-independent warm caster stream (build walks)
	int								m_WarmStreamF4 = 0;	// its capacity in float4 elements
	nvrhi::BufferHandle				m_WarmRecvStream;	// reused receiver-tri stream (seed rasterises, keys the reads)
	int								m_WarmRecvStreamF4 = 0;
	static const int				SW_STATS_RING = 4;	// staging depth so the readback is always past its frame
	nvrhi::BufferHandle				m_StatsRing[SW_STATS_RING];	// per-frame counter snapshots (non-blocking HUD readback)
	int								m_StatsRingWrite = 0;
	int								m_StatsRingFilled = 0;	// frames written since last wipe; read a slot only once >= SW_STATS_RING
	uint32_t						m_HudStats[4] = { 0, 0, 0, 0 };	// last read-back [hit, miss, walkalways, anchor-rej]
	// Queues are written by the GAME/LOAD thread (interaction/spawn hooks) and drained by the RENDER
	// thread (GL_StartFrame), so they are mutex-guarded. All m_LightHash / GPU work stays render-thread
	// only (inside DrainWarmQueue -> WarmLight); the game thread only pushes light indices here.
	std::vector<int>				m_WarmQueue;		// indices to (re)build (fresh warm)
	std::vector<int>				m_InvalidateQueue;	// indices whose geometry changed: bump gen, then warm
	class idRenderWorldLocal*		m_WarmWorld = nullptr;	// world the queued indices resolve against
	std::mutex						m_WarmMutex;
	int								m_WarmScanCursor = 0;	// bounded lightDefs sweep for whole-map warm
	class idRenderWorldLocal*		m_LastBurstWorld = nullptr;	// world already load-warmed (TakeLoadWarm)
	int								m_TableCap = 0;		// slots (power of two)
	int								m_PoolCap = 0;		// uints
	int								m_Budget = 0;		// texels built (and claimable) per frame
	int								m_QueueWords = 0;	// budget + 1 (count word)
	float							m_Texel = 0.0f;
	float							m_Thr = -1.0f;
	float							m_ErrTol = -1.0f;
	// reduced-set snapshot (build semantics; change drops the cache) lives in EnsureResources static
	// locals, NOT as members - growing this class shifts its layout and surfaces a heap fault at init.
	bool							m_NeedClear = false;
	bool							m_Active = false;
	// prewarm state: seed on first sight of a light's static-set hash, then sweep the table window
	// by window until the cursor wraps (cursor >= tableCap = idle -> lazy queue mode)
	int								m_ActiveBudget = 0;		// this frame's build budget (governed)
	int								m_ScanCursor = 0;		// >= m_TableCap = prewarm idle
	bool							m_SeededThisFrame = false;
	bool							m_WantRestart = false;	// an in-view light missed the last sweep
	struct lightState_t
	{
		uint64_t	hash = 0;
		uint32_t	generation = 0;		// bumped on each fingerprint change: orphans THIS light's stale
		// GPU slots (state word high bits) instead of wiping the whole table - other lights keep warm
		bool		seedPending = false;
		bool		needSweep = false;	// seeded but not yet carried through a full table sweep
	};
	std::unordered_map<int, lightState_t>	m_LightHash;	// lightDef->index -> static-set fingerprint state
	int								m_GenBumps = 0;			// generation bumps since last real GC clear (bounds slot leakage)

	// Per-light warm build state (render-thread only). The assembled static stream is CACHED here so a
	// multi-frame budgeted table sweep re-uploads (cheap) without re-collecting faces each frame; the
	// build sweeps m_Budget slots per frame (NOT the whole table - that TDRs the GPU on a many-caster
	// light), advancing sweepCursor until it covers the table, then the light is warm.
	struct warmEntry_t
	{
		std::vector<float>	tris;		// float4 stream, 4 floats/elem (3 elems/tri)
		std::vector<float>	casters;	// float4 stream, 4 floats/elem (2 elems/caster)
		int					nCas = 0;
		int					nTris = 0;
		int					sweepCursor = 0;
		bool				seeded = false;
	};
	std::unordered_map<int, warmEntry_t>	m_WarmEntries;

public:
	// per-light GPU generation for the term dispatch's CB (0 if the light is unknown/uncached)
	uint32_t GetLightGeneration( int lightIndex ) const
	{
		auto it = m_LightHash.find( lightIndex );
		return it != m_LightHash.end() ? it->second.generation : 0u;
	}

private:
	std::vector<int>				m_DispatchedLights;		// lights BuildLight ran for this view
};

#endif // __SOFT_SHADOW_SURF_CACHE_H__
