/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 2026 Aleksandr Petrosyan

===========================================================================
*/

// FUSED REPAIR (r_softShadowRepairFused, softrepair_fused.cs.hlsl, session 2026-08-28-b): the
// shipped three-dispatch repair chain (ants -> turds -> holes; softrepair.cs.hlsl phases 0/1/2)
// pays three setComputeState transitions, three UAV barriers, and three group-launch waves per
// light. This single kernel runs all three phases in one 16x16 group with LDS barriers between
// them, halving dispatch overhead and sharing the per-pixel texture reads.
//
// LDS predicate: a pixel classified as an ant-candidate is written in-place BEFORE the turd/hole
// stage samples the ring, so a corrected ant does not trigger a stale turd/hole probe. A
// DeviceMemoryBarrier separates the ants writes from the turd/hole reads (a group-scope barrier
// alone would leave the ring's out-of-group reads racing the atlas). Ring reads across group
// boundaries still see the OTHER groups' pre-ants values, matching the shipped semantics (the
// three passes were not previously separated by a cross-group barrier either - each pass runs
// as one dispatch, and reads from a still-being-written neighbouring tile were racing already).
//
// The turd/hole arbiter runs in the same group as its collectors; thread 0 fires up to TWO
// FaceCoverageList calls per group (turd + hole) instead of the shipped model's one per phase.
// Groups with zero cand pixels bail after the first LDS-OR gate (mirroring task A).

// *INDENT-OFF*
#include "softscan_word.inc.hlsl"

StructuredBuffer<float4>	t_SoftEdges	: register( t0 );
StructuredBuffer<uint>		t_SoftTiles	: register( t1 );

#include "softwedge_coverage.inc.hlsl"

Texture2D<float4>			t_WorldPos	: register( t2 );
RWTexture2D<float>			u_Term		: register( u0 );

cbuffer c_Term : register( b0 )
{
	float4	g_lightR;
	int4	g_range;
	int4	g_tile;
	int4	g_rect;
	float4	g_falloffS;
	float4	g_projS;
	float4	g_projT;
	float4	g_projQ;
	int4	g_flags;
	float4	g_classAabbCell;
	int4	g_classDims;
	float4	g_surfParams;
	int4	g_surfA;
	float4	g_aa;
	int4	g_surfCost;
	float4	g_misc;
	float4	g_econ;
	uint4	g_areaMask;
	float4	g_sub;
	int4	g_wedge;
	int4	g_repairBin;
};

// STATS census (r_softShadowRepairStats): populate the SAME 16-uint buffer the split shader uses,
// so probes read one place. The fused kernel writes phase-1 slots (turds) at base 0 and phase-2
// slots (holes) at base 8; ants stats stay unattributed here (ants are cheap and their attribution
// is not the target of this exercise).
#ifndef SW_REPAIR_STATS
	#define SW_REPAIR_STATS 0
#endif
#if SW_REPAIR_STATS
	RWStructuredBuffer<uint>	u_RepStats	: register( u1 );
	#define REP_STATS_BUMP( base, slot )		{ uint _u; InterlockedAdd( u_RepStats[( base ) + ( slot )], 1u, _u ); }
#else
	#define REP_STATS_BUMP( base, slot )		do {} while (0)
#endif

#define SW_RP_DARK	0.985f
#define SW_RP_LIT	0.995f
#define SW_RP_UMBRA	0.05f

#define SW_RP_RING	24
#define SW_RP_TILE_SIZE		16
#define SW_RP_TILE_UMBRA	0xFFFFFFFEu
#define SW_RP_TILE_SPILL	0xFFFFFFFDu

float SwRpTerm( int2 rp )
{
	if( rp.x < 0 || rp.y < 0 || rp.x >= g_rect.z || rp.y >= g_rect.w )
	{
		return 0.0f;
	}
	return u_Term[ uint2( int2( g_rect.xy ) + rp + g_tile.zw ) ];
}

// LDS arbiter state — SEPARATE T (turd) and H (hole) pools so both can fire once per group.
// NOTE: the union-undercount (U) per-fragment arbiter tried here (session 2026-08-28-e) was
// REVERTED — it ran a FaceCoverage walk per lit-quant fragment and tripled frame time
// (17.5 -> 61.9 ms). Max-combine union undercount must be fixed in the wedge itself.
groupshared uint g_rpCountT;
groupshared uint g_rpSumXT;
groupshared uint g_rpSumYT;
groupshared uint g_rpAgreeT;
groupshared uint g_rpCountH;
groupshared uint g_rpSumXH;
groupshared uint g_rpSumYH;
groupshared uint g_rpAgreeH;
groupshared uint g_rpMaybe;		// task-A LDS-OR gate (cheap early-out)

[numthreads( 16, 16, 1 )]
void main( uint3 tid : SV_DispatchThreadID, uint3 gtid : SV_GroupThreadID )
{
	if( all( gtid.xy == uint2( 0, 0 ) ) )
	{
		g_rpCountT = 0u; g_rpSumXT = 0u; g_rpSumYT = 0u; g_rpAgreeT = 0u;
		g_rpCountH = 0u; g_rpSumXH = 0u; g_rpSumYH = 0u; g_rpAgreeH = 0u;
		g_rpMaybe  = 0u;
		REP_STATS_BUMP( 0, 0 );				// phase-1 slot: groupsDispatched (fused group)
		REP_STATS_BUMP( 8, 0 );				// phase-2 slot: groupsDispatched (mirror)
	}
	GroupMemoryBarrierWithGroupSync();

	const int2 rp = int2( tid.xy );
	const bool inRect = ( rp.x < g_rect.z && rp.y < g_rect.w );

	// ---- STAGE 1: ANTS (per-pixel, no LDS) ---------------------------------------------------
	// Local 5x5 border test: any dark speckle whose 5x5 border is uniformly lit and whose 3x3
	// inner has <=1 dark neighbour is an ant (wedge sliver, isolated 1-2 px). Writes u_Term
	// in place so the subsequent turd stage sees the corrected value at this pixel.
	if( inRect )
	{
		const float t = SwRpTerm( rp );
		if( t < SW_RP_DARK )
		{
			float minBorder = 1.0f;
			int   darkInner = 0;
			SW_UNROLL
			for( int dy = -2; dy <= 2; dy++ )
			{
				SW_UNROLL
				for( int dx = -2; dx <= 2; dx++ )
				{
					if( dx == 0 && dy == 0 )
					{
						continue;
					}
					const float v = SwRpTerm( rp + int2( dx, dy ) );
					if( max( abs( dx ), abs( dy ) ) == 2 )
					{
						minBorder = min( minBorder, v );
					}
					else if( v < SW_RP_DARK )
					{
						darkInner++;
					}
				}
			}
			if( minBorder >= SW_RP_LIT && darkInner <= 1 )
			{
				u_Term[ uint2( int2( g_rect.xy ) + rp + g_tile.zw ) ] = minBorder;
			}
		}
	}
	// Device-scope barrier so ants writes reach memory before turd/hole reads sample them
	// (the group's texture reads route through the SRV cache, which sees UAV writes only after
	// this fence). Group-scope sync so all threads observe the barrier ordering.
	DeviceMemoryBarrierWithGroupSync();

	// ---- STAGE 2a: TURD/HOLE cand pre-gate (task A) ------------------------------------------
	// One tex read per thread; LDS-OR bail for groups with zero cand pixels. Post-ants term.
	const float myT = inRect ? SwRpTerm( rp ) : 0.5f;
	const bool isTurdCandRaw = inRect && ( myT < SW_RP_DARK );
	const bool isHoleCandRaw = inRect && ( myT > SW_RP_LIT );
	if( isTurdCandRaw || isHoleCandRaw )
	{
		g_rpMaybe = 1u;
	}
	GroupMemoryBarrierWithGroupSync();
	if( g_rpMaybe == 0u )
	{
		if( all( gtid.xy == uint2( 0, 0 ) ) )
		{
			REP_STATS_BUMP( 0, 1 );			// phase-1 groupsBailed
			REP_STATS_BUMP( 8, 1 );			// phase-2 groupsBailed
		}
		return;
	}

	if( isTurdCandRaw )
	{
		REP_STATS_BUMP( 0, 2 );				// phase-1 candPixels
	}
	if( isHoleCandRaw )
	{
		REP_STATS_BUMP( 8, 2 );				// phase-2 candPixels
	}

	// Compile-time constant ring (angle = k*2pi/16, radius 24)
	const int2 swRpRing[16] =
	{
		int2(  24,   0 ), int2(  22,   9 ), int2(  17,  17 ), int2(   9,  22 ),
		int2(   0,  24 ), int2(  -9,  22 ), int2( -17,  17 ), int2( -22,   9 ),
		int2( -24,   0 ), int2( -22,  -9 ), int2( -17, -17 ), int2(  -9, -22 ),
		int2(   0, -24 ), int2(   9, -22 ), int2(  17, -17 ), int2(  22,  -9 ),
	};

	// ---- STAGE 2b: TURD predicate (nOpp == 8 tightened, ring fully lit) ----------------------
	bool turdCand = false;
	if( isTurdCandRaw )
	{
		int nOppT = 0;
		SW_UNROLL
		for( int ny = -1; ny <= 1; ny++ )
		{
			SW_UNROLL
			for( int nx = -1; nx <= 1; nx++ )
			{
				if( ( nx != 0 || ny != 0 ) && SwRpTerm( rp + int2( nx, ny ) ) >= SW_RP_LIT )
				{
					nOppT++;
				}
			}
		}
		if( nOppT == 8 )
		{
			turdCand = true;
			SW_UNROLL
			for( int k = 0; k < 16; k++ )
			{
				if( SwRpTerm( rp + swRpRing[k] ) < SW_RP_LIT )
				{
					turdCand = false;
					break;
				}
			}
		}
		if( turdCand )
		{
			InterlockedAdd( g_rpCountT, 1u );
			InterlockedAdd( g_rpSumXT, ( uint )rp.x );
			InterlockedAdd( g_rpSumYT, ( uint )rp.y );
		}
	}

	// ---- STAGE 2c: HOLE predicate (LOOSENED, session 2026-08-28-c gate-driven fix) ------------
	// Gate on the fused build showed LIT_IN_UMBRA=958 residual - most affected lights carry
	// 20-150 lit-in-umbra pixels each. Distribution suggests penumbra-adjacent lit patches where
	// the strict "3x3 nOpp>=5 fully-umbra AND all 16 ring samples fully-umbra" predicate rejects.
	// Two loosenings, both validated by the arbiter (a false positive fires the arbiter walk,
	// which either agrees -> real repair or disagrees -> no write):
	//   (a) nOpp threshold "in the darker half" (t < 0.5) instead of strictly umbra (t <= 0.05);
	//   (b) ring check tolerates up to 2 of 16 samples non-umbra (penumbra transition edge).
	// Cost: more arbiter walks. The arbiter branch was ~700/frame; expected uplift ~10-30x.
	bool holeCand = false;
	if( isHoleCandRaw )
	{
		int nOppH = 0;
		SW_UNROLL
		for( int ny = -1; ny <= 1; ny++ )
		{
			SW_UNROLL
			for( int nx = -1; nx <= 1; nx++ )
			{
				if( ( nx != 0 || ny != 0 ) && SwRpTerm( rp + int2( nx, ny ) ) < 0.5f )
				{
					nOppH++;
				}
			}
		}
		if( nOppH >= 5 )
		{
			holeCand = true;
			int ringMiss = 0;
			SW_UNROLL
			for( int k = 0; k < 16; k++ )
			{
				if( SwRpTerm( rp + swRpRing[k] ) > SW_RP_UMBRA )
				{
					ringMiss++;
					if( ringMiss > 2 )
					{
						holeCand = false;
						break;
					}
				}
			}
		}
		if( holeCand )
		{
			InterlockedAdd( g_rpCountH, 1u );
			InterlockedAdd( g_rpSumXH, ( uint )rp.x );
			InterlockedAdd( g_rpSumYH, ( uint )rp.y );
		}
	}

	// ---- STAGE 3: ARBITER — one FaceCoverageList per surviving pool (thread 0) ---------------
	if( all( gtid.xy == uint2( 0, 0 ) ) )
	{
		g_swMinDnR = g_misc.x;
		// TURD arbiter (dark blob in lit ring - exact says "should be lit")
		if( g_rpCountT > 0u )
		{
			REP_STATS_BUMP( 0, 3 );				// phase-1 groupsRingSurvive
			const int2 mid = int2( ( int )( g_rpSumXT / g_rpCountT ), ( int )( g_rpSumYT / g_rpCountT ) );
			const int2 pxScreen = int2( g_rect.xy ) + mid;
			const float4 wp = t_WorldPos[ uint2( pxScreen ) ];
			if( wp.w != 0.0f )
			{
				float occ = 1.0f;
				bool arbiterRan = false;
				const int tx = pxScreen.x / SW_RP_TILE_SIZE - g_tile.x;
				const int ty = pxScreen.y / SW_RP_TILE_SIZE - g_tile.y;
				if( g_repairBin.x >= 0 && tx >= 0 && ty >= 0 )
				{
					const int slot = g_repairBin.x + ( ty * g_range.w + tx ) * ( g_flags.w + 1 );
					uint cnt = t_SoftTiles[ slot ];
					if( cnt == SW_RP_TILE_UMBRA )
					{
						occ = 1.0f;
						arbiterRan = true;
					}
					else if( cnt == SW_RP_TILE_SPILL )
					{
						const uint spillOfs = t_SoftTiles[ slot + 1 ];
						const uint spillN   = min( t_SoftTiles[ slot + 2 ], 65536u );
						occ = SoftShadow_FaceCoverageClusterList( wp.xyz, g_lightR.xyz, max( g_lightR.w, 1e-2 ),
															g_range.x, ( int )spillOfs, ( int )spillN, 0.0f );
						arbiterRan = true;
						REP_STATS_BUMP( 0, 5 );
					}
					else if( cnt != 0xFFFFFFFFu && cnt <= ( uint )g_flags.w )
					{
						occ = SoftShadow_FaceCoverageList( wp.xyz, g_lightR.xyz, max( g_lightR.w, 1e-2 ),
													g_range.x, slot + 1, ( int )cnt, 0.0f );
						arbiterRan = true;
						REP_STATS_BUMP( 0, 4 );
					}
				}
				if( !arbiterRan )
				{
					occ = SoftShadow_FaceCoverage( wp.xyz, g_lightR.xyz, max( g_lightR.w, 1e-2 ),
									g_range.x, g_flags.y, g_range.y, 0.0f );
					REP_STATS_BUMP( 0, 6 );
				}
				if( occ < 1.0f - SW_RP_LIT + 0.005f )		// exact confirms LIT
				{
					g_rpAgreeT = 1u;
					REP_STATS_BUMP( 0, 7 );
				}
			}
		}
		// HOLE arbiter (lit blob in umbra ring - exact says "should be umbra")
		if( g_rpCountH > 0u )
		{
			REP_STATS_BUMP( 8, 3 );				// phase-2 groupsRingSurvive
			const int2 mid = int2( ( int )( g_rpSumXH / g_rpCountH ), ( int )( g_rpSumYH / g_rpCountH ) );
			const int2 pxScreen = int2( g_rect.xy ) + mid;
			const float4 wp = t_WorldPos[ uint2( pxScreen ) ];
			if( wp.w != 0.0f )
			{
				float occ = 1.0f;
				bool arbiterRan = false;
				const int tx = pxScreen.x / SW_RP_TILE_SIZE - g_tile.x;
				const int ty = pxScreen.y / SW_RP_TILE_SIZE - g_tile.y;
				if( g_repairBin.x >= 0 && tx >= 0 && ty >= 0 )
				{
					const int slot = g_repairBin.x + ( ty * g_range.w + tx ) * ( g_flags.w + 1 );
					uint cnt = t_SoftTiles[ slot ];
					if( cnt == SW_RP_TILE_UMBRA )
					{
						occ = 1.0f;
						arbiterRan = true;
					}
					else if( cnt == SW_RP_TILE_SPILL )
					{
						const uint spillOfs = t_SoftTiles[ slot + 1 ];
						const uint spillN   = min( t_SoftTiles[ slot + 2 ], 65536u );
						occ = SoftShadow_FaceCoverageClusterList( wp.xyz, g_lightR.xyz, max( g_lightR.w, 1e-2 ),
															g_range.x, ( int )spillOfs, ( int )spillN, 0.0f );
						arbiterRan = true;
						REP_STATS_BUMP( 8, 5 );
					}
					else if( cnt != 0xFFFFFFFFu && cnt <= ( uint )g_flags.w )
					{
						occ = SoftShadow_FaceCoverageList( wp.xyz, g_lightR.xyz, max( g_lightR.w, 1e-2 ),
													g_range.x, slot + 1, ( int )cnt, 0.0f );
						arbiterRan = true;
						REP_STATS_BUMP( 8, 4 );
					}
				}
				if( !arbiterRan )
				{
					occ = SoftShadow_FaceCoverage( wp.xyz, g_lightR.xyz, max( g_lightR.w, 1e-2 ),
									g_range.x, g_flags.y, g_range.y, 0.0f );
					REP_STATS_BUMP( 8, 6 );
				}
				if( occ > 1.0f - SW_RP_UMBRA )				// exact confirms UMBRA
				{
					g_rpAgreeH = 1u;
					REP_STATS_BUMP( 8, 7 );
				}
			}
		}
	}
	GroupMemoryBarrierWithGroupSync();

	// ---- STAGE 4: WRITE the corrected value (per-thread) -------------------------------------
	if( turdCand && g_rpAgreeT != 0u )
	{
		u_Term[ uint2( int2( g_rect.xy ) + rp + g_tile.zw ) ] = 1.0f;		// turd -> lit
	}
	if( holeCand && g_rpAgreeH != 0u )
	{
		u_Term[ uint2( int2( g_rect.xy ) + rp + g_tile.zw ) ] = 0.0f;		// hole -> shadow
	}
}
