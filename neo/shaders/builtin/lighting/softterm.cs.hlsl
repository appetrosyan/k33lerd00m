/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 2026 Aleksandr Petrosyan

This program is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version. See <http://www.gnu.org/licenses/>.

===========================================================================
*/

// Analytic soft shadows: PER-LIGHT COMPUTE evaluation of the coverage integral
// (r_softShadowCompute). One dispatch per soft light over the light's scissor rect; each thread
// evaluates the SAME face-coverage integral the interaction pixel shader runs (the include below is
// the shared, unit-tested source), against the EXACT rasterised receiver world position from the
// softShadowPos G-buffer (written by softpos.{vs,ps}.hlsl at depth-EQUAL - NO depth
// reconstruction, so no grazing instability), and writes the visibility term into this light's
// slot of the R32F term atlas. The interaction pixel shader then just Loads its term texel
// (rpUser6) instead of running the integral in the wave64 fragment shader.
//
// Designed properties:
//   - BIT-EXACT vs the fragment path: same include, same inputs (the position G-buffer stores the
//     same interpolated model position -> world transform at float32), R32F storage (no
//     quantisation) => the anaTerm hashes must MATCH the FS path.
//   - Rate-independent: the term is computed at full pixel rate regardless of raster VRS on the
//     interaction pass, and the cost is decoupled from raster/overdraw.
//   - Wave32-friendly: 32-thread workgroups (8x4) so RDNA3 can run wave32 (VOPD dual-issue),
//     which the wave64 pixel shader forecloses. (No explicit subgroup-size control: this nvrhi
//     has no VK_EXT_subgroup_size_control plumbing; the workgroup size only encourages it.)

#define SW_TILE_SIZE	16
#define SW_TILE_K		512		// must match softtile_bin.cs.hlsl + SoftTileBinPass.h
#define SW_TILE_UMBRA	0xFFFFFFFEu	// whole-tile umbra sentinel (softtile_bin.cs.hlsl): term is exactly 0
#define SW_TILE_SPILL	0xFFFFFFFDu	// overflowed tile spilled its FULL list: slot+1/+2 = ( span offset, count )

// *INDENT-OFF*
// Declared BEFORE the include: the coverage functions read these globals directly (HLSL).
StructuredBuffer<float4>	t_SoftEdges	: register( t0 );	// softShadowEdge_t stream (float4 pairs), whole joint buffer
StructuredBuffer<uint>		t_SoftTiles	: register( t1 );	// per-tile triangle lists (softtile_bin.cs.hlsl)
#if SW_GPU_WALK_COUNTERS
// WALK-ATTRIBUTION counting permutation (built only for -D SW_GPU_WALK_COUNTERS=1, bound only under
// r_softShadowWalkCounters). Declared before the include so the walker's SW_ATTRIB_ADD macro (HLSL
// counting branch) can InterlockedAdd into it. The shipped permutation (=0) never declares this and
// gets the empty macro -> byte-identical shader, anaTerm contract preserved. Slots: 0..6 per
// SW_WALKIDX_* (softwedge_coverage.inc.hlsl), 7 = fragments that ran the walk.
RWStructuredBuffer<uint>	u_WalkCnt	: register( u1 );
#endif
#if SW_SURF_CACHE
// SURFACE-FOLD CACHE probe (r_softShadowSurfCache, separate pipeline - the shipped shader never
// declares these). Table: open-addressing hash of texel records, 8 uints each: [0] keyLo (0xFFFFFFFF
// = empty), [1] keyHi, [2] state (1 requested / 2 built / 3 walk-always), [3] residual pool offset,
// [4] residual count, [5][6] the 4 corner F values as packed fp16, [7] anchor (claiming fragment's
// height along the texel's normal axis, asuint). Queue: [0] = count, then claimed slot indices for
// the build CS. Pool: [0] = alloc counter, then residual triangle indices (static-prefix stream).
RWStructuredBuffer<uint>	u_SurfTable	: register( u2 );
// u_SurfQueue removed: the READ-ONLY runtime term never claims/enqueues (the burst seeds via
// softsurf_seed), so it binds NO request queue. This also keeps the reflected binding layout stable
// across sample-count permutations (a stripped-but-declared u3 desynced the layout and crashed at s32).
#if SW_SURF_GRID
// GRID mode reuses register t6 (the residual pool is excluded in grid mode) for the parallel static
// Fubini grid buffer - so the binding LAYOUT is identical to the scalar surf permutation (u2 table + t6),
// no extra slot to strip/desync. SW_SCAN_CHORDS words per slot.
StructuredBuffer<uint>		t_SurfGrid	: register( t6 );
#else
StructuredBuffer<uint>		t_SurfPool	: register( t6 );	// read before the include: the residual walk consumes it
#endif

// order-preserving float->uint encoding for the texel anchor (word 7): anchors accumulate via
// InterlockedMin so the value is the MIN height over all contributors - order-independent, so the
// claim-race winner cannot leak into the cached value. MUST match softsurf_seed/softsurf_build.
uint SwSurfFlipF( float f )
{
	const uint u = asuint( f );
	return ( u & 0x80000000u ) ? ~u : ( u | 0x80000000u );
}
#endif
// GRID mode REQUIRES the Fubini scanline primitives (SoftScan_*, SW_SCAN_CHORDS). The shader-permutation
// matrix (shaders.cfg) varies SW_SURF_GRID and SW_SCANLINE independently, so force SW_SCANLINE=1 whenever
// SW_SURF_GRID=1 - the surfgrid pipeline is always requested with SW_SCANLINE=1 anyway; this just makes the
// otherwise-invalid SW_SURF_GRID=1/SW_SCANLINE=0 cross-product entries compile (they are dead, never bound).
#if SW_SURF_GRID
	#undef SW_SCANLINE
	#define SW_SCANLINE 1
#endif
// the contributor cache is SCANLINE-only (its record/serve paths fill the Fubini grid); force the
// axis for the same dead-cross-product-compiles reason as SW_SURF_GRID above.
#if SW_CONTRIB_CACHE
	#undef SW_SCANLINE
	#define SW_SCANLINE 1
#endif
// CULL-BEFORE-LOAD (term compute path only; the interaction PS keeps the load-then-cull walk): the tile
// walk reads this parallel per-slot (centroid, triRad) buffer to run the cone cull BEFORE the scattered
// vertex gather, so culled entries never load verts. Declared + bound unconditionally to keep the reflected
// binding layout stable across the sample/scanline/surf permutations (see the u3 desync note above).
#define SW_CULL_BEFORE_LOAD 1
// runtime toggle (r_softShadowCullBeforeLoad, via SoftTermCB.surfCost[1]). The coverage function is
// defined by the include below - BEFORE the c_Term cbuffer - so it cannot read g_surfCost directly; main()
// stashes the flag into this per-thread static before the walk, and SW_CBL_RT reads it.
static bool swCblRT = false;
#define SW_CBL_RT swCblRT
StructuredBuffer<uint2>		t_SoftCull	: register( t7 );

// CONTRIBUTOR CACHE (r_softShadowContribCache, SW_CONTRIB_CACHE permutation, SCANLINE only): the
// evaluate-once-union design (studies: 95% of walk iterations are proven waste; per-cell contributor
// unions of 7-17 tris are coverage-exact at K=64 evaluations, held-out verified on cap0004/7/9).
// Per (world cell, light) the FIRST K fragments run the full walk and RECORD their solo-contributing
// STATIC triangles into the cell's slot (the fragments themselves are the evaluation points - no
// heuristic anywhere); at K evaluations the cell flips BUILT and later fragments walk only the
// recorded union + live dynamic casters. Warm-up is INCREMENTAL: cells enter RECORDING only while a
// bounded slot pool has room (header word 0), so a camera entering a fresh area warms over frames
// with no atomic storm. All fallbacks (unbuilt / pool-full / overflow / stale generation) are the
// exact shipped walk - the cache can only remove proven-waste iterations, never coverage.
// Slot layout (stride SW_CONTRIB_STRIDE uints): [0] keyLo [1] keyHi (gen | light key)
// [2] evalCount, bit31 = BUILT [3] triCount (<= SW_CONTRIB_K) [4..] recorded GLOBAL tri indices.
// Header (first SW_CONTRIB_HEADER uints of the buffer): [0] recording-pool occupancy
// [1] serve hits [2] recording evals [3] claimed cells (stats, wave-aggregated).
#ifndef SW_CONTRIB_CACHE
	#define SW_CONTRIB_CACHE 0
#endif
#if SW_CONTRIB_CACHE
#define SW_CONTRIB_K		64
// TILE-COVERAGE certification, frame-deferred (the BUILT-flip-defer analogue for tiles): a
// record finalize PENDS its tile bit; any prober PROMOTES pends to LIVE only in a frame with no
// record finalize yet (PENDFRAME != now). Trusting a bit the frame it was set let late-launching
// waves serve a union whose recorders were still walking (spill records are slow) - measured
// 4.6%/15.7% serve-verify mismatches; the deferral closes that window to one wave race.
#define SW_CONTRIB_TILEBIT	( 4 + SW_CONTRIB_K )		// slot word: LIVE tile-coverage mask (serve gate)
#define SW_CONTRIB_TILEPEND	( 4 + SW_CONTRIB_K + 1 )	// slot word: PENDING tile bits (this frame's records)
#define SW_CONTRIB_TILEPFR	( 4 + SW_CONTRIB_K + 2 )	// slot word: frame of the last pend write
#define SW_CONTRIB_STRIDE	( 4 + SW_CONTRIB_K + 3 )
// header words 16-19: serve-verify mismatch attribution ([16] serve DARKER = over-coverage,
// [17] serve LIGHTER = under-coverage, [18] on a SPILL fragment, [19] on a tile-list fragment)
#define SW_CONTRIB_HEADER	24
#define SW_CONTRIB_BUILT	0x80000000u
#define SW_CONTRIB_POISON	0x40000000u		// list overflowed K after the flip: cell may NEVER serve (exact walk forever)
RWStructuredBuffer<uint>	u_Contrib	: register( u2 );
// wave-aggregated diagnostic counter into a free header word (one atomic per wave, SwSurfStat
// pattern): [7] budget-blocked claims [8] probe-exhausted (16 slots, no key/no empty admitted)
// [9] BUILT-but-unservable-tile plain walks [10] refinement records. One-shot readback deltas.
void SwContribDiag( uint word )
{
	const uint c = WaveActiveCountBits( true );
	if( WaveIsFirstLane() )
	{
		InterlockedAdd( u_Contrib[ word ], c );
	}
}
// append tri rt (1-based encoded) to cell slot sB's union with linear dedup; overflow POISONs.
// Returns true when the union grew (the refinement revoke signal).
bool SwContribAppend( uint sB, int rt )
{
	const uint have = min( u_Contrib[ sB + 3 ], ( uint )SW_CONTRIB_K );
	for( uint dd = 0; dd < have; dd++ )
	{
		if( u_Contrib[ sB + 4 + dd ] == ( uint )( rt + 1 ) )
		{
			return false;								// already recorded
		}
	}
	uint at;
	InterlockedAdd( u_Contrib[ sB + 3 ], 1u, at );
	if( at < ( uint )SW_CONTRIB_K )
	{
		u_Contrib[ sB + 4 + at ] = ( uint )( rt + 1 );	// benign dup on race: union semantics
	}
	else
	{
		// overflow: a contributor could not be stored, so serving this cell would under-cover.
		// POISON it - pre-flip the BUILT withhold also blocks, but a post-flip overflow needs
		// this to revoke serving.
		uint pz;
		InterlockedOr( u_Contrib[ sB + 2 ], SW_CONTRIB_POISON, pz );
	}
	return true;
}
// wave-aggregated SUM of a per-lane value into a header word: serve-work attribution
// ([12] union entries walked [13] union FillTri survivors [14] dyn FillTri survivors
// [15] serve umbra early-outs). One atomic per wave.
void SwContribDiagSum( uint word, uint v )
{
	const uint s = WaveActiveSum( v );
	if( WaveIsFirstLane() )
	{
		InterlockedAdd( u_Contrib[ word ], s );
	}
}
#endif
#include "softwedge_coverage.inc.hlsl"

Texture2D<float4>			t_WorldPos	: register( t2 );	// exact receiver world position (softShadowPosImage)
// this light's falloff/projection textures (the same ones the interaction PS samples): the
// COVERAGE early-out below skips the integral where their product is exactly zero. Bound to
// black + any sampler when g_flags.x == 0 (early-out ineligible/disabled).
Texture2D<float4>			t_Falloff	: register( t3 );
Texture2D<float4>			t_Proj		: register( t4 );
Texture2D<float4>			t_WorldNormal	: register( t5 );	// world SHADING normal (softpos 2nd MRT), N.L<=0 early-out
SamplerState				s_Falloff	: register( s0 );	// the falloff image's OWN sampler (zero-clamp border preserved)
SamplerState				s_Proj		: register( s1 );	// the projection image's OWN sampler
RWTexture2D<float>			u_Term		: register( u0 );	// R32F term atlas, one screen-size slot per light

cbuffer c_Term : register( b0 )
{
	float4	g_lightR;	// light origin xyz, disk radius w
	int4	g_range;	// firstElem (tri stream float4 base), casterCount, tileBase | -1, tilesX
	int4	g_tile;		// tile origin x, y (in tiles), atlas slot offset x, y (in pixels)
	int4	g_rect;		// scissor origin x, y (absolute pixels), width, height
	float4	g_falloffS;	// WORLD-space light falloff plane (vLight->lightProject[3])
	float4	g_projS;	// WORLD-space light projection planes (vLight->lightProject[0..2]);
	float4	g_projT;	//   the VS folds these into model space per surface - plane . worldPos
	float4	g_projQ;	//   yields the same texcoord value the PS's idtex2Dproj consumes
	int4	g_flags;	// x: 1 = coverage early-outs enabled (0 under debug shaders so the
						//    anaTerm-hash instrument stays full-field, and for lights the CPU
						//    could not qualify: multi-stage light shaders / stage texture
						//    matrices, where one static plane set cannot represent the stages)
						// y: caster table base (float4 elements into t_SoftEdges) - stream v2
	float4	g_classAabbCell;	// lit classifier grid: origin xyz + cellSize
	int4	g_classDims;		// dims xyz + BASE (float4 elem offset into t_SoftEdges); base < 0 = disabled
	float4	g_surfParams;		// x = surf-cache texel size G (world units), y = surf-cache viz mode,
								//   z = r_softShadowLitEarlyOut (intensity threshold; 0 = exact),
								//   w = r_softShadowRotGrid (rotation-hash world grid; 0 = exact per-position)
	int4	g_surfA;			// surface-fold cache: x = table capacity (slots, pow2), y = queue capacity
								//   (uints incl. count word), z = static caster count (dynamic suffix starts
								//   there), w = light key (13 bits). x <= 0 = cache disabled for this light.
	float4	g_aa;				// x = r_softShadowSamples: disk ray count. 0 = soft coverage off (term 1.0),
								//   16 = shipped tile-bin walk, else runtime-N walk. y/z/w unused.
	int4	g_surfCost;			// x = surf-cache COST GATE (r_softShadowSurfCacheMinCost): tiles with fewer
								//   than x occluders bypass the cache entirely (no probe, no build request) -
								//   we only pay the lookup where the walk is dear enough to beat it. 0 = gate off.
};
// *INDENT-ON*

#if SW_SURF_CACHE
// Wave-aggregated per-frame path counter (see SW_SURF_STAT): sum the active lanes in each class and
// commit ONE atomic per wave per class, instead of one InterlockedAdd per fragment on a single global
// word (that serialised the whole term at low hit rate). Global scope - HLSL forbids nested functions.
void SwSurfStat( uint idx )
{
	[unroll] for( uint v = 0u; v < 4u; v++ )
	{
		const uint c = WaveActiveCountBits( idx == v );
		if( c != 0u && WaveIsFirstLane() )
		{
			InterlockedAdd( u_SurfTable[ ( uint )g_surfA.x * 8u + v ], c );
		}
	}
}
// MISS sub-reason instrument (4 words after the 4 class counters): 0 stale-gen, 1 requested-unbuilt,
// 2 empty-slot (never seeded), 3 probe-overflow (key not found within the 16-slot chain). Same wave
// aggregation as SwSurfStat. Only called for miss fragments, so it splits WHERE the 36% miss comes from.
void SwSurfMissReason( uint r )
{
	[unroll] for( uint v = 0u; v < 4u; v++ )
	{
		const uint c = WaveActiveCountBits( r == v );
		if( c != 0u && WaveIsFirstLane() )
		{
			InterlockedAdd( u_SurfTable[ ( uint )g_surfA.x * 8u + 4u + v ], c );
		}
	}
}
#endif

[numthreads( 8, 4, 1 )]
void main( uint3 tid : SV_DispatchThreadID )
{
	if( ( int )tid.x >= g_rect.z || ( int )tid.y >= g_rect.w )
	{
		return;
	}
	const int2 px = int2( g_rect.x + ( int )tid.x, g_rect.y + ( int )tid.y );
	swCblRT = ( g_surfCost.y != 0 );	// cull-before-load runtime toggle (read by the tile-list walk)

	// EXACT receiver position (same value the interaction PS computes from texcoord7 x model
	// matrix; softpos.ps stored it at float32). Pixels never rasterised by the position pass
	// (sky, translucent-only) hold the clear value (w == 0); the interaction shader never reads
	// the term there (translucent draws keep the in-shader integral, sky draws no soft
	// interaction), so writing 1.0 and skipping the integral is free.
	const float4 swPos = t_WorldPos.Load( int3( px, 0 ) );
	const float3 swP = swPos.xyz;

	// COVERAGE EARLY-OUTS - the fix for the measured 1.8x loss of the first compute-decoupling
	// cut (see r_softShadowCompute help): the fragment path pays only for pixels that survive
	// depth/scissor/stencil culling AND its falloff-first zero test, while this dispatch covers
	// the whole scissor rect. Mirror both tests here so the covered pixel sets converge.
	// Skipped pixels WRITE 1.0 (never a bare return): the interaction PS may still Load any
	// pixel inside the scissor - on the exact-zero boundary its own interpolated falloff sample
	// can disagree with ours in the last bit, and 1.0 x (a contribution of exactly ~0) is the
	// value the falloff-first FS path produces there anyway.
	if( g_flags.x != 0 )
	{
		bool swSkip = ( swPos.w == 0.0f );				// never rasterised: no receiver here
		if( !swSkip )
		{
			const float4 swWP = float4( swP.x, swP.y, swP.z, 1.0f );
			const float  swPw = dot( swWP, g_projQ );
			if( swPw > 0.0f )							// behind the projection apex: keep the integral (conservative)
			{
				float2 swFuv = float2( dot( swWP, g_falloffS ), 0.5f );
				float2 swPuv = float2( dot( swWP, g_projS ), dot( swWP, g_projT ) ) / swPw;
				float4 swFall = t_Falloff.SampleLevel( s_Falloff, swFuv, 0 );
				float4 swProj = t_Proj.SampleLevel( s_Proj, swPuv, 0 );
				// INTENSITY LIT EARLY-OUT (r_softShadowLitEarlyOut = g_surfParams.z, default 0 = exact):
				// the analytic penumbra extends into regions the light barely reaches; there the shadow term
				// is multiplied by a near-zero contribution, so skipping the walk (-> term 1.0, no far
				// penumbra) is visually lossless. Cut where the light's falloff*projection product is below
				// the threshold, not just exactly zero. 0 reproduces the exact <=0 behaviour.
				swSkip = ( max( max( swProj.x * swFall.x, swProj.y * swFall.y ), swProj.z * swFall.z ) <= g_surfParams.z );
			}
		}
		// BACK-FACING RECEIVER early-out: the interaction masks BOTH diffuse and specular by
		// saturate(dot(shadingNormal, lightVector)) (lambert = ldotN; USE_HALF_LAMBERT off), so a
		// receiver with N.L <= 0 contributes EXACTLY 0 regardless of the shadow term. softpos wrote the
		// world SHADING normal with the interaction's exact bump decode, so this N.L matches the
		// interaction's ldotN; at the ldotN=0 boundary the contribution vanishes, so the shortcut is
		// lossless (the term is Load()ed per pixel, no filtering to bleed a wrong value). Self-disables
		// under half-Lambert (where the term matters down to N.L=-1).
#if !defined( USE_HALF_LAMBERT )
		if( !swSkip && g_flags.z != 0 )		// g_flags.z = r_softShadowBackfaceCull (A/B toggle)
		{
			const float3 swN = t_WorldNormal.Load( int3( px, 0 ) ).xyz;
			swSkip = ( dot( swN, g_lightR.xyz - swP ) <= 0.0f );	// L-P unnormalised: the sign is all that matters
		}
#endif
		if( swSkip )
		{
			u_Term[ uint2( px + g_tile.zw ) ] = 1.0f;
			return;
		}
	}

	// WORLD-CELL LIT CLASSIFIER (r_softShadowClassify): if this fragment's world cell is provably LIT (no
	// occluder cone reaches it - built at flatten with the same conservative cull), the coverage integral
	// is 0, so the term is 1.0 and the whole walk is skipped. Bit-exact vs the walk (a lit fragment's mask
	// is empty -> occ 0 -> term 1.0). Grid rides the joint buffer packed 16 class bytes/float4. Out-of-grid
	// or non-lit -> fall through to the walk. LIT = 0 (SW_CLASS_LIT).
	if( g_classDims.w >= 0 )
	{
		const int ccx = ( int )floor( ( swP.x - g_classAabbCell.x ) / g_classAabbCell.w );
		const int ccy = ( int )floor( ( swP.y - g_classAabbCell.y ) / g_classAabbCell.w );
		const int ccz = ( int )floor( ( swP.z - g_classAabbCell.z ) / g_classAabbCell.w );
		if( ccx >= 0 && ccy >= 0 && ccz >= 0 && ccx < g_classDims.x && ccy < g_classDims.y && ccz < g_classDims.z )
		{
			const int  ccell = ( ccz * g_classDims.y + ccy ) * g_classDims.x + ccx;
			const uint4 cw = asuint( t_SoftEdges[ g_classDims.w + ( ccell >> 4 ) ] );
			const uint  cbyte = ( cw[ ( ccell >> 2 ) & 3 ] >> ( ( uint )( ccell & 3 ) * 8u ) ) & 0xFFu;
			if( cbyte == 0u )
			{
				u_Term[ uint2( px + g_tile.zw ) ] = 1.0f;
				return;
			}
		}
	}

#if SW_GPU_WALK_COUNTERS
	InterlockedAdd( u_WalkCnt[ 7 ], 1u );	// this pixel survived the early-outs and runs the walk
#endif

	const float3 swL = g_lightR.xyz;
	const float  swR = max( g_lightR.w, 1e-2 );

	// per-fragment sample-set rotation: the SAME world-position hash the interaction PS computes
	// (interactionSM.ps.hlsl) - swP is bit-identical by the position G-buffer design, so the angle
	// (and therefore the whole integral) stays bit-exact vs the fragment path.
	// TEMPORAL-STABILITY (r_softShadowRotGrid = g_surfParams.w, default 0 = exact): the hash is
	// ULTRA high-frequency, so under TAA jitter each screen pixel samples a subpixel-different swP
	// every frame -> a totally different rotation -> the 1/16 coverage quantum FLIPS per frame and
	// TAA cannot resolve the stepped term (the crawling "fingerprint"). Snapping swP to a small world
	// grid before hashing makes the rotation CONSTANT within a cell, so subpixel jitter keeps the same
	// quantum and the term is temporally stable per world point (bands become static, not crawling).
	// Neighbours a cell apart still decorrelate. 0 keeps the exact per-position hash.
	float3 swRotP = swP;
	if( g_surfParams.w > 0.0f ) { swRotP = round( swP / g_surfParams.w ) * g_surfParams.w; }
	const float swRotHash = dot( swRotP, float3( 12.9898, 78.233, 37.719 ) );
	const float swRotAng = ( swRotHash - floor( swRotHash ) ) * 6.28318531;

#if SW_CONTRIB_CACHE
	// ---- CONTRIBUTOR CACHE (see the declaration block above for the design + slot layout) ----
	// g_surfParams.x = cell size G, g_surfCost.z = eval threshold K', g_surfCost.w = table capacity
	// (slots, pow2), g_surfA.w = light key, g_aa.y = per-light generation, g_aa.z = recording-pool
	// budget (g_flags.w stays the tile-K stride for the fallback tile walk).
	if( g_surfParams.x > 0.0f && g_surfCost.w > 0 )
	{
		// TILE PEEK: (a) UMBRA-SENTINEL BYPASS - a whole-tile umbra certificate writes term EXACTLY 0
		// with zero walk; strictly better than any cached walk, and mixing the two flickers the last
		// coverage bit (measured TEMPORAL gate defects). (b) capture the tile's NORMAL list for the
		// serve path's tiled dynamic walk - fragments without one (spill/corrupt/untiled) never
		// serve (the plain walk handles them; the untiled dynamic suffix measured hit > tiled miss).
		int swServeListBase = 0, swServeListCount = -1;
		int swSpillBase = 0, swSpillN = -1;
		const int swPTx = px.x / SW_TILE_SIZE - g_tile.x;
		const int swPTy = px.y / SW_TILE_SIZE - g_tile.y;
		// this fragment's TILE-COVERAGE bit (slot word SW_CONTRIB_TILEBIT): local 8x4-tile neighborhood
		// bit. The union is recorded from TILE LISTS, so it is only complete for tiles that contributed
		// an eval - serving a tile whose bit is unset produced under-covering terms (measured 0.033%
		// verify mismatches, max|diff| 1.0). Distant cells span 1-4 tiles (exact); near cells alias
		// (a wrongly-set bit), which the refinement validator converges instead.
		const uint swTileBit = 1u << ( ( ( uint )swPTx & 7u ) * 4u + ( ( uint )swPTy & 3u ) );
		{
			if( g_range.z >= 0 && swPTx >= 0 && swPTy >= 0 )
			{
				const int  swPSlot = g_range.z + ( swPTy * g_range.w + swPTx ) * ( g_flags.w + 1 );
				const uint swPCnt = t_SoftTiles[ swPSlot ];
				if( swPCnt == SW_TILE_UMBRA )
				{
					u_Term[ uint2( px + g_tile.zw ) ] = 0.0f;
					return;
				}
				if( swPCnt <= ( uint )g_flags.w )
				{
					swServeListBase  = swPSlot + 1;
					swServeListCount = ( int )swPCnt;
				}
				else if( swPCnt == SW_TILE_SPILL )
				{
					// SPILL tile: the fragment's walk set is the cluster list (absolute cluster-record
					// offsets). The heavy caps are spill-DOMINATED (cap0009: claims 839, recordings 0 -
					// the tile-list-only guard disabled the cache exactly where the walk is dearest), so
					// record/serve must run through the cluster hierarchy too.
					swSpillBase = ( int )t_SoftTiles[ swPSlot + 1 ];
					swSpillN    = ( int )min( t_SoftTiles[ swPSlot + 2 ], 65536u );
				}
			}
		}
		const float cg = g_surfParams.x;
		const int3  cc = int3( floor( swP / cg ) );
		// COLLISION-FREE cell key, surf-cache style: 16 bits per axis (biased) + 13-bit light key =
		// 61 of 64 bits, no folding. The earlier 21-bit XOR-fold aliased distinct cells into one
		// slot - two cells serving one union (measured: ANT speckle + cell-grain SEAM defects).
		// Out-of-range cells (|coord| >= 32768 cells) are never cached (exact walk).
		const int3 cb2 = cc + int3( 32768, 32768, 32768 );
		if( ( uint )cb2.x > 65535u || ( uint )cb2.y > 65535u || ( uint )cb2.z > 65535u )
		{
			// fall through to the plain path below
		}
		else
		{
		const uint keyLo = ( uint )cb2.x | ( ( uint )cb2.y << 16 );
		const uint keyHi = ( uint )cb2.z | ( ( uint )g_surfA.w << 16 );
		if( keyLo == 0u )
		{
			// the (-32768,-32768,*) far corner aliases the vacancy sentinel: never cached
		}
		else
		{
		const uint curGen = ( uint )( g_aa.y + 0.5f );
		const uint capM = ( uint )g_surfCost.w - 1u;
		uint h = keyLo * 0x9E3779B1u ^ keyHi * 0x85EBCA77u ^ curGen * 0xC2B2AE35u;
		uint slot = h & capM;
		int  servedBase = -1, servedCount = 0;
		int  recordSlot = -1;
		// probe-outcome diagnosis flags (classified into header words 7/8/9 after the loop)
		bool swDgBudget = false, swDgKey = false, swDgUnserv = false;
		bool swTileMissRec = false;			// this record is a tile-coverage fill-in, not a refinement verdict
		[loop] for( int pr = 0; pr < 16; pr++ )
		{
			const uint sBase = SW_CONTRIB_HEADER + slot * SW_CONTRIB_STRIDE;
			const uint w0 = u_Contrib[ sBase ];
			if( w0 == keyLo && u_Contrib[ sBase + 1 ] == ( keyHi ^ ( curGen * 0x9E3779B9u ) ) )
			{
				swDgKey = true;
				// PROMOTE pended tile bits to LIVE - only while no record has finalized THIS frame
				// (a finalize stamps PENDFRAME before pending its bit, closing the same-frame window)
				{
					const uint swFrameP = ( uint )g_surfA.y & 0x7FFFu;
					if( u_Contrib[ sBase + SW_CONTRIB_TILEPFR ] != swFrameP )
					{
						const uint swPendB = u_Contrib[ sBase + SW_CONTRIB_TILEPEND ];
						if( swPendB != 0u )
						{
							uint swPdD;
							InterlockedOr( u_Contrib[ sBase + SW_CONTRIB_TILEBIT ], swPendB, swPdD );
						}
					}
				}
				const uint ev = u_Contrib[ sBase + 2 ];
				// FLIP-FRAME DEFER (bits 16-30 = the frame the cell flipped BUILT): a cell must not
				// serve in the dispatch that flipped it - stragglers of the same dispatch may still be
				// appending (count is incremented before the entry store), which read as stale-zero
				// entries / missing contributors: nondeterministic terms (measured TEMPORAL=43 gate
				// defects). One frame later the list is quiescent (post-flip fragments only serve).
				const uint swFrameNow = ( uint )g_surfA.y & 0x7FFFu;
				if( ( ev & SW_CONTRIB_POISON ) != 0u && ( ev & SW_CONTRIB_BUILT ) == 0u )
				{
					// pre-flip overflow poison: the union needs > K entries, the cell can never
					// serve - and it must NOT keep recording either (recording costs 2-3x a plain
					// walk; measured: perpetual recording DOUBLED the live term). Plain walk.
					break;
				}
				if( ( ev & SW_CONTRIB_BUILT ) != 0u && ( ev & SW_CONTRIB_POISON ) == 0u
						&& ( swServeListCount >= 0 || swSpillN >= 0 )
						&& ( ( ev >> 16 ) & 0x7FFFu ) != swFrameNow )
				{
					// CONTINUOUS REFINEMENT: 1 in 64 served fragments (pixel hash) runs the exact
					// recording walk instead - its term is exact, its contributors append, and if it
					// found one MISSING from the union the cell is revoked (POISON) and re-records
					// with the new view's fragments included. Frozen unions were the measured
					// CONTINUITY defect source (displaced views hit receivers the flip-view never
					// evaluated); refinement converges them at ~1.5% average walk cost.
					// SPILL fragments refine 8x rarer: their record walk is the full cluster hierarchy
					// (~10x a tile-list record), and at 1/64 it alone regressed cap0007 term ~5 ms
					const uint swRefN = ( uint )( g_aa.w + 0.5f ) * ( ( swServeListCount < 0 ) ? 8u : 1u );
					if( ( u_Contrib[ sBase + SW_CONTRIB_TILEBIT ] & swTileBit ) == 0u )
					{
						// TILE-MISS: no eval from this tile has fed the union yet, so it may lack this
						// tile's contributors - serving it under-covered (the 0.033% max|diff|=1.0
						// verify class). Record instead: one dispatch's worth of this tile's fragments
						// walk+append, the bit is set at finalize, and the tile serves next frame.
						swTileMissRec = true;
						recordSlot = ( int )sBase;
					}
					else if( swRefN != 0u && ( ( ( uint )px.x * 7u + ( uint )px.y * 13u ) % swRefN ) == 0u )
					{
						SwContribDiag( 10u );						// refinement record volume
						recordSlot = ( int )sBase;
					}
					else
					{
						servedBase  = ( int )( sBase + 4u );
						servedCount = ( int )min( u_Contrib[ sBase + 3 ], ( uint )SW_CONTRIB_K );
					}
				}
				else if( ( ev & SW_CONTRIB_BUILT ) != 0u && swServeListCount < 0 && swSpillN < 0 )
				{
					// BUILT but this fragment's tile cannot serve (corrupt/untiled): PLAIN walk.
					// Routing these to the record path made every spill-tile fragment of a built cell
					// run the 2-3x recording walk EVERY frame - the dominant live overhead (measured:
					// term 30.9 -> 49-66 ms net-NEGATIVE).
					swDgUnserv = true;
					break;
				}
				else if( ( ev & SW_CONTRIB_BUILT ) != 0u )
				{
					// flipped THIS frame, or POISON-revoked by refinement: REFINEMENT-HASH fragments
					// only may record; everyone else plain-walks. v1 routed ALL these fragments to the
					// 2-3x record walk - measured 434-500k recording walks per STEADY frame (claims 0)
					// on the cap0007 bench, term 46.8 -> 66.6 ms. The union is defined by the K'
					// ticketed evals plus refinement convergence, not by a whole-cell stampede.
					const uint swRefN2 = ( uint )( g_aa.w + 0.5f );
					if( swRefN2 != 0u && ( ( ( uint )px.x * 7u + ( uint )px.y * 13u ) % swRefN2 ) == 0u )
					{
						SwContribDiag( 10u );	// refinement-class record (poison re-validation)
						recordSlot = ( int )sBase;
					}
				}
				else
				{
					// RECORDING state (not yet BUILT): the pre-walk CAS ticket below decides whether
					// this fragment is one of the K' evaluators or plain-walks while the cell builds.
					recordSlot = ( int )sBase;
				}
				break;
			}
			if( w0 == 0u )
			{
				if( swServeListCount < 0 && swSpillN < 0 )
				{
					break;		// no walk list: this fragment could never record - don't burn a slot
				}
				// empty: try to CLAIM (bounded by the recording pool - incremental warm-up)
				uint pool;
				// PER-FRAME claim budget (header[0] resets each frame on the CPU): budget is consumed by
				// SUCCESSFUL claims only - the earlier attempt-consuming version burned the whole frame
				// budget on duplicate attempts against the same few hot cells (~10-40 unique claims per
				// frame instead of ~budget). The pre-check read is racy; overshoot is bounded by
				// in-flight concurrency and harmless (the budget is a rate limiter, not a hard cap).
				const uint swPoolBudget = ( uint )( g_aa.z + 0.5f );
				pool = u_Contrib[ 0 ];
				if( pool < swPoolBudget )
				{
					uint prev;
					InterlockedCompareExchange( u_Contrib[ sBase ], 0u, keyLo, prev );
					if( prev == 0u )
					{
						u_Contrib[ sBase + 1 ] = keyHi ^ ( curGen * 0x9E3779B9u );
						InterlockedAdd( u_Contrib[ 0 ], 1u );		// budget: one successful claim
						InterlockedAdd( u_Contrib[ 3 ], 1u );		// stat: claimed cells
						recordSlot = ( int )sBase;
						break;
					}
					if( prev == keyLo && u_Contrib[ sBase + 1 ] == ( keyHi ^ ( curGen * 0x9E3779B9u ) ) )
					{
						recordSlot = ( int )sBase;					// lost the race to our own key
						break;
					}
				}
				else
				{
					swDgBudget = true;
					break;											// frame budget exhausted: plain walk this frame
				}
			}
			slot = ( slot + 1u ) & capM;
		}
		// PRE-WALK EVAL TICKET (bounded-transient fix): only K' fragments per cell EVER run the
		// record walk. The ticket is a CAS (never a blind add) so the count can NEVER carry into the
		// flip-stamp bits, no matter how many fragments race: a CAS only succeeds from a value below
		// K' with BUILT/POISON clear. Losers plain-walk while the cell builds - v1 instead recorded
		// EVERY in-flight fragment of a claiming cell (the unbounded transient: term +20 ms).
		// Refinement fragments (BUILT cell) keep the frozen counter and take no ticket.
		uint swMyTicket = 0xFFFFFFFFu;
		if( recordSlot >= 0 && swServeListCount < 0 && swSpillN < 0 )
		{
			// the record walk is LIST based (normal tile list or spill cluster list): a fragment with
			// neither (corrupt/untiled) cannot record (nor ever serve its tile) - plain, no ticket
			recordSlot = -1;
		}
		if( recordSlot >= 0 )
		{
			const uint swEvPre = u_Contrib[ ( uint )recordSlot + 2u ];
			if( ( swEvPre & SW_CONTRIB_BUILT ) == 0u )
			{
				[loop] for( int swTa = 0; swTa < 4; swTa++ )
				{
					const uint evc = u_Contrib[ ( uint )recordSlot + 2u ];
					if( ( evc & ( SW_CONTRIB_BUILT | SW_CONTRIB_POISON ) ) != 0u
							|| ( evc & 0xFFFFu ) >= ( uint )g_surfCost.z )
					{
						break;
					}
					uint got;
					InterlockedCompareExchange( u_Contrib[ ( uint )recordSlot + 2u ], evc, evc + 1u, got );
					if( got == evc )
					{
						swMyTicket = evc & 0xFFFFu;
						break;
					}
				}
				if( swMyTicket == 0xFFFFFFFFu )
				{
					SwContribDiag( 11u );			// ticket denied: exact plain walk while the cell builds
					recordSlot = -1;
				}
			}
		}
		// probe-outcome classification (plain-walk fall-throughs only; serve/record paths excluded):
		// budget-blocked = the claim rate limiter, probe-exhausted = table congestion. One wave-
		// aggregated atomic per class - readback is one-shot, deltas taken CPU-side.
		if( servedBase < 0 && recordSlot < 0 )
		{
			if( swDgBudget )
			{
				SwContribDiag( 7u );
			}
			else if( swDgUnserv )
			{
				SwContribDiag( 9u );
			}
			else if( !swDgKey )
			{
				SwContribDiag( 8u );
			}
		}

		if( servedBase >= 0 )
		{
			// ---- SERVE: recorded static union + live dynamic casters, one Fubini grid ----
			InterlockedAdd( u_Contrib[ 1 ], 1u );					// stat: serve hit
			softFrame_t swFC = SoftShadow_Frame( swP, g_lightR.xyz );
			const float swRC = max( g_lightR.w, 1e-2 );
			uint swGrid[SW_SCAN_CHORDS];
			uint swDiskMask[SW_SCAN_CHORDS];
			int  swDiskBits = 0;
			[unroll] for( int gm = 0; gm < SW_SCAN_CHORDS; gm++ )
			{
				swGrid[gm] = 0u;
				swDiskMask[gm] = SoftScan_Run( -SW_SCAN_HC[gm], SW_SCAN_HC[gm] );
				swDiskBits += SoftPopcount32( swDiskMask[gm] );
			}
			// BIT-PARITY RULE: every path must FillTri exactly the tris the plain walk would - the
			// per-tri cone cull is only approximately conservative at float boundaries, so a path
			// that skips (or adds) the cull flips single penumbra-edge bits vs the plain frames
			// (measured: 1-5 px TEMPORAL defects per light at the 1e-6 gate tolerance).
			// UMBRA EARLY-OUT, exactly the plain list walker's (softwedge_coverage.inc.hlsl): once the
			// OR-union covers >=99% of the disk no later occluder can reduce it (monotone), and the tail
			// rounds to full coverage anyway - so stop walking. Without it every umbra serve walked ALL
			// union entries + the whole dynamic scan while the plain walk it replaced exited after a few
			// tris: measured serves 3.6M/frame costing term 46.8 -> 72.0 ms (the serve-loses-to-plain
			// economics). break (not return) so serve-verify still compares.
			bool swUmbE = false;
			int  swUCnt = 0, swUFill = 0, swDFill = 0;			// serve-work attribution (diag sums)
			[loop] for( int ls = 0; ls < servedCount; ls++ )
			{
				const uint seEnc = u_Contrib[ servedBase + ls ];	// 1-BASED entries: 0 = a concurrent
				if( seEnc == 0u )									// append reserved this slot but has
				{													// not stored yet - skip, never read torn
					continue;
				}
				swUCnt++;
				const int se = ( int )( seEnc - 1u );
				const int b = g_range.x + se * 3;
				const float4 r0 = t_SoftEdges[ b + 0 ];
				const float4 r1 = t_SoftEdges[ b + 1 ];
				const float4 r2 = t_SoftEdges[ b + 2 ];
				const float3 tc = ( r0.xyz + r1.xyz + r2.xyz ) * ( 1.0f / 3.0f );
				const float3 rcv = tc - swP;
				const float  cdv = dot( rcv, swFC.nrm );
				const float  trv = r1.w;
				if( cdv + trv < SW_NEAR_EPS ) { continue; }
				if( cdv - trv > swFC.distPL ) { continue; }
				const float3 ppv = rcv - cdv * swFC.nrm;
				const float  crv = swRC * ( cdv + trv ) / swFC.distPL;
				if( dot( ppv, ppv ) > ( crv + trv ) * ( crv + trv ) ) { continue; }
				swUFill++;
				SoftScan_FillTri( swGrid, r0.xyz, r1.xyz, r2.xyz, swP, swFC, swRC, SW_NEAR_EPS );
				int swCovE = 0;
				[unroll] for( int fm = 0; fm < SW_SCAN_CHORDS; fm++ )
				{
					swCovE += SoftPopcount32( swGrid[fm] & swDiskMask[fm] );
				}
				if( swCovE * 100 >= swDiskBits * 99 )
				{
					swUmbE = true;
					break;
				}
			}
			// dynamic casters via THIS fragment's TILE LIST, dynamic tris only (se >= first dynamic
			// tri) - the surf cache measured the untiled all-caster suffix costing a HIT more than a
			// tiled MISS on dynamics-heavy scenes (the RoE intro), and the same held here (probe:
			// term 30.9 -> 30.2 only). Serving requires a NORMAL tile list; spill/corrupt/untiled
			// fragments never reach here (the serve guard below routed them to the plain walk).
			const int swDynFirstTri = ( g_surfA.z < g_range.y ) ? ( int )t_SoftEdges[ g_flags.y + g_surfA.z * 2 + 1 ].x : 0x7FFFFFFF;
			// ALL-STATIC light: no dynamic suffix exists, skip both dynamic scans outright - the spill
			// cluster scan alone (2 reads x every cluster, all filtered out) measured cap0007 term
			// 41.1 -> 47.4 ms once spill tiles started serving.
			const bool swHasDyn = swDynFirstTri != 0x7FFFFFFF;
			[loop] for( int ld2 = 0; swHasDyn && !swUmbE && ld2 < swServeListCount; ld2++ )
			{
				const int dt = ( int )t_SoftTiles[ swServeListBase + ld2 ];
				if( dt < swDynFirstTri )
				{
					continue;						// static tri: served from the recorded union above
				}
				const int b = g_range.x + dt * 3;
				const float4 r0 = t_SoftEdges[ b + 0 ];
				const float4 r1 = t_SoftEdges[ b + 1 ];
				const float4 r2 = t_SoftEdges[ b + 2 ];
				const float3 tc = ( r0.xyz + r1.xyz + r2.xyz ) * ( 1.0f / 3.0f );
				const float3 rcv = tc - swP;
				const float  cdv = dot( rcv, swFC.nrm );
				const float  trv = r1.w;
				if( cdv + trv < SW_NEAR_EPS ) { continue; }
				if( cdv - trv > swFC.distPL ) { continue; }
				const float3 ppv = rcv - cdv * swFC.nrm;
				const float  crv = swRC * ( cdv + trv ) / swFC.distPL;
				if( dot( ppv, ppv ) > ( crv + trv ) * ( crv + trv ) ) { continue; }
				swDFill++;
				SoftScan_FillTri( swGrid, r0.xyz, r1.xyz, r2.xyz, swP, swFC, swRC, SW_NEAR_EPS );
				int swCovD = 0;
				[unroll] for( int fm2 = 0; fm2 < SW_SCAN_CHORDS; fm2++ )
				{
					swCovD += SoftPopcount32( swGrid[fm2] & swDiskMask[fm2] );
				}
				if( swCovD * 100 >= swDiskBits * 99 )
				{
					break;							// umbra early-out, same rule as the union loop
				}
			}
			// SPILL-tile dynamics: same dynamic-only filter through the cluster hierarchy. Clusters
			// are per-caster contiguous tri runs and a caster is wholly static or wholly dynamic, so
			// firstTri >= dynFirstTri identifies dynamic clusters exactly.
			[loop] for( int sc2 = 0; swHasDyn && !swUmbE && sc2 < swSpillN; sc2++ )
			{
				const int se2 = ( int )t_SoftTiles[ swSpillBase + sc2 ];	// ABSOLUTE cluster-record offset
				const float4 q1 = t_SoftEdges[ se2 + 1 ];					// ( firstTri, numTris, 0, 0 )
				if( ( int )q1.x < swDynFirstTri )
				{
					continue;						// static cluster: served from the recorded union
				}
				const float4 q0 = t_SoftEdges[ se2 + 0 ];					// ( centre.xyz, radius )
				{
					const float3 crc = q0.xyz - swP;
					const float  ccd = dot( crc, swFC.nrm );
					if( ccd + q0.w < SW_NEAR_EPS ) { continue; }
					if( ccd - q0.w > swFC.distPL ) { continue; }
					const float3 cpp = crc - ccd * swFC.nrm;
					const float  ccr = swRC * ( ccd + q0.w ) / swFC.distPL;
					if( dot( cpp, cpp ) > ( ccr + q0.w ) * ( ccr + q0.w ) ) { continue; }
				}
				const int sTriEnd = ( int )q1.x + ( int )q1.y;
				[loop] for( int st = ( int )q1.x; st < sTriEnd; st++ )
				{
					const int b = g_range.x + st * 3;
					const float4 r0 = t_SoftEdges[ b + 0 ];
					{
						// the plain spill walker's COARSE v0-radius reject, mirrored in order (bit parity)
						const float3 rc0 = r0.xyz - swP;
						const float  cd0 = dot( rc0, swFC.nrm );
						if( cd0 + r0.w < SW_NEAR_EPS ) { continue; }
						if( cd0 - r0.w > swFC.distPL ) { continue; }
						const float3 pp0 = rc0 - cd0 * swFC.nrm;
						const float  cr0 = swRC * ( cd0 + r0.w ) / swFC.distPL;
						if( dot( pp0, pp0 ) > ( cr0 + r0.w ) * ( cr0 + r0.w ) ) { continue; }
					}
					const float4 r1 = t_SoftEdges[ b + 1 ];
					const float4 r2 = t_SoftEdges[ b + 2 ];
					const float3 tc = ( r0.xyz + r1.xyz + r2.xyz ) * ( 1.0f / 3.0f );
					const float3 rcv = tc - swP;
					const float  cdv = dot( rcv, swFC.nrm );
					const float  trv = r1.w;
					if( cdv + trv < SW_NEAR_EPS ) { continue; }
					if( cdv - trv > swFC.distPL ) { continue; }
					const float3 ppv = rcv - cdv * swFC.nrm;
					const float  crv = swRC * ( cdv + trv ) / swFC.distPL;
					if( dot( ppv, ppv ) > ( crv + trv ) * ( crv + trv ) ) { continue; }
					swDFill++;
					SoftScan_FillTri( swGrid, r0.xyz, r1.xyz, r2.xyz, swP, swFC, swRC, SW_NEAR_EPS );
					int swCovS2 = 0;
					[unroll] for( int fm3 = 0; fm3 < SW_SCAN_CHORDS; fm3++ )
					{
						swCovS2 += SoftPopcount32( swGrid[fm3] & swDiskMask[fm3] );
					}
					if( swCovS2 * 100 >= swDiskBits * 99 )
					{
						swUmbE = true;
						break;
					}
				}
			}
			// serve-work attribution (one-shot readback deltas, one atomic per wave per word)
			SwContribDiagSum( 12u, ( uint )swUCnt );
			SwContribDiagSum( 13u, ( uint )swUFill );
			SwContribDiagSum( 14u, ( uint )swDFill );
			if( swUmbE )
			{
				SwContribDiag( 15u );
			}
			int swCov = 0;
			[unroll] for( int cm = 0; cm < SW_SCAN_CHORDS; cm++ )
			{
				swCov += SoftPopcount32( swGrid[cm] & swDiskMask[cm] );
			}
			// match the plain walk's >=99% umbra rounding EXACTLY (it returns coverage 1.0 there);
			// without it plain-vs-cached frames flicker the last coverage bit (TEMPORAL defects)
			if( swCov * 100 >= swDiskBits * 99 )
			{
				swCov = swDiskBits;
			}
			const float swTermServe = 1.0 - saturate( swDiskBits > 0 ? ( float )swCov / ( float )swDiskBits : 0.0 );
			if( g_surfParams.y != 0.0f )
			{
				// SERVE-VERIFY (r_softShadowContribCache 2): run the PLAIN walk for the same fragment,
				// tally mismatches into the table header ([4] compares, [5] mismatches, [6] max |diff|
				// as float bits via InterlockedMax - monotonic for non-negative floats), and RENDER the
				// plain term. Millions of deterministic comparisons per run - the cache's self-check.
				float swOccP;
				const int swVTx = px.x / SW_TILE_SIZE - g_tile.x;
				const int swVTy = px.y / SW_TILE_SIZE - g_tile.y;
				if( g_range.z >= 0 && swVTx >= 0 && swVTy >= 0 )
				{
					const int  swVSlot = g_range.z + ( swVTy * g_range.w + swVTx ) * ( g_flags.w + 1 );
					uint swVCnt = t_SoftTiles[ swVSlot ];
					if( swVCnt > ( uint )g_flags.w && swVCnt < SW_TILE_SPILL )
					{
						swVCnt = 0xFFFFFFFFu;
					}
					if( swVCnt == SW_TILE_SPILL )
					{
						const uint swVOfs = t_SoftTiles[ swVSlot + 1 ];
						const uint swVSpN = min( t_SoftTiles[ swVSlot + 2 ], 65536u );
						swOccP = SoftShadow_FaceCoverageClusterList( swP, g_lightR.xyz, swRC, g_range.x, ( int )swVOfs, ( int )swVSpN, swRotAng );
					}
					else if( swVCnt != 0xFFFFFFFFu )
					{
						swOccP = SoftShadow_FaceCoverageList( swP, g_lightR.xyz, swRC, g_range.x, swVSlot + 1, ( int )swVCnt, swRotAng );
					}
					else
					{
						swOccP = SoftShadow_Coverage( swP, g_lightR.xyz, swRC, g_range.x, g_flags.y, g_range.y, 0.0, true, swRotAng );
					}
				}
				else
				{
					swOccP = SoftShadow_Coverage( swP, g_lightR.xyz, swRC, g_range.x, g_flags.y, g_range.y, 0.0, true, swRotAng );
				}
				const float swTermPlain = 1.0 - saturate( swOccP );
				InterlockedAdd( u_Contrib[ 4 ], 1u );
				if( abs( swTermPlain - swTermServe ) > 1e-7f )
				{
					InterlockedAdd( u_Contrib[ 5 ], 1u );
					uint swDm;
					InterlockedMax( u_Contrib[ 6 ], asuint( abs( swTermPlain - swTermServe ) ), swDm );
					// mismatch attribution: direction + fragment class (the debugging split)
					InterlockedAdd( u_Contrib[ swTermServe < swTermPlain ? 16 : 17 ], 1u );
					InterlockedAdd( u_Contrib[ ( swServeListCount < 0 ) ? 18 : 19 ], 1u );
				}
				u_Term[ uint2( px + g_tile.zw ) ] = swTermPlain;
				return;
			}
			u_Term[ uint2( px + g_tile.zw ) ] = swTermServe;
			return;
		}

		if( recordSlot >= 0 )
		{
			// ---- RECORD: this fragment's TILE LIST walked with per-triangle SOLO contribution tests
			// (the validated recording signal; delta-recording measured lossy). The tile list is the
			// shipped-exact triangle set for this fragment (the plain walk uses nothing else), so it
			// finds exactly this fragment's contributors at ~1/10th the cost of the old full-caster
			// walk - which made the 1-in-64 refinement validator alone cost +37 ms/frame (measured:
			// refine OFF dropped term 70.6 -> 33.5 ms). Ticketing requires a normal tile list, so
			// swServeListCount >= 0 here always. Records STATIC tris only (index below the first
			// dynamic tri); the fragment's own term comes from the same grid - one pass.
			InterlockedAdd( u_Contrib[ 2 ], 1u );					// stat: recording eval
			bool swAppendedNew = false;								// did this eval extend the union?
			const int dynFirstTri = ( g_surfA.z < g_range.y ) ? ( int )t_SoftEdges[ g_flags.y + g_surfA.z * 2 + 1 ].x : 0x7FFFFFFF;
			softFrame_t swFC = SoftShadow_Frame( swP, g_lightR.xyz );
			const float swRC = max( g_lightR.w, 1e-2 );
			uint swGrid[SW_SCAN_CHORDS];
			uint swDiskMask[SW_SCAN_CHORDS];
			int  swDiskBits = 0;
			[unroll] for( int gm = 0; gm < SW_SCAN_CHORDS; gm++ )
			{
				swGrid[gm] = 0u;
				swDiskMask[gm] = SoftScan_Run( -SW_SCAN_HC[gm], SW_SCAN_HC[gm] );
				swDiskBits += SoftPopcount32( swDiskMask[gm] );
			}
			[loop] for( int rl2 = 0; rl2 < swServeListCount; rl2++ )
			{
				const int rt = ( int )t_SoftTiles[ swServeListBase + rl2 ];
				{
					const int b = g_range.x + rt * 3;
					const float4 r0 = t_SoftEdges[ b + 0 ];
					const float4 r1 = t_SoftEdges[ b + 1 ];
					const float4 r2 = t_SoftEdges[ b + 2 ];
					// the SAME per-tri cone cull as the plain walk (bit-parity rule, see the serve path)
					const float3 tc = ( r0.xyz + r1.xyz + r2.xyz ) * ( 1.0f / 3.0f );
					const float3 rcv = tc - swP;
					const float  cdv = dot( rcv, swFC.nrm );
					const float  trv = r1.w;
					if( cdv + trv < SW_NEAR_EPS ) { continue; }
					if( cdv - trv > swFC.distPL ) { continue; }
					const float3 ppv = rcv - cdv * swFC.nrm;
					const float  crv = swRC * ( cdv + trv ) / swFC.distPL;
					if( dot( ppv, ppv ) > ( crv + trv ) * ( crv + trv ) ) { continue; }
					uint solo[SW_SCAN_CHORDS];
					[unroll] for( int sm = 0; sm < SW_SCAN_CHORDS; sm++ )
					{
						solo[sm] = 0u;
					}
					SoftScan_FillTri( solo, r0.xyz, r1.xyz, r2.xyz, swP, swFC, swRC, SW_NEAR_EPS );
					// record on ANY RAW solo bits, not only in-disk-mask bits: a tri whose intervals
					// land just outside the disk at every evaluated pixel becomes a contributor under
					// a sub-pixel view displacement - the dominant frozen-union CONTINUITY class
					// (gate: 2717 defects evidence-only vs 52 with all serves re-validated)
					uint soloRaw = 0u;
					[unroll] for( int sm2b = 0; sm2b < SW_SCAN_CHORDS; sm2b++ )
					{
						soloRaw |= solo[sm2b];
						swGrid[sm2b] |= solo[sm2b];
					}
					if( soloRaw != 0u && rt < dynFirstTri )
					{
						// ENTRIES ARE 1-BASED (rt+1): 0 marks a reserved-but-unstored slot, so concurrent
						// serves can never read a torn entry (they skip 0s).
						if( SwContribAppend( ( uint )recordSlot, rt ) )
						{
							swAppendedNew = true;
						}
					}
				}
			}
			// SPILL-tile record: the same solo-contribution walk through the cluster hierarchy (the
			// heavy caps are spill-dominated; without this the cache never engages exactly where the
			// walk is dearest). Mirrors the plain spill walker's cluster + coarse + tight culls.
			[loop] for( int sr = 0; sr < swSpillN; sr++ )
			{
				const int se3 = ( int )t_SoftTiles[ swSpillBase + sr ];		// ABSOLUTE cluster-record offset
				const float4 q0 = t_SoftEdges[ se3 + 0 ];					// ( centre.xyz, radius )
				{
					const float3 crc = q0.xyz - swP;
					const float  ccd = dot( crc, swFC.nrm );
					if( ccd + q0.w < SW_NEAR_EPS ) { continue; }
					if( ccd - q0.w > swFC.distPL ) { continue; }
					const float3 cpp = crc - ccd * swFC.nrm;
					const float  ccr = swRC * ( ccd + q0.w ) / swFC.distPL;
					if( dot( cpp, cpp ) > ( ccr + q0.w ) * ( ccr + q0.w ) ) { continue; }
				}
				const float4 q1 = t_SoftEdges[ se3 + 1 ];					// ( firstTri, numTris, 0, 0 )
				const int sTriEnd2 = ( int )q1.x + ( int )q1.y;
				[loop] for( int rt2 = ( int )q1.x; rt2 < sTriEnd2; rt2++ )
				{
					const int b = g_range.x + rt2 * 3;
					const float4 r0 = t_SoftEdges[ b + 0 ];
					{
						// plain spill walker's COARSE v0-radius reject, in order (bit parity)
						const float3 rc0 = r0.xyz - swP;
						const float  cd0 = dot( rc0, swFC.nrm );
						if( cd0 + r0.w < SW_NEAR_EPS ) { continue; }
						if( cd0 - r0.w > swFC.distPL ) { continue; }
						const float3 pp0 = rc0 - cd0 * swFC.nrm;
						const float  cr0 = swRC * ( cd0 + r0.w ) / swFC.distPL;
						if( dot( pp0, pp0 ) > ( cr0 + r0.w ) * ( cr0 + r0.w ) ) { continue; }
					}
					const float4 r1 = t_SoftEdges[ b + 1 ];
					const float4 r2 = t_SoftEdges[ b + 2 ];
					const float3 tc = ( r0.xyz + r1.xyz + r2.xyz ) * ( 1.0f / 3.0f );
					const float3 rcv = tc - swP;
					const float  cdv = dot( rcv, swFC.nrm );
					const float  trv = r1.w;
					if( cdv + trv < SW_NEAR_EPS ) { continue; }
					if( cdv - trv > swFC.distPL ) { continue; }
					const float3 ppv = rcv - cdv * swFC.nrm;
					const float  crv = swRC * ( cdv + trv ) / swFC.distPL;
					if( dot( ppv, ppv ) > ( crv + trv ) * ( crv + trv ) ) { continue; }
					uint solo2[SW_SCAN_CHORDS];
					[unroll] for( int sm3 = 0; sm3 < SW_SCAN_CHORDS; sm3++ )
					{
						solo2[sm3] = 0u;
					}
					SoftScan_FillTri( solo2, r0.xyz, r1.xyz, r2.xyz, swP, swFC, swRC, SW_NEAR_EPS );
					uint soloRaw2 = 0u;								// ANY raw bits (see the tile-list record)
					[unroll] for( int sm4 = 0; sm4 < SW_SCAN_CHORDS; sm4++ )
					{
						soloRaw2 |= solo2[sm4];
						swGrid[sm4] |= solo2[sm4];
					}
					if( soloRaw2 != 0u && rt2 < dynFirstTri )
					{
						if( SwContribAppend( ( uint )recordSlot, rt2 ) )
						{
							swAppendedNew = true;
						}
					}
				}
			}
			// FINALIZE. Every completed record marks its TILE-COVERAGE bit (appends are done in
			// program order for this thread; cross-thread visibility is the same relaxed model the
			// rest of the protocol uses - a same-dispatch under-covered serve is transient and the
			// refinement validator converges it). Ticketed evaluators (swMyTicket valid) counted
			// themselves via the pre-walk CAS; EXACTLY the holder of ticket K'-1 flips BUILT.
			// Refinement fragments (no ticket, BUILT cell, tile already covered) resolve the
			// revoke/converge outcome - and ONLY they: a ticketed straggler or a tile-miss fill-in
			// legitimately appends (poisoning on those would revoke every freshly built cell /
			// every newly joined tile).
			// ONLY a tile-miss record certifies its tile: it is a FULL-TILE evaluation (every fragment
			// of the tile records that dispatch), so the union provably holds that tile's contributors.
			// Warm-phase ticket winners must NOT set bits - K' wave-order tickets cluster, and a
			// shadow-boundary tile certified by K' lit fragments served an EMPTY union to its shadowed
			// pixels (the persistent max|diff|=1.0 verify class).
			if( swTileMissRec )
			{
				// PEND (never LIVE directly): stamp PENDFRAME FIRST so concurrent probers stop
				// promoting this frame, THEN pend the bit - the promotion in a later frame makes
				// it servable only after every recorder of this dispatch has finalized its appends
				uint swTbDummy;
				InterlockedExchange( u_Contrib[ ( uint )recordSlot + SW_CONTRIB_TILEPFR ], ( uint )g_surfA.y & 0x7FFFu, swTbDummy );
				InterlockedOr( u_Contrib[ ( uint )recordSlot + SW_CONTRIB_TILEPEND ], swTileBit, swTbDummy );
			}
			const uint swEvNow = u_Contrib[ ( uint )recordSlot + 2 ];
			if( ( swEvNow & SW_CONTRIB_BUILT ) != 0u && swMyTicket == 0xFFFFFFFFu && !swTileMissRec )
			{
				// REFINEMENT outcome on a BUILT cell: a NEW contributor means the served union was
				// incomplete for this view - REVOKE serving (POISON) until a later eval converges
				// (appends nothing new), which restores BUILT service with a fresh flip stamp.
				if( swAppendedNew )
				{
					uint pz2;
					InterlockedOr( u_Contrib[ ( uint )recordSlot + 2 ], SW_CONTRIB_POISON, pz2 );
				}
				else if( ( swEvNow & SW_CONTRIB_POISON ) != 0u
						 && u_Contrib[ ( uint )recordSlot + 3 ] <= ( uint )SW_CONTRIB_K )
				{
					// converged: this full eval found every contributor already recorded
					uint pz3;
					InterlockedAnd( u_Contrib[ ( uint )recordSlot + 2 ], ~SW_CONTRIB_POISON, pz3 );
				}
				int swCovS = 0;
				[unroll] for( int cs2 = 0; cs2 < SW_SCAN_CHORDS; cs2++ )
				{
					swCovS += SoftPopcount32( swGrid[cs2] & swDiskMask[cs2] );
				}
				if( swCovS * 100 >= swDiskBits * 99 )
				{
					swCovS = swDiskBits;		// plain-walk umbra rounding, matched exactly
				}
				u_Term[ uint2( px + g_tile.zw ) ] = 1.0 - saturate( swDiskBits > 0 ? ( float )swCovS / ( float )swDiskBits : 0.0 );
				return;
			}
			if( swMyTicket != 0xFFFFFFFFu
					&& swMyTicket + 1u == ( uint )g_surfCost.z
					&& u_Contrib[ ( uint )recordSlot + 3 ] <= ( uint )SW_CONTRIB_K )
			{
				uint dummy;
				// stamp the flip FRAME (bits 16-30) with the BUILT bit so serving defers one frame
				// (no pool release: the claim counter is per-frame now and resets on the CPU)
				InterlockedOr( u_Contrib[ ( uint )recordSlot + 2 ], SW_CONTRIB_BUILT | ( ( ( uint )g_surfA.y & 0x7FFFu ) << 16 ), dummy );
			}
			int swCovR = 0;
			[unroll] for( int cr = 0; cr < SW_SCAN_CHORDS; cr++ )
			{
				swCovR += SoftPopcount32( swGrid[cr] & swDiskMask[cr] );
			}
			if( swCovR * 100 >= swDiskBits * 99 )
			{
				swCovR = swDiskBits;			// plain-walk umbra rounding, matched exactly
			}
			u_Term[ uint2( px + g_tile.zw ) ] = 1.0 - saturate( swDiskBits > 0 ? ( float )swCovR / ( float )swDiskBits : 0.0 );
			return;
		}
		}	// keyLo != sentinel
		}	// cell coords in range
	}
#endif	// SW_CONTRIB_CACHE

#if SW_SURF_CACHE
	// SURFACE-FOLD CACHE probe: world-anchored texel lookup. BUILT texel -> term = 1 - saturate(
	// bilerp of the 4 corner F values [the folded static part] + exact walk of the texel's residual
	// static occluders and the frame's dynamic casters ). Anything else (miss / requested / walk-
	// always / out of key range / table full) falls through to the shipped exact walk; a fresh miss
	// additionally claims a slot and enqueues it for the budgeted build CS. Races here are benign by
	// design: the worst outcome is a duplicate slot for a key or a lost request - both only cost a
	// texel staying on the exact miss path (see the probe plan noble-sniffing-rose).
	// per-frame path counters ride the 4 words after the table (base = tableCap*8):
	// 0 = cached-hit, 1 = miss (no/unbuilt record), 2 = walk-always, 3 = anchor-reject.
	// Cleared by EndBuilds each frame; read by the HUD / bench. Wave-aggregated (SwSurfStat, defined at
	// global scope): one atomic per wave per class instead of one per fragment - the per-fragment
	// InterlockedAdd on a single global word serialised the whole term at low hit rate.
#define SW_SURF_STAT( idx ) SwSurfStat( idx )
	int swVizClass = 1;		// r_softShadowSurfCacheViz 4: path class for the fall-through write (1 miss / 2 walk-always / 3 anchor-reject)
	// COST GATE (attack 1+2, r_softShadowSurfCacheMinCost -> g_surfCost.x): the whole cache machinery (probe,
	// slot claim, build request, hit) only earns its keep on EXPENSIVE tiles - a hit still walks the residual
	// + dynamic, so on a cheap tile the lookup + fall-through costs MORE than just walking. Read this tile's
	// occluder count (the same t_SoftTiles slot the walk uses) and, below the threshold, skip the cache
	// outright: no probe (kills the 86%-overflow waste), no slot claim -> the build budget spends only on the
	// costly tiles. A spilled tile is by definition expensive, so it always caches. 0 = gate off (cache all).
	bool swCostWorth = true;
	if( g_surfCost.x > 0 && g_range.z >= 0 )
	{
		const int swCgTx = px.x / SW_TILE_SIZE - g_tile.x;
		const int swCgTy = px.y / SW_TILE_SIZE - g_tile.y;
		if( swCgTx >= 0 && swCgTy >= 0 && swCgTx < g_range.w )
		{
			const uint swCgCnt = t_SoftTiles[ g_range.z + ( swCgTy * g_range.w + swCgTx ) * ( g_flags.w + 1 ) ];
			swCostWorth = ( swCgCnt == SW_TILE_SPILL ) || ( swCgCnt >= ( uint )g_surfCost.x && swCgCnt < SW_TILE_SPILL );
		}
	}
	if( g_surfA.x > 0 && g_surfA.z > 0 && swPos.w != 0.0f && swCostWorth )
	{
		// GEOMETRIC dominant axis from softpos.normal.w - NOT the normal-mapped shading .xyz. The prewarm
		// SEED keys texels off the flat GEOMETRIC triangle normal; deriving the key axis here from the bumped
		// shading normal disagreed on every normal-mapped surface whose bump flips the dominant axis, so the
		// warm read missed (measured hit ~4% -> the empty-slot majority). softpos now writes the flat
		// geometric dominant axis (ddx/ddy of world pos, same tie-break) into .w, so seed and read keys align.
		const int   d  = ( int )( t_WorldNormal.Load( int3( px, 0 ) ).w + 0.5 );
		const uint  axis = ( uint )d;
		// tangent-plane axes per dominant axis: d=0 -> (u,v)=(y,z), d=1 -> (z,x), d=2 -> (x,y)
		// (explicit selects, no dynamic vector subscripts; MUST match softsurf_build.cs.hlsl)
		const float g  = g_surfParams.x;
		const uint curGen = ( uint )( g_aa.y + 0.5f );	// per-light generation: a set change bumps it,
		// orphaning THIS light's stale slots without wiping the whole table (other lights untouched)
		const float pu = ( d == 0 ) ? swP.y : ( ( d == 1 ) ? swP.z : swP.x );
		const float pv = ( d == 0 ) ? swP.z : ( ( d == 1 ) ? swP.x : swP.y );
		const float pw = ( d == 0 ) ? swP.x : ( ( d == 1 ) ? swP.y : swP.z );
		const int   cu = ( int )floor( pu / g ) + 32768;
		const int   cv = ( int )floor( pv / g ) + 32768;
		const int   cw = ( int )floor( pw / g ) + 32768;
		if( cu >= 0 && cu <= 65535 && cv >= 0 && cv <= 65535 && cw >= 0 && cw <= 65535 )
		{
			const uint keyLo = ( uint )cu | ( ( uint )cv << 16 );
			const uint keyHi = ( uint )cw | ( axis << 16 ) | ( ( uint )g_surfA.w << 19 );
			uint swStatIdx = 1u;		// default: MISS (unbuilt/absent); overridden on the other paths
			uint swMissR = 3u;			// MISS sub-reason (instrument): 0 stale-gen, 1 requested-unbuilt, 2 empty-slot, 3 probe-overflow (key not found in 16 slots) - default 3 (loop fell through)
			// DEBUG probe-tax isolation (r_softShadowSurfCacheForceWalk -> g_aa.z): 2 = SKIP the probe loop
			// entirely (gated-fragment SETUP still ran: normal Load + axis + key), fall straight to the walk
			// -> measures the SETUP tax alone. 1 = run the full probe then force the exact walk (see code==2u
			// below) -> measures SETUP + probe-loop. Delta of the two isolates the 16-slot table-probe memory.
			if( keyLo != 0xFFFFFFFFu && g_aa.z < 1.5f )	// the far world-corner cell aliases the empty sentinel: never cached
			{
				const uint capM = ( uint )g_surfA.x - 1u;	// capacity is a power of two (CPU-enforced)
				const uint h = keyLo * 0x9E3779B1u ^ keyHi * 0x85EBCA77u;
				uint slot = h & capM;
				[loop]										// keep the 16-slot probe ROLLED: unrolling it exploded the
				for( int pr = 0; pr < 16; pr++ )		// surf-permutation VGPR count (256 + spill), collapsing occupancy
				{
					const uint sBase = slot * 8u;
					const uint w0 = u_SurfTable[ sBase ];
					if( w0 == keyLo && u_SurfTable[ sBase + 1 ] == keyHi )
					{
						const uint s2 = u_SurfTable[ sBase + 2 ];
						const uint code = s2 & 3u;
						if( ( s2 >> 2u ) != curGen )
						{
							// STALE generation (the light's static set changed). READ-ONLY runtime: do NOT
							// reclaim/re-request here - that was a per-fragment atomic storm on every un-warm
							// texel (the dominant cache overhead). The camera-independent invalidation hook
							// re-warms this light off the burst path; this frame just takes the exact walk.
							swStatIdx = 1u;
							swMissR = 0u;		// stale generation
							break;
						}
						if( code == 2u )	// BUILT: consume
						{
							if( g_aa.z != 0.0f ) { swStatIdx = 2u; break; }	// DEBUG force-walk (g_aa.z==1): probe ran, key found -> take the exact walk instead of the cached hit (probe-tax isolation)
							// ANCHOR-PROXIMITY GUARD: two surfaces inside the same G-height slab share
							// this key (floor + step, tabletop + crate base) and the record was built
							// at the LOWER plane (min-anchor). A fragment far from the built plane
							// consuming it renders the under-geometry shadow onto the surface above -
							// texel-aligned black rectangles (playtest-observed 2026-08-20). Serve the
							// cache only near the built plane; everything else takes the exact walk.
							// The band is 0.0625*G ~= 0.5 world units (G=8): tight enough that surfaces
							// only ~0.16-2 units apart no longer alias (MEASURED 2026-08-22: the old
							// 0.25*G = 2u let a grazing floor pair alias -> the cached record UNDER-shadowed
							// a whole penumbra band, gate EXTENT=7 on erebus1_14 L1), and it also rejects a
							// receiver whose height varies > ~0.5u across the texel (tilt/curve), where the
							// flat-plane F is unsound. Flat receivers stay on the plane and still hit.
							const uint aEnc = u_SurfTable[ sBase + 7 ];
							const float aH = asfloat( ( aEnc & 0x80000000u ) ? ( aEnc ^ 0x80000000u ) : ~aEnc );
#if SW_SURF_GRID
							// GRID mode: the bit-grid represents tilt/curve as bits directly (no flat-plane F),
							// so the tilt/curve half of the guard's job is gone - a fragment up to ~1 texel above
							// the min-anchor is the SAME tilted surface and the center-built grid is method-A valid.
							// Only the genuine floor+step distinct-surface alias (two surfaces sharing the G-slab
							// key) must still reject; widen the band to ~1 texel so tilt/curve stops walking.
							// (measure-first 2026-08-23: 56% anchor-rej at 0.0625*g was almost all tilt; gate arbiter
							// catches any floor+step alias that leaks through the wider band.)
							const float swAnchBand = g * 0.0625f;
#else
							const float swAnchBand = g * 0.0625f;
#endif
							if( abs( pw - aH ) > swAnchBand )
							{
								swStatIdx = 3u;	// ANCHOR-REJECT: wrong surface for this record
								break;			// exact miss path
							}
							const uint w4 = u_SurfTable[ sBase + 4 ];
							const uint foldedCnt = w4 >> 16;			// high 16: static occluders gridded/folded = the per-texel saving (viz)
#if SW_SURF_GRID
							// GRID hit: load the frozen static bit-grid into swGrid + build this fragment's
							// circular disk mask (fragment-invariant). Dynamic casters OR into swGrid below;
							// coverage = popcount(swGrid & diskMask)/diskBits - the exact Fubini union (no
							// bilinear F, no residual pool; umbra / silhouette / tilt represented as bits).
							uint swGrid[SW_SCAN_CHORDS];
							uint swDiskMask[SW_SCAN_CHORDS];
							int  swDiskBits = 0;
							{
								const uint gBaseR = slot * ( uint )SW_SCAN_CHORDS;
								[unroll] for( int gr = 0; gr < SW_SCAN_CHORDS; gr++ )
								{
									swGrid[gr]     = t_SurfGrid[ gBaseR + gr ];
									swDiskMask[gr] = SoftScan_Run( -SW_SCAN_HC[gr], SW_SCAN_HC[gr] );
									swDiskBits    += SoftPopcount32( swDiskMask[gr] );
								}
							}
#else
							const uint resOfs = u_SurfTable[ sBase + 3 ];
							const uint resCnt = w4 & 0xFFFFu;			// low 16: residual occluders still walked
							const uint f01 = u_SurfTable[ sBase + 5 ];
							const uint f23 = u_SurfTable[ sBase + 6 ];
							const float fu = pu / g - floor( pu / g );
							const float fv = pv / g - floor( pv / g );
							const float fLo = lerp( f16tof32( f01 & 0xFFFFu ), f16tof32( f01 >> 16 ), fu );
							const float fHi = lerp( f16tof32( f23 & 0xFFFFu ), f16tof32( f23 >> 16 ), fu );
							const float fFold = lerp( fLo, fHi, fv );
#endif
							// TILE the hit-path dynamic-caster walk (g_aa.w = r_softShadowSurfCacheTileDyn). The
							// untiled all-caster residual walk re-walked the whole dynamic suffix per fragment and
							// cost a HIT more than a tiled MISS when dynamic casters are many. Pass this fragment's
							// tile list so the residual fn walks only its DYNAMIC tris. -1 = no dynamic / spill /
							// corrupt -> exact untiled fallback.
							int swHitListBase = 0, swHitListCount = -1, swHitDynTri = 0;
							if( g_surfA.z >= g_range.y )
							{
								swHitListCount = 0;		// no dynamic casters: nothing for the dynamic walk
							}
							else
							{
								swHitDynTri = ( int )t_SoftEdges[ g_flags.y + g_surfA.z * 2 + 1 ].x;	// first dynamic tri
								if( g_aa.w != 0.0f )
								{
									const int hTx = px.x / SW_TILE_SIZE - g_tile.x;
									const int hTy = px.y / SW_TILE_SIZE - g_tile.y;
									if( g_range.z >= 0 && hTx >= 0 && hTy >= 0 )
									{
										const int  hSlot = g_range.z + ( hTy * g_range.w + hTx ) * ( g_flags.w + 1 );
										const uint hCnt  = t_SoftTiles[ hSlot ];
										if( hCnt == SW_TILE_UMBRA )
										{
											SW_SURF_STAT( 0u );
											u_Term[ uint2( px + g_tile.zw ) ] = 0.0f;	// whole tile provably umbra (as the miss path)
											return;
										}
										if( hCnt <= ( uint )g_flags.w )		// normal list (exclude spill/corrupt sentinels)
										{
											swHitListBase  = hSlot + 1;
											swHitListCount = ( int )hCnt;
										}
									}
								}
							}
#if SW_SURF_GRID
							// OR this fragment's DYNAMIC casters into the static grid at the true apex P: static
							// bits are frozen at the texel centre (Phase-1 method-A drift), dynamic bits exact at
							// P (contact shadows keep parallax). term = 1 - popcount(grid & diskMask)/diskBits.
							// SPILL / corrupt / tiling-off (swHitListCount < 0): do NOT grid-rasterise here. The
							// bounded dynamic handling already lives in the exact miss walk below - and in THIS
							// (surfgrid) permutation that walk is itself Fubini (SW_SCANLINE=1) with the tile
							// CLUSTER list on spill, so it stays banding-free AND bounded. Grid-rasterising every
							// dynamic caster here was the dominant hit cost (measured: 14% of hits, 63% of the
							// dynamic fill work, ~139 tris/hit). Fall through to it instead.
							if( swHitListCount < 0 )
							{
								swStatIdx = 3u;		// take the exact (Fubini) miss walk for this fragment
								break;
							}
							const softFrame_t swFP = SoftShadow_Frame( swP, g_lightR.xyz );
							const float swRP = max( g_lightR.w, 1e-2 );
							// TILED: OR this tile's DYNAMIC casters into the static grid at the TRUE apex P (contact
							// shadows keep full parallax - filling them at the texel centre wrecks contact/umbra).
							// The aperture shift only reconstructs the STATIC bits; dynamic is small vs a sub-texel
							// aperture shift, so the shared shift is a negligible perturbation on it.
							[loop] for( int ld = 0; ld < swHitListCount; ld++ )
							{
								const int se = ( int )t_SoftTiles[ swHitListBase + ld ];
								if( se < swHitDynTri ) { continue; }		// dynamic tris only (static already gridded)
								const int bd = g_range.x + se * 3;
								SoftScan_FillTri( swGrid, t_SoftEdges[ bd + 0 ].xyz, t_SoftEdges[ bd + 1 ].xyz, t_SoftEdges[ bd + 2 ].xyz, swP, swFP, swRP, SW_NEAR_EPS );
							}
							int swCovG = 0;
							[unroll] for( int cm = 0; cm < SW_SCAN_CHORDS; cm++ ) { swCovG += SoftPopcount32( swGrid[cm] & swDiskMask[cm] ); }
							float swTermC = 1.0 - saturate( swDiskBits > 0 ? ( float )swCovG / ( float )swDiskBits : 0.0 );
#else
							const float occEx = SoftShadow_FaceCoverageSurfResidual(
													swP, g_lightR.xyz, max( g_lightR.w, 1e-2 ), g_range.x,
													( int )resOfs, ( int )resCnt,
													g_flags.y, g_surfA.z, g_range.y,
													swHitListBase, swHitListCount, swHitDynTri, swRotAng );
							float swTermC = 1.0 - saturate( fFold + occEx );
#endif
							const int viz = ( int )g_surfParams.y;
							if( viz == 1 && ( ( ( cu ^ cv ^ cw ) & 1 ) != 0 ) )
							{
								swTermC *= 0.85;	// texel-parity checker: seams/structure inspection
							}
							else if( viz == 2 )
							{
								swTermC *= 0.75;	// uniform: which pixels the cache serves
							}
							else if( viz == 3 )
							{
								// SAVING heatmap: brightness = folded static occluders removed from the walk
								// (per-pixel time saved, in walk-units). 16+ folded -> full bright.
								swTermC = saturate( ( float )foldedCnt * ( 1.0f / 16.0f ) );
							}
							else if( viz == 4 )
							{
								swTermC = 1.0f;		// COST-CLASS: hit -> full bright (non-hit classes tinted at the fall-through write)
							}
							SW_SURF_STAT( 0u );		// cached HIT
							u_Term[ uint2( px + g_tile.zw ) ] = swTermC;
							return;
						}
						if( code == 3u )
						{
							swStatIdx = 2u;			// WALK-ALWAYS (self-gate / overflow rejected the texel)
						}
						else
						{
							swMissR = 1u;			// code 1 REQUESTED: seeded but the build has not filled it yet
						}
						// code 1 (REQUESTED, not yet built by the burst) -> miss. READ-ONLY: no anchor write.
						break;						// requested / walk-always: exact miss path
					}
					if( w0 == 0xFFFFFFFFu )
					{
						uint prev;
						prev = 0u;	// READ-ONLY runtime: never CLAIM an empty texel (the per-fragment claim/enqueue atomics were the dominant low-hit-rate cost); prev=0 skips the claim body -> exact walk. Seeding happens once at load in softsurf_seed (the burst).
						if( prev == 0xFFFFFFFFu )
						{
							u_SurfTable[ sBase + 1 ] = keyHi;
							// anchor: build probes this height plane, EXACT surface height (MEASURED
							// 2026-08-20: snapping to a G/16 quantum exploded the gate 48 -> 667,
							// CONTINUITY 517 - contact shadows are hyper-sensitive to even G/32 of
							// off-surface anchor, same class as the PCSS bias floor). Accumulated via
							// flip-encoded InterlockedMin (cleared slot = +inf), so the anchor is the
							// MIN height over the contributing fragments - claim-race-independent.
							InterlockedMin( u_SurfTable[ sBase + 7 ], SwSurfFlipF( pw ) );
							uint qi;
							qi = 0u;	// read-only term: no request queue (dead claim path, kept for reference)
							if( qi + 1u < ( uint )g_surfA.y )
							{
								qi = qi;	// read-only term: no request queue write (dead path)
								u_SurfTable[ sBase + 2 ] = ( curGen << 2 ) | 1u;	// REQUESTED (gen-tagged)
							}
							else
							{
								// queue full: revert, retry later. The anchor resets FIRST (before the
								// key) - a later different-key claim of this slot Min-accumulates and
								// must not inherit this texel's stale height.
								u_SurfTable[ sBase + 7 ] = 0xFFFFFFFFu;
								u_SurfTable[ sBase ] = 0xFFFFFFFFu;
							}
						}
						swMissR = 2u;				// EMPTY slot: this texel was never seeded/claimed
						break;						// claimed (by us or a racer): exact miss path this frame
					}
					slot = ( slot + 1u ) & capM;	// occupied by another key: linear probe
				}
			}
			SW_SURF_STAT( swStatIdx );			// fall-through: miss / walk-always / anchor-reject
			if( swStatIdx == 1u ) { SwSurfMissReason( swMissR ); }	// instrument: split the 36% miss by cause
			swVizClass = ( int )swStatIdx;
		}
	}
#endif	// SW_SURF_CACHE

	// Mirror of the interaction PS swFace branch (tile-binned walk with bit-exact full-walk
	// fallback). swCentreLit is 0: the face path ignores it (see SoftShadow_Coverage).
	float swOcc;
	// r_softShadowSamples DISABLE (g_aa.x == 0): soft coverage off -> term 1.0 (no penumbra). The actual
	// sample COUNT is the compile-time SW_FACE_SAMPLES of this shader permutation (the pass picks the
	// 8/16/32 variant by the cvar), so the walk below stays the fast fp16 path at whatever count is built.
	if( g_aa.x < 0.5f )
	{
		u_Term[ uint2( px + g_tile.zw ) ] = 1.0;
		return;
	}
	const int swTx = px.x / SW_TILE_SIZE - g_tile.x;
	const int swTy = px.y / SW_TILE_SIZE - g_tile.y;
	if( g_range.z >= 0 && swTx >= 0 && swTy >= 0 )
	{
		const int  swSlot = g_range.z + ( swTy * g_range.w + swTx ) * ( g_flags.w + 1 );	// stride = active tile-K
		uint swCnt  = t_SoftTiles[ swSlot ];
		// HANG-PROOFING: a count word must be a sane count (<= K) or a known sentinel. Anything else
		// is corrupt tile data (a stale/aliased slot) - walking it would loop the GPU into a device
		// reset (observed live: gfx-ring timeout, 10s watchdog). Degrade to the bounded full walk.
		if( swCnt > ( uint )g_flags.w && swCnt < SW_TILE_SPILL )
		{
			swCnt = 0xFFFFFFFFu;
		}
		if( swCnt == SW_TILE_UMBRA )
		{
#if SW_GPU_WALK_COUNTERS
			InterlockedAdd( u_WalkCnt[ 16 ], 1u );	// TILE-CLASS census: umbra-sentinel thread (zero walk iterations)
#endif
			// whole tile provably in umbra: the integral saturates to 1 for every receiver here
			u_Term[ uint2( px + g_tile.zw ) ] = 0.0f;
			return;
		}
		if( swCnt == SW_TILE_SPILL )
		{
#if SW_GPU_WALK_COUNTERS
			InterlockedAdd( u_WalkCnt[ 18 ], 1u );	// TILE-CLASS census: spill-tile thread (cluster walk)
#endif
			// overflowed tile: the span holds this tile's surviving CLUSTER records (stream v3) -
			// the two-level walk amortizes the cone cull ~3.6x exactly where lists are huge
			const uint swOfs = t_SoftTiles[ swSlot + 1 ];
			// span length hard-capped (worst measured tile is ~334 clusters; 64k = deep margin):
			// a corrupt descriptor must degrade to a truncated walk, never a device reset
			const uint swSpN = min( t_SoftTiles[ swSlot + 2 ], 65536u );
			swOcc = SoftShadow_FaceCoverageClusterList( swP, swL, swR, g_range.x, ( int )swOfs, ( int )swSpN, swRotAng );
		}
		else if( swCnt != 0xFFFFFFFFu )
		{
#if SW_GPU_WALK_COUNTERS
			InterlockedAdd( u_WalkCnt[ ( swCnt == 0u ) ? 17 : 19 ], 1u );	// TILE-CLASS census: 17 = empty-list (provably lit, zero iterations), 19 = listed thread (walks swCnt entries)
#endif
			swOcc = SoftShadow_FaceCoverageList( swP, swL, swR, g_range.x, swSlot + 1, ( int )swCnt, swRotAng );
		}
		else
		{
			swOcc = SoftShadow_Coverage( swP, swL, swR, g_range.x, g_flags.y, g_range.y, 0.0, true, swRotAng );
		}
	}
	else
	{
		swOcc = SoftShadow_Coverage( swP, swL, swR, g_range.x, g_flags.y, g_range.y, 0.0, true, swRotAng );
	}

#if SW_SURF_CACHE
	if( ( int )g_surfParams.y == 4 )
	{
		// COST-CLASS heatmap, non-hit fragments: miss 0.25, walk-always 0.5, anchor-reject 0.75
		// (hit is full-bright, tinted on the hit path). Shows WHERE the overhead-payers cluster.
		const float swClassLvl[4] = { 1.0f, 0.25f, 0.5f, 0.75f };
		u_Term[ uint2( px + g_tile.zw ) ] = swClassLvl[ min( swVizClass, 3 ) ];
		return;
	}
#endif
	u_Term[ uint2( px + g_tile.zw ) ] = 1.0 - saturate( swOcc );
}
