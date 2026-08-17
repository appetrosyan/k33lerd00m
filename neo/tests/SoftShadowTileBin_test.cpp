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

// triangle count / index list of a v2 face stream
inline int StreamTris( const FaceStreamCPU& s )
{
	return ( ( int )s.buf.size() - s.triBase() ) / 3;
}
inline std::vector<uint32_t> AllTris( const FaceStreamCPU& s )
{
	std::vector<uint32_t> out;
	for( int t = 0; t < StreamTris( s ); t++ ) { out.push_back( ( uint32_t )t ); }
	return out;
}

// the compute prepass's conservative cone/slab cull (softtile_bin.cs.hlsl) for a receiver AABB
// (centre Pc, radius tR), applied to a bounding ball (centre bc, radius br): the ball survives
// unless provably outside every receiver's sample cone. Shared by the caster-sphere pre-cull
// (stage 1) and the per-triangle cull (stage 2) - the shader runs the SAME test for both.
inline bool BinKeepBall( float3 bc, float br, float3 Pc, float tR, float3 L, float swR )
{
	float3 toL = L - Pc;
	float distPL = std::fmax( len3( toL ), 1e-4f );
	float3 nrm = toL * ( 1.0f / distPL );
	float R = br + tR;
	float3 rc = bc - Pc;
	float cd = dot( rc, nrm );
	if( cd + R < 1e-3f )
	{
		return false;
	}
	if( cd - R > distPL + tR )
	{
		return false;
	}
	float3 perp = rc - cd * nrm;
	float coneR = std::fmax( swR, 1e-2f ) * ( cd + R ) / std::fmax( distPL - tR, 1e-4f );
	return std::sqrt( dot( perp, perp ) ) - R <= coneR;
}

// two-stage bin mirror: caster sphere first, then the caster's triangles (exactly the shader's order)
inline std::vector<uint32_t> BinList( const FaceStreamCPU& s, float3 Pc, float tR, float3 L, float swR )
{
	std::vector<uint32_t> out;
	for( int c = 0; c < s.nCasters; c++ )
	{
		const float4& c0 = s.buf[c * 2 + 0];
		const float4& c1 = s.buf[c * 2 + 1];
		if( !BinKeepBall( float3( c0.x, c0.y, c0.z ), c0.w, Pc, tR, L, swR ) )
		{
			continue;
		}
		const int first = ( int )c1.x, cnt = ( int )c1.y;
		for( int t = first; t < first + cnt; t++ )
		{
			const int b = s.triBase() + t * 3;
			float3 v0( s.buf[b].x, s.buf[b].y, s.buf[b].z );
			float3 v1( s.buf[b + 1].x, s.buf[b + 1].y, s.buf[b + 1].z );
			float3 v2( s.buf[b + 2].x, s.buf[b + 2].y, s.buf[b + 2].z );
			float3 tcen = ( v0 + v1 + v2 ) * ( 1.0f / 3.0f );
			if( BinKeepBall( tcen, s.buf[b].w, Pc, tR, L, swR ) )
			{
				out.push_back( ( uint32_t )t );
			}
		}
	}
	return out;
}

inline float ListOcclusion( const FaceStreamCPU& s, const std::vector<uint32_t>& list,
							float3 P, float3 L, float r )
{
	SoftEdgeBuffer eb{ s.buf.data(), ( int )s.buf.size() };
	SoftTileBuffer tb{ list.data(), ( int )list.size() };
	return SoftShadow_FaceCoverageList( P, L, r, s.triBase(), 0, ( int )list.size(), SoftRotAngle( P ), tb, eb );
}

TEST( SoftShadowTileBin, list_walk_bit_identical_and_bin_cull_lossless )
{
	uint32_t seed = 0xC0FFEEu;
	int probes = 0;
	for( int scene = 0; scene < 12; scene++ )
	{
		// a few boxes between a light and a receiver plane region, varied scale/offset/rotation-ish
		FaceStreamCPU buf;
		for( int b = 0; b < 3; b++ )
		{
			float3 c( Rnd( seed, -60, 60 ), Rnd( seed, -60, 60 ), Rnd( seed, 20, 90 ) );
			float3 h( Rnd( seed, 2, 30 ), Rnd( seed, 2, 30 ), Rnd( seed, 1, 18 ) );
			buf.Append( BuildFaceCasterUnit( MakeBox( c, h ) ) );
		}
		float3 L( Rnd( seed, -25, 25 ), Rnd( seed, -25, 25 ), Rnd( seed, 110, 170 ) );
		float  swR = Rnd( seed, 2, 24 );
		std::vector<uint32_t> all = AllTris( buf );
		SoftEdgeBuffer eb{ buf.buf.data(), ( int )buf.buf.size() };

		for( int p = 0; p < 40; p++ )
		{
			float3 P( Rnd( seed, -80, 80 ), Rnd( seed, -80, 80 ), Rnd( seed, -8, 6 ) );
			float full = SoftShadow_FaceCoverage( P, L, swR, buf.triBase(), 0, buf.nCasters, SoftRotAngle( P ), eb );

			// 1) complete list == full walk, bit-identical
			float listAll = ListOcclusion( buf, all, P, L, swR );
			CHECK( full == listAll );

			// 2) conservatively culled list (receiver AABB around P, like one screen tile; the shader's
			//    two-stage caster-sphere + triangle cull) == full
			float tR = Rnd( seed, 0.5f, 12.0f );
			std::vector<uint32_t> culled = BinList( buf, P, tR, L, swR );
			float listCulled = ListOcclusion( buf, culled, P, L, swR );
			CHECK( full == listCulled );

			// 3) and for a DISPLACED receiver still inside the AABB the culled list must stay lossless
			float3 Pd = P + float3( Rnd( seed, -1, 1 ), Rnd( seed, -1, 1 ), Rnd( seed, -1, 1 ) ) * ( tR * 0.57f );
			float fullD = SoftShadow_FaceCoverage( Pd, L, swR, buf.triBase(), 0, buf.nCasters, SoftRotAngle( Pd ), eb );
			// bin cull is evaluated at the AABB CENTRE P, receiver at Pd
			float listD = ListOcclusion( buf, BinList( buf, P, tR, L, swR ), Pd, L, swR );
			CHECK( fullD == listD );
			probes++;
		}
	}
	CHECK( probes == 480 );
}

// -------------------------------------------------------------------- whole-tile umbra sentinel
// CPU mirror of the bin pass's per-triangle umbra certificate (softtile_bin.cs.hlsl,
// r_softShadowUmbraTiles): triangle-plane separation + per-edge planes tangent to the light sphere,
// every test carrying the tile radius. PROPERTY: whenever the certificate fires for a tile ball
// (Pc, tR), the SHIPPED coverage integral must return exactly 1 for every receiver in the ball -
// the sentinel's occ=1 shortcut is then the integral's own value.
inline bool UmbraCertifies( const FaceStreamCPU& s, int tri, float3 Pc, float tR, float3 L, float swR )
{
	const int b = s.triBase() + tri * 3;
	float3 V[3] =
	{
		float3( s.buf[b].x, s.buf[b].y, s.buf[b].z ),
		float3( s.buf[b + 1].x, s.buf[b + 1].y, s.buf[b + 1].z ),
		float3( s.buf[b + 2].x, s.buf[b + 2].y, s.buf[b + 2].z ),
	};
	float R = std::fmax( swR, 1e-2f );
	float3 toL = L - Pc;
	float distPL = std::fmax( len3( toL ), 1e-4f );
	float3 nt = cross( V[1] - V[0], V[2] - V[0] );
	float ntl = std::sqrt( dot( nt, nt ) );
	if( ntl <= 1e-6f )
	{
		return false;
	}
	nt = nt * ( 1.0f / ntl );
	float dL = dot( nt, L - V[0] );
	if( dL < 0.0f )
	{
		nt = nt * -1.0f;	// hlsl_compat float3 has no unary minus
		dL = -dL;
	}
	float dP = dot( nt, Pc - V[0] );
	float slack = tR + 2e-4f * ( distPL + tR );
	if( !( dL > R + 1e-3f && dP < -slack ) )
	{
		return false;
	}
	for( int e = 0; e < 3; e++ )
	{
		float3 Va = V[e], Vb = V[( e + 1 ) % 3], Vc = V[( e + 2 ) % 3];
		float3 ed = Vb - Va;
		float el2 = dot( ed, ed );
		if( el2 < 1e-12f )
		{
			return false;
		}
		float3 u2 = ed * ( 1.0f / std::sqrt( el2 ) );
		float3 w = L - Va;
		float3 wp = w - u2 * dot( w, u2 );
		float W2 = dot( wp, wp );
		if( W2 <= R * R + 1e-6f )
		{
			return false;
		}
		float invW = 1.0f / std::sqrt( W2 );
		float3 n0 = wp * invW;
		float3 m = cross( u2, n0 );
		float sinT = R * invW;
		float cosT = std::sqrt( std::fmax( 1.0f - sinT * sinT, 0.0f ) );
		// inner-penumbra bound: tangent point AWAY from the triangle interior; sphere and umbra are
		// on the SAME (+) side of the bound (see softtile_bin.cs.hlsl)
		float sigma = ( dot( m, Vc - Va ) >= 0.0f ) ? 1.0f : -1.0f;
		float3 ne = n0 * sinT + m * ( sigma * cosT );
		if( dot( ne, Pc - Va ) < tR )
		{
			return false;
		}
	}
	return true;
}

TEST( SoftShadowTileBin, umbra_sentinel_certificate_implies_saturated_integral )
{
	uint32_t seed = 0xBADCAFEu;
	long certified = 0, probesChecked = 0;
	for( int scene = 0; scene < 30; scene++ )
	{
		// one large slab between a high light and a low receiver region - deep umbra exists
		float3 c( Rnd( seed, -20, 20 ), Rnd( seed, -20, 20 ), Rnd( seed, 30, 60 ) );
		float3 h( Rnd( seed, 25, 80 ), Rnd( seed, 25, 80 ), Rnd( seed, 1, 6 ) );
		FaceStreamCPU buf = BuildFaceCaster( MakeBox( c, h ) );
		float3 L( Rnd( seed, -15, 15 ), Rnd( seed, -15, 15 ), Rnd( seed, 120, 200 ) );
		float swR = Rnd( seed, 2, 16 );
		SoftEdgeBuffer eb{ buf.buf.data(), ( int )buf.buf.size() };
		const int nTris = StreamTris( buf );

		for( int p = 0; p < 25; p++ )
		{
			float3 Pc( Rnd( seed, -30, 30 ), Rnd( seed, -30, 30 ), Rnd( seed, -10, 5 ) );
			float tR = Rnd( seed, 0.5f, 10.0f );
			bool cert = false;
			for( int t = 0; t < nTris && !cert; t++ )
			{
				cert = UmbraCertifies( buf, t, Pc, tR, L, swR );
			}
			if( !cert )
			{
				continue;
			}
			certified++;
			for( int k = 0; k < 8; k++ )		// receivers throughout the certified ball
			{
				float3 d( Rnd( seed, -1, 1 ), Rnd( seed, -1, 1 ), Rnd( seed, -1, 1 ) );
				float dl = std::sqrt( dot( d, d ) );
				float3 P = Pc + ( dl > 1e-6f ? d * ( 1.0f / dl ) : float3( 0, 0, 1 ) ) * ( tR * Rnd( seed, 0.0f, 0.999f ) );
				float occ = SoftShadow_FaceCoverage( P, L, swR, buf.triBase(), 0, buf.nCasters, SoftRotAngle( P ), eb );
				if( occ != 1.0f && probesChecked >= 0 )
				{
					static int printed = 0;
					if( printed++ < 6 )
					{
						std::printf( "    [umbra-LEAK] occ=%.6f P=(%.2f,%.2f,%.2f) Pc=(%.2f,%.2f,%.2f) tR=%.3f L=(%.2f,%.2f,%.2f) swR=%.3f slab c=(%.1f,%.1f,%.1f) h=?\n",
									 occ, P.x, P.y, P.z, Pc.x, Pc.y, Pc.z, tR, L.x, L.y, L.z, swR, c.x, c.y, c.z );
					}
				}
				CHECK( occ == 1.0f );			// the sentinel's shortcut is the integral's exact value
				probesChecked++;
			}
		}
	}
	std::printf( "    [umbra-sentinel] certified balls=%ld, receiver probes=%ld (all saturated)\n", certified, probesChecked );
	CHECK( certified > 20 );					// the property must not be vacuously green
}

// ------------------------------------------------------------- SPILL cluster walk (stream v3)
// Overflowed tiles spill CLUSTER records (k-d leaves of <=32 tris, built in R_CollectPenumbraFaces;
// caster c1.zw spans them) and the consumers walk them with SoftShadow_FaceCoverageClusterList.
// PROPERTIES: (1) the complete cluster list must agree BIT-IDENTICALLY with the full walk (the
// cluster cull is conservative and the per-tri gate is unchanged); (2) a conservatively bin-culled
// cluster list (the shader's tile-grain ball cull) must too, including for displaced receivers.
TEST( SoftShadowTileBin, spill_cluster_walk_bit_identical_and_cull_lossless )
{
	uint32_t seed = 0xBEEF01u;
	int probes = 0;
	for( int scene = 0; scene < 12; scene++ )
	{
		FaceStreamCPU buf;
		for( int b = 0; b < 3; b++ )
		{
			float3 c( Rnd( seed, -60, 60 ), Rnd( seed, -60, 60 ), Rnd( seed, 20, 90 ) );
			float3 h( Rnd( seed, 2, 30 ), Rnd( seed, 2, 30 ), Rnd( seed, 1, 18 ) );
			buf.Append( BuildFaceCasterUnit( MakeBox( c, h ) ) );
		}
		float3 L( Rnd( seed, -25, 25 ), Rnd( seed, -25, 25 ), Rnd( seed, 110, 170 ) );
		float  swR = Rnd( seed, 2, 24 );

		// append cluster records after the tris (offsets are ABSOLUTE, placement is free): chunks
		// of <=4 tris per caster so multiple clusters exist even on 12-tri boxes
		std::vector<float4> cbuf = buf.buf;
		struct clu_t
		{
			uint32_t absOfs;
			int caster;
			float3 cen;
			float rad;
		};
		std::vector<clu_t> clusters;
		for( int cas = 0; cas < buf.nCasters; cas++ )
		{
			const float4& c1 = buf.buf[cas * 2 + 1];
			const int first = ( int )c1.x, cnt = ( int )c1.y;
			for( int f = first; f < first + cnt; f += 4 )
			{
				const int leaf = std::min( 4, first + cnt - f );
				float3 acc( 0, 0, 0 );
				for( int t = f; t < f + leaf; t++ )
				{
					const int b = buf.triBase() + t * 3;
					acc = acc + ( float3( buf.buf[b].x, buf.buf[b].y, buf.buf[b].z )
								  + float3( buf.buf[b + 1].x, buf.buf[b + 1].y, buf.buf[b + 1].z )
								  + float3( buf.buf[b + 2].x, buf.buf[b + 2].y, buf.buf[b + 2].z ) ) * ( 1.0f / 3.0f );
				}
				const float3 cen = acc * ( 1.0f / leaf );
				float rad = 0.0f;
				for( int t = f; t < f + leaf; t++ )
				{
					const int b = buf.triBase() + t * 3;
					for( int k = 0; k < 3; k++ )
					{
						rad = std::fmax( rad, len3( float3( buf.buf[b + k].x, buf.buf[b + k].y, buf.buf[b + k].z ) - cen ) );
					}
				}
				rad *= 1.00001f;
				clu_t cl;
				cl.absOfs = ( uint32_t )cbuf.size();
				cl.caster = cas;
				cl.cen = cen;
				cl.rad = rad;
				cbuf.push_back( float4( cen.x, cen.y, cen.z, rad ) );
				cbuf.push_back( float4( ( float )f, ( float )leaf, 0, 0 ) );
				clusters.push_back( cl );
			}
		}
		std::vector<uint32_t> all;
		for( const clu_t& cl : clusters )
		{
			all.push_back( cl.absOfs );
		}
		SoftEdgeBuffer eb{ cbuf.data(), ( int )cbuf.size() };

		for( int p = 0; p < 40; p++ )
		{
			float3 P( Rnd( seed, -80, 80 ), Rnd( seed, -80, 80 ), Rnd( seed, -8, 6 ) );
			float full = SoftShadow_FaceCoverage( P, L, swR, buf.triBase(), 0, buf.nCasters, SoftRotAngle( P ), eb );

			// 1) complete cluster list == full walk, bit-identical
			SoftTileBuffer tbAll{ all.data(), ( int )all.size() };
			float listAll = SoftShadow_FaceCoverageClusterList( P, L, swR, buf.triBase(), 0, ( int )all.size(), SoftRotAngle( P ), tbAll, eb );
			CHECK( full == listAll );

			// 2) bin-culled cluster list (caster ball then cluster ball, the shader's spill order)
			//    == full, for the probe point AND a displaced receiver inside the tile ball
			float tR = Rnd( seed, 0.5f, 12.0f );
			std::vector<uint32_t> culled;
			for( const clu_t& cl : clusters )
			{
				const float4& c0 = buf.buf[cl.caster * 2 + 0];
				if( !BinKeepBall( float3( c0.x, c0.y, c0.z ), c0.w, P, tR, L, swR ) )
				{
					continue;
				}
				if( BinKeepBall( cl.cen, cl.rad, P, tR, L, swR ) )
				{
					culled.push_back( cl.absOfs );
				}
			}
			SoftTileBuffer tbCul{ culled.data(), ( int )culled.size() };
			float listCulled = SoftShadow_FaceCoverageClusterList( P, L, swR, buf.triBase(), 0, ( int )culled.size(), SoftRotAngle( P ), tbCul, eb );
			CHECK( full == listCulled );

			float3 Pd = P + float3( Rnd( seed, -1, 1 ), Rnd( seed, -1, 1 ), Rnd( seed, -1, 1 ) ) * ( tR * 0.57f );
			float fullD = SoftShadow_FaceCoverage( Pd, L, swR, buf.triBase(), 0, buf.nCasters, SoftRotAngle( Pd ), eb );
			float listD = SoftShadow_FaceCoverageClusterList( Pd, L, swR, buf.triBase(), 0, ( int )culled.size(), SoftRotAngle( Pd ), tbCul, eb );
			CHECK( fullD == listD );
			probes++;
		}
	}
	CHECK( probes == 480 );
}

} // namespace
