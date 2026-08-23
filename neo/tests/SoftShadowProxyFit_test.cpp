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

// PROXY-FIT FEASIBILITY PROBE (study, not a test). Measure-first gate for the "replace a caster's
// thousands of triangles with one analytic primitive" direction. Every per-instruction walk lever is at
// the instruction-issue floor (see softshadow-walk-attribution); the ONE remaining lever is RECORD COUNT
// per fragment, and a caster's triangle count IS that count. A 2000-tri pillar swapped for a 12-tri box
// removes ~160x records from every tile it spans. But it is LOSSY, so before building the fitter this
// probe answers three numbers straight off a .cap, OFFLINE, no renderer/GPU:
//   (1) RECORD CEILING  - fraction of caster tri-mass on casters a primitive fits within tolerance.
//   (2) COVERAGE ERROR  - real-mesh ray-truth vs proxy ray-truth over each light's penumbra receivers
//                         (= what com_softShadowGate's RT oracle would see).
//   (3) FIT MIX         - which primitive wins per caster; what stays triangles.
// Cheap axis-aligned fits first (sphere / world-AABB box / world-Z cylinder): Doom3 architecture is
// axis-aligned, so if these already proxy a big tri-mass at low error the direction is proven and OBB/PCA
// is a later refinement. Small casters (<= SMALL_TRIS) are not worth proxying and are only tallied.
//
// Run:  CAP=/path/to/foo.cap ./rbdoom3bfg_tests @study:SoftShadowProxyFit
//   env: PROXY_TOL (penumbra mean-err ship threshold, default 0.05) | PROXY_N (disk samples/side, 16) |
//        PROXY_RECV (receiver budget/light, 80) | PROXY_MINTRIS (probe only casters over this, 64)

#include "hlsl_compat.h"
#include "SoftShadowMesh.h"					// Cap + LoadCap + MeshTruthShadowSoup + RayHitsMesh
#include "idUnitTest.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <algorithm>
#include <functional>
#include <unordered_set>

using namespace swtest;

namespace
{

struct TileAABB { float3 c; float r; };		// world-space receiver AABB of one 16x16 screen tile

struct ProxyMesh
{
	std::vector<float>    v;		// float3 packed
	std::vector<uint32_t> idx;	// global into v
	const char*           name;
	int Tris() const { return ( int )idx.size() / 3; }
};

inline void PushTri( ProxyMesh& m, uint32_t a, uint32_t b, uint32_t c )
{
	m.idx.push_back( a ); m.idx.push_back( b ); m.idx.push_back( c );
}
inline uint32_t PushVert( ProxyMesh& m, float3 p )
{
	uint32_t i = ( uint32_t )( m.v.size() / 3 );
	m.v.push_back( p.x ); m.v.push_back( p.y ); m.v.push_back( p.z );
	return i;
}

// world-AABB box: 8 corners, 12 tris (winding irrelevant - RayHitsMesh is unsigned)
ProxyMesh MakeBox( float3 mn, float3 mx )
{
	ProxyMesh m; m.name = "box";
	uint32_t c[8];
	for( int i = 0; i < 8; i++ )
	{
		c[i] = PushVert( m, float3( ( i & 1 ) ? mx.x : mn.x, ( i & 2 ) ? mx.y : mn.y, ( i & 4 ) ? mx.z : mn.z ) );
	}
	// 6 faces as quads (2 tris each), by corner-bit pairs
	static const int F[6][4] = { {0,2,6,4}, {1,3,7,5}, {0,1,5,4}, {2,3,7,6}, {0,1,3,2}, {4,5,7,6} };
	for( int f = 0; f < 6; f++ )
	{
		PushTri( m, c[F[f][0]], c[F[f][1]], c[F[f][2]] );
		PushTri( m, c[F[f][0]], c[F[f][2]], c[F[f][3]] );
	}
	return m;
}

// world-Z cylinder: axis vertical through (cx,cy), radius R, [zmin,zmax], N sides. ~4N tris.
ProxyMesh MakeCylinderZ( float cx, float cy, float R, float zmin, float zmax, int N )
{
	ProxyMesh m; m.name = "cyl";
	std::vector<uint32_t> lo( N ), hi( N );
	for( int i = 0; i < N; i++ )
	{
		float a = ( float )( 2.0 * 3.14159265358979 * i / N );
		float x = cx + R * std::cos( a ), y = cy + R * std::sin( a );
		lo[i] = PushVert( m, float3( x, y, zmin ) );
		hi[i] = PushVert( m, float3( x, y, zmax ) );
	}
	uint32_t cLo = PushVert( m, float3( cx, cy, zmin ) ), cHi = PushVert( m, float3( cx, cy, zmax ) );
	for( int i = 0; i < N; i++ )
	{
		int j = ( i + 1 ) % N;
		PushTri( m, lo[i], lo[j], hi[j] );	// side quad
		PushTri( m, lo[i], hi[j], hi[i] );
		PushTri( m, cLo, lo[j], lo[i] );	// bottom cap fan
		PushTri( m, cHi, hi[i], hi[j] );	// top cap fan
	}
	return m;
}

// icosahedron sphere: centroid + radius, 12 verts / 20 tris (coarse but closed).
ProxyMesh MakeSphere( float3 ctr, float R )
{
	ProxyMesh m; m.name = "sph";
	const float t = 1.61803398875f;			// golden ratio
	const float s = R / std::sqrt( 1.0f + t * t );
	static const float P[12][3] = {
		{-1, t, 0}, {1, t, 0}, {-1,-t, 0}, {1,-t, 0},
		{0,-1, t}, {0, 1, t}, {0,-1,-t}, {0, 1,-t},
		{ t, 0,-1}, { t, 0, 1}, {-t, 0,-1}, {-t, 0, 1}
	};
	for( int i = 0; i < 12; i++ )
	{
		PushVert( m, float3( ctr.x + P[i][0] * s, ctr.y + P[i][1] * s, ctr.z + P[i][2] * s ) );
	}
	static const int F[20][3] = {
		{0,11,5},{0,5,1},{0,1,7},{0,7,10},{0,10,11},{1,5,9},{5,11,4},{11,10,2},{10,7,6},{7,1,8},
		{3,9,4},{3,4,2},{3,2,6},{3,6,8},{3,8,9},{4,9,5},{2,4,11},{6,2,10},{8,6,7},{9,8,1}
	};
	for( int f = 0; f < 20; f++ ) { PushTri( m, F[f][0], F[f][1], F[f][2] ); }
	return m;
}

// oriented box: centre + 3 orthonormal axes u,v,w and half-extents. 12 tris.
ProxyMesh MakeOBB( float3 c, float3 u, float3 v, float3 w, float3 he )
{
	ProxyMesh m; m.name = "obb";
	uint32_t cor[8];
	for( int i = 0; i < 8; i++ )
	{
		float su = ( i & 1 ) ? he.x : -he.x, sv = ( i & 2 ) ? he.y : -he.y, sw = ( i & 4 ) ? he.z : -he.z;
		cor[i] = PushVert( m, float3( c.x + u.x * su + v.x * sv + w.x * sw, c.y + u.y * su + v.y * sv + w.y * sw, c.z + u.z * su + v.z * sv + w.z * sw ) );
	}
	static const int F[6][4] = { {0,2,6,4}, {1,3,7,5}, {0,1,5,4}, {2,3,7,6}, {0,1,3,2}, {4,5,7,6} };
	for( int f = 0; f < 6; f++ ) { PushTri( m, cor[F[f][0]], cor[F[f][1]], cor[F[f][2]] ); PushTri( m, cor[F[f][0]], cor[F[f][2]], cor[F[f][3]] ); }
	return m;
}

// capped cylinder around an arbitrary axis p0->p1, radius R, N sides.
ProxyMesh MakeCylinderAxis( float3 p0, float3 p1, float R, int N )
{
	ProxyMesh m; m.name = "cyl";
	float3 ax = p1 - p0; float L = std::sqrt( dot( ax, ax ) ); if( L < 1e-5f ) { return m; }
	ax = ax * ( 1.0f / L );
	float3 up = ( std::fabs( ax.z ) < 0.9f ) ? float3( 0, 0, 1 ) : float3( 1, 0, 0 );
	float3 e0 = normalize( cross( up, ax ) ), e1 = cross( ax, e0 );
	std::vector<uint32_t> lo( N ), hi( N );
	for( int i = 0; i < N; i++ )
	{
		float a = ( float )( 2.0 * 3.14159265358979 * i / N );
		float3 off = e0 * ( R * std::cos( a ) ) + e1 * ( R * std::sin( a ) );
		lo[i] = PushVert( m, p0 + off );
		hi[i] = PushVert( m, p1 + off );
	}
	uint32_t cLo = PushVert( m, p0 ), cHi = PushVert( m, p1 );
	for( int i = 0; i < N; i++ )
	{
		int j = ( i + 1 ) % N;
		PushTri( m, lo[i], lo[j], hi[j] ); PushTri( m, lo[i], hi[j], hi[i] );
		PushTri( m, cLo, lo[j], lo[i] ); PushTri( m, cHi, hi[i], hi[j] );
	}
	return m;
}

} // namespace

STUDY_TEST( SoftShadowProxyFit, feasibility )
{
	const char* path = std::getenv( "CAP" );
	if( path == NULL )
	{
		std::printf( "    [proxyfit] CAP unset; skipping\n" );
		CHECK( true );
		return;
	}
	Cap cap;
	if( !LoadCap( path, cap ) )
	{
		std::printf( "    [proxyfit] cannot load %s\n", path );
		CHECK( false );
		return;
	}

	auto envF = []( const char* k, float d ) { const char* s = std::getenv( k ); return s ? ( float )std::atof( s ) : d; };
	auto envI = []( const char* k, int d )   { const char* s = std::getenv( k ); return s ? std::atoi( s ) : d; };
	const float TOL       = envF( "PROXY_TOL", 0.05f );		// penumbra mean-err ship threshold
	const int   N         = envI( "PROXY_N", 16 );			// disk samples per side
	const int   RECV      = envI( "PROXY_RECV", 80 );		// receiver budget per light
	const int   SMALL_TRIS = envI( "PROXY_MINTRIS", 64 );	// probe only casters over this

	// receiver-centroid P set per light (reused for every caster of that light)
	std::vector<std::vector<float3>> lightRecv( cap.lights.size() );
	for( const capReceiver_t& r : cap.receivers )
	{
		if( r.lightIndex >= cap.lights.size() ) { continue; }
		std::vector<float3>& dst = lightRecv[r.lightIndex];
		const uint32_t tc = r.numIndex / 3;
		for( uint32_t t = 0; t < tc; t++ )
		{
			const uint32_t i0 = cap.recvIdx[r.firstIndex + t * 3 + 0];
			const uint32_t i1 = cap.recvIdx[r.firstIndex + t * 3 + 1];
			const uint32_t i2 = cap.recvIdx[r.firstIndex + t * 3 + 2];
			float3 p0( cap.recvVerts[i0 * 3], cap.recvVerts[i0 * 3 + 1], cap.recvVerts[i0 * 3 + 2] );
			float3 p1( cap.recvVerts[i1 * 3], cap.recvVerts[i1 * 3 + 1], cap.recvVerts[i1 * 3 + 2] );
			float3 p2( cap.recvVerts[i2 * 3], cap.recvVerts[i2 * 3 + 1], cap.recvVerts[i2 * 3 + 2] );
			// centroid + 3 edge-midpoints: 4 samples/triangle so a sub-triangle shadow can't slip between
			// a single centroid (the centroid-resolution floor that would fake "no-evidence").
			dst.push_back( ( p0 + p1 + p2 ) * ( 1.0f / 3.0f ) );
			dst.push_back( ( p0 + p1 ) * 0.5f );
			dst.push_back( ( p1 + p2 ) * 0.5f );
			dst.push_back( ( p2 + p0 ) * 0.5f );
		}
	}

	// per-light SCREEN-TILE receiver AABBs: reproduce softtile_bin exactly. Project each receiver vert
	// through worldMVP to a 16px tile, accumulate that tile's world-space receiver AABB. A tile straddling
	// a silhouette collects fg+bg verts -> a fat AABB, reproducing the depth-discontinuity inflation that
	// makes the tile cone conservative. tR = AABB half-diagonal (the stage-1 inflation term).
	const int SW = ( int )cap.hdr.screenW, SH = ( int )cap.hdr.screenH;
	const int tilesX = ( SW + 15 ) / 16;
	const float* MVP = cap.hdr.worldMVP;
	auto Project = [&]( float3 p, int& tx, int& ty, float& ndcz ) -> bool
	{
		float x = MVP[0] * p.x + MVP[1] * p.y + MVP[2] * p.z + MVP[3];
		float y = MVP[4] * p.x + MVP[5] * p.y + MVP[6] * p.z + MVP[7];
		float z = MVP[8] * p.x + MVP[9] * p.y + MVP[10] * p.z + MVP[11];
		float w = MVP[12] * p.x + MVP[13] * p.y + MVP[14] * p.z + MVP[15];
		if( w <= 1e-6f ) { return false; }
		float px = ( x / w * 0.5f + 0.5f ) * SW, py = ( 1.0f - ( y / w * 0.5f + 0.5f ) ) * SH;
		if( px < 0 || px >= SW || py < 0 || py >= SH ) { return false; }
		tx = ( int )px / 16; ty = ( int )py / 16; ndcz = z / w; return true;
	};
	// collect per-tile receiver verts (+ depth). lightTiles = 1 AABB/tile (the SHIPPED tile-bin). lightTiles2
	// = DEPTH-CLUSTERED: split each tile's verts at the largest depth gap into 2 thin bands -> a silhouette
	// tile's empty fg/bg middle is excluded, so the cone stops keeping casters that shadow that empty gap.
	std::vector<std::vector<TileAABB>> lightTiles( cap.lights.size() ), lightTiles2( cap.lights.size() );
	{
		std::vector<std::unordered_map<int, std::vector<std::pair<float3, float>>>> tv( cap.lights.size() );
		for( const capReceiver_t& r : cap.receivers )
		{
			if( r.lightIndex >= cap.lights.size() ) { continue; }
			auto& m = tv[r.lightIndex];
			for( uint32_t k = r.firstVert; k < r.firstVert + r.numVerts; k++ )
			{
				float3 p( cap.recvVerts[k * 3], cap.recvVerts[k * 3 + 1], cap.recvVerts[k * 3 + 2] );
				int tx, ty; float dz; if( !Project( p, tx, ty, dz ) ) { continue; }
				m[ty * tilesX + tx].push_back( { p, dz } );
			}
		}
		auto aabbOf = []( const std::vector<std::pair<float3, float>>& vs, int lo, int hi ) -> TileAABB
		{
			float3 mn( 1e30f, 1e30f, 1e30f ), mx( -1e30f, -1e30f, -1e30f );
			for( int i = lo; i < hi; i++ ) { float3 p = vs[i].first; mn = float3( std::fmin( mn.x, p.x ), std::fmin( mn.y, p.y ), std::fmin( mn.z, p.z ) ); mx = float3( std::fmax( mx.x, p.x ), std::fmax( mx.y, p.y ), std::fmax( mx.z, p.z ) ); }
			float3 c = ( mn + mx ) * 0.5f, he = ( mx - mn ) * 0.5f;
			return { c, std::sqrt( dot( he, he ) ) };
		};
		for( size_t L = 0; L < cap.lights.size(); L++ )
		{
			for( auto& kv : tv[L] )
			{
				auto& vs = kv.second;
				lightTiles[L].push_back( aabbOf( vs, 0, ( int )vs.size() ) );
				std::sort( vs.begin(), vs.end(), []( const std::pair<float3, float>& a, const std::pair<float3, float>& b ) { return a.second < b.second; } );
				int splitAt = -1; float bestGap = 0;
				for( int i = 1; i < ( int )vs.size(); i++ ) { float g = vs[i].second - vs[i - 1].second; if( g > bestGap ) { bestGap = g; splitAt = i; } }
				if( splitAt > 0 )
				{
					lightTiles2[L].push_back( aabbOf( vs, 0, splitAt ) );
					lightTiles2[L].push_back( aabbOf( vs, splitAt, ( int )vs.size() ) );
				}
				else { lightTiles2[L].push_back( aabbOf( vs, 0, ( int )vs.size() ) ); }
			}
		}
	}

	// exact softtile_bin stage-1 over a given tile set: does the caster sphere survive >=1 tile of its light?
	auto BinTest = [&]( float3 ctr, float sphR, uint32_t li, const std::vector<std::vector<TileAABB>>& tiles ) -> bool
	{
		const float3 Lp( cap.lights[li].origin[0], cap.lights[li].origin[1], cap.lights[li].origin[2] );
		const float  swR = std::fmax( cap.lights[li].penumbraSize, 1e-2f );
		for( const TileAABB& t : tiles[li] )
		{
			float R = sphR + t.r;
			float3 rc = ctr - t.c;
			float3 toL = Lp - t.c; float distPL = std::fmax( std::sqrt( dot( toL, toL ) ), 1e-4f );
			float3 nrm = toL * ( 1.0f / distPL );
			float cd = dot( rc, nrm );
			if( cd + R < 1e-4f ) { continue; }
			if( cd - R > distPL + t.r ) { continue; }
			float3 perp = rc - nrm * cd;
			float coneR = swR * ( cd + R ) / std::fmax( distPL - t.r, 1e-4f );
			if( dot( perp, perp ) > ( coneR + R ) * ( coneR + R ) ) { continue; }
			return true;
		}
		return false;
	};
	auto IsBinned  = [&]( float3 ctr, float sphR, uint32_t li ) { return BinTest( ctr, sphR, li, lightTiles ); };
	auto IsBinned2 = [&]( float3 ctr, float sphR, uint32_t li ) { return BinTest( ctr, sphR, li, lightTiles2 ); };
	long tileTot = 0; for( auto& v : lightTiles ) { tileTot += ( long )v.size(); }

	// Is a caster's CENTRE occluded from the light by another same-light caster/world face? If so the caster
	// is itself in shadow and casts nothing - a per-caster light-visibility cull would remove it (cheap: one
	// ray per caster). Measures whether the no-penumbra waste is "self-shadowed casters".
	auto selfShadowed = [&]( float3 ctr, uint32_t self, const float3& Lp ) -> bool
	{
		float3 dir = Lp - ctr;
		const uint32_t sl = cap.casters[self].lightIndex;
		for( uint32_t cc = 0; cc < cap.casters.size(); cc++ )
		{
			if( cc == self || cap.casters[cc].lightIndex != sl ) { continue; }
			const capCaster_t& C = cap.casters[cc];
			if( RayHitsMesh( ctr, dir, cap.meshVerts.data(), &cap.meshIdx[C.firstIndex], C.numIndex ) ) { return true; }
		}
		return false;
	};

	// aggregates
	long   nEvBinned = 0, nEvBinned2 = 0;		// SANITY: evidenced casters binned by 1-band / 2-band (both ~= nProbed)
	double totalTris = 0, smallTris = 0, probedTris = 0, noEvTris = 0, binnedNoEvTris = 0, unbinnedNoEvTris = 0;
	double binnedNoEv2Tris = 0;					// no-evidence tris STILL binned after 2-band depth-clustering (residual)
	double selfShadowNoEvTris = 0; long nSelfShadow = 0;	// no-evidence casters that are themselves in the light's shadow
	double proxyableTris = 0, savedRecords = 0;			// tri-mass proxyable @ TOL, records removed
	long   nBox = 0, nCyl = 0, nSph = 0, nKeep = 0, nProbed = 0, nSmall = 0, nNoEv = 0, nBinned = 0, nBinned2 = 0;

	struct Row { uint32_t c; int realT, proxyT; const char* prim; float meanErr, maxErr; };
	std::vector<Row> rows;

	for( uint32_t c = 0; c < cap.casters.size(); c++ )
	{
		const capCaster_t& cs = cap.casters[c];
		const int realT = ( int )( cs.numIndex / 3 );
		if( realT <= 0 ) { continue; }
		totalTris += realT;
		if( realT <= SMALL_TRIS ) { smallTris += realT; nSmall++; continue; }
		if( cs.lightIndex >= cap.lights.size() ) { continue; }
		const std::vector<float3>& recv = lightRecv[cs.lightIndex];
		if( recv.empty() ) { continue; }

		// world-AABB + cylinder/sphere params from this caster's verts
		float3 mn( 1e30f, 1e30f, 1e30f ), mx( -1e30f, -1e30f, -1e30f ), sum( 0, 0, 0 );
		for( uint32_t k = cs.firstVert; k < cs.firstVert + cs.numVerts; k++ )
		{
			float3 p( cap.meshVerts[k * 3], cap.meshVerts[k * 3 + 1], cap.meshVerts[k * 3 + 2] );
			mn = float3( std::fmin( mn.x, p.x ), std::fmin( mn.y, p.y ), std::fmin( mn.z, p.z ) );
			mx = float3( std::fmax( mx.x, p.x ), std::fmax( mx.y, p.y ), std::fmax( mx.z, p.z ) );
			sum = sum + p;
		}
		float3 ctr = sum * ( 1.0f / std::fmax( 1.0f, ( float )cs.numVerts ) );
		float3 bctr = ( mn + mx ) * 0.5f;
		float  sphR = 0.0f, cylR = 0.0f;
		for( uint32_t k = cs.firstVert; k < cs.firstVert + cs.numVerts; k++ )
		{
			float3 p( cap.meshVerts[k * 3], cap.meshVerts[k * 3 + 1], cap.meshVerts[k * 3 + 2] );
			float3 d = p - ctr; sphR = std::fmax( sphR, std::sqrt( dot( d, d ) ) );
			float rx = p.x - bctr.x, ry = p.y - bctr.y; cylR = std::fmax( cylR, std::sqrt( rx * rx + ry * ry ) );
		}

		ProxyMesh cand[3] = { MakeBox( mn, mx ), MakeCylinderZ( bctr.x, bctr.y, cylR, mn.z, mx.z, 8 ), MakeSphere( ctr, sphR ) };

		const size_t stride = std::max( ( size_t )1, recv.size() / ( size_t )RECV );
		const float3 Lp( cap.lights[cs.lightIndex].origin[0], cap.lights[cs.lightIndex].origin[1], cap.lights[cs.lightIndex].origin[2] );
		const float  swR = std::fmax( cap.lights[cs.lightIndex].penumbraSize, 1e-2f );
		const float* rv = cap.meshVerts.data();
		const uint32_t* ri = &cap.meshIdx[cs.firstIndex];

		// PASS 1: real single-caster coverage per sampled receiver. realCov is candidate-independent, so
		// compute it ONCE and keep only the PENUMBRA samples (receivers this caster alone shadows). If none,
		// this caster shadows no sampled receiver -> NO EVIDENCE (not "perfectly proxyable"): separate bucket.
		std::vector<float3> pen; pen.reserve( ( size_t )RECV );
		for( size_t s = 0; s < recv.size(); s += stride )
		{
			const float3 P = recv[s];
			float realCov = MeshTruthShadowSoup( rv, ri, cs.numIndex, P, Lp, swR, N );
			if( realCov < 0.99f ) { pen.push_back( P ); }
		}
		if( pen.empty() )		// no penumbra evidence: this caster shadows no receiver -> CULL candidate
		{
			nNoEv++; noEvTris += realT;
			// THE decisive test: does the ACTUAL tile-bin still BIN this non-shadowing caster? Run the exact
			// softtile_bin stage-1 (R = sphR + tR, coneR = swR*(cd+R)/(distPL-tR)) against this light's tile
			// AABBs. Survives >=1 tile -> the shipped walk PROCESSES it despite it shadowing nothing =
			// RECOVERABLE waste a tighter cull removes. Survives 0 tiles -> tile-bin already drops it = no win.
			if( IsBinned( ctr, sphR, cs.lightIndex ) ) { nBinned++; binnedNoEvTris += realT; }
			else { unbinnedNoEvTris += realT; }
			if( IsBinned2( ctr, sphR, cs.lightIndex ) ) { nBinned2++; binnedNoEv2Tris += realT; }	// still binned after depth-clustering
			if( selfShadowed( ctr, c, Lp ) ) { nSelfShadow++; selfShadowNoEvTris += realT; }		// caster itself in shadow -> casts nothing
			continue;
		}

		// PASS 2: each candidate over the penumbra samples only. Real coverage recomputed here (cheap: pen is
		// small) so the error uses the exact same disk as the proxy - keeps both truths on identical rays.
		nProbed++; probedTris += realT;
		if( IsBinned( ctr, sphR, cs.lightIndex ) ) { nEvBinned++; }		// sanity: should be ~all
		if( IsBinned2( ctr, sphR, cs.lightIndex ) ) { nEvBinned2++; }	// depth-clustering MUST keep real shadowers
		float bestMean = 1e30f, bestMax = 0.0f; int bi = -1;
		for( int p = 0; p < 3; p++ )
		{
			double sumErr = 0.0, mx2 = 0.0;
			for( float3 P : pen )
			{
				float realCov = MeshTruthShadowSoup( rv, ri, cs.numIndex, P, Lp, swR, N );
				float proxCov = MeshTruthShadowSoup( cand[p].v.data(), cand[p].idx.data(), ( uint32_t )cand[p].idx.size(), P, Lp, swR, N );
				double e = std::fabs( realCov - proxCov );
				sumErr += e; mx2 = std::fmax( mx2, e );
			}
			double mean = sumErr / ( double )pen.size();
			if( mean < bestMean ) { bestMean = mean; bestMax = ( float )mx2; bi = p; }
		}
		rows.push_back( { c, realT, cand[bi].Tris(), cand[bi].name, ( float )bestMean, bestMax } );
		if( bestMean <= TOL )
		{
			proxyableTris += realT;
			savedRecords += ( realT - cand[bi].Tris() );
			if( bi == 0 ) { nBox++; } else if( bi == 1 ) { nCyl++; } else { nSph++; }
		}
		else { nKeep++; }
	}

	// worst offenders by realTris (the tiles they flood) - the prize casters
	std::sort( rows.begin(), rows.end(), []( const Row& a, const Row& b ) { return a.realT > b.realT; } );
	std::printf( "    [proxyfit] top casters by tri-mass (best axis-aligned fit):\n" );
	for( int i = 0; i < ( int )rows.size() && i < 16; i++ )
	{
		const Row& r = rows[i];
		std::printf( "      c%u  %5d tris -> %2d %s  mean %.3f max %.3f  %s\n",
					 r.c, r.realT, r.proxyT, r.prim, r.meanErr, r.maxErr, r.meanErr <= TOL ? "PROXY" : "keep" );
	}

	std::printf( "    [proxyfit] ===== AGGREGATE  (TOL %.2f, N %d, recv %d, minTris %d) =====\n", TOL, N, RECV, SMALL_TRIS );
	std::printf( "      casters: %zu total | %ld small(<=%d, %.0f%% of tris) | %ld no-evidence(%.0f%% of tris) | %ld probed-w/evidence\n",
				 cap.casters.size(), nSmall, SMALL_TRIS, 100.0 * smallTris / std::fmax( 1.0, totalTris ),
				 nNoEv, 100.0 * noEvTris / std::fmax( 1.0, totalTris ), nProbed );
	std::printf( "      evidenced verdict: %ld proxyable | %ld keep  (fit mix: box %ld | cyl %ld | sphere %ld)\n",
				 nBox + nCyl + nSph, nKeep, nBox, nCyl, nSph );
	std::printf( "      RECORD CEILING (evidenced-proxyable only, conservative): %.0f%% of ALL caster tri-mass (%.0fk of %.0fk tris)\n",
				 100.0 * proxyableTris / std::fmax( 1.0, totalTris ), proxyableTris / 1000.0, totalTris / 1000.0 );
	std::printf( "      of EVIDENCED (>%d tri, shadows a receiver) mass: %.0f%% proxyable; records removed %.0fk (%.1fx fewer on those)\n",
				 SMALL_TRIS, 100.0 * proxyableTris / std::fmax( 1.0, probedTris ),
				 savedRecords / 1000.0, probedTris / std::fmax( 1.0, probedTris - savedRecords ) );
	std::printf( "      [sanity] tiles built %ld; evidenced casters binned 1-band %ld/%ld, 2-band %ld/%ld (both want ~all)\n", tileTot, nEvBinned, nProbed, nEvBinned2, nProbed );
	std::printf( "      NO-EVIDENCE: %.0fk tris (%.0f%% of all) shadow no receiver.\n",
				 noEvTris / 1000.0, 100.0 * noEvTris / std::fmax( 1.0, totalTris ) );
	std::printf( "      1-band tile-bin (SHIPPED) still bins %.0fk (%ld casters) = %.0f%% of all records = RECOVERABLE waste\n",
				 binnedNoEvTris / 1000.0, nBinned, 100.0 * binnedNoEvTris / std::fmax( 1.0, totalTris ) );
	std::printf( "      2-band DEPTH-CLUSTER bins only %.0fk (%ld) = %.0f%% -> CULLS %.0fk (%.0f%% of all records); %.0f%% of the recoverable waste removed\n",
				 binnedNoEv2Tris / 1000.0, nBinned2, 100.0 * binnedNoEv2Tris / std::fmax( 1.0, totalTris ),
				 ( binnedNoEvTris - binnedNoEv2Tris ) / 1000.0, 100.0 * ( binnedNoEvTris - binnedNoEv2Tris ) / std::fmax( 1.0, totalTris ),
				 100.0 * ( binnedNoEvTris - binnedNoEv2Tris ) / std::fmax( 1.0, binnedNoEvTris ) );
	std::printf( "      SELF-SHADOWED: %.0fk (%ld casters) of the no-evidence mass are themselves in the light's shadow = %.0f%% of all records = per-caster light-visibility cull ceiling\n",
				 selfShadowNoEvTris / 1000.0, nSelfShadow, 100.0 * selfShadowNoEvTris / std::fmax( 1.0, totalTris ) );
	CHECK( true );
}

// LIT-CLASSIFIER HEADROOM. The GPU walk-bucket measurement (com_softShadowGate + r_softShadowWalkCounters)
// showed the per-fragment walk WORK is ~81-86% spent in LIT fragments (final mask 0) that walk ~90 caster
// triangles and block nothing - the 16x16 tile bin is too coarse to prove them lit, so they walk. The
// static-shadow win hinges on a WORLD-SPACE classifier that proves such fragments lit WITHOUT walking. This
// study measures the achievable CEILING of that classifier as a function of granularity: how much of the
// lit-work sits in world cells that are PURELY lit (no penumbra/umbra sample) - a conservative classifier at
// that cell size could skip exactly those. It mirrors the shader's cull (caster sphere -> per-triangle cone)
// so 'survivors' == the walk's work weight, then casts the 16 disk rays ONLY at survivors to classify
// lit/penumbra/umbra. Reports the win curve (cell size -> % of lit-work skippable) so we can tell whether a
// CHEAP (coarse) classifier captures the 86% or whether lit/penumbra interleave too finely to exploit.
//   env: LIT_N (disk samples/side, default 4 => 16 to match the shipped mask) | LIT_SPACING (receiver grid
//        spacing in world units, default 4) | LIT_MAXSAMP (sample cap per light, default 40000)
// Deterministic: fixed grid, no RNG. Run twice -> bit-identical.
STUDY_TEST( SoftShadowLitClassifier, headroom )
{
	const char* path = std::getenv( "CAP" );
	if( path == NULL ) { std::printf( "    [litclass] CAP unset; skipping\n" ); CHECK( true ); return; }
	Cap cap;
	if( !LoadCap( path, cap ) ) { std::printf( "    [litclass] cannot load %s\n", path ); CHECK( false ); return; }

	auto envI = []( const char* k, int d )   { const char* s = std::getenv( k ); return s ? std::atoi( s ) : d; };
	auto envF = []( const char* k, float d ) { const char* s = std::getenv( k ); return s ? ( float )std::atof( s ) : d; };
	const int   N       = envI( "LIT_N", 4 );				// disk samples/side (4 => 16, the shipped set size)
	const float SPACING = envF( "LIT_SPACING", 4.0f );		// receiver grid spacing (world units)
	const int   MAXSAMP = envI( "LIT_MAXSAMP", 40000 );		// sample cap per light (stride if exceeded)

	// per light: the caster spheres + tri ranges (all same-light casters), for the shader-mirror cull.
	struct CasterRef { float3 c; float rad; uint32_t first, num; };
	std::vector<std::vector<CasterRef>> lightCasters( cap.lights.size() );
	for( const capCaster_t& cs : cap.casters )
	{
		if( cs.lightIndex >= cap.lights.size() || cs.numIndex < 3 ) { continue; }
		float3 mn( 1e30f, 1e30f, 1e30f ), mx( -1e30f, -1e30f, -1e30f );
		for( uint32_t k = cs.firstVert; k < cs.firstVert + cs.numVerts; k++ )
		{
			float3 p( cap.meshVerts[k * 3], cap.meshVerts[k * 3 + 1], cap.meshVerts[k * 3 + 2] );
			mn = float3( std::fmin( mn.x, p.x ), std::fmin( mn.y, p.y ), std::fmin( mn.z, p.z ) );
			mx = float3( std::fmax( mx.x, p.x ), std::fmax( mx.y, p.y ), std::fmax( mx.z, p.z ) );
		}
		CasterRef cr;
		cr.c = ( mn + mx ) * 0.5f;
		cr.rad = length( mx - mn ) * 0.5f;
		cr.first = cs.firstIndex;
		cr.num = cs.numIndex;
		lightCasters[cs.lightIndex].push_back( cr );
	}

	// receiver sample points per light: grid-sample each receiver triangle at ~SPACING so cells down to
	// ~2*SPACING have several samples (a sparse cell could hide a penumbra sample -> optimistic skip; the
	// density guards against that). Deterministic barycentric lattice.
	std::vector<std::vector<float3>> lightPts( cap.lights.size() );
	std::vector<std::vector<float3>> lightNrm( cap.lights.size() );		// receiver surface normal per sample (SURFACE cache plane)
	for( const capReceiver_t& r : cap.receivers )
	{
		if( r.lightIndex >= cap.lights.size() ) { continue; }
		std::vector<float3>& dst = lightPts[r.lightIndex];
		std::vector<float3>& dstN = lightNrm[r.lightIndex];
		for( uint32_t t = 0; t < r.numIndex / 3; t++ )
		{
			const uint32_t i0 = cap.recvIdx[r.firstIndex + t * 3 + 0], i1 = cap.recvIdx[r.firstIndex + t * 3 + 1], i2 = cap.recvIdx[r.firstIndex + t * 3 + 2];
			float3 p0( cap.recvVerts[i0 * 3], cap.recvVerts[i0 * 3 + 1], cap.recvVerts[i0 * 3 + 2] );
			float3 p1( cap.recvVerts[i1 * 3], cap.recvVerts[i1 * 3 + 1], cap.recvVerts[i1 * 3 + 2] );
			float3 p2( cap.recvVerts[i2 * 3], cap.recvVerts[i2 * 3 + 1], cap.recvVerts[i2 * 3 + 2] );
			const float3 nrmT = normalize( cross( p1 - p0, p2 - p0 ) );
			const float e1 = length( p1 - p0 ), e2 = length( p2 - p0 );
			const int su = ( int )std::fmax( 1.0f, std::ceil( e1 / SPACING ) );
			const int sv = ( int )std::fmax( 1.0f, std::ceil( e2 / SPACING ) );
			for( int a = 0; a <= su; a++ )
				for( int b = 0; b <= sv; b++ )
				{
					float fu = ( float )a / su, fv = ( float )b / sv;
					if( fu + fv > 1.0f ) { continue; }
					dst.push_back( p0 + ( p1 - p0 ) * fu + ( p2 - p0 ) * fv );
					dstN.push_back( nrmT );
				}
		}
	}

	const float CELLS[] = { 2.0f, 4.0f, 8.0f, 16.0f, 32.0f, 64.0f, 128.0f };
	const int   NC = ( int )( sizeof( CELLS ) / sizeof( CELLS[0] ) );
	// per cell size: cell -> ( litWork, hasShadowSample, maxSurvivors ). A cell purely-lit (no pen/umb
	// sample) is skippable; maxSurv approximates the penumbra cell's cached static list size (memory).
	// covMin/covMax: the true coverage spread inside a cell = the resolution error a per-cell MASK/TERM
	// CACHE (option 3) would introduce - a fragment reads its cell's one cached value, so the worst error
	// is (covMax-covMin)/2 (representative = midpoint). workAll = total walk-work in the cell.
	struct CellAgg { double litWork = 0; int shadow = 0; int maxSurv = 0; float covMin = 1e30f; float covMax = -1e30f; double workAll = 0; };
	std::vector<std::unordered_map<uint64_t, CellAgg>> cellMap( NC );
	// per-light receiver world-AABB -> dense 3D cell count at each G (the dense-grid memory question).
	std::vector<float3> aabbMin( cap.lights.size(), float3( 1e30f, 1e30f, 1e30f ) ), aabbMax( cap.lights.size(), float3( -1e30f, -1e30f, -1e30f ) );
	for( size_t L = 0; L < cap.lights.size(); L++ )
		for( const float3& p : lightPts[L] )
		{
			aabbMin[L] = float3( std::fmin( aabbMin[L].x, p.x ), std::fmin( aabbMin[L].y, p.y ), std::fmin( aabbMin[L].z, p.z ) );
			aabbMax[L] = float3( std::fmax( aabbMax[L].x, p.x ), std::fmax( aabbMax[L].y, p.y ), std::fmax( aabbMax[L].z, p.z ) );
		}

	double totWork = 0, litWork = 0, penWork = 0, umbWork = 0;
	long   nLit = 0, nPen = 0, nUmb = 0, nSamp = 0;
	// NORMAL-AWARE cheap classifier (no ray test): a survivor tri entirely at/below the receiver tangent
	// plane cannot occlude a light above it (coplanar self-surface tris included) -> drop it; a fragment
	// with NO survivor rising above the plane is provably LIT. naLit = classified lit; naLitWrong = classed
	// lit but truth is penumbra/umbra (MUST be 0 for a lossless skip); naLitWork = walk work it would save.
	long   naLit = 0, naLitWrong = 0;  double naLitWork = 0;

	// ---- OFFLINE TEST-TIGHTNESS BAKE: candidate CONSERVATIVE lit/umbra tests vs the ray-truth class. ----
	// capture = of ray-truth-lit/umb WORK, how much each test PROVES (and thus would skip); FP = proves
	// lit/umbra where the ray truth says otherwise (must be ~0 - a light leak / over-dark). The cone test
	// (surv.empty()) is the shipped tile-bin cull; disk-projection projects each survivor onto the light
	// disk and proves lit iff none reaches it (m=0 per-point ceiling, or +cell inflation m).
	double coneLitCap = 0, diskLitCap0 = 0, diskLitCapC = 0, umbCapC = 0;
	double diskLitFP0 = 0, diskLitFPC = 0, umbFPC = 0;
	// OPTION 3' : scalar TERM cache + TRILINEAR interp. Store true coverage at grid VERTICES, interpolate
	// at the fragment. Error = |cov(P) - trilinear(8 corners)|, work-weighted, per cell size. Measured only
	// for the feasible range (indices TRI_LO..TRI_HI of CELLS) to bound the vertex-eval cost.
	const int TRI_LO = 1, TRI_HI = 5;			// 8u .. 64u
	double triW[7] = {0}, triDeg01[7] = {0}, triDeg10[7] = {0}, triErr[7] = {0};
	// SURFACE cache (planar projection + BILINEAR): the chosen substrate. Coverage sampled at texels on the
	// fragment's tangent plane (all ON the surface, where the field is smooth), bilinear-interpolated. Error
	// vs texel size settles the resolution/memory budget. Same TRI_LO..TRI_HI range.
	double surfW[7] = {0}, surfErr[7] = {0}, surfErr01[7] = {0}, surfErr10[7] = {0};
	// ELIMINATION targets + REUSE model (user direction 2026-08-19):
	// noEvWork = survivor-work spent on occluders that shadow NO receiver (pure cull waste).
	// cont* = walk-work by CONTINUOUS (high-N) coverage class: truly-lit fragments that still walk = a cull
	//         blunder; penumbra = genuine work.
	// coh* = occluder-SET overlap (Jaccard) between a penumbra point and a neighbour at world-offset COHD[k]
	//        = the reuse radius for an exact partial cache (cache the occluder set, re-evaluate exactly).
	double noEvWork = 0, contLitWork = 0, contPenWork = 0, contUmbWork = 0;
	const float COHD[5] = { 2, 4, 8, 16, 32 };
	double cohSum[5] = {0}; long cohN[5] = {0};
	// COMMON-CHUNK hypothesis (user 2026-08-19): per texel, skip the K common occluders and add a constant C
	// for their contribution; walk only the variable remainder. K = common-set size (skip fraction); the
	// crux is C's variation within a texel (measured at offset COHD[COMMON_K]). Reported at that offset.
	const int COMMON_K = 3;			// 16u texel
	double commonSkip = 0, commonVar = 0, commonVar01 = 0, commonVar10 = 0; long commonN = 0; double commonWsum = 0;
	// PER-OCCLUDER DERIVATIVE (refined chunk hypothesis): freeze occluders whose solo-coverage derivative
	// across the texel is below a threshold (they contribute ~constant), walk only the steep ones. Measures
	// the SKIPPABLE fraction at 3 thresholds x 3 texel sizes (finer texel -> flatter -> more skippable).
	const float DERIV_TEX[3] = { 8, 16, 32 };
	const float DERIV_THR[3] = { 0.02f, 0.05f, 0.10f };
	double derivSkip[3][3] = {{0}}; long derivOccTot[3] = {0};
	// LINEAR-PLANE model (user 2026-08-20): fold every occluder's LINEAR term into one per-texel plane
	// C + grad.offset; walk only occluders with significant SECOND derivative (curvature). Measures the
	// fraction FOLDABLE (2nd-diff below thr) = the ones we DON'T need to walk.
	const float SECOND_THR[3] = { 0.01f, 0.03f, 0.06f };
	double secondFold[3][3] = {{0}}; long secondTot[3] = {0};
	// SLIVER approximation: shrink the per-tri cone by FRAC (drop edge-clipping survivors = thin slivers),
	// setting those near-lit fragments toward 1. Measure walk-work saved vs the coverage error it introduces.
	const float SLIVFRAC[3] = { 0.10f, 0.20f, 0.30f };
	double slivSaved[3] = {0}, slivErr[3] = {0}, slivErr10[3] = {0};
	const float testTR = 0.5f * ( float )envI( "LIT_TESTCELL", 16 );	// cell inflation half-extent for the tight tests
	struct v2 { float x, y; };
	auto segD = []( v2 p, v2 a, v2 b ) -> float {
		float abx = b.x - a.x, aby = b.y - a.y, apx = p.x - a.x, apy = p.y - a.y;
		float tt = ( apx * abx + apy * aby ) / std::fmax( abx * abx + aby * aby, 1e-12f );
		tt = std::fmin( 1.0f, std::fmax( 0.0f, tt ) );
		float cx = a.x + abx * tt - p.x, cy = a.y + aby * tt - p.y;
		return std::sqrt( cx * cx + cy * cy );
	};
	auto inTri = []( v2 p, v2 a, v2 b, v2 c ) -> bool {
		auto sg = []( v2 p1, v2 p2, v2 p3 ) { return ( p1.x - p3.x ) * ( p2.y - p3.y ) - ( p2.x - p3.x ) * ( p1.y - p3.y ); };
		float d1 = sg( p, a, b ), d2 = sg( p, b, c ), d3 = sg( p, c, a );
		bool ng = ( d1 < 0 ) || ( d2 < 0 ) || ( d3 < 0 ), ps = ( d1 > 0 ) || ( d2 > 0 ) || ( d3 > 0 );
		return !( ng && ps );
	};
	auto ptTriD = [&]( v2 p, v2 a, v2 b, v2 c ) -> float {
		if( inTri( p, a, b, c ) ) { return 0.0f; }
		return std::fmin( segD( p, a, b ), std::fmin( segD( p, b, c ), segD( p, c, a ) ) );
	};

	for( size_t L = 0; L < cap.lights.size(); L++ )
	{
		const std::vector<CasterRef>& cast = lightCasters[L];
		std::vector<float3>& pts = lightPts[L];
		if( cast.empty() || pts.empty() ) { continue; }
		const float3 Lp( cap.lights[L].origin[0], cap.lights[L].origin[1], cap.lights[L].origin[2] );
		const float  swR = std::fmax( cap.lights[L].penumbraSize, 1e-2f );
		const int stride = ( int )std::fmax( 1.0, std::ceil( ( double )pts.size() / MAXSAMP ) );
		const float* rv = cap.meshVerts.data();
		std::vector<uint32_t> surv;								// survivor tri indices (reused per sample)
		std::vector<uint32_t> survT[3];							// sliver-tightened survivor subsets (reused)

		// per-caster NO-EVIDENCE flag: does this caster shadow ANY of the light's receiver points (solo)?
		// A no-evidence caster's tris still get walked (cone reaches disks) but block nothing = pure cull waste.
		// Cone-precull each receiver (only soup the few the caster could reach) to keep this tractable.
		std::vector<char> noEv( cast.size(), 1 );
		{
			const int rstr = ( int )std::fmax( 1.0, pts.size() / 400.0 );
			std::vector<uint32_t> solo;
			for( size_t ci = 0; ci < cast.size(); ci++ )
			{
				const CasterRef& cr = cast[ci];
				solo.clear();
				for( uint32_t t = 0; t < cr.num; t++ ) { solo.push_back( cap.meshIdx[cr.first + t] ); }
				for( size_t ri = 0; ri < pts.size() && noEv[ci]; ri += rstr )
				{
					const float3 Q = pts[ri]; float3 tl = Lp - Q; float dp = std::fmax( length( tl ), 1e-4f ); float3 nr = tl * ( 1.0f / dp );
					float3 rc = cr.c - Q; float cd = dot( rc, nr );
					if( cd + cr.rad < 1e-3f || cd - cr.rad > dp ) { continue; }
					float3 pp = rc - nr * cd; float cR = swR * ( cd + cr.rad ) / dp;
					if( dot( pp, pp ) > ( cR + cr.rad ) * ( cR + cr.rad ) ) { continue; }
					if( MeshTruthShadowSoup( rv, solo.data(), ( uint32_t )solo.size(), Q, Lp, swR, N ) > 1e-4f ) { noEv[ci] = 0; }
				}
			}
		}

		// coverage at an ARBITRARY world point (cull survivors + disk-ray truth) - for grid-VERTEX evals (3').
		std::vector<uint32_t> vsv;
		auto coverageAt = [&]( float3 Q ) -> float {
			float3 tl = Lp - Q; float dp = std::fmax( length( tl ), 1e-4f ); float3 nr = tl * ( 1.0f / dp );
			const float ep = 1e-3f;
			vsv.clear();
			for( const CasterRef& cr : cast )
			{
				float3 rc = cr.c - Q; float cdc = dot( rc, nr );
				if( cdc + cr.rad < ep || cdc - cr.rad > dp ) { continue; }
				float3 pp = rc - nr * cdc; float cR = swR * ( cdc + cr.rad ) / dp;
				if( dot( pp, pp ) > ( cR + cr.rad ) * ( cR + cr.rad ) ) { continue; }
				for( uint32_t t = 0; t < cr.num / 3; t++ )
				{
					const uint32_t j0 = cap.meshIdx[cr.first + t * 3 + 0], j1 = cap.meshIdx[cr.first + t * 3 + 1], j2 = cap.meshIdx[cr.first + t * 3 + 2];
					float3 a( rv[j0 * 3], rv[j0 * 3 + 1], rv[j0 * 3 + 2] ), bb( rv[j1 * 3], rv[j1 * 3 + 1], rv[j1 * 3 + 2] ), cc( rv[j2 * 3], rv[j2 * 3 + 1], rv[j2 * 3 + 2] );
					float3 tc = ( a + bb + cc ) * ( 1.0f / 3.0f ); float trd = std::fmax( length( a - tc ), std::fmax( length( bb - tc ), length( cc - tc ) ) );
					float3 r2 = tc - Q; float cd2 = dot( r2, nr );
					if( cd2 + trd < ep || cd2 - trd > dp ) { continue; }
					float3 p2 = r2 - nr * cd2; float cr2 = swR * ( cd2 + trd ) / dp;
					if( dot( p2, p2 ) > ( cr2 + trd ) * ( cr2 + trd ) ) { continue; }
					vsv.push_back( j0 ); vsv.push_back( j1 ); vsv.push_back( j2 );
				}
			}
			return vsv.empty() ? 0.0f : MeshTruthShadowSoup( rv, vsv.data(), ( uint32_t )vsv.size(), Q, Lp, swR, N );
		};
		std::unordered_map<uint64_t, float> vmemo[7];		// per cell-size vertex-coverage cache (this light)
		auto vkey = []( int ix, int iy, int iz ) -> uint64_t {
			return ( ( uint64_t )( uint32_t )ix * 0x9E3779B1u ) ^ ( ( uint64_t )( uint32_t )iy * 0x85EBCA77u ) ^ ( ( uint64_t )( uint32_t )iz * 0xC2B2AE3Du );
		};
		auto getVCov = [&]( int g, int ix, int iy, int iz ) -> float {
			const uint64_t k = vkey( ix, iy, iz );
			auto it = vmemo[g].find( k );
			if( it != vmemo[g].end() ) { return it->second; }
			const float c = coverageAt( float3( ix * CELLS[g], iy * CELLS[g], iz * CELLS[g] ) );
			vmemo[g][k] = c; return c;
		};
		// occluder-SET (survivor tri ids) a fragment at Q would walk - for neighbour coherence.
		auto survAt = [&]( float3 Q, std::vector<uint32_t>& out ) {
			out.clear();
			float3 tl = Lp - Q; float dp = std::fmax( length( tl ), 1e-4f ); float3 nr = tl * ( 1.0f / dp );
			const float ep = 1e-3f;
			for( const CasterRef& cr : cast )
			{
				float3 rc = cr.c - Q; float cd = dot( rc, nr );
				if( cd + cr.rad < ep || cd - cr.rad > dp ) { continue; }
				float3 pp = rc - nr * cd; float cR = swR * ( cd + cr.rad ) / dp;
				if( dot( pp, pp ) > ( cR + cr.rad ) * ( cR + cr.rad ) ) { continue; }
				for( uint32_t t = 0; t < cr.num / 3; t++ )
				{
					const uint32_t j0 = cap.meshIdx[cr.first + t * 3 + 0], j1 = cap.meshIdx[cr.first + t * 3 + 1], j2 = cap.meshIdx[cr.first + t * 3 + 2];
					float3 a( rv[j0 * 3], rv[j0 * 3 + 1], rv[j0 * 3 + 2] ), bb( rv[j1 * 3], rv[j1 * 3 + 1], rv[j1 * 3 + 2] ), cc( rv[j2 * 3], rv[j2 * 3 + 1], rv[j2 * 3 + 2] );
					float3 tc = ( a + bb + cc ) * ( 1.0f / 3.0f ); float trd = std::fmax( length( a - tc ), std::fmax( length( bb - tc ), length( cc - tc ) ) );
					float3 r2 = tc - Q; float cd2 = dot( r2, nr );
					if( cd2 + trd < ep || cd2 - trd > dp ) { continue; }
					float3 p2 = r2 - nr * cd2; float cr2 = swR * ( cd2 + trd ) / dp;
					if( dot( p2, p2 ) > ( cr2 + trd ) * ( cr2 + trd ) ) { continue; }
					out.push_back( cr.first + t * 3 );		// triangle id
				}
			}
			std::sort( out.begin(), out.end() );
		};
		std::vector<uint32_t> cohA, cohB;		// reused for the coherence probes
		// SURFACE-cache texel coverage, memoized by quantized world position (adjacent samples share texels).
		std::unordered_map<uint64_t, float> surfMemo;
		auto surfCov = [&]( float3 Q ) -> float {
			const int64_t qx = ( int64_t )std::floor( Q.x * 2.0f ), qy = ( int64_t )std::floor( Q.y * 2.0f ), qz = ( int64_t )std::floor( Q.z * 2.0f );	// 0.5u grid
			const uint64_t k = ( uint64_t )( qx * 73856093 ) ^ ( uint64_t )( qy * 19349663 ) ^ ( uint64_t )( qz * 83492791 );
			auto it = surfMemo.find( k );
			if( it != surfMemo.end() ) { return it->second; }
			const float c = coverageAt( Q );
			surfMemo[k] = c; return c;
		};

		for( size_t pi = 0; pi < pts.size(); pi += stride )
		{
			const float3 P = pts[pi];
			float3 toL = Lp - P;
			float  distPL = std::fmax( length( toL ), 1e-4f );
			float3 nrm = toL * ( 1.0f / distPL );
			const float eps = 1e-3f;

			// shader-mirror cull: caster sphere, then per-triangle centroid cone. survivors = walk work.
			surv.clear();
			survT[0].clear(); survT[1].clear(); survT[2].clear();
			int noEvSurv = 0;
			for( size_t ci = 0; ci < cast.size(); ci++ )
			{
				const CasterRef& cr = cast[ci];
				float3 rc = cr.c - P;
				float  cd = dot( rc, nrm );
				if( cd + cr.rad < eps || cd - cr.rad > distPL ) { continue; }
				float3 perp = rc - nrm * cd;
				float  coneR = swR * ( cd + cr.rad ) / distPL;
				if( dot( perp, perp ) > ( coneR + cr.rad ) * ( coneR + cr.rad ) ) { continue; }
				for( uint32_t t = 0; t < cr.num / 3; t++ )
				{
					const uint32_t j0 = cap.meshIdx[cr.first + t * 3 + 0], j1 = cap.meshIdx[cr.first + t * 3 + 1], j2 = cap.meshIdx[cr.first + t * 3 + 2];
					float3 a( rv[j0 * 3], rv[j0 * 3 + 1], rv[j0 * 3 + 2] ), bb( rv[j1 * 3], rv[j1 * 3 + 1], rv[j1 * 3 + 2] ), cc( rv[j2 * 3], rv[j2 * 3 + 1], rv[j2 * 3 + 2] );
					float3 tcen = ( a + bb + cc ) * ( 1.0f / 3.0f );
					float  triRad = std::fmax( length( a - tcen ), std::fmax( length( bb - tcen ), length( cc - tcen ) ) );
					float3 rc2 = tcen - P;
					float  cd2 = dot( rc2, nrm );
					if( cd2 + triRad < eps || cd2 - triRad > distPL ) { continue; }
					float3 pp2 = rc2 - nrm * cd2;
					float  cr2 = swR * ( cd2 + triRad ) / distPL;
					if( dot( pp2, pp2 ) > ( cr2 + triRad ) * ( cr2 + triRad ) ) { continue; }
					surv.push_back( j0 ); surv.push_back( j1 ); surv.push_back( j2 );
					if( noEv[ci] ) { noEvSurv++; }
				}
			}
			const double work = ( double )( surv.size() / 3 );		// survivors reaching MT = the per-fragment cost
			// classify by disk coverage over the survivors only (bit-identical occlusion to the full set).
			float cov = surv.empty() ? 0.0f : MeshTruthShadowSoup( rv, surv.data(), ( uint32_t )surv.size(), P, Lp, swR, N );
			const int cls = ( cov < 1e-4f ) ? 0 : ( ( cov > 1.0f - 1e-4f ) ? 2 : 1 );	// 0 lit, 1 penumbra, 2 umbra

			// SLIVER approximation (by ACTUAL disk-overlap): project each survivor onto the light disk; a
			// survivor whose projection only grazes the RIM (dist-to-centre near swR) clips a thin sliver.
			// Drop survivors reaching less than (1-FRAC)*swR into the disk. saved = survivors dropped (walk-
			// work removed); err = coverage change (the real visual cost). Straddlers kept (conservative).
			{
				const float3 su = normalize( cross( nrm, ( std::fabs( nrm.z ) < 0.9f ) ? float3( 0, 0, 1 ) : float3( 1, 0, 0 ) ) );
				const float3 sv = cross( nrm, su );
				survT[0].clear(); survT[1].clear(); survT[2].clear();
				for( size_t s = 0; s < surv.size(); s += 3 )
				{
					v2 q[3]; bool ok = true;
					for( int m = 0; m < 3; m++ )
					{
						const uint32_t jj = surv[s + m]; float3 vv( rv[jj * 3], rv[jj * 3 + 1], rv[jj * 3 + 2] );
						float3 d = vv - P; float dn = dot( d, nrm );
						if( dn <= 1e-3f ) { ok = false; break; }
						float3 q3 = P + d * ( distPL / dn ); q[m] = { dot( q3 - Lp, su ), dot( q3 - Lp, sv ) };
					}
					const float dd = ok ? ptTriD( { 0, 0 }, q[0], q[1], q[2] ) : 0.0f;	// dist disk-centre->tri
					for( int k = 0; k < 3; k++ )
					{
						if( dd <= swR * ( 1.0f - SLIVFRAC[k] ) ) { survT[k].push_back( surv[s] ); survT[k].push_back( surv[s + 1] ); survT[k].push_back( surv[s + 2] ); }
					}
				}
				for( int k = 0; k < 3; k++ )
				{
					const float covT = survT[k].empty() ? 0.0f : MeshTruthShadowSoup( rv, survT[k].data(), ( uint32_t )survT[k].size(), P, Lp, swR, N );
					slivSaved[k] += ( double )( ( surv.size() - survT[k].size() ) / 3 );
					const double err = std::fabs( ( double )cov - covT );
					slivErr[k] += err * work;
					if( err > 0.10 ) { slivErr10[k] += work; }
				}
			}

			// NORMAL-AWARE cheap classify (measure-first for the surface classifier): drop survivors whose
			// whole tri sits at/below the receiver tangent plane; if none rise above, provably lit (sound).
			{
				const float3 sfN = lightNrm[L][pi];
				bool anyAbove = false;
				for( size_t s = 0; s < surv.size() && !anyAbove; s += 3 )
				{
					float mx = -1e30f;
					for( int m = 0; m < 3; m++ ) { const uint32_t jj = surv[s + m]; float3 vv( rv[jj * 3], rv[jj * 3 + 1], rv[jj * 3 + 2] ); mx = std::fmax( mx, dot( vv - P, sfN ) ); }
					if( mx > 0.05f ) { anyAbove = true; }
				}
				if( !anyAbove ) { naLit++; naLitWork += work; if( cls != 0 ) { naLitWrong++; } }
			}

			totWork += work;
			if( cls == 0 ) { litWork += work; nLit++; }
			else if( cls == 1 ) { penWork += work; nPen++; }
			else { umbWork += work; nUmb++; }
			nSamp++;

			// ---- ELIMINATION targets + REUSE model ----
			noEvWork += noEvSurv;								// survivor-work on occluders that shadow nothing
			if( work > 0 )										// continuous (64-ray) class of this WALKING fragment
			{
				const float covHi = MeshTruthShadowSoup( rv, surv.data(), ( uint32_t )surv.size(), P, Lp, swR, 8 );
				if( covHi < 1e-3f ) { contLitWork += work; }			// truly lit yet walked = cull blunder
				else if( covHi > 1.0f - 1e-3f ) { contUmbWork += work; }
				else { contPenWork += work; }							// genuine penumbra work
			}
			if( cls == 1 )										// occluder-SET coherence at a penumbra fragment
			{
				const float3 uax = normalize( cross( nrm, ( std::fabs( nrm.z ) < 0.9f ) ? float3( 0, 0, 1 ) : float3( 1, 0, 0 ) ) );
				survAt( P, cohA );
				if( !cohA.empty() )
				{
					for( int k = 0; k < 5; k++ )
					{
						survAt( P + uax * COHD[k], cohB );
						size_t i = 0, j = 0, inter = 0;
						while( i < cohA.size() && j < cohB.size() )
						{
							if( cohA[i] == cohB[j] ) { inter++; i++; j++; }
							else if( cohA[i] < cohB[j] ) { i++; }
							else { j++; }
						}
						const size_t uni = cohA.size() + cohB.size() - inter;
						if( uni > 0 ) { cohSum[k] += ( double )inter / uni; cohN[k]++; }

						if( k == COMMON_K )		// COMMON-CHUNK: does the common set's contribution stay constant?
						{
							std::vector<uint32_t> commonTid;
							{
								size_t a = 0, b = 0;
								while( a < cohA.size() && b < cohB.size() )
								{
									if( cohA[a] == cohB[b] ) { commonTid.push_back( cohA[a] ); a++; b++; }
									else if( cohA[a] < cohB[b] ) { a++; }
									else { b++; }
								}
							}
							if( !commonTid.empty() && !cohA.empty() )
							{
								std::vector<uint32_t> cidx; cidx.reserve( commonTid.size() * 3 );
								for( uint32_t tid : commonTid )
								{
									cidx.push_back( cap.meshIdx[tid] ); cidx.push_back( cap.meshIdx[tid + 1] ); cidx.push_back( cap.meshIdx[tid + 2] );
								}
								const float ccP = MeshTruthShadowSoup( rv, cidx.data(), ( uint32_t )cidx.size(), P, Lp, swR, N );
								const float ccN = MeshTruthShadowSoup( rv, cidx.data(), ( uint32_t )cidx.size(), P + uax * COHD[k], Lp, swR, N );
								const double var = std::fabs( ( double )ccP - ccN );
								commonSkip += ( double )commonTid.size() / cohA.size();
								commonVar += var * work; commonWsum += work;
								if( var > 0.01 ) { commonVar01 += work; }
								if( var > 0.10 ) { commonVar10 += work; }
								commonN++;
							}
						}
					}
				}
			}

			// ---- PER-OCCLUDER FREEZE: how many survivors have a STABLE blocked-SAMPLE set across a texel ----
			// (solo COVERAGE can be constant while the blocked samples SHIFT - the shift is what builds the
			// union gradient. So the honest metric is the solo MASK's Hamming change, not the coverage change.)
			if( cls == 1 && ( pi % 8 == 0 ) )		// strided: per-survivor solo masks are costly
			{
				const float3 dua = normalize( cross( nrm, ( std::fabs( nrm.z ) < 0.9f ) ? float3( 0, 0, 1 ) : float3( 1, 0, 0 ) ) );
				const int NM = 8;
				auto soloMask = [&]( const uint32_t * tri, float3 Q ) -> uint64_t {
					float3 tL = Lp - Q; float d = std::sqrt( dot( tL, tL ) ); if( d < 1e-6f ) { return 0; }
					float3 nr = tL * ( 1.0f / d );
					float3 up = ( std::fabs( nr.z ) > 0.9f ) ? float3( 0, 1, 0 ) : float3( 0, 0, 1 );
					float3 mu = normalize( cross( up, nr ) ), mv = cross( nr, mu );
					uint64_t m = 0;
					for( int iy = 0; iy < NM; iy++ )
						for( int ix = 0; ix < NM; ix++ )
						{
							float du = ( ix + 0.5f ) / NM * 2 - 1, dv = ( iy + 0.5f ) / NM * 2 - 1;
							if( du * du + dv * dv > 1.0f ) { continue; }
							float3 Dp = Lp + mu * ( du * swR ) + mv * ( dv * swR );
							if( RayHitsMesh( Q, Dp - Q, rv, tri, 3 ) ) { m |= ( uint64_t )1 << ( iy * NM + ix ); }
						}
					return m;
				};
				for( int tx = 0; tx < 3; tx++ )
				{
					const float3 Q = P + dua * DERIV_TEX[tx];
					for( size_t s = 0; s + 2 < surv.size(); s += 3 )
					{
						uint32_t tri[3] = { surv[s], surv[s + 1], surv[s + 2] };
						const uint64_t mP = soloMask( tri, P ), mQ = soloMask( tri, Q );
						const int ham = __builtin_popcountll( mP ^ mQ );		// samples that changed
						derivOccTot[tx]++;
						if( ham == 0 ) { derivSkip[tx][0] += 1.0; }			// mask IDENTICAL -> freeze losslessly
						if( ham <= 1 ) { derivSkip[tx][1] += 1.0; }			// <=1 sample shifted
						if( ham <= 3 ) { derivSkip[tx][2] += 1.0; }			// <=3 shifted
					}
				}
				for( int tx = 0; tx < 3; tx++ )		// LINEAR-PLANE: per-occluder 2nd derivative (curvature) of solo coverage
				{
					const float3 Qm = P - dua * ( DERIV_TEX[tx] * 0.5f ), Qp = P + dua * ( DERIV_TEX[tx] * 0.5f );
					for( size_t s = 0; s + 2 < surv.size(); s += 3 )
					{
						uint32_t tri[3] = { surv[s], surv[s + 1], surv[s + 2] };
						const float cm = MeshTruthShadowSoup( rv, tri, 3, Qm, Lp, swR, 8 );
						const float c0 = MeshTruthShadowSoup( rv, tri, 3, P, Lp, swR, 8 );
						const float cp = MeshTruthShadowSoup( rv, tri, 3, Qp, Lp, swR, 8 );
						const float d2 = std::fabs( cm - 2.0f * c0 + cp );		// discrete 2nd derivative -> foldable if small
						secondTot[tx]++;
						for( int th = 0; th < 3; th++ ) { if( d2 < SECOND_THR[th] ) { secondFold[tx][th] += 1.0; } }
					}
				}
			}

			// ---- OPTION 3' : trilinear TERM-cache error at this fragment (feasible cell sizes) ----
			for( int g = TRI_LO; g <= TRI_HI; g++ )
			{
				const float G = CELLS[g];
				const int ix = ( int )std::floor( P.x / G ), iy = ( int )std::floor( P.y / G ), iz = ( int )std::floor( P.z / G );
				const float fx = P.x / G - ix, fy = P.y / G - iy, fz = P.z / G - iz;
				const float c000 = getVCov( g, ix, iy, iz ),       c100 = getVCov( g, ix + 1, iy, iz );
				const float c010 = getVCov( g, ix, iy + 1, iz ),   c110 = getVCov( g, ix + 1, iy + 1, iz );
				const float c001 = getVCov( g, ix, iy, iz + 1 ),   c101 = getVCov( g, ix + 1, iy, iz + 1 );
				const float c011 = getVCov( g, ix, iy + 1, iz + 1 ), c111 = getVCov( g, ix + 1, iy + 1, iz + 1 );
				const float c00 = c000 * ( 1 - fx ) + c100 * fx, c10 = c010 * ( 1 - fx ) + c110 * fx;
				const float c01 = c001 * ( 1 - fx ) + c101 * fx, c11 = c011 * ( 1 - fx ) + c111 * fx;
				const float c0 = c00 * ( 1 - fy ) + c10 * fy, c1 = c01 * ( 1 - fy ) + c11 * fy;
				const float ci = c0 * ( 1 - fz ) + c1 * fz;
				const double err = std::fabs( ( double )cov - ci );
				triW[g] += work; triErr[g] += err * work;
				if( err > 0.01 ) { triDeg01[g] += work; }
				if( err > 0.10 ) { triDeg10[g] += work; }
			}

			// ---- SURFACE cache (planar projection + BILINEAR) error at this fragment ----
			{
				const float3 nR = lightNrm[L][pi];
				const float3 st1 = normalize( cross( nR, ( std::fabs( nR.z ) < 0.9f ) ? float3( 0, 0, 1 ) : float3( 1, 0, 0 ) ) );
				const float3 st2 = cross( nR, st1 );
				const float su0 = dot( P, st1 ), sv0 = dot( P, st2 );
				for( int g = TRI_LO; g <= TRI_HI; g++ )
				{
					const float G = CELLS[g];
					const int iu = ( int )std::floor( su0 / G ), iv = ( int )std::floor( sv0 / G );
					const float fu = su0 / G - iu, fv = sv0 / G - iv;
					auto corner = [&]( int cu, int cv ) -> float3 { return P + st1 * ( cu * G - su0 ) + st2 * ( cv * G - sv0 ); };
					const float c00 = surfCov( corner( iu, iv ) ),     c10 = surfCov( corner( iu + 1, iv ) );
					const float c01 = surfCov( corner( iu, iv + 1 ) ), c11 = surfCov( corner( iu + 1, iv + 1 ) );
					const float ci = ( c00 * ( 1 - fu ) + c10 * fu ) * ( 1 - fv ) + ( c01 * ( 1 - fu ) + c11 * fu ) * fv;
					const double err = std::fabs( ( double )cov - ci );
					surfW[g] += work; surfErr[g] += err * work;
					if( err > 0.01 ) { surfErr01[g] += work; }
					if( err > 0.10 ) { surfErr10[g] += work; }
				}
			}

			// ---- candidate conservative tests over the survivors (disk basis perpendicular to nrm) ----
			const float3 uax = normalize( cross( nrm, ( std::fabs( nrm.z ) < 0.9f ) ? float3( 0, 0, 1 ) : float3( 1, 0, 0 ) ) );
			const float3 vax = cross( nrm, uax );
			// project a survivor tri onto the disk plane (through P), returning its 2D coords + min depth;
			// bad = a vertex not strictly toward the light (tri may straddle P) -> caller treats as blocking.
			auto projTri = [&]( size_t s, v2 q[3], float& mindn ) -> bool {
				mindn = 1e30f;
				for( int k = 0; k < 3; k++ )
				{
					const uint32_t j = surv[s + k];
					const float3 vv( rv[j * 3], rv[j * 3 + 1], rv[j * 3 + 2] );
					const float3 d = vv - P; const float dn = dot( d, nrm );
					if( dn <= 1e-3f ) { return false; }
					const float3 q3 = P + d * ( distPL / dn );
					q[k] = { dot( q3 - Lp, uax ), dot( q3 - Lp, vax ) };
					mindn = std::fmin( mindn, dn );
				}
				return true;
			};
			const bool straddleSkip = envI( "LIT_STRADDLE_SKIP", 0 ) != 0;	// diagnostic: skip straddlers (UNSAFE, ceiling only)
			auto diskLit = [&]( float m ) -> bool {
				for( size_t s = 0; s < surv.size(); s += 3 )
				{
					v2 q[3]; float mindn;
					if( !projTri( s, q, mindn ) ) { if( straddleSkip ) { continue; } return false; }	// straddles P -> conservatively blocks
					const float mm = m * distPL / std::fmax( mindn, 1e-3f );	// cell radius projected onto the disk
					if( ptTriD( { 0, 0 }, q[0], q[1], q[2] ) < swR + mm ) { return false; }	// tri reaches the disk
				}
				return true;
			};
			auto diskUmb = [&]( float m ) -> bool {
				for( size_t s = 0; s < surv.size(); s += 3 )
				{
					v2 q[3]; float mindn;
					if( !projTri( s, q, mindn ) ) { continue; }
					const float mm = m * distPL / std::fmax( mindn, 1e-3f );
					if( inTri( { 0, 0 }, q[0], q[1], q[2] ) && ptTriD( { 0, 0 }, q[0], q[1], q[2] ) >= swR + mm ) { return true; }
				}
				return false;
			};
			const bool coneLit = surv.empty();
			const bool dLit0 = coneLit || diskLit( 0.0f );
			const bool dLitC = coneLit || diskLit( testTR );
			const bool uUmbC = diskUmb( testTR );
			if( cls == 0 )		// ray-truth LIT: capture
			{
				if( coneLit ) { coneLitCap += work; }
				if( dLit0 )   { diskLitCap0 += work; }
				if( dLitC )   { diskLitCapC += work; }
			}
			else				// not lit: a firing lit test is a FALSE POSITIVE
			{
				if( dLit0 ) { diskLitFP0 += work; }
				if( dLitC ) { diskLitFPC += work; }
			}
			if( cls == 2 ) { if( uUmbC ) { umbCapC += work; } }		// ray-truth UMBRA: capture
			else { if( uUmbC ) { umbFPC += work; } }				// not umbra: FALSE POSITIVE

			for( int g = 0; g < NC; g++ )
			{
				const float G = CELLS[g];
				const int64_t cx = ( int64_t )std::floor( P.x / G ), cy = ( int64_t )std::floor( P.y / G ), cz = ( int64_t )std::floor( P.z / G );
				uint64_t key = ( uint64_t )( cx * 73856093 ) ^ ( uint64_t )( cy * 19349663 ) ^ ( uint64_t )( cz * 83492791 ) ^ ( ( uint64_t )L << 1 );
				auto& e = cellMap[g][key];
				if( cls == 0 ) { e.litWork += work; }
				else { e.shadow = 1; }			// this cell touches penumbra/umbra -> NOT purely lit
				if( ( int )work > e.maxSurv ) { e.maxSurv = ( int )work; }
				e.covMin = std::fmin( e.covMin, cov );
				e.covMax = std::fmax( e.covMax, cov );
				e.workAll += work;
			}
		}
	}

	std::printf( "    [litclass] %s: %ld samples (%ld lit / %ld pen / %ld umb) | work-share lit/pen/umb %.0f%%/%.0f%%/%.0f%% (spacing %.0f, N=%d)\n",
				 cap.mapName.empty() ? path : cap.mapName.c_str(), nSamp, nLit, nPen, nUmb,
				 totWork > 0 ? 100.0 * litWork / totWork : 0.0, totWork > 0 ? 100.0 * penWork / totWork : 0.0, totWork > 0 ? 100.0 * umbWork / totWork : 0.0,
				 ( double )SPACING, N );
	std::printf( "    [litclass] NORMAL-AWARE surface classify (no ray test, sound): lit %ld/%ld samples (%.0f%% of lit-truth captured) | removes %.0f%% of WHOLE walk | UNSOUND(classed-lit-but-shadowed) %ld <- MUST be 0\n",
				 naLit, nLit, nLit > 0 ? 100.0 * ( naLit - naLitWrong ) / nLit : 0.0,
				 totWork > 0 ? 100.0 * naLitWork / totWork : 0.0, naLitWrong );
	std::printf( "    [litclass] WORLD-CELL lit short-circuit ceiling (purely-lit cells; conservative classifier at that grain):\n" );
	for( int g = 0; g < NC; g++ )
	{
		double skip = 0;
		for( auto& kv : cellMap[g] ) { if( kv.second.shadow == 0 ) { skip += kv.second.litWork; } }
		std::printf( "      cell %4.0fu:  captures %.0f%% of lit-work  =>  %.0f%% of the WHOLE walk removed  (umbra short-circuit adds %.0f%%)\n",
					 ( double )CELLS[g],
					 litWork > 0 ? 100.0 * skip / litWork : 0.0,
					 totWork > 0 ? 100.0 * skip / totWork : 0.0,
					 totWork > 0 ? 100.0 * umbWork / totWork : 0.0 );
	}
	// DENSE-GRID MEMORY (the dense-vs-hash decision): class-byte array over each light's receiver-AABB +
	// penumbra cells' cached static lists (~maxSurv tris x 4B/index). Reports total across all lights.
	std::printf( "    [litclass] DENSE-GRID memory (class byte/cell over receiver-AABB + penumbra list ~maxSurv*4B):\n" );
	for( int g = 0; g < NC; g++ )
	{
		const float G = CELLS[g];
		double denseCells = 0;
		for( size_t L = 0; L < cap.lights.size(); L++ )
		{
			if( lightPts[L].empty() ) { continue; }
			double nx = std::floor( ( aabbMax[L].x - aabbMin[L].x ) / G ) + 1, ny = std::floor( ( aabbMax[L].y - aabbMin[L].y ) / G ) + 1, nz = std::floor( ( aabbMax[L].z - aabbMin[L].z ) / G ) + 1;
			denseCells += nx * ny * nz;
		}
		long occ = 0, pen = 0; double listBytes = 0;
		for( auto& kv : cellMap[g] ) { occ++; if( kv.second.shadow ) { pen++; listBytes += ( double )kv.second.maxSurv * 3.0 * 4.0; } }
		std::printf( "      cell %4.0fu:  dense %.1fM cells (%.1f MB class) | occupied %ld, penumbra %ld (%.1f MB lists) | TOTAL %.1f MB\n",
					 ( double )G, denseCells / 1e6, denseCells / 1e6, occ, pen, listBytes / 1e6, denseCells / 1e6 + listBytes / 1e6 );
	}
	std::printf( "    [litclass] MASK/TERM CACHE (option 3) resolution error - the cache stores one term/cell,\n"
				 "               so a fragment's worst error is (covMax-covMin)/2 in its cell. Work-fraction it degrades:\n" );
	for( int g = 0; g < NC; g++ )
	{
		double wTot = 0, w01 = 0, w10 = 0, spreadW = 0;
		for( auto& kv : cellMap[g] )
		{
			if( kv.second.covMax < kv.second.covMin ) { continue; }
			const double half = 0.5 * ( kv.second.covMax - kv.second.covMin );
			wTot += kv.second.workAll;
			spreadW += half * kv.second.workAll;
			if( half > 0.01 ) { w01 += kv.second.workAll; }
			if( half > 0.10 ) { w10 += kv.second.workAll; }
		}
		std::printf( "      cell %4.0fu:  %5.1f%% of walk-work degrades >0.01, %5.1f%% >0.1 | mean cache error %.3f\n",
					 ( double )CELLS[g], wTot > 0 ? 100.0 * w01 / wTot : 0.0, wTot > 0 ? 100.0 * w10 / wTot : 0.0,
					 wTot > 0 ? spreadW / wTot : 0.0 );
	}
	std::printf( "    [litclass] TERM CACHE + TRILINEAR (option 3') error - store true coverage at grid vertices,\n"
				 "               interpolate at the fragment. Work-fraction degraded (vs the nearest-neighbour table above):\n" );
	for( int g = TRI_LO; g <= TRI_HI; g++ )
	{
		std::printf( "      cell %4.0fu:  %5.1f%% of walk-work degrades >0.01, %5.1f%% >0.1 | mean error %.4f\n",
					 ( double )CELLS[g], triW[g] > 0 ? 100.0 * triDeg01[g] / triW[g] : 0.0,
					 triW[g] > 0 ? 100.0 * triDeg10[g] / triW[g] : 0.0, triW[g] > 0 ? triErr[g] / triW[g] : 0.0 );
	}
	std::printf( "    [litclass] ELIMINATION targets (lossless, no cache):\n" );
	std::printf( "      no-evidence occluders : %5.1f%% of walk-work (survivors from casters that shadow NO receiver)\n",
				 totWork > 0 ? 100.0 * noEvWork / totWork : 0.0 );
	std::printf( "      continuous(64-ray) class of WALKING work: truly-lit %5.1f%% | penumbra %5.1f%% | umbra %5.1f%%\n",
				 totWork > 0 ? 100.0 * contLitWork / totWork : 0.0, totWork > 0 ? 100.0 * contPenWork / totWork : 0.0,
				 totWork > 0 ? 100.0 * contUmbWork / totWork : 0.0 );
	std::printf( "    [litclass] OCCLUDER-SET COHERENCE between penumbra neighbours (Jaccard of survivor sets = reuse radius):\n" );
	for( int k = 0; k < 5; k++ )
	{
		std::printf( "      offset %3.0fu:  %.2f mean overlap  (%ld pairs)\n",
					 ( double )COHD[k], cohN[k] > 0 ? cohSum[k] / cohN[k] : 0.0, cohN[k] );
	}
	std::printf( "    [litclass] COMMON-CHUNK @%.0fu texel (skip K common occluders + add constant C, walk the variable rest):\n",
				 ( double )COHD[COMMON_K] );
	std::printf( "      common = %.0f%% of occluders (the SKIP fraction) | C variation across texel: mean %.4f | %.1f%% work >0.01, %.1f%% >0.1\n",
				 commonN > 0 ? 100.0 * commonSkip / commonN : 0.0, commonWsum > 0 ? commonVar / commonWsum : 0.0,
				 commonWsum > 0 ? 100.0 * commonVar01 / commonWsum : 0.0, commonWsum > 0 ? 100.0 * commonVar10 / commonWsum : 0.0 );
	std::printf( "    [litclass] PER-OCCLUDER FREEZE (%% of survivors whose solo blocked-SAMPLE mask stays stable across the texel):\n" );
	for( int tx = 0; tx < 3; tx++ )
	{
		std::printf( "      texel %2.0fu:  mask IDENTICAL %.0f%% | <=1 sample shifted %.0f%% | <=3 shifted %.0f%%\n",
					 ( double )DERIV_TEX[tx],
					 derivOccTot[tx] > 0 ? 100.0 * derivSkip[tx][0] / derivOccTot[tx] : 0.0,
					 derivOccTot[tx] > 0 ? 100.0 * derivSkip[tx][1] / derivOccTot[tx] : 0.0,
					 derivOccTot[tx] > 0 ? 100.0 * derivSkip[tx][2] / derivOccTot[tx] : 0.0 );
	}
	std::printf( "    [litclass] LINEAR-PLANE FOLD (%% of survivors LINEAR enough to fold into the per-texel plane; walk only the rest):\n" );
	for( int tx = 0; tx < 3; tx++ )
	{
		std::printf( "      texel %2.0fu:  foldable @2nd<0.01 %.0f%% | <0.03 %.0f%% | <0.06 %.0f%%\n",
					 ( double )DERIV_TEX[tx],
					 secondTot[tx] > 0 ? 100.0 * secondFold[tx][0] / secondTot[tx] : 0.0,
					 secondTot[tx] > 0 ? 100.0 * secondFold[tx][1] / secondTot[tx] : 0.0,
					 secondTot[tx] > 0 ? 100.0 * secondFold[tx][2] / secondTot[tx] : 0.0 );
	}
	std::printf( "    [litclass] SURFACE CACHE (planar projection + bilinear) error - the CHOSEN substrate; error vs texel size:\n" );
	for( int g = TRI_LO; g <= TRI_HI; g++ )
	{
		std::printf( "      texel %4.0fu:  %5.1f%% of work degrades >0.01, %5.1f%% >0.1 | mean error %.4f\n",
					 ( double )CELLS[g], surfW[g] > 0 ? 100.0 * surfErr01[g] / surfW[g] : 0.0,
					 surfW[g] > 0 ? 100.0 * surfErr10[g] / surfW[g] : 0.0, surfW[g] > 0 ? surfErr[g] / surfW[g] : 0.0 );
	}
	std::printf( "    [litclass] SLIVER approximation (drop survivors reaching <(1-x)*swR into the disk = rim grazers):\n" );
	for( int k = 0; k < 3; k++ )
	{
		std::printf( "      drop rim %2.0f%%:  walk-work saved %5.1f%% | mean coverage err %.4f | %5.1f%% of work errs >0.1\n",
					 100.0 * SLIVFRAC[k], totWork > 0 ? 100.0 * slivSaved[k] / totWork : 0.0,
					 totWork > 0 ? slivErr[k] / totWork : 0.0, totWork > 0 ? 100.0 * slivErr10[k] / totWork : 0.0 );
	}
	std::printf( "    [litclass] CONSERVATIVE TEST tightness (capture of ray-truth WORK; FP must be ~0):\n" );
	std::printf( "      cone (tile-bin cull)    : %5.1f%% of lit-work  (the shipped cull - upper bound of a cone classifier)\n",
				 litWork > 0 ? 100.0 * coneLitCap / litWork : 0.0 );
	std::printf( "      disk-proj m=0 (ceiling) : %5.1f%% of lit-work | FP %.2f%% of walk\n",
				 litWork > 0 ? 100.0 * diskLitCap0 / litWork : 0.0, totWork > 0 ? 100.0 * diskLitFP0 / totWork : 0.0 );
	std::printf( "      disk-proj cell %-3d      : %5.1f%% of lit-work | FP %.2f%% of walk\n",
				 ( int )( 2 * testTR ), litWork > 0 ? 100.0 * diskLitCapC / litWork : 0.0, totWork > 0 ? 100.0 * diskLitFPC / totWork : 0.0 );
	std::printf( "      umbra 1-tri cell %-3d    : %5.1f%% of umbra-work | FP %.2f%% of walk\n",
				 ( int )( 2 * testTR ), umbWork > 0 ? 100.0 * umbCapC / umbWork : 0.0, totWork > 0 ? 100.0 * umbFPC / totWork : 0.0 );
	std::printf( "      COMBINED (disk-lit cell + umbra) removes %.0f%% of the WHOLE walk\n",
				 totWork > 0 ? 100.0 * ( diskLitCapC + umbCapC ) / totWork : 0.0 );
	CHECK( true );
}

// PROXY FALLOFF QUANTIFICATION. The gate is a thresholded defect detector (0 defects != box==RT). This
// measures the ACTUAL intensity difference the box proxy introduces vs the RT oracle: for exactly the
// casters the ENGINE boxes (r_softShadowProxyBox: >64 tris AND maxGap/diag < BOXGAP), it compares
// box-proxy coverage against high-sample ray-truth on the TRUE mesh (= the RT oracle) over the penumbra,
// and reports the error DISTRIBUTION and WHERE across the penumbra gradient it diverges + the signed bias
// (does the box over- or under-shadow). Coverage is fraction-of-disk-blocked = 1 lit .. 0 umbra, so
// |box-mesh| IS the intensity error at that receiver.
//
// Run:  CAP=/path/foo.cap ./rbdoom3bfg_tests @study:SoftShadowProxyFalloff
//   env: BOXGAP (box-ness thresh, default 0.06 = the engine cvar) | PROXY_N (disk samples/side, default 64)
STUDY_TEST( SoftShadowProxyFalloff, quantify )
{
	const char* path = std::getenv( "CAP" );
	if( path == NULL ) { std::printf( "    [falloff] CAP unset; skipping\n" ); CHECK( true ); return; }
	Cap cap;
	if( !LoadCap( path, cap ) ) { std::printf( "    [falloff] cannot load %s\n", path ); CHECK( false ); return; }

	auto envF = []( const char* k, float d ) { const char* s = std::getenv( k ); return s ? ( float )std::atof( s ) : d; };
	auto envI = []( const char* k, int d )   { const char* s = std::getenv( k ); return s ? std::atoi( s ) : d; };
	const float BOXGAP = envF( "BOXGAP", 0.06f );		// must match r_softShadowProxyBoxGap
	const int   N      = envI( "PROXY_N", 64 );			// HIGH-sample RT oracle
	const int   MINTRIS = 64;							// must match SW_PROXY_MIN_TRIS

	// per-light receiver-centroid P set
	std::vector<std::vector<float3>> lightRecv( cap.lights.size() );
	for( const capReceiver_t& r : cap.receivers )
	{
		if( r.lightIndex >= cap.lights.size() ) { continue; }
		const uint32_t tc = r.numIndex / 3;
		for( uint32_t t = 0; t < tc; t++ )
		{
			const uint32_t i0 = cap.recvIdx[r.firstIndex + t * 3 + 0], i1 = cap.recvIdx[r.firstIndex + t * 3 + 1], i2 = cap.recvIdx[r.firstIndex + t * 3 + 2];
			float3 p0( cap.recvVerts[i0 * 3], cap.recvVerts[i0 * 3 + 1], cap.recvVerts[i0 * 3 + 2] );
			float3 p1( cap.recvVerts[i1 * 3], cap.recvVerts[i1 * 3 + 1], cap.recvVerts[i1 * 3 + 2] );
			float3 p2( cap.recvVerts[i2 * 3], cap.recvVerts[i2 * 3 + 1], cap.recvVerts[i2 * 3 + 2] );
			lightRecv[r.lightIndex].push_back( ( p0 + p1 + p2 ) * ( 1.0f / 3.0f ) );
		}
	}

	const int NB = 10;					// penumbra bins by mesh coverage [0,1)
	double binAbs[NB] = {}, binSgn[NB] = {}, binMesh[NB] = {}, binBox[NB] = {}; long binN[NB] = {};
	double sumSq = 0, sumAbs = 0, sumSgn = 0, maxAbs = 0; long nSamp = 0, nGt05 = 0, nGt10 = 0;
	long nBoxed = 0; double boxedTris = 0;

	for( uint32_t c = 0; c < cap.casters.size(); c++ )
	{
		const capCaster_t& cs = cap.casters[c];
		const int realT = ( int )( cs.numIndex / 3 );
		if( realT < MINTRIS || cs.lightIndex >= cap.lights.size() ) { continue; }
		const std::vector<float3>& recv = lightRecv[cs.lightIndex];
		if( recv.empty() ) { continue; }

		// box-ness test IDENTICAL to the engine (Interaction.cpp R_CollectPenumbraFaces)
		float3 mn( 1e30f, 1e30f, 1e30f ), mx( -1e30f, -1e30f, -1e30f );
		for( uint32_t k = cs.firstVert; k < cs.firstVert + cs.numVerts; k++ )
		{
			float3 p( cap.meshVerts[k * 3], cap.meshVerts[k * 3 + 1], cap.meshVerts[k * 3 + 2] );
			mn = float3( std::fmin( mn.x, p.x ), std::fmin( mn.y, p.y ), std::fmin( mn.z, p.z ) );
			mx = float3( std::fmax( mx.x, p.x ), std::fmax( mx.y, p.y ), std::fmax( mx.z, p.z ) );
		}
		float3 ext = mx - mn; float diag = std::sqrt( dot( ext, ext ) );
		if( diag <= 1e-3f ) { continue; }
		float maxGap = 0.0f;
		for( uint32_t k = cs.firstVert; k < cs.firstVert + cs.numVerts; k++ )
		{
			float3 p( cap.meshVerts[k * 3], cap.meshVerts[k * 3 + 1], cap.meshVerts[k * 3 + 2] );
			float g = std::fmin( std::fmin( std::fmin( p.x - mn.x, mx.x - p.x ), std::fmin( p.y - mn.y, mx.y - p.y ) ), std::fmin( p.z - mn.z, mx.z - p.z ) );
			maxGap = std::fmax( maxGap, g );
		}
		if( maxGap >= BOXGAP * diag ) { continue; }		// engine would NOT box this caster
		nBoxed++; boxedTris += realT;

		ProxyMesh box = MakeBox( mn, mx );
		const float3 Lp( cap.lights[cs.lightIndex].origin[0], cap.lights[cs.lightIndex].origin[1], cap.lights[cs.lightIndex].origin[2] );
		const float  swR = std::fmax( cap.lights[cs.lightIndex].penumbraSize, 1e-2f );
		const float* rv = cap.meshVerts.data();
		const uint32_t* ri = &cap.meshIdx[cs.firstIndex];
		const size_t stride = std::max( ( size_t )1, recv.size() / 200 );
		for( size_t s = 0; s < recv.size(); s += stride )
		{
			const float3 P = recv[s];
			float meshCov = MeshTruthShadowSoup( rv, ri, cs.numIndex, P, Lp, swR, N );		// RT oracle (true mesh)
			if( meshCov > 0.999f ) { continue; }										// fully lit: not this caster's penumbra
			float boxCov = MeshTruthShadowSoup( box.v.data(), box.idx.data(), ( uint32_t )box.idx.size(), P, Lp, swR, N );
			double d = boxCov - meshCov;
			int b = ( int )( meshCov * NB ); if( b < 0 ) { b = 0; } if( b >= NB ) { b = NB - 1; }
			binAbs[b] += std::fabs( d ); binSgn[b] += d; binMesh[b] += meshCov; binBox[b] += boxCov; binN[b]++;
			sumAbs += std::fabs( d ); sumSgn += d; sumSq += d * d; maxAbs = std::fmax( maxAbs, std::fabs( d ) );
			nSamp++; if( std::fabs( d ) > 0.05 ) { nGt05++; } if( std::fabs( d ) > 0.10 ) { nGt10++; }
		}
	}

	if( nSamp == 0 ) { std::printf( "    [falloff] no boxed casters with penumbra in this capture\n" ); CHECK( true ); return; }
	std::printf( "    [falloff] boxed %ld casters (%.0fk tris), %ld penumbra samples, N=%d disk/side\n", nBoxed, boxedTris / 1000.0, nSamp, N );
	std::printf( "    [falloff] ERROR |box - RT|:  mean %.4f  RMS %.4f  max %.4f  | signed bias %+.4f (>0 = box OVER-shadows)\n",
				 sumAbs / nSamp, std::sqrt( sumSq / nSamp ), maxAbs, sumSgn / nSamp );
	std::printf( "    [falloff] tail:  %.1f%% of penumbra samples exceed 0.05 (>1/16 quantum),  %.1f%% exceed 0.10\n",
				 100.0 * nGt05 / nSamp, 100.0 * nGt10 / nSamp );
	std::printf( "    [falloff] across the penumbra gradient (mesh coverage bin -> mean box, mesh, |err|, signed):\n" );
	for( int b = 0; b < NB; b++ )
	{
		if( binN[b] == 0 ) { continue; }
		std::printf( "      meshCov [%.1f,%.1f)  n%6ld  box %.3f  RT %.3f  |err| %.4f  signed %+.4f\n",
					 b * 0.1, b * 0.1 + 0.1, binN[b], binBox[b] / binN[b], binMesh[b] / binN[b], binAbs[b] / binN[b], binSgn[b] / binN[b] );
	}
	CHECK( true );
}

// PRIMITIVE POPULATION SIZING. Measure-first for the "represent a box/cylinder caster as ONE analytic
// record instead of N triangles" direction. The walk's dominant cost is per-record list iteration, so a
// genuine box (yaw-DOF OBB) or cylinder/pipe (axis+radius+extent, its light-silhouette is a rect) collapses
// to 1 walk record - EXACT for a real primitive, so no over-shadow. This fits BOTH to every caster (incl.
// the small ones the box-proxy test excluded - genuine 12-tri boxes live there), falloff-gates each against
// the true mesh (ray-truth) so only GENUINE primitives count, and reports the walk-RECORD fraction each
// captures + the reduction under "1 record per proxied primitive". Answers: is there enough genuine
// box+cylinder geometry to matter, or is the whole primitive direction marginal?  Coarse N (population, not
// precise falloff).  Run: CAP=... ./rbdoom3bfg_tests @study:SoftShadowPrimitivePop
STUDY_TEST( SoftShadowPrimitivePop, sizing )
{
	const char* path = std::getenv( "CAP" );
	if( path == NULL ) { std::printf( "    [primpop] CAP unset; skipping\n" ); CHECK( true ); return; }
	Cap cap;
	if( !LoadCap( path, cap ) ) { std::printf( "    [primpop] cannot load %s\n", path ); CHECK( false ); return; }
	auto envF = []( const char* k, float d ) { const char* s = std::getenv( k ); return s ? ( float )std::atof( s ) : d; };
	auto envI = []( const char* k, int d )   { const char* s = std::getenv( k ); return s ? std::atoi( s ) : d; };
	const float TOL = envF( "PROXY_TOL", 0.05f );
	const int   N   = envI( "PROXY_N", 8 );		// coarse: population, not precise falloff
	const int   RCAP = 60;

	std::vector<std::vector<float3>> lightRecv( cap.lights.size() );
	for( const capReceiver_t& r : cap.receivers )
	{
		if( r.lightIndex >= cap.lights.size() ) { continue; }
		const uint32_t tc = r.numIndex / 3;
		for( uint32_t t = 0; t < tc; t++ )
		{
			const uint32_t i0 = cap.recvIdx[r.firstIndex + t * 3 + 0], i1 = cap.recvIdx[r.firstIndex + t * 3 + 1], i2 = cap.recvIdx[r.firstIndex + t * 3 + 2];
			float3 p0( cap.recvVerts[i0 * 3], cap.recvVerts[i0 * 3 + 1], cap.recvVerts[i0 * 3 + 2] );
			float3 p1( cap.recvVerts[i1 * 3], cap.recvVerts[i1 * 3 + 1], cap.recvVerts[i1 * 3 + 2] );
			float3 p2( cap.recvVerts[i2 * 3], cap.recvVerts[i2 * 3 + 1], cap.recvVerts[i2 * 3 + 2] );
			lightRecv[r.lightIndex].push_back( ( p0 + p1 + p2 ) * ( 1.0f / 3.0f ) );
		}
	}

	double totRec = 0, recBox = 0, recCyl = 0, recKeep = 0, recNoPen = 0, primRecords = 0;
	long nBox = 0, nCyl = 0, nKeep = 0, nNoPen = 0, nCas = 0;

	for( uint32_t c = 0; c < cap.casters.size(); c++ )
	{
		const capCaster_t& cs = cap.casters[c];
		const int realT = ( int )( cs.numIndex / 3 );
		if( realT <= 0 || cs.lightIndex >= cap.lights.size() ) { continue; }
		totRec += realT; nCas++;
		const std::vector<float3>& recv = lightRecv[cs.lightIndex];
		if( recv.empty() ) { recNoPen += realT; nNoPen++; primRecords += realT; continue; }

		float3 mn( 1e30f, 1e30f, 1e30f ), mx( -1e30f, -1e30f, -1e30f ), sum( 0, 0, 0 );
		for( uint32_t k = cs.firstVert; k < cs.firstVert + cs.numVerts; k++ )
		{
			float3 p( cap.meshVerts[k * 3], cap.meshVerts[k * 3 + 1], cap.meshVerts[k * 3 + 2] );
			mn = float3( std::fmin( mn.x, p.x ), std::fmin( mn.y, p.y ), std::fmin( mn.z, p.z ) );
			mx = float3( std::fmax( mx.x, p.x ), std::fmax( mx.y, p.y ), std::fmax( mx.z, p.z ) );
			sum = sum + p;
		}
		float3 ctr = sum * ( 1.0f / std::fmax( 1.0f, ( float )cs.numVerts ) );

		const float3 Lp( cap.lights[cs.lightIndex].origin[0], cap.lights[cs.lightIndex].origin[1], cap.lights[cs.lightIndex].origin[2] );
		const float  swR = std::fmax( cap.lights[cs.lightIndex].penumbraSize, 1e-2f );
		const float* rv = cap.meshVerts.data();
		const uint32_t* ri = &cap.meshIdx[cs.firstIndex];
		const size_t stride = std::max( ( size_t )1, recv.size() / RCAP );

		// penumbra receivers + their mesh-truth coverage (the RT oracle), computed ONCE
		std::vector<std::pair<float3, float>> pen;
		for( size_t s = 0; s < recv.size(); s += stride )
		{
			float mc = MeshTruthShadowSoup( rv, ri, cs.numIndex, recv[s], Lp, swR, N );
			if( mc < 0.999f ) { pen.push_back( { recv[s], mc } ); }
		}
		if( pen.empty() ) { recNoPen += realT; nNoPen++; primRecords += realT; continue; }

		auto falloff = [&]( const ProxyMesh& pm ) -> float
		{
			if( pm.idx.empty() ) { return 1e30f; }
			double se = 0; for( auto& pr : pen ) { se += std::fabs( MeshTruthShadowSoup( pm.v.data(), pm.idx.data(), ( uint32_t )pm.idx.size(), pr.first, Lp, swR, N ) - pr.second ); }
			return ( float )( se / pen.size() );
		};

		// FIT BOX: yaw-DOF OBB (Z world-up; one face flat kills tilt). Min-error over yaw samples.
		float bestBox = 1e30f;
		for( int a = 0; a < 8; a++ )
		{
			float th = ( float )( a * ( 3.14159265 / 2.0 ) / 8.0 );
			float3 u( std::cos( th ), std::sin( th ), 0 ), v( -std::sin( th ), std::cos( th ), 0 ), w( 0, 0, 1 );
			float umn = 1e30f, umx = -1e30f, vmn = 1e30f, vmx = -1e30f, wmn = 1e30f, wmx = -1e30f;
			for( uint32_t k = cs.firstVert; k < cs.firstVert + cs.numVerts; k++ )
			{
				float3 p( cap.meshVerts[k * 3] - ctr.x, cap.meshVerts[k * 3 + 1] - ctr.y, cap.meshVerts[k * 3 + 2] - ctr.z );
				float du = dot( p, u ), dv = dot( p, v ), dw = dot( p, w );
				umn = std::fmin( umn, du ); umx = std::fmax( umx, du ); vmn = std::fmin( vmn, dv ); vmx = std::fmax( vmx, dv ); wmn = std::fmin( wmn, dw ); wmx = std::fmax( wmx, dw );
			}
			float3 bc( ctr.x + u.x * ( umn + umx ) * 0.5f + v.x * ( vmn + vmx ) * 0.5f, ctr.y + u.y * ( umn + umx ) * 0.5f + v.y * ( vmn + vmx ) * 0.5f, ctr.z + ( wmn + wmx ) * 0.5f );
			float3 he( ( umx - umn ) * 0.5f, ( vmx - vmn ) * 0.5f, ( wmx - wmn ) * 0.5f );
			bestBox = std::fmin( bestBox, falloff( MakeOBB( bc, u, v, w, he ) ) );
		}

		// FIT CYLINDER: candidate axes = world X/Y/Z + AABB-longest. radius/extent from projection.
		float3 axes[4] = { float3( 1, 0, 0 ), float3( 0, 1, 0 ), float3( 0, 0, 1 ), float3( 0, 0, 1 ) };
		{ float3 e = mx - mn; axes[3] = ( e.x >= e.y && e.x >= e.z ) ? float3( 1, 0, 0 ) : ( ( e.y >= e.z ) ? float3( 0, 1, 0 ) : float3( 0, 0, 1 ) ); }
		float bestCyl = 1e30f;
		for( int ai = 0; ai < 4; ai++ )
		{
			float3 d = axes[ai];
			float tmn = 1e30f, tmx = -1e30f, rr = 0;
			for( uint32_t k = cs.firstVert; k < cs.firstVert + cs.numVerts; k++ )
			{
				float3 p( cap.meshVerts[k * 3] - ctr.x, cap.meshVerts[k * 3 + 1] - ctr.y, cap.meshVerts[k * 3 + 2] - ctr.z );
				float t = dot( p, d ); tmn = std::fmin( tmn, t ); tmx = std::fmax( tmx, t );
				float3 perp = p - d * t; rr = std::fmax( rr, std::sqrt( dot( perp, perp ) ) );
			}
			float3 p0( ctr.x + d.x * tmn, ctr.y + d.y * tmn, ctr.z + d.z * tmn ), p1( ctr.x + d.x * tmx, ctr.y + d.y * tmx, ctr.z + d.z * tmx );
			bestCyl = std::fmin( bestCyl, falloff( MakeCylinderAxis( p0, p1, rr, 16 ) ) );
		}

		const float best = std::fmin( bestBox, bestCyl );
		if( best <= TOL )
		{
			primRecords += 1;		// ONE analytic record replaces the whole caster
			if( bestBox <= bestCyl ) { nBox++; recBox += realT; }
			else { nCyl++; recCyl += realT; }
		}
		else { primRecords += realT; nKeep++; recKeep += realT; }
	}

	std::printf( "    [primpop] ===== %ld casters, %.0fk total walk records (TOL %.2f, N %d) =====\n", nCas, totRec / 1000.0, TOL, N );
	std::printf( "      GENUINE BOX      : %ld casters, %.0f%% of records (%.0fk tris)\n", nBox, 100.0 * recBox / std::fmax( 1.0, totRec ), recBox / 1000.0 );
	std::printf( "      GENUINE CYLINDER : %ld casters, %.0f%% of records (%.0fk tris)\n", nCyl, 100.0 * recCyl / std::fmax( 1.0, totRec ), recCyl / 1000.0 );
	std::printf( "      keep-mesh        : %ld casters, %.0f%% of records | no-penumbra (unjudged) %ld, %.0f%%\n",
				 nKeep, 100.0 * recKeep / std::fmax( 1.0, totRec ), nNoPen, 100.0 * recNoPen / std::fmax( 1.0, totRec ) );
	std::printf( "      RECORD REDUCTION : %.0fk -> %.0fk records (%.1fx fewer) under 1-record-per-primitive; box+cyl capture %.0f%% of records\n",
				 totRec / 1000.0, primRecords / 1000.0, totRec / std::fmax( 1.0, primRecords ), 100.0 * ( recBox + recCyl ) / std::fmax( 1.0, totRec ) );
	CHECK( true );
}

// OCCLUDER-UMBRA CULL validation. The user's mechanism: per light, process occluders LARGEST-FIRST; an
// occluder fully inside the accumulated UMBRA of already-included larger occluders casts nothing (the
// region is already fully dark) and is NOT included. Geometric (bounding-sphere umbra cone), no PCSS - so
// a pebble in another caster's PENUMBRA (not umbra) is correctly KEPT. This simulates the cull and checks
// SAFETY against ray-truth: a culled caster MUST actually shadow no receiver (else it's an unsafe cull =
// under-shadow the gate would flag). Sweep UMBRA_SHRINK (safety factor on the occluder radius) to find the
// yield at 0 unsafe culls.  Run: CAP=... UMBRA_SHRINK=1.0 ./rbdoom3bfg_tests @study:SoftShadowOccluderCull
STUDY_TEST( SoftShadowOccluderCull, validate )
{
	const char* path = std::getenv( "CAP" );
	if( path == NULL ) { std::printf( "    [umbracull] CAP unset; skipping\n" ); CHECK( true ); return; }
	Cap cap;
	if( !LoadCap( path, cap ) ) { std::printf( "    [umbracull] cannot load %s\n", path ); CHECK( false ); return; }
	auto envF = []( const char* k, float d ) { const char* s = std::getenv( k ); return s ? ( float )std::atof( s ) : d; };
	const float SHRINK = envF( "UMBRA_SHRINK", 1.0f );
	const int N = 8;

	// per-light receiver centroids
	std::vector<std::vector<float3>> lightRecv( cap.lights.size() );
	for( const capReceiver_t& r : cap.receivers )
	{
		if( r.lightIndex >= cap.lights.size() ) { continue; }
		const uint32_t tc = r.numIndex / 3;
		for( uint32_t t = 0; t < tc; t++ )
		{
			const uint32_t i0 = cap.recvIdx[r.firstIndex + t * 3 + 0], i1 = cap.recvIdx[r.firstIndex + t * 3 + 1], i2 = cap.recvIdx[r.firstIndex + t * 3 + 2];
			float3 p0( cap.recvVerts[i0 * 3], cap.recvVerts[i0 * 3 + 1], cap.recvVerts[i0 * 3 + 2] );
			float3 p1( cap.recvVerts[i1 * 3], cap.recvVerts[i1 * 3 + 1], cap.recvVerts[i1 * 3 + 2] );
			float3 p2( cap.recvVerts[i2 * 3], cap.recvVerts[i2 * 3 + 1], cap.recvVerts[i2 * 3 + 2] );
			lightRecv[r.lightIndex].push_back( ( p0 + p1 + p2 ) * ( 1.0f / 3.0f ) );
		}
	}

	struct Cas { uint32_t idx; float3 c; float r; int tris; };
	struct Plane { float3 n; float d; };		// umbra half-space: dot(n,P) + d <= 0

	// OBJECT UMBRA from the SHIPPED wedge-PS math (softwedge.ps.hlsl:108-135). Per silhouette edge, the
	// plane through the edge and the light CENTRE (N = cross(E1-E0, L-E0)) is the point-light boundary
	// (t=0, the MIDDLE of the penumbra - "the silhouette bears no relation"). The UMBRA is t <= -1, i.e.
	// dP <= -halfWidth, halfWidth = R*distER/distLE -> LINEAR in P, so a genuine plane:
	//   dot(P-E0, N) + (R/distLE)*dot(P-edgeMid, toShadow) <= 0.
	// The object's umbra = behind ALL its silhouette edges' umbra planes (conservative for concave: extra
	// edges only shrink it). SHRINK scales R (>1 = smaller/safer umbra).
	auto buildObjUmbra = [&]( const capCaster_t& C, float3 Lp, float R ) -> std::vector<Plane>
	{
		const uint32_t* ri = &cap.meshIdx[C.firstIndex];
		struct EAcc { float3 a, b; int nF, nB; };
		std::unordered_map<uint64_t, EAcc> em;
		for( uint32_t i = 0; i + 2 < C.numIndex; i += 3 )
		{
			const uint32_t ia = ri[i], ib = ri[i + 1], ic = ri[i + 2];
			float3 v0( cap.meshVerts[ia * 3], cap.meshVerts[ia * 3 + 1], cap.meshVerts[ia * 3 + 2] );
			float3 v1( cap.meshVerts[ib * 3], cap.meshVerts[ib * 3 + 1], cap.meshVerts[ib * 3 + 2] );
			float3 v2( cap.meshVerts[ic * 3], cap.meshVerts[ic * 3 + 1], cap.meshVerts[ic * 3 + 2] );
			float3 nrm = cross( v1 - v0, v2 - v0 ); float nl = std::sqrt( dot( nrm, nrm ) ); if( nl < 1e-9f ) { continue; } nrm = nrm * ( 1.0f / nl );
			float3 cen = ( v0 + v1 + v2 ) * ( 1.0f / 3.0f );
			bool front = dot( nrm, Lp - cen ) > 0.0f;			// this triangle faces the light
			float3 tv[3] = { v0, v1, v2 };
			for( int e = 0; e < 3; e++ )
			{
				float3 pa = tv[e], pb = tv[( e + 1 ) % 3];
				uint64_t key = SoftEdgeKey( pa, pb );
				auto it = em.find( key );
				if( it == em.end() ) { EAcc ea; ea.nF = front ? 1 : 0; ea.nB = front ? 0 : 1; if( front ) { ea.a = pa; ea.b = pb; } else { ea.a = pb; ea.b = pa; } em[key] = ea; }
				else { if( front ) { it->second.nF++; it->second.a = pa; it->second.b = pb; } else { it->second.nB++; } }	// prefer front winding
			}
		}
		std::vector<Plane> planes;
		for( auto& kv : em )
		{
			const EAcc& e = kv.second;
			if( !( ( e.nF > 0 && e.nB > 0 ) || ( e.nF + e.nB == 1 ) ) ) { continue; }		// silhouette (facing-XOR) or open boundary
			float3 E0 = e.a, E1 = e.b, edge = E1 - E0;
			float3 N = cross( edge, Lp - E0 ); float Nl = std::sqrt( dot( N, N ) ); if( Nl < 1e-6f ) { continue; } N = N * ( 1.0f / Nl );
			float3 edgeMid = ( E0 + E1 ) * 0.5f, toShadow = edgeMid - Lp; float dLE = std::sqrt( dot( toShadow, toShadow ) ); if( dLE < 1e-4f ) { continue; }
			toShadow = toShadow * ( 1.0f / dLE ); dLE = std::fmax( dLE, R );
			float k = R / dLE;
			float3 nun = N + toShadow * k; float dconst = dot( E0, N ) + k * dot( edgeMid, toShadow );
			float ln = std::sqrt( dot( nun, nun ) ); if( ln < 1e-6f ) { continue; }
			planes.push_back( { nun * ( 1.0f / ln ), -dconst / ln } );
		}
		return planes;
	};
	auto inUmbra = [&]( float3 C, float rT, const std::vector<Plane>& pl ) -> bool
	{
		if( pl.empty() ) { return false; }
		for( const Plane& p : pl ) { if( dot( p.n, C ) + p.d + rT > 0.0f ) { return false; } }		// sphere not fully behind this umbra plane
		return true;
	};

	const int MOCC = 96;		// build umbra planes for the M largest occluders per light; test casters against them
	double totRec = 0, culledRec = 0, unsafeRec = 0; long nCull = 0, nUnsafe = 0, nCas = 0;
	for( uint32_t li = 0; li < cap.lights.size(); li++ )
	{
		std::vector<Cas> cs;
		for( uint32_t c = 0; c < cap.casters.size(); c++ )
		{
			if( cap.casters[c].lightIndex != li ) { continue; }
			const capCaster_t& C = cap.casters[c];
			int t = ( int )( C.numIndex / 3 ); if( t <= 0 ) { continue; }
			float3 mn( 1e30f, 1e30f, 1e30f ), mx( -1e30f, -1e30f, -1e30f );
			for( uint32_t k = C.firstVert; k < C.firstVert + C.numVerts; k++ )
			{
				float3 p( cap.meshVerts[k * 3], cap.meshVerts[k * 3 + 1], cap.meshVerts[k * 3 + 2] );
				mn = float3( std::fmin( mn.x, p.x ), std::fmin( mn.y, p.y ), std::fmin( mn.z, p.z ) );
				mx = float3( std::fmax( mx.x, p.x ), std::fmax( mx.y, p.y ), std::fmax( mx.z, p.z ) );
			}
			cs.push_back( { c, ( mn + mx ) * 0.5f, std::sqrt( dot( mx - mn, mx - mn ) ) * 0.5f, t } );
		}
		if( cs.empty() ) { continue; }
		const float3 Lp( cap.lights[li].origin[0], cap.lights[li].origin[1], cap.lights[li].origin[2] );
		const float  R = std::fmax( cap.lights[li].penumbraSize, 1e-2f ) * SHRINK;
		// LARGEST occluders first (biggest angular size = biggest umbra)
		std::sort( cs.begin(), cs.end(), [&]( const Cas& a, const Cas& b )
		{
			float da = std::fmax( std::sqrt( dot( Lp - a.c, Lp - a.c ) ), 1e-3f ), db = std::fmax( std::sqrt( dot( Lp - b.c, Lp - b.c ) ), 1e-3f );
			return a.r / da > b.r / db;
		} );
		std::vector<std::pair<uint32_t, std::vector<Plane>>> occ;	// (owner, umbra planes) for the M largest
		for( int m = 0; m < ( int )cs.size() && m < MOCC; m++ ) { occ.push_back( { cs[m].idx, buildObjUmbra( cap.casters[cs[m].idx], Lp, R ) } ); }

		const float swRtrue = std::fmax( cap.lights[li].penumbraSize, 1e-2f );
		// PER-TRIANGLE: cull each triangle fully inside some larger occluder's umbra volume.
		for( const Cas& D : cs )
		{
			nCas++;
			const uint32_t* ri = &cap.meshIdx[cap.casters[D.idx].firstIndex];
			for( uint32_t i = 0; i + 2 < cap.casters[D.idx].numIndex; i += 3 )
			{
				totRec += 1;
				float3 v0( cap.meshVerts[ri[i] * 3], cap.meshVerts[ri[i] * 3 + 1], cap.meshVerts[ri[i] * 3 + 2] );
				float3 v1( cap.meshVerts[ri[i + 1] * 3], cap.meshVerts[ri[i + 1] * 3 + 1], cap.meshVerts[ri[i + 1] * 3 + 2] );
				float3 v2( cap.meshVerts[ri[i + 2] * 3], cap.meshVerts[ri[i + 2] * 3 + 1], cap.meshVerts[ri[i + 2] * 3 + 2] );
				float3 tc = ( v0 + v1 + v2 ) * ( 1.0f / 3.0f );
				float tr = std::sqrt( std::fmax( dot( v0 - tc, v0 - tc ), std::fmax( dot( v1 - tc, v1 - tc ), dot( v2 - tc, v2 - tc ) ) ) );
				int certifier = -1;
				for( int m = 0; m < ( int )occ.size(); m++ ) { if( occ[m].first != D.idx && inUmbra( tc, tr, occ[m].second ) ) { certifier = m; break; } }
				if( certifier < 0 ) { continue; }
				nCull++; culledRec += 1;
				// SAFETY: the certifying occluder's TRUE ray-cast umbra must actually contain the triangle centroid.
				const capCaster_t& A = cap.casters[occ[certifier].first];
				if( MeshTruthShadowSoup( cap.meshVerts.data(), &cap.meshIdx[A.firstIndex], A.numIndex, tc, Lp, swRtrue, N ) > 0.01f ) { nUnsafe++; unsafeRec += 1; }
			}
		}
	}
	std::printf( "    [umbracull] SHRINK %.2f: %ld casters, %.0fk records | CULLED %ld (%.0f%% of records) | UNSAFE %ld culls (%.0fk records = %.1f%% of all) <- must be 0\n",
				 SHRINK, nCas, totRec / 1000.0, nCull, 100.0 * culledRec / std::fmax( 1.0, totRec ), nUnsafe, unsafeRec / 1000.0, 100.0 * unsafeRec / std::fmax( 1.0, totRec ) );
	CHECK( true );
}

// NO-SHADOW CASTER CLASSIFICATION. Hard data, no umbra extrapolation. For every caster that drops NO
// sampled receiver below full-lit (shadows nothing, by ray-truth), report WHAT it is (tri count, bbox
// shape = flat/blocky, area, distance to light vs the receivers) and the DIRECTLY-MEASURED reason it
// casts nothing:
//   OFF-PATH   : blocks ZERO light-disk rays for EVERY receiver (coverage == 1.0 exactly) - it is never
//                geometrically between the light and any receiver.
//   SUB-QUANTUM: blocks a few rays somewhere but max occlusion < 1% (never reaches one 1/16 coverage step).
// Run: CAP=... ./rbdoom3bfg_tests @study:SoftShadowNoShadow
STUDY_TEST( SoftShadowNoShadow, classify )
{
	const char* path = std::getenv( "CAP" );
	if( path == NULL ) { std::printf( "    [noshadow] CAP unset; skipping\n" ); CHECK( true ); return; }
	Cap cap;
	if( !LoadCap( path, cap ) ) { std::printf( "    [noshadow] cannot load %s\n", path ); CHECK( false ); return; }
	const int N = 16;

	std::vector<std::vector<float3>> lightRecv( cap.lights.size() );
	for( const capReceiver_t& r : cap.receivers )
	{
		if( r.lightIndex >= cap.lights.size() ) { continue; }
		const uint32_t tc = r.numIndex / 3;
		for( uint32_t t = 0; t < tc; t++ )
		{
			const uint32_t i0 = cap.recvIdx[r.firstIndex + t * 3 + 0], i1 = cap.recvIdx[r.firstIndex + t * 3 + 1], i2 = cap.recvIdx[r.firstIndex + t * 3 + 2];
			float3 p0( cap.recvVerts[i0 * 3], cap.recvVerts[i0 * 3 + 1], cap.recvVerts[i0 * 3 + 2] );
			float3 p1( cap.recvVerts[i1 * 3], cap.recvVerts[i1 * 3 + 1], cap.recvVerts[i1 * 3 + 2] );
			float3 p2( cap.recvVerts[i2 * 3], cap.recvVerts[i2 * 3 + 1], cap.recvVerts[i2 * 3 + 2] );
			lightRecv[r.lightIndex].push_back( ( p0 + p1 + p2 ) * ( 1.0f / 3.0f ) );
		}
	}

	double totRec = 0, nsRec = 0, offRec = 0, subRec = 0;
	double coneCullRec = 0, coneUnsafeRec = 0;		// penumbra-cone cull yield + any over-cull (of an on-screen shadower)
	long nConeCull = 0, nConeUnsafe = 0;			// EXACT caster counts
	auto envF = []( const char* k, float d ) { const char* s = std::getenv( k ); return s ? ( float )std::atof( s ) : d; };
	const float CONE_MARGIN = envF( "CONE_MARGIN", 0.0f );	// extra angular pad on the penumbra outer boundary (radians)
	double offFlatBig = 0, offBlocky = 0, offSmall = 0;		// off-path tri-mass by shape
	long nNS = 0, nOff = 0, nSub = 0;
	struct Row { int tris; float dx, dy, dz; float dL, dRecv; float graze; float side; const char* cls; };
	long nBehind = 0; double behindRec = 0;		// off-path casters on the opposite side of the light from the receivers
	std::vector<Row> rows;

	for( uint32_t li = 0; li < cap.lights.size(); li++ )
	{
		const std::vector<float3>& recv = lightRecv[li];
		if( recv.empty() ) { continue; }
		const float3 Lp( cap.lights[li].origin[0], cap.lights[li].origin[1], cap.lights[li].origin[2] );
		const float  swR = std::fmax( cap.lights[li].penumbraSize, 1e-2f );
		double dRecvSum = 0; float3 recvCen( 0, 0, 0 ); for( float3 P : recv ) { dRecvSum += std::sqrt( dot( P - Lp, P - Lp ) ); recvCen = recvCen + P; }
		float dRecvMean = ( float )( dRecvSum / recv.size() );
		recvCen = recvCen * ( 1.0f / recv.size() );
		float3 toRecv = recvCen - Lp; float trl = std::sqrt( dot( toRecv, toRecv ) ); if( trl > 1e-4f ) { toRecv = toRecv * ( 1.0f / trl ); }
		// receiver cone: bounding sphere of the receivers, angular radius from the light
		float Rr = 0; for( float3 P : recv ) { Rr = std::fmax( Rr, std::sqrt( dot( P - recvCen, P - recvCen ) ) ); }
		float thetaR = std::asin( std::fmin( 1.0f, Rr / std::fmax( trl, 1e-3f ) ) );
		const size_t stride = std::max( ( size_t )1, recv.size() / 300 );

		for( uint32_t c = 0; c < cap.casters.size(); c++ )
		{
			if( cap.casters[c].lightIndex != li ) { continue; }
			const capCaster_t& C = cap.casters[c];
			int t = ( int )( C.numIndex / 3 ); if( t <= 0 ) { continue; }
			totRec += t;
			float3 mn( 1e30f, 1e30f, 1e30f ), mx( -1e30f, -1e30f, -1e30f ), sum( 0, 0, 0 );
			for( uint32_t k = C.firstVert; k < C.firstVert + C.numVerts; k++ )
			{
				float3 p( cap.meshVerts[k * 3], cap.meshVerts[k * 3 + 1], cap.meshVerts[k * 3 + 2] );
				mn = float3( std::fmin( mn.x, p.x ), std::fmin( mn.y, p.y ), std::fmin( mn.z, p.z ) );
				mx = float3( std::fmax( mx.x, p.x ), std::fmax( mx.y, p.y ), std::fmax( mx.z, p.z ) ); sum = sum + p;
			}
			float3 ctr = sum * ( 1.0f / std::fmax( 1u, C.numVerts ) );
			const uint32_t* ri = &cap.meshIdx[C.firstIndex];
			// TRUE bounding sphere about the centroid (max vertex distance) - the AABB half-diagonal is NOT a
			// valid radius here because the sphere is centred on the centroid, not the AABB centre, and would
			// under-cover a caster whose verts skew to one side -> angC too small -> a real shadower missed.
			float casterR = 0.0f;
			for( uint32_t k = C.firstVert; k < C.firstVert + C.numVerts; k++ ) { float3 p( cap.meshVerts[k * 3], cap.meshVerts[k * 3 + 1], cap.meshVerts[k * 3 + 2] ); casterR = std::fmax( casterR, std::sqrt( dot( p - ctr, p - ctr ) ) ); }
			// min coverage + does it block ANY ray (coverage < 1)
			float minCov = 1.0f; bool blocksAny = false; float3 minCovP( 0, 0, 0 );
			for( size_t s = 0; s < recv.size(); s += stride )
			{
				float cov = MeshTruthShadowSoup( cap.meshVerts.data(), ri, C.numIndex, recv[s], Lp, swR, N );
				if( cov < minCov ) { minCov = cov; minCovP = recv[s]; }
				if( cov < 0.9999f ) { blocksAny = true; }
			}
			// PENUMBRA-CONE cull: cull iff NO captured (on-screen) receiver is even possibly in the caster's
			// penumbra - conservative angular-disk overlap (caster's angular radius + light's, caster in front).
			// Over-includes 'possibly-shadowed', so it never culls an on-screen shadower; off-screen shadows
			// are not rendered, so dropping the caster is safe (view-dependent, rebuilt per frame).
			bool onScreenShadow = false;
			for( size_t s = 0; s < recv.size() && !onScreenShadow; s += stride )
			{
				float3 P = recv[s], tC = ctr - P, tL = Lp - P;
				float dC = std::sqrt( dot( tC, tC ) ), dLp = std::sqrt( dot( tL, tL ) );
				// TIGHT contact test: receiver inside/touching the caster's AABB (not the loose half-diagonal
				// sphere) -> real contact shadow, keep. This is where the far-field angular-disk model is invalid.
				if( P.x >= mn.x - swR && P.x <= mx.x + swR && P.y >= mn.y - swR && P.y <= mx.y + swR && P.z >= mn.z - swR && P.z <= mx.z + swR ) { onScreenShadow = true; break; }
				float along = dot( tC, tL ) / std::fmax( dLp, 1e-4f );
				if( along + casterR <= 0.0f || along - casterR > dLp ) { continue; }	// the WHOLE caster is behind P or beyond the light disk
				float angC = std::asin( std::fmin( 1.0f, casterR / std::fmax( dC, 1e-3f ) ) );
				float angLr = std::asin( std::fmin( 1.0f, swR / std::fmax( dLp, 1e-3f ) ) );
				float ca = std::fmax( -1.0f, std::fmin( 1.0f, dot( tC * ( 1.0f / dC ), tL * ( 1.0f / dLp ) ) ) );
				if( std::acos( ca ) < angC + angLr + CONE_MARGIN ) { onScreenShadow = true; }		// angular disks overlap -> possibly shadows P
			}
			if( !onScreenShadow )
			{
				coneCullRec += t; nConeCull++;
				if( minCov < 0.99f )
				{
					coneUnsafeRec += t; nConeUnsafe++;
					static int dbg = 0;
					if( dbg < 12 )
					{
						dbg++;
						float3 P = minCovP, tC = ctr - P, tL = Lp - P;
						float dC = std::sqrt( dot( tC, tC ) ), dLp = std::sqrt( dot( tL, tL ) );
						float along = dot( tC, tL ) / std::fmax( dLp, 1e-4f );
						float angC = std::asin( std::fmin( 1.0f, casterR / std::fmax( dC, 1e-3f ) ) );
						float angLr = std::asin( std::fmin( 1.0f, swR / std::fmax( dLp, 1e-3f ) ) );
						float ang = std::acos( std::fmax( -1.0f, std::fmin( 1.0f, dot( tC * ( 1.0f / dC ), tL * ( 1.0f / dLp ) ) ) ) );
						std::printf( "      [unsafe] c%u nV%u t%d minCov %.3f | ctr(%.0f,%.0f,%.0f) P(%.0f,%.0f,%.0f) L(%.0f,%.0f,%.0f) casterR %.0f | dC %.0f dLp %.0f along %.0f | angC %.3f angLr %.3f ang %.3f need<%.3f %s\n",
									 c, C.numVerts, t, minCov, ctr.x, ctr.y, ctr.z, P.x, P.y, P.z, Lp.x, Lp.y, Lp.z, casterR, dC, dLp, along, angC, angLr, ang, angC + angLr, ( along + casterR <= 0.0f || along - casterR > dLp ) ? "SKIPPED-along" : "angle-missed" );
					}
				}
			}		// SAFETY: culled but shadows a captured receiver?
			if( minCov < 0.99f ) { continue; }		// SHADOWER - not our subject
			nNS++; nsRec += t;
			// geometry
			float3 dim = mx - mn; float d[3] = { dim.x, dim.y, dim.z }; std::sort( d, d + 3 );
			bool flat = d[2] > 1e-3f && d[0] / d[2] < 0.05f;		// one dimension ~0 => planar (wall/floor/panel)
			bool big = d[2] > 0.25f * dRecvMean;					// large relative to the scene
			float dL = std::sqrt( dot( ctr - Lp, ctr - Lp ) );
			// grazing: |normal . toLight| averaged (area-weighted) - 0 = edge-on to the light
			float3 an( 0, 0, 0 );
			for( uint32_t i = 0; i + 2 < C.numIndex; i += 3 )
			{
				float3 a( cap.meshVerts[ri[i] * 3], cap.meshVerts[ri[i] * 3 + 1], cap.meshVerts[ri[i] * 3 + 2] );
				float3 b( cap.meshVerts[ri[i + 1] * 3], cap.meshVerts[ri[i + 1] * 3 + 1], cap.meshVerts[ri[i + 1] * 3 + 2] );
				float3 cc( cap.meshVerts[ri[i + 2] * 3], cap.meshVerts[ri[i + 2] * 3 + 1], cap.meshVerts[ri[i + 2] * 3 + 2] );
				an = an + cross( b - a, cc - a );
			}
			float anl = std::sqrt( dot( an, an ) ); float graze = anl > 1e-6f ? std::fabs( dot( an * ( 1.0f / anl ), ( Lp - ctr ) * ( 1.0f / std::fmax( dL, 1e-4f ) ) ) ) : -1.0f;
			float side = dot( ( ctr - Lp ) * ( 1.0f / std::fmax( dL, 1e-4f ) ), toRecv );	// +1 = same side as receivers, -1 = behind the light
			const char* cls;
			if( !blocksAny ) { cls = "OFF-PATH"; nOff++; offRec += t; if( flat && big ) { offFlatBig += t; } else if( d[0] > 0.1f * d[2] ) { offBlocky += t; } else { offSmall += t; } if( side < 0.0f ) { nBehind++; behindRec += t; } }
			else { cls = "SUB-QUANTUM"; nSub++; subRec += t; }
			if( t >= 40 ) { rows.push_back( { t, d[0], d[1], d[2], dL, dRecvMean, graze, side, cls } ); }
		}
	}

	std::sort( rows.begin(), rows.end(), []( const Row& a, const Row& b ) { return a.tris > b.tris; } );
	std::printf( "    [noshadow] biggest NO-SHADOW casters (bbox dims sorted; dL=caster->light, dRecv=mean recv->light; graze=|n.toL|):\n" );
	for( int i = 0; i < ( int )rows.size() && i < 18; i++ )
	{
		const Row& r = rows[i];
		std::printf( "      %5d tris  bbox %6.0f x %6.0f x %6.0f  dL %6.0f (dRecv %6.0f)  graze %.2f  side %+.2f  %s\n", r.tris, r.dx, r.dy, r.dz, r.dL, r.dRecv, r.graze, r.side, r.cls );
	}
	std::printf( "    [noshadow] ===== of %.0fk total records: NO-SHADOW %.0fk (%.0f%%) = OFF-PATH %.0fk (%.0f%%) + SUB-QUANTUM %.0fk (%.0f%%) =====\n",
				 totRec / 1000.0, nsRec / 1000.0, 100.0 * nsRec / std::fmax( 1.0, totRec ), offRec / 1000.0, 100.0 * offRec / std::fmax( 1.0, totRec ), subRec / 1000.0, 100.0 * subRec / std::fmax( 1.0, totRec ) );
	std::printf( "      OFF-PATH shape breakdown: flat+large (wall/floor) %.0fk (%.0f%% of all) | blocky %.0fk | small %.0fk  | casters: %ld NS = %ld off + %ld sub\n",
				 offFlatBig / 1000.0, 100.0 * offFlatBig / std::fmax( 1.0, totRec ), offBlocky / 1000.0, offSmall / 1000.0, nNS, nOff, nSub );
	std::printf( "      OFF-PATH position: %ld casters (%.0fk records, %.0f%% of all) sit BEHIND the light (opposite side from the receivers)\n",
				 nBehind, behindRec / 1000.0, 100.0 * behindRec / std::fmax( 1.0, totRec ) );
	std::printf( "      *** PENUMBRA-CONE cull (margin %.3f): culls %ld casters / %.0f records (%.0f%% of ALL) | UNSAFE %ld casters / %.0f records <- must be EXACTLY 0 ***\n",
				 CONE_MARGIN, nConeCull, coneCullRec, 100.0 * coneCullRec / std::fmax( 1.0, totRec ), nConeUnsafe, coneUnsafeRec );
	CHECK( true );
}

// WORLD-SPACE CULL-CEILING ENUMERATION (per user spec, 2026-08-19). Uses the ENTIRE captured scene in
// world space - no reliance on which receivers happen to be enumerated. Per light, per occluder D:
//   (b) UMBRA-OVERLAP : every surface sample of D has EVERY light-disk ray blocked by OTHER casters
//                       (ray-truth union, not any plane construction) -> D receives no light -> casts
//                       nothing anywhere -> cullable, view-independent.
//   (a) OFF-SCREEN    : D receives light, but every shadow landing point - the FIRST scene hit beyond D
//                       along light->D rays (light centre + disk jitters), traced through the full soup
//                       (this light's casters + ALL receiver surfaces) - projects off-viewport or fails
//                       the DEPTH-BUFFER visibility test -> its shadow is invisible this frame -> cullable,
//                       view-dependent.
// Deterministic (fixed LCG); run twice, outputs must be bit-identical. Exact integer counts.
// Run: CAP=... ./rbdoom3bfg_tests @study:SoftShadowCullCeiling
STUDY_TEST( SoftShadowCullCeiling, enumerate )
{
	const char* path = std::getenv( "CAP" );
	if( path == NULL ) { std::printf( "    [ceiling] CAP unset; skipping\n" ); CHECK( true ); return; }
	Cap cap;
	if( !LoadCap( path, cap ) ) { std::printf( "    [ceiling] cannot load %s\n", path ); CHECK( false ); return; }
	const int KPTS = 12, KDISK = 8, KORG = 3;
	const int SW = ( int )cap.hdr.screenW, SH = ( int )cap.hdr.screenH;
	const float* MVP = cap.hdr.worldMVP;
	const float3 EYE( cap.hdr.vieworg[0], cap.hdr.vieworg[1], cap.hdr.vieworg[2] );
	struct Elem { uint32_t first, count; float3 c; float r; };	// a mesh range + bounding sphere (ray precull)
	auto sphereOf = [&]( const float* v, uint32_t firstVert, uint32_t numVerts ) -> std::pair<float3, float>
	{
		float3 mn( 1e30f, 1e30f, 1e30f ), mx( -1e30f, -1e30f, -1e30f );
		for( uint32_t k = firstVert; k < firstVert + numVerts; k++ )
		{
			float3 p( v[k * 3], v[k * 3 + 1], v[k * 3 + 2] );
			mn = float3( std::fmin( mn.x, p.x ), std::fmin( mn.y, p.y ), std::fmin( mn.z, p.z ) );
			mx = float3( std::fmax( mx.x, p.x ), std::fmax( mx.y, p.y ), std::fmax( mx.z, p.z ) );
		}
		return { ( mn + mx ) * 0.5f, 0.5f * std::sqrt( dot( mx - mn, mx - mn ) ) + 1e-2f };
	};
	// receiver surfaces once (landing geometry for every light)
	std::vector<Elem> recvElems;
	for( const capReceiver_t& r : cap.receivers )
	{
		if( r.numIndex < 3 ) { continue; }
		auto s = sphereOf( cap.recvVerts.data(), r.firstVert, r.numVerts );
		recvElems.push_back( { r.firstIndex, r.numIndex, s.first, s.second } );
	}

	// nearest hit of ray O + t*dir against a mesh range (indices global), t in (tmin, tbest); returns updated tbest
	auto nearestHit = [&]( float3 O, float3 dir, const float* verts, const uint32_t* idx, uint32_t numIdx, double tmin, double tbest ) -> double
	{
		for( uint32_t i = 0; i + 2 < numIdx; i += 3 )
		{
			const uint32_t ia = idx[i], ib = idx[i + 1], ic = idx[i + 2];
			float3 A( verts[ia * 3], verts[ia * 3 + 1], verts[ia * 3 + 2] );
			float3 B( verts[ib * 3], verts[ib * 3 + 1], verts[ib * 3 + 2] );
			float3 C( verts[ic * 3], verts[ic * 3 + 1], verts[ic * 3 + 2] );
			float3 e1 = B - A, e2 = C - A, pv = cross( dir, e2 );
			float det = dot( e1, pv ); if( std::fabs( det ) < 1e-12f ) { continue; }
			float inv = 1.0f / det; float3 tv = O - A;
			float u = dot( tv, pv ) * inv; if( u < -1e-6f || u > 1.0f + 1e-6f ) { continue; }
			float3 qv = cross( tv, e1 );
			float vv = dot( dir, qv ) * inv; if( vv < -1e-6f || u + vv > 1.0f + 1e-6f ) { continue; }
			double t = dot( e2, qv ) * inv;
			if( t > tmin && t < tbest ) { tbest = t; }
		}
		return tbest;
	};
	auto raySphere = [&]( float3 O, float3 dir, float3 C, float r, double tmax ) -> bool
	{
		float3 oc = C - O; double tp = dot( oc, dir );
		if( tp < -( double )r || tp > tmax + r ) { return false; }
		double t = std::fmax( 0.0, std::fmin( tp, tmax ) );
		float3 q = float3( O.x + dir.x * ( float )t - C.x, O.y + dir.y * ( float )t - C.y, O.z + dir.z * ( float )t - C.z );
		return dot( q, q ) <= r * r;
	};

	uint32_t rng = 0x9E3779B9u;
	auto frand = [&]() -> float { rng = rng * 1664525u + 1013904223u; return ( rng >> 8 ) * ( 1.0f / 16777216.0f ); };

	// GLOBAL occlusion soup for camera-visibility: all receiver surfaces + every light's caster meshes
	// (duplicates across lights are harmless - any hit occludes). The captured depth buffer is EMPTY
	// (all-1.0, measured), so visibility is ray-traced in world space against the captured geometry.
	std::vector<Elem> vizElems = recvElems;		// receiver ranges (recv arrays)
	const size_t vizRecvCount = vizElems.size();
	for( uint32_t c = 0; c < cap.casters.size(); c++ )
	{
		const capCaster_t& C = cap.casters[c];
		if( C.numIndex < 3 ) { continue; }
		auto s = sphereOf( cap.meshVerts.data(), C.firstVert, C.numVerts );
		vizElems.push_back( { C.firstIndex, C.numIndex, s.first, s.second } );	// mesh arrays
	}
	// SYNTHESIZED DEPTH: the captured depth block is empty (all-1.0, measured), so render our own - one
	// camera ray per grid pixel through the whole soup; store eye-DISTANCE of the first hit. The grid's
	// hit points ARE the frame's visible surface points (the enclosing volume rendered in world space).
	const int GW = 256, GH = 144;
	std::vector<float>  synthD( ( size_t )GW * GH, -1.0f );	// eye distance; -1 = sky/no hit
	std::vector<float3> synthP( ( size_t )GW * GH );			// the visible world point per grid pixel
	{
		// pixel -> world ray via the capture's unprojection matrix (two unprojected depths span the ray)
		const float* UM = cap.hdr.unprojectionToWorldMatrix;
		const float projZz = cap.hdr.projectionMatrix[10], projZw = cap.hdr.projectionMatrix[11];
		auto unproj = [&]( float ndcx, float ndcy, float ndcz ) -> float3
		{
			float clipW = -projZw / ( -projZz - ndcz );
			float cl[4] = { ndcx * clipW, ndcy * clipW, ndcz * clipW, clipW };
			float wx = UM[0] * cl[0] + UM[1] * cl[1] + UM[2] * cl[2] + UM[3] * cl[3];
			float wy = UM[4] * cl[0] + UM[5] * cl[1] + UM[6] * cl[2] + UM[7] * cl[3];
			float wz = UM[8] * cl[0] + UM[9] * cl[1] + UM[10] * cl[2] + UM[11] * cl[3];
			float ww = UM[12] * cl[0] + UM[13] * cl[1] + UM[14] * cl[2] + UM[15] * cl[3];
			float inv = ( std::fabs( ww ) > 1e-12f ) ? 1.0f / ww : 0.0f;
			return float3( wx * inv, wy * inv, wz * inv );
		};
		for( int gy = 0; gy < GH; gy++ )
		{
			for( int gx = 0; gx < GW; gx++ )
			{
				float ndcx = ( gx + 0.5f ) / GW * 2.0f - 1.0f, ndcy = 1.0f - ( gy + 0.5f ) / GH * 2.0f;
				float3 A = unproj( ndcx, ndcy, 0.2f ), B = unproj( ndcx, ndcy, 0.8f );
				float3 dir = B - A; float dl = std::sqrt( dot( dir, dir ) ); if( dl < 1e-6f ) { continue; }
				dir = dir * ( 1.0f / dl );
				double tbest = 1e30;
				for( size_t b = 0; b < vizElems.size(); b++ )
				{
					if( !raySphere( EYE, dir, vizElems[b].c, vizElems[b].r, tbest > 1e29 ? 1e9 : tbest ) ) { continue; }
					const float* v = ( b < vizRecvCount ) ? cap.recvVerts.data() : cap.meshVerts.data();
					const uint32_t* ix = ( b < vizRecvCount ) ? &cap.recvIdx[vizElems[b].first] : &cap.meshIdx[vizElems[b].first];
					tbest = nearestHit( EYE, dir, v, ix, vizElems[b].count, 1e-2, tbest );
				}
				if( tbest < 1e29 )
				{
					synthD[( size_t )gy * GW + gx] = ( float )tbest;
					synthP[( size_t )gy * GW + gx] = EYE + dir * ( float )tbest;
				}
			}
		}
		long filled = 0; for( float d : synthD ) { if( d > 0 ) { filled++; } }
		std::printf( "    [ceiling] synthesized depth %dx%d: %ld/%d pixels hit geometry (%.0f%%)\n", GW, GH, filled, GW * GH, 100.0 * filled / ( GW * GH ) );
	}
	// Visible(H) = projects into the viewport AND matches the synthesized frontmost distance at its pixel
	// (relative eps; a coarser grid than the real frame, so allow the neighbourhood's best match).
	auto Visible = [&]( float3 H ) -> bool
	{
		float x = MVP[0] * H.x + MVP[1] * H.y + MVP[2] * H.z + MVP[3];
		float y = MVP[4] * H.x + MVP[5] * H.y + MVP[6] * H.z + MVP[7];
		float w = MVP[12] * H.x + MVP[13] * H.y + MVP[14] * H.z + MVP[15];
		if( w <= 1e-6f ) { return false; }
		float fx = ( x / w * 0.5f + 0.5f ) * GW, fy = ( 1.0f - ( y / w * 0.5f + 0.5f ) ) * GH;
		if( fx < 0 || fx >= GW || fy < 0 || fy >= GH ) { return false; }
		float3 d3 = H - EYE; float hd = std::sqrt( dot( d3, d3 ) );
		for( int oy = -1; oy <= 1; oy++ )
		{
			for( int ox = -1; ox <= 1; ox++ )
			{
				int px = ( int )fx + ox, py = ( int )fy + oy;
				if( px < 0 || px >= GW || py < 0 || py >= GH ) { continue; }
				float sd = synthD[( size_t )py * GW + px];
				if( sd > 0 && hd <= sd * 1.03f + 4.0f ) { return true; }	// at/in front of the frontmost surface here
			}
		}
		return false;
	};

	double totRec = 0, offRec = 0, umbRec = 0;
	long nCas = 0, nOff = 0, nUmb = 0;
	std::vector<std::pair<uint32_t, uint32_t>> offList;		// (caster, light) classified off-screen - audited below
	for( uint32_t li = 0; li < cap.lights.size(); li++ )
	{
		std::vector<Elem> casElems; std::vector<uint32_t> casIdx;
		for( uint32_t c = 0; c < cap.casters.size(); c++ )
		{
			if( cap.casters[c].lightIndex != li ) { continue; }
			const capCaster_t& C = cap.casters[c];
			if( C.numIndex < 3 ) { continue; }
			auto s = sphereOf( cap.meshVerts.data(), C.firstVert, C.numVerts );
			casElems.push_back( { C.firstIndex, C.numIndex, s.first, s.second } );
			casIdx.push_back( c );
		}
		if( casElems.empty() ) { continue; }
		const float3 Lp( cap.lights[li].origin[0], cap.lights[li].origin[1], cap.lights[li].origin[2] );
		const float  swR = std::fmax( cap.lights[li].penumbraSize, 1e-2f );
		float3 up = ( std::fabs( Lp.z ) < 0.9f * ( std::fabs( Lp.x ) + std::fabs( Lp.y ) + std::fabs( Lp.z ) + 1.0f ) ) ? float3( 0, 0, 1 ) : float3( 1, 0, 0 );

		for( size_t dci = 0; dci < casElems.size(); dci++ )
		{
			const capCaster_t& D = cap.casters[casIdx[dci]];
			const int tcount = ( int )( D.numIndex / 3 );
			nCas++; totRec += tcount;
			// area-weighted deterministic surface samples of D
			const uint32_t* ri = &cap.meshIdx[D.firstIndex];
			std::vector<double> cum( tcount ); double atot = 0;
			for( int t = 0; t < tcount; t++ )
			{
				float3 A( cap.meshVerts[ri[t * 3] * 3], cap.meshVerts[ri[t * 3] * 3 + 1], cap.meshVerts[ri[t * 3] * 3 + 2] );
				float3 B( cap.meshVerts[ri[t * 3 + 1] * 3], cap.meshVerts[ri[t * 3 + 1] * 3 + 1], cap.meshVerts[ri[t * 3 + 1] * 3 + 2] );
				float3 C2( cap.meshVerts[ri[t * 3 + 2] * 3], cap.meshVerts[ri[t * 3 + 2] * 3 + 1], cap.meshVerts[ri[t * 3 + 2] * 3 + 2] );
				float3 cr = cross( B - A, C2 - A ); atot += 0.5 * std::sqrt( dot( cr, cr ) ); cum[t] = atot;
			}
			if( atot <= 0 ) { continue; }
			float3 pts[KPTS];
			for( int s = 0; s < KPTS; s++ )
			{
				double pick = frand() * atot;
				int t = ( int )( std::lower_bound( cum.begin(), cum.end(), pick ) - cum.begin() ); if( t >= tcount ) { t = tcount - 1; }
				float u = frand(), v = frand(); if( u + v > 1.0f ) { u = 1.0f - u; v = 1.0f - v; }
				float3 A( cap.meshVerts[ri[t * 3] * 3], cap.meshVerts[ri[t * 3] * 3 + 1], cap.meshVerts[ri[t * 3] * 3 + 2] );
				float3 B( cap.meshVerts[ri[t * 3 + 1] * 3], cap.meshVerts[ri[t * 3 + 1] * 3 + 1], cap.meshVerts[ri[t * 3 + 1] * 3 + 2] );
				float3 C2( cap.meshVerts[ri[t * 3 + 2] * 3], cap.meshVerts[ri[t * 3 + 2] * 3 + 1], cap.meshVerts[ri[t * 3 + 2] * 3 + 2] );
				pts[s] = A + ( B - A ) * u + ( C2 - A ) * v;
			}
			// (b) UMBRA-OVERLAP: every disk ray of every sample blocked by an OTHER caster
			bool contained = true;
			for( int s = 0; s < KPTS && contained; s++ )
			{
				float3 toL = Lp - pts[s]; float dL = std::sqrt( dot( toL, toL ) ); if( dL < 1e-3f ) { contained = false; break; }
				float3 nrm = toL * ( 1.0f / dL );
				float3 u2 = normalize( cross( up, nrm ) ), v2 = cross( nrm, u2 );
				for( int k = 0; k < KDISK && contained; k++ )
				{
					float a = ( float )( 2.0 * 3.14159265358979 * k / KDISK );
					float rr = swR * std::sqrt( ( k + 0.5f ) / KDISK );
					float3 tgt = Lp + u2 * ( rr * std::cos( a ) ) + v2 * ( rr * std::sin( a ) );
					float3 seg = tgt - pts[s];
					bool blocked = false;
					for( size_t b = 0; b < casElems.size() && !blocked; b++ )
					{
						if( b == dci ) { continue; }
						float segL = std::sqrt( dot( seg, seg ) ); if( segL < 1e-4f ) { continue; }
						if( !raySphere( pts[s], seg * ( 1.0f / segL ), casElems[b].c, casElems[b].r, segL ) ) { continue; }
						if( RayHitsMesh( pts[s], seg, cap.meshVerts.data(), &cap.meshIdx[casElems[b].first], casElems[b].count ) ) { blocked = true; }
					}
					if( !blocked ) { contained = false; }
				}
			}
			if( contained ) { nUmb++; umbRec += tcount; continue; }
			// (a) OFF-SCREEN, SCREEN-SIDE EXHAUSTIVE: D's shadow is on-screen iff some VISIBLE point has a
			// disk ray blocked by D. Test every even-grid visible point (cone prefilter, early-out); no
			// visible point shadowed -> D's whole soft shadow lies off-screen -> cullable this frame.
			bool shadowsVisible = false;
			const std::pair<float3, float> Ds = { casElems[dci].c, casElems[dci].r };
			for( int gy = 0; gy < GH && !shadowsVisible; gy += 2 )
			{
				for( int gx = 0; gx < GW && !shadowsVisible; gx += 2 )
				{
					float sd = synthD[( size_t )gy * GW + gx]; if( sd <= 0 ) { continue; }
					float3 P = synthP[( size_t )gy * GW + gx];
					float3 tC = Ds.first - P, tL = Lp - P;
					float dC = std::sqrt( dot( tC, tC ) ), dLp2 = std::sqrt( dot( tL, tL ) );
					if( dC > 1e-3f && dC > Ds.second + swR )
					{
						float angC = std::asin( std::fmin( 1.0f, Ds.second / dC ) );
						float angL = std::asin( std::fmin( 1.0f, swR / std::fmax( dLp2, 1e-3f ) ) );
						float ca = std::fmax( -1.0f, std::fmin( 1.0f, dot( tC * ( 1.0f / dC ), tL * ( 1.0f / std::fmax( dLp2, 1e-3f ) ) ) ) );
						if( std::acos( ca ) > angC + angL ) { continue; }
					}
					float3 nrm = tL * ( 1.0f / std::fmax( dLp2, 1e-3f ) );
					float3 u2 = normalize( cross( ( std::fabs( nrm.z ) < 0.9f ) ? float3( 0, 0, 1 ) : float3( 1, 0, 0 ), nrm ) ), v2 = cross( nrm, u2 );
					static const float DOFF[5][2] = { {0, 0}, {1, 0}, {-1, 0}, {0, 1}, {0, -1} };
					for( int k = 0; k < 5 && !shadowsVisible; k++ )
					{
						float3 tgt = Lp + u2 * ( swR * DOFF[k][0] ) + v2 * ( swR * DOFF[k][1] );
						if( RayHitsMesh( P, tgt - P, cap.meshVerts.data(), &cap.meshIdx[D.firstIndex], D.numIndex ) ) { shadowsVisible = true; }
					}
				}
			}
			if( !shadowsVisible ) { nOff++; offRec += tcount; offList.push_back( { casIdx[dci], li } ); }
		}
	}

	// AUDIT (independent residual): re-test every off-screen-classified caster against the ODD grid
	// pixels - a sample set DISJOINT from the classifier's even grid. Any blocked disk ray there = a
	// shadow sliver the classifier's grid missed. Exact integer count; the standard is EXACTLY 0.
	long nAuditWrong = 0; double auditWrongRec = 0;
	{
		for( auto& oc : offList )
		{
			const capCaster_t& D = cap.casters[oc.first];
			const float3 Lp( cap.lights[oc.second].origin[0], cap.lights[oc.second].origin[1], cap.lights[oc.second].origin[2] );
			const float  swR = std::fmax( cap.lights[oc.second].penumbraSize, 1e-2f );
			auto s = sphereOf( cap.meshVerts.data(), D.firstVert, D.numVerts );
			bool shadowVisible = false;
			for( int gy = 1; gy < GH && !shadowVisible; gy += 2 )
			{
				for( int gx = 1; gx < GW && !shadowVisible; gx += 2 )
				{
					float sd = synthD[( size_t )gy * GW + gx]; if( sd <= 0 ) { continue; }
					float3 P = synthP[( size_t )gy * GW + gx];
					// cheap cone prefilter: caster sphere vs the P->light-disk angular cone
					float3 tC = s.first - P, tL = Lp - P;
					float dC = std::sqrt( dot( tC, tC ) ), dLp2 = std::sqrt( dot( tL, tL ) );
					if( dC > 1e-3f && dC > s.second + swR )
					{
						float angC = std::asin( std::fmin( 1.0f, s.second / dC ) );
						float angL = std::asin( std::fmin( 1.0f, swR / std::fmax( dLp2, 1e-3f ) ) );
						float ca = std::fmax( -1.0f, std::fmin( 1.0f, dot( tC * ( 1.0f / dC ), tL * ( 1.0f / std::fmax( dLp2, 1e-3f ) ) ) ) );
						if( std::acos( ca ) > angC + angL ) { continue; }
					}
					// disk targets: centre + 4 rim points; ANY blocked ray = visible shadow
					float3 nrm = tL * ( 1.0f / std::fmax( dLp2, 1e-3f ) );
					float3 u2 = normalize( cross( ( std::fabs( nrm.z ) < 0.9f ) ? float3( 0, 0, 1 ) : float3( 1, 0, 0 ), nrm ) ), v2 = cross( nrm, u2 );
					for( int k = 0; k < 5 && !shadowVisible; k++ )
					{
						float3 tgt = ( k == 0 ) ? Lp : Lp + u2 * ( swR * ( ( k & 1 ) ? 1.0f : -1.0f ) * ( ( k < 3 ) ? 1.0f : 0.0f ) ) + v2 * ( swR * ( ( k >= 3 ) ? ( ( k & 1 ) ? 1.0f : -1.0f ) : 0.0f ) );
						if( RayHitsMesh( P, tgt - P, cap.meshVerts.data(), &cap.meshIdx[D.firstIndex], D.numIndex ) ) { shadowVisible = true; }
					}
				}
			}
			if( shadowVisible ) { nAuditWrong++; auditWrongRec += D.numIndex / 3; }
		}
	}

	std::printf( "    [ceiling] ===== %ld casters, %.0f records =====\n", nCas, totRec );
	std::printf( "      (b) UMBRA-OVERLAP  (view-independent): %ld casters, %.0f records (%.1f%% of all)\n", nUmb, umbRec, 100.0 * umbRec / std::fmax( 1.0, totRec ) );
	std::printf( "      (a) OFF-SCREEN shadow (view-dependent): %ld casters, %.0f records (%.1f%% of all)\n", nOff, offRec, 100.0 * offRec / std::fmax( 1.0, totRec ) );
	std::printf( "      UNION cullable: %ld casters, %.0f records (%.1f%% of all)\n", nUmb + nOff, umbRec + offRec, 100.0 * ( umbRec + offRec ) / std::fmax( 1.0, totRec ) );
	std::printf( "      AUDIT of (a): %ld casters (%.0f records) shadow a synthesized VISIBLE point <- must be EXACTLY 0\n", nAuditWrong, auditWrongRec );
	CHECK( true );
}

// SAME-LIGHT UMBRA ACCUMULATION (depth-ordered, vertex-exact) vs RAY TRUTH. The user's algorithm:
// per LIGHT (never across lights), walk triangles in DEPTH ORDER from the light; kept triangles emit
// their exact umbra certificate (triangle plane + 3 edge planes tangent to the light sphere - the
// SW_TILE_UMBRA construction); a triangle whose 3 vertices all lie inside ONE accumulated certificate
// is fully contained in that umbra (the region is CONVEX, so vertex containment is exact - no bounding
// sphere slop) and cannot produce any shadow -> culled. If a caster's every light-FRONT-facing triangle
// was culled, its backfaces are culled too (they lie deeper in the same umbra). Validated against ray
// truth: FALSE POSITIVES (culled but genuinely contributing) must be EXACTLY 0; FALSE NEGATIVES
// (redundant but kept) estimated on a strided sample. Deterministic - run twice, bit-identical.
// Run: CAP=... ./rbdoom3bfg_tests @study:SoftShadowUmbraAccum
STUDY_TEST( SoftShadowUmbraAccum, validate )
{
	const char* path = std::getenv( "CAP" );
	if( path == NULL ) { std::printf( "    [umbacc] CAP unset; skipping\n" ); CHECK( true ); return; }
	Cap cap;
	if( !LoadCap( path, cap ) ) { std::printf( "    [umbacc] cannot load %s\n", path ); CHECK( false ); return; }
	const int KDISK = 8;			// ground-truth disk rays per surface sample
	const int KS = 6;				// ground-truth surface samples per triangle
	const int FN_STRIDE = 7;		// kept-triangle stride for the false-negative estimate
	auto envI = []( const char* k, int d ) { const char* s = std::getenv( k ); return s ? std::atoi( s ) : d; };
	const int SUBDIV_DEPTH = envI( "UMB_SUBDIV", 3 );	// union-containment midpoint subdivision depth
	const int PASSES = envI( "UMB_PASSES", 2 );			// depth-ordered passes (pass 2+ sees ALL certificates)
	const int CELLAGG = envI( "UMB_CELLAGG", 1 );		// 1 = CONSERVATIVE cell-cert aggregate (new), 0 = legacy sampled rays
	const int CELLS_G = envI( "UMB_CELLS", 6 );			// light-sphere ball-grid resolution per axis
	long cellCertsBuilt = 0, cellCertFails = 0;			// diagnostics

	auto raySphere2 = []( float3 O, float3 dir, float3 C, float r, float tmax ) -> bool
	{
		float3 oc = C - O; float tp = dot( oc, dir );
		if( tp < -r || tp > tmax + r ) { return false; }
		float t = std::fmax( 0.0f, std::fmin( tp, tmax ) );
		float3 q = float3( O.x + dir.x * t - C.x, O.y + dir.y * t - C.y, O.z + dir.z * t - C.z );
		return dot( q, q ) <= r * r;
	};

	struct Cert						// exact umbra certificate of one kept CONVEX polygon (tri or merged coplanar group)
	{
		float3 nt;					// unit polygon normal, oriented TOWARD the light
		float3 V0;					// plane anchor
		float3 en[12]; float3 ea[12]; int ne;	// edge tangent planes: inside iff dot(en, v - ea) >= 0 for all
		float3 axis; float cosH;	// prune cone from the light through the polygon
		float dMin;					// nearest vertex distance to the light (umbra starts behind this)
		float3 src[3];				// first 3 source verts (FP diagnosis)
	};

	double totRec = 0, culledRec = 0, backRec = 0; long nCulled = 0, nBack = 0;
	long nFP = 0, fnRedundant = 0, fnChecked = 0; long fullCasters = 0;
	long fnCause[4] = { 0, 0, 0, 0 };	// 0=GRAZE 1=LATER 2=CERT(union gap) 3=other
	long fpSev[4] = { 0, 0, 0, 0 };		// FP severity by unblocked audit rays (of 512): <=2, <=8, <=32, >32
	long fpUnblockedMax = 0;
	long fnAchievable = 0;				// FN tris the converged cull test would NOW accept (real residual)

	for( uint32_t li = 0; li < cap.lights.size(); li++ )
	{
		// gather this light's triangles + caster spans
		struct Tri { float3 v[3]; uint32_t owner; float depth; bool front; };
		std::vector<Tri> tris;
		std::vector<uint32_t> lightCas;
		struct BElem { uint32_t first, count; float3 c; float r; uint32_t owner; };
		std::vector<BElem> blockers;			// per-caster mesh ranges for ground-truth rays
		const float3 Lp( cap.lights[li].origin[0], cap.lights[li].origin[1], cap.lights[li].origin[2] );
		const float  swR = std::fmax( cap.lights[li].penumbraSize, 1e-2f );
		for( uint32_t c = 0; c < cap.casters.size(); c++ )
		{
			if( cap.casters[c].lightIndex != li ) { continue; }
			const capCaster_t& C = cap.casters[c];
			if( C.numIndex < 3 ) { continue; }
			lightCas.push_back( c );
			float3 mn( 1e30f, 1e30f, 1e30f ), mx( -1e30f, -1e30f, -1e30f );
			for( uint32_t k = C.firstVert; k < C.firstVert + C.numVerts; k++ )
			{
				float3 p( cap.meshVerts[k * 3], cap.meshVerts[k * 3 + 1], cap.meshVerts[k * 3 + 2] );
				mn = float3( std::fmin( mn.x, p.x ), std::fmin( mn.y, p.y ), std::fmin( mn.z, p.z ) );
				mx = float3( std::fmax( mx.x, p.x ), std::fmax( mx.y, p.y ), std::fmax( mx.z, p.z ) );
			}
			blockers.push_back( { C.firstIndex, C.numIndex, ( mn + mx ) * 0.5f, 0.5f * std::sqrt( dot( mx - mn, mx - mn ) ) + 1e-2f, c } );
			const uint32_t* ri = &cap.meshIdx[C.firstIndex];
			for( uint32_t i = 0; i + 2 < C.numIndex; i += 3 )
			{
				Tri t;
				for( int k = 0; k < 3; k++ ) { t.v[k] = float3( cap.meshVerts[ri[i + k] * 3], cap.meshVerts[ri[i + k] * 3 + 1], cap.meshVerts[ri[i + k] * 3 + 2] ); }
				t.owner = c;
				t.depth = std::fmin( std::fmin( std::sqrt( dot( t.v[0] - Lp, t.v[0] - Lp ) ), std::sqrt( dot( t.v[1] - Lp, t.v[1] - Lp ) ) ), std::sqrt( dot( t.v[2] - Lp, t.v[2] - Lp ) ) );
				float3 n = cross( t.v[1] - t.v[0], t.v[2] - t.v[0] );
				float3 cen = ( t.v[0] + t.v[1] + t.v[2] ) * ( 1.0f / 3.0f );
				t.front = dot( n, Lp - cen ) > 0.0f || dot( n, n ) < 1e-12f;
				tris.push_back( t );
			}
		}
		if( tris.empty() ) { continue; }
		totRec += ( double )tris.size();
		std::vector<int> order( tris.size() );
		for( size_t i = 0; i < order.size(); i++ ) { order[i] = ( int )i; }
		std::sort( order.begin(), order.end(), [&]( int a, int b ) { return tris[a].depth < tris[b].depth; } );

		// emit the exact umbra certificate of a kept triangle; false = no umbra (light too close/degenerate)
		auto makeCert = [&]( const Tri& T, Cert& out ) -> bool
		{
			float3 nt = cross( T.v[1] - T.v[0], T.v[2] - T.v[0] );
			float ntl = std::sqrt( dot( nt, nt ) ); if( ntl <= 1e-6f ) { return false; }
			nt = nt * ( 1.0f / ntl );
			float dL = dot( nt, Lp - T.v[0] ); if( dL < 0.0f ) { nt = nt * -1.0f; dL = -dL; }
			if( dL <= swR + 1e-3f ) { return false; }			// light sphere not strictly in front of the plane
			for( int e = 0; e < 3; e++ )
			{
				float3 Va = T.v[e], Vb = T.v[( e + 1 ) % 3], Vc = T.v[( e + 2 ) % 3];
				float3 ed = Vb - Va; float el2 = dot( ed, ed ); if( el2 < 1e-12f ) { return false; }
				float3 u2 = ed * ( 1.0f / std::sqrt( el2 ) );
				float3 w = Lp - Va; float3 wp = w - u2 * dot( w, u2 ); float W2 = dot( wp, wp );
				if( W2 <= swR * swR + 1e-6f ) { return false; }
				float invW = 1.0f / std::sqrt( W2 ); float3 n0 = wp * invW; float3 m = cross( u2, n0 );
				float sinT = swR * invW, cosT = std::sqrt( std::fmax( 1.0f - sinT * sinT, 0.0f ) );
				float sigma = ( dot( m, Vc - Va ) >= 0.0f ) ? 1.0f : -1.0f;
				out.en[e] = n0 * sinT + m * ( sigma * cosT );
				out.ea[e] = Va;
			}
			out.nt = nt; out.V0 = T.v[0];
			out.src[0] = T.v[0]; out.src[1] = T.v[1]; out.src[2] = T.v[2];
			float3 cen = ( T.v[0] + T.v[1] + T.v[2] ) * ( 1.0f / 3.0f );
			float3 ax = cen - Lp; float axl = std::sqrt( dot( ax, ax ) ); if( axl < 1e-4f ) { return false; }
			out.axis = ax * ( 1.0f / axl );
			out.cosH = 1.0f;
			out.dMin = 1e30f;
			for( int k = 0; k < 3; k++ )
			{
				float3 dv = T.v[k] - Lp; float dl = std::sqrt( dot( dv, dv ) );
				out.dMin = std::fmin( out.dMin, dl );
				if( dl > 1e-4f ) { out.cosH = std::fmin( out.cosH, dot( dv * ( 1.0f / dl ), out.axis ) ); }
			}
			out.cosH -= 1e-3f;									// prune-cone margin only (the exact test decides)
			return true;
		};
		// vertex-exact containment of point v in cert (slack mirrors UmbraCertifies with tR = 0)
		auto inCert = [&]( const Cert& C, float3 v ) -> bool
		{
			float distPL = std::sqrt( dot( Lp - v, Lp - v ) );
			if( dot( C.nt, v - C.V0 ) >= -2e-4f * distPL ) { return false; }		// not strictly behind the plane
			for( int e = 0; e < C.ne; e++ ) { if( dot( C.en[e], v - C.ea[e] ) < 0.0f ) { return false; } }
			return true;
		};
		// polygon certificate: same construction over a CONVEX CCW vert loop (sigma oriented by centroid)
		// certificate w.r.t. an ARBITRARY light ball (LC, LR): the umbra region of polygon pv against that
		// ball. LC=Lp, LR=swR is the classic full-light certificate; a CELL of the light-sphere grid gives
		// the per-cell certificate the conservative aggregate composes (different blockers per cell).
		auto makePolyCertAt = [&]( const float3* pv, int n, float3 LC, float LR, Cert& out ) -> bool
		{
			if( n < 3 || n > 12 ) { return false; }
			float3 nt = cross( pv[1] - pv[0], pv[2] - pv[0] );
			float ntl = std::sqrt( dot( nt, nt ) ); if( ntl <= 1e-6f ) { return false; }
			nt = nt * ( 1.0f / ntl );
			float dL = dot( nt, LC - pv[0] ); if( dL < 0.0f ) { nt = nt * -1.0f; dL = -dL; }
			if( dL <= LR + 1e-3f ) { return false; }
			float3 cen( 0, 0, 0 ); for( int k = 0; k < n; k++ ) { cen = cen + pv[k]; } cen = cen * ( 1.0f / n );
			for( int e = 0; e < n; e++ )
			{
				float3 Va = pv[e], Vb = pv[( e + 1 ) % n];
				float3 ed = Vb - Va; float el2 = dot( ed, ed ); if( el2 < 1e-12f ) { return false; }
				float3 u2 = ed * ( 1.0f / std::sqrt( el2 ) );
				float3 w = LC - Va; float3 wp = w - u2 * dot( w, u2 ); float W2 = dot( wp, wp );
				if( W2 <= LR * LR + 1e-6f ) { return false; }
				float invW = 1.0f / std::sqrt( W2 ); float3 n0 = wp * invW; float3 m = cross( u2, n0 );
				float sinT = LR * invW, cosT = std::sqrt( std::fmax( 1.0f - sinT * sinT, 0.0f ) );
				float sigma = ( dot( m, cen - Va ) >= 0.0f ) ? 1.0f : -1.0f;	// interior = polygon centroid side
				out.en[e] = n0 * sinT + m * ( sigma * cosT );
				out.ea[e] = Va;
			}
			out.ne = n;
			out.nt = nt; out.V0 = pv[0];
			out.src[0] = pv[0]; out.src[1] = pv[1]; out.src[2] = pv[2];
			float3 ax = cen - LC; float axl = std::sqrt( dot( ax, ax ) ); if( axl < 1e-4f ) { return false; }
			out.axis = ax * ( 1.0f / axl );
			out.cosH = 1.0f;
			out.dMin = dL;			// nearest possible umbra point = the ball's perpendicular distance to
									// the PLANE (a vertex min under-bounds when the ball sits over the
									// polygon interior -> wrongly pruned containment, measured FN source)
			for( int k = 0; k < n; k++ )
			{
				float3 dv = pv[k] - LC; float dl = std::sqrt( dot( dv, dv ) );
				if( dl > 1e-4f ) { out.cosH = std::fmin( out.cosH, dot( dv * ( 1.0f / dl ), out.axis ) ); }
			}
			out.cosH -= 1e-3f;
			return true;
		};
		auto makePolyCert = [&]( const float3* pv, int n, Cert& out ) -> bool
		{
			return makePolyCertAt( pv, n, Lp, swR, out );
		};

		// ---- EMISSION UNITS: coplanar groups whose triangle UNION is CONVEX (tri-area sum == 2D hull
		// area) merge into ONE hull-polygon certificate - a tessellated wall/panel becomes one big
		// umbra that swallows whole backfaces the per-triangle certificates cannot. Everything else
		// stays a per-triangle unit. Units walk in depth order; a unit fully inside one accumulated
		// certificate culls all its member tris, else it emits its certificate.
		struct Unit { std::vector<int> members; std::vector<float3> poly; float depth; };
		std::vector<Unit> units;
		{
			// bucket by (owner, quantized plane)
			std::unordered_map<uint64_t, std::vector<int>> groups;
			for( size_t i = 0; i < tris.size(); i++ )
			{
				const Tri& T = tris[i];
				float3 n = cross( T.v[1] - T.v[0], T.v[2] - T.v[0] );
				float nl = std::sqrt( dot( n, n ) );
				if( nl < 1e-9f ) { Unit u; u.members = { ( int )i }; u.poly = { T.v[0], T.v[1], T.v[2] }; u.depth = T.depth; units.push_back( u ); continue; }
				n = n * ( 1.0f / nl );
				// NO sign canonicalization: merging opposite-facing coplanar tris (double-sided walls)
				// double-counts their area, letting a non-convex union pass the hull-area test (measured
				// FP on cap0062 L6/c387: the hull covered a hole in the real geometry).
				float d = dot( n, T.v[0] );
				uint64_t key = ( ( uint64_t )T.owner << 40 )
							   ^ ( ( uint64_t )( int64_t )std::llround( n.x * 512 ) & 0x3FF )
							   ^ ( ( ( uint64_t )( int64_t )std::llround( n.y * 512 ) & 0x3FF ) << 10 )
							   ^ ( ( ( uint64_t )( int64_t )std::llround( n.z * 512 ) & 0x3FF ) << 20 )
							   ^ ( ( ( uint64_t )( int64_t )std::llround( d * 2.0 ) & 0xFFFF ) << 24 );
				groups[key].push_back( ( int )i );
			}
			for( auto& kv : groups )
			{
				std::vector<int>& g = kv.second;
				bool merged = false;
				// TRUE PLANARITY gate: the quantized plane key admits ~0.5u of sag, and a hull built from
				// sagging tris certifies a FLATTENED polygon that does not physically exist - the
				// full-sphere tangent slack hid it; the cell certificates exposed it (measured FP at G=12:
				// a 4-gon certifier whose real triangles do not block the ray). Require every group vert
				// within a tight absolute distance of the actual plane before merging.
				bool planarOK = g.size() >= 2;
				if( planarOK )
				{
					const Tri& T0g = tris[g[0]];
					float3 ng = normalize( cross( T0g.v[1] - T0g.v[0], T0g.v[2] - T0g.v[0] ) );
					for( size_t gi2 = 0; gi2 < g.size() && planarOK; gi2++ )
					{
						for( int k = 0; k < 3 && planarOK; k++ )
						{
							if( std::fabs( dot( ng, tris[g[gi2]].v[k] - T0g.v[0] ) ) > 0.03f ) { planarOK = false; }
						}
					}
				}
				if( planarOK )
				{
					// 2D hull of the group's verts in the plane basis; convex-union iff areas match
					const Tri& T0 = tris[g[0]];
					float3 n = normalize( cross( T0.v[1] - T0.v[0], T0.v[2] - T0.v[0] ) );
					float3 e0 = normalize( cross( ( std::fabs( n.z ) < 0.9f ) ? float3( 0, 0, 1 ) : float3( 1, 0, 0 ), n ) );
					float3 e1 = cross( n, e0 );
					std::vector<std::pair<float, float>> p2; std::vector<float3> p3;
					double triArea = 0;
					for( int gi : g )
					{
						const Tri& T = tris[gi];
						float3 cr = cross( T.v[1] - T.v[0], T.v[2] - T.v[0] );
						triArea += 0.5 * std::sqrt( dot( cr, cr ) );
						for( int k = 0; k < 3; k++ )
						{
							float u = dot( T.v[k] - T0.v[0], e0 ), v = dot( T.v[k] - T0.v[0], e1 );
							bool dup = false;
							for( auto& q : p2 ) { if( std::fabs( q.first - u ) < 1e-3f && std::fabs( q.second - v ) < 1e-3f ) { dup = true; break; } }
							if( !dup ) { p2.push_back( { u, v } ); p3.push_back( T.v[k] ); }
						}
					}
					if( p2.size() >= 3 && p2.size() <= 64 )
					{
						// monotone chain hull (indices into p2/p3)
						std::vector<int> idx( p2.size() );
						for( size_t k = 0; k < idx.size(); k++ ) { idx[k] = ( int )k; }
						std::sort( idx.begin(), idx.end(), [&]( int a, int b ) { return p2[a] < p2[b]; } );
						auto cr2 = [&]( int o, int a, int b ) { return ( double )( p2[a].first - p2[o].first ) * ( p2[b].second - p2[o].second ) - ( double )( p2[a].second - p2[o].second ) * ( p2[b].first - p2[o].first ); };
						std::vector<int> hull( 2 * idx.size() ); int hn = 0;
						for( size_t k = 0; k < idx.size(); k++ ) { while( hn >= 2 && cr2( hull[hn - 2], hull[hn - 1], idx[k] ) <= 0 ) { hn--; } hull[hn++] = idx[k]; }
						int lower = hn + 1;
						for( int k = ( int )idx.size() - 2; k >= 0; k-- ) { while( hn >= lower && cr2( hull[hn - 2], hull[hn - 1], idx[k] ) <= 0 ) { hn--; } hull[hn++] = idx[k]; }
						hn--;	// last == first
						double hullArea = 0;
						for( int k = 0; k < hn; k++ ) { int a = hull[k], b = hull[( k + 1 ) % hn]; hullArea += 0.5 * ( ( double )p2[a].first * p2[b].second - ( double )p2[b].first * p2[a].second ); }
						hullArea = std::fabs( hullArea );
						if( hn >= 3 && hn <= 12 && triArea >= hullArea * 0.999 )
						{
							Unit u; u.members = g;
							u.depth = 1e30f;
							for( int gi : g ) { u.depth = std::fmin( u.depth, tris[gi].depth ); }
							for( int k = 0; k < hn; k++ ) { u.poly.push_back( p3[hull[k]] ); }
							units.push_back( u ); merged = true;
						}
					}
				}
				if( !merged )
				{
					for( int gi : g ) { Unit u; u.members = { gi }; u.poly = { tris[gi].v[0], tris[gi].v[1], tris[gi].v[2] }; u.depth = tris[gi].depth; units.push_back( u ); }
				}
			}
		}
		std::sort( units.begin(), units.end(), []( const Unit& a, const Unit& b ) { return a.depth < b.depth; } );

		std::vector<Cert> certs; certs.reserve( 1024 );
		std::vector<uint8_t> certAlive; certAlive.reserve( 1024 );	// pass 2 retires a culled unit's cert
		std::vector<uint8_t> culled( tris.size(), 0 );
		std::vector<int> culledBy( tris.size(), -1 );
		// is triangle (a,b,c) contained in the UNION of accumulated umbras? whole-in-one-cert first; else
		// split at edge midpoints (each leaf inside ONE cert => leaf in the union; all leaves => whole tri).
		// Sound: every cert's polygon is KEPT geometry, so union containment = every ray blocked by kept tris.
		const int SUBDIV = SUBDIV_DEPTH;
		std::function<bool( float3, float3, float3, int, int& )> inUnion = [&]( float3 a, float3 b, float3 c, int depth, int& by ) -> bool
		{
			for( size_t ci = 0; ci < certs.size(); ci++ )
			{
				if( !certAlive[ci] ) { continue; }
				const Cert& C = certs[ci];
				bool maybe = true;
				float3 vv[3] = { a, b, c };
				for( int k = 0; k < 3 && maybe; k++ )
				{
					float3 dv = vv[k] - Lp; float dl = std::sqrt( dot( dv, dv ) );
					if( dl <= C.dMin || dot( dv * ( 1.0f / dl ), C.axis ) < C.cosH ) { maybe = false; }
				}
				if( !maybe ) { continue; }
				if( inCert( C, a ) && inCert( C, b ) && inCert( C, c ) ) { by = ( int )ci; return true; }
			}
			if( depth <= 0 ) { return false; }
			float3 ab = ( a + b ) * 0.5f, bc = ( b + c ) * 0.5f, ca = ( c + a ) * 0.5f;
			int dummy;
			return inUnion( a, ab, ca, depth - 1, dummy ) && inUnion( ab, b, bc, depth - 1, dummy )
				   && inUnion( ca, bc, c, depth - 1, dummy ) && inUnion( ab, bc, ca, depth - 1, by );
		};
		std::vector<int> triCert( tris.size(), -1 );		// cert index a kept tri's unit emitted (-1 = none)
		// AGGREGATE-OCCLUSION fallback (the evaluated integral): kept-so-far triangles per caster; a ray
		// blocked by ANY kept tri counts, so different blockers may cover different disk directions - the
		// combined umbra the per-certificate union test cannot see (measured: 94% of the residual FN).
		std::vector<std::vector<int>> keptOf( blockers.size() );
		auto rayBlockedByKept = [&]( float3 P, float3 tgt ) -> bool
		{
			float3 seg = tgt - P; float segL = std::sqrt( dot( seg, seg ) ); if( segL < 1e-4f ) { return true; }
			float3 dir = seg * ( 1.0f / segL );
			for( size_t b = 0; b < blockers.size(); b++ )
			{
				if( keptOf[b].empty() ) { continue; }
				if( !raySphere2( P, dir, blockers[b].c, blockers[b].r, segL ) ) { continue; }
				for( int q : keptOf[b] )
				{
					if( culled[q] ) { continue; }		// pass 2 retires culled tris immediately
					const Tri& K = tris[q];
					// Moller-Trumbore segment test (mirrors RayHitsMesh)
					float3 e1 = K.v[1] - K.v[0], e2 = K.v[2] - K.v[0], pv = cross( seg, e2 );
					float det = dot( e1, pv ); if( std::fabs( det ) < 1e-12f ) { continue; }
					float inv = 1.0f / det; float3 tv = P - K.v[0];
					float u = dot( tv, pv ) * inv; if( u < -1e-6f || u > 1.0f + 1e-6f ) { continue; }
					float3 qv = cross( tv, e1 );
					float v = dot( seg, qv ) * inv; if( v < -1e-6f || u + v > 1.0f + 1e-6f ) { continue; }
					float t = dot( e2, qv ) * inv;
					if( t > 1e-5f && t < 1.0f - 1e-5f ) { return true; }
				}
			}
			return false;
		};
		auto triFullyShadowedByKept = [&]( const Tri& T ) -> bool
		{
			// 26 surface samples x 32 disk rays: interior + tight vert/edge probes + DILATED corner probes
			// (negative barycentrics = points OUTSIDE the corners in the triangle plane; a lit corner
			// sliver extends past the corner by continuity, so the dilated probe catches what on-triangle
			// sampling structurally misses - the audit localized every escaping sliver at corners).
			static const float BC[26][3] = { {1.f/3,1.f/3,1.f/3}, {0.6f,0.2f,0.2f}, {0.2f,0.6f,0.2f}, {0.2f,0.2f,0.6f}, {0.45f,0.45f,0.1f}, {0.1f,0.45f,0.45f},
											 {0.9f,0.05f,0.05f}, {0.05f,0.9f,0.05f}, {0.05f,0.05f,0.9f}, {0.475f,0.475f,0.05f}, {0.05f,0.475f,0.475f}, {0.475f,0.05f,0.475f},
											 {0.96f,0.02f,0.02f}, {0.02f,0.96f,0.02f}, {0.02f,0.02f,0.96f}, {0.25f,0.5f,0.25f},
											 {1.12f,-0.06f,-0.06f}, {-0.06f,1.12f,-0.06f}, {-0.06f,-0.06f,1.12f}, {0.56f,0.56f,-0.12f},
											 {1.3f,-0.15f,-0.15f}, {-0.15f,1.3f,-0.15f}, {-0.15f,-0.15f,1.3f},
											 {-0.12f,0.56f,0.56f}, {0.56f,-0.12f,0.56f}, {0.65f,0.65f,-0.3f} };
			const int KD = 32;
			for( int s = 0; s < 26; s++ )
			{
				float3 P = T.v[0] * BC[s][0] + T.v[1] * BC[s][1] + T.v[2] * BC[s][2];
				float3 toL = Lp - P; float dL = std::sqrt( dot( toL, toL ) ); if( dL < 1e-3f ) { return false; }
				float3 nrm = toL * ( 1.0f / dL );
				float3 u2 = normalize( cross( ( std::fabs( nrm.z ) < 0.9f ) ? float3( 0, 0, 1 ) : float3( 1, 0, 0 ), nrm ) ), v2 = cross( nrm, u2 );
				for( int k = 0; k < KD; k++ )
				{
					float a = ( float )( 2.0 * 3.14159265358979 * k / KD );
					float rr = swR * std::sqrt( ( k + 0.5f ) / KD );
					if( !rayBlockedByKept( P, Lp + u2 * ( rr * std::cos( a ) ) + v2 * ( rr * std::sin( a ) ) ) ) { return false; }
				}
			}
			return true;
		};
		// caster index -> position in blockers[]
		std::vector<int> casToBlocker( cap.casters.size(), -1 );
		for( size_t b = 0; b < blockers.size(); b++ ) { casToBlocker[blockers[b].owner] = ( int )b; }

		// ---- CONSERVATIVE CELL AGGREGATE: cover the light SPHERE with a ball grid; unit U is cullable
		// iff EVERY cell has SOME kept unit whose certificate w.r.t. that cell-ball contains all of U's
		// verts. Different blockers per cell = the multi-blocker union, conservative in the light dimension
		// (ball inflation) and EXACT in the surface dimension (convexity + vertex containment) - no
		// sampling anywhere. Grazing blockers (no full-sphere cert) still yield per-cell certs.
		std::vector<float3> cellC; float cellRad = 0.0f;
		{
			const int G = CELLS_G;
			const float step = 2.0f * swR / G;
			cellRad = 0.5f * step * 1.7320508f;
			for( int i = 0; i < G; i++ )
				for( int j = 0; j < G; j++ )
					for( int k = 0; k < G; k++ )
					{
						float3 off( -swR + ( i + 0.5f ) * step, -swR + ( j + 0.5f ) * step, -swR + ( k + 0.5f ) * step );
						if( std::sqrt( dot( off, off ) ) <= swR + cellRad ) { cellC.push_back( Lp + off ); }
					}
		}
		std::vector<int> keptUnitIdx; std::vector<uint8_t> keptUnitAlive;
		std::vector<int> unitKeptPos( units.size(), -1 );
		std::unordered_map<uint64_t, Cert> cellCertMemo;
		std::unordered_set<uint64_t> cellCertBad;
		std::vector<int> cellLastHit( cellC.size(), -1 );
		auto cellCovered = [&]( const Unit& U, int ci ) -> bool
		{
			auto tryW = [&]( int w ) -> bool
			{
				if( w < 0 || !keptUnitAlive[w] ) { return false; }
				const uint64_t key = ( ( uint64_t )w << 24 ) | ( uint64_t )ci;
				if( cellCertBad.count( key ) ) { return false; }
				auto it = cellCertMemo.find( key );
				if( it == cellCertMemo.end() )
				{
					const Unit& W = units[keptUnitIdx[w]];
					Cert tmp;
					if( !makePolyCertAt( W.poly.data(), ( int )W.poly.size(), cellC[ci], cellRad, tmp ) )
					{
						cellCertBad.insert( key ); cellCertFails++; return false;
					}
					it = cellCertMemo.emplace( key, tmp ).first; cellCertsBuilt++;
				}
				const Cert& C = it->second;
				for( size_t k = 0; k < U.poly.size(); k++ )
				{
					float3 dv = U.poly[k] - Lp; float dl = std::sqrt( dot( dv, dv ) );
					if( dl <= C.dMin * 0.5f ) { return false; }		// loose prune only; the exact test decides
					if( !inCert( C, U.poly[k] ) ) { return false; }
				}
				return true;
			};
			if( tryW( cellLastHit[ci] ) ) { return true; }
			for( int w = ( int )keptUnitIdx.size() - 1; w >= 0; w-- )	// near-depth kept units first (later = deeper)
			{
				if( w == cellLastHit[ci] ) { continue; }
				if( tryW( w ) ) { cellLastHit[ci] = w; return true; }
			}
			return false;
		};
		auto conservativeAggregate = [&]( const Unit& U ) -> bool
		{
			for( size_t ci = 0; ci < cellC.size(); ci++ ) { if( !cellCovered( U, ( int )ci ) ) { return false; } }
			return true;
		};
		std::vector<int> triUnit( tris.size(), -1 );
		for( size_t ui = 0; ui < units.size(); ui++ ) { for( int m : units[ui].members ) { triUnit[m] = ( int )ui; } }
		// FP forensics: which cell claims to cover the escaping ray, which kept unit certified it, and does
		// that unit's polygon actually block the ray (RayHitsMesh over its fan)?
		auto diagFP = [&]( size_t ti, float3 P, float3 tgt )
		{
			int ui = triUnit[ti]; if( ui < 0 ) { std::printf( "        [diag] no unit\n" ); return; }
			const Unit& U = units[ui];
			// cells containing the target
			for( size_t ci = 0; ci < cellC.size(); ci++ )
			{
				float3 d = tgt - cellC[ci];
				if( std::sqrt( dot( d, d ) ) > cellRad ) { continue; }
				// find the certifying kept unit for this cell (re-run the scan)
				int hitW = -1;
				{
					for( int w = ( int )keptUnitIdx.size() - 1; w >= 0 && hitW < 0; w-- )
					{
						if( !keptUnitAlive[w] ) { continue; }
						const uint64_t key = ( ( uint64_t )w << 24 ) | ( uint64_t )ci;
						auto it = cellCertMemo.find( key );
						if( it == cellCertMemo.end() ) { continue; }		// only memoized certs can have certified
						const Cert& C = it->second;
						bool inside = true;
						for( size_t k = 0; k < U.poly.size() && inside; k++ ) { if( !inCert( C, U.poly[k] ) ) { inside = false; } }
						if( inside ) { hitW = w; }
					}
				}
				if( hitW < 0 ) { std::printf( "        [diag] cell %zu contains tgt but NO memoized cert covers unit -> coverage hole?!\n", ci ); continue; }
				const Unit& W = units[keptUnitIdx[hitW]];
				bool blocks = false;
				for( size_t k = 1; k + 1 < W.poly.size() && !blocks; k++ )
				{
					float wv[9] = { W.poly[0].x, W.poly[0].y, W.poly[0].z, W.poly[k].x, W.poly[k].y, W.poly[k].z, W.poly[k + 1].x, W.poly[k + 1].y, W.poly[k + 1].z };
					uint32_t wi2[3] = { 0, 1, 2 };
					if( RayHitsMesh( P, tgt - P, wv, wi2, 3 ) ) { blocks = true; }
				}
				const uint64_t dkey = ( ( uint64_t )hitW << 24 ) | ( uint64_t )ci;
				const Cert& DC = cellCertMemo.find( dkey )->second;
				std::printf( "        [diag] cell %zu certifier W(unit %d, %zu-gon) blocksRay=%d PinCert=%d Uverts=%zu | planeDot(P) %.4f | W0(%.1f,%.1f,%.1f) W2(%.1f,%.1f,%.1f) cellC(%.1f,%.1f,%.1f) r%.2f\n",
							 ci, keptUnitIdx[hitW], W.poly.size(), blocks ? 1 : 0, inCert( DC, P ) ? 1 : 0, U.poly.size(),
							 dot( DC.nt, P - DC.V0 ),
							 W.poly[0].x, W.poly[0].y, W.poly[0].z, W.poly[2].x, W.poly[2].y, W.poly[2].z, cellC[ci].x, cellC[ci].y, cellC[ci].z, cellRad );
			}
		};

		// PASS 1: certificates accumulate from kept nearer units. PASS 2+: every surviving unit re-tested
		// with ALL surviving certificates visible (pass 1 withholds a large slanted blocker's certificate
		// from victims processed before it - nearest-vert depth misorders along individual rays). Soundness
		// across passes: depth order + IMMEDIATE retirement of a culled unit's tris and certificate, so a
		// certification chain can never pass through something that is itself culled (no mutual removal).
		for( int pass = 0; pass < PASSES; pass++ )
		{
			for( size_t ui = 0; ui < units.size(); ui++ )
			{
				const Unit& U = units[ui];
				if( culled[U.members[0]] ) { continue; }		// unit already gone (members cull together)
				bool cull = false; int by = -1;
				if( U.poly.size() == 3 )
				{
					cull = inUnion( U.poly[0], U.poly[1], U.poly[2], SUBDIV, by );
				}
				else
				{
					// merged hull: fan-triangulate and require every fan triangle union-contained
					cull = true;
					for( size_t k = 1; k + 1 < U.poly.size() && cull; k++ ) { cull = inUnion( U.poly[0], U.poly[k], U.poly[k + 1], SUBDIV, by ); }
				}
				if( !cull )
				{
					if( CELLAGG )
					{
						cull = conservativeAggregate( U );
					}
					else
					{
						// legacy: sampled aggregate integral vs kept-so-far (known unsound at gate resolution)
						cull = true;
						for( int m : U.members ) { if( !triFullyShadowedByKept( tris[m] ) ) { cull = false; break; } }
					}
					if( cull ) { by = -2; }
				}
				if( cull )
				{
					for( int m : U.members ) { culled[m] = 1; culledBy[m] = by; nCulled++; culledRec += 1; }
					if( triCert[U.members[0]] >= 0 ) { certAlive[triCert[U.members[0]]] = 0; }	// retire the unit's own cert
					if( unitKeptPos[ui] >= 0 ) { keptUnitAlive[unitKeptPos[ui]] = 0; }			// retire from the cell aggregate
					continue;
				}
				if( pass == 0 )
				{
					Cert nc;
					if( makePolyCert( U.poly.data(), ( int )U.poly.size(), nc ) )
					{
						certs.push_back( nc ); certAlive.push_back( 1 );
						for( int m : U.members ) { triCert[m] = ( int )certs.size() - 1; }
					}
					unitKeptPos[ui] = ( int )keptUnitIdx.size();
					keptUnitIdx.push_back( ( int )ui ); keptUnitAlive.push_back( 1 );
					for( int m : U.members ) { int b = casToBlocker[tris[m].owner]; if( b >= 0 ) { keptOf[b].push_back( m ); } }
				}
			}
		}
		// Backface propagation DISABLED: measured on cap0062 it produced the study's ONLY false
		// positive (an open-mesh caster: front faces culled by DIFFERENT certificates union-cover the
		// front, but the backface pokes out of every single one and genuinely contributes) for a yield
		// of just 6 tris. "All front faces culled => backfaces cullable" is sound only for closed
		// manifold casters, which Doom3 soft casters are not.
		{
			long full = 0;
			for( uint32_t c : lightCas )
			{
				bool all = true; bool any = false;
				for( size_t i = 0; i < tris.size(); i++ ) { if( tris[i].owner == c ) { any = true; if( !culled[i] ) { all = false; break; } } }
				if( any && all ) { full++; }
			}
			fullCasters += full;
		}

		// ---- ray-truth: redundant(T) = every disk ray from every T-sample blocked by scene-minus-T ----
		float3 failP( 0, 0, 0 ), failTgt( 0, 0, 0 );		// first unblocked ray of the last redundant()==false
		int unblockedCnt = 0, rayTot = 0;					// filled when countAll (FP severity)
		std::function<bool( size_t, bool )> redundantC = [&]( size_t ti, bool countAll ) -> bool
		{
			const Tri& T = tris[ti];
			// DENSER than the cull test on purpose (24 samples incl. extreme vert/edge probes, 64 disk
			// rays, rotated phase): an FP audit sharing the cull's rays would pass vacuously.
			static const float BC[24][3] = { {1.f/3,1.f/3,1.f/3}, {0.6f,0.2f,0.2f}, {0.2f,0.6f,0.2f}, {0.2f,0.2f,0.6f}, {0.45f,0.45f,0.1f}, {0.1f,0.45f,0.45f},
											 {0.9f,0.05f,0.05f}, {0.05f,0.9f,0.05f}, {0.05f,0.05f,0.9f}, {0.475f,0.475f,0.05f}, {0.05f,0.475f,0.475f}, {0.475f,0.05f,0.475f},
											 {0.96f,0.02f,0.02f}, {0.02f,0.96f,0.02f}, {0.02f,0.02f,0.96f}, {0.25f,0.5f,0.25f},
											 {0.99f,0.005f,0.005f}, {0.005f,0.99f,0.005f}, {0.005f,0.005f,0.99f}, {0.495f,0.495f,0.01f}, {0.01f,0.495f,0.495f}, {0.495f,0.01f,0.495f},
											 {0.7f,0.15f,0.15f}, {0.15f,0.15f,0.7f} };
			const int KD2 = 64;
			for( int s = 0; s < 24; s++ )
			{
				float3 P = T.v[0] * BC[s][0] + T.v[1] * BC[s][1] + T.v[2] * BC[s][2];
				float3 toL = Lp - P; float dL = std::sqrt( dot( toL, toL ) ); if( dL < 1e-3f ) { return false; }
				float3 nrm = toL * ( 1.0f / dL );
				float3 u2 = normalize( cross( ( std::fabs( nrm.z ) < 0.9f ) ? float3( 0, 0, 1 ) : float3( 1, 0, 0 ), nrm ) ), v2 = cross( nrm, u2 );
				for( int k = 0; k < KD2; k++ )
				{
					float a = ( float )( 2.0 * 3.14159265358979 * ( k + 0.37 ) / KD2 );
					float rr = swR * std::sqrt( ( k + 0.5f ) / KD2 );
					float3 tgt = Lp + u2 * ( rr * std::cos( a ) ) + v2 * ( rr * std::sin( a ) );
					float3 seg = tgt - P; float segL = std::sqrt( dot( seg, seg ) ); if( segL < 1e-4f ) { continue; }
					bool blocked = false;
					for( const BElem& B : blockers )
					{
						if( !raySphere2( P, seg * ( 1.0f / segL ), B.c, B.r, segL ) ) { continue; }
						// MT over B's tris, skipping T itself (identified by owner + vertex equality)
						const uint32_t* ix = &cap.meshIdx[B.first];
						for( uint32_t i = 0; i + 2 < B.count && !blocked; i += 3 )
						{
							float3 A0( cap.meshVerts[ix[i] * 3], cap.meshVerts[ix[i] * 3 + 1], cap.meshVerts[ix[i] * 3 + 2] );
							if( B.owner == T.owner && dot( A0 - T.v[0], A0 - T.v[0] ) < 1e-10f )
							{
								float3 A1( cap.meshVerts[ix[i + 1] * 3], cap.meshVerts[ix[i + 1] * 3 + 1], cap.meshVerts[ix[i + 1] * 3 + 2] );
								float3 A2( cap.meshVerts[ix[i + 2] * 3], cap.meshVerts[ix[i + 2] * 3 + 1], cap.meshVerts[ix[i + 2] * 3 + 2] );
								if( dot( A1 - T.v[1], A1 - T.v[1] ) < 1e-10f && dot( A2 - T.v[2], A2 - T.v[2] ) < 1e-10f ) { continue; }	// T itself
							}
							if( RayHitsMesh( P, seg, cap.meshVerts.data(), &ix[i], 3 ) ) { blocked = true; }
						}
						if( blocked ) { break; }
					}
					if( countAll ) { rayTot++; if( !blocked ) { unblockedCnt++; failP = P; failTgt = tgt; } }
					else if( !blocked ) { failP = P; failTgt = tgt; return false; }
				}
			}
			return !( countAll && unblockedCnt > 0 );
		};
		auto redundant = [&]( size_t ti ) -> bool { return redundantC( ti, false ); };
		// FP: every culled triangle must be redundant. FN: strided kept triangles that are redundant.
		for( size_t i = 0; i < tris.size(); i++ )
		{
			if( culled[i] )
			{
				unblockedCnt = 0; rayTot = 0;
				if( !redundantC( i, true ) )
				{
					nFP++;
					fpSev[( unblockedCnt <= 6 ) ? 0 : ( unblockedCnt <= 24 ? 1 : ( unblockedCnt <= 96 ? 2 : 3 ) )]++;	// 96/1536 = one 1/16 walk quantum
					fpUnblockedMax = std::max( fpUnblockedMax, ( long )unblockedCnt );
					if( nFP <= 3 && culledBy[i] == -2 && std::getenv( "UMB_DIAG" ) != NULL ) { diagFP( i, failP, failTgt ); }
					if( nFP <= 8 )
					{
						std::printf( "      [FP] L%u caster c%u depth %.0f front %d culledBy %d\n", li, tris[i].owner, tris[i].depth, tris[i].front ? 1 : 0, culledBy[i] );
						if( culledBy[i] >= 0 )
						{
							const Cert& C = certs[culledBy[i]];
							float srcV[9] = { C.src[0].x, C.src[0].y, C.src[0].z, C.src[1].x, C.src[1].y, C.src[1].z, C.src[2].x, C.src[2].y, C.src[2].z };
							uint32_t srcI[3] = { 0, 1, 2 };
							bool certBlocks = RayHitsMesh( failP, failTgt - failP, srcV, srcI, 3 );
							std::printf( "        failRay P(%.1f,%.1f,%.1f)->tgt(%.1f,%.1f,%.1f) certTriBlocks=%d\n",
										 failP.x, failP.y, failP.z, failTgt.x, failTgt.y, failTgt.z, certBlocks ? 1 : 0 );
							for( int k = 0; k < 3; k++ )
							{
								float dPl = dot( C.nt, tris[i].v[k] - C.V0 );
								std::printf( "        v%d planeDot %.4f edgeDots %.4f %.4f %.4f\n", k, dPl,
											 dot( C.en[0], tris[i].v[k] - C.ea[0] ), dot( C.en[1], tris[i].v[k] - C.ea[1] ), dot( C.en[2], tris[i].v[k] - C.ea[2] ) );
							}
						}
					}
				}
			}
			else if( ( i % FN_STRIDE ) == 0 )
			{
				fnChecked++;
				if( redundant( i ) )
				{
					fnRedundant++;
					if( triFullyShadowedByKept( tris[i] ) ) { fnAchievable++; }	// the cull test WOULD pass now -> real residual miss; else structural tax
					// WHY was this redundancy missed? find the blocker of the centroid->light-centre ray and
					// classify: GRAZE (blocker plane too edge-on to emit a cert), LATER (blocker deeper than
					// T - depth order withheld its cert), CERT (blocker's cert exists - union-resolution gap).
					const Tri& T = tris[i];
					float3 P = ( T.v[0] + T.v[1] + T.v[2] ) * ( 1.0f / 3.0f );
					float3 seg = Lp - P;
					int cause = 3;		// other
					for( const BElem& B : blockers )
					{
						const uint32_t* ix = &cap.meshIdx[B.first];
						bool found = false;
						for( uint32_t bi2 = 0; bi2 + 2 < B.count && !found; bi2 += 3 )
						{
							float3 A0( cap.meshVerts[ix[bi2] * 3], cap.meshVerts[ix[bi2] * 3 + 1], cap.meshVerts[ix[bi2] * 3 + 2] );
							if( B.owner == T.owner && dot( A0 - T.v[0], A0 - T.v[0] ) < 1e-10f ) { continue; }
							if( !RayHitsMesh( P, seg, cap.meshVerts.data(), &ix[bi2], 3 ) ) { continue; }
							found = true;
							float3 B1( cap.meshVerts[ix[bi2 + 1] * 3], cap.meshVerts[ix[bi2 + 1] * 3 + 1], cap.meshVerts[ix[bi2 + 1] * 3 + 2] );
							float3 B2( cap.meshVerts[ix[bi2 + 2] * 3], cap.meshVerts[ix[bi2 + 2] * 3 + 1], cap.meshVerts[ix[bi2 + 2] * 3 + 2] );
							float3 bn = cross( B1 - A0, B2 - A0 ); float bnl = std::sqrt( dot( bn, bn ) );
							float bdL = ( bnl > 1e-9f ) ? std::fabs( dot( bn * ( 1.0f / bnl ), Lp - A0 ) ) : 0.0f;
							float bDepth = std::fmin( std::fmin( std::sqrt( dot( A0 - Lp, A0 - Lp ) ), std::sqrt( dot( B1 - Lp, B1 - Lp ) ) ), std::sqrt( dot( B2 - Lp, B2 - Lp ) ) );
							// find the blocker tri's index to look up its cert
							int bTriIdx = -1;
							for( size_t q = 0; q < tris.size(); q++ ) { if( tris[q].owner == B.owner && dot( tris[q].v[0] - A0, tris[q].v[0] - A0 ) < 1e-10f && dot( tris[q].v[1] - B1, tris[q].v[1] - B1 ) < 1e-10f ) { bTriIdx = ( int )q; break; } }
							if( bdL <= swR + 1e-3f ) { cause = 0; }
							else if( bDepth > T.depth ) { cause = 1; }
							else if( bTriIdx >= 0 && triCert[bTriIdx] >= 0 ) { cause = 2; }
							else { cause = 3; }
						}
						if( found ) { break; }
					}
					fnCause[cause]++;
				}
			}
		}
	}

	std::printf( "    [umbacc] ===== %.0f records =====\n", totRec );
	std::printf( "      CULLED (vertex-exact depth-ordered): %ld tris + %ld backface-propagated = %.0f records (%.1f%% of all) | %ld casters fully culled\n",
				 nCulled, nBack, culledRec + backRec, 100.0 * ( culledRec + backRec ) / std::fmax( 1.0, totRec ), fullCasters );
	std::printf( "      FALSE POSITIVES: %ld <- must be EXACTLY 0\n", nFP );
	std::printf( "      FALSE NEGATIVES: %ld of %ld strided kept tris are redundant (%.1f%%) -> est. missed %.1f%% of all records\n",
				 fnRedundant, fnChecked, 100.0 * fnRedundant / std::fmax( 1L, fnChecked ),
				 100.0 * ( ( double )fnRedundant / std::fmax( 1L, fnChecked ) ) * ( 1.0 - ( culledRec + backRec ) / std::fmax( 1.0, totRec ) ) );
	std::printf( "      FN cause (centroid-ray blocker): GRAZE %ld | LATER-depth %ld | CERT-exists(union gap) %ld | other %ld\n",
				 fnCause[0], fnCause[1], fnCause[2], fnCause[3] );
	std::printf( "      FP severity (unblocked of 1536 audit rays; 96 = one 1/16 walk quantum): <=6: %ld | 7-24: %ld | 25-96: %ld | >96 SUPRA-QUANTUM: %ld  (max %ld)\n",
				 fpSev[0], fpSev[1], fpSev[2], fpSev[3], fpUnblockedMax );
	std::printf( "      FN split: %ld of %ld redundant-but-kept would pass the cull test NOW (real residual); the rest is structural tax (mutual redundancy + dilation safety)\n",
				 fnAchievable, fnRedundant );
	CHECK( true );
}

// ============================================================================================================
// FAR-BAND EXTENT DISCRIMINATOR  (plan noble-sniffing-rose, Phase 0)
// The gate's 2 EXTENT defects (erebus1_05 L0, far thin strips) read anaPen=0 vs truthPen>0: the analytic
// penumbra band is ENTIRELY ABSENT. Two candidate mechanisms:
//   A - 16-sample disk floor: a far caster subtending < 1/16 of the light disk is missed by all 16 golden-
//       angle rays; coverage quantizes to 0 and the band vanishes.
//   B - caster missing from the consumed stream (would need truth and analytic to see DIFFERENT geometry).
// The capture stores the truth caster meshes FROM vLight->softShadowWedges (RenderCapture.cpp:466) - the SAME
// list the shipped GPU walk consumes - so truth and analytic share geometry by construction and B cannot
// produce a truth-only band in the gate. This study PROVES A directly: at the defect-region receiver points
// it compares the shipped 16-ray golden-angle sampler (cov16) against a dense N=32 ray truth (covDense) over
// L0's exact caster soup. A confirmed where covDense is in-band (0<cov<1) while cov16 pins to fully-lit.
//   env: FARBAND_LIGHT (default 0) | FARBAND_BBOX ("x0,y0,x1,y1;x0,y0,x1,y1" screen-space, default the two
//        erebus1_05 defect strips) | FARBAND_DENSE (dense samples/side, default 32)
STUDY_TEST( SoftShadowFarBand, discriminate )
{
	const char* path = std::getenv( "CAP" );
	if( path == NULL ) { std::printf( "    [farband] CAP unset; skipping\n" ); CHECK( true ); return; }
	Cap cap;
	if( !LoadCap( path, cap ) ) { std::printf( "    [farband] cannot load %s\n", path ); CHECK( false ); return; }

	auto envI = []( const char* k, int d ) { const char* s = std::getenv( k ); return s ? std::atoi( s ) : d; };
	const uint32_t LI    = ( uint32_t )envI( "FARBAND_LIGHT", 0 );
	const int      DENSE = envI( "FARBAND_DENSE", 32 );
	if( LI >= cap.lights.size() ) { std::printf( "    [farband] light %u absent (have %zu)\n", LI, cap.lights.size() ); CHECK( true ); return; }

	// defect bboxes (screen space); default = the two erebus1_05 L0 EXTENT strips from the gate direction print
	struct Box { int x0, y0, x1, y1; };
	std::vector<Box> boxes;
	if( const char* bs = std::getenv( "FARBAND_BBOX" ) )
	{
		int x0, y0, x1, y1; const char* p = bs;
		while( std::sscanf( p, "%d,%d,%d,%d", &x0, &y0, &x1, &y1 ) == 4 )
		{
			boxes.push_back( { x0, y0, x1, y1 } );
			const char* semi = std::strchr( p, ';' ); if( !semi ) { break; } p = semi + 1;
		}
	}
	if( boxes.empty() ) { boxes = { { 594, 133, 726, 152 }, { 440, 145, 528, 157 } }; }

	// L0's exact caster soup (the truth==analytic geometry): idx global into cap.meshVerts.
	std::vector<uint32_t> soup;
	for( const capCaster_t& cs : cap.casters )
	{
		if( cs.lightIndex != LI ) { continue; }
		for( uint32_t k = cs.firstIndex; k < cs.firstIndex + cs.numIndex && k < cap.meshIdx.size(); k++ ) { soup.push_back( cap.meshIdx[k] ); }
	}
	if( soup.empty() ) { std::printf( "    [farband] no casters for light %u\n", LI ); CHECK( true ); return; }

	// CONSUMED STREAM soup: cap.edges for this light is what the GPU walk actually consumes. The ON-DISK
	// encoding is VERSION-dependent: v5 = pure V2 tri triples (v0,triRad)(v1,0)(v2,0); pre-v5 (v1/v4) =
	// inline caster headers (e0.w<0) + triangle PAIRS ( recA.e0=v0, recA.e1=v1, recB.e1=v2 ). Parse per
	// version so we trace exactly the GPU's geometry vs the truth mesh (RenderCapture.h CAP_VERSION note;
	// mirrors FaceStreamFromV1Records in SoftShadowBox.h).
	std::vector<float>    consV;
	std::vector<uint32_t> consIdx;
	{
		const capLight_t& L = cap.lights[LI];
		auto pushTri = [&]( const float* a, const float* b, const float* c )
		{
			uint32_t base = ( uint32_t )( consV.size() / 3 );
			consV.insert( consV.end(), { a[0], a[1], a[2], b[0], b[1], b[2], c[0], c[1], c[2] } );
			consIdx.push_back( base ); consIdx.push_back( base + 1 ); consIdx.push_back( base + 2 );
		};
		if( cap.hdr.version >= 5u )
		{
			const uint32_t f4count = L.edgeCount * 2;			// float4 elements, 3 per tri
			for( uint32_t g = 0; g + 2 < f4count; g += 3 )
			{
				const float* v[3];
				for( int e = 0; e < 3; e++ )
				{
					uint32_t rec = L.firstEdge + ( g + e ) / 2;
					v[e] = ( ( g + e ) & 1 ) ? cap.edges[rec].e1 : cap.edges[rec].e0;
				}
				pushTri( v[0], v[1], v[2] );
			}
		}
		else												// v1/v4: headers (e0.w<0) + triangle pairs
		{
			for( uint32_t i = 0; i < L.edgeCount; i++ )
			{
				const capEdge_t& rA = cap.edges[L.firstEdge + i];
				if( rA.e0[3] < 0.0f ) { continue; }			// caster header
				if( i + 1 >= L.edgeCount ) { break; }
				const capEdge_t& rB = cap.edges[L.firstEdge + i + 1];
				pushTri( rA.e0, rA.e1, rB.e1 );				// v0=recA.e0, v1=recA.e1, v2=recB.e1
				i++;										// consumed recB
			}
		}
	}
	std::printf( "    [farband] L%u (v%u) truth-soup %zu tris vs consumed-stream %zu tris\n",
				 LI, cap.hdr.version, soup.size() / 3, consIdx.size() / 3 );

	const float3 Lp( cap.lights[LI].origin[0], cap.lights[LI].origin[1], cap.lights[LI].origin[2] );
	const float  swR = std::fmax( cap.lights[LI].penumbraSize, 1e-2f );

	// precompute soup tri centroid+radius for a per-point cone precull (the whole 6878-tri soup per ray is
	// intractable; only tris near the P->L axis can shadow P's disk).
	const size_t nTri = soup.size() / 3;
	std::vector<float3> triC( nTri ); std::vector<float> triR( nTri );
	for( size_t ti = 0; ti < nTri; ti++ )
	{
		float3 A( cap.meshVerts[soup[ti * 3] * 3], cap.meshVerts[soup[ti * 3] * 3 + 1], cap.meshVerts[soup[ti * 3] * 3 + 2] );
		float3 B( cap.meshVerts[soup[ti * 3 + 1] * 3], cap.meshVerts[soup[ti * 3 + 1] * 3 + 1], cap.meshVerts[soup[ti * 3 + 1] * 3 + 2] );
		float3 C( cap.meshVerts[soup[ti * 3 + 2] * 3], cap.meshVerts[soup[ti * 3 + 2] * 3 + 1], cap.meshVerts[soup[ti * 3 + 2] * 3 + 2] );
		float3 ctr = ( A + B + C ) * ( 1.0f / 3.0f );
		triC[ti] = ctr;
		triR[ti] = std::fmax( length( A - ctr ), std::fmax( length( B - ctr ), length( C - ctr ) ) );
	}
	std::vector<uint32_t> culled; culled.reserve( 512 );
	auto CulledSoup = [&]( float3 P ) -> const std::vector<uint32_t>&
	{
		culled.clear();
		float3 seg = Lp - P; float segLen2 = dot( seg, seg );
		for( size_t ti = 0; ti < nTri; ti++ )
		{
			float3 w = triC[ti] - P;
			float tproj = segLen2 > 1e-9f ? dot( w, seg ) / segLen2 : 0.0f;
			tproj = std::fmax( 0.0f, std::fmin( 1.0f, tproj ) );
			float3 closest = P + seg * tproj;
			float3 d = triC[ti] - closest;
			if( dot( d, d ) < ( triR[ti] + swR ) * ( triR[ti] + swR ) )
			{
				culled.push_back( soup[ti * 3] ); culled.push_back( soup[ti * 3 + 1] ); culled.push_back( soup[ti * 3 + 2] );
			}
		}
		return culled;
	};
	// The gate re-renders at >=1920x1080 (RenderCapture.cpp:2204) and its EXTENT bboxes are in THAT space,
	// projected via cap.hdr.worldMVP (GateProject, W=1920 H=1080) - NOT the capture's downscaled screenW/H.
	const int SW = envI( "FARBAND_W", 1920 ), SH = envI( "FARBAND_H", 1080 );
	const float* MVP = cap.hdr.worldMVP;
	auto ProjPx = [&]( float3 p, float& px, float& py ) -> bool
	{
		float x = MVP[0] * p.x + MVP[1] * p.y + MVP[2] * p.z + MVP[3];
		float y = MVP[4] * p.x + MVP[5] * p.y + MVP[6] * p.z + MVP[7];
		float w = MVP[12] * p.x + MVP[13] * p.y + MVP[14] * p.z + MVP[15];
		if( w <= 1e-6f ) { return false; }
		px = ( x / w * 0.5f + 0.5f ) * SW; py = ( 1.0f - ( y / w * 0.5f + 0.5f ) ) * SH;
		return px >= 0 && px < SW && py >= 0 && py < SH;
	};

	// SHIPPED 16-sample golden-angle sampler: fraction of the light disk NOT blocked (1=lit, 0=occluded).
	// Same swDisk coords + same L + (u*dx+v*dy)*swR mapping as SoftShadow_FaceCoverage.
	static const float SW_DISK16[16][2] =
	{
		{ 0.176777f, 0.000000f}, {-0.225772f, 0.206826f}, { 0.034558f,-0.393771f}, { 0.284571f, 0.371173f},
		{-0.522223f,-0.092374f}, { 0.494695f,-0.314685f}, {-0.165466f, 0.615525f}, {-0.315561f,-0.607594f},
		{ 0.684642f, 0.250030f}, {-0.712256f, 0.294009f}, { 0.343354f,-0.733729f}, { 0.253730f, 0.808932f},
		{-0.764746f,-0.443186f}, { 0.897134f,-0.197232f}, {-0.547507f, 0.778772f}, {-0.126487f,-0.976090f},
	};
	auto Cover16 = [&]( float3 P, const std::vector<uint32_t>& idx ) -> float
	{
		float3 toL = Lp - P; float dist = std::sqrt( dot( toL, toL ) ); if( dist < 1e-6f ) { return 1.0f; }
		float3 nrm = toL * ( 1.0f / dist );
		float3 up = ( std::fabs( nrm.z ) > 0.9f ) ? float3( 0, 1, 0 ) : float3( 0, 0, 1 );
		float3 u = normalize( cross( up, nrm ) ), v = cross( nrm, u );
		int blocked = 0;
		for( int s = 0; s < 16; s++ )
		{
			float3 Dp = Lp + u * ( SW_DISK16[s][0] * swR ) + v * ( SW_DISK16[s][1] * swR );
			if( RayHitsMesh( P, Dp - P, cap.meshVerts.data(), idx.data(), ( uint32_t )idx.size() ) ) { blocked++; }
		}
		return 1.0f - blocked / 16.0f;
	};
	// 16-ray coverage, but each triangle first passes the SHIPPED per-triangle cone/slab reject
	// (SoftShadow_FaceCoverage :999-1008) - so if this zeroes a band that Cover16 (no cull) sees, the GPU cone
	// cull is the mechanism, not the sampler. swEps = SW_NEAR_EPS.
	const float SW_NEAR_EPS_T = 0.05f;
	auto Cover16Culled = [&]( float3 P, const std::vector<uint32_t>& idx ) -> float
	{
		float3 toL = Lp - P; float distPL = std::sqrt( dot( toL, toL ) ); if( distPL < 1e-6f ) { return 1.0f; }
		float3 nrm = toL * ( 1.0f / distPL );
		float3 up = ( std::fabs( nrm.z ) > 0.9f ) ? float3( 0, 1, 0 ) : float3( 0, 0, 1 );
		float3 u = normalize( cross( up, nrm ) ), v = cross( nrm, u );
		// keep only survivors of the per-triangle cone/slab reject
		std::vector<uint32_t> surv;
		for( size_t t = 0; t + 2 < idx.size(); t += 3 )
		{
			float3 v0( cap.meshVerts[idx[t] * 3], cap.meshVerts[idx[t] * 3 + 1], cap.meshVerts[idx[t] * 3 + 2] );
			float3 v1( cap.meshVerts[idx[t + 1] * 3], cap.meshVerts[idx[t + 1] * 3 + 1], cap.meshVerts[idx[t + 1] * 3 + 2] );
			float3 v2( cap.meshVerts[idx[t + 2] * 3], cap.meshVerts[idx[t + 2] * 3 + 1], cap.meshVerts[idx[t + 2] * 3 + 2] );
			float3 tcen = ( v0 + v1 + v2 ) * ( 1.0f / 3.0f );
			float3 rc = tcen - P; float cd = dot( rc, nrm );
			float triRad = std::fmax( length( v0 - tcen ), std::fmax( length( v1 - tcen ), length( v2 - tcen ) ) );
			if( cd + triRad < SW_NEAR_EPS_T ) { continue; }
			if( cd - triRad > distPL ) { continue; }
			float3 perp = rc - cd * nrm;
			float coneR = swR * ( cd + triRad ) / distPL;
			if( dot( perp, perp ) > ( coneR + triRad ) * ( coneR + triRad ) ) { continue; }
			surv.push_back( idx[t] ); surv.push_back( idx[t + 1] ); surv.push_back( idx[t + 2] );
		}
		int blocked = 0;
		for( int s = 0; s < 16; s++ )
		{
			float3 Dp = Lp + u * ( SW_DISK16[s][0] * swR ) + v * ( SW_DISK16[s][1] * swR );
			if( RayHitsMesh( P, Dp - P, cap.meshVerts.data(), surv.data(), ( uint32_t )surv.size() ) ) { blocked++; }
		}
		return 1.0f - blocked / 16.0f;
	};

	std::printf( "    [farband] light %u  penumbra r=%.3f  casters-soup %zu tris  screen %dx%d  L0org=(%.1f %.1f %.1f)\n",
				 LI, swR, soup.size() / 3, SW, SH, Lp.x, Lp.y, Lp.z );

	// POINT mode: query one explicit world point (the fardump world pos) at rising dense-truth N + golden-16.
	// Converging to ~truth => real shallow penumbra (fix the analytic); converging to ~1.0 => the gate's
	// 16-sample truth is quantization noise (fix the arbiter).
	if( const char* pt = std::getenv( "FARBAND_POINT" ) )
	{
		float qx, qy, qz;
		if( std::sscanf( pt, "%f,%f,%f", &qx, &qy, &qz ) == 3 )
		{
			float3 P( qx, qy, qz );
			const std::vector<uint32_t>& cs = CulledSoup( P );
			std::printf( "      [point] P=(%.1f %.1f %.1f)  culled-soup %zu tris\n", qx, qy, qz, cs.size() / 3 );
			for( int N : { 4, 8, 16, 32, 64, 128 } )
			{
				float cv = cs.empty() ? 1.0f : MeshTruthShadowSoup( cap.meshVerts.data(), cs.data(), ( uint32_t )cs.size(), P, Lp, swR, N );
				std::printf( "        dense N=%-4d (~%d rays)  cover=%.4f\n", N, N * N * 785 / 1000, cv );
			}
			std::printf( "        golden-16 (shipped sampler, no rot) cover=%.4f\n", cs.empty() ? 1.0f : Cover16( P, cs ) );
			// consumed-stream trace (what the GPU walk actually sees): if this is ~1.0 while truth-mesh
			// dense is in-band, the flatten dropped the occluder tris from the consumed stream.
			if( !consIdx.empty() )
			{
				float ccD = MeshTruthShadowSoup( consV.data(), consIdx.data(), ( uint32_t )consIdx.size(), P, Lp, swR, 64 );
				std::printf( "        CONSUMED-stream dense N=64 cover=%.4f\n", ccD );
			}
			// IDENTIFY THE BLOCKER: which L0 caster shadows this point? Trace the P->L centre ray per caster,
			// print the blocking caster's index, tri count and AABB extent (small AABB = a prop/dynamic caster
			// the gate's static scene omits; large = world structure).
			float3 toLb = Lp - P; float db = std::sqrt( dot( toLb, toLb ) );
			float3 nb = toLb * ( 1.0f / db );
			float3 upb = ( std::fabs( nb.z ) > 0.9f ) ? float3( 0, 1, 0 ) : float3( 0, 0, 1 );
			float3 ub = normalize( cross( upb, nb ) ), vb = cross( nb, ub );
			for( size_t ci = 0; ci < cap.casters.size(); ci++ )
			{
				const capCaster_t& C = cap.casters[ci];
				if( C.lightIndex != LI ) { continue; }
				std::vector<uint32_t> one;
				for( uint32_t k = C.firstIndex; k < C.firstIndex + C.numIndex && k < cap.meshIdx.size(); k++ ) { one.push_back( cap.meshIdx[k] ); }
				if( one.empty() ) { continue; }
				int nHit = 0;
				for( int s = 0; s < 16; s++ )
				{
					float3 Dp = Lp + ub * ( SW_DISK16[s][0] * swR ) + vb * ( SW_DISK16[s][1] * swR );
					if( RayHitsMesh( P, Dp - P, cap.meshVerts.data(), one.data(), ( uint32_t )one.size() ) ) { nHit++; }
				}
				if( nHit == 0 ) { continue; }
				float3 mn( 1e30f, 1e30f, 1e30f ), mx( -1e30f, -1e30f, -1e30f );
				for( uint32_t vi = C.firstVert; vi < C.firstVert + C.numVerts; vi++ )
				{
					float3 p( cap.meshVerts[vi * 3], cap.meshVerts[vi * 3 + 1], cap.meshVerts[vi * 3 + 2] );
					mn = float3( std::fmin( mn.x, p.x ), std::fmin( mn.y, p.y ), std::fmin( mn.z, p.z ) );
					mx = float3( std::fmax( mx.x, p.x ), std::fmax( mx.y, p.y ), std::fmax( mx.z, p.z ) );
				}
				float3 ext = mx - mn;
				std::printf( "        BLOCKER caster[%zu] hits=%d/16 tris=%u verts=%u AABB ext=(%.1f %.1f %.1f) ctr=(%.0f %.0f %.0f)\n",
							 ci, nHit, C.numIndex / 3, C.numVerts, ext.x, ext.y, ext.z,
							 ( mn.x + mx.x ) * 0.5f, ( mn.y + mx.y ) * 0.5f, ( mn.z + mx.z ) * 0.5f );
			}
		}
	}
	std::printf( "      region        pts | dense-band 16-band both  | A-EVIDENCE(dense in-band & 16 fully-lit)  meanDense meanCov16\n" );

	for( size_t b = 0; b < boxes.size(); b++ )
	{
		const Box& bb = boxes[b];
		int nPts = 0, denseBand = 0, cov16Band = 0, bothBand = 0, aEvidence = 0, cullEvidence = 0;
		double sumDense = 0, sum16 = 0, sumCull = 0, sumCons = 0;
		// GRID-sample each L0 receiver triangle finely in world space (the penumbra strip is a few units wide
		// and falls BETWEEN sparse mesh verts - vertex sampling misses it). ~0.5-unit spacing.
		const float SPACING = 0.5f;
		for( const capReceiver_t& r : cap.receivers )
		{
			if( r.lightIndex != LI ) { continue; }
			for( uint32_t t = r.firstIndex; t + 2 < r.firstIndex + r.numIndex && t + 2 < cap.recvIdx.size(); t += 3 )
			{
				uint32_t ia = cap.recvIdx[t], ib = cap.recvIdx[t + 1], ic = cap.recvIdx[t + 2];
				float3 A( cap.recvVerts[ia * 3], cap.recvVerts[ia * 3 + 1], cap.recvVerts[ia * 3 + 2] );
				float3 B( cap.recvVerts[ib * 3], cap.recvVerts[ib * 3 + 1], cap.recvVerts[ib * 3 + 2] );
				float3 C( cap.recvVerts[ic * 3], cap.recvVerts[ic * 3 + 1], cap.recvVerts[ic * 3 + 2] );
				int su = ( int )std::fmax( 1.0f, std::ceil( length( B - A ) / SPACING ) );
				int sv = ( int )std::fmax( 1.0f, std::ceil( length( C - A ) / SPACING ) );
				for( int iu = 0; iu <= su; iu++ )
				for( int iv = 0; iv <= sv - iu * sv / su; iv++ )
			{
				float fu = ( float )iu / su, fv = ( float )iv / sv;
				if( fu + fv > 1.0f ) { continue; }
				float3 P = A + ( B - A ) * fu + ( C - A ) * fv;
				float px, py; if( !ProjPx( P, px, py ) ) { continue; }
				if( px < bb.x0 || px > bb.x1 || py < bb.y0 || py > bb.y1 ) { continue; }
				const std::vector<uint32_t>& cs = CulledSoup( P );
				if( cs.empty() ) { nPts++; sumDense += 1.0; sum16 += 1.0; sumCull += 1.0; continue; }	// nothing can shadow: fully lit
				float cd = MeshTruthShadowSoup( cap.meshVerts.data(), cs.data(), ( uint32_t )cs.size(), P, Lp, swR, DENSE );
				float c16 = Cover16( P, cs );
				if( !consIdx.empty() ) { sumCons += MeshTruthShadowSoup( consV.data(), consIdx.data(), ( uint32_t )consIdx.size(), P, Lp, swR, 8 ); }
				float c16c = Cover16Culled( P, cs );
				nPts++; sumDense += cd; sum16 += c16; sumCull += c16c;
				bool dBand = cd > 1e-3f && cd < 1.0f - 1e-3f;
				bool sBand = c16 > 1e-3f && c16 < 1.0f - 1e-3f;
				if( dBand ) { denseBand++; }
				if( sBand ) { cov16Band++; }
				if( dBand && sBand ) { bothBand++; }
				if( dBand && c16 > 1.0f - 1e-3f ) { aEvidence++; }		// dense sees a band, 16 sees fully lit -> A
				if( dBand && c16c > 1.0f - 1e-3f ) { cullEvidence++; }	// dense sees a band, 16+shipped-cull fully lit -> CULL is the mechanism
			}
		}
		}
		std::printf( "      [%3d,%3d]-[%3d,%3d] %4d | dBand=%d 16Band=%d | A-EVID(no-cull)=%d (%.0f%%) CULL-EVID=%d (%.0f%%) | mDense=%.3f m16=%.3f m16cull=%.3f\n",
					 bb.x0, bb.y0, bb.x1, bb.y1, nPts, denseBand, cov16Band,
					 aEvidence, denseBand ? 100.0 * aEvidence / denseBand : 0.0,
					 cullEvidence, denseBand ? 100.0 * cullEvidence / denseBand : 0.0,
					 nPts ? sumDense / nPts : 1.0, nPts ? sum16 / nPts : 1.0, nPts ? sumCull / nPts : 1.0 );
		std::printf( "                   -> mCons(consumed-stream, N=8)=%.3f  (truth mDense=%.3f: gap = tris dropped from stream)\n",
					 nPts ? sumCons / nPts : 1.0, nPts ? sumDense / nPts : 1.0 );
	}
	std::printf( "      VERDICT: A (16-sample floor) if A-EVIDENCE dominant; B ruled out by construction (shared geometry).\n" );
	CHECK( true );
}
