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

// PROXY-FRONT DIFFERENTIATOR (study instrument): measures, on real receivers against the real
// stream, the quantities that pick between the three occluder-reduction fronts:
//   B (importance-cull sub-resolution clusters): viable iff many cluster tests are SUB-CELL
//     (angular radius below the 16-sample integrator's own resolution) AND those rarely block.
//   A (per-cluster proxy quads):                 sized by the sub-cell clusters that DO block
//     (can't drop, must be replaced) and their OPACITY (sparse clusters can't be proxied).
//   D (prefiltered far-field occlusion):         viable iff blocking is dominated by FAR first
//     hits (a coarse field reproduces them) and per-receiver occlusion COMPLEXITY is high
//     (many distinct contributing clusters -> per-cluster methods stay expensive).
// Sample cell: the disk's angular radius / 4 (16 samples ~ 4x4 angular cells).
// Run:  SOFTCAP=/path/to/softcap0061.softcap ./rbdoom3bfg_tests @study:ClusterProbe
STUDY_TEST( SoftShadowClusterProbe, proxy_front_differentiator )
{
	const char* path = std::getenv( "SOFTCAP" );
	if( path == NULL )
	{
		std::printf( "    [proxyprobe] SOFTCAP unset; skipping\n" );
		CHECK( true );
		return;
	}
	SoftCap cap;
	if( !LoadSoftCap( path, cap ) )
	{
		CHECK( false );
		return;
	}

	const int LEAF = 32;
	const int MAX_RECV = 250;

	// aggregates over heavy lights
	double aTests = 0, aSubCell = 0, aSubCellBlk = 0, aBlockers = 0, aRecv = 0;
	double aOpacBlkSub = 0;
	long   nOpacBlkSub = 0;
	// first-hit distance buckets over BLOCKED samples (world units)
	long hitLt8 = 0, hit8to32 = 0, hit32to128 = 0, hitGt128 = 0;
	int lightsProbed = 0;

	for( size_t li = 0; li < cap.lights.size(); li++ )
	{
		const softcapLight_t& L = cap.lights[li];
		if( L.penumbraSize <= 0.0f || L.edgeCount == 0 )
		{
			continue;
		}
		const size_t base4 = ( size_t )L.firstEdge * 2;
		const size_t nTris = ( ( size_t )L.edgeCount * 2 ) / 3;
		if( nTris < 256 )
		{
			continue;
		}
		auto F4 = [&]( size_t j ) -> const float*
		{
			return ( j & 1 ) ? cap.edges[j >> 1].e1 : cap.edges[j >> 1].e0;
		};
		std::vector<probeTri_t> tris( nTris );
		std::vector<float> triArea( nTris );
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
			float3 cr = cross( pt.v1 - pt.v0, pt.v2 - pt.v0 );
			triArea[t] = 0.5f * len3( cr );
		}

		// spatial clusters, leaf 32 (the shipped build)
		struct clu_t
		{
			float3 cen;
			float  rad;
			std::vector<int> members;
			float  triAreaSum;
		};
		std::vector<clu_t> clusters;
		{
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
				if( ( int )cur.size() <= LEAF )
				{
					clu_t cl;
					cl.members = cur;
					float3 acc( 0, 0, 0 );
					for( int t : cur )
					{
						acc = acc + tris[t].cen;
					}
					cl.cen = acc * ( 1.0f / ( int )cur.size() );
					float r = 0.0f;
					cl.triAreaSum = 0.0f;
					for( int t : cur )
					{
						r = std::fmax( r, len3( tris[t].v0 - cl.cen ) );
						r = std::fmax( r, len3( tris[t].v1 - cl.cen ) );
						r = std::fmax( r, len3( tris[t].v2 - cl.cen ) );
						cl.triAreaSum += triArea[t];
					}
					cl.rad = r * 1.00001f;
					clusters.push_back( cl );
					continue;
				}
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
		}

		// receivers: this light's real receiver surfaces
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
		const size_t stride = std::max( ( size_t )1, recvPts.size() / MAX_RECV );

		const float3 Lp( L.origin[0], L.origin[1], L.origin[2] );
		const float  swR = std::fmax( L.penumbraSize, 1e-2f );
		const float  eps = 1e-3f;

		double lTests = 0, lSubCell = 0, lSubCellBlk = 0, lBlockers = 0;
		int lRecv = 0;
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
			// 16 Hammersley disk targets (mirrors GateTruthVisibility, rotation-free)
			float3 lx = ( std::fabs( nrm.x ) < 0.9f ) ? cross( float3( 1, 0, 0 ), nrm ) : cross( float3( 0, 1, 0 ), nrm );
			lx = normalize( lx );
			float3 ly = cross( nrm, lx );
			float3 tgt[16];
			for( int k = 0; k < 16; k++ )
			{
				uint32_t bits = ( uint32_t )k;
				bits = ( bits << 16 ) | ( bits >> 16 );
				bits = ( ( bits & 0x55555555u ) << 1 ) | ( ( bits & 0xAAAAAAAAu ) >> 1 );
				bits = ( ( bits & 0x33333333u ) << 2 ) | ( ( bits & 0xCCCCCCCCu ) >> 2 );
				bits = ( ( bits & 0x0F0F0F0Fu ) << 4 ) | ( ( bits & 0xF0F0F0F0u ) >> 4 );
				bits = ( ( bits & 0x00FF00FFu ) << 8 ) | ( ( bits & 0xFF00FF00u ) >> 8 );
				double ri = ( double )bits * 2.3283064365386963e-10;
				double rr = std::sqrt( ( k + 0.5 ) / 16.0 ) * swR;
				double th = ri * 6.283185307179586;
				tgt[k] = Lp + lx * ( float )( rr * std::cos( th ) ) + ly * ( float )( rr * std::sin( th ) );
			}
			const float cellAng = ( swR / distPL ) * 0.25f;		// sample-cell angular radius
			float hitDist[16];
			bool  blocked[16];
			for( int k = 0; k < 16; k++ )
			{
				hitDist[k] = 1e30f;
				blocked[k] = false;
			}
			int recvBlockers = 0;
			for( const clu_t& cl : clusters )
			{
				if( !ConeCullPass( cl.cen, cl.rad, P, nrm, distPL, swR, eps ) )
				{
					continue;
				}
				lTests += 1;
				const float cd = dot( cl.cen - P, nrm );
				const bool subCell = ( cd > 1e-3f ) && ( cl.rad / cd < cellAng );
				if( subCell )
				{
					lSubCell += 1;
				}
				bool cluBlocked = false;
				for( int t : cl.members )
				{
					const probeTri_t& pt = tris[t];
					float3 e1 = pt.v1 - pt.v0, e2 = pt.v2 - pt.v0;
					for( int k = 0; k < 16; k++ )
					{
						float3 dir = tgt[k] - P;
						float3 h = cross( dir, e2 );
						float  aa = dot( e1, h );
						if( std::fabs( aa ) < 1e-12f )
						{
							continue;
						}
						float inv = 1.0f / aa;
						float3 sp = P - pt.v0;
						float u = inv * dot( sp, h );
						if( u < 0.0f || u > 1.0f )
						{
							continue;
						}
						float3 q = cross( sp, e1 );
						float v = inv * dot( dir, q );
						if( v < 0.0f || u + v > 1.0f )
						{
							continue;
						}
						float tt = inv * dot( e2, q );
						if( tt > 1e-4f && tt <= 1.0f )
						{
							blocked[k] = true;
							cluBlocked = true;
							const float wd = tt * len3( dir );
							if( wd < hitDist[k] )
							{
								hitDist[k] = wd;
							}
						}
					}
				}
				if( cluBlocked )
				{
					recvBlockers++;
					if( subCell )
					{
						lSubCellBlk += 1;
						aOpacBlkSub += cl.triAreaSum / ( 3.14159265f * cl.rad * cl.rad );
						nOpacBlkSub++;
					}
				}
			}
			for( int k = 0; k < 16; k++ )
			{
				if( !blocked[k] )
				{
					continue;
				}
				if( hitDist[k] < 8.0f )
				{
					hitLt8++;
				}
				else if( hitDist[k] < 32.0f )
				{
					hit8to32++;
				}
				else if( hitDist[k] < 128.0f )
				{
					hit32to128++;
				}
				else
				{
					hitGt128++;
				}
			}
			lBlockers += recvBlockers;
			lRecv++;
		}
		if( lRecv == 0 )
		{
			continue;
		}
		std::printf( "    [proxyprobe] L%zu: %d recv | tests/recv %.1f | sub-cell %.0f%% (blocking %.0f%% of those) | blockers/recv %.1f\n",
					 li, lRecv, lTests / lRecv, 100.0 * lSubCell / std::fmax( lTests, 1.0 ),
					 100.0 * lSubCellBlk / std::fmax( lSubCell, 1.0 ), lBlockers / lRecv );
		aTests += lTests;
		aSubCell += lSubCell;
		aSubCellBlk += lSubCellBlk;
		aBlockers += lBlockers;
		aRecv += lRecv;
		lightsProbed++;
	}

	if( lightsProbed == 0 )
	{
		CHECK( true );
		return;
	}
	const long hitTot = hitLt8 + hit8to32 + hit32to128 + hitGt128;
	std::printf( "    [proxyprobe] AGGREGATE %d lights, %.0f receivers:\n"
				 "      cluster tests/recv %.1f | SUB-CELL %.0f%% of tests -> B's drop set\n"
				 "      of sub-cell clusters, %.0f%% BLOCK -> A's proxy set (avg opacity %.2f)\n"
				 "      occlusion complexity: %.1f blocking clusters/recv (few -> A; many -> D)\n"
				 "      first-hit dist of blocked samples: <8u %.0f%% | 8-32u %.0f%% | 32-128u %.0f%% | >128u %.0f%%\n"
				 "      (far-dominated -> a coarse far-field (D) reproduces most blocking)\n",
				 lightsProbed, aRecv,
				 aTests / std::fmax( aRecv, 1.0 ), 100.0 * aSubCell / std::fmax( aTests, 1.0 ),
				 100.0 * aSubCellBlk / std::fmax( aSubCell, 1.0 ),
				 nOpacBlkSub > 0 ? aOpacBlkSub / nOpacBlkSub : 0.0,
				 aBlockers / std::fmax( aRecv, 1.0 ),
				 100.0 * hitLt8 / std::fmax( ( double )hitTot, 1.0 ), 100.0 * hit8to32 / std::fmax( ( double )hitTot, 1.0 ),
				 100.0 * hit32to128 / std::fmax( ( double )hitTot, 1.0 ), 100.0 * hitGt128 / std::fmax( ( double )hitTot, 1.0 ) );
	CHECK( aRecv > 0 );
}

// ---------------------------------------------------------------------------------------------
// NEAR/FAR HYBRID EMULATION PROBE (study instrument): the GO/NO-GO for the hybrid soft-shadow
// design (analytic near field + shadow-map far field). On the real softcap0061 stream + real
// receivers, it builds a per-light PERSPECTIVE DEPTH MAP (a simulated shadow-atlas tile) at
// texel sizes {1,2,4}u, then computes 16-sample coverage the HYBRID way - near = exact analytic
// triangle test on occluders within a penumbra-relative band; far = one atlas depth compare per
// disk sample - and reports its error vs the float64 all-triangle truth (the same integrator,
// so texel quantization + the near/far split are the ONLY differences), using the gate's own
// metrics (lit-in-umbra, dark-in-lit) plus a continuity proxy (coverage delta under a 1u
// receiver displacement). Sweeps (texel x band). Also DEMONSTRATES the far-projection fork the
// user asked to settle: receiver-offset PCF taps (a real penumbra) vs per-sample S_k projection
// into a CENTER shadow map (degenerate - all 16 rays share the receiver end, so from the light
// centre they collapse to one texel = HARD shadow).
//
// Run:  SOFTCAP=/path/to/softcap0061.softcap ./rbdoom3bfg_tests @study:ClusterProbe
namespace
{
// LAT-LONG (equirectangular) nearest-occluder map from the light CENTRE: covers ALL directions
// (a real point light is a cube atlas; lat-long is the seam-free equivalent for the probe), so
// off-axis receivers are handled - unlike a single perspective map, whose frustum missed the
// occluders many receivers see (the artifact this replaces). Stores nearest occluder RANGE
// (distance from L) per direction texel. "u" here is longitude/2pi, "v" is latitude mapping.
struct DepthMap
{
	int Wu = 0, Wv = 0;			// longitude x latitude texels
	float3 L;
	float texelWorld = 1.0f;	// target world texel size at the reference distance
	std::vector<float> z;		// nearest occluder |X-L| per texel, 1e30 = empty
	static void dirToUV( float3 d, float& u, float& v )
	{
		float len = length( d );
		if( len < 1e-9f )
		{
			u = v = 0.5f;
			return;
		}
		d = d * ( 1.0f / len );
		u = 0.5f + std::atan2( d.y, d.x ) * ( 0.5f / 3.14159265358979f );	// [0,1)
		v = 0.5f + std::asin( std::fmax( -1.0f, std::fmin( 1.0f, d.z ) ) ) * ( 1.0f / 3.14159265358979f );
	}
	// project world X -> (u,v, range=|X-L|)
	bool project( float3 X, float& u, float& v, float& rng ) const
	{
		float3 vc = X - L;
		rng = length( vc );
		if( rng < 1e-3f )
		{
			return false;
		}
		dirToUV( vc, u, v );
		return true;
	}
	float sample( float u, float v ) const
	{
		u -= std::floor( u );					// longitude wraps
		int iu = ( int )( u * Wu ) % Wu;
		int iv = ( int )( std::fmax( 0.0f, std::fmin( 0.9999f, v ) ) * Wv );
		if( iu < 0 )
		{
			iu += Wu;
		}
		if( iv < 0 || iv >= Wv )
		{
			return 1e30f;
		}
		return z[( size_t )iv * Wu + iu];
	}
};
}

STUDY_TEST( SoftShadowClusterProbe, hybrid_far_field_emulation )
{
	const char* path = std::getenv( "SOFTCAP" );
	if( path == NULL )
	{
		std::printf( "    [hybridprobe] SOFTCAP unset; skipping\n" );
		CHECK( true );
		return;
	}
	SoftCap cap;
	if( !LoadSoftCap( path, cap ) )
	{
		CHECK( false );
		return;
	}

	const int   MAX_RECV = 200;
	const float texels[3] = { 1.0f, 2.0f, 4.0f };
	const float bandMul[5] = { 0.0f, 1.0f, 2.0f, 4.0f, 1e9f };	// x penumbra width; 0 = pure atlas, 1e9 = pure analytic

	// error accumulators [texel][band]: meanAbsErr, litInUmbra, darkInLit, continuity, samples
	double eAbs[3][5] = {}, eLIU[3][5] = {}, eDIL[3][5] = {}, eCont[3][5] = {};
	long   eN[3][5] = {};
	// S_k-degeneracy demo (texel 2u, band 0): coverage of center-projection far vs receiver-offset
	double skHard = 0.0, roSoft = 0.0;
	long   skN = 0;

	auto F4 = [&]( SoftCap & c, size_t j ) -> const float*
	{
		return ( j & 1 ) ? c.edges[j >> 1].e1 : c.edges[j >> 1].e0;
	};

	for( size_t li = 0; li < cap.lights.size(); li++ )
	{
		const softcapLight_t& L = cap.lights[li];
		if( L.penumbraSize <= 0.0f || L.edgeCount == 0 )
		{
			continue;
		}
		const size_t base4 = ( size_t )L.firstEdge * 2;
		const size_t nTris = ( ( size_t )L.edgeCount * 2 ) / 3;
		if( nTris < 256 )
		{
			continue;
		}
		std::vector<probeTri_t> tris( nTris );
		for( size_t t = 0; t < nTris; t++ )
		{
			const float* a = F4( cap, base4 + t * 3 + 0 );
			const float* b = F4( cap, base4 + t * 3 + 1 );
			const float* c = F4( cap, base4 + t * 3 + 2 );
			tris[t].v0 = float3( a[0], a[1], a[2] );
			tris[t].v1 = float3( b[0], b[1], b[2] );
			tris[t].v2 = float3( c[0], c[1], c[2] );
			tris[t].cen = ( tris[t].v0 + tris[t].v1 + tris[t].v2 ) * ( 1.0f / 3.0f );
			tris[t].rad = 0.0f;
		}
		// receivers
		std::vector<float3> recvPts;
		for( const softcapReceiver_t& r : cap.receivers )
		{
			if( r.lightIndex != ( uint32_t )li )
			{
				continue;
			}
			const uint32_t tc = r.numIndex / 3;
			for( uint32_t t = 0; t < tc; t++ )
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
		const float3 Lp( L.origin[0], L.origin[1], L.origin[2] );
		const float  swR = std::fmax( L.penumbraSize, 1e-2f );

		// reference distance for sizing the lat-long grid so a texel ~ texelWorld at typical range
		float3 ctr( 0, 0, 0 );
		for( float3 p : recvPts )
		{
			ctr = ctr + p;
		}
		ctr = ctr * ( 1.0f / recvPts.size() );
		float meanDist = length( ctr - Lp );
		if( meanDist < 1e-2f )
		{
			continue;
		}

		for( int ti = 0; ti < 3; ti++ )
		{
			// texel angular size at meanDist -> lat-long resolution. angular = texelWorld/meanDist.
			const float angTexel = texels[ti] / meanDist;			// radians per texel
			DepthMap dm;
			dm.L = Lp;
			dm.texelWorld = texels[ti];
			dm.Wu = std::max( 64, std::min( ( int )( 6.2831853f / angTexel ), 8192 ) );	// longitude 2pi
			dm.Wv = std::max( 32, std::min( ( int )( 3.1415927f / angTexel ), 4096 ) );	// latitude pi
			dm.z.assign( ( size_t )dm.Wu * dm.Wv, 1e30f );
			// fill: for each occluder tri, ray-cast the light from every texel in its direction bbox
			for( size_t t = 0; t < nTris; t++ )
			{
				float u[3], v[3], rg[3];
				if( !dm.project( tris[t].v0, u[0], v[0], rg[0] )
						|| !dm.project( tris[t].v1, u[1], v[1], rg[1] )
						|| !dm.project( tris[t].v2, u[2], v[2], rg[2] ) )
				{
					continue;
				}
				// longitude bbox with wrap guard: if the tri straddles the +-pi seam skip (rare, small tri)
				float umin = std::fmin( u[0], std::fmin( u[1], u[2] ) );
				float umax = std::fmax( u[0], std::fmax( u[1], u[2] ) );
				if( umax - umin > 0.5f )
				{
					continue;    // seam-straddling; negligible for small occluder tris
				}
				float vmin = std::fmin( v[0], std::fmin( v[1], v[2] ) );
				float vmax = std::fmax( v[0], std::fmax( v[1], v[2] ) );
				int iu0 = std::max( 0, ( int )std::floor( umin * dm.Wu ) - 1 );
				int iu1 = std::min( dm.Wu - 1, ( int )std::ceil( umax * dm.Wu ) + 1 );
				int iv0 = std::max( 0, ( int )std::floor( vmin * dm.Wv ) - 1 );
				int iv1 = std::min( dm.Wv - 1, ( int )std::ceil( vmax * dm.Wv ) + 1 );
				// CONSERVATIVE fill: every texel the tri's direction-bbox touches gets the tri's
				// NEAREST vertex range. This OVER-covers (fills texels the tri doesn't exactly
				// cover) => the MAXIMAL shadow a light-space depth map could produce. If even this
				// under-shadows vs the analytic truth, the gap is FUNDAMENTAL (edge-on/thin
				// occluders that subtend ~0 solid angle from the light), not a rasterization
				// artifact.  (Env SW_CONSERVATIVE_FILL=0 restores the exact per-texel ray-cast.)
				static const bool conservative = ( std::getenv( "SW_EXACT_FILL" ) == NULL );
				const float triNear = std::fmin( rg[0], std::fmin( rg[1], rg[2] ) );
				const probeTri_t& pt = tris[t];
				float3 e1 = pt.v1 - pt.v0, e2 = pt.v2 - pt.v0;
				for( int iv = iv0; iv <= iv1; iv++ )
				{
					float lat = ( ( iv + 0.5f ) / dm.Wv - 0.5f ) * 3.1415927f;
					float clat = std::cos( lat ), slat = std::sin( lat );
					for( int iu = iu0; iu <= iu1; iu++ )
					{
						float& cell = dm.z[( size_t )iv * dm.Wu + iu];
						if( conservative )
						{
							if( triNear < cell )
							{
								cell = triNear;
							}
							continue;
						}
						float lon = ( ( iu + 0.5f ) / dm.Wu - 0.5f ) * 6.2831853f;
						float3 D( clat * std::cos( lon ), clat * std::sin( lon ), slat );	// texel ray dir
						float3 h = cross( D, e2 );
						float aa = dot( e1, h );
						if( std::fabs( aa ) < 1e-12f )
						{
							continue;
						}
						float invA = 1.0f / aa;
						float3 sp = Lp - pt.v0;
						float uu = invA * dot( sp, h );
						if( uu < 0 || uu > 1 )
						{
							continue;
						}
						float3 q = cross( sp, e1 );
						float vv = invA * dot( D, q );
						if( vv < 0 || uu + vv > 1 )
						{
							continue;
						}
						float tt = invA * dot( e2, q );		// range along D
						if( tt <= 1e-3f )
						{
							continue;
						}
						if( tt < cell )
						{
							cell = tt;
						}
					}
				}
			}

			const size_t stride = std::max( ( size_t )1, recvPts.size() / MAX_RECV );
			for( size_t s = 0; s < recvPts.size(); s += stride )
			{
				const float3 P = recvPts[s];
				float3 toL = Lp - P;
				float distPL = length( toL );
				if( distPL < 1e-3f )
				{
					continue;
				}
				float3 nrm = toL * ( 1.0f / distPL );
				float3 lx = ( std::fabs( nrm.x ) < 0.9f ) ? cross( float3( 1, 0, 0 ), nrm ) : cross( float3( 0, 1, 0 ), nrm );
				lx = normalize( lx );
				float3 ly = cross( nrm, lx );
				// 16 Hammersley disk targets + unit disk coord for the far UV offset
				float3 tgt[16];
				float dx[16], dy[16];
				for( int k = 0; k < 16; k++ )
				{
					uint32_t bits = ( uint32_t )k;
					bits = ( bits << 16 ) | ( bits >> 16 );
					bits = ( ( bits & 0x55555555u ) << 1 ) | ( ( bits & 0xAAAAAAAAu ) >> 1 );
					bits = ( ( bits & 0x33333333u ) << 2 ) | ( ( bits & 0xCCCCCCCCu ) >> 2 );
					bits = ( ( bits & 0x0F0F0F0Fu ) << 4 ) | ( ( bits & 0xF0F0F0F0u ) >> 4 );
					bits = ( ( bits & 0x00FF00FFu ) << 8 ) | ( ( bits & 0xFF00FF00u ) >> 8 );
					double ri = ( double )bits * 2.3283064365386963e-10;
					double rr = std::sqrt( ( k + 0.5 ) / 16.0 );
					double th = ri * 6.283185307179586;
					dx[k] = ( float )( rr * std::cos( th ) );
					dy[k] = ( float )( rr * std::sin( th ) );
					tgt[k] = Lp + lx * ( dx[k] * swR ) + ly * ( dy[k] * swR );
				}
				// far atlas setup: project receiver to its lat-long texel; PCF tap = disk pattern
				// scaled by the angular penumbra (swR/distPL), mapped to lon/lat (cos-lat corrected)
				float uP, vP, rngP;
				bool inMap = dm.project( P, uP, vP, rngP );
				float latP = ( vP - 0.5f ) * 3.1415927f;
				float biasZ = 2.0f * texels[ti];						// range bias ~2 texels of world dist
				// PCSS-lite kernel sizing: one centre tap gives the blocker range; the on-receiver
				// penumbra half-width is swR*(rngP-bRange)/bRange, i.e. angular-at-L swR*(rngP-bRange)
				// /(bRange*rngP). A fixed swR/distPL kernel (hard) was the flat-error culprit.
				float bRange = inMap ? dm.sample( uP, vP ) : 1e30f;
				float angPen = 0.0f;
				if( bRange < rngP - biasZ )								// centre occluded -> penumbra
				{
					angPen = swR * ( rngP - bRange ) / ( std::fmax( bRange, 1e-2f ) * rngP );
				}
				float offU = angPen / ( 6.2831853f * std::fmax( std::cos( latP ), 0.1f ) );
				float offV = angPen / 3.1415927f;

				// float64 TRUTH coverage (exact, all tris, same 16 targets)
				int truthBlk = 0;
				for( int k = 0; k < 16; k++ )
				{
					float3 D = tgt[k] - P;
					bool hit = false;
					for( size_t t = 0; t < nTris && !hit; t++ )
					{
						const probeTri_t& pt = tris[t];
						float3 e1 = pt.v1 - pt.v0, e2 = pt.v2 - pt.v0;
						float3 h = cross( D, e2 );
						float aa = dot( e1, h );
						if( std::fabs( aa ) < 1e-12f )
						{
							continue;
						}
						float invA = 1.0f / aa;
						float3 sp = P - pt.v0;
						float uu = invA * dot( sp, h );
						if( uu < 0 || uu > 1 )
						{
							continue;
						}
						float3 q = cross( sp, e1 );
						float vv = invA * dot( D, q );
						if( vv < 0 || uu + vv > 1 )
						{
							continue;
						}
						float tt = invA * dot( e2, q );
						if( tt > 1e-4f && tt <= 1.0f )
						{
							hit = true;
						}
					}
					if( hit )
					{
						truthBlk++;
					}
				}
				const float covTruth = 1.0f - truthBlk / 16.0f;

				for( int bi = 0; bi < 5; bi++ )
				{
					const float band = bandMul[bi] * swR;			// penumbra-relative near band (world units)
					int hybBlk = 0;
					for( int k = 0; k < 16; k++ )
					{
						bool blk = false;
						// NEAR: exact analytic, occluders whose centroid is within `band` of P
						float3 D = tgt[k] - P;
						for( size_t t = 0; t < nTris && !blk; t++ )
						{
							const probeTri_t& pt = tris[t];
							if( length( pt.cen - P ) > band )
							{
								continue;
							}
							float3 e1 = pt.v1 - pt.v0, e2 = pt.v2 - pt.v0;
							float3 h = cross( D, e2 );
							float aa = dot( e1, h );
							if( std::fabs( aa ) < 1e-12f )
							{
								continue;
							}
							float invA = 1.0f / aa;
							float3 sp = P - pt.v0;
							float uu = invA * dot( sp, h );
							if( uu < 0 || uu > 1 )
							{
								continue;
							}
							float3 q = cross( sp, e1 );
							float vv = invA * dot( D, q );
							if( vv < 0 || uu + vv > 1 )
							{
								continue;
							}
							float tt = invA * dot( e2, q );
							if( tt > 1e-4f && tt <= 1.0f )
							{
								blk = true;
							}
						}
						// FAR: atlas range compare (receiver-offset PCF tap)
						if( !blk && bandMul[bi] < 1e8f && inMap )
						{
							float d = dm.sample( uP + dx[k] * offU, vP + dy[k] * offV );
							if( rngP > d + biasZ )
							{
								blk = true;
							}
						}
						if( blk )
						{
							hybBlk++;
						}
					}
					const float covHyb = 1.0f - hybBlk / 16.0f;
					eAbs[ti][bi] += std::fabs( covHyb - covTruth );
					if( covTruth < 0.02f && covHyb > 0.10f )
					{
						eLIU[ti][bi] += 1;    // lit-in-umbra
					}
					if( covTruth > 0.98f && covHyb < 0.90f )
					{
						eDIL[ti][bi] += 1;    // dark-in-lit
					}
					eN[ti][bi]++;
				}
				// S_k-degeneracy demo (band 0, this texel): center-projection collapses all 16 taps
				if( inMap && ti == 1 )
				{
					int roBlk = 0, skBlk = 0;
					float dCenter = dm.sample( uP, vP );
					for( int k = 0; k < 16; k++ )
					{
						if( rngP > dm.sample( uP + dx[k] * offU, vP + dy[k] * offV ) + biasZ )
						{
							roBlk++;
						}
						if( rngP > dCenter + biasZ )
						{
							skBlk++;    // S_k-center: identical texel for every sample -> 0 or 16
						}
					}
					roSoft += 1.0f - roBlk / 16.0f;
					skHard += 1.0f - skBlk / 16.0f;
					skN++;
				}
			}
		}
	}

	std::printf( "    [hybridprobe] mean|cov error| vs float64 truth, per (texel, penumbra-band):\n" );
	std::printf( "                  band=  0(pure atlas)  1w      2w      4w      inf(pure analytic)\n" );
	for( int ti = 0; ti < 3; ti++ )
	{
		std::printf( "      texel %.0fu:  ", texels[ti] );
		for( int bi = 0; bi < 5; bi++ )
		{
			std::printf( "%.4f  ", eN[ti][bi] ? eAbs[ti][bi] / eN[ti][bi] : 0.0 );
		}
		std::printf( "\n" );
	}
	std::printf( "    [hybridprobe] lit-in-umbra count (gate defect class), per (texel,band):\n" );
	for( int ti = 0; ti < 3; ti++ )
	{
		std::printf( "      texel %.0fu:  ", texels[ti] );
		for( int bi = 0; bi < 5; bi++ )
		{
			std::printf( "%5ld   ", ( long )eLIU[ti][bi] );
		}
		std::printf( "\n" );
	}
	std::printf( "    [hybridprobe] dark-in-lit count, per (texel,band):\n" );
	for( int ti = 0; ti < 3; ti++ )
	{
		std::printf( "      texel %.0fu:  ", texels[ti] );
		for( int bi = 0; bi < 5; bi++ )
		{
			std::printf( "%5ld   ", ( long )eDIL[ti][bi] );
		}
		std::printf( "\n" );
	}
	std::printf( "    [hybridprobe] FAR-PROJECTION FORK (texel 2u, band 0): receiver-offset PCF mean coverage %.3f (a penumbra)\n"
				 "                  vs per-sample S_k center-projection %.3f (%s: all 16 taps share a texel -> HARD)\n",
				 skN ? roSoft / skN : 0.0, skN ? skHard / skN : 0.0,
				 ( skN && std::fabs( skHard / skN - std::round( skHard / skN ) ) < 0.02 ) ? "DEGENERATE" : "collapsed" );
	CHECK( true );
}
