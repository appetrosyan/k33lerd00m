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

// Tile-binned face coverage (r_softShadowTileBin) parity: SoftShadow_FaceCoverageList duplicates the
// shipped-config triangle test + crack close of SoftShadow_FaceCoverage (they cannot share a body
// without disturbing the hot path's toggle structure), so this test HOLDS THEM BIT-IDENTICAL:
//   1. the full walk vs the list walk over the COMPLETE triangle list must agree exactly;
//   2. the list walk over a CONSERVATIVELY CULLED list (the compute prepass's inflated cone/slab
//      test, reimplemented here on the CPU for a receiver AABB around each probe point) must agree
//      exactly too - the culled triangles can hit no sample ray, so dropping them changes nothing.
// Any drift between the two walks, or an over-aggressive bin cull, fails loudly.

#include "hlsl_compat.h"
#include "softwedge_coverage.inc.hlsl"
#include "SoftShadowBox.h"
#include "idUnitTest.h"

#include <vector>
#include <cmath>
#include <cstdint>

using namespace swtest;

namespace
{

// deterministic LCG so the corpus is reproducible
inline float Rnd( uint32_t& s, float lo, float hi )
{
	s = s * 1664525u + 1013904223u;
	return lo + ( hi - lo ) * ( ( s >> 8 ) & 0xFFFFFF ) / 16777215.0f;
}

// pair-start record indices of every triangle in a face stream (header-aware, mirrors the frontend's
// pair list); 'records' are softShadowEdge_t units = float4 PAIRS, matching the walkers' `se` index.
inline std::vector<uint32_t> PairStarts( const std::vector<float4>& buf )
{
	std::vector<uint32_t> out;
	int n = ( int )( buf.size() / 2 );
	for( int se = 0; se < n; se++ )
	{
		if( buf[se * 2].w < 0.0f )
		{
			continue;			// header
		}
		out.push_back( ( uint32_t )se );
		se++;					// consume recB
	}
	return out;
}

// the compute prepass's conservative cull (softtile_bin.cs.hlsl) for a receiver AABB (centre Pc,
// radius tR): triangle survives unless provably outside every receiver's sample cone.
inline bool BinKeep( const std::vector<float4>& buf, int se, float3 Pc, float tR, float3 L, float swR )
{
	float3 v0( buf[se * 2].x, buf[se * 2].y, buf[se * 2].z );
	float3 v1( buf[se * 2 + 1].x, buf[se * 2 + 1].y, buf[se * 2 + 1].z );
	float3 v2( buf[( se + 1 ) * 2 + 1].x, buf[( se + 1 ) * 2 + 1].y, buf[( se + 1 ) * 2 + 1].z );
	float3 toL = L - Pc;
	float distPL = std::fmax( len3( toL ), 1e-4f );
	float3 nrm = toL * ( 1.0f / distPL );
	float3 tcen = ( v0 + v1 + v2 ) * ( 1.0f / 3.0f );
	float triRad = buf[se * 2].w + tR;
	float3 rc = tcen - Pc;
	float cd = dot( rc, nrm );
	if( cd + triRad < 1e-3f )
	{
		return false;
	}
	if( cd - triRad > distPL + tR )
	{
		return false;
	}
	float3 perp = rc - cd * nrm;
	float coneR = std::fmax( swR, 1e-2f ) * ( cd + triRad ) / std::fmax( distPL - tR, 1e-4f );
	return std::sqrt( dot( perp, perp ) ) - triRad <= coneR;
}

inline float ListOcclusion( const std::vector<float4>& buf, const std::vector<uint32_t>& list,
							float3 P, float3 L, float r )
{
	SoftEdgeBuffer eb{ buf.data(), ( int )buf.size() };
	SoftTileBuffer tb{ list.data(), ( int )list.size() };
	return SoftShadow_FaceCoverageList( P, L, r, 0, 0, ( int )list.size(), tb, eb );
}

TEST( SoftShadowTileBin, list_walk_bit_identical_and_bin_cull_lossless )
{
	uint32_t seed = 0xC0FFEEu;
	int probes = 0;
	for( int scene = 0; scene < 12; scene++ )
	{
		// a few boxes between a light and a receiver plane region, varied scale/offset/rotation-ish
		std::vector<float4> buf;
		for( int b = 0; b < 3; b++ )
		{
			float3 c( Rnd( seed, -60, 60 ), Rnd( seed, -60, 60 ), Rnd( seed, 20, 90 ) );
			float3 h( Rnd( seed, 2, 30 ), Rnd( seed, 2, 30 ), Rnd( seed, 1, 18 ) );
			Box box = MakeBox( c, h );
			std::vector<float4> one = BuildFaceCaster( box );
			buf.insert( buf.end(), one.begin(), one.end() );
		}
		float3 L( Rnd( seed, -25, 25 ), Rnd( seed, -25, 25 ), Rnd( seed, 110, 170 ) );
		float  swR = Rnd( seed, 2, 24 );
		std::vector<uint32_t> all = PairStarts( buf );
		const int nRec = ( int )( buf.size() / 2 );
		SoftEdgeBuffer eb{ buf.data(), ( int )buf.size() };

		for( int p = 0; p < 40; p++ )
		{
			float3 P( Rnd( seed, -80, 80 ), Rnd( seed, -80, 80 ), Rnd( seed, -8, 6 ) );
			float full = SoftShadow_FaceCoverage( P, L, swR, 0, nRec, eb );

			// 1) complete list == full walk, bit-identical
			float listAll = ListOcclusion( buf, all, P, L, swR );
			CHECK( full == listAll );

			// 2) conservatively culled list (receiver AABB around P, like one screen tile) == full
			float tR = Rnd( seed, 0.5f, 12.0f );
			std::vector<uint32_t> culled;
			for( uint32_t se : all )
			{
				if( BinKeep( buf, ( int )se, P, tR, L, swR ) )
				{
					culled.push_back( se );
				}
			}
			float listCulled = ListOcclusion( buf, culled, P, L, swR );
			CHECK( full == listCulled );

			// 3) and for a DISPLACED receiver still inside the AABB the culled list must stay lossless
			float3 Pd = P + float3( Rnd( seed, -1, 1 ), Rnd( seed, -1, 1 ), Rnd( seed, -1, 1 ) ) * ( tR * 0.57f );
			float fullD = SoftShadow_FaceCoverage( Pd, L, swR, 0, nRec, eb );
			std::vector<uint32_t> culledD;	// bin cull is evaluated at the AABB CENTRE P, receiver at Pd
			for( uint32_t se : all )
			{
				if( BinKeep( buf, ( int )se, P, tR, L, swR ) )
				{
					culledD.push_back( se );
				}
			}
			float listD = ListOcclusion( buf, culledD, Pd, L, swR );
			CHECK( fullD == listD );
			probes++;
		}
	}
	CHECK( probes == 480 );
}

} // namespace
