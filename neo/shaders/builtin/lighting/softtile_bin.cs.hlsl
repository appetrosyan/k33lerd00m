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

// Analytic soft shadows: PER-TILE TRIANGLE BINNING (r_softShadowTileBin), STREAM V2. One threadgroup
// per 16x16 screen tile of one soft light's scissor. The group reduces the tile's depth min/max,
// unprojects the tile's 8 corner points into a world-space receiver AABB, then culls in two stages:
// first every CASTER's bounding sphere against the sample cone of that AABB (a rejected sphere can
// hide no triangle), then the surviving casters' TRIANGLES with the SAME cone/slab cull the
// interaction fragment shader runs per fragment - made conservative for the whole tile by inflating
// with the AABB radius (translating the cone apex by d changes any point-cone distance by at most d,
// and the receiver->light distance by at most d, so radius + 2d covers every receiver in the tile).
// Surviving TRIANGLE indices are appended to this tile's list; the fragment shader then walks ONLY
// that list (SoftShadow_FaceCoverageList). Measured motivation: the per-fragment record walk +
// triangle culls are 69% of the soft-shadow cost and are near-identical across a tile's 256 fragments.
//
// CONSERVATIVE by construction - a tile list can only OVER-include:
//   - depth-degenerate tiles (huge z range) inflate the AABB, keeping more triangles;
//   - a tile with more than SW_TILE_K survivors writes the overflow sentinel and the fragment shader
//     falls back to the full walk;
//   - sky/invalid-depth texels are excluded from the reduce; an all-sky tile bins nothing (no receiver
//     fragments will read it).
// The triangle list layout per tile: [count | idx0 .. idx(K-1)], count == 0xFFFFFFFF => overflow.

#pragma pack_matrix( row_major )

#define SW_TILE_SIZE	16
#define SW_TILE_K		512		// indices per tile; must match interactionSM.ps.hlsl + SoftTileBinPass.h

// *INDENT-OFF*
StructuredBuffer<float4>	t_Edges		: register( t0 );	// STREAM V2: tri stream (3 float4/tri) + caster table (2 float4/caster), whole joint buffer
StructuredBuffer<uint>		t_MinMax	: register( t2 );	// per-SCREEN-tile depth min/max bits (softtile_minmax.cs.hlsl)
RWStructuredBuffer<uint>	u_Tiles		: register( u0 );	// [count | K indices] per tile + SPILL region at the buffer tail
RWStructuredBuffer<uint>	u_SpillCnt	: register( u1 );	// spill allocator + stats (cleared per view): [0] bump cursor
															// (== total demand: failed allocations bump it too),
															// [1] overflow-tile count, [2] max per-tile survivor count

cbuffer c_TileBin : register( b0 )
{
	float4	g_invMvp0;		// world MVP inverse, rows (clip -> world, homogeneous)
	float4	g_invMvp1;
	float4	g_invMvp2;
	float4	g_invMvp3;
	float4	g_lightR;		// light origin xyz, disk radius w
	int4	g_tileRect;		// tile origin x, y (in tiles), tilesX, tilesY
	int4	g_range;		// firstElem (tri stream float4 base), numCasters, outBase (uint elements), casterBase (float4 elements)
	float4	g_screen;		// viewport W, H, viewport origin x, y
	int4	g_minmax;		// screen tiles X (t_MinMax row stride), umbra-tiles enable, SPILL region base, SPILL region end (uint elements)
};

#define SW_NEAR_EPS 1e-3f
// caster capacity of the groupshared survivor mask; casters past it are conservatively kept
#define SW_BIN_MAX_CASTERS 2048
// per-tile UMBRA sentinel (count slot value): one triangle provably blocks the WHOLE light disk for
// EVERY receiver in the tile -> the coverage integral saturates to 1 everywhere in the tile, so the
// consumers write term 0 without walking. Distinct from the overflow sentinel 0xFFFFFFFF.
#define SW_TILE_UMBRA 0xFFFFFFFEu
// per-tile SPILL sentinel (count slot value): the tile's survivor list exceeded SW_TILE_K, so its FULL
// index list was written to a bump-allocated span in the spill region instead; the tile's first two
// index words hold ( absolute span offset, count ). Consumers walk the span exactly like a tile list -
// the old 0xFFFFFFFF overflow sentinel (O(all-casters) full-walk fallback, measured ~12 ms/frame at
// live density) is now only the spill-region-exhausted fallback. Must match softterm.cs.hlsl +
// interactionSM.ps.hlsl.
#define SW_TILE_SPILL 0xFFFFFFFDu
// *INDENT-ON*

groupshared uint gsCount;
groupshared uint gsSpillBase;	// absolute uint element of this tile's spill span; 0xFFFFFFFF = none/failed
groupshared uint gsSpillFill;	// pass-2 append cursor within the span
groupshared float3 gsCentre;
groupshared float  gsRad;
groupshared float3 gsNrm;
groupshared float  gsDistPL;
groupshared uint gsCasterKeep[SW_BIN_MAX_CASTERS / 32];
groupshared uint gsUmbra;

float3 TileUnproject( float px, float py, float depth )
{
	float uvx = ( px - g_screen.z ) / g_screen.x;
	float uvy = ( py - g_screen.w ) / g_screen.y;
	float4 clip = float4( uvx * 2.0f - 1.0f, 1.0f - uvy * 2.0f, depth, 1.0f );
	float wx = dot( g_invMvp0, clip );
	float wy = dot( g_invMvp1, clip );
	float wz = dot( g_invMvp2, clip );
	float ww = dot( g_invMvp3, clip );
	float inv = ( abs( ww ) > 1e-12f ) ? 1.0f / ww : 0.0f;
	return float3( wx * inv, wy * inv, wz * inv );
}

[numthreads( 64, 1, 1 )]
void main( uint3 groupId : SV_GroupID, uint tid : SV_GroupThreadID )
{
	const int tileX = g_tileRect.x + ( int )groupId.x;
	const int tileY = g_tileRect.y + ( int )groupId.y;
	const int tileIdx = ( int )groupId.y * g_tileRect.z + ( int )groupId.x;
	const int outSlot = g_range.z + tileIdx * ( SW_TILE_K + 1 );

	if( tid == 0 )
	{
		gsCount = 0u;
		gsUmbra = 0u;
		gsSpillBase = 0xFFFFFFFFu;
	}

	// ---- tile depth min/max from the SHARED once-per-view reduce (softtile_minmax.cs.hlsl) ----
	const uint zMinBits = t_MinMax[ ( tileY * g_minmax.x + tileX ) * 2 + 0 ];
	const uint zMaxBits = t_MinMax[ ( tileY * g_minmax.x + tileX ) * 2 + 1 ];
	if( zMinBits > zMaxBits )	// no valid depth in the tile: nothing will shade here
	{
		if( tid == 0 )
		{
			u_Tiles[outSlot] = 0u;
		}
		return;
	}
	GroupMemoryBarrierWithGroupSync();

	// ---- receiver AABB from the tile's 8 unprojected corners ----
	if( tid == 0 )
	{
		float zmn = asfloat( zMinBits );
		float zmx = asfloat( zMaxBits );
		float x0 = ( float )( tileX * SW_TILE_SIZE );
		float y0 = ( float )( tileY * SW_TILE_SIZE );
		float x1 = min( x0 + ( float )SW_TILE_SIZE, g_screen.z + g_screen.x );
		float y1 = min( y0 + ( float )SW_TILE_SIZE, g_screen.w + g_screen.y );
		float3 mn = float3( 1e30f, 1e30f, 1e30f );
		float3 mx = float3( -1e30f, -1e30f, -1e30f );
		for( int c = 0; c < 8; c++ )
		{
			float3 w = TileUnproject( ( c & 1 ) != 0 ? x1 : x0, ( c & 2 ) != 0 ? y1 : y0, ( c & 4 ) != 0 ? zmx : zmn );
			mn = min( mn, w );
			mx = max( mx, w );
		}
		float3 cen = ( mn + mx ) * 0.5f;
		float3 he  = ( mx - mn ) * 0.5f;
		gsCentre = cen;
		gsRad    = sqrt( dot( he, he ) );
		float3 toL = float3( g_lightR.x, g_lightR.y, g_lightR.z ) - cen;
		gsDistPL = max( length( toL ), 1e-4f );
		gsNrm    = toL * ( 1.0f / gsDistPL );
	}
	GroupMemoryBarrierWithGroupSync();

	const float3 Pc     = gsCentre;
	const float  tR     = gsRad;
	const float3 nrm    = gsNrm;
	const float  distPL = gsDistPL;
	const float  swR    = max( g_lightR.w, 1e-2f );
	const float  eps    = SW_NEAR_EPS;

	// ---- stage 1: CASTER pre-cull. The caster's bounding sphere gets the SAME cone/slab test the
	// triangles get (a triangle is contained in its caster's sphere, so a rejected sphere can hide
	// no triangle - conservative). This is the O(tiles x tris) -> O(tiles x casters + survivors)
	// reduction: most casters are far from any given tile's cone. Survivor bits live in groupshared;
	// casters beyond SW_BIN_MAX_CASTERS are conservatively kept.
	for( int ci = ( int )tid; ci < ( SW_BIN_MAX_CASTERS / 32 ); ci += 64 )
	{
		gsCasterKeep[ci] = 0u;
	}
	GroupMemoryBarrierWithGroupSync();
	for( int c = ( int )tid; c < g_range.y && c < SW_BIN_MAX_CASTERS; c += 64 )
	{
		float4 c0 = t_Edges[ g_range.w + c * 2 + 0 ];			// ( centre.xyz, radius )
		float  R  = c0.w + tR;									// apex may sit anywhere in the tile AABB
		float3 rc = float3( c0.x, c0.y, c0.z ) - Pc;
		float  cd = dot( rc, nrm );
		if( cd + R < eps ) { continue; }						// wholly behind every receiver in the tile
		if( cd - R > distPL + tR ) { continue; }				// wholly beyond the light for every receiver
		float3 perp = rc - cd * nrm;
		float  coneR = swR * ( cd + R ) / max( distPL - tR, 1e-4f );
		if( dot( perp, perp ) > ( coneR + R ) * ( coneR + R ) ) { continue; }
		InterlockedOr( gsCasterKeep[c >> 5], 1u << ( c & 31 ) );
	}
	GroupMemoryBarrierWithGroupSync();

	// ---- stage 2: triangle cull over SURVIVING casters only. The caster loop is group-uniform (every
	// thread sees the same survivor bit), so a culled caster's whole span is skipped by the group in
	// one scalar branch; inside a surviving caster the threads stride its contiguous tri span.
	//
	// BIG-OCCLUDER-FIRST ORDERING was measured (cap61 2026-08-17) and REJECTED: a two-phase append
	// (angularly-big survivors first, rest second) is bit-exact - the per-fragment walk unions a
	// sample mask, which is order-independent - but the doubled per-tile bin cull cost (35.0 -> 36.3
	// ms) was not recovered by faster per-fragment saturation, because the walk's swMask==swAll
	// early-out already fires quickly in the umbra. Kept single-pass.
	for( int cc = 0; cc < g_range.y; cc++ )
	{
		if( cc < SW_BIN_MAX_CASTERS && ( gsCasterKeep[cc >> 5] & ( 1u << ( cc & 31 ) ) ) == 0u )
		{
			continue;
		}
		float4 c1 = t_Edges[ g_range.w + cc * 2 + 1 ];			// ( firstTri, numTris, 0, 0 )
		const int triFirst = ( int )c1.x;
		const int triEnd   = triFirst + ( int )c1.y;
		for( int t = triFirst + ( int )tid; t < triEnd; t += 64 )
		{
			const int b = g_range.x + t * 3;
			float4 r0 = t_Edges[ b + 0 ];
			float4 r1 = t_Edges[ b + 1 ];
			float4 r2 = t_Edges[ b + 2 ];
			float3 tcen = ( float3( r0.x, r0.y, r0.z ) + float3( r1.x, r1.y, r1.z ) + float3( r2.x, r2.y, r2.z ) ) * ( 1.0f / 3.0f );
			float  triRad = r1.w + tR;							// r1.w = centroid radius (tight); apex may sit anywhere in the tile AABB
			float3 rc = tcen - Pc;
			float  cd = dot( rc, nrm );
			if( cd + triRad < eps ) { continue; }				// wholly behind every receiver in the tile
			if( cd - triRad > distPL + tR ) { continue; }		// wholly beyond the light for every receiver
			float3 perp = rc - cd * nrm;
			float  coneR = swR * ( cd + triRad ) / max( distPL - tR, 1e-4f );
			if( dot( perp, perp ) > ( coneR + triRad ) * ( coneR + triRad ) ) { continue; }
			uint slot;
			InterlockedAdd( gsCount, 1u, slot );
			if( slot < SW_TILE_K )
			{
				u_Tiles[ outSlot + 1 + ( int )slot ] = ( uint )t;	// TRIANGLE index (stream v2)
			}

			// ---- WHOLE-TILE UMBRA SENTINEL (g_minmax.y, r_softShadowUmbraTiles) ----------------
			// Prove: this ONE triangle blocks the ENTIRE light disk for EVERY receiver in the tile
			// ball (Pc, tR) - then every sample ray of every fragment in the tile hits it, the
			// coverage mask saturates to all-16, and the integral's value is exactly 1: the
			// consumers may write term 0 without walking. The proof is the classic per-edge
			// inner-penumbra construction (Assarsson wedges): a receiver P is in the triangle's
			// umbra w.r.t. the light SPHERE (centre L, radius swR >= the sample disk, so sphere
			// occlusion implies disk occlusion) iff
			//   (1) the triangle plane separates P from the whole sphere, and
			//   (2) for each edge, P lies behind the plane through the edge tangent to the sphere
			//       with the sphere on the lit side and the triangle interior on P's side
			// - then any ray P->S (S in sphere) crosses the triangle plane on the interior side of
			// all three edge lines, i.e. inside the triangle. Every test carries the tile radius tR
			// so it holds for the whole ball, plus a slack for the ray test's t > 1e-4 floor.
			// CONSERVATIVE: any failure to certify just skips the sentinel (no speedup, no error).
			if( g_minmax.y != 0 && gsUmbra == 0u )
			{
				float3 Lp = float3( g_lightR.x, g_lightR.y, g_lightR.z );
				float3 nt = cross( float3( r1.x, r1.y, r1.z ) - float3( r0.x, r0.y, r0.z ),
								   float3( r2.x, r2.y, r2.z ) - float3( r0.x, r0.y, r0.z ) );
				float ntl = sqrt( dot( nt, nt ) );
				if( ntl > 1e-6f )
				{
					nt = nt * ( 1.0f / ntl );
					float3 v0 = float3( r0.x, r0.y, r0.z );
					float dL = dot( nt, Lp - v0 );
					if( dL < 0.0f ) { nt = -nt; dL = -dL; }			// orient toward the light
					float dP = dot( nt, Pc - v0 );
					float slack = tR + 2e-4f * ( distPL + tR );		// covers the walk's t > 1e-4 floor
					if( dL > swR + 1e-3f && dP < -slack )
					{
						bool ok = true;
						float3 V[3];
						V[0] = v0; V[1] = float3( r1.x, r1.y, r1.z ); V[2] = float3( r2.x, r2.y, r2.z );
						[unroll]
						for( int e = 0; e < 3; e++ )
						{
							float3 Va = V[e], Vb = V[( e + 1 ) % 3], Vc = V[( e + 2 ) % 3];
							float3 ed = Vb - Va;
							float el2 = dot( ed, ed );
							if( el2 < 1e-12f ) { ok = false; break; }
							float3 u2 = ed * rsqrt( el2 );
							float3 w  = Lp - Va;
							float3 wp = w - dot( w, u2 ) * u2;
							float  W2 = dot( wp, wp );
							if( W2 <= swR * swR + 1e-6f ) { ok = false; break; }	// sphere touches the edge line
							float invW = rsqrt( W2 );
							float3 n0 = wp * invW;
							float3 m  = cross( u2, n0 );
							float sinT = swR * invW;
							float cosT = sqrt( max( 1.0f - sinT * sinT, 0.0f ) );
							// of the two tangent planes through the edge, the INNER-PENUMBRA bound is
							// the one whose tangent point lies AWAY from the triangle interior (the
							// umbra-boundary sight line grazes the edge toward that far side of the
							// sphere): sigma picks it by the interior direction Vc - Va. The bound
							// leans OVER the shadow: light sphere and umbra receivers are on the SAME
							// (positive) side, the penumbra on the other - so the ball must be fully
							// on the + side (>= +tR), verified numerically by the 2D boundary case.
							float sigma = ( dot( m, Vc - Va ) >= 0.0f ) ? 1.0f : -1.0f;
							float3 ne = sinT * n0 + sigma * cosT * m;
							if( dot( ne, Pc - Va ) < tR ) { ok = false; break; }	// tile ball not fully inside the umbra wedge
						}
						if( ok )
						{
							InterlockedOr( gsUmbra, 1u );
						}
					}
				}
			}
		}
	}
	GroupMemoryBarrierWithGroupSync();

	// ---- SPILL PASS (overflowed tiles) -------------------------------------------------------
	// gsCount counted EVERY stage-2 survivor, but only the first K fit the tile slot. Allocate a
	// span of gsCount elements from the spill region [g_minmax.z, g_minmax.w) via the global bump
	// counter and RE-RUN the stage-2 cull, appending every surviving index there. The re-walk is
	// deterministic (same inputs, same culls => exactly gsCount survivors again); its append ORDER
	// differs from a K-fit list, which is irrelevant - the consumers union a per-sample mask
	// (order-independence ledgered by the big-occluder-first rejection). The umbra proof is
	// skipped (already resolved in pass 1). Region exhausted -> keep the old 0xFFFFFFFF sentinel
	// (full-walk fallback): graceful degradation, never corruption.
	if( gsUmbra == 0u && gsCount > SW_TILE_K )
	{
		if( tid == 0 )
		{
			uint rel;
			InterlockedAdd( u_SpillCnt[0], gsCount, rel );
			uint dead;
			InterlockedAdd( u_SpillCnt[1], 1u, dead );		// stats: overflow-tile count
			InterlockedMax( u_SpillCnt[2], gsCount );		// stats: worst per-tile survivor count
			const uint base = ( uint )g_minmax.z + rel;
			gsSpillBase = ( base + gsCount <= ( uint )g_minmax.w ) ? base : 0xFFFFFFFFu;
			gsSpillFill = 0u;
		}
		GroupMemoryBarrierWithGroupSync();
		if( gsSpillBase != 0xFFFFFFFFu )
		{
			for( int cc2 = 0; cc2 < g_range.y; cc2++ )
			{
				if( cc2 < SW_BIN_MAX_CASTERS && ( gsCasterKeep[cc2 >> 5] & ( 1u << ( cc2 & 31 ) ) ) == 0u )
				{
					continue;
				}
				float4 c1 = t_Edges[ g_range.w + cc2 * 2 + 1 ];		// ( firstTri, numTris, 0, 0 )
				const int triFirst = ( int )c1.x;
				const int triEnd   = triFirst + ( int )c1.y;
				for( int t = triFirst + ( int )tid; t < triEnd; t += 64 )
				{
					const int b = g_range.x + t * 3;
					float4 r0 = t_Edges[ b + 0 ];
					float4 r1 = t_Edges[ b + 1 ];
					float4 r2 = t_Edges[ b + 2 ];
					float3 tcen = ( float3( r0.x, r0.y, r0.z ) + float3( r1.x, r1.y, r1.z ) + float3( r2.x, r2.y, r2.z ) ) * ( 1.0f / 3.0f );
					float  triRad = r1.w + tR;						// IDENTICAL cull to pass 1 - keep in lock-step
					float3 rc = tcen - Pc;
					float  cd = dot( rc, nrm );
					if( cd + triRad < eps ) { continue; }
					if( cd - triRad > distPL + tR ) { continue; }
					float3 perp = rc - cd * nrm;
					float  coneR = swR * ( cd + triRad ) / max( distPL - tR, 1e-4f );
					if( dot( perp, perp ) > ( coneR + triRad ) * ( coneR + triRad ) ) { continue; }
					uint slot;
					InterlockedAdd( gsSpillFill, 1u, slot );
					if( slot < gsCount )							// == gsCount by determinism; guard is belt-and-braces
					{
						u_Tiles[ gsSpillBase + slot ] = ( uint )t;	// TRIANGLE index (stream v2)
					}
				}
			}
			if( tid == 0 )
			{
				// span descriptor in the tile's (otherwise dead) first two index words
				u_Tiles[ outSlot + 1 ] = gsSpillBase;
				u_Tiles[ outSlot + 2 ] = gsCount;
			}
		}
	}

	if( tid == 0 )
	{
		u_Tiles[outSlot] = ( gsUmbra != 0u ) ? SW_TILE_UMBRA
						   : ( ( gsCount <= SW_TILE_K ) ? gsCount
							   : ( ( gsSpillBase != 0xFFFFFFFFu ) ? SW_TILE_SPILL : 0xFFFFFFFFu ) );
	}
}
