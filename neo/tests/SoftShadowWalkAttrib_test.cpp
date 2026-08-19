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

// WALK-ATTRIBUTION INSTRUMENT (study, not a test). Measure-first: the granular GPU phase timer says
// the coverage WALK is ~17ms of a 27ms frame on the heavy captures, but NOT where inside the walk it
// goes. This runs the EXACT shipped walker (SoftShadow_FaceCoverage, compiled as C++ via hlsl_compat)
// over a real .softcap's receivers with the SW_ATTRIB op-counters ON, and reports the breakdown:
//   caster-sphere tests/rejects | per-triangle COARSE (v0) tests/rejects | per-triangle TIGHT
//   (centroid cone) tests/rejects | triangles reaching the Moller-Trumbore + 16-sample loop.
// The last one (mtTri) x 16 is the expensive sample work; the rest is cull volume. So the numbers say
// whether the 17ms is cull-bound (too many triangles tested) or sample-bound (too many survivors), and
// a per-caster tri-count histogram says whether a few huge casters dominate. NOTHING is assumed - the
// optimisation candidate is chosen from these numbers.
//
// NOTE the shipped GPU path runs the TILE-BINNED walker (SoftShadow_FaceCoverageList), which pre-culls
// at tile grain; this CPU study runs the FULL walk (all casters). The per-triangle cull/MT ratios are
// caster-grouping-independent and transfer; the GPU per-fragment counters (r_softShadowWalkCounters)
// measure the real tile-list path for cross-check.
//
// Run:  SOFTCAP=/path/to/foo.softcap ./rbdoom3bfg_tests @study:SoftShadowWalkAttrib

#include "hlsl_compat.h"
#include "softwedge_coverage.inc.hlsl"		// the live shader source, compiled as C++ (SW_FUNC=inline)
#include "SoftShadowBox.h"					// SoftRotAngle, namespace swtest
#include "SoftShadowMesh.h"					// SoftCap + LoadSoftCap
#include "idUnitTest.h"

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <algorithm>

// The ONE definition of the SW_ATTRIB counter globals for the whole test binary (declared extern in
// softwedge_coverage.inc.hlsl). Every test TU that includes the walker references these; defining them
// here (and nowhere else) keeps the inline walker bodies identical across TUs -> no ODR divergence.
swAttrib_t g_swAttrib = {};
bool       g_swAttribOn = false;

using namespace swtest;

STUDY_TEST( SoftShadowWalkAttrib, breakdown )
{
	const char* path = std::getenv( "SOFTCAP" );
	if( path == NULL )
	{
		std::printf( "    [walkattrib] SOFTCAP unset; skipping (set it to a .softcap)\n" );
		CHECK( true );
		return;
	}
	SoftCap cap;
	if( !LoadSoftCap( path, cap ) )
	{
		std::printf( "    [walkattrib] cannot load %s\n", path );
		CHECK( false );
		return;
	}

	const int MAX_RECV = 400;		// per-light receiver sample budget

	// reinterpret the capture's edge block as the flat float4 tri stream (2 float4 per softcapEdge_t)
	auto F4 = [&]( size_t j ) -> const float*
	{
		return ( j & 1 ) ? cap.edges[j >> 1].e1 : cap.edges[j >> 1].e0;
	};

	swAttrib_t agg = {};
	long   aggRecv = 0;
	int    lightsProbed = 0;
	// per-caster tri-count histogram (from the ray-truth caster meshes): the static/dynamic size shape
	long   casSmall = 0, casMid = 0, casBig = 0;		// <=16 tris | 17-256 | >256
	double casTriTot = 0;
	long   casTot = 0;

	for( size_t li = 0; li < cap.lights.size(); li++ )
	{
		const softcapLight_t& L = cap.lights[li];
		if( L.penumbraSize <= 0.0f || L.edgeCount == 0 )
		{
			continue;
		}
		const size_t base4 = ( size_t )L.firstEdge * 2;			// float4 index of this light's tri stream
		const size_t nTris = ( ( size_t )L.edgeCount * 2 ) / 3;	// 3 float4 per triangle
		if( nTris == 0 )
		{
			continue;
		}

		// COMBINED walker buffer: [tri stream 3*nTris float4][one bounding-caster record 2 float4].
		// The capture stores only the tri stream (RenderCapture.cpp:1859), not the walker's caster
		// table, so wrap ALL tris in one caster - the coarse/tight per-triangle culls (the cost the
		// attribution is after) are independent of caster grouping; only the caster-cull count differs.
		std::vector<float4> buf;
		buf.reserve( nTris * 3 + 2 );
		float3 mn( 1e30f, 1e30f, 1e30f ), mx( -1e30f, -1e30f, -1e30f );
		for( size_t t = 0; t < nTris; t++ )
		{
			for( int k = 0; k < 3; k++ )
			{
				const float* v = F4( base4 + t * 3 + k );
				float4 e( v[0], v[1], v[2], v[3] );		// preserves r0.w (v0-radius) and r1.w (triRad)
				buf.push_back( e );
				mn = float3( std::fmin( mn.x, v[0] ), std::fmin( mn.y, v[1] ), std::fmin( mn.z, v[2] ) );
				mx = float3( std::fmax( mx.x, v[0] ), std::fmax( mx.y, v[1] ), std::fmax( mx.z, v[2] ) );
			}
		}
		float3 cen = ( mn + mx ) * 0.5f;
		float  rad = length( mx - mn ) * 0.5f + 1e-2f;
		buf.push_back( float4( cen.x, cen.y, cen.z, rad ) );					// caster c0: sphere
		buf.push_back( float4( 0.0f, ( float )nTris, 0.0f, 0.0f ) );			// caster c1: (firstTri, numTris)
		const int swTriBase = 0;
		const int swCasterBase = ( int )( nTris * 3 );
		SoftEdgeBuffer sbuf{ buf.data(), ( int )buf.size() };

		// receiver-centroid P set for this light (the fragment positions the walk runs on)
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
				float3 p0( cap.recvVerts[i0 * 3], cap.recvVerts[i0 * 3 + 1], cap.recvVerts[i0 * 3 + 2] );
				float3 p1( cap.recvVerts[i1 * 3], cap.recvVerts[i1 * 3 + 1], cap.recvVerts[i1 * 3 + 2] );
				float3 p2( cap.recvVerts[i2 * 3], cap.recvVerts[i2 * 3 + 1], cap.recvVerts[i2 * 3 + 2] );
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

		swAttrib_t before = g_swAttrib;
		long lRecv = 0;
		g_swAttribOn = true;
		for( size_t s = 0; s < recvPts.size(); s += stride )
		{
			const float3 P = recvPts[s];
			( void )SoftShadow_FaceCoverage( P, Lp, swR, swTriBase, swCasterBase, 1, SoftRotAngle( P ), sbuf );
			lRecv++;
		}
		g_swAttribOn = false;

		// per-caster tri histogram from the ray-truth caster meshes of this light
		for( uint32_t c = L.firstCaster; c < L.firstCaster + L.casterCount && c < cap.casters.size(); c++ )
		{
			const uint32_t tr = cap.casters[c].numIndex / 3;
			casTriTot += tr;
			casTot++;
			if( tr <= 16 )
			{
				casSmall++;
			}
			else if( tr <= 256 )
			{
				casMid++;
			}
			else
			{
				casBig++;
			}
		}

		if( lRecv > 0 )
		{
			const double inv = 1.0 / lRecv;
			std::printf( "    [walkattrib] L%zu: %zu tris, %ld recv | per-frag: coarse %.0f (cull %.0f%%) | tight %.0f (cull %.0f%%) | MT %.1f tris x16\n",
						 li, nTris, lRecv,
						 ( g_swAttrib.coarseTest - before.coarseTest ) * inv,
						 100.0 * ( g_swAttrib.coarseCull - before.coarseCull ) / std::fmax( 1.0, ( double )( g_swAttrib.coarseTest - before.coarseTest ) ),
						 ( g_swAttrib.tightTest - before.tightTest ) * inv,
						 100.0 * ( g_swAttrib.tightCull - before.tightCull ) / std::fmax( 1.0, ( double )( g_swAttrib.tightTest - before.tightTest ) ),
						 ( g_swAttrib.mtTri - before.mtTri ) * inv );
		}
		aggRecv += lRecv;
		lightsProbed++;
	}

	agg = g_swAttrib;
	if( lightsProbed == 0 || aggRecv == 0 )
	{
		std::printf( "    [walkattrib] no soft lights with receivers in this capture\n" );
		CHECK( true );
		return;
	}
	const double inv = 1.0 / ( double )aggRecv;
	const double mtTests = ( double )agg.mtTri * 16.0;			// sample-test count (survivors x 16 samples)
	std::printf( "    [walkattrib] ============ AGGREGATE over %d lights, %ld receiver-frags ============\n", lightsProbed, aggRecv );
	std::printf( "      caster tests/frag %.1f  (cull %.0f%%)\n",
				 agg.casterTest * inv, 100.0 * agg.casterCull / std::fmax( 1.0, ( double )agg.casterTest ) );
	std::printf( "      COARSE (v0) tests/frag %.1f  (cull %.0f%%)   -> reject volume before the 48B r1/r2 load\n",
				 agg.coarseTest * inv, 100.0 * agg.coarseCull / std::fmax( 1.0, ( double )agg.coarseTest ) );
	std::printf( "      TIGHT (cone) tests/frag %.1f  (cull %.0f%%)\n",
				 agg.tightTest * inv, 100.0 * agg.tightCull / std::fmax( 1.0, ( double )agg.tightTest ) );
	std::printf( "      MT SURVIVORS/frag %.2f  -> %.1f sample-tests/frag (x16)   <== the expensive work\n",
				 agg.mtTri * inv, mtTests * inv );
	std::printf( "      split of per-frag work: coarse-cull %.0f%% | tight-cull %.0f%% | MT-survive %.0f%% of triangles tested\n",
				 100.0 * agg.coarseCull / std::fmax( 1.0, ( double )agg.coarseTest ),
				 100.0 * agg.tightCull / std::fmax( 1.0, ( double )agg.tightTest ),
				 100.0 * agg.mtTri / std::fmax( 1.0, ( double )agg.coarseTest ) );
	std::printf( "      caster tri-size histogram: <=16 tris %ld | 17-256 %ld | >256 %ld  (mean %.0f tris/caster, %ld casters)\n",
				 casSmall, casMid, casBig, casTot ? casTriTot / casTot : 0.0, casTot );
	CHECK( true );
}
