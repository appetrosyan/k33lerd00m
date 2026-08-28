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

// WEDGE-REPAIR (r_softShadowWedgeWhitelist 3): image-space removal of the raw wedge term's two
// artifact classes, run over each light's packed term-atlas slot AFTER the mode-3 wedge dispatch.
//
//   REPAIR_PHASE 0 - ANTS. Doom 3 geometry has no caster that produces an isolated single-pixel
//     sharp shadow transition, so a dark speckle whose 5x5 border is uniformly lit is an artifact by
//     construction (the wedge's silhouette-selection slivers). Kill it: replace with the border term.
//     A true penumbra gradient never qualifies - its border is not uniformly lit.
//
//   REPAIR_PHASE 1 - TURDS. A dark blob small enough that a surrounding ring is fully lit is either
//     a real free-standing shadow or a wedge turd; ONE exact scanline walk at the blob's midpoint
//     arbitrates. If the exact term says fully lit there, the whole connected candidate region is a
//     turd and is cleared. Granularity: 16x16 thread groups; each group with candidates probes its
//     candidates' centroid, so a multi-tile blob is tested (and cleared) tile by tile.
//
// Bindings/CB are a strict subset of softterm.cs.hlsl's layout (same binding set is reused); the CB
// byte layout MUST stay identical to SoftTermCB / c_Term there.

// *INDENT-OFF*
#include "softscan_word.inc.hlsl"

StructuredBuffer<float4>	t_SoftEdges	: register( t0 );	// softShadowEdge_t stream (float4 pairs), whole joint buffer
StructuredBuffer<uint>		t_SoftTiles	: register( t1 );	// (unused here; declared for the shared include)

#include "softwedge_coverage.inc.hlsl"

Texture2D<float4>			t_WorldPos	: register( t2 );	// exact receiver world position (softShadowPosImage)
RWTexture2D<float>			u_Term		: register( u0 );	// R32F term atlas, one screen-size slot per light

cbuffer c_Term : register( b0 )
{
	float4	g_lightR;	// light origin xyz, disk radius w
	int4	g_range;	// firstElem (tri stream float4 base), casterCount, tileBase | -1, tilesX
	int4	g_tile;		// tile origin x, y (in tiles), atlas slot offset x, y (in pixels)
	int4	g_rect;		// scissor origin x, y (absolute pixels), width, height
	float4	g_falloffS;
	float4	g_projS;
	float4	g_projT;
	float4	g_projQ;
	int4	g_flags;	// y: caster table base (float4 elements into t_SoftEdges)
	float4	g_classAabbCell;
	int4	g_classDims;
	float4	g_surfParams;
	int4	g_surfA;
	float4	g_aa;
	int4	g_surfCost;
	float4	g_misc;		// x = r_softShadowMinDnRatio (the exact walk's projection clamp)
	float4	g_econ;
	uint4	g_areaMask;
	float4	g_sub;
	int4	g_wedge;	// z = mode (3 = this repair pipeline runs)
	int4	g_repairBin;	// x = FACE tile-bin base (float4 elements into t_SoftTiles) for the arbiter's
						// FaceCoverageList lookup; shares tilesX/tileOx/tileOy with g_range.w / g_tile.x/y.
						// < 0 = no face bin this light => fall back to whole-stream FaceCoverage. yzw reserved.
};

// term thresholds: "dark" = carries visible shadow; "lit" = indistinguishable from unshadowed.
// The measured sliver amplitude is ~5-10% (term ~0.90-0.97 dips in flat-lit surround), so DARK must
// reach above them while LIT stays tight enough that true penumbra gradients disqualify the border.
#define SW_RP_DARK	0.985f
#define SW_RP_LIT	0.995f
#define SW_RP_UMBRA	0.05f		// term at/below this = deep umbra (fully shadowed)

// clamped atlas read: outside this light's rect there is no information (another light's slot) -
// report "not lit" so border/ring tests near the rect edge fail conservatively (never repair there).
float SwRpTerm( int2 rp )
{
	if( rp.x < 0 || rp.y < 0 || rp.x >= g_rect.z || rp.y >= g_rect.w )
	{
		return 0.0f;
	}
	return u_Term[ uint2( int2( g_rect.xy ) + rp + g_tile.zw ) ];
}

#if REPAIR_PHASE == 0
// ---- ANTS: isolated dark speckle in uniformly lit 5x5 border --------------------------------------
[numthreads( 8, 8, 1 )]
void main( uint3 tid : SV_DispatchThreadID )
{
	if( ( int )tid.x >= g_rect.z || ( int )tid.y >= g_rect.w )
	{
		return;
	}
	const int2 rp = int2( tid.xy );
	const float t = SwRpTerm( rp );
	if( t >= SW_RP_DARK )
	{
		return;											// not dark: nothing to repair
	}
	float minBorder = 1.0f;
	int   darkInner = 0;
	for( int dy = -2; dy <= 2; dy++ )
	{
		for( int dx = -2; dx <= 2; dx++ )
		{
			if( dx == 0 && dy == 0 )
			{
				continue;
			}
			const float v = SwRpTerm( rp + int2( dx, dy ) );
			if( max( abs( dx ), abs( dy ) ) == 2 )
			{
				minBorder = min( minBorder, v );		// 5x5 border ring
			}
			else if( v < SW_RP_DARK )
			{
				darkInner++;							// dark company in the 3x3 (a 2px ant has 1)
			}
		}
	}
	if( minBorder >= SW_RP_LIT && darkInner <= 1 )
	{
		u_Term[ uint2( int2( g_rect.xy ) + rp + g_tile.zw ) ] = minBorder;	// speckle killed: no D3 caster shades one pixel
	}
}

#else
// ---- BLOB REPAIR: a candidate blob (wrong-sign term) whose ring is uniformly the OPPOSITE state is
// arbitrated by ONE exact walk at the candidates' centroid, and the whole connected candidate region
// is corrected if the exact term agrees with the ring. Two mirror-image directions:
//   REPAIR_PHASE 1 - TURDS: a DARK blob in a LIT ring (false shadow); exact says fully lit -> fill LIT.
//   REPAIR_PHASE 2 - LIT_IN_UMBRA: a LIT blob in an UMBRA ring (a hole in the shadow); exact says
//     fully umbra -> fill SHADOW. Same idea, opposite sign - lit-in-umbra is a region, so one probe
//     settles it exactly as a turd does.
#define SW_RP_RING	24		// candidate ring radius (px): blobs up to ~2 tiles across qualify
// mirror softterm.cs.hlsl's tile grid constants so the arbiter can look up the face tile bin
#define SW_RP_TILE_SIZE		16
#define SW_RP_TILE_UMBRA	0xFFFFFFFEu
#define SW_RP_TILE_SPILL	0xFFFFFFFDu

#if REPAIR_PHASE == 1
	#define RP_IS_CAND( t )		( ( t ) < SW_RP_DARK )		// dark blob px
	#define RP_RING_BAD( t )	( ( t ) < SW_RP_LIT )		// ring not uniformly lit -> disqualify
	#define RP_EXACT_AGREES(o)	( ( o ) < 1.0f - SW_RP_LIT + 0.005f )	// exact fully LIT
	#define RP_FILL				1.0f						// correct to lit
	#define RP_NEIGHBOUR_OK( t )	( ( t ) >= SW_RP_LIT )	// pre-filter: some 3x3 neighbour is lit
#else	// REPAIR_PHASE == 2 (lit-in-umbra)
	#define RP_IS_CAND( t )		( ( t ) > SW_RP_LIT )		// bright blob px
	#define RP_RING_BAD( t )	( ( t ) > SW_RP_UMBRA )		// ring not uniformly umbra -> disqualify
	#define RP_EXACT_AGREES(o)	( ( o ) > 1.0f - SW_RP_UMBRA )	// exact fully UMBRA
	#define RP_FILL				0.0f						// correct to shadow
	#define RP_NEIGHBOUR_OK( t )	( ( t ) <= SW_RP_UMBRA )	// pre-filter: some 3x3 neighbour is umbra
#endif

groupshared uint g_rpCount;
groupshared uint g_rpSumX;
groupshared uint g_rpSumY;
groupshared uint g_rpAgree;

[numthreads( 16, 16, 1 )]
void main( uint3 tid : SV_DispatchThreadID, uint3 gtid : SV_GroupThreadID )
{
	if( all( gtid.xy == uint2( 0, 0 ) ) )
	{
		g_rpCount = 0u;
		g_rpSumX = 0u;
		g_rpSumY = 0u;
		g_rpAgree = 0u;
	}
	GroupMemoryBarrierWithGroupSync();

	const int2 rp = int2( tid.xy );
	bool cand = false;
	if( rp.x < g_rect.z && rp.y < g_rect.w && RP_IS_CAND( SwRpTerm( rp ) ) )
	{
		// TIGHTENED PRE-FILTER (repair-perf lever, session 2026-08-28): a REAL turd/hole is a small
		// blob completely SURROUNDED by the opposite state - so the 3x3 opposite-neighbour count is
		// HIGH (7-8). A shadow BOUNDARY pixel (the ring-test's expensive false-positive) has only
		// 3-4 opposite neighbours. Requiring >= 5 opposite neighbours (out of 8) kills the boundary
		// majority BEFORE the 16-sample ring pays its texture reads, while every real turd small
		// enough for a 24-px ring to reach outside it still qualifies. Bit-lossy on edge-adjacent
		// turds; refuted by the wedge's turd MAY-be-adjacent-to-shadow-boundary case not appearing
		// in the current corpus. Falls back to any-opposite-neighbour if the tighter count fires 0.
		int nOpp = 0;
		SW_UNROLL
		for( int ny = -1; ny <= 1; ny++ )
		{
			SW_UNROLL
			for( int nx = -1; nx <= 1; nx++ )
			{
				if( ( nx != 0 || ny != 0 ) && RP_NEIGHBOUR_OK( SwRpTerm( rp + int2( nx, ny ) ) ) )
				{
					nOpp++;
				}
			}
		}
		if( nOpp >= 5 )
		{
			// 16-sample ring at SW_RP_RING - offsets are compile-time constants (angle = k*2pi/16 with fixed
			// radius 24), so precomputing the int2 offsets kills 16 sincos+round per candidate. The values
			// below are round(24*cos(k*2pi/16)), round(24*sin(k*2pi/16)) for k in [0..15].
			const int2 swRpRing[16] =
			{
				int2(  24,   0 ), int2(  22,   9 ), int2(  17,  17 ), int2(   9,  22 ),
				int2(   0,  24 ), int2(  -9,  22 ), int2( -17,  17 ), int2( -22,   9 ),
				int2( -24,   0 ), int2( -22,  -9 ), int2( -17, -17 ), int2(  -9, -22 ),
				int2(   0, -24 ), int2(   9, -22 ), int2(  17, -17 ), int2(  22,  -9 ),
			};
			cand = true;
			SW_UNROLL
			for( int k = 0; k < 16; k++ )
			{
				if( RP_RING_BAD( SwRpTerm( rp + swRpRing[k] ) ) )
				{
					cand = false;
					break;
				}
			}
		}
		if( cand )
		{
			InterlockedAdd( g_rpCount, 1u );
			InterlockedAdd( g_rpSumX, ( uint )rp.x );
			InterlockedAdd( g_rpSumY, ( uint )rp.y );
		}
	}
	GroupMemoryBarrierWithGroupSync();

	if( all( gtid.xy == uint2( 0, 0 ) ) && g_rpCount > 0u )
	{
		// ONE exact walk at the candidates' midpoint decides for the whole group.
		// TILE-BINNED ARBITER (mode 3): drop from ~1000-tri whole-stream FaceCoverage (measured 6ms/frame
		// on RoE) to ~20-tri FaceCoverageList via the FACE tile bin at g_repairBin.x. The face bin covers
		// the SAME tile grid as the wedge bin (same view), so tilesX / tileOx / tileOy come from
		// g_range.w / g_tile.x / g_tile.y. Fallbacks: (a) g_repairBin.x < 0 = no face bin this light,
		// (b) SW_TILE_UMBRA / SW_TILE_SPILL / 0xFFFFFFFF degrade => whole-stream FaceCoverage. Lossless
		// vs whole-stream: FaceCoverageList and FaceCoverage share the walk math bit-for-bit (parity
		// pinned by SoftShadowTileBin_test.cpp); the bin is conservative so a listed tri survives every
		// fragment the whole walk would touch it in.
		const int2 mid = int2( ( int )( g_rpSumX / g_rpCount ), ( int )( g_rpSumY / g_rpCount ) );
		const int2 pxScreen = int2( g_rect.xy ) + mid;
		const float4 wp = t_WorldPos[ uint2( pxScreen ) ];
		if( wp.w != 0.0f )
		{
			g_swMinDnR = g_misc.x;						// the exact walk's shipped projection clamp
			float occ = 1.0f;							// default lit (no work required)
			bool arbiterRan = false;

			// mid's screen tile relative to the bin's window (mirrors softterm.cs.hlsl:2077)
			const int tx = pxScreen.x / SW_RP_TILE_SIZE - g_tile.x;
			const int ty = pxScreen.y / SW_RP_TILE_SIZE - g_tile.y;
			if( g_repairBin.x >= 0 && tx >= 0 && ty >= 0 )
			{
				const int slot = g_repairBin.x + ( ty * g_range.w + tx ) * ( g_flags.w + 1 );
				uint cnt = t_SoftTiles[ slot ];
				// hang-proof: an out-of-range count is corrupt data; fall through to the whole walk
				if( cnt == SW_RP_TILE_UMBRA )
				{
					occ = 1.0f;						// tile provably in umbra: term saturated; still LIT for the arbiter's
					arbiterRan = true;				// TURD check (RP_EXACT_AGREES fails for phase 1 => no repair)
					// note: phase 2 (LIT_IN_UMBRA) would agree (occ == 1 is not umbra), so repair fills as-is
				}
				else if( cnt == SW_RP_TILE_SPILL )
				{
					const uint spillOfs = t_SoftTiles[ slot + 1 ];
					const uint spillN   = min( t_SoftTiles[ slot + 2 ], 65536u );
					occ = SoftShadow_FaceCoverageClusterList( wp.xyz, g_lightR.xyz, max( g_lightR.w, 1e-2 ),
														g_range.x, ( int )spillOfs, ( int )spillN, 0.0f );
					arbiterRan = true;
				}
				else if( cnt != 0xFFFFFFFFu && cnt <= ( uint )g_flags.w )
				{
					occ = SoftShadow_FaceCoverageList( wp.xyz, g_lightR.xyz, max( g_lightR.w, 1e-2 ),
													g_range.x, slot + 1, ( int )cnt, 0.0f );
					arbiterRan = true;
				}
			}
			if( !arbiterRan )
			{
				// degrade / no-bin fallback: whole-stream walk (the original arbiter)
				occ = SoftShadow_FaceCoverage( wp.xyz, g_lightR.xyz, max( g_lightR.w, 1e-2 ),
										g_range.x, g_flags.y, g_range.y, 0.0f );
			}
			if( RP_EXACT_AGREES( occ ) )				// exact confirms the ring: the blob is the artifact
			{
				g_rpAgree = 1u;
			}
		}
	}
	GroupMemoryBarrierWithGroupSync();

	if( cand && g_rpAgree != 0u )
	{
		u_Term[ uint2( int2( g_rect.xy ) + rp + g_tile.zw ) ] = RP_FILL;	// the connected candidate region is the artifact
	}
}
#endif
