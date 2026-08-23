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

// FORENSICS for the ragdoll "large black bands on top of themselves" play-test report (2026-08-17,
// cap0042 = Excavation Hall, corpse close-up). For every light, evaluates the SHIPPED coverage
// at receiver vertices, and for the fully-black ones re-traces the sample rays to identify WHAT is
// blocking: the distance from the receiver to the nearest blocking triangle separates the candidate
// mechanisms -
//     < ~0.5u  : the receiver's own surface / adjacent facets (terminator-style self acne),
//   0.5 - 10u  : other body parts, or a caster mesh in a DIFFERENT POSE than the rendered one
//                (posedShadowVerts lagging or mismatching the drawn skinning),
//      > 10u   : environment geometry (a legitimate shadow).
// STUDY tier: diagnostic instrument, not a pass/fail gate - run with `rbdoom3bfg_tests @study`.

#include "hlsl_compat.h"
#include "softwedge_coverage.inc.hlsl"
#include "SoftShadowBox.h"
#include "SoftShadowMesh.h"
#include "idUnitTest.h"

#include <cstdio>
#include <cstring>
#include <vector>
#include <cmath>

using namespace swtest;

namespace
{

struct BlockStat
{
	int buckets[5] = { 0, 0, 0, 0, 0 };	// <0.5, 0.5-2, 2-10, 10-50, >50 units
	void Add( float d )
	{
		buckets[ d < 0.5f ? 0 : d < 2.0f ? 1 : d < 10.0f ? 2 : d < 50.0f ? 3 : 4 ]++;
	}
};

inline int BucketOf( float d )
{
	return d < 0.5f ? 0 : d < 2.0f ? 1 : d < 10.0f ? 2 : d < 50.0f ? 3 : 4;
}

} // namespace

STUDY_TEST( SoftShadowRagdoll, black_blob_forensics_cap0042 )
{
	Cap cap;
	const char* paths[] = { "../tests/data/cap0042.cap", "tests/data/cap0042.cap", "neo/tests/data/cap0042.cap" };
	bool loaded = false;
	for( const char* p : paths )
	{
		if( LoadCap( p, cap ) )
		{
			loaded = true;
			break;
		}
	}
	if( !loaded )
	{
		std::printf( "  cap0042.cap not found - skipping\n" );
		return;
	}
	std::printf( "  %d lights, %d edges, %d receivers, %d recv verts\n",
				 ( int )cap.lights.size(), ( int )cap.edges.size(), ( int )cap.receivers.size(),
				 ( int )( cap.recvVerts.size() / 3 ) );

	const float4* allRecs = ( const float4* )cap.edges.data();

	for( size_t li = 0; li < cap.lights.size(); li++ )
	{
		const capLight_t& L = cap.lights[li];
		if( L.edgeCount == 0 )
		{
			continue;
		}
		float3 lp( L.origin[0], L.origin[1], L.origin[2] );
		float  swR = L.penumbraSize > 0 ? L.penumbraSize : 8.0f;
		// pre-v5 capture blob = v1 records (inline headers + tri pairs); convert to the v2 stream
		FaceStreamCPU fs = FaceStreamFromV1Records( allRecs, ( int )L.firstEdge * 2, ( int )L.edgeCount );
		SoftEdgeBuffer buf{ fs.buf.data(), ( int )fs.buf.size() };

		int totalV = 0, blackV = 0, litV = 0, penV = 0;
		BlockStat firstBlockers;			// nearest blocking triangle per blocked sample of BLACK verts
		int engulfed = 0;					// black verts whose EVERY blocker is within 2 units
		int printedExamples = 0;

		for( size_t ri = 0; ri < cap.receivers.size(); ri++ )
		{
			const capReceiver_t& rs = cap.receivers[ri];
			if( rs.lightIndex != ( uint32_t )li )
			{
				continue;
			}
			for( uint32_t v = 0; v < rs.numVerts; v += 7 )	// subsample: every 7th vert
			{
				const float* pv = &cap.recvVerts[( rs.firstVert + v ) * 3];
				float3 P( pv[0], pv[1], pv[2] );
				totalV++;
				float occ = saturate( SoftShadow_FaceCoverage( P, lp, swR, fs.triBase(), 0, fs.nCasters, SoftRotAngle( P ), buf ) );
				if( occ <= 0.01f )
				{
					litV++;
					continue;
				}
				if( occ < 0.99f )
				{
					penV++;
					continue;
				}
				blackV++;

				// re-trace the 16 rays: nearest blocker distance per blocked sample
				softFrame_t f = SoftShadow_Frame( P, lp );
				float ang = SoftRotAngle( P );
				float ca = std::cos( ang ), sa = std::sin( ang );
				const float2 disk[16] =
				{
					float2( 0.176777f, 0.000000f ), float2( -0.225772f, 0.206826f ),
					float2( 0.034558f, -0.393771f ), float2( 0.284571f, 0.371173f ),
					float2( -0.522223f, -0.092374f ), float2( 0.494695f, -0.314685f ),
					float2( -0.165466f, 0.615525f ), float2( -0.315561f, -0.607594f ),
					float2( 0.684642f, 0.250030f ), float2( -0.712256f, 0.294009f ),
					float2( 0.343354f, -0.733729f ), float2( 0.253730f, 0.808932f ),
					float2( -0.764746f, -0.443186f ), float2( 0.897134f, -0.197232f ),
					float2( -0.547507f, 0.778772f ), float2( -0.126487f, -0.976090f ),
				};
				float3 base = lp - P;
				float3 su = f.u * swR, sv = f.v * swR;
				float worstNear = -1.0f;
				bool allNear = true;
				for( int s = 0; s < 16; s++ )
				{
					float2 s0 = disk[s];
					float2 sc( s0.x * ca - s0.y * sa, s0.x * sa + s0.y * ca );
					float3 dir = base + su * sc.x + sv * sc.y;
					float bestT = 1e30f;
					const int numTris = ( ( int )fs.buf.size() - fs.triBase() ) / 3;
					for( int t = 0; t < numTris; t++ )
					{
						const int b = fs.triBase() + t * 3;
						float4 r0 = buf[b + 0];
						float4 r1 = buf[b + 1];
						float4 r2 = buf[b + 2];
						float3 v0( r0.x, r0.y, r0.z ), v1( r1.x, r1.y, r1.z ), v2( r2.x, r2.y, r2.z );
						float3 edge1 = v1 - v0, edge2 = v2 - v0, sp = P - v0;
						float3 h = cross( dir, edge2 );
						float aa = dot( edge1, h );
						if( std::fabs( aa ) < 1e-12f )
						{
							continue;
						}
						float inv = 1.0f / aa;
						float u = inv * dot( sp, h );
						if( u < 0.0f || u > 1.0f )
						{
							continue;
						}
						float3 qq = cross( sp, edge1 );
						float vv = inv * dot( dir, qq );
						if( vv < 0.0f || u + vv > 1.0f )
						{
							continue;
						}
						float tt = inv * dot( edge2, qq );
						if( tt > 1e-4f && tt <= 1.0f && tt < bestT )
						{
							bestT = tt;
						}
					}
					if( bestT < 1e30f )
					{
						float d = bestT * len3( dir );	// world distance receiver -> blocker
						firstBlockers.Add( d );
						if( d > worstNear )
						{
							worstNear = d;
						}
						if( d > 2.0f )
						{
							allNear = false;
						}
					}
				}
				if( allNear && worstNear >= 0.0f )
				{
					engulfed++;
				}
				if( printedExamples < 3 && worstNear >= 0.0f )
				{
					std::printf( "    L%d BLACK vert (%.1f %.1f %.1f): farthest nearest-blocker %.2fu\n",
								 ( int )li, P.x, P.y, P.z, worstNear );
					printedExamples++;
				}
			}
		}
		if( totalV == 0 )
		{
			continue;
		}
		std::printf( "  L%d: %d verts sampled - lit %d, penumbra %d, BLACK %d (engulfed-within-2u: %d)\n",
					 ( int )li, totalV, litV, penV, blackV, engulfed );
		std::printf( "      blocker distances: <0.5u:%d  0.5-2u:%d  2-10u:%d  10-50u:%d  >50u:%d\n",
					 firstBlockers.buckets[0], firstBlockers.buckets[1], firstBlockers.buckets[2],
					 firstBlockers.buckets[3], firstBlockers.buckets[4] );
	}
	CHECK( true );
}

// POSE-MATCH probe: the ragdoll is BOTH a receiver surface (rendered pose, recvVerts) and a caster
// mesh (shadow pose, meshVerts). If the collect streams the same pose the renderer draws, every
// receiver vertex should have a caster vertex of its own mesh at ~0 distance; a systematic offset
// of units = the shadow is cast by a DIFFERENTLY-POSED copy of the body - which then darkens the
// rendered body from centimetres above it ("large black bands on top").
STUDY_TEST( SoftShadowRagdoll, pose_match_cap0042 )
{
	Cap cap;
	const char* paths[] = { "../tests/data/cap0042.cap", "tests/data/cap0042.cap", "neo/tests/data/cap0042.cap" };
	bool loaded = false;
	for( const char* p : paths )
	{
		if( LoadCap( p, cap ) )
		{
			loaded = true;
			break;
		}
	}
	if( !loaded )
	{
		std::printf( "  cap0042.cap not found - skipping\n" );
		return;
	}

	// for each receiver surface: find the best-matching caster mesh (smallest mean nearest-vertex
	// distance over a subsample) and report the match quality. O(recv * caster verts) brute force
	// on subsamples - study-tier cost.
	for( size_t ri = 0; ri < cap.receivers.size(); ri++ )
	{
		const capReceiver_t& rs = cap.receivers[ri];
		if( rs.numVerts < 200 )
		{
			continue;		// small brushes/decals: not the corpse
		}
		float bestMean = 1e30f, bestMax = 0.0f;
		int bestCaster = -1;
		for( size_t ci = 0; ci < cap.casters.size(); ci++ )
		{
			const capCaster_t& cs = cap.casters[ci];
			if( cs.lightIndex != rs.lightIndex || cs.numVerts < 200 )
			{
				continue;
			}
			double meanD = 0.0;
			float maxD = 0.0f;
			int n = 0;
			for( uint32_t v = 0; v < rs.numVerts; v += 23 )
			{
				const float* pv = &cap.recvVerts[( rs.firstVert + v ) * 3];
				float best = 1e30f;
				for( uint32_t cv = 0; cv < cs.numVerts; cv += 3 )
				{
					const float* qv = &cap.meshVerts[( cs.firstVert + cv ) * 3];
					float dx = pv[0] - qv[0], dy = pv[1] - qv[1], dz = pv[2] - qv[2];
					float d2 = dx * dx + dy * dy + dz * dz;
					if( d2 < best )
					{
						best = d2;
					}
				}
				best = std::sqrt( best );
				meanD += best;
				if( best > maxD )
				{
					maxD = best;
				}
				n++;
			}
			if( n > 0 && meanD / n < bestMean )
			{
				bestMean = ( float )( meanD / n );
				bestMax = maxD;
				bestCaster = ( int )ci;
			}
		}
		if( bestCaster >= 0 && bestMean < 100.0f )
		{
			const capCaster_t& cs = cap.casters[bestCaster];
			std::printf( "  recv %d (L%u, %u verts) ~ caster %d (id %.0f, %u verts): mean nearest %.2fu, max %.2fu %s\n",
						 ( int )ri, rs.lightIndex, rs.numVerts, bestCaster, cs.casterId, cs.numVerts,
						 bestMean, bestMax, bestMean > 1.0f ? "  <-- POSE MISMATCH?" : "" );
		}
	}
	CHECK( true );
}
