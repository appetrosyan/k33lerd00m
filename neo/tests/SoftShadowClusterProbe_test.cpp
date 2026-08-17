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

// CLUSTER-HIERARCHY AMORTIZATION PROBE (study instrument, not a test).
//
// GO/NO-GO gate for the stream-v3 cluster (meshlet) hierarchy: before committing to the format
// migration, measure - on a REAL capture's exact triangle stream and real receiver surfaces -
// how much per-fragment cull work a second bounding level would save. The fragment walk today
// tests EVERY tile-listed triangle against the fragment's sample cone (~2.5k tris/tile at
// softcap0061 density, the measured ~12 ms residual); a 64-tri cluster sphere rejects its whole
// span for the price of one test, IF consecutive-index clusters are spatially tight enough that
// cluster-level rejection tracks triangle-level rejection.
//
//   flat cost       = nTris                      (every tri gets a cone test)
//   two-level cost  = nClusters + sum of surviving clusters' tri counts
//   reduction       = flat / two-level           (GO criterion: >= 3x on the heavy lights)
//
// Run:  SOFTCAP=/path/to/softcap0061.softcap ./rbdoom3bfg_tests @study:ClusterProbe

#include "hlsl_compat.h"
float SoftShadow_WedgeOcclusion( float3 swP, float3 swL, float swR, int swFirstElem, int swN, float swCentreLit, SoftEdgeBuffer t_SoftEdges );
float SoftShadow_FaceCoverage( float3 swP, float3 swL, float swR, int swTriBase, int swCasterBase, int swCasterCount, float swRotAng, SoftEdgeBuffer t_SoftEdges );
#include "SoftShadowBox.h"
#include "SoftShadowMesh.h"
#include "idUnitTest.h"

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <algorithm>

using namespace swtest;

namespace
{

struct probeTri_t
{
	float3 v0, v1, v2;
	float3 cen;
	float  rad;			// centroid radius (the walk's tight bound)
};

// the fragment walk's tight cone/slab cull, mirrored (softwedge_coverage.inc.hlsl, squared form)
static bool ConeCullPass( const float3& c, float r, const float3& P, const float3& nrm,
						  float distPL, float swR, float eps )
{
	float3 rc = c - P;
	float  cd = dot( rc, nrm );
	if( cd + r < eps )
	{
		return false;
	}
	if( cd - r > distPL )
	{
		return false;
	}
	float3 perp = rc - nrm * cd;
	float  coneR = swR * ( cd + r ) / distPL;
	return dot( perp, perp ) <= ( coneR + r ) * ( coneR + r );
}

}

STUDY_TEST( SoftShadowClusterProbe, amortization_on_real_capture )
{
	const char* path = std::getenv( "SOFTCAP" );
	if( path == NULL )
	{
		std::printf( "    [clusterprobe] SOFTCAP unset; skipping (set it to a .softcap)\n" );
		CHECK( true );
		return;
	}
	SoftCap cap;
	if( !LoadSoftCap( path, cap ) )
	{
		std::printf( "    [clusterprobe] cannot load %s\n", path );
		CHECK( false );
		return;
	}

	const int CLUSTER_TRIS = 64;
	const int MAX_RECV_SAMPLES = 400;

	double aggFlat = 0.0, aggTwo = 0.0;		// tri-count-weighted aggregate over the heavy lights
	int lightsProbed = 0;

	for( size_t li = 0; li < cap.lights.size(); li++ )
	{
		const softcapLight_t& L = cap.lights[li];
		if( L.penumbraSize <= 0.0f || L.edgeCount == 0 )
		{
			continue;
		}
		// STREAM V2 decode: the light's pair-record range is a pure-tri float4 stream, 3 float4/tri,
		// zero-padded to an even float4 count (same decode as the gate's float64 judge).
		const size_t base4 = ( size_t )L.firstEdge * 2;
		const size_t nTris = ( ( size_t )L.edgeCount * 2 ) / 3;
		if( nTris < 256 )
		{
			continue;			// only the heavy lights matter for the 12 ms
		}
		auto F4 = [&]( size_t j ) -> const float*
		{
			return ( j & 1 ) ? cap.edges[j >> 1].e1 : cap.edges[j >> 1].e0;
		};
		std::vector<probeTri_t> tris( nTris );
		for( size_t t = 0; t < nTris; t++ )
		{
			const float* a = F4( base4 + t * 3 + 0 );
			const float* b = F4( base4 + t * 3 + 1 );
			const float* c = F4( base4 + t * 3 + 2 );
			probeTri_t& pt = tris[t];
			pt.v0 = float3( a[0], a[1], a[2] );
			pt.v1 = float3( b[0], b[1], b[2] );
			pt.v2 = float3( c[0], c[1], c[2] );
			pt.cen = ( pt.v0 + pt.v1 + pt.v2 ) * ( 1.0f / 3.0f );
			pt.rad = std::fmax( len3( pt.v0 - pt.cen ), std::fmax( len3( pt.v1 - pt.cen ), len3( pt.v2 - pt.cen ) ) ) * 1.00001f;
		}

		// two cluster variants:
		//   NAIVE   - consecutive stream-order chunks (free at build time)
		//   SPATIAL - k-d median split on tri centroids into <=64-tri leaves (a cheap build-time
		//             reorder; legal because the union coverage is order-independent)
		struct cluster_t
		{
			float3 cen;
			float  rad;
			int    count;
			std::vector<int> members;	// probe only; the real stream stores contiguous spans
		};
		auto boundCluster = [&]( cluster_t & cl )
		{
			float3 acc( 0, 0, 0 );
			for( int t : cl.members )
			{
				acc = acc + tris[t].cen;
			}
			cl.cen = acc * ( 1.0f / ( int )cl.members.size() );
			float r = 0.0f;
			for( int t : cl.members )
			{
				const probeTri_t& pt = tris[t];
				r = std::fmax( r, len3( pt.v0 - cl.cen ) );
				r = std::fmax( r, len3( pt.v1 - cl.cen ) );
				r = std::fmax( r, len3( pt.v2 - cl.cen ) );
			}
			cl.rad = r * 1.00001f;
			cl.count = ( int )cl.members.size();
		};
		std::vector<cluster_t> naive;
		for( size_t f = 0; f < nTris; f += CLUSTER_TRIS )
		{
			cluster_t cl;
			for( size_t k = f; k < std::min( f + CLUSTER_TRIS, nTris ); k++ )
			{
				cl.members.push_back( ( int )k );
			}
			boundCluster( cl );
			naive.push_back( cl );
		}
		auto buildSpatial = [&]( int leafTris ) -> std::vector<cluster_t>
		{
			std::vector<cluster_t> out;
			std::vector<int> all( nTris );
			for( size_t t = 0; t < nTris; t++ )
			{
				all[t] = ( int )t;
			}
			std::vector<std::vector<int>> stack;
			stack.push_back( all );
			while( !stack.empty() )
			{
				std::vector<int> cur = std::move( stack.back() );
				stack.pop_back();
				if( ( int )cur.size() <= leafTris )
				{
					cluster_t cl;
					cl.members = cur;
					boundCluster( cl );
					out.push_back( cl );
					continue;
				}
				// split at the median of the longest centroid-extent axis
				float3 mn( 1e30f, 1e30f, 1e30f ), mx( -1e30f, -1e30f, -1e30f );
				for( int t : cur )
				{
					const float3& c = tris[t].cen;
					mn = float3( std::fmin( mn.x, c.x ), std::fmin( mn.y, c.y ), std::fmin( mn.z, c.z ) );
					mx = float3( std::fmax( mx.x, c.x ), std::fmax( mx.y, c.y ), std::fmax( mx.z, c.z ) );
				}
				float3 ext = mx - mn;
				int axis = ( ext.x >= ext.y && ext.x >= ext.z ) ? 0 : ( ( ext.y >= ext.z ) ? 1 : 2 );
				auto key = [&]( int t ) -> float
				{
					const float3& c = tris[t].cen;
					return axis == 0 ? c.x : ( axis == 1 ? c.y : c.z );
				};
				std::nth_element( cur.begin(), cur.begin() + cur.size() / 2, cur.end(),
								  [&]( int a, int b )
				{
					return key( a ) < key( b );
				} );
				std::vector<int> lo( cur.begin(), cur.begin() + cur.size() / 2 );
				std::vector<int> hi( cur.begin() + cur.size() / 2, cur.end() );
				stack.push_back( std::move( lo ) );
				stack.push_back( std::move( hi ) );
			}
			return out;
		};
		const int leafSizes[3] = { 32, 64, 128 };
		std::vector<cluster_t> spatialV[3];
		for( int v = 0; v < 3; v++ )
		{
			spatialV[v] = buildSpatial( leafSizes[v] );
		}
		double sumTriRad = 0.0, sumNaiveRad = 0.0;
		for( size_t t = 0; t < nTris; t++ )
		{
			sumTriRad += tris[t].rad;
		}
		for( const cluster_t& cl : naive )
		{
			sumNaiveRad += cl.rad;
		}

		// receiver population: this light's REAL receiver surfaces from the capture, sampled at
		// triangle centroids (fragments live on exactly these surfaces)
		std::vector<float3> recvPts;
		for( const softcapReceiver_t& r : cap.receivers )
		{
			if( r.lightIndex != ( uint32_t )li )
			{
				continue;
			}
			const uint32_t triCount = r.numIndex / 3;
			for( uint32_t t = 0; t < triCount; t++ )
			{
				const uint32_t i0 = cap.recvIdx[r.firstIndex + t * 3 + 0];
				const uint32_t i1 = cap.recvIdx[r.firstIndex + t * 3 + 1];
				const uint32_t i2 = cap.recvIdx[r.firstIndex + t * 3 + 2];
				float3 p0( cap.recvVerts[( r.firstVert + i0 ) * 3], cap.recvVerts[( r.firstVert + i0 ) * 3 + 1], cap.recvVerts[( r.firstVert + i0 ) * 3 + 2] );
				float3 p1( cap.recvVerts[( r.firstVert + i1 ) * 3], cap.recvVerts[( r.firstVert + i1 ) * 3 + 1], cap.recvVerts[( r.firstVert + i1 ) * 3 + 2] );
				float3 p2( cap.recvVerts[( r.firstVert + i2 ) * 3], cap.recvVerts[( r.firstVert + i2 ) * 3 + 1], cap.recvVerts[( r.firstVert + i2 ) * 3 + 2] );
				recvPts.push_back( ( p0 + p1 + p2 ) * ( 1.0f / 3.0f ) );
			}
		}
		if( recvPts.empty() )
		{
			continue;
		}
		const size_t stride = std::max( ( size_t )1, recvPts.size() / MAX_RECV_SAMPLES );

		const float3 Lp( L.origin[0], L.origin[1], L.origin[2] );
		const float  swR = std::fmax( L.penumbraSize, 1e-2f );
		const float  eps = 1e-3f;

		double sumFlatPass = 0.0, sumNaiveTris = 0.0;
		double sumSpatialTris[3] = { 0.0, 0.0, 0.0 };
		int samples = 0;
		for( size_t s = 0; s < recvPts.size(); s += stride )
		{
			const float3 P = recvPts[s];
			float3 toL = Lp - P;
			float distPL = len3( toL );
			if( distPL < 1e-3f )
			{
				continue;
			}
			float3 nrm = toL * ( 1.0f / distPL );
			int flatPass = 0;
			for( size_t t = 0; t < nTris; t++ )
			{
				if( ConeCullPass( tris[t].cen, tris[t].rad, P, nrm, distPL, swR, eps ) )
				{
					flatPass++;
				}
			}
			int naiveTris = 0;
			for( const cluster_t& cl : naive )
			{
				if( ConeCullPass( cl.cen, cl.rad, P, nrm, distPL, swR, eps ) )
				{
					naiveTris += cl.count;
				}
			}
			for( int v = 0; v < 3; v++ )
			{
				int st = 0;
				for( const cluster_t& cl : spatialV[v] )
				{
					if( ConeCullPass( cl.cen, cl.rad, P, nrm, distPL, swR, eps ) )
					{
						st += cl.count;
					}
				}
				sumSpatialTris[v] += st;
			}
			sumFlatPass  += flatPass;
			sumNaiveTris += naiveTris;
			samples++;
		}
		if( samples == 0 )
		{
			continue;
		}
		const double flatCost  = ( double )nTris;
		const double naiveCost = ( double )naive.size() + sumNaiveTris / samples;
		std::printf( "    [clusterprobe] L%zu: %zu tris (avgTriRad %.1f), %d recv samples, flat pass %.0f\n"
					 "                   NAIVE/64 %zu clusters (avgRad %.1f): %.0f tests, %.2fx\n",
					 li, nTris, sumTriRad / ( double )nTris, samples, sumFlatPass / samples,
					 naive.size(), sumNaiveRad / ( double )naive.size(), naiveCost, flatCost / naiveCost );
		double spatialCost64 = 0.0;
		for( int v = 0; v < 3; v++ )
		{
			const double c = ( double )spatialV[v].size() + sumSpatialTris[v] / samples;
			std::printf( "                   SPATIAL/%-3d %4zu clusters: %.0f tests, %.2fx\n",
						 leafSizes[v], spatialV[v].size(), c, flatCost / c );
			if( leafSizes[v] == 64 )
			{
				spatialCost64 = c;
			}
		}
		aggFlat += flatCost * samples;
		aggTwo  += spatialCost64 * samples;		// the aggregate GO metric uses SPATIAL/64
		lightsProbed++;
	}

	if( lightsProbed == 0 )
	{
		std::printf( "    [clusterprobe] no heavy lights (>=256 tris) with receivers in this capture\n" );
		CHECK( true );
		return;
	}
	std::printf( "    [clusterprobe] AGGREGATE over %d heavy lights: reduction %.2fx  (GO criterion: >= 3x)\n",
				 lightsProbed, aggFlat / aggTwo );
	CHECK( aggTwo < aggFlat );		// two-level must at least not be WORSE
}
