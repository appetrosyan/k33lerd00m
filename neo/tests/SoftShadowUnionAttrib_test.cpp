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

// UNION-UNDERCOUNT ATTRIBUTION (study, not a test). The gate's LIT_IN_UMBRA class traces 100% to ONE
// light (origin ~(3635,7267,396), R=9, 1737 edge records) with the ana=0.9995 signature: the wedge's
// MAX-combine across casters reports ~0.05% disk coverage where the RT oracle says full umbra. This
// study proves/refutes the "many tiny solos, union = 1" hypothesis with numbers: for each defect
// fragment it runs the LIVE walker per-caster (solo occ), prints the solo distribution (max, top-10,
// counts, sum), the full-stream MAX-combine result, and the ray-cast ground truth. If maxSolo is tiny
// while sum >> 1 and truth == umbra, the source is the combine rule, not the per-caster math.
//
// Run:  CAP=~/.local/share/rbdoom3bfg/base/cap/cap0002.cap ./rbdoom3bfg_tests @study:SoftShadowUnionAttrib
//   optional: LIGHT=<index> (default: nearest to (3635.1,7267.4,396.5))
//             FRAGS="x,y,z;x,y,z;..." (default: the gate-dump defect fragments)

#include "hlsl_compat.h"
#include "softwedge_coverage.inc.hlsl"		// the live shader source, compiled as C++ (SW_FUNC=inline)
#include "SoftShadowBox.h"
#include "SoftShadowMesh.h"					// Cap + LoadCap + MeshTruthShadow
#include "idUnitTest.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <algorithm>

using namespace swtest;

STUDY_TEST( SoftShadowUnionAttrib, soloDistribution )
{
	const char* path = std::getenv( "CAP" );
	if( path == NULL )
	{
		std::printf( "    [unionattrib] CAP unset; skipping (set it to a .cap)\n" );
		CHECK( true );
		return;
	}
	Cap cap;
	if( !LoadCap( path, cap ) )
	{
		std::printf( "    [unionattrib] LoadCap failed: %s\n", path );
		CHECK( false );
		return;
	}

	// pick the light: LIGHT env index, else nearest to the gate-dump origin
	int li = -1;
	if( const char* lenv = std::getenv( "LIGHT" ) )
	{
		li = std::atoi( lenv );
	}
	else
	{
		const float3 target( 3635.1f, 7267.4f, 396.5f );
		float best = 1e30f;
		for( size_t i = 0; i < cap.lights.size(); i++ )
		{
			const float3 o( cap.lights[i].origin[0], cap.lights[i].origin[1], cap.lights[i].origin[2] );
			const float3 d = o - target;
			const float dd = dot( d, d );
			if( dd < best ) { best = dd; li = ( int )i; }
		}
	}
	if( li < 0 || li >= ( int )cap.lights.size() )
	{
		std::printf( "    [unionattrib] no light\n" );
		CHECK( false );
		return;
	}
	const capLight_t& L = cap.lights[li];
	const float3 Lo( L.origin[0], L.origin[1], L.origin[2] );
	const float  R = ( L.penumbraSize > 0.0f ) ? L.penumbraSize : 1.0f;
	std::printf( "    [unionattrib] light %d origin=(%.1f,%.1f,%.1f) R=%.1f edgeRecs=%u casters=%u\n",
				 li, Lo.x, Lo.y, Lo.z, R, L.edgeCount, L.casterCount );

	// The .cap EDGES block is the MT TRIANGLE stream (3 float4/tri, per WalkAttrib), NOT wedge records —
	// the gate's `ana` comes from GPU replay which rebuilds wedge records live. So the per-caster solo
	// here is computed in GROUND TRUTH terms: ray-cast each caster MESH alone against the disk. That
	// distinguishes the two candidate sources: max_k(solo) ~= 1 for some k => the per-caster wedge math
	// is broken on that caster; max_k(solo) small while the union is full => the MAX combine is the bug.
	const capCaster_t* firstC = &cap.casters[L.firstCaster];

	// defect fragments: FRAGS env or the gate-dump defaults (cap0001/cap0002, light L1)
	std::vector<float3> frags;
	if( const char* fenv = std::getenv( "FRAGS" ) )
	{
		float x, y, z;
		const char* s = fenv;
		while( std::sscanf( s, "%f,%f,%f", &x, &y, &z ) == 3 )
		{
			frags.push_back( float3( x, y, z ) );
			const char* semi = std::strchr( s, ';' );
			if( !semi ) { break; }
			s = semi + 1;
		}
	}
	else
	{
		frags.push_back( float3( 3762.7f, 7139.8f, 430.8f ) );	// cap0002 33k-px blob
		frags.push_back( float3( 3600.9f, 7165.8f, 445.8f ) );	// cap0001 3.7k-px blob (wall)
		frags.push_back( float3( 3593.5f, 7154.6f, 415.2f ) );	// cap0001 7.3k-px blob
		frags.push_back( float3( 3638.5f, 7180.0f, 384.0f ) );	// cap0001 floor z=384
		frags.push_back( float3( 3723.8f, 7154.8f, 384.0f ) );	// cap0001 floor 7.1k-px blob
	}
	// bulk mode: FRAGFILE = text file of "x y z" per line (all gate-dumped defect fragments). Runs the
	// A2 verdict only (no diagnostic bisects) and prints the population fix-rate.
	const bool bulk = ( std::getenv( "FRAGFILE" ) != NULL );
	if( bulk )
	{
		frags.clear();
		std::FILE* ff = std::fopen( std::getenv( "FRAGFILE" ), "r" );
		if( ff )
		{
			float x, y, z;
			while( std::fscanf( ff, "%f %f %f", &x, &y, &z ) == 3 ) { frags.push_back( float3( x, y, z ) ); }
			std::fclose( ff );
		}
		std::printf( "    [unionattrib] BULK: %d fragments\n", ( int )frags.size() );
	}
	if( bulk )
	{
		// SHADER-SEMANTICS validation: per caster build the STORED light-apex record stream (chained, the
		// engine emit shape) PLUS the record-aligned adjacent-normal stream (convention: nA fronts the
		// light; boundary edges emit their single face as nA with nB = -nA). Then drive the LIVE walker
		// with g_swRcvStyle 0..4 over all defect fragments: style 0 must reproduce the collapse, and the
		// per-style fix-rates choose the drop/flip semantics the GPU port ships.
		struct CasterStream { std::vector<float4> rec; std::vector<float4> nrm; };
		std::vector<CasterStream> streams( L.casterCount );
		for( uint32_t k = 0; k < L.casterCount; k++ )
		{
			const capCaster_t& c = firstC[k];
			std::vector<TriEdgeAdj> adj;
			BuildTriEdgeAdj( cap.meshVerts.data(), cap.meshIdx.data() + c.firstIndex, c.numIndex, adj );
			struct DEdge { uint32_t a, b; float3 pa, pb, nA, nB; };
			std::vector<DEdge> es;
			for( const TriEdgeAdj& e : adj )
			{
				DEdge dd;
				if( e.count < 2 )
				{
					const bool la = dot( e.nA, Lo - e.A ) > 0.0f;
					const float3 nf = la ? e.nA : float3( -e.nA.x, -e.nA.y, -e.nA.z );	// nA fronts the light
					if( la ) { dd.pa = e.A; dd.pb = e.B; dd.a = e.va; dd.b = e.vb; }
					else     { dd.pa = e.B; dd.pb = e.A; dd.a = e.vb; dd.b = e.va; }
					dd.nA = nf;
					dd.nB = float3( -nf.x, -nf.y, -nf.z );
				}
				else
				{
					const bool la = dot( e.nA, Lo - e.A ) > 0.0f, lb = dot( e.nB, Lo - e.A ) > 0.0f;
					if( la == lb ) { continue; }							// not a light-apex silhouette
					if( la ) { dd.pa = e.A; dd.pb = e.B; dd.a = e.va; dd.b = e.vb; dd.nA = e.nA; dd.nB = e.nB; }
					else     { dd.pa = e.B; dd.pb = e.A; dd.a = e.vb; dd.b = e.va; dd.nA = e.nB; dd.nB = e.nA; }
				}
				es.push_back( dd );
			}
			CasterStream& cs = streams[k];
			cs.rec.push_back( float4( 0, 0, 0, -1.0f ) );					// header pair (+dummy nrm pair)
			cs.rec.push_back( float4( 1e6f, 0, 0, 0 ) );
			cs.nrm.push_back( float4( 0, 0, 0, 0 ) );
			cs.nrm.push_back( float4( 0, 0, 0, 0 ) );
			std::vector<char> used( es.size(), 0 );
			for( int s = 0; s < ( int )es.size(); s++ )
			{
				int cur = s;
				while( cur >= 0 && !used[cur] )
				{
					used[cur] = 1;
					cs.rec.push_back( float4( es[cur].pa.x, es[cur].pa.y, es[cur].pa.z, 1.0f ) );
					cs.rec.push_back( float4( es[cur].pb.x, es[cur].pb.y, es[cur].pb.z, 0.0f ) );
					cs.nrm.push_back( float4( es[cur].nA.x, es[cur].nA.y, es[cur].nA.z, 0.0f ) );
					cs.nrm.push_back( float4( es[cur].nB.x, es[cur].nB.y, es[cur].nB.z, 0.0f ) );
					uint32_t endv = es[cur].b; int nxt = -1;
					for( int j = 0; j < ( int )es.size(); j++ ) { if( !used[j] && es[j].a == endv ) { nxt = j; break; } }
					cur = nxt;
				}
			}
		}
		int fixedBy[6] = { 0, 0, 0, 0, 0, 0 };
		int total = 0, notUmbra = 0;
		for( const float3& P : frags )
		{
			const float truthLit = MeshTruthShadow( cap, P, Lo, R, 32 );
			if( truthLit > 0.05f ) { notUmbra++; continue; }
			total++;
			for( int style = 0; style <= 5; style++ )
			{
				float maxOcc = 0.0f;
				for( uint32_t k = 0; k < L.casterCount && maxOcc < 0.95f; k++ )
				{
					const CasterStream& cs = streams[k];
					if( cs.rec.size() <= 2 ) { continue; }
					SoftEdgeBuffer abuf{ const_cast<float4*>( cs.rec.data() ), ( int )cs.rec.size() };
					g_swMinDnR = 0.04f;
					if( style == 5 )
					{
						// CONDITIONAL hybrid (the GPU-portable shape): normal walk first; the receiver-apex
						// reselect re-walk fires ONLY when the caster's light-apex solo collapsed. Healthy
						// contours keep the shipped result, so the reselect's damage to correct loops never
						// applies; collapsed casters get the chord+neg resolve that fixed c4L0 145/145.
						g_swEdgeNrm = NULL;
						g_swRcvStyle = 0;
						float exGap; int exNC, exClip;
						float s1 = saturate( SoftShadow_WedgeOcclusionEx( P, Lo, R, 0, ( int )( cs.rec.size() / 2 ), 0.0f,
																		  exGap, exNC, exClip, abuf ) );
						// trigger = collapsed solo AND a material closure bridge fired (broken-chain evidence,
						// swClipOut bit 2). Lit fragments walk clean loops -> no bridge -> no second pass.
						if( s1 < 0.02f && ( exClip & 4 ) != 0 )
						{
							g_swEdgeNrm = cs.nrm.data();
							g_swRcvStyle = 3;
							s1 = std::max( s1, saturate( SoftShadow_WedgeOcclusion( P, Lo, R, 0, ( int )( cs.rec.size() / 2 ), 0.0f, abuf ) ) );
						}
						maxOcc = std::max( maxOcc, s1 );
					}
					else
					{
						g_swEdgeNrm = cs.nrm.data();
						g_swRcvStyle = style;
						maxOcc = std::max( maxOcc, saturate( SoftShadow_WedgeOcclusion( P, Lo, R, 0, ( int )( cs.rec.size() / 2 ), 0.0f, abuf ) ) );
					}
				}
				if( maxOcc > 0.95f ) { fixedBy[style]++; }
				else if( style == 5 )
				{
					std::printf( "    [rcv residual s5] P=(%.1f,%.1f,%.1f) maxOcc=%.6f\n", P.x, P.y, P.z, maxOcc );
				}
			}
		}
		g_swEdgeNrm = NULL;
		g_swRcvStyle = 0;
		std::printf( "    [rcv BULK] umbraFrags=%d notUmbraByRT=%d | fixed: off=%d chord+swap=%d split+swap=%d chord+neg=%d split+neg=%d COND(chord+neg)=%d\n",
					 total, notUmbra, fixedBy[0], fixedBy[1], fixedBy[2], fixedBy[3], fixedBy[4], fixedBy[5] );
		CHECK( true );
		return;
	}

	for( const float3& P : frags )
	{
		// per-caster ground-truth solo occlusion (each caster mesh ALONE vs the disk)
		std::vector<float> solos( L.casterCount );
		double sum = 0.0;
		int nPos = 0, nBig = 0;
		for( uint32_t k = 0; k < L.casterCount; k++ )
		{
			const capCaster_t& c = firstC[k];
			const float lit = MeshTruthShadowSoup( cap.meshVerts.data(), cap.meshIdx.data() + c.firstIndex,
												   c.numIndex, P, Lo, R, 64 );
			solos[k] = 1.0f - lit;
			sum += solos[k];
			if( solos[k] > 1e-4f ) { nPos++; }
			if( solos[k] > 0.01f ) { nBig++; }
		}
		std::vector<int> order( solos.size() );
		for( size_t k = 0; k < order.size(); k++ ) { order[k] = ( int )k; }
		std::sort( order.begin(), order.end(), [&]( int a, int b ) { return solos[a] > solos[b]; } );

		const float truthLit = MeshTruthShadow( cap, P, Lo, R, 64 );	// ALL casters: 1=lit, 0=umbra

		std::printf( "    [unionattrib] P=(%.1f,%.1f,%.1f) dist=%.1f | unionLit=%.4f | solo: max=%.4f sum=%.3f n>1e-4=%d n>0.01=%d of %u\n",
					 P.x, P.y, P.z, std::sqrt( dot( Lo - P, Lo - P ) ), truthLit,
					 order.empty() ? 0.0f : solos[order[0]], sum, nPos, nBig, L.casterCount );
		for( int t = 0; t < 10 && t < ( int )order.size(); t++ )
		{
			const int k = order[t];
			if( solos[k] <= 0.0f ) { break; }
			const capCaster_t& c = firstC[k];
			// caster bbox for identification
			float3 bmin( 1e30f, 1e30f, 1e30f ), bmax( -1e30f, -1e30f, -1e30f );
			for( uint32_t vi = 0; vi < c.numVerts; vi++ )
			{
				const float* v = &cap.meshVerts[( ( size_t )c.firstVert + vi ) * 3];
				bmin.x = std::min( bmin.x, v[0] ); bmax.x = std::max( bmax.x, v[0] );
				bmin.y = std::min( bmin.y, v[1] ); bmax.y = std::max( bmax.y, v[1] );
				bmin.z = std::min( bmin.z, v[2] ); bmax.z = std::max( bmax.z, v[2] );
			}
			std::printf( "      top%02d solo=%.4f casterId=%.0f tris=%u verts=%u bbox=(%.0f,%.0f,%.0f)-(%.0f,%.0f,%.0f)\n",
						 t, solos[k], c.casterId, c.numIndex / 3, c.numVerts,
						 bmin.x, bmin.y, bmin.z, bmax.x, bmax.y, bmax.z );
		}

		// ---- ANALYTIC wedge solo on the TOP caster: reproduce the GPU collapse + bisect the mechanism.
		// Build the LIGHT-apex silhouette records (the contour the engine ships) from the caster mesh and
		// run the LIVE walker. Then step the fragment toward the light: if occ recovers as the fragment
		// leaves the caster surface, the collapse is the contact/near-clip loop cut; if it stays ~0 at all
		// heights, the loop itself is broken (open-mesh boundary decomposition).
		// A2 over EVERY blocking caster (RT solo > 0.9): max-combine only needs ONE of them to reach ~1
		// per fragment, so the per-defect verdict is the max of the A2 solos.
		{
			float a2max = 0.0f;
			for( size_t k = 0; k < solos.size(); k++ )
			{
				if( solos[k] < 0.9f ) { continue; }
				const capCaster_t& c = firstC[k];
				std::vector<TriEdgeAdj> adj;
				BuildTriEdgeAdj( cap.meshVerts.data(), cap.meshIdx.data() + c.firstIndex, c.numIndex, adj );
				struct DEdge { uint32_t a, b; float3 pa, pb; };
				std::vector<DEdge> es;
				for( const TriEdgeAdj& e : adj )
				{
					bool inLightSet;
					if( e.count < 2 ) { inLightSet = true; }
					else
					{
						const bool la = dot( e.nA, Lo - e.A ) > 0.0f, lb = dot( e.nB, Lo - e.A ) > 0.0f;
						inLightSet = ( la != lb );
					}
					if( !inLightSet ) { continue; }
					DEdge dd;
					if( e.count < 2 )
					{
						const bool fa = dot( e.nA, P - e.A ) > 0.0f;
						if( fa ) { dd.pa = e.A; dd.pb = e.B; dd.a = e.va; dd.b = e.vb; }
						else     { dd.pa = e.B; dd.pb = e.A; dd.a = e.vb; dd.b = e.va; }
					}
					else
					{
						const bool fa = dot( e.nA, P - e.A ) > 0.0f, fb = dot( e.nB, P - e.A ) > 0.0f;
						if( fa == fb ) { continue; }
						if( !fb ) { dd.pa = e.A; dd.pb = e.B; dd.a = e.va; dd.b = e.vb; }
						else      { dd.pa = e.B; dd.pb = e.A; dd.a = e.vb; dd.b = e.va; }
					}
					es.push_back( dd );
				}
				std::vector<float4> arec;
				arec.push_back( float4( 0, 0, 0, -1.0f ) );
				arec.push_back( float4( 1e6f, 0, 0, 0 ) );
				std::vector<char> used( es.size(), 0 );
				for( int s = 0; s < ( int )es.size(); s++ )
				{
					int cur = s;
					while( cur >= 0 && !used[cur] )
					{
						used[cur] = 1;
						arec.push_back( float4( es[cur].pa.x, es[cur].pa.y, es[cur].pa.z, 1.0f ) );
						arec.push_back( float4( es[cur].pb.x, es[cur].pb.y, es[cur].pb.z, 0.0f ) );
						uint32_t endv = es[cur].b; int nxt = -1;
						for( int j = 0; j < ( int )es.size(); j++ ) { if( !used[j] && es[j].a == endv ) { nxt = j; break; } }
						cur = nxt;
					}
				}
				SoftEdgeBuffer abuf{ arec.data(), ( int )arec.size() };
				g_swMinDnR = 0.04f;
				const float aocc = saturate( SoftShadow_WedgeOcclusion( P, Lo, R, 0, ( int )( arec.size() / 2 ), 0.0f, abuf ) );
				g_swMinDnR = 0.0f;
				const float aocc0 = saturate( SoftShadow_WedgeOcclusion( P, Lo, R, 0, ( int )( arec.size() / 2 ), 0.0f, abuf ) );
				a2max = std::max( a2max, aocc );
				std::printf( "      [A2 caster %02d] rtSolo=%.4f a2Occ=%.6f a2Occ(minDnR=0)=%.6f (edges=%d, tris=%u)\n",
							 ( int )k, solos[k], aocc, aocc0, ( int )es.size(), c.numIndex / 3 );
			}
			std::printf( "      [A2 VERDICT] maxCombine over A2 solos = %.6f (defect fixed: %s)\n",
						 a2max, a2max > 0.95f ? "YES" : "NO" );
		}

		if( !order.empty() && solos[order[0]] > 0.9f )
		{
			const capCaster_t& c = firstC[order[0]];
			std::vector<TriEdgeAdj> adj;
			BuildTriEdgeAdj( cap.meshVerts.data(), cap.meshIdx.data() + c.firstIndex, c.numIndex, adj );
			int boundary = 0;
			for( const TriEdgeAdj& e : adj ) { if( e.count < 2 ) { boundary++; } }
			std::vector<float4> recs;
			AppendReceiverSilhouetteRecords( adj, Lo, recs );	// apex = LIGHT: the engine's contour
			SoftEdgeBuffer wbuf{ recs.data(), ( int )recs.size() };
			const float3 toL = normalize( Lo - P );
			std::printf( "      [wedge-solo top0] silRecs=%d meshEdges=%d boundaryEdges=%d\n",
						 ( int )( recs.size() / 2 ) - 1, ( int )adj.size(), boundary );
			const float step[6] = { 0.0f, 0.25f, 1.0f, 4.0f, 16.0f, 64.0f };
			for( int si = 0; si < 6; si++ )
			{
				const float3 Q = P + toL * step[si];
				g_swMinDnR = 0.04f;
				const float occ = saturate( SoftShadow_WedgeOcclusion( Q, Lo, R, 0, ( int )( recs.size() / 2 ), 0.0f, wbuf ) );
				const float lit = MeshTruthShadowSoup( cap.meshVerts.data(), cap.meshIdx.data() + c.firstIndex,
													   c.numIndex, Q, Lo, R, 64 );
				std::printf( "      [wedge-solo top0] +%.2f toward L: wedgeOcc=%.6f rtSoloOcc=%.4f\n",
							 step[si], occ, 1.0f - lit );
			}

			// cut-caster predicate check (mechanism-B feasibility): does the shipped whitelist census
			// (WedgeOcclusionEx swClipOut) flag this caster as "cut" at this fragment? bits 0-2 = per-caster
			// cut condition flags, bits 3+ = cut-caster count.
			{
				float gap; int ncontrib, clip;
				g_swMinDnR = 0.04f;
				const float exOcc = saturate( SoftShadow_WedgeOcclusionEx( P, Lo, R, 0, ( int )( recs.size() / 2 ), 0.0f,
																		   gap, ncontrib, clip, wbuf ) );
				std::printf( "      [wedge-solo top0] Ex: occ=%.6f gap=%.4f nContrib=%d clipBits=0x%x nCut=%d\n",
							 exOcc, gap, ncontrib, clip & 7, clip >> 3 );
			}

			// receiver-apex contour on the SAME mesh: if this recovers occ~1 where the light-apex contour
			// collapses, the source is the WRONG CONTOUR (light-apex silhouette), not the area math.
			{
				std::vector<float4> rrec;
				AppendReceiverSilhouetteRecords( adj, P, rrec );
				SoftEdgeBuffer rbuf{ rrec.data(), ( int )rrec.size() };
				g_swMinDnR = 0.04f;
				const float rocc = saturate( SoftShadow_WedgeOcclusion( P, Lo, R, 0, ( int )( rrec.size() / 2 ), 0.0f, rbuf ) );
				std::printf( "      [wedge-solo top0] RECEIVER-apex contour: wedgeOcc=%.6f (silRecs=%d)\n",
							 rocc, ( int )( rrec.size() / 2 ) - 1 );
			}

			// MECHANISM-A2 validation: start from the LIGHT-apex candidate set (what the engine emits),
			// then per-fragment (a) DROP edges that are not a silhouette from P, (b) RE-ORIENT survivors
			// by facing-from-P, (c) chain and walk. If this recovers occ~1, the shader fix is a per-edge
			// 2-dot test + conditional endpoint swap on the existing record stream + stored normals.
			// Also the intermediate "reorient only, no drop" variant to see which half does the work.
			for( int variant = 0; variant < 2; variant++ )
			{
				struct DEdge { uint32_t a, b; float3 pa, pb; };
				std::vector<DEdge> es;
				for( const TriEdgeAdj& e : adj )
				{
					// LIGHT-apex membership (what flatten ships)
					bool inLightSet;
					if( e.count < 2 ) { inLightSet = true; }
					else
					{
						const bool la = dot( e.nA, Lo - e.A ) > 0.0f, lb = dot( e.nB, Lo - e.A ) > 0.0f;
						inLightSet = ( la != lb );
					}
					if( !inLightSet ) { continue; }
					// receiver-apex test + orientation
					DEdge dd;
					if( e.count < 2 )
					{
						const bool fa = dot( e.nA, P - e.A ) > 0.0f;
						if( fa ) { dd.a = e.va; dd.b = e.vb; dd.pa = e.A; dd.pb = e.B; }
						else     { dd.a = e.vb; dd.b = e.va; dd.pa = e.B; dd.pb = e.A; }
					}
					else
					{
						const bool fa = dot( e.nA, P - e.A ) > 0.0f, fb = dot( e.nB, P - e.A ) > 0.0f;
						if( fa == fb )
						{
							if( variant == 0 ) { continue; }			// variant 0: drop non-receiver-sil edges
							// variant 1: keep, oriented by light-apex rule
							const bool la = dot( e.nA, Lo - e.A ) > 0.0f;
							if( la ) { dd.a = e.va; dd.b = e.vb; dd.pa = e.A; dd.pb = e.B; }
							else     { dd.a = e.vb; dd.b = e.va; dd.pa = e.B; dd.pb = e.A; }
						}
						else
						{
							if( !fb ) { dd.a = e.va; dd.b = e.vb; dd.pa = e.A; dd.pb = e.B; }
							else      { dd.a = e.vb; dd.b = e.va; dd.pa = e.B; dd.pb = e.A; }
						}
					}
					es.push_back( dd );
				}
				// chain into loops (builder's tie-break) and emit one record stream
				std::vector<float4> arec;
				arec.push_back( float4( 0, 0, 0, -1.0f ) );
				arec.push_back( float4( 1e6f, 0, 0, 0 ) );
				std::vector<char> used( es.size(), 0 );
				for( int s = 0; s < ( int )es.size(); s++ )
				{
					int cur = s;
					while( cur >= 0 && !used[cur] )
					{
						used[cur] = 1;
						arec.push_back( float4( es[cur].pa.x, es[cur].pa.y, es[cur].pa.z, 1.0f ) );
						arec.push_back( float4( es[cur].pb.x, es[cur].pb.y, es[cur].pb.z, 0.0f ) );
						uint32_t endv = es[cur].b; int nxt = -1;
						for( int j = 0; j < ( int )es.size(); j++ ) { if( !used[j] && es[j].a == endv ) { nxt = j; break; } }
						cur = nxt;
					}
				}
				SoftEdgeBuffer abuf{ arec.data(), ( int )arec.size() };
				g_swMinDnR = 0.04f;
				const float aocc = saturate( SoftShadow_WedgeOcclusion( P, Lo, R, 0, ( int )( arec.size() / 2 ), 0.0f, abuf ) );
				std::printf( "      [wedge-solo top0] A2 %s: wedgeOcc=%.6f (edges=%d of %d light-set)\n",
							 variant == 0 ? "drop+reorient" : "reorient-only", aocc, ( int )es.size(),
							 ( int )( recs.size() / 2 ) - 1 );
			}

			// per-LOOP breakdown at the raw fragment: chain the silhouette edges exactly like the builder,
			// but emit each loop as its OWN record stream and walk it separately. If two big loops carry
			// ~equal |area| with opposite winding, the net cancellation is the collapse mechanism.
			{
				struct DEdge { uint32_t a, b; float3 pa, pb; };
				std::vector<DEdge> es;
				for( const TriEdgeAdj& e : adj )
				{
					DEdge dd;
					if( e.count < 2 )
					{
						bool fa = dot( e.nA, Lo - e.A ) > 0.0f;
						if( fa ) { dd.a = e.va; dd.b = e.vb; dd.pa = e.A; dd.pb = e.B; }
						else     { dd.a = e.vb; dd.b = e.va; dd.pa = e.B; dd.pb = e.A; }
					}
					else
					{
						bool fa = dot( e.nA, Lo - e.A ) > 0.0f, fb = dot( e.nB, Lo - e.A ) > 0.0f;
						if( fa == fb ) { continue; }
						if( !fb ) { dd.a = e.va; dd.b = e.vb; dd.pa = e.A; dd.pb = e.B; }
						else      { dd.a = e.vb; dd.b = e.va; dd.pa = e.B; dd.pb = e.A; }
					}
					es.push_back( dd );
				}
				std::vector<char> used( es.size(), 0 );
				int loopIdx = 0;
				for( int s = 0; s < ( int )es.size(); s++ )
				{
					if( used[s] ) { continue; }
					std::vector<float4> lrec;
					lrec.push_back( float4( 0, 0, 0, -1.0f ) );		// header (bounds don't matter: walker culls via header only when present)
					lrec.push_back( float4( 1e6f, 0, 0, 0 ) );		// huge sphere: never culled
					int cur = s, n = 0;
					bool closed = false;
					uint32_t startv = es[s].a;
					while( cur >= 0 && !used[cur] )
					{
						used[cur] = 1;
						lrec.push_back( float4( es[cur].pa.x, es[cur].pa.y, es[cur].pa.z, 1.0f ) );
						lrec.push_back( float4( es[cur].pb.x, es[cur].pb.y, es[cur].pb.z, 0.0f ) );
						n++;
						uint32_t endv = es[cur].b;
						closed = ( endv == startv );
						int nxt = -1;
						for( int j = 0; j < ( int )es.size(); j++ ) { if( !used[j] && es[j].a == endv ) { nxt = j; break; } }
						cur = nxt;
					}
					SoftEdgeBuffer lbuf{ lrec.data(), ( int )lrec.size() };
					g_swMinDnR = 0.04f;
					const float locc = saturate( SoftShadow_WedgeOcclusion( P, Lo, R, 0, ( int )( lrec.size() / 2 ), 0.0f, lbuf ) );
					if( locc > 0.01f || n > 8 )
					{
						std::printf( "      [loop %02d] edges=%d closed=%d |area|/disk=%.4f\n", loopIdx, n, closed ? 1 : 0, locc );
					}
					loopIdx++;
				}
			}
		}
	}
	CHECK( true );
}
