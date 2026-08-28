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
#include "softscan_word.inc.hlsl"		// SwGridWord typedef - needed by t_SurfGrid below, before the coverage include
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
// = empty), [1] keyHi, [2] state (1 requested / 2 built / 3 walk-always), [3] residual pool offset
// (1-uint entries), [4] residual count, [5][6] the 4 corner F values as packed fp16, [7] anchor
// (claiming fragment's height along the texel's normal axis, asuint). Queue: [0] = count, then
// claimed slot indices for the build CS. Pool: [0] = alloc counter, then 1-uint residual triangle
// indices into the light's PERSISTENT static stream segment (t_SurfStream; audit finding #4).
RWStructuredBuffer<uint>	u_SurfTable	: register( u2 );
// u_SurfQueue removed: the READ-ONLY runtime term never claims/enqueues (the burst seeds via
// softsurf_seed), so it binds NO request queue. This also keeps the reflected binding layout stable
// across sample-count permutations (a stripped-but-declared u3 desynced the layout and crashed at s32).
#if SW_SURF_GRID
// GRID mode reuses register t6 (the residual pool is excluded in grid mode) for the parallel static
// Fubini grid buffer - so the binding LAYOUT is identical to the scalar surf permutation (u2 table + t6),
// no extra slot to strip/desync. SW_SCAN_CHORDS words per slot.
StructuredBuffer<SwGridWord>	t_SurfGrid	: register( t6 );	// per-chord grid word (uint or uint2, see SW_SCAN_BITS)
#else
StructuredBuffer<uint>		t_SurfPool	: register( t6 );	// read before the include: the residual walk consumes it
// PERSISTENT per-light static caster stream (audit finding #4): every warmed light's [tris][casters]
// float4 stream, appended at a per-light base by SoftShadowSurfCache::WarmLight. The pool's 1-uint
// residual indices resolve here (segment tri base = asuint(g_econ.z), passed to the residual walk).
// Scalar surf permutation only - grid mode has no residual pool and reuses t6 for the grid.
StructuredBuffer<float4>	t_SurfStream	: register( t8 );
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
// evaluate-once-union design (studies: 95% of walk iterations are proven waste). Per (world cell,
// light) the fragments of each screen TILE record their solo-contributing STATIC triangles into
// the cell's slot (the fragments themselves are the evaluation points - no heuristic anywhere);
// once a tile's full-dispatch evaluation is certified, its fragments walk only the recorded union
// + live dynamic casters. Warm-up is INCREMENTAL: cells enter the table only while the per-frame
// claim budget has room (header word 0). All fallbacks (unclaimed / uncertified tile / budget /
// overflow / stale generation) are the exact shipped walk.
// Slot layout (stride SW_CONTRIB_STRIDE uints): [0] keyLo [1] keyHi (cz | light key, GEN-free)
// [2] state (POISON) [3] triCount (advisory max, <= SW_CONTRIB_K) [4..67] recorded GLOBAL tri
// indices (1-based, claim-by-value, contiguous) [68] LIVE tile mask [69] PEND tile mask
// [70] pend frame [71] generation.
// Header (first SW_CONTRIB_HEADER uints): [0] claim budget used this frame; [1..] stats (wave-
// aggregated; see GetContribStats).
#ifndef SW_CONTRIB_CACHE
	#define SW_CONTRIB_CACHE 0
#endif
#if SW_CONTRIB_CACHE
#define SW_CONTRIB_K		64
// v3 protocol (BUILT-at-claim): a CLAIM plants the key + generation and nothing else; the ONLY
// warm path is the TILE-MISS record (a fragment whose tile's LIVE bit is unset - its whole tile
// records that dispatch), and serving is gated purely by the frame-deferred tile certification.
// The v2 eval-ticket/BUILT-flip machinery is gone: it existed only to bound a transient the
// tile-cert one-shot already bounds, and burned a CAS retry loop on every warm fragment.
// TILE-COVERAGE certification, frame-deferred: a record finalize PENDS its tile bit; any prober
// PROMOTES pends to LIVE only in a frame with no record finalize yet (PENDFRAME != now). Trusting
// a bit the frame it was set let late-launching waves serve a union whose recorders were still
// walking (spill records are slow) - measured 4.6%/15.7% serve-verify mismatches.
#define SW_CONTRIB_TILEBIT	( 4 + SW_CONTRIB_K )		// slot word: LIVE tile-coverage mask (serve gate)
#define SW_CONTRIB_TILEPEND	( 4 + SW_CONTRIB_K + 1 )	// slot word: PENDING tile bits (this frame's records)
#define SW_CONTRIB_TILEPFR	( 4 + SW_CONTRIB_K + 2 )	// slot word: frame of the last pend write
#define SW_CONTRIB_GEN		( 4 + SW_CONTRIB_K + 3 )	// slot word: generation (stale slot -> FREE + reclaim)
#define SW_CONTRIB_STRIDE	( 4 + SW_CONTRIB_K + 4 )
// header words 16-19: serve-verify mismatch attribution ([16] serve DARKER = over-coverage,
// [17] serve LIGHTER = under-coverage, [18] on a SPILL fragment, [19] on a tile-list fragment)
#define SW_CONTRIB_HEADER	24
#define SW_CONTRIB_POISON	0x40000000u		// refinement-revoked, or (with triCount >= K) permanent overflow
#define SW_CONTRIB_FREEING	0xFFFFFFFFu		// keyLo sentinel while a stale slot is being cleared
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
// CLAIM-BY-VALUE append of tri rt (stored 1-based) into cell slot sB's union. The entry CAS is
// the publication, so the list is contiguous (scan to the first zero) and only genuinely DISTINCT
// contributors consume capacity - the earlier count-reserve scheme let torn-window duplicate
// reservations from a full-tile dispatch racing the same few contributors spuriously overflow-
// POISON a tiny union (caught by the SoftContribProtocolV3 sim). triCount is an advisory
// InterlockedMax. Returns: 0 = already present, 1 = appended NEW (refinement revoke signal),
// 2 = contention give-up (the caller must NOT certify its tile), 3 = genuine overflow (POISONed).
int SwContribAppend( uint sB, int rt )
{
	const uint val = ( uint )( rt + 1 );
	[loop] for( int aTry = 0; aTry < SW_CONTRIB_K + 16; aTry++ )
	{
		int idx = -1;
		[loop] for( int i = 0; i < SW_CONTRIB_K; i++ )
		{
			const uint e = u_Contrib[ sB + 4u + ( uint )i ];
			if( e == val )
			{
				return 0;								// already recorded
			}
			if( e == 0u )
			{
				idx = i;
				break;
			}
		}
		if( idx < 0 )
		{
			// K DISTINCT contributors exceeded: the union cannot represent this cell - permanent
			// POISON (probe classifies POISON && triCount >= K as plain-walk-forever, no refinement)
			uint pz;
			InterlockedOr( u_Contrib[ sB + 2 ], SW_CONTRIB_POISON, pz );
			u_Contrib[ sB + 3 ] = ( uint )SW_CONTRIB_K;
			return 3;
		}
		uint got;
		InterlockedCompareExchange( u_Contrib[ sB + 4u + ( uint )idx ], 0u, val, got );
		if( got == 0u )
		{
			uint mx;
			InterlockedMax( u_Contrib[ sB + 3 ], ( uint )( idx + 1 ), mx );
			return 1;									// appended new
		}
		if( got == val )
		{
			return 0;									// a racer published our id into that slot
		}
		// slot taken by another id since the scan: rescan (their entry may still be ours upstream)
	}
	return 2;											// contention cap: do not certify this tile
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
// ADAPTIVE SUB-SAMPLING (r_softShadowSubSample, SW_SUBSAMPLE permutation axis): two-phase dispatch.
// Phase A (g_sub.x phase 1) evaluates the light rect's CORNER LATTICE at stride S with the exact
// body, storing ( term, wMin ) per corner; Phase B (phase 2) runs the exact early-outs, then a
// conservative detect-then-refine criterion - C1 receiver continuity, C2 corner-window term spread,
// C3 penumbra-width floor - and bilinearly interpolates the 4 corner terms where all three hold,
// falling through into the unchanged exact walk everywhere else. Cvar 0 = the pre-existing single
// dispatch on the pre-existing pipelines (this axis compiled out).
#ifndef SW_SUBSAMPLE
	#define SW_SUBSAMPLE 0
#endif
#if SW_SUBSAMPLE
	#define SW_SUBSAMPLE_WMIN 1		// enables the C3 wMin expanded-cull sites in the coverage include
// side lattice: [0..3].x = wave-aggregated counters (walked-class, interpolated, refined, lattice
// evals), then latW*latH entries of ( asuint(term), asuint(wMin) ) written by Phase A.
RWStructuredBuffer<uint2>	u_SubLattice	: register( u3 );
#endif
// SW_RADIAL 1 was tested here as an umbra early-out for the wedge term (task #122). REFUTED (RoE intro
// 1080p, com_fixedTic, r_softShadowWedgeAblate 4 / 0): whole-block 22ms->101ms, tile-binned 19ms->87ms.
// The K=32 per-edge radial-bin update (32 sin/cos + cross tests) burned any saturation-early-out win by
// ~4x. Kept at 0 here so the include default (0) drives the wedge term.
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
	float4	g_misc;				// x = r_softShadowMinDnRatio: projection dn-clamp (grazing grain fix).
								// yzw = VIEW ORIGIN (cache-economics tiers 1/3: proximity policy)
	float4	g_econ;				// cache economics (task #112): x = near-exact radius SQUARED
								//   (r_softShadowNearRadius; 0 = off) - fragments closer than this to the
								//   view NEVER use the cache (full accuracy + zero popping where visible);
								//   y = tier-1 serve/walk gate base margin (r_softShadowSurfCacheGateMargin;
								//   0 = off = always serve on hit, today's behavior);
								//   z = THIS light's persistent static-stream segment TRI BASE (float4 elems
								//   into t_SurfStream), BIT-CAST uint (asuint - exact at any size, unlike a
								//   float-rounded lane) - audit finding #4; w reserved.
	uint4	g_areaMask;			// PORTAL-AREA CULL (r_softShadowAreaCull): 128-bit mask, bit areaNum set iff
								//   the light's portal flood reached that area. ALL-ONES = cull inert (cvar
								//   off / unqualified light: conservative, never skips). A world fragment
								//   (normal .w encodes areaP1 = areaNum+1 above the 2-bit axis) in an
								//   unreached area has NO interaction draw under this light - its term is
								//   provably never read, so the walk is skipped (term 1.0).
	float4	g_sub;				// ADAPTIVE SUB-SAMPLING (SW_SUBSAMPLE): x = phase | (viz << 2)
								//   (0 = off/exact, 1 = lattice, 2 = full-rect refine), y = stride S,
								//   z = C2 spread epsilon, w = C3 width-floor beta. Mirrors sub[4].
	int4	g_wedge;			// HYBRID WHITELIST (r_softShadowWedgeWhitelist): x = wedge-block float4
								// element base into t_SoftEdges (< 0 = none this light), y = record count
								// (float4 pairs), z = cvar mode (>= 2 arms SwWlTryServe), w = bound-gap
								// tolerance x10000 (r_softShadowWLGapTol). Caster order == the caster
								// table's. Mirrors wedge[4] in SoftTermCB.
	int4	g_repairBin;		// REPAIR ARBITER (mode 3): x = FACE tile-bin base (float4 elements into
								// t_SoftTiles) for the softrepair arbiter's FaceCoverageList lookup.
								// Shares tilesX / tileOx / tileOy with g_range.w / g_tile.x / g_tile.y.
								// Softterm does not read this; it exists here to keep the CB layout in
								// lockstep with softrepair.cs.hlsl. yzw reserved.
};
// *INDENT-ON*

// TERM-LEVEL DIAGNOSTIC (r_softShadowTermLevels -> g_surfCost.z): quantize the visibility term to
// N levels before it is stored in the R16F atlas. This isolates the ATLAS-STORAGE / term-precision
// hypothesis for the penumbra banding: coarsen N and, if the banding worsens/matches the observed,
// the term's own level count is what shows through (the R16F format holds ~190 Fubini levels, but
// 190 levels can still band on a wide grazing penumbra). 0 = off (full precision, shipped).
float SwTermQuant( float t )
{
	const int lv = g_surfCost.z;
	return ( lv > 0 ) ? ( round( saturate( t ) * ( float )lv ) / ( float )lv ) : t;
}

// C1 LIT-EARLY-OUT HOIST (r_softShadowLitEarlyOut = g_surfParams.z): the skip below the threshold
// writes term 1.0; without a ramp the first non-skipped fragment jumps straight to its full
// occlusion - a visible step along the iso-intensity contour. Scale the OCCLUSION by a
// smoothstep(T, 2T, intensity) factor so coverage fades in continuously from the skip boundary.
// swHoist == 1.0 must return the ORIGINAL value bit-exactly (T=0 stays byte-identical), hence the
// branch instead of the algebraically-equal 1-(1-t), which reassociates the rounding.
float SwHoistTerm( float swTerm, float swHoist )
{
	return ( swHoist < 1.0f ) ? ( 1.0f - swHoist * ( 1.0f - swTerm ) ) : swTerm;
}

// WALK-COST HEATMAP (r_softShadowSurfCacheViz 9): override every term write with this fragment's
// fill-kernel entry count on a LOG ramp (the old linear /256 clamped everything expensive to a
// uniform white, hiding WHICH red region is worst), and band-encode WHY a zero-walk pixel is
// black. Wraps EVERY u_Term write site with the site's termination class; diagnostic only (the
// image is wrong by construction); the bench viz dump captures the PNG and prints this legend.
// LEGEND (single R16F channel -> red PNG):
//   0.00        never written (unpacked atlas space; the atlas is never cleared - a fresh
//               allocation reads 0, so 0 = no dispatch ever touched the texel)
//   0.04        coverage/falloff or N.L early-out               (SWVC_EARLYOUT)
//   0.08        whole-tile umbra sentinel, zero walk            (SWVC_UMBRA)
//   0.12        cache-hit serve with fills == 0                 (SWVC_HIT; a hit that walked
//               residual/dynamic fills has fills > 0 and shows as COST - expensive hits visible)
//   0.16        other zero-fill termination                     (SWVC_OTHER: classifier lit,
//               force-hit/debug, samples-disabled) and a walk whose tile list was empty
//               (SWVC_WALK at fills == 0: it genuinely walked nothing)
//   0.25..1.00  log cost, any class with fills > 0:
//               0.25 + 0.75 * log2(1+fills) / log2(1+SWVC_MAX_FILLS); 1 fill sits visibly
//               above the class bands, SWVC_MAX_FILLS+ saturates to white
#define SWVC_MAX_FILLS	4096.0f
#define SWVC_EARLYOUT	0.04f
#define SWVC_UMBRA		0.08f
#define SWVC_HIT		0.12f
#define SWVC_OTHER		0.16f
#define SWVC_WALK		0.16f
float SwVizCostTerm( float v, float band )
{
	if( ( int )g_surfParams.y != 9 )
	{
		return v;		// viz off: bit-identical passthrough
	}
	return ( g_swVizFills > 0u )
		   ? saturate( 0.25f + 0.75f * log2( 1.0f + ( float )g_swVizFills ) * ( 1.0f / log2( 1.0f + SWVC_MAX_FILLS ) ) )
		   : band;
}

// ---- WEDGE-WHITELIST SERVE (r_softShadowWedgeWhitelist 2 -> g_wedge.z >= 2) -----------------------
// The HYBRID's serving dispatch: serve CLEAN casters by their exact per-caster wedge areas and walk
// ONLY the CUT casters through the scanline, guarded by rigorous bounds. Per fragment:
//   solo_k  = caster k's solo occlusion from ITS wedge span (header + edges; exact when the loop
//             walked CLEAN, i.e. no material bridging connector: (clipOut >> 3) == 0).
//   scanOcc = the EXACT union of the CUT casters (masked caster-table walk).
// The true union U over ALL casters obeys
//   lo = max( scanOcc, max_k clean solo_k ) <= U <= min( 1, scanOcc + Sum_k clean solo_k ) = hi
// (lower bound: each term is the measure of a subset of the union; upper bound: subadditivity).
// Serving lo therefore UNDER-shadows by at most hi - lo, and the fragment is served only when
// hi - lo <= gapTol (r_softShadowWLGapTol, carried as g_wedge.w x10000). Everything uncertain -
// parallax-unsafe fragment, malformed/truncated wedge block, bound gap over tolerance - returns
// false and the caller falls through to the existing walk paths untouched. Wave-coherence: the
// entry conditions are CB-uniform and sinA varies smoothly with receiver depth, so waves branch
// together; only the final serve/fallback verdict diverges per lane.
bool SwWlTryServe( float3 swP, float3 swL, float swR, float swRotAng, float swHoist, out float swTermOut )
{
	swTermOut = 1.0f;
	// (a) PARALLAX GUARD: the wedge picks ONE silhouette at the disk centre; at large disk half-angle
	// (sinA = R / distPL) a deep caster's true contour shifts across the disk and the wedge area is no
	// longer certifiable (measured exact below 0.10 in SoftShadowWedge_test). Full walk instead.
	const float swWlDistPL = length( swL - swP );
	if( swR >= SW_WL_SINA_MAX * swWlDistPL )
	{
		return false;
	}
	// (b) PER-CASTER WEDGE SCAN (the census loop's shape, run unconditionally): caster k's span = its
	// inline header pair (e0.w < 0; e1.y = edgeCount) + edgeCount edge pairs, and the wedge block's
	// caster order EQUALS the caster table's (emit contract), so k indexes both streams.
	float swWlWedgeOcc = 0.0f;					// max over CLEAN casters' solo occlusion (lower-bound term)
	float swWlSumClean = 0.0f;					// Sum over CLEAN casters' solo occlusion (upper-bound term)
	uint2 swWlCutMask = uint2( 0u, 0u );		// bit k set = caster k must WALK (cut / uncertifiable)
	int swWlSe  = 0;
	int swWlCas = 0;
	while( swWlSe < g_wedge.y && swWlCas < g_range.y )
	{
		const float4 swWlH0 = t_SoftEdges[ g_wedge.x + swWlSe * 2 + 0 ];
		const float4 swWlH1 = t_SoftEdges[ g_wedge.x + swWlSe * 2 + 1 ];
		if( swWlH0.w >= 0.0 )
		{
			return false;						// malformed block (a header was due here): never serve from it
		}
		const int swWlCnt = ( int )swWlH1.y;	// this caster's edge-record count
		if( swWlCas >= 64 )
		{
			// past the walk mask's 64 bits: the masked walk ALWAYS walks these casters, so their
			// contribution lands in scanOcc exactly - treat as cut, skip the wedge integral.
			swWlCas++;
			swWlSe += swWlCnt + 1;
			continue;
		}
		if( swWlCnt <= 0 )
		{
			// NO WEDGE FORM (hull/box casters emit an edgeCount-0 header purely for order alignment):
			// an empty span walks to solo 0 WITHOUT a cut flag, which would classify a real occluder
			// as "clean with zero coverage" and erase its shadow (measured: cap0006 L2's box-proxied
			// fixtures, 8 LIT_IN_UMBRA). No wedge form = the scanline must walk it: mark CUT.
			if( swWlCas < 32 ) { swWlCutMask.x |= 1u << ( uint )swWlCas; }
			else               { swWlCutMask.y |= 1u << ( uint )( swWlCas - 32 ); }
			swWlCas++;
			swWlSe += 1;
			continue;
		}
		float swWlGapC = 0.0f;
		int   swWlNCC  = 0;
		int   swWlClipC = 0;
		// walk ONE caster's span so the return IS that caster's solo occlusion and the cut verdict is per caster
		const float swWlSolo = SoftShadow_WedgeOcclusionEx( swP, swL, swR,
										g_wedge.x + swWlSe * 2, swWlCnt + 1, 0.0,
										swWlGapC, swWlNCC, swWlClipC );
		if( ( swWlClipC >> 3 ) > 0 )
		{
			// CUT: a material bridging connector shaped this caster's integral - its area is
			// untrusted (contact-clipped open surface), so the exact scanline walks it instead
			if( swWlCas < 32 ) { swWlCutMask.x |= 1u << ( uint )swWlCas; }
			else               { swWlCutMask.y |= 1u << ( uint )( swWlCas - 32 ); }
		}
		else
		{
			swWlWedgeOcc = max( swWlWedgeOcc, saturate( swWlSolo ) );
			swWlSumClean += saturate( swWlSolo );
		}
		swWlCas++;
		swWlSe += swWlCnt + 1;
	}
	if( swWlCas < g_range.y )
	{
		return false;							// fewer wedge headers than casters: malformed/truncated block
	}
	// (c) EXACT SCANLINE of ONLY the cut casters: the unbinned caster-table walk with the cut mask
	const float swWlScanOcc = SoftShadow_FaceCoverageMasked( swP, swL, swR, g_range.x, g_flags.y,
									g_range.y, swRotAng, swWlCutMask );
	// (d) BOUNDS: true union in [lo, hi]; serve lo (under-shadows by <= gapTol) iff the gap is tight
	const float swWlLo = max( swWlScanOcc, swWlWedgeOcc );
	const float swWlHi = min( 1.0f, swWlScanOcc + swWlSumClean );
	if( swWlHi - swWlLo > ( float )g_wedge.w * ( 1.0f / 10000.0f ) )
	{
		return false;							// bounds too far apart: the full walk must arbitrate
	}
	// served: the same term shape the plain walk returns for a coverage value of lo
	swTermOut = SwVizCostTerm( SwTermQuant( SwHoistTerm( 1.0f - saturate( swWlLo ), swHoist ) ), SWVC_HIT );	// wedge-whitelist serve
	return true;
}

// WEDGE TILE-BIN walk (r_softShadowWedgeWhitelist 3): the tile list holds one entry per caster whose shadow
// reaches THIS tile (softtile_bin's analytic append: 0x80000000 | wedgeHeaderRec, screen-footprint sphere-cone
// culled). Walk ONLY those casters' wedge spans and max-combine - identical to the whole-stream wedge's internal
// max-combine, restricted to the screen-footprint survivors. Lossless: a binner-culled caster's shadow does not
// reach this tile, so it contributed 0 here anyway. listBase/listCnt index t_SoftTiles (inline slots or a spill span).
//
// L1 (task #123): SKIP the caster's header record entirely. The binner already vetted this caster against the
// tile receiver AABB (a strictly tighter cull than the per-fragment sphere test the header would run), so the
// per-fragment SoftShadow_CullCaster call is provably redundant on a listed caster. Pass base at the FIRST EDGE
// (headerRec + 2 float4s past the header pair) and N = edgeCount, not headerRec + (ec+1). The walk's entry state
// (haveCaster=true, swArea=0, swFirstValid=false, havePrevE1=false) is exactly what the header would have set,
// so skipping the header is bit-exact. Saves 1 sqrt + ~14 flops + 32 B load per listed caster per fragment.
float SwWedgeTileOcc( float3 swP, float3 swL, float swR, int listBase, int listCnt )
{
	float occ = 0.0f;
	for( int i = 0; i < listCnt; i++ )
	{
		const uint he   = t_SoftTiles[ listBase + i ] & 0x7FFFFFFFu;	// wedge header record (strip the analytic hi bit)
		const int  base = g_wedge.x + ( int )he * 2;					// float4 base of this caster's header pair
		const int  ec   = ( int )t_SoftEdges[ base + 1 ].y;			// edgeCount (header e1.y)
		// L1: skip the header record - jump base past its 2 float4s, walk only ec edges. Header cull redundant.
		occ = max( occ, SoftShadow_WedgeOcclusion( swP, swL, swR, base + 2, ec, 0.0 ) );
		if( occ >= 0.999f ) { break; }								// saturated umbra: no further caster can raise it
	}
	return occ;
}

#if SW_SUBSAMPLE
// wave-aggregated path counter into u_SubLattice[word].x (SwSurfStat pattern): one atomic per wave
// per class. Words: 0 walked-class (reached the criterion), 1 interpolated, 2 refined, 3 lattice evals.
void SwSubStat( uint word, bool pred )
{
	const uint c = WaveActiveCountBits( pred );
	if( c != 0u && WaveIsFirstLane() )
	{
		InterlockedAdd( u_SubLattice[ word ].x, c );
	}
}
#endif

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
			if( v < 2u )
			{
				// PER-LIGHT hit/miss attribution (bench instrument): 16 rings of {hit,miss} pairs after
				// the 8 global counters, keyed lightKey&15 (collisions acceptable at <=21 term lights).
				// The dispatch is per-light, so g_surfA.w is wave-uniform - one extra atomic per wave.
				InterlockedAdd( u_SurfTable[ ( uint )g_surfA.x * 8u + 8u + ( ( uint )g_surfA.w & 15u ) * 2u + v ], c );
			}
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

// ---- REFACTOR (adaptive sub-sampling): the old main() body, split into PRELUDE + WALK so the
// SW_SUBSAMPLE Phase-B dispatch can run the cheap early-outs bit-exactly, apply the interpolation
// criterion, and fall through into the unchanged exact walk only for refined pixels. Every former
// `u_Term[ ... ] = V; return;` site became `return V;` with its exact value expression preserved
// (SwVizCostTerm / SwHoistTerm / SwTermQuant wrappers included); main() does the one atlas write.
// SwTermFinal( px ) == the old main body, bit-identical by construction.

// PRELUDE: coverage/falloff + N.L early-outs and the lit classifier. Returns true when the pixel is
// fully handled (swTermOut holds the value to write); false -> run SwTermWalk with swPosOut/swHoistOut.
bool SwTermPrelude( int2 px, out float4 swPosOut, out float swHoistOut, out float swTermOut )
{
	swCblRT = ( g_surfCost.y != 0 );	// cull-before-load runtime toggle (read by the tile-list walk)

	// EXACT receiver position (same value the interaction PS computes from texcoord7 x model
	// matrix; softpos.ps stored it at float32). Pixels never rasterised by the position pass
	// (sky, translucent-only) hold the clear value (w == 0); the interaction shader never reads
	// the term there (translucent draws keep the in-shader integral, sky draws no soft
	// interaction), so writing 1.0 and skipping the integral is free.
	const float4 swPos = t_WorldPos.Load( int3( px, 0 ) );
	const float3 swP = swPos.xyz;
	swPosOut = swPos;
	swHoistOut = 1.0f;
	swTermOut = 1.0f;

	// COVERAGE EARLY-OUTS - the fix for the measured 1.8x loss of the first compute-decoupling
	// cut (see r_softShadowCompute help): the fragment path pays only for pixels that survive
	// depth/scissor/stencil culling AND its falloff-first zero test, while this dispatch covers
	// the whole scissor rect. Mirror both tests here so the covered pixel sets converge.
	// Skipped pixels WRITE 1.0 (never a bare return): the interaction PS may still Load any
	// pixel inside the scissor - on the exact-zero boundary its own interpolated falloff sample
	// can disagree with ours in the last bit, and 1.0 x (a contribution of exactly ~0) is the
	// value the falloff-first FS path produces there anyway.
	float swHoist = 1.0f;	// lit-early-out C1 hoist factor (1.0 = untouched; see SwHoistTerm)
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
				const float swInt = max( max( swProj.x * swFall.x, swProj.y * swFall.y ), swProj.z * swFall.z );
				swSkip = ( swInt <= g_surfParams.z );
#if SW_GPU_WALK_COUNTERS
				if( swSkip && g_surfParams.z > 0.0f )
				{
					InterlockedAdd( u_WalkCnt[ 27 ], 1u );	// lit-early-out fired above exact-zero (gate-execution proof)
				}
#endif
				// C1 HOIST above the cut: fade the occlusion in over [T, 2T] (see SwHoistTerm).
				// T <= 0 keeps swHoist at 1.0 (smoothstep(0,0,x) is degenerate; exact path untouched).
				if( !swSkip && g_surfParams.z > 0.0f )
				{
					swHoist = smoothstep( g_surfParams.z, 2.0f * g_surfParams.z, swInt );
				}
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
			swTermOut = SwVizCostTerm( 1.0f, SWVC_EARLYOUT );
			return true;
		}
	}

	// PORTAL-AREA DEAD-WORK CULL (r_softShadowAreaCull): g_areaMask is all-ones unless the cvar armed it,
	// so the guard keeps the normal Load off the shipped path. areaP1 = 0 (non-world receiver, or the
	// never-rasterised clear) never skips; a world fragment in an area the light's flood never reached has
	// no interaction draw under this light - its term is provably never read, write 1.0 and stop.
	if( ( g_areaMask.x & g_areaMask.y & g_areaMask.z & g_areaMask.w ) != 0xFFFFFFFFu )
	{
		const int swAreaP1 = ( ( int )( t_WorldNormal.Load( int3( px, 0 ) ).w + 0.5 ) ) >> 2;
		if( swAreaP1 > 0 && ( ( g_areaMask[ ( swAreaP1 - 1 ) >> 5 ] >> ( ( uint )( swAreaP1 - 1 ) & 31u ) ) & 1u ) == 0u )
		{
			swTermOut = 1.0f;
			return true;
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
				swTermOut = SwVizCostTerm( 1.0f, SWVC_OTHER );	// classifier lit
				return true;
			}
			// CLASSIFIER UMBRA (SW_CLASS_UMBRA == 2): the cell is provably fully occluded (a single
			// occluder's umbra wedge contains the whole cell ball), so the coverage integral saturates
			// to 1 and the term is EXACTLY 0 - skip the walk. Same value the tile-umbra sentinel writes
			// (SwHoistTerm(0,swHoist)), lossless by the identical all-or-nothing argument as lit.
			if( cbyte == 2u )
			{
#if SW_GPU_WALK_COUNTERS
				InterlockedAdd( u_WalkCnt[ 29 ], 1u );	// classifier UMBRA skip (frags never reaching the walk)
#endif
				swTermOut = SwVizCostTerm( SwHoistTerm( 0.0f, swHoist ), SWVC_UMBRA );
				return true;
			}
		}
	}

	swHoistOut = swHoist;
	return false;
}

// WALK: everything after the prelude - contributor/surf cache paths + the exact tile/spill/full walk.
// Bit-identical to the old main body from the walk-counter marker on; returns the term to write.
float SwTermWalk( int2 px, float4 swPos, float swHoist )
{
	const float3 swP = swPos.xyz;

#if SW_GPU_WALK_COUNTERS
	InterlockedAdd( u_WalkCnt[ 7 ], 1u );	// this pixel survived the early-outs and runs the walk
	// HYBRID-WHITELIST CENSUS (counting permutation only - shipped codegen untouched): scan the whitelist
	// block CASTER BY CASTER (headers carry the span) and tally, TRIANGLE-WEIGHTED via the caster table,
	// what a per-caster dispatch would offload. SPARSE 16x16 LATTICE: 1/256 of walked fragments - keeps
	// every count EXACT in uint32 (the dense sums reached ~1.6e10/frame and WRAPPED, printing impossible
	// >100% shares). cut and clean accumulate SEPARATELY, so share = cut/(cut+clean) sums to 100% by
	// construction. 30 = SAMPLED frags, 31 = SUM cut-caster tris, 32 = SUM clean-caster tris, 33 = SUM cut casters.
	if( g_wedge.x >= 0 && g_wedge.y > 0 && ( ( px.x & 15 ) == 0 ) && ( ( px.y & 15 ) == 0 ) )
	{
		InterlockedAdd( u_WalkCnt[ 30 ], 1u );
		int  swWlSe = 0;
		int  swWlCas = 0;
		uint swWlCutTris = 0;
		uint swWlCleanTris = 0;
		uint swWlNCut = 0;
		while( swWlSe < g_wedge.y && swWlCas < g_range.y )
		{
			const float4 swWlH0 = t_SoftEdges[ g_wedge.x + swWlSe * 2 + 0 ];
			const float4 swWlH1 = t_SoftEdges[ g_wedge.x + swWlSe * 2 + 1 ];
			if( swWlH0.w < 0.0 )
			{
				const int swWlCnt = ( int )swWlH1.y;					// this caster's edge-record count
				float swWlGapC = 0.0;
				int   swWlNCC = 0;
				int   swWlClipC = 0;
				// walk ONE caster's span (header + its edges) so the cut verdict is per caster
				SoftShadow_WedgeOcclusionEx( swPos.xyz, g_lightR.xyz, max( g_lightR.w, 1e-2 ),
											 g_wedge.x + swWlSe * 2, swWlCnt + 1, 0.0,
											 swWlGapC, swWlNCC, swWlClipC );
				// caster table (order-aligned by the emit contract): |c1.y| = triangle count (neg = box).
				// Every caster lands in EXACTLY ONE bucket, so cut% + clean% = 100% by construction.
				const int swWlTris = abs( ( int )t_SoftEdges[ g_flags.y + swWlCas * 2 + 1 ].y );
				if( ( swWlClipC >> 3 ) > 0 )
				{
					swWlNCut++;
					swWlCutTris += ( uint )swWlTris;
				}
				else
				{
					swWlCleanTris += ( uint )swWlTris;
				}
				swWlCas++;
				swWlSe += swWlCnt + 1;
			}
			else
			{
				swWlSe++;		// malformed record guard: resync on the next header
			}
		}
		InterlockedAdd( u_WalkCnt[ 31 ], swWlCutTris );
		InterlockedAdd( u_WalkCnt[ 32 ], swWlCleanTris );
		InterlockedAdd( u_WalkCnt[ 33 ], swWlNCut );
	}
#endif

	const float3 swL = g_lightR.xyz;
	const float  swR = max( g_lightR.w, 1e-2 );
	g_swMinDnR = g_misc.x;			// projection dn-clamp (r_softShadowMinDnRatio): grazing-grain fix; 0 = exact



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
	float swRotAng = ( swRotHash - floor( swRotHash ) ) * 6.28318531;
#if SW_SCANLINE
	// FUBINI is rotation-INVARIANT in the continuous limit, so the per-fragment rotation adds no
	// signal - it only decorrelates the discrete grid's (32-col x N-chord) DISCRETISATION error into
	// per-pixel NOISE (the "ants", scanline-only; the sampled path averages its rotation over the 16
	// samples so it reads smooth). Zero it: the residual error becomes a SMOOTH function of world
	// position (spatially coherent, sub-visible) and temporally stable per world point.
	// g_surfCost.w (r_softShadowScanRotate) != 0 re-injects the old rotation: a POSITIVE CONTROL that
	// deliberately reproduces the grain so the gate's GateGrain detector can be validated to fire.
	if( g_surfCost.w == 0 ) { swRotAng = 0.0f; }
#endif

#if SW_CONTRIB_CACHE
	// ---- CONTRIBUTOR CACHE (see the declaration block above for the design + slot layout) ----
	// g_surfParams.x = cell size G, g_surfCost.w = table capacity
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
					return SwVizCostTerm( SwHoistTerm( 0.0f, swHoist ), SWVC_UMBRA );
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
		// GEN-FREE hash and keys: a generation bump must NOT move the slot index, or the stale slot
		// at the old index is orphaned forever (measured live: 172M probe-exhausts, serve share 5.9%
		// - the table filled with unreclaimable dead slots). The generation lives in its own slot
		// word; a mismatch FREES the slot in place (see below).
		uint h = keyLo * 0x9E3779B1u ^ keyHi * 0x85EBCA77u;
		uint slot = h & capM;
		int  servedBase = -1, servedCount = 0;
		int  recordSlot = -1;
		// probe-outcome diagnosis flags (classified into header words 7/8/9 after the loop)
		bool swDgBudget = false, swDgKey = false, swDgUnserv = false;
		bool swTileMissRec = false;			// this record is a tile-coverage fill-in, not a refinement verdict
		bool swRefineRec = false;			// this record is the continuous validator (revoke/converge verdict)
		[loop] for( int pr = 0; pr < 16; pr++ )
		{
			const uint sBase = SW_CONTRIB_HEADER + slot * SW_CONTRIB_STRIDE;
			const uint w0 = u_Contrib[ sBase ];
			if( w0 == keyLo && u_Contrib[ sBase + 1 ] == keyHi )
			{
				swDgKey = true;
				// GENERATION RECLAIM = FREE the slot. The winner CASes keyLo to the FREEING sentinel
				// (matches neither a key nor the vacancy, so every other prober skips past), clears
				// state/counts/masks AND the entries (claim-by-value scans to the first zero, so a
				// stale non-zero entry would both survive a count reset and break the scan), then
				// publishes keyLo = 0 LAST - no reader can observe a half-cleared slot under a live
				// key. The cell re-enters through the normal budget-gated claim.
				if( u_Contrib[ sBase + SW_CONTRIB_GEN ] != curGen )
				{
					uint prevK;
					InterlockedCompareExchange( u_Contrib[ sBase ], keyLo, SW_CONTRIB_FREEING, prevK );
					if( prevK == keyLo )
					{
						u_Contrib[ sBase + 2 ] = 0u;
						u_Contrib[ sBase + 3 ] = 0u;
						u_Contrib[ sBase + SW_CONTRIB_TILEBIT ]  = 0u;
						u_Contrib[ sBase + SW_CONTRIB_TILEPEND ] = 0u;
						u_Contrib[ sBase + SW_CONTRIB_GEN ] = 0u;
						[loop] for( int fc = 0; fc < SW_CONTRIB_K; fc++ )
						{
							u_Contrib[ sBase + 4u + ( uint )fc ] = 0u;
						}
						u_Contrib[ sBase ] = 0u;		// publish the vacancy LAST
						SwContribDiag( 11u );			// stat: slots reclaimed
					}
					break;								// plain walk this frame either way
				}
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
				const uint st = u_Contrib[ sBase + 2 ];
				if( ( st & SW_CONTRIB_POISON ) != 0u && u_Contrib[ sBase + 3 ] >= ( uint )SW_CONTRIB_K )
				{
					// PERMANENT overflow: > K distinct contributors - the union cannot represent this
					// cell. Plain walk forever, and no refinement either (its verdict cannot change).
					break;
				}
				if( swServeListCount < 0 && swSpillN < 0 )
				{
					// corrupt/untiled fragment: can neither record (list-based walk) nor serve
					swDgUnserv = true;
					break;
				}
				if( ( u_Contrib[ sBase + SW_CONTRIB_TILEBIT ] & swTileBit ) == 0u )
				{
					// TILE-MISS: no full-tile evaluation has certified this tile, so the union may
					// lack its contributors - serving it under-covered (measured max|diff|=1.0).
					// Record instead: the whole tile's fragments walk+append this dispatch, the bit
					// pends at finalize, and the tile serves after the deferred promotion. This is
					// the ONLY warm path (v3): a fresh claim's tiles are all uncertified.
					swTileMissRec = true;
					recordSlot = ( int )sBase;
				}
				else
				{
					// CONTINUOUS REFINEMENT: 1 in N served fragments runs the exact recording walk
					// instead - a NEW append revokes the union (POISON), a converged eval restores
					// it. Frozen unions were the measured CONTINUITY defect source. SPILL fragments
					// refine 8x rarer (their record walk is the whole cluster hierarchy, ~10x a
					// tile-list record: at 1/64 it alone regressed cap0007 term ~5 ms) - except at
					// divisor 1, the exactness-debug config, which must stay exact everywhere.
					const uint swRefBase = ( uint )( g_aa.w + 0.5f );
					const uint swRefN = swRefBase * ( ( swServeListCount < 0 && swRefBase > 1u ) ? 8u : 1u );
					if( swRefN != 0u && u_Contrib[ sBase + 3 ] < ( uint )SW_CONTRIB_K
							&& ( ( ( uint )px.x * 7u + ( uint )px.y * 13u ) % swRefN ) == 0u )
					{
						SwContribDiag( 10u );			// refinement record volume
						swRefineRec = true;
						recordSlot = ( int )sBase;
					}
					else if( ( st & SW_CONTRIB_POISON ) != 0u )
					{
						break;							// revoked: only refinement records until convergence
					}
					else
					{
						servedBase  = ( int )( sBase + 4u );
						servedCount = ( int )min( u_Contrib[ sBase + 3 ], ( uint )SW_CONTRIB_K );
					}
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
						// GEN before keyHi: readers match on keyLo+keyHi, so the generation must be
						// in place before the slot becomes matchable (else a racer frees a fresh claim)
						u_Contrib[ sBase + SW_CONTRIB_GEN ] = curGen;
						u_Contrib[ sBase + 1 ] = keyHi;
						InterlockedAdd( u_Contrib[ 0 ], 1u );		// budget: one successful claim
						InterlockedAdd( u_Contrib[ 3 ], 1u );		// stat: claimed cells
						swTileMissRec = true;						// a fresh cell's tiles are all uncertified
						recordSlot = ( int )sBase;
						break;
					}
					if( prev == keyLo )
					{
						// lost the race to our own key mid-construction (keyHi/GEN may not be
						// visible yet): record as a tile-miss - the slot's tiles are uncertified
						swTileMissRec = true;
						recordSlot = ( int )sBase;
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
			SwContribDiag( 1u );									// stat: serve hit (wave-aggregated)
			softFrame_t swFC = SoftShadow_Frame( swP, g_lightR.xyz );
			const float swRC = max( g_lightR.w, 1e-2 );
			SwGridWord swGrid[SW_SCAN_CHORDS];
			float2 swEnv[SW_SCAN_CHORDS];		// fractional-endpoint envelope (task #90)
			const int swDiskBits = SW_SCAN_DISKBITS;	// compile-time (register diet: no per-thread mask array)
			[unroll] for( int gm = 0; gm < SW_SCAN_CHORDS; gm++ )
			{
				swGrid[gm] = SwGridZero();
				swEnv[gm] = SwEnvZero();
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
				SoftScan_FillTri( swGrid, swEnv, r0.xyz, r1.xyz, r2.xyz, swP, swFC, swRC, SW_NEAR_EPS );
				int swCovE = 0;
				[unroll] for( int fm = 0; fm < SW_SCAN_CHORDS; fm++ )
				{
					swCovE += SoftScan_PC( swGrid[fm] & SW_SCAN_MASK[fm] );
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
				const uint dtEnc = t_SoftTiles[ swServeListBase + ld2 ];
				if( dtEnc & 0x80000000u )
				{
					// ANALYTIC caster: high-bit-tagged FIRST tri-slot index. The first slot's .w discriminates
					// BOX (.w = v0Rad > 0, read 8 corners -> FillBox) from a convex HULL (.w = -vertexCount,
					// read that many hull verts -> FillHull). FillHull is the generalisation: it projects the
					// hull, takes its 2-D silhouette and fills that through the shared FillPoly core, so a
					// COPLANAR record (a flat caster) reduces to FillPoly bit-exactly while a 3-D brush hull
					// gets its true silhouette. Both evaluate ONCE - no triangle rasterisation.
					const int bft = ( int )( dtEnc & 0x7FFFFFFFu );
					const int bb  = g_range.x + bft * 3;
					const float4 bslot0 = t_SoftEdges[ bb + 0 ];
					swDFill++;
					if( bslot0.w < 0.0f )
					{
						const int pn = min( ( int )( -bslot0.w ), SW_POLY_MAX_VERTS );
						float3 bloop[SW_POLY_MAX_VERTS];
						for( int pk = 0; pk < SW_POLY_MAX_VERTS; pk++ ) { bloop[pk] = t_SoftEdges[ bb + min( pk, pn - 1 ) ].xyz; }
						// per-fragment cone/slab reject (SoftScan_HullConeReject): skip the fill when the hull's
						// bounding sphere cannot shadow this fragment's disk - bit-exact, caps the hull walk floor
						float3 shc = float3( 0.0f, 0.0f, 0.0f );
						for( int shk = 0; shk < pn; shk++ ) { shc += bloop[shk]; }
						shc /= ( float )pn;
						float shr2 = 0.0f;
						for( int shj = 0; shj < pn; shj++ ) { float3 shd = bloop[shj] - shc; shr2 = max( shr2, dot( shd, shd ) ); }
						if( SoftScan_HullConeReject( shc, sqrt( shr2 ), swP, swFC.nrm, swFC.distPL, swRC, SW_NEAR_EPS ) ) { continue; }
						SoftScan_FillHull( swGrid, swEnv, bloop, pn, swP, swFC, swRC, SW_NEAR_EPS );
					}
					else
					{
					float3 bcorner[8];
					bcorner[0] = bslot0.xyz;                bcorner[1] = t_SoftEdges[ bb + 1 ].xyz;
					bcorner[2] = t_SoftEdges[ bb + 2 ].xyz; bcorner[3] = t_SoftEdges[ bb + 3 ].xyz;
					bcorner[4] = t_SoftEdges[ bb + 4 ].xyz; bcorner[5] = t_SoftEdges[ bb + 5 ].xyz;
					bcorner[6] = t_SoftEdges[ bb + 6 ].xyz; bcorner[7] = t_SoftEdges[ bb + 7 ].xyz;
					float3 sbc = float3( 0.0f, 0.0f, 0.0f );
					for( int sck = 0; sck < 8; sck++ ) { sbc += bcorner[sck]; }
					sbc *= 0.125f;
					float sbr2 = 0.0f;
					for( int scj = 0; scj < 8; scj++ ) { float3 sbd = bcorner[scj] - sbc; sbr2 = max( sbr2, dot( sbd, sbd ) ); }
					if( SoftScan_HullConeReject( sbc, sqrt( sbr2 ), swP, swFC.nrm, swFC.distPL, swRC, SW_NEAR_EPS ) ) { continue; }
					SoftScan_FillBox( swGrid, swEnv, bcorner, swP, swFC, swRC, SW_NEAR_EPS );
					}
					int swCovB = 0;
					[unroll] for( int fmb = 0; fmb < SW_SCAN_CHORDS; fmb++ ) { swCovB += SoftScan_PC( swGrid[fmb] & SW_SCAN_MASK[fmb] ); }
					if( swCovB * 100 >= swDiskBits * 99 ) { break; }
					continue;
				}
				const int dt = ( int )dtEnc;
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
				SoftScan_FillTri( swGrid, swEnv, r0.xyz, r1.xyz, r2.xyz, swP, swFC, swRC, SW_NEAR_EPS );
				int swCovD = 0;
				[unroll] for( int fm2 = 0; fm2 < SW_SCAN_CHORDS; fm2++ )
				{
					swCovD += SoftScan_PC( swGrid[fm2] & SW_SCAN_MASK[fm2] );
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
					SoftScan_FillTri( swGrid, swEnv, r0.xyz, r1.xyz, r2.xyz, swP, swFC, swRC, SW_NEAR_EPS );
					int swCovS2 = 0;
					[unroll] for( int fm3 = 0; fm3 < SW_SCAN_CHORDS; fm3++ )
					{
						swCovS2 += SoftScan_PC( swGrid[fm3] & SW_SCAN_MASK[fm3] );
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
			float swCov = SoftScan_ReduceCov( swGrid, swEnv, SW_SCAN_MASK );	// fractional endpoints (task #90)
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
				return SwVizCostTerm( SwHoistTerm( swTermPlain, swHoist ), SWVC_WALK );	// serve-verify: the plain walk ran
			}
			return SwVizCostTerm( SwTermQuant( SwHoistTerm( swTermServe, swHoist ) ), SWVC_HIT );	// contrib-cache serve
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
			SwContribDiag( 2u );									// stat: recording eval (wave-aggregated)
			bool swAppendedNew = false;								// did this eval extend the union?
			bool swAppendGaveUp = false;							// CAS contention cap: must not certify
			const int dynFirstTri = ( g_surfA.z < g_range.y ) ? ( int )t_SoftEdges[ g_flags.y + g_surfA.z * 2 + 1 ].x : 0x7FFFFFFF;
			softFrame_t swFC = SoftShadow_Frame( swP, g_lightR.xyz );
			const float swRC = max( g_lightR.w, 1e-2 );
			SwGridWord swGrid[SW_SCAN_CHORDS];
			float2 swEnv[SW_SCAN_CHORDS];		// fractional-endpoint envelope (task #90)
			const int swDiskBits = SW_SCAN_DISKBITS;	// compile-time (register diet: no per-thread mask array)
			[unroll] for( int gm = 0; gm < SW_SCAN_CHORDS; gm++ )
			{
				swGrid[gm] = SwGridZero();
				swEnv[gm] = SwEnvZero();
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
					SwGridWord solo[SW_SCAN_CHORDS];
					[unroll] for( int sm = 0; sm < SW_SCAN_CHORDS; sm++ )
					{
						solo[sm] = SwGridZero();
					}
					// the MAIN envelope takes the tri's exact endpoints directly: the solo grid merges into
					// swGrid unconditionally below, so a separate per-solo envelope (+2 float2[CHORDS] of
					// register state in the record path - a spill risk) is pure redundancy
					SoftScan_FillTri( solo, swEnv, r0.xyz, r1.xyz, r2.xyz, swP, swFC, swRC, SW_NEAR_EPS );
					// record on ANY RAW solo bits, not only in-disk-mask bits: a tri whose intervals
					// land just outside the disk at every evaluated pixel becomes a contributor under
					// a sub-pixel view displacement - the dominant frozen-union CONTINUITY class
					// (gate: 2717 defects evidence-only vs 52 with all serves re-validated)
					SwGridWord soloRaw = SwGridZero();
					[unroll] for( int sm2b = 0; sm2b < SW_SCAN_CHORDS; sm2b++ )
					{
						soloRaw |= solo[sm2b];
						swGrid[sm2b] |= solo[sm2b];
					}
					if( SwGridNZ( soloRaw ) && rt < dynFirstTri )
					{
						// ENTRIES ARE 1-BASED (rt+1): 0 marks a reserved-but-unstored slot, so concurrent
						// serves can never read a torn entry (they skip 0s).
						const int swApR = SwContribAppend( ( uint )recordSlot, rt );
						swAppendedNew = swAppendedNew || ( swApR == 1 );
						swAppendGaveUp = swAppendGaveUp || ( swApR == 2 );
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
					SwGridWord solo2[SW_SCAN_CHORDS];
					[unroll] for( int sm3 = 0; sm3 < SW_SCAN_CHORDS; sm3++ )
					{
						solo2[sm3] = SwGridZero();
					}
					// main envelope updated directly (solo2 merges unconditionally; see the tile-list record)
					SoftScan_FillTri( solo2, swEnv, r0.xyz, r1.xyz, r2.xyz, swP, swFC, swRC, SW_NEAR_EPS );
					SwGridWord soloRaw2 = SwGridZero();				// ANY raw bits (see the tile-list record)
					[unroll] for( int sm4 = 0; sm4 < SW_SCAN_CHORDS; sm4++ )
					{
						soloRaw2 |= solo2[sm4];
						swGrid[sm4] |= solo2[sm4];
					}
					if( SwGridNZ( soloRaw2 ) && rt2 < dynFirstTri )
					{
						const int swApR2 = SwContribAppend( ( uint )recordSlot, rt2 );
						swAppendedNew = swAppendedNew || ( swApR2 == 1 );
						swAppendGaveUp = swAppendGaveUp || ( swApR2 == 2 );
					}
				}
			}
			// FINALIZE (v3). A TILE-MISS record certifies its tile - it is a FULL-TILE evaluation, so
			// the union provably holds that tile's contributors - unless an append gave up under CAS
			// contention (the tile then re-records next frame instead of certifying incompletely).
			// PEND, never LIVE directly: stamp PENDFRAME FIRST so concurrent probers stop promoting
			// this frame, THEN pend the bit - the deferred promotion makes the tile servable only
			// after every recorder of this dispatch has finalized its appends.
			if( swTileMissRec && !swAppendGaveUp )
			{
				uint swTbDummy;
				InterlockedExchange( u_Contrib[ ( uint )recordSlot + SW_CONTRIB_TILEPFR ], ( uint )g_surfA.y & 0x7FFFu, swTbDummy );
				InterlockedOr( u_Contrib[ ( uint )recordSlot + SW_CONTRIB_TILEPEND ], swTileBit, swTbDummy );
			}
			else if( swRefineRec )
			{
				// REFINEMENT outcome: a NEW contributor means the served union was incomplete for
				// this view - REVOKE serving (POISON) until a later eval converges (appends nothing
				// new), which restores service. Only the validator renders this verdict: a tile-miss
				// fill-in legitimately appends (poisoning on it would revoke every newly joined tile).
				if( swAppendedNew )
				{
					uint pz2;
					InterlockedOr( u_Contrib[ ( uint )recordSlot + 2 ], SW_CONTRIB_POISON, pz2 );
				}
				else if( u_Contrib[ ( uint )recordSlot + 3 ] < ( uint )SW_CONTRIB_K )
				{
					// converged: this full eval found every contributor already recorded
					uint pz3;
					InterlockedAnd( u_Contrib[ ( uint )recordSlot + 2 ], ~SW_CONTRIB_POISON, pz3 );
				}
			}
			float swCovR = SoftScan_ReduceCov( swGrid, swEnv, SW_SCAN_MASK );	// fractional endpoints (task #90)
			if( swCovR * 100 >= swDiskBits * 99 )
			{
				swCovR = swDiskBits;			// plain-walk umbra rounding, matched exactly
			}
			return SwVizCostTerm( SwTermQuant( SwHoistTerm( 1.0 - saturate( swDiskBits > 0 ? ( float )swCovR / ( float )swDiskBits : 0.0 ), swHoist ) ), SWVC_WALK );	// record path walked its tile list
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
	// TIER-3 PROXIMITY (task #112): fragments closer than r_softShadowNearRadius to the view NEVER
	// use the cache - proximal shadows get the exact walk (full accuracy) and cannot pop (no cached
	// state dependency where it is most visible). Skips the 16-slot probe too. 0 = off.
	if( g_econ.x > 0.0f )
	{
		const float3 swCamD = swP - float3( g_misc.y, g_misc.z, g_misc.w );
		if( dot( swCamD, swCamD ) < g_econ.x )
		{
			swCostWorth = false;
		}
	}
	if( g_surfA.x > 0 && g_surfA.z > 0 && swPos.w != 0.0f && swCostWorth )
	{
		// GEOMETRIC dominant axis from softpos.normal.w - NOT the normal-mapped shading .xyz. The prewarm
		// SEED keys texels off the flat GEOMETRIC triangle normal; deriving the key axis here from the bumped
		// shading normal disagreed on every normal-mapped surface whose bump flips the dominant axis, so the
		// warm read missed (measured hit ~4% -> the empty-slot majority). softpos now writes the flat
		// geometric dominant axis (ddx/ddy of world pos, same tie-break) into .w, so seed and read keys align.
		// .w now also carries the portal-area tag above the axis (enc = axis + 4*areaP1): mask to 2 bits
		const int   d  = ( ( int )( t_WorldNormal.Load( int3( px, 0 ) ).w + 0.5 ) ) & 3;
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
		const int   cw = ( int )floor( ( pw + 0.5f * g ) / g ) + 32768;	// half-cell bias - MUST match the seed's kw (knife-edge fix, see softsurf_seed)
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
			if( keyLo < 0xFFFFFFFEu && ( g_aa.z < 1.5f || g_aa.z > 3.5f ) )	// the two far world-corner cells alias the empty/tombstone sentinels: never cached; modes 2/3 skip the probe, mode 4 runs it (real hits serve)
			{
				// CANDIDATE-TOTAL PROBING (2026-08-26): the seed keys from TRIANGLE data (cross-product
				// axis, plane height), the serve from FRAGMENT data (derivative axis, rasterized position)
				// - forcing the two derivations to agree exactly leaked six ways (axis sign, kw knife-edge,
				// kw slab, duplicates, chains, area). Instead the serve probes the COMPLETE candidate set
				// the seed could have planted for this world position: 3 axes x {kw, kw+1, kw-1} - a
				// provably exhaustive enumeration (axis has three values; the dominant-axis slope bound
				// caps drift at one slab, re-proven 2026-08-26). The record's anchor plane arbitrates every
				// candidate identically (the key is only bucketing), so serving any candidate's record is
				// exactly as safe as the primary's. Measured en route: kw+-1 alone = 34% of cap0007's
				// misses (35->48% hit); alt-axis floor 8.6%. After this, a miss MEANS the seed never
				// rasterized the surface - a collector-coverage fact, not a keying failure. Only the
				// PRIMARY candidate writes the miss classification (stat comparability).
				[loop] for( int swCand = 0; swCand < 9; swCand++ )
				{
				const bool swPrimary = ( swCand == 0 );
				const int  swAx3 = swCand / 3;			// 0 = fragment's derived axis, 1/2 = the other two
				const int  swKw3 = swCand % 3;			// 0:+0  1:+1  2:-1
				const int  dC = ( swAx3 == 0 ) ? d : ( ( swAx3 == 1 ) ? ( ( d + 1 ) % 3 ) : ( ( d + 2 ) % 3 ) );
				const float puC = ( dC == 0 ) ? swP.y : ( ( dC == 1 ) ? swP.z : swP.x );
				const float pvC = ( dC == 0 ) ? swP.z : ( ( dC == 1 ) ? swP.x : swP.y );
				const float pwC = ( dC == 0 ) ? swP.x : ( ( dC == 1 ) ? swP.y : swP.z );
				const int  cuC = ( int )floor( puC / g ) + 32768;
				const int  cvC = ( int )floor( pvC / g ) + 32768;
				const int  swCwC = ( int )floor( ( pwC + 0.5f * g ) / g ) + 32768
								   + ( ( swKw3 == 0 ) ? 0 : ( ( swKw3 == 1 ) ? 1 : -1 ) );
				if( cuC < 0 || cuC > 65535 || cvC < 0 || cvC > 65535 || swCwC < 0 || swCwC > 65535 )
				{
					continue;
				}
				const uint keyLoC = ( uint )cuC | ( ( uint )cvC << 16 );
				if( keyLoC >= 0xFFFFFFFEu )
				{
					continue;			// empty/tombstone sentinel alias: never cached
				}
				const uint keyHiC = ( uint )swCwC | ( ( uint )dC << 16 ) | ( ( uint )g_surfA.w << 19 );
				const uint capM = ( uint )g_surfA.x - 1u;	// capacity is a power of two (CPU-enforced)
				const uint h = keyLoC * 0x9E3779B1u ^ keyHiC * 0x85EBCA77u;
				uint slot = h & capM;
				// DOUBLE HASHING - second-hash odd step, MUST match softsurf_seed (see rationale there)
				const uint hstep = ( ( keyLoC * 0x85EBCA77u ^ keyHiC * 0x9E3779B1u ) | 1u );
				[loop]										// keep the probe ROLLED: unrolling it exploded the
				for( int pr = 0; pr < 128; pr++ )		// surf-permutation VGPR count (256 + spill), collapsing occupancy.
														// 16 -> 64 -> 128 (2026-08-26): viz-8 measured millions of misses whose
														// keys sit past the probe horizon; each doubling converted them to hits
														// (cap0007 20->35% at 64) and the probe itself is measured ~free. The
														// depth pathology at ~35% load is unexplained - root-cause pending.
				{
					const uint sBase = slot * 8u;
					const uint w0 = u_SurfTable[ sBase ];
					if( w0 == 0xFFFFFFFEu )				// TOMBSTONE (build freed a record here): the chain
					{									// continues past it - do NOT treat as end-of-chain
						slot = ( slot + hstep ) & capM;
						continue;
					}
					if( w0 == keyLoC && u_SurfTable[ sBase + 1 ] == keyHiC )
					{
						const uint s2 = u_SurfTable[ sBase + 2 ];
						const uint code = s2 & 3u;
						if( ( s2 >> 2u ) != curGen )
						{
							// STALE generation (the light's static set changed). READ-ONLY runtime: do NOT
							// reclaim/re-request here - that was a per-fragment atomic storm on every un-warm
							// texel (the dominant cache overhead). The camera-independent invalidation hook
							// re-warms this light off the burst path; this frame just takes the exact walk.
							if( swPrimary )
							{
								swStatIdx = 1u;
								swMissR = 0u;		// stale generation
							}
							break;
						}
						if( code == 2u )	// BUILT: consume
						{
							if( g_aa.z > 0.5f && g_aa.z < 1.5f ) { if( swPrimary ) { swStatIdx = 2u; } break; }	// DEBUG force-walk (g_aa.z==1 ONLY): probe ran, key found -> take the exact walk instead of the cached hit (probe-tax isolation); modes 3/4 must serve real hits
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
							// key) must still reject; the band IS the widened ~1 texel so tilt/curve stops walking.
							// (measure-first 2026-08-23: 56% anchor-rej at 0.0625*g was almost all tilt; gate arbiter
							// catches any floor+step alias that leaks through the wider band.)
							const float swAnchBand = g * 1.0f;
#else
							const float swAnchBand = g * 0.0625f;
#endif
							if( abs( pwC - aH ) > swAnchBand )
							{
								if( swPrimary )
								{
									swStatIdx = 3u;	// ANCHOR-REJECT: wrong surface for this record
								}
								break;			// exact miss path (a kw fallback candidate may still pass its own band)
							}
							const uint w4 = u_SurfTable[ sBase + 4 ];
							const uint foldedCnt = w4 >> 16;			// high 16: static occluders gridded/folded = the per-texel saving (viz)
#if SW_SURF_GRID
							// GRID hit: load the frozen static bit-grid into swGrid + build this fragment's
							// circular disk mask (fragment-invariant). Dynamic casters OR into swGrid below;
							// coverage = popcount(swGrid & diskMask)/diskBits - the exact Fubini union (no
							// bilinear F, no residual pool; umbra / silhouette / tilt represented as bits).
							// SW_SURF_GRID cached path: reads the per-chord grid PERSISTED in t_SurfGrid. The
							// buffer element is SwGridWord too (softsurf_build writes the same width), so the
							// cache tracks the live grid resolution automatically.
								float2 swEnv[SW_SCAN_CHORDS];		// envelope required by the FillTri signature; grid-mode serve stays discrete (task #90 follow-up), so this is unused here
							SwGridWord swGrid[SW_SCAN_CHORDS];
							const int swDiskBits = SW_SCAN_DISKBITS;	// compile-time (register diet: no per-thread mask array)
							{
								const uint gBaseR = slot * ( uint )SW_SCAN_CHORDS;
								[unroll] for( int gr = 0; gr < SW_SCAN_CHORDS; gr++ )
								{
									swGrid[gr]     = t_SurfGrid[ gBaseR + gr ];
										swEnv[gr]  = SwEnvZero();		// grid-mode static envelope not cached yet (task #90 follow-up); reduction stays discrete here
								}
							}
#ifdef SW_SUBSAMPLE_WMIN
							// baked static bit-grid: per-occluder widths are unavailable, so this corner
							// cannot vouch a C3 floor - force refinement around it (hits are cheap anyway)
							g_swWMin = 0.0f;
#endif
#else
							const uint resOfs = u_SurfTable[ sBase + 3 ];
							const uint resCnt = w4 & 0xFFFFu;			// low 16: residual occluders still walked
							const uint f01 = u_SurfTable[ sBase + 5 ];
							const uint f23 = u_SurfTable[ sBase + 6 ];
							const float fu = puC / g - floor( puC / g );
							const float fv = pvC / g - floor( pvC / g );
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
											return SwVizCostTerm( SwHoistTerm( 0.0f, swHoist ), SWVC_UMBRA );	// whole tile provably umbra (as the miss path)
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
								SoftScan_FillTri( swGrid, swEnv, t_SoftEdges[ bd + 0 ].xyz, t_SoftEdges[ bd + 1 ].xyz, t_SoftEdges[ bd + 2 ].xyz, swP, swFP, swRP, SW_NEAR_EPS );
							}
							int swCovG = 0;
							[unroll] for( int cm = 0; cm < SW_SCAN_CHORDS; cm++ ) { swCovG += SoftScan_PC( swGrid[cm] & SW_SCAN_MASK[cm] ); }
							float swTermC = 1.0 - saturate( swDiskBits > 0 ? ( float )swCovG / ( float )swDiskBits : 0.0 );
#else
							// TIER-1 ECONOMICS GATE (task #112): serve only when it is provably cheaper
							// than the direct walk. Both costs are in-register: direct = this fragment's
							// tile-list length (spill/untiled -> effectively unbounded, always serve);
							// serve = residual count + a probe/overhead constant. The dynamic share
							// cancels (walked either way). Margin scales with view distance: near the
							// exact-radius boundary serving must win decisively (2x), far away any
							// positive margin serves. Fail -> fall through to the exact walk, counted
							// as walk-always. g_econ.y = 0 disables (today's always-serve).
							if( g_econ.y > 0.0f )
							{
								const float swDirectC = ( swHitListCount >= 0 ) ? ( float )swHitListCount : 1e6f;
								float swMargin = g_econ.y;
								if( g_econ.x > 0.0f )
								{
									const float3 swCamD1 = swP - float3( g_misc.y, g_misc.z, g_misc.w );
									const float swRn = sqrt( g_econ.x );
									swMargin = lerp( 2.0f * g_econ.y, g_econ.y,
													 saturate( ( length( swCamD1 ) - swRn ) / ( 4.0f * swRn ) ) );
								}
								if( ( float )resCnt + 16.0f >= swMargin * swDirectC )
								{
									swStatIdx = 2u;		// gated walk: direct computation is cheaper here
									break;				// exact fall-through walk (same as walk-always)
								}
							}
							const float occEx = SoftShadow_FaceCoverageSurfResidual(
													swP, g_lightR.xyz, max( g_lightR.w, 1e-2 ), g_range.x,
													( int )resOfs, ( int )resCnt, ( int )asuint( g_econ.z ),
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
							return SwVizCostTerm( SwHoistTerm( swTermC, swHoist ), SWVC_HIT );	// surf-cache BUILT consume
						}
						if( swPrimary )
						{
							if( code == 3u )
							{
								swStatIdx = 2u;		// WALK-ALWAYS (self-gate / overflow rejected the texel)
							}
							else
							{
								swMissR = 1u;		// code 1 REQUESTED: seeded but the build has not filled it yet
							}
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
							u_SurfTable[ sBase + 1 ] = keyHiC;
							// anchor: build probes this height plane, EXACT surface height (MEASURED
							// 2026-08-20: snapping to a G/16 quantum exploded the gate 48 -> 667,
							// CONTINUITY 517 - contact shadows are hyper-sensitive to even G/32 of
							// off-surface anchor, same class as the PCSS bias floor). Accumulated via
							// flip-encoded InterlockedMin (cleared slot = +inf), so the anchor is the
							// MIN height over the contributing fragments - claim-race-independent.
							InterlockedMin( u_SurfTable[ sBase + 7 ], SwSurfFlipF( pwC ) );
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
						if( swPrimary )
						{
							swMissR = 2u;			// EMPTY slot: this texel was never seeded/claimed
						}
						break;						// claimed (by us or a racer): exact miss path this frame
					}
					slot = ( slot + hstep ) & capM;	// occupied by another key: double-hash step
				}
				}	// kw candidate loop
			}
			// FORCE-HIT CEILING (r_softShadowSurfCacheForceWalk 3/4, task #87): UNREALISTIC benchmark
			// modes for latency attribution. Mode 3: probe skipped, EVERY gated fragment served as if
			// built with ZERO static work (no grid loads, no residuals; the dynamic tile walk still
			// runs) - the 100%-hit upper bound. Mode 4: the probe RAN above, real hits served normally
			// (returned already), and only the fall-through fragments (miss/walk-always/anchor-reject)
			// are force-hit - the delta vs mode 0 prices the misses' static walk, the delta vs mode 3
			// prices the real serve. The image is wrong either way; only the frame time means anything.
			if( g_aa.z > 2.5f )
			{
				int fhBase = 0, fhCount = -1, fhDyn = 0;
				if( g_surfA.z >= g_range.y )
				{
					fhCount = 0;		// no dynamic casters at all
				}
				else
				{
					fhDyn = ( int )t_SoftEdges[ g_flags.y + g_surfA.z * 2 + 1 ].x;	// first dynamic tri
					const int fTx = px.x / SW_TILE_SIZE - g_tile.x;
					const int fTy = px.y / SW_TILE_SIZE - g_tile.y;
					if( g_range.z >= 0 && fTx >= 0 && fTy >= 0 )
					{
						const int  fSlot = g_range.z + ( fTy * g_range.w + fTx ) * ( g_flags.w + 1 );
						const uint fCnt  = t_SoftTiles[ fSlot ];
						if( fCnt == SW_TILE_UMBRA )
						{
							SW_SURF_STAT( ( g_aa.z > 3.5f ) ? swStatIdx : 0u );	// mode 4 keeps the honest class split
							return SwVizCostTerm( SwHoistTerm( 0.0f, swHoist ), SWVC_UMBRA );	// provable umbra: same as the real hit path
						}
						if( fCnt <= ( uint )g_flags.w )		// normal list (exclude spill/corrupt sentinels)
						{
							fhBase  = fSlot + 1;
							fhCount = ( int )fCnt;
						}
					}
				}
				if( fhCount >= 0 )
				{
#if SW_SURF_GRID
					// zero static grid + dynamic tile fill: mirrors the grid hit path with the t_SurfGrid
					// loads and all static bits gone
					float2 fhEnv[SW_SCAN_CHORDS];
					SwGridWord fhGrid[SW_SCAN_CHORDS];
					[unroll] for( int fz = 0; fz < SW_SCAN_CHORDS; fz++ ) { fhGrid[fz] = SwGridZero(); fhEnv[fz] = SwEnvZero(); }
					const softFrame_t fhFP = SoftShadow_Frame( swP, g_lightR.xyz );
					const float fhRP = max( g_lightR.w, 1e-2 );
					[loop] for( int fl = 0; fl < fhCount; fl++ )
					{
						const int fse = ( int )t_SoftTiles[ fhBase + fl ];
						if( fse < fhDyn ) { continue; }		// dynamic tris only (static is "cached" = zero)
						const int fbd = g_range.x + fse * 3;
						SoftScan_FillTri( fhGrid, fhEnv, t_SoftEdges[ fbd + 0 ].xyz, t_SoftEdges[ fbd + 1 ].xyz, t_SoftEdges[ fbd + 2 ].xyz, swP, fhFP, fhRP, SW_NEAR_EPS );
					}
					int fhCov = 0;
					[unroll] for( int fc = 0; fc < SW_SCAN_CHORDS; fc++ ) { fhCov += SoftScan_PC( fhGrid[fc] & SW_SCAN_MASK[fc] ); }
					const float occFh = ( float )fhCov / ( float )SW_SCAN_DISKBITS;
#else
					const float occFh = SoftShadow_FaceCoverageSurfResidual(
											swP, g_lightR.xyz, max( g_lightR.w, 1e-2 ), g_range.x,
											0, 0, 0,
											g_flags.y, g_surfA.z, g_range.y,
											fhBase, fhCount, fhDyn, swRotAng );
#endif
					if( g_aa.z > 3.5f )		// mode 4: honest class split (real hits already returned above)
					{
						SW_SURF_STAT( swStatIdx );
						if( swStatIdx == 1u ) { SwSurfMissReason( swMissR ); }
					}
					else
					{
						SW_SURF_STAT( 0u );	// mode 3: counted as HIT, the bench line must read 100%
					}
					return SwVizCostTerm( SwHoistTerm( 1.0f - saturate( occFh ), swHoist ), SWVC_OTHER );	// force-hit debug path
				}
				// spill/untiled tile: fall through to the exact walk (the real hit path does the same)
			}
			// AXIS-TIE INSTRUMENT (viz 5, task #87): an empty-slot miss whose texel IS built under a
			// DIFFERENT dominant axis is the silent built-but-not-read class - the seed keys off the
			// tri cross-product while the read keys off softpos's ddx/ddy axis, and near-45-degree
			// geometry can disagree, which today reads as "never seeded". Under viz 5 ONLY, sub-reason
			// 3 is REDEFINED to count found-under-other-axis (genuine probe-overflow folds into
			// empty-slot). Costs two extra 16-probes per miss - instrument mode, never default.
			if( ( int )g_surfParams.y == 5 && swStatIdx == 1u && swMissR != 0u && swMissR != 1u )
			{
				if( swMissR == 3u ) { swMissR = 2u; }
				const uint capM5 = ( uint )g_surfA.x - 1u;
				[loop] for( int da = 0; da < 3; da++ )
				{
					if( da == d ) { continue; }
					const float pu5 = ( da == 0 ) ? swP.y : ( ( da == 1 ) ? swP.z : swP.x );
					const float pv5 = ( da == 0 ) ? swP.z : ( ( da == 1 ) ? swP.x : swP.y );
					const float pw5 = ( da == 0 ) ? swP.x : ( ( da == 1 ) ? swP.y : swP.z );
					const int cu5 = ( int )floor( pu5 / g ) + 32768;
					const int cv5 = ( int )floor( pv5 / g ) + 32768;
					const int cw5 = ( int )floor( ( pw5 + 0.5f * g ) / g ) + 32768;	// half-cell bias - MUST match the seed's kw (same as the primary key above; unbiased here undercounted the alt-axis class)
					if( cu5 < 0 || cu5 > 65535 || cv5 < 0 || cv5 > 65535 || cw5 < 0 || cw5 > 65535 ) { continue; }
					const uint kLo5 = ( uint )cu5 | ( ( uint )cv5 << 16 );
					const uint kHi5 = ( uint )cw5 | ( ( uint )da << 16 ) | ( ( uint )g_surfA.w << 19 );
					const uint h5 = kLo5 * 0x9E3779B1u ^ kHi5 * 0x85EBCA77u;
					uint s5 = h5 & capM5;
					const uint st5 = ( ( kLo5 * 0x85EBCA77u ^ kHi5 * 0x9E3779B1u ) | 1u );	// double-hash step (matches seed/serve)
					[loop] for( int p5 = 0; p5 < 128; p5++ )	// depth MUST match the serve's 128 or the class % is a floor (audit 2026-08-26)
					{
						const uint b5 = s5 * 8u;
						if( u_SurfTable[ b5 ] == kLo5 && u_SurfTable[ b5 + 1u ] == kHi5 )
						{
							if( ( u_SurfTable[ b5 + 2u ] & 3u ) == 2u ) { swMissR = 3u; }	// BUILT under the other axis
							break;
						}
						s5 = ( s5 + st5 ) & capM5;
					}
					if( swMissR == 3u ) { break; }
				}
			}
			// KW-DRIFT INSTRUMENT (viz 6, task #87): an empty-slot miss whose texel IS built one cw cell
			// up/down under the SAME axis is the slope-drift class - the seed anchors the TRIANGLE PLANE's
			// height at the texel center while the serve keys the fragment's rasterized world height, and on
			// tilted receivers the two straddle a cell boundary (kw off by one). Under viz 6 ONLY, sub-reason
			// 3 is REDEFINED to count found-at-kw+-1 (genuine probe-overflow folds into empty-slot). Costs
			// two extra 16-probes per miss - instrument mode, never default.
			if( ( int )g_surfParams.y == 6 && swStatIdx == 1u && swMissR != 0u && swMissR != 1u )
			{
				if( swMissR == 3u ) { swMissR = 2u; }
				const uint capM6 = ( uint )g_surfA.x - 1u;
				[loop] for( int dw = -1; dw <= 1; dw += 2 )
				{
					const int cw6 = cw + dw;
					if( cw6 < 0 || cw6 > 65535 ) { continue; }
					const uint kHi6 = ( uint )cw6 | ( axis << 16 ) | ( ( uint )g_surfA.w << 19 );
					const uint h6 = keyLo * 0x9E3779B1u ^ kHi6 * 0x85EBCA77u;
					uint s6 = h6 & capM6;
					const uint st6 = ( ( keyLo * 0x85EBCA77u ^ kHi6 * 0x9E3779B1u ) | 1u );	// double-hash step (matches seed/serve)
					[loop] for( int p6 = 0; p6 < 128; p6++ )	// depth MUST match the serve's 128 (audit 2026-08-26)
					{
						const uint b6 = s6 * 8u;
						if( u_SurfTable[ b6 ] == keyLo && u_SurfTable[ b6 + 1u ] == kHi6 )
						{
							if( ( u_SurfTable[ b6 + 2u ] & 3u ) == 2u ) { swMissR = 3u; }	// BUILT one cell up/down
							break;
						}
						s6 = ( s6 + st6 ) & capM6;
					}
					if( swMissR == 3u ) { break; }
				}
			}
			// REGION-VS-BOUNDARY INSTRUMENT (viz 7, task #87): an empty-slot miss with a BUILT in-plane
			// NEIGHBOR texel (cu+-1 / cv+-1, same axis/kw) sits at the EDGE of a seeded region - a seed
			// rasterization/footprint shortfall. No built neighbor = the whole region is unseeded - a
			// collector gap (surface class never emitted). Under viz 7 ONLY, sub-reason 3 is REDEFINED
			// to count edge-of-seeded (genuine probe-overflow folds into empty-slot). 4 extra 16-probes
			// per miss - instrument mode, never default.
			if( ( int )g_surfParams.y == 7 && swStatIdx == 1u && swMissR != 0u && swMissR != 1u )
			{
				if( swMissR == 3u ) { swMissR = 2u; }
				const uint capM7 = ( uint )g_surfA.x - 1u;
				[loop] for( int nb = 0; nb < 4; nb++ )
				{
					const int cu7 = cu + ( ( nb == 0 ) ? -1 : ( ( nb == 1 ) ? 1 : 0 ) );
					const int cv7 = cv + ( ( nb == 2 ) ? -1 : ( ( nb == 3 ) ? 1 : 0 ) );
					if( cu7 < 0 || cu7 > 65535 || cv7 < 0 || cv7 > 65535 ) { continue; }
					const uint kLo7 = ( uint )cu7 | ( ( uint )cv7 << 16 );
					if( kLo7 == 0xFFFFFFFFu ) { continue; }
					const uint h7 = kLo7 * 0x9E3779B1u ^ keyHi * 0x85EBCA77u;
					uint s7 = h7 & capM7;
					const uint st7 = ( ( kLo7 * 0x85EBCA77u ^ keyHi * 0x9E3779B1u ) | 1u );	// double-hash step (matches seed/serve)
					[loop] for( int p7 = 0; p7 < 128; p7++ )	// depth MUST match the serve's 128 (audit 2026-08-26)
					{
						const uint b7 = s7 * 8u;
						if( u_SurfTable[ b7 ] == kLo7 && u_SurfTable[ b7 + 1u ] == keyHi )
						{
							if( ( u_SurfTable[ b7 + 2u ] & 3u ) == 2u ) { swMissR = 3u; }	// BUILT neighbor: edge of a seeded region
							break;
						}
						s7 = ( s7 + st7 ) & capM7;
					}
					if( swMissR == 3u ) { break; }
				}
			}
			// DEEP-PROBE INSTRUMENT (viz 8, 2026-08-26): a probe-overflow miss re-probed along the SAME
			// double-hash sequence to depth 64. Found BUILT deeper -> sub-reason 3 stays (key present,
			// probe depth is the binding constraint); not found -> reclassified to 2 (key ABSENT: the
			// seed never planted it - an insert-side loss, not congestion). Splits the 2.3M cap0007
			// overflow bucket decisively. Instrument mode, never default.
			if( ( int )g_surfParams.y == 8 && swStatIdx == 1u && swMissR == 3u )
			{
				swMissR = 2u;
				const uint capM8 = ( uint )g_surfA.x - 1u;
				const uint st8 = ( ( keyLo * 0x85EBCA77u ^ keyHi * 0x9E3779B1u ) | 1u );
				uint s8 = ( keyLo * 0x9E3779B1u ^ keyHi * 0x85EBCA77u ) & capM8;
				[loop] for( int p8 = 0; p8 < 256; p8++ )	// deeper than the serve's 128 by design: "present past the serve horizon" must be distinguishable from absent (audit: 64 was vacuous)
				{
					const uint b8 = s8 * 8u;
					const uint w8 = u_SurfTable[ b8 ];
					if( w8 == 0xFFFFFFFFu )
					{
						break;			// true end-of-chain: key absent
					}
					if( w8 == keyLo && u_SurfTable[ b8 + 1u ] == keyHi )
					{
						swMissR = 3u;	// present past the 16-probe horizon
						break;
					}
					s8 = ( s8 + st8 ) & capM8;
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
		return SwVizCostTerm( 1.0, SWVC_OTHER );	// samples-disabled
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
			InterlockedAdd( u_WalkCnt[ 12 ], 1u );	// bucket: umbra fragment (sentinel exit never reaches a walker's
													// flush; without this the buckets under-count slot 7 by exactly
													// the sentinel threads and the lit/pen/umb split lies)
#endif
			// whole tile provably in umbra: the integral saturates to 1 for every receiver here
			return SwVizCostTerm( SwHoistTerm( 0.0f, swHoist ), SWVC_UMBRA );
		}
		// WEDGE TERM (r_softShadowWedgeWhitelist 3): the whole-stream wedge coverage is the term. EMPTY-TILE
		// SKIP is the perf lever: an empty face-caster tile (swCnt==0) has no caster near this tile => the
		// wedge is provably 0 there too (lit), so short-circuit WITHOUT walking the block. The face-caster
		// bins the scanline builds are shared truth (same casters, both representations), so tile emptiness
		// applies to the wedge. Only tiles with casters (and the spill/degrade fallbacks) pay the walk -
		// the fully-lit scissor majority is skipped, the way the scanline's FaceCoverageList(0) skips it.
		if( g_wedge.x >= 0 && ( g_wedge.z == 3 || g_wedge.z >= 30 ) )
		{
			if( swCnt == 0u )
			{
#if SW_GPU_WALK_COUNTERS
				InterlockedAdd( u_WalkCnt[ 17 ], 1u );	// empty-list (provably lit, zero iterations)
#endif
				return SwVizCostTerm( SwHoistTerm( 1.0f, swHoist ), SWVC_OTHER );	// provably lit: no wedge walk
			}
			// CULL-ONLY (z==34, r_softShadowWedgeAblate 5): full-N walk, sphere-cull only, ZERO edge math.
			// This subtracts from the mode-4 (whole-block) baseline to split cull-cost vs edge-cost.
			if( g_wedge.z == 34 )
			{
				const float swClOcc = SoftShadow_WedgeCullCost( swP, swL, swR, g_wedge.x, g_wedge.y );
				return SwVizCostTerm( SwTermQuant( SwHoistTerm( 1.0f - saturate( swClOcc ), swHoist ) ), SWVC_HIT );
			}
			// TILE-BINNED WEDGE (z==3): walk only the casters the binner listed for this tile (screen-footprint
			// culled), not the whole ~19-caster stream. The tile list is the wedge caster bin (analytic append);
			// SPILL = a bump-allocated span, 0xFFFFFFFF (degrade) = fall back to the whole block.
			if( g_wedge.z == 3 )
			{
				float swWlOcc;
				if( swCnt == SW_TILE_SPILL )
				{
					const uint swOfs = t_SoftTiles[ swSlot + 1 ];
					const uint swSpN = min( t_SoftTiles[ swSlot + 2 ], 65536u );
					swWlOcc = SwWedgeTileOcc( swP, swL, swR, ( int )swOfs, ( int )swSpN );
				}
				else if( swCnt != 0xFFFFFFFFu )
				{
					swWlOcc = SwWedgeTileOcc( swP, swL, swR, swSlot + 1, ( int )swCnt );
				}
				else
				{
					swWlOcc = SoftShadow_WedgeOcclusion( swP, swL, swR, g_wedge.x, g_wedge.y, 0.0 );	// degrade: whole block
				}
				return SwVizCostTerm( SwTermQuant( SwHoistTerm( 1.0f - saturate( swWlOcc ), swHoist ) ), SWVC_HIT );
			}
			// ABLATION (r_softShadowWedgeAblate, z = 30 + step): whole-stream record-count scaling to linear-fit
			// the term cost - tile-independent by design (N=0 z30 = pure overhead ... N z33 = full walk).
			const int   swAblN  = ( g_wedge.y * ( g_wedge.z - 30 ) ) / 3;
			const float swAblOcc = SoftShadow_WedgeOcclusion( swP, swL, swR, g_wedge.x, swAblN, 0.0 );
			return SwVizCostTerm( SwTermQuant( SwHoistTerm( 1.0f - saturate( swAblOcc ), swHoist ) ), SWVC_HIT );
		}
		// WEDGE-WHITELIST SERVE (r_softShadowWedgeWhitelist 2): this fragment passed every early-out
		// and the tile umbra sentinel, so it WOULD pay a walk (spill/list/full) - try serving it from
		// the clean-caster wedge areas + cut-caster scanline first (see SwWlTryServe for the bounds
		// argument). A failed attempt falls through to the existing walk paths completely unchanged;
		// modes 0/1 never enter (g_wedge.z < 2), keeping their output byte-identical to today.
		if( g_wedge.x >= 0 && g_wedge.z == 2 )
		{
			float swWlTerm;
			if( SwWlTryServe( swP, swL, swR, swRotAng, swHoist, swWlTerm ) )
			{
				return swWlTerm;
			}
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
		// WEDGE-REPAIR / WEDGE-WHITELIST SERVE, unbinned-light leg (no tile grid this light - the
		// contention fallback): same modes as the binned leg above; there is no umbra sentinel to
		// consult here, so every fragment on this leg would pay the full walk regardless.
		if( g_wedge.x >= 0 && ( g_wedge.z == 3 || g_wedge.z >= 30 ) )
		{
			if( g_wedge.z == 34 )	// mode 34 = cull-only, full N (unbinned leg mirrors the binned leg)
			{
				const float swClOcc = SoftShadow_WedgeCullCost( swP, swL, swR, g_wedge.x, g_wedge.y );
				return SwVizCostTerm( SwTermQuant( SwHoistTerm( 1.0f - saturate( swClOcc ), swHoist ) ), SWVC_HIT );
			}
			int swAblN = g_wedge.y;
			if( g_wedge.z >= 30 ) { swAblN = ( g_wedge.y * ( g_wedge.z - 30 ) ) / 3; }	// ablation (see binned leg)
			const float swWlOcc = SoftShadow_WedgeOcclusion( swP, swL, swR, g_wedge.x, swAblN, 0.0 );
			return SwVizCostTerm( SwTermQuant( SwHoistTerm( 1.0f - saturate( swWlOcc ), swHoist ) ), SWVC_HIT );
		}
		if( g_wedge.x >= 0 && g_wedge.z == 2 )
		{
			float swWlTerm;
			if( SwWlTryServe( swP, swL, swR, swRotAng, swHoist, swWlTerm ) )
			{
				return swWlTerm;
			}
		}
		swOcc = SoftShadow_Coverage( swP, swL, swR, g_range.x, g_flags.y, g_range.y, 0.0, true, swRotAng );
	}

#if SW_SURF_CACHE
	if( ( int )g_surfParams.y == 4 )
	{
		// COST-CLASS heatmap, non-hit fragments: miss 0.25, walk-always 0.5, anchor-reject 0.75
		// (hit is full-bright, tinted on the hit path). Shows WHERE the overhead-payers cluster.
		const float swClassLvl[4] = { 1.0f, 0.25f, 0.5f, 0.75f };
		return swClassLvl[ min( swVizClass, 3 ) ];
	}
#endif
	return SwVizCostTerm( SwTermQuant( SwHoistTerm( 1.0 - saturate( swOcc ), swHoist ) ), SWVC_WALK );	// plain/list/spill walk (fills==0 = empty tile list)
}

// the old main body, whole: prelude, then the walk. Bit-identical value per pixel.
float SwTermFinal( int2 px )
{
	float4 swPos;
	float swHoist, swT;
	if( SwTermPrelude( px, swPos, swHoist, swT ) )
	{
		return swT;
	}
	return SwTermWalk( px, swPos, swHoist );
}

[numthreads( 8, 4, 1 )]
void main( uint3 tid : SV_DispatchThreadID )
{
#if SW_SUBSAMPLE
	// g_sub: x = phase | (viz << 2), y = lattice stride S, z = C2 spread epsilon, w = C3 beta
	const int sPh  = ( ( int )g_sub.x ) & 3;
	const int sViz = ( ( int )g_sub.x ) >> 2;
	const int sS   = max( ( int )g_sub.y, 2 );
	if( sPh == 1 )
	{
		// ---- PHASE A: corner lattice at stride S. Each thread evaluates the EXACT body at its
		// (clamped) corner pixel, writes the term to the atlas as usual AND ( term, wMin ) to the
		// side lattice for Phase B's criterion. wMin accumulates inside the walk via the expanded
		// cone-cull sites (SW_SUBSAMPLE_WMIN); early-out corners never walk and keep wMin = +inf
		// by design (the design's stated contract for early-out pixels).
		const int latW = ( g_rect.z + sS - 2 ) / sS + 1;	// ceil((w-1)/S)+1
		const int latH = ( g_rect.w + sS - 2 ) / sS + 1;
		if( ( int )tid.x >= latW || ( int )tid.y >= latH )
		{
			return;
		}
		const int lx = ( int )tid.x, ly = ( int )tid.y;
		const int2 px = int2( g_rect.x + min( lx * sS, g_rect.z - 1 ), g_rect.y + min( ly * sS, g_rect.w - 1 ) );
		// receiver frame + expansion radius for the wMin sites: the block world radius = max world
		// distance to the +-S pixel neighbours, so the expanded cull covers every block this corner
		// borders (S divides the tile size, so a pixel's own-tile corner always covers it).
		const float4 aPos = t_WorldPos.Load( int3( px, 0 ) );
		g_swWMinP = aPos.xyz;
		g_swWMinN = t_WorldNormal.Load( int3( px, 0 ) ).xyz;
		float aRad = 0.0f;
		if( aPos.w != 0.0f )
		{
			[unroll] for( int nb = 0; nb < 4; nb++ )
			{
				const int2 nd = ( nb == 0 ) ? int2( sS, 0 ) : ( ( nb == 1 ) ? int2( -sS, 0 ) : ( ( nb == 2 ) ? int2( 0, sS ) : int2( 0, -sS ) ) );
				const int2 np = int2( clamp( px.x + nd.x, g_rect.x, g_rect.x + g_rect.z - 1 ),
									  clamp( px.y + nd.y, g_rect.y, g_rect.y + g_rect.w - 1 ) );
				const float4 nP = t_WorldPos.Load( int3( np, 0 ) );
				if( nP.w != 0.0f )
				{
					aRad = max( aRad, length( nP.xyz - aPos.xyz ) );
				}
			}
		}
		// beta (g_sub.w) <= 0 disables the whole wMin machinery (C3 then passes as 0 >= 0):
		// both an isolation lever for the Phase-A expanded-walk cost and a "trust C1/C2 only" knob.
		g_swWMinRad = ( g_sub.w > 0.0f ) ? aRad : 0.0f;
		g_swWMin = 1e30f;
		// relevance ceiling: beta x the largest block diagonal this corner can serve (adjacent block
		// edges are +-S lattice steps, bounded by aRad; sqrt2 for the diagonal, x2 slack for the
		// blocks whose P00 is a NEIGHBOUR corner). The lattice stores min(wMin, cap): the certified
		// width floor Phase B compares against - 0 = poisoned (refine).
		g_swWMinCap = g_sub.w * 1.41421356f * 2.0f * aRad;
		const float aTerm = SwTermFinal( px );
		u_Term[ uint2( px + g_tile.zw ) ] = aTerm;
		u_SubLattice[ 4 + ly * latW + lx ] = uint2( asuint( aTerm ), asuint( min( g_swWMin, g_swWMinCap ) ) );
		SwSubStat( 3u, true );		// counter: lattice evals
		return;
	}
	if( sPh == 2 )
	{
		// ---- PHASE B: full rect. Lattice pixels were written by Phase A; the rest run the exact
		// early-outs bit-exactly, then the conservative criterion; PASS = bilerp of the 4 corner
		// terms, FAIL = fall through into the unchanged exact walk.
		if( ( int )tid.x >= g_rect.z || ( int )tid.y >= g_rect.w )
		{
			return;
		}
		const int tx = ( int )tid.x, ty = ( int )tid.y;
		const bool latX = ( tx % sS ) == 0 || tx == g_rect.z - 1;
		const bool latY = ( ty % sS ) == 0 || ty == g_rect.w - 1;
		if( latX && latY )
		{
			return;					// lattice pixel: Phase A already wrote it
		}
		const int2 px = int2( g_rect.x + tx, g_rect.y + ty );
		g_swWMinRad = 0.0f;			// Phase B never accumulates wMin (the include sites no-op)
		float4 swPos;
		float swHoist, swT;
		if( SwTermPrelude( px, swPos, swHoist, swT ) )
		{
			u_Term[ uint2( px + g_tile.zw ) ] = swT;	// early-out: bit-exact, no criterion
			return;
		}
		// tile-umbra sentinel hoist: bit-exact - under the sentinel EVERY body path (surf hit, plain
		// walk, contrib serve) writes exactly SwHoistTerm(0, swHoist), so answering it here only
		// saves the criterion loads on umbra tiles.
		{
			const int uTx = px.x / SW_TILE_SIZE - g_tile.x;
			const int uTy = px.y / SW_TILE_SIZE - g_tile.y;
			if( g_range.z >= 0 && uTx >= 0 && uTy >= 0 )
			{
				if( t_SoftTiles[ g_range.z + ( uTy * g_range.w + uTx ) * ( g_flags.w + 1 ) ] == SW_TILE_UMBRA )
				{
					u_Term[ uint2( px + g_tile.zw ) ] = SwVizCostTerm( SwHoistTerm( 0.0f, swHoist ), SWVC_UMBRA );
					return;
				}
			}
		}
		// ---- criterion: INTERPOLATE iff C1 (receiver continuity) && C2 (corner-window term
		// spread) && C3 (penumbra-width floor) over the surrounding corner lattice ----
		const int latW = ( g_rect.z + sS - 2 ) / sS + 1;
		const int latH = ( g_rect.w + sS - 2 ) / sS + 1;
		const int lx0 = tx / sS, ly0 = ty / sS;
		const int cx0 = lx0 * sS;
		const int cy0 = ly0 * sS;
		const int cx1 = min( cx0 + sS, g_rect.z - 1 );
		const int cy1 = min( cy0 + sS, g_rect.w - 1 );
		const uint2 e00 = u_SubLattice[ 4 + ly0 * latW + lx0 ];
		const uint2 e10 = u_SubLattice[ 4 + ly0 * latW + ( lx0 + 1 ) ];
		const uint2 e01 = u_SubLattice[ 4 + ( ly0 + 1 ) * latW + lx0 ];
		const uint2 e11 = u_SubLattice[ 4 + ( ly0 + 1 ) * latW + ( lx0 + 1 ) ];
		const int2 p00 = int2( g_rect.x + cx0, g_rect.y + cy0 );
		const int2 p10 = int2( g_rect.x + cx1, g_rect.y + cy0 );
		const int2 p01 = int2( g_rect.x + cx0, g_rect.y + cy1 );
		const int2 p11 = int2( g_rect.x + cx1, g_rect.y + cy1 );
		const float4 w00 = t_WorldPos.Load( int3( p00, 0 ) );
		const float4 w10 = t_WorldPos.Load( int3( p10, 0 ) );
		const float4 w01 = t_WorldPos.Load( int3( p01, 0 ) );
		const float4 w11 = t_WorldPos.Load( int3( p11, 0 ) );
		// all 4 corners rasterised, else refine
		bool sPass = ( w00.w != 0.0f ) && ( w10.w != 0.0f ) && ( w01.w != 0.0f ) && ( w11.w != 0.0f );
		const float sBlk = max( length( w10.xyz - w00.xyz ), length( w01.xyz - w00.xyz ) );
		if( sPass )
		{
			// C1: the pixel and its 4 corners lie on one continuous, like-oriented receiver
			const float3 sN = t_WorldNormal.Load( int3( px, 0 ) ).xyz;
			const float3 sP = swPos.xyz;
			const float sHTol = 0.25f * sBlk;
			sPass = abs( dot( sN, w00.xyz - sP ) ) <= sHTol && dot( sN, t_WorldNormal.Load( int3( p00, 0 ) ).xyz ) >= 0.95f
					&& abs( dot( sN, w10.xyz - sP ) ) <= sHTol && dot( sN, t_WorldNormal.Load( int3( p10, 0 ) ).xyz ) >= 0.95f
					&& abs( dot( sN, w01.xyz - sP ) ) <= sHTol && dot( sN, t_WorldNormal.Load( int3( p01, 0 ) ).xyz ) >= 0.95f
					&& abs( dot( sN, w11.xyz - sP ) ) <= sHTol && dot( sN, t_WorldNormal.Load( int3( p11, 0 ) ).xyz ) >= 0.95f;
		}
		if( sPass )
		{
			// C2: term max-min over the 4x4 corner window (own 4 + 12 surrounding, clamped)
			float tMin = 1e30f, tMax = -1e30f;
			[unroll] for( int wy = -1; wy <= 2; wy++ )
			{
				[unroll] for( int wx = -1; wx <= 2; wx++ )
				{
					const int cwx = clamp( lx0 + wx, 0, latW - 1 );
					const int cwy = clamp( ly0 + wy, 0, latH - 1 );
					const float tw = asfloat( u_SubLattice[ 4 + cwy * latW + cwx ].x );
					tMin = min( tMin, tw );
					tMax = max( tMax, tw );
				}
			}
			sPass = ( tMax - tMin ) <= g_sub.z;
		}
		if( sPass )
		{
			// C3: every corner's min penumbra width clears beta x the block diagonal - thin casters
			// and contact shadows (small cdv => tiny w) force refinement even when they shadow no
			// lattice corner (the expanded Phase-A cull saw them).
			const float sWMin = min( min( asfloat( e00.y ), asfloat( e10.y ) ), min( asfloat( e01.y ), asfloat( e11.y ) ) );
			sPass = sWMin >= g_sub.w * sBlk * 1.41421356f;
		}
		SwSubStat( 0u, true );		// counter: walked-class pixels (reached the criterion)
		SwSubStat( 1u, sPass );		// counter: interpolated
		SwSubStat( 2u, !sPass );	// counter: refined
		if( sPass )
		{
			// bilinear interpolation, weights from the ACTUAL clamped corner pixel coords
			const float fx = ( cx1 > cx0 ) ? ( float )( tx - cx0 ) / ( float )( cx1 - cx0 ) : 0.0f;
			const float fy = ( cy1 > cy0 ) ? ( float )( ty - cy0 ) / ( float )( cy1 - cy0 ) : 0.0f;
			const float tL = lerp( asfloat( e00.x ), asfloat( e10.x ), fx );
			const float tH = lerp( asfloat( e01.x ), asfloat( e11.x ), fx );
			u_Term[ uint2( px + g_tile.zw ) ] = ( sViz != 0 ) ? 0.25f : lerp( tL, tH, fy );
			return;
		}
		u_Term[ uint2( px + g_tile.zw ) ] = ( sViz != 0 ) ? 1.0f : SwTermWalk( px, swPos, swHoist );
		return;
	}
	// phase 0: exact single dispatch (same behaviour as the non-subsample permutations)
#endif	// SW_SUBSAMPLE
	if( ( int )tid.x >= g_rect.z || ( int )tid.y >= g_rect.w )
	{
		return;
	}
	const int2 px = int2( g_rect.x + ( int )tid.x, g_rect.y + ( int )tid.y );
	u_Term[ uint2( px + g_tile.zw ) ] = SwTermFinal( px );
}
