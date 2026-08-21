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
RWStructuredBuffer<uint>	u_SurfQueue	: register( u3 );
StructuredBuffer<uint>		t_SurfPool	: register( t6 );	// read before the include: the residual walk consumes it

// order-preserving float->uint encoding for the texel anchor (word 7): anchors accumulate via
// InterlockedMin so the value is the MIN height over all contributors - order-independent, so the
// claim-race winner cannot leak into the cached value. MUST match softsurf_seed/softsurf_build.
uint SwSurfFlipF( float f )
{
	const uint u = asuint( f );
	return ( u & 0x80000000u ) ? ~u : ( u | 0x80000000u );
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
};
// *INDENT-ON*

[numthreads( 8, 4, 1 )]
void main( uint3 tid : SV_DispatchThreadID )
{
	if( ( int )tid.x >= g_rect.z || ( int )tid.y >= g_rect.w )
	{
		return;
	}
	const int2 px = int2( g_rect.x + ( int )tid.x, g_rect.y + ( int )tid.y );

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
	// Cleared by EndBuilds each frame; read by the bench. Cheap enough to keep always-on in the
	// probe permutation (one InterlockedAdd per fragment).
#define SW_SURF_STAT( idx ) InterlockedAdd( u_SurfTable[ ( uint )g_surfA.x * 8u + ( idx ) ], 1u )
	if( g_surfA.x > 0 && g_surfA.z > 0 && swPos.w != 0.0f )
	{
		const float3 swN2 = t_WorldNormal.Load( int3( px, 0 ) ).xyz;
		const float3 an = abs( swN2 );
		const int   d  = ( an.x >= an.y && an.x >= an.z ) ? 0 : ( ( an.y >= an.z ) ? 1 : 2 );
		const float nd = ( d == 0 ) ? swN2.x : ( ( d == 1 ) ? swN2.y : swN2.z );
		// SIGN-AGNOSTIC dominant axis (no |4 sign bit): the prewarm SEED keys receiver texels off the
		// tri's GEOMETRIC normal (winding-dependent sign), while this reads the SHADING normal - an
		// opposite sign made every warm read miss. The anchor height is sign-invariant, and back-facing
		// receivers early-out (N.L<=0), so dropping the sign is lossless and aligns seed and read keys.
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
			if( keyLo != 0xFFFFFFFFu )	// the far world-corner cell aliases the empty sentinel: never cached
			{
				const uint capM = ( uint )g_surfA.x - 1u;	// capacity is a power of two (CPU-enforced)
				const uint h = keyLo * 0x9E3779B1u ^ keyHi * 0x85EBCA77u;
				uint slot = h & capM;
				for( int pr = 0; pr < 16; pr++ )
				{
					const uint sBase = slot * 8u;
					const uint w0 = u_SurfTable[ sBase ];
					if( w0 == keyLo && u_SurfTable[ sBase + 1 ] == keyHi )
					{
						const uint s2 = u_SurfTable[ sBase + 2 ];
						const uint code = s2 & 3u;
						if( ( s2 >> 2u ) != curGen )
						{
							// STALE generation: this light's static set changed, so any record under this
							// key is from a prior generation. Reclaim in place (same cell+light, never
							// another cell's slot). Winner re-requests + re-anchors; a lost anchor race at
							// worst yields ANCHOR-REJECT -> exact walk, never a wrong shadow.
							uint prevS;
							InterlockedCompareExchange( u_SurfTable[ sBase + 2 ], s2, ( curGen << 2 ) | 1u, prevS );
							if( prevS == s2 )
							{
								u_SurfTable[ sBase + 7 ] = SwSurfFlipF( pw );
								uint rqi;
								InterlockedAdd( u_SurfQueue[ 0 ], 1u, rqi );
								if( rqi + 1u >= ( uint )g_surfA.y ) { u_SurfTable[ sBase + 2 ] = ( curGen << 2 ) | 3u; }
								else { u_SurfQueue[ 1u + rqi ] = slot; }
							}
							else
							{
								InterlockedMin( u_SurfTable[ sBase + 7 ], SwSurfFlipF( pw ) );
							}
							break;		// exact miss this frame; rebuilt next
						}
						if( code == 2u )	// BUILT: consume
						{
							// ANCHOR-PROXIMITY GUARD: two surfaces inside the same G-height slab share
							// this key (floor + step, tabletop + crate base) and the record was built
							// at the LOWER plane (min-anchor). A fragment far from the built plane
							// consuming it renders the under-geometry shadow onto the surface above -
							// texel-aligned black rectangles (playtest-observed 2026-08-20). Serve the
							// cache only near the built plane; everything else takes the exact walk.
							const uint aEnc = u_SurfTable[ sBase + 7 ];
							const float aH = asfloat( ( aEnc & 0x80000000u ) ? ( aEnc ^ 0x80000000u ) : ~aEnc );
							if( abs( pw - aH ) > g * 0.25f )
							{
								swStatIdx = 3u;	// ANCHOR-REJECT: wrong surface for this record
								break;			// exact miss path
							}
							const uint resOfs = u_SurfTable[ sBase + 3 ];
							const uint resCnt = u_SurfTable[ sBase + 4 ];
							const uint f01 = u_SurfTable[ sBase + 5 ];
							const uint f23 = u_SurfTable[ sBase + 6 ];
							const float fu = pu / g - floor( pu / g );
							const float fv = pv / g - floor( pv / g );
							const float fLo = lerp( f16tof32( f01 & 0xFFFFu ), f16tof32( f01 >> 16 ), fu );
							const float fHi = lerp( f16tof32( f23 & 0xFFFFu ), f16tof32( f23 >> 16 ), fu );
							const float fFold = lerp( fLo, fHi, fv );
							const float occEx = SoftShadow_FaceCoverageSurfResidual(
													swP, g_lightR.xyz, max( g_lightR.w, 1e-2 ), g_range.x,
													( int )resOfs, ( int )resCnt,
													g_flags.y, g_surfA.z, g_range.y, swRotAng );
							float swTermC = 1.0 - saturate( fFold + occEx );
							const int viz = ( int )g_surfParams.y;
							if( viz == 1 && ( ( ( cu ^ cv ^ cw ) & 1 ) != 0 ) )
							{
								swTermC *= 0.85;	// texel-parity checker: seams/structure inspection
							}
							else if( viz == 2 )
							{
								swTermC *= 0.75;	// uniform: which pixels the cache serves
							}
							SW_SURF_STAT( 0u );		// cached HIT
							u_Term[ uint2( px + g_tile.zw ) ] = swTermC;
							return;
						}
						if( code == 1u )
						{
							// REQUESTED, not yet built: contribute this fragment's height to the
							// deterministic min-anchor (order-independent over the texel's fragments)
							InterlockedMin( u_SurfTable[ sBase + 7 ], SwSurfFlipF( pw ) );
						}
						else if( code == 3u )
						{
							swStatIdx = 2u;			// WALK-ALWAYS (self-gate / overflow rejected the texel)
						}
						break;						// requested / walk-always: exact miss path
					}
					if( w0 == 0xFFFFFFFFu )
					{
						uint prev;
						InterlockedCompareExchange( u_SurfTable[ sBase ], 0xFFFFFFFFu, keyLo, prev );
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
							InterlockedAdd( u_SurfQueue[ 0 ], 1u, qi );
							if( qi + 1u < ( uint )g_surfA.y )
							{
								u_SurfQueue[ 1u + qi ] = slot;
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
						break;						// claimed (by us or a racer): exact miss path this frame
					}
					slot = ( slot + 1u ) & capM;	// occupied by another key: linear probe
				}
			}
			SW_SURF_STAT( swStatIdx );			// fall-through: miss / walk-always / anchor-reject
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
			// whole tile provably in umbra: the integral saturates to 1 for every receiver here
			u_Term[ uint2( px + g_tile.zw ) ] = 0.0f;
			return;
		}
		if( swCnt == SW_TILE_SPILL )
		{
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

	u_Term[ uint2( px + g_tile.zw ) ] = 1.0 - saturate( swOcc );
}
