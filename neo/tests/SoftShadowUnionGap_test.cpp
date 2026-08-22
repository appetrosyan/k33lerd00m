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

// GENERAL-CASE structure study (data-driven, 2026-08-22). The general soft-shadow coverage is
// occlusion(P) = Area( lightDisk INTERSECT UNION_t project(tri_t) ) / piR^2 - a UNION, so overlapping
// occluders do NOT add. The banding-free, open-geometry-robust candidate is the per-triangle analytic
// sum Sigma_solo = sum_t SurfBuild_SoloCovExact (continuous, no 1/N quantum, no closed-silhouette needed),
// whose ONLY error is overlap double-counting (Sigma_solo >= union). This measures that gap on REAL
// capture geometry, binned across the penumbra gradient, to decide the whole approach:
//   - if saturate(Sigma_solo) ~= union across the penumbra  -> per-triangle sum IS a banding-free general
//     coverage (overlaps sparse); only umbra/apex texels need a correction.
//   - if Sigma_solo >> union everywhere -> irreducibly a union; a different decomposition is needed.
// Union truth = MeshTruthShadowSoup (ray oracle). Env: SOFTCAP (required), UG_N (disk side, def 32),
// UG_SAMP (recv samples/caster, def 40), UG_CASTERS (max casters, def 120), UG_MAXTRI (skip huge, def 1500).

#include "hlsl_compat.h"
#include "softwedge_coverage.inc.hlsl"		// SoftShadow_Frame, softFrame_t, SoftDisk_CircleTriArea
#include "softsurf_classify.inc.hlsl"		// SurfBuild_SoloCovExact (per-triangle analytic occlusion)
#include "SoftShadowMesh.h"					// SoftCap, LoadSoftCap, MeshTruthShadowSoup (union ray oracle)
#include "idUnitTest.h"

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <unordered_map>
#include <map>
#include <cstdint>
#include <utility>

using namespace swtest;

// mesh -> per-edge (two adjacent face normals + boundary flag), WELDED by position so T-junction
// duplicate verts do not masquerade as boundaries. Mirrors BuildCasterEdges in SoftShadowPrimitives_test.
struct RAEdge2 { float3 a, b, nA, nB; bool boundary; };
static std::vector<RAEdge2> BuildWeldedCasterEdges( const SoftCap& cap, const softcapCaster_t& cs, float weld )
{
	std::unordered_map<uint64_t, int> wmap;
	auto weld1 = [&]( uint32_t gi ) -> int
	{
		const float* p = &cap.meshVerts[gi * 3];
		int64_t x = ( int64_t )std::llround( p[0] / weld ), y = ( int64_t )std::llround( p[1] / weld ), z = ( int64_t )std::llround( p[2] / weld );
		uint64_t k = ( uint64_t )( ( x * 73856093LL ) ^ ( y * 19349663LL ) ^ ( z * 83492791LL ) );
		auto it = wmap.find( k );
		if( it != wmap.end() ) { return it->second; }
		int id = ( int )wmap.size(); wmap[k] = id; return id;
	};
	const int nT = ( int )( cs.numIndex / 3 );
	std::map<std::pair<int, int>, int> emap;
	std::vector<RAEdge2> out;
	for( int t = 0; t < nT; t++ )
	{
		int wi[3]; float3 v[3];
		for( int j = 0; j < 3; j++ ) { uint32_t gi = cap.meshIdx[cs.firstIndex + t * 3 + j]; wi[j] = weld1( gi ); v[j] = float3( cap.meshVerts[gi * 3], cap.meshVerts[gi * 3 + 1], cap.meshVerts[gi * 3 + 2] ); }
		float3 nf = cross( v[1] - v[0], v[2] - v[0] );
		for( int e = 0; e < 3; e++ )
		{
			int u = wi[e], w = wi[( e + 1 ) % 3];
			std::pair<int, int> key( std::min( u, w ), std::max( u, w ) );
			auto it = emap.find( key );
			if( it == emap.end() ) { RAEdge2 re; re.a = v[e]; re.b = v[( e + 1 ) % 3]; re.nA = nf; re.nB = float3( 0, 0, 0 ); re.boundary = true; emap[key] = ( int )out.size(); out.push_back( re ); }
			else { out[it->second].nB = nf; out[it->second].boundary = false; }
		}
	}
	return out;
}
static std::vector<RAEdge2> BuildEdgesRaw( const float* verts, const uint32_t* idx, uint32_t numIdx, float weld )
{
	std::unordered_map<uint64_t, int> wmap;
	auto weld1 = [&]( uint32_t gi ) -> int
	{
		const float* p = &verts[gi * 3];
		int64_t x = ( int64_t )std::llround( p[0] / weld ), y = ( int64_t )std::llround( p[1] / weld ), z = ( int64_t )std::llround( p[2] / weld );
		uint64_t k = ( uint64_t )( ( x * 73856093LL ) ^ ( y * 19349663LL ) ^ ( z * 83492791LL ) );
		auto it = wmap.find( k ); if( it != wmap.end() ) { return it->second; }
		int id = ( int )wmap.size(); wmap[k] = id; return id;
	};
	std::map<std::pair<int, int>, int> emap; std::vector<RAEdge2> out;
	for( uint32_t k = 0; k + 2 < numIdx; k += 3 )
	{
		int wi[3]; float3 v[3];
		for( int j = 0; j < 3; j++ ) { uint32_t gi = idx[k + j]; wi[j] = weld1( gi ); v[j] = float3( verts[gi * 3], verts[gi * 3 + 1], verts[gi * 3 + 2] ); }
		float3 nf = cross( v[1] - v[0], v[2] - v[0] );
		for( int e = 0; e < 3; e++ )
		{
			int u = wi[e], w = wi[( e + 1 ) % 3];
			std::pair<int, int> key( std::min( u, w ), std::max( u, w ) );
			auto it = emap.find( key );
			if( it == emap.end() ) { RAEdge2 re; re.a = v[e]; re.b = v[( e + 1 ) % 3]; re.nA = nf; re.nB = float3( 0, 0, 0 ); re.boundary = true; emap[key] = ( int )out.size(); out.push_back( re ); }
			else { out[it->second].nB = nf; out[it->second].boundary = false; }
		}
	}
	return out;
}

// ANALYTIC silhouette coverage: sum SoftDisk_CircleTriArea over the silhouette edges (boundary OR
// facing-flip, receiver-apex orientation - the F6b-validated ProcCaster method). Connector-free (accepts
// small near-plane-clip error). This is the banding-free, winding-consistent candidate for the union.
static float SilhouetteCov( const std::vector<RAEdge2>& edges, float3 P, float3 L, float swR )
{
	softFrame_t f = SoftShadow_Frame( P, L );
	const float swR2 = swR * swR;
	float area = 0.0f;
	for( const RAEdge2& e : edges )
	{
		const bool va = dot( e.nA, P - e.a ) > 0.0f;
		bool sil, flip;
		if( e.boundary ) { sil = true; flip = !va; }
		else { const bool vb = dot( e.nB, P - e.a ) > 0.0f; sil = ( va != vb ); flip = !va; }
		if( !sil ) { continue; }
		const float3 A = flip ? e.b : e.a, B = flip ? e.a : e.b;
		const float3 a = A - P, b = B - P;
		const float dnA = dot( a, f.nrm ), dnB = dot( b, f.nrm ), d = dnB - dnA;
		softClip_t cl = SoftShadow_ClipSlab( dnA, dnB, SW_NEAR_EPS, f.distPL );
		if( cl.empty ) { continue; }
		float2 q0 = SoftShadow_ProjectVert( a + cl.t0 * ( b - a ), dnA + cl.t0 * d, f );
		float2 q1 = SoftShadow_ProjectVert( a + cl.t1 * ( b - a ), dnA + cl.t1 * d, f );
		area += SoftDisk_CircleTriArea( q0, q1, swR2 );
	}
	float c = std::fabs( area ) / ( PI * swR2 );
	return c > 1.0f ? 1.0f : c;
}

STUDY_TEST( SoftShadowUnionGap, quantify )
{
	const char* path = std::getenv( "SOFTCAP" );
	if( path == NULL ) { std::printf( "    [uniongap] SOFTCAP unset; skipping\n" ); CHECK( true ); return; }
	SoftCap cap;
	if( !LoadSoftCap( path, cap ) ) { std::printf( "    [uniongap] cannot load %s\n", path ); CHECK( false ); return; }
	auto envI = []( const char* k, int d ) { const char* s = std::getenv( k ); return s ? std::atoi( s ) : d; };
	const int N = envI( "UG_N", 32 ), SAMP = envI( "UG_SAMP", 40 ), MAXC = envI( "UG_CASTERS", 120 ), MAXTRI = envI( "UG_MAXTRI", 1500 );

	// receiver centroids per light (copied from the proxyfit harness)
	std::vector<std::vector<float3>> lightRecv( cap.lights.size() );
	for( const softcapReceiver_t& r : cap.receivers )
	{
		if( r.lightIndex >= cap.lights.size() ) { continue; }
		for( uint32_t t = 0; t < r.numIndex / 3; t++ )
		{
			const uint32_t i0 = cap.recvIdx[r.firstIndex + t * 3 + 0], i1 = cap.recvIdx[r.firstIndex + t * 3 + 1], i2 = cap.recvIdx[r.firstIndex + t * 3 + 2];
			float3 p0( cap.recvVerts[i0 * 3], cap.recvVerts[i0 * 3 + 1], cap.recvVerts[i0 * 3 + 2] );
			float3 p1( cap.recvVerts[i1 * 3], cap.recvVerts[i1 * 3 + 1], cap.recvVerts[i1 * 3 + 2] );
			float3 p2( cap.recvVerts[i2 * 3], cap.recvVerts[i2 * 3 + 1], cap.recvVerts[i2 * 3 + 2] );
			lightRecv[r.lightIndex].push_back( ( p0 + p1 + p2 ) * ( 1.0f / 3.0f ) );
		}
	}

	const int NB = 10;						// bins by UNION occlusion [0,1)
	double bAbs[NB] = {}, bSgn[NB] = {}, bSum[NB] = {}, bUni[NB] = {}, bRatio[NB] = {}, bRatioAll[NB] = {}; long bN[NB] = {};
	long nSamp = 0, nNear = 0, nPen = 0, nPenNear = 0;		// nNear = |clampedSum-union|<=0.06 ; nPen = penumbra samples
	int usedC = 0;

	for( uint32_t c = 0; c < cap.casters.size() && usedC < MAXC; c++ )
	{
		const softcapCaster_t& cs = cap.casters[c];
		const int nT = ( int )( cs.numIndex / 3 );
		if( nT < 1 || nT > MAXTRI || cs.lightIndex >= cap.lights.size() ) { continue; }
		const std::vector<float3>& recv = lightRecv[cs.lightIndex];
		if( recv.empty() ) { continue; }
		const float3 L( cap.lights[cs.lightIndex].origin[0], cap.lights[cs.lightIndex].origin[1], cap.lights[cs.lightIndex].origin[2] );
		const float  swR = std::fmax( cap.lights[cs.lightIndex].penumbraSize, 1e-2f );
		const float  swR2 = swR * swR;
		const float* rv = cap.meshVerts.data();
		const uint32_t* ri = &cap.meshIdx[cs.firstIndex];
		const size_t stride = std::max( ( size_t )1, recv.size() / ( size_t )SAMP );
		bool anyPen = false;
		for( size_t s = 0; s < recv.size(); s += stride )
		{
			const float3 P = recv[s];
			const float lit = MeshTruthShadowSoup( rv, ri, cs.numIndex, P, L, swR, N );	// 1 = lit
			const float uni = 1.0f - lit;												// UNION occlusion
			if( uni < 0.002f ) { continue; }											// this caster does not shadow P
			anyPen = true;
			// per-triangle analytic occlusion, summed (the continuous, robust candidate)
			const softFrame_t f = SoftShadow_Frame( P, L );
			float sum = 0.0f, sumFF = 0.0f;		// all triangles vs FRONT-FACING (normal toward light) only
			for( int t = 0; t < nT; t++ )
			{
				const uint32_t a = cap.meshIdx[cs.firstIndex + t * 3 + 0], b = cap.meshIdx[cs.firstIndex + t * 3 + 1], d = cap.meshIdx[cs.firstIndex + t * 3 + 2];
				float3 v0( rv[a * 3], rv[a * 3 + 1], rv[a * 3 + 2] );
				float3 v1( rv[b * 3], rv[b * 3 + 1], rv[b * 3 + 2] );
				float3 v2( rv[d * 3], rv[d * 3 + 1], rv[d * 3 + 2] );
				const float solo = SurfBuild_SoloCovExact( P, v0, v1, v2, f, swR2 );
				sum += solo;
				const float3 gn = cross( v1 - v0, v2 - v0 );			// geometric normal
				const float3 tc = ( v0 + v1 + v2 ) * ( 1.0f / 3.0f );
				if( dot( gn, L - tc ) > 0.0f ) { sumFF += solo; }		// keep faces turned toward the light
			}
			const float clamped = sum < 0.0f ? 0.0f : ( sum > 1.0f ? 1.0f : sum );
			const float clampedFF = sumFF < 0.0f ? 0.0f : ( sumFF > 1.0f ? 1.0f : sumFF );
			const float gap = clampedFF - uni;		// PRIMARY metric now: front-face sum vs union
			int bi = ( int )( uni * NB ); if( bi < 0 ) { bi = 0; } if( bi >= NB ) { bi = NB - 1; }
			bAbs[bi] += std::fabs( gap ); bSgn[bi] += gap; bSum[bi] += clampedFF; bUni[bi] += uni;
			bRatio[bi] += ( uni > 0.02f ? sumFF / uni : 1.0f ); bRatioAll[bi] += ( uni > 0.02f ? sum / uni : 1.0f ); bN[bi]++;
			nSamp++;
			if( std::fabs( gap ) <= 0.06f ) { nNear++; }
			if( uni < 0.998f ) { nPen++; if( std::fabs( gap ) <= 0.06f ) { nPenNear++; } }
		}
		if( anyPen ) { usedC++; }
	}

	if( nSamp == 0 ) { std::printf( "    [uniongap] no shadowing samples\n" ); CHECK( true ); return; }
	std::printf( "    [uniongap] %s | casters %d, samples %ld, N=%d disk\n", path, usedC, nSamp, N );
	std::printf( "    [uniongap] saturate(FRONT-FACE Sigma_solo) vs UNION:  |gap|<=0.06 on %.1f%% of ALL samples, %.1f%% of PENUMBRA samples (nPen %ld)\n",
				 100.0 * nNear / nSamp, nPen ? 100.0 * nPenNear / nPen : 0.0, nPen );
	std::printf( "    [uniongap] union-occlusion bin -> n | union | sat(FF-Ssolo) | |gap| | signed | FF overcount | ALL-tri overcount\n" );
	for( int b = 0; b < NB; b++ )
	{
		if( bN[b] == 0 ) { continue; }
		std::printf( "      occ [%.1f,%.1f)  n%6ld  union %.3f  FF %.3f  |gap| %.4f  signed %+.4f  FFx %.2f  ALLx %.2f\n",
					 b * 0.1, b * 0.1 + 0.1, bN[b], bUni[b] / bN[b], bSum[b] / bN[b], bAbs[b] / bN[b], bSgn[b] / bN[b], bRatio[b] / bN[b], bRatioAll[b] / bN[b] );
	}
	CHECK( true );
}

// WINDING / TOPOLOGY of the captured caster meshes - is the winding actually INCONSISTENT, and is it
// FIXABLE? Method: weld coincident verts by position (Doom3 duplicates verts at brush/model junctions,
// so raw indices lie about sharing), then over the welded topology per caster:
//  - classify each edge by share count: 1 = BOUNDARY (open), 2 = MANIFOLD, >2 = NON-MANIFOLD (unfixable).
//  - a manifold edge is CONSISTENT iff its two triangles traverse it in OPPOSITE directions.
//  - ORIENTABILITY (fixability by a global per-triangle flip): union-find with parity over triangles
//    linked by manifold edges; a contradiction (odd cycle) = NON-orientable = winding cannot be made
//    consistent by flipping. Non-manifold edges also make it unfixable (no consistent silhouette exists).
// Verdict per caster: CLEAN (closed, manifold, orientable) -> the analytic silhouette WOULD work here,
// so "inconsistent winding" is fixable and prior closing arguments can be relitigated. Otherwise report
// which failure dominates (open boundary vs non-manifold vs non-orientable).
STUDY_TEST( SoftShadowWinding, quantify )
{
	const char* path = std::getenv( "SOFTCAP" );
	if( path == NULL ) { std::printf( "    [winding] SOFTCAP unset; skipping\n" ); CHECK( true ); return; }
	SoftCap cap;
	if( !LoadSoftCap( path, cap ) ) { std::printf( "    [winding] cannot load %s\n", path ); CHECK( false ); return; }
	auto envI = []( const char* k, int d ) { const char* s = std::getenv( k ); return s ? std::atoi( s ) : d; };
	auto envF = []( const char* k, float d ) { const char* s = std::getenv( k ); return s ? ( float )std::atof( s ) : d; };
	const int MINTRI = envI( "WD_MINTRI", 32 ), MAXTRI = envI( "WD_MAXTRI", 40000 );
	const float WELD = envF( "WD_WELD", 0.05f );

	long totB = 0, totM = 0, totNM = 0, totCons = 0, totIncons = 0;
	long cTot = 0, cClean = 0, cOrientable = 0, cClosed = 0, cAlreadyConsistent = 0, cNonManifold = 0, cNonOrientable = 0, cOpen = 0;
	double sumBoundaryFrac = 0;
	long inconB2B = 0, inconDup = 0, inconFold = 0;		// inconsistent-edge kinds: back-to-back / duplicate / genuine fold
	auto triNrm = [&]( const softcapCaster_t& cs2, int t ) -> float3
	{
		const uint32_t a = cap.meshIdx[cs2.firstIndex + t * 3 + 0], b = cap.meshIdx[cs2.firstIndex + t * 3 + 1], d = cap.meshIdx[cs2.firstIndex + t * 3 + 2];
		float3 v0( cap.meshVerts[a * 3], cap.meshVerts[a * 3 + 1], cap.meshVerts[a * 3 + 2] );
		float3 v1( cap.meshVerts[b * 3], cap.meshVerts[b * 3 + 1], cap.meshVerts[b * 3 + 2] );
		float3 v2( cap.meshVerts[d * 3], cap.meshVerts[d * 3 + 1], cap.meshVerts[d * 3 + 2] );
		float3 n = cross( v1 - v0, v2 - v0 ); float l = std::sqrt( dot( n, n ) );
		return l > 1e-12f ? n * ( 1.0f / l ) : float3( 0, 0, 0 );
	};

	for( uint32_t c = 0; c < cap.casters.size(); c++ )
	{
		const softcapCaster_t& cs = cap.casters[c];
		const int nT = ( int )( cs.numIndex / 3 );
		if( nT < MINTRI || nT > MAXTRI ) { continue; }
		cTot++;

		std::unordered_map<uint64_t, int> weldMap;
		auto weld = [&]( uint32_t gi ) -> int
		{
			const float* p = &cap.meshVerts[gi * 3];
			int64_t x = ( int64_t )std::llround( p[0] / WELD ), y = ( int64_t )std::llround( p[1] / WELD ), z = ( int64_t )std::llround( p[2] / WELD );
			uint64_t k = ( uint64_t )( ( x * 73856093LL ) ^ ( y * 19349663LL ) ^ ( z * 83492791LL ) );
			auto it = weldMap.find( k );
			if( it != weldMap.end() ) { return it->second; }
			int id = ( int )weldMap.size(); weldMap[k] = id; return id;
		};
		std::vector<int> tw( nT * 3 );
		for( int t = 0; t < nT; t++ )
			for( int k = 0; k < 3; k++ ) { tw[t * 3 + k] = weld( cap.meshIdx[cs.firstIndex + t * 3 + k] ); }

		struct ERec { int t; bool lh; };
		std::unordered_map<uint64_t, std::vector<ERec>> edges;
		for( int t = 0; t < nT; t++ )
			for( int k = 0; k < 3; k++ )
			{
				int a = tw[t * 3 + k], b = tw[t * 3 + ( k + 1 ) % 3];
				if( a == b ) { continue; }
				int lo = a < b ? a : b, hi = a < b ? b : a;
				edges[( ( uint64_t )( uint32_t )lo << 32 ) | ( uint32_t )hi].push_back( { t, a < b } );
			}

		// union-find with parity (0 = same flip, 1 = opposite flip) over triangles
		std::vector<int> uf( nT ), par( nT, 0 );
		for( int i = 0; i < nT; i++ ) { uf[i] = i; }
		auto find = [&]( int x, int& p ) -> int { p = 0; while( uf[x] != x ) { p ^= par[x]; x = uf[x]; } return x; };

		long lB = 0, lM = 0, lNM = 0, lCons = 0, lIncons = 0; bool orientable = true, hasNonManifold = false;
		for( auto& e : edges )
		{
			const std::vector<ERec>& r = e.second;
			if( r.size() == 1 ) { lB++; continue; }
			if( r.size() > 2 ) { lNM++; hasNonManifold = true; continue; }
			lM++;
			const bool consistent = ( r[0].lh != r[1].lh );	// opposite traversal = consistently wound
			if( consistent ) { lCons++; }
			else
			{
				lIncons++;
				const float cosA = dot( triNrm( cs, r[0].t ), triNrm( cs, r[1].t ) );
				if( cosA < -0.99f ) { inconB2B++; }			// normals ~opposite: back-to-back double-sided face
				else if( cosA > 0.99f ) { inconDup++; }		// normals ~equal: duplicate/coplanar triangle
				else { inconFold++; }						// genuine wrong-wound fold
			}
			const int rel = consistent ? 0 : 1;				// flip parity needed to make consistent
			int pa, pb; int ra = find( r[0].t, pa ), rb = find( r[1].t, pb );
			if( ra == rb ) { if( ( ( pa ^ pb ) & 1 ) != rel ) { orientable = false; } }
			else { uf[ra] = rb; par[ra] = ( pa ^ pb ^ rel ) & 1; }
		}
		totB += lB; totM += lM; totNM += lNM; totCons += lCons; totIncons += lIncons;
		const double bf = ( lB + lM + lNM ) ? ( double )lB / ( lB + lM + lNM ) : 0.0;
		sumBoundaryFrac += bf;
		if( lB > 0 ) { cOpen++; }
		if( hasNonManifold ) { cNonManifold++; }
		if( !orientable ) { cNonOrientable++; }
		if( orientable && !hasNonManifold ) { cOrientable++; }
		if( lB == 0 ) { cClosed++; }
		if( lIncons == 0 && lM > 0 ) { cAlreadyConsistent++; }
		if( lB == 0 && !hasNonManifold && orientable ) { cClean++; }	// closed + manifold + orientable
	}

	if( cTot == 0 ) { std::printf( "    [winding] no casters in [%d,%d] tris\n", MINTRI, MAXTRI ); CHECK( true ); return; }
	const long totE = totB + totM + totNM;
	std::printf( "    [winding] %s | %ld casters (>=%d tris), weld %.2fu\n", path, cTot, MINTRI, WELD );
	std::printf( "    [winding] EDGES: boundary %.1f%% | manifold %.1f%% | NON-manifold %.1f%%   (of %ld welded edges)\n",
				 100.0 * totB / totE, 100.0 * totM / totE, 100.0 * totNM / totE, totE );
	std::printf( "    [winding] of MANIFOLD edges: consistently wound %.1f%% | inconsistent %.1f%% (%ld edges)\n",
				 totM ? 100.0 * totCons / totM : 0.0, totM ? 100.0 * totIncons / totM : 0.0, totIncons );
	std::printf( "    [winding] the INCONSISTENT edges are: back-to-back double-sided %.1f%% | duplicate/coplanar %.1f%% | GENUINE wrong-wound fold %.1f%%\n",
				 totIncons ? 100.0 * inconB2B / totIncons : 0.0, totIncons ? 100.0 * inconDup / totIncons : 0.0, totIncons ? 100.0 * inconFold / totIncons : 0.0 );
	std::printf( "    [winding] mean boundary-edge fraction per caster: %.1f%% (open surfaces have a real silhouette hole)\n", 100.0 * sumBoundaryFrac / cTot );
	std::printf( "    [winding] CASTER verdict: CLEAN(closed+manifold+orientable) %.1f%% | already-consistent %.1f%% | orientable(fixable) %.1f%%\n",
				 100.0 * cClean / cTot, 100.0 * cAlreadyConsistent / cTot, 100.0 * cOrientable / cTot );
	std::printf( "    [winding] failure modes: OPEN(has boundary) %.1f%% | NON-manifold %.1f%% | NON-orientable %.1f%%\n",
				 100.0 * cOpen / cTot, 100.0 * cNonManifold / cTot, 100.0 * cNonOrientable / cTot );
	CHECK( true );
}

// THE TEST (winding now known consistent): does the ANALYTIC SILHOUETTE coverage match the ray-truth
// UNION on the real, open, single-sided capture geometry? If yes across the penumbra, we have a
// banding-free general-case coverage the analytic-drains/inconsistent-winding arguments wrongly foreclosed.
STUDY_TEST( SoftShadowSilhouetteCov, quantify )
{
	const char* path = std::getenv( "SOFTCAP" );
	if( path == NULL ) { std::printf( "    [silcov] SOFTCAP unset; skipping\n" ); CHECK( true ); return; }
	SoftCap cap;
	if( !LoadSoftCap( path, cap ) ) { std::printf( "    [silcov] cannot load %s\n", path ); CHECK( false ); return; }
	auto envI = []( const char* k, int d ) { const char* s = std::getenv( k ); return s ? std::atoi( s ) : d; };
	auto envF = []( const char* k, float d ) { const char* s = std::getenv( k ); return s ? ( float )std::atof( s ) : d; };
	const int N = envI( "UG_N", 32 ), SAMP = envI( "UG_SAMP", 60 ), MAXC = envI( "UG_CASTERS", 200 ), MAXTRI = envI( "UG_MAXTRI", 4000 );
	const float WELD = envF( "WD_WELD", 0.05f );

	std::vector<std::vector<float3>> lightRecv( cap.lights.size() );
	for( const softcapReceiver_t& r : cap.receivers )
	{
		if( r.lightIndex >= cap.lights.size() ) { continue; }
		for( uint32_t t = 0; t < r.numIndex / 3; t++ )
		{
			const uint32_t i0 = cap.recvIdx[r.firstIndex + t * 3 + 0], i1 = cap.recvIdx[r.firstIndex + t * 3 + 1], i2 = cap.recvIdx[r.firstIndex + t * 3 + 2];
			float3 p0( cap.recvVerts[i0 * 3], cap.recvVerts[i0 * 3 + 1], cap.recvVerts[i0 * 3 + 2] );
			float3 p1( cap.recvVerts[i1 * 3], cap.recvVerts[i1 * 3 + 1], cap.recvVerts[i1 * 3 + 2] );
			float3 p2( cap.recvVerts[i2 * 3], cap.recvVerts[i2 * 3 + 1], cap.recvVerts[i2 * 3 + 2] );
			lightRecv[r.lightIndex].push_back( ( p0 + p1 + p2 ) * ( 1.0f / 3.0f ) );
		}
	}

	const int NB = 10;
	double bAbs[NB] = {}, bSgn[NB] = {}, bSil[NB] = {}, bUni[NB] = {}; long bN[NB] = {};
	long nSamp = 0, nPen = 0, nPenNear = 0; int usedC = 0;
	for( uint32_t c = 0; c < cap.casters.size() && usedC < MAXC; c++ )
	{
		const softcapCaster_t& cs = cap.casters[c];
		const int nT = ( int )( cs.numIndex / 3 );
		if( nT < 1 || nT > MAXTRI || cs.lightIndex >= cap.lights.size() ) { continue; }
		const std::vector<float3>& recv = lightRecv[cs.lightIndex];
		if( recv.empty() ) { continue; }
		const float3 L( cap.lights[cs.lightIndex].origin[0], cap.lights[cs.lightIndex].origin[1], cap.lights[cs.lightIndex].origin[2] );
		const float  swR = std::fmax( cap.lights[cs.lightIndex].penumbraSize, 1e-2f );
		const std::vector<RAEdge2> edges = BuildWeldedCasterEdges( cap, cs, WELD );
		const float* rv = cap.meshVerts.data();
		const uint32_t* ri = &cap.meshIdx[cs.firstIndex];
		const size_t stride = std::max( ( size_t )1, recv.size() / ( size_t )SAMP );
		bool anyPen = false;
		for( size_t s = 0; s < recv.size(); s += stride )
		{
			const float3 P = recv[s];
			const float uni = 1.0f - MeshTruthShadowSoup( rv, ri, cs.numIndex, P, L, swR, N );
			if( uni < 0.002f ) { continue; }
			anyPen = true;
			const float sil = SilhouetteCov( edges, P, L, swR );
			const float gap = sil - uni;
			int bi = ( int )( uni * NB ); if( bi < 0 ) { bi = 0; } if( bi >= NB ) { bi = NB - 1; }
			bAbs[bi] += std::fabs( gap ); bSgn[bi] += gap; bSil[bi] += sil; bUni[bi] += uni; bN[bi]++;
			nSamp++;
			if( uni < 0.998f ) { nPen++; if( std::fabs( gap ) <= 0.06f ) { nPenNear++; } }
		}
		if( anyPen ) { usedC++; }
	}
	if( nSamp == 0 ) { std::printf( "    [silcov] no shadowing samples\n" ); CHECK( true ); return; }
	std::printf( "    [silcov] %s | casters %d, samples %ld, N=%d\n", path, usedC, nSamp, N );
	std::printf( "    [silcov] ANALYTIC silhouette vs UNION: |gap|<=0.06 on %.1f%% of PENUMBRA samples (nPen %ld)\n",
				 nPen ? 100.0 * nPenNear / nPen : 0.0, nPen );
	std::printf( "    [silcov] union bin -> n | union | silhouette | |gap| | signed\n" );
	for( int b = 0; b < NB; b++ )
	{
		if( bN[b] == 0 ) { continue; }
		std::printf( "      occ [%.1f,%.1f)  n%6ld  union %.3f  sil %.3f  |gap| %.4f  signed %+.4f\n",
					 b * 0.1, b * 0.1 + 0.1, bN[b], bUni[b] / bN[b], bSil[b] / bN[b], bAbs[b] / bN[b], bSgn[b] / bN[b] );
	}
	CHECK( true );
}

// VALIDATION (STUDY: it FAILS - the connector-free/unordered edge sum in SilhouetteCov is WRONG, giving
// 0.196 vs true 0.914 at umbra on a CLOSED box). Kept as a study to document that SilhouetteCov is NOT a
// valid analytic coverage; the real ProcCaster (ordered chain + near-plane connectors) is needed for a
// correct open-geometry test. Do NOT trust the SoftShadowSilhouetteCov capture numbers - they measure
// this bug, not the geometry.
STUDY_TEST( SilhouetteImpl, box_matches_union )
{
	std::vector<float> V; std::vector<uint32_t> I;
	for( int i = 0; i < 8; i++ ) { V.push_back( ( i & 1 ) ? 1.f : -1.f ); V.push_back( ( i & 2 ) ? 1.f : -1.f ); V.push_back( ( i & 4 ) ? 1.f : -1.f ); }
	const int quads[6][4] = { {0, 2, 6, 4}, {1, 5, 7, 3}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 1, 3, 2}, {4, 6, 7, 5} };
	for( int q = 0; q < 6; q++ ) { int a = quads[q][0], b = quads[q][1], c = quads[q][2], d = quads[q][3]; I.push_back( a ); I.push_back( b ); I.push_back( c ); I.push_back( a ); I.push_back( c ); I.push_back( d ); }
	for( size_t k = 0; k < I.size(); k += 3 )		// fix winding OUTWARD (consistent)
	{
		float3 v0( V[I[k] * 3], V[I[k] * 3 + 1], V[I[k] * 3 + 2] ), v1( V[I[k + 1] * 3], V[I[k + 1] * 3 + 1], V[I[k + 1] * 3 + 2] ), v2( V[I[k + 2] * 3], V[I[k + 2] * 3 + 1], V[I[k + 2] * 3 + 2] );
		float3 n = cross( v1 - v0, v2 - v0 ), ctr = ( v0 + v1 + v2 ) * ( 1.f / 3.f );
		if( dot( n, ctr ) < 0.0f ) { uint32_t tmp = I[k + 1]; I[k + 1] = I[k + 2]; I[k + 2] = tmp; }
	}
	std::vector<RAEdge2> edges = BuildEdgesRaw( V.data(), I.data(), ( uint32_t )I.size(), 0.01f );
	int nb = 0; for( const RAEdge2& e : edges ) { if( e.boundary ) { nb++; } }
	const float3 L( 0, 0, 30 ); const float R = 5;
	float maxErr = 0;
	for( float x = 0; x <= 8.0f; x += 1.0f )
	{
		float3 P( x, 0, -10 );
		float uni = 1.f - MeshTruthShadowSoup( V.data(), I.data(), ( uint32_t )I.size(), P, L, R, 128 );
		float sil = SilhouetteCov( edges, P, L, R );
		std::printf( "    [silbox] P.x=%.0f  union %.3f  sil %.3f  err %.3f\n", x, uni, sil, std::fabs( sil - uni ) );
		maxErr = std::fmax( maxErr, std::fabs( sil - uni ) );
	}
	std::printf( "    [silbox] boundary edges=%d (want 0), maxErr=%.3f\n", nb, maxErr );
	CHECK( nb == 0 );
	CHECK( maxErr < 0.06f );
}
