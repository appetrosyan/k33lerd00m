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
// probe answers three numbers straight off a .softcap, OFFLINE, no renderer/GPU:
//   (1) RECORD CEILING  - fraction of caster tri-mass on casters a primitive fits within tolerance.
//   (2) COVERAGE ERROR  - real-mesh ray-truth vs proxy ray-truth over each light's penumbra receivers
//                         (= what com_softShadowGate's RT oracle would see).
//   (3) FIT MIX         - which primitive wins per caster; what stays triangles.
// Cheap axis-aligned fits first (sphere / world-AABB box / world-Z cylinder): Doom3 architecture is
// axis-aligned, so if these already proxy a big tri-mass at low error the direction is proven and OBB/PCA
// is a later refinement. Small casters (<= SMALL_TRIS) are not worth proxying and are only tallied.
//
// Run:  SOFTCAP=/path/to/foo.softcap ./rbdoom3bfg_tests @study:SoftShadowProxyFit
//   env: PROXY_TOL (penumbra mean-err ship threshold, default 0.05) | PROXY_N (disk samples/side, 16) |
//        PROXY_RECV (receiver budget/light, 80) | PROXY_MINTRIS (probe only casters over this, 64)

#include "hlsl_compat.h"
#include "SoftShadowMesh.h"					// SoftCap + LoadSoftCap + MeshTruthShadowSoup + RayHitsMesh
#include "idUnitTest.h"

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <algorithm>
#include <functional>

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
	const char* path = std::getenv( "SOFTCAP" );
	if( path == NULL )
	{
		std::printf( "    [proxyfit] SOFTCAP unset; skipping\n" );
		CHECK( true );
		return;
	}
	SoftCap cap;
	if( !LoadSoftCap( path, cap ) )
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
	for( const softcapReceiver_t& r : cap.receivers )
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
		for( const softcapReceiver_t& r : cap.receivers )
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
			const softcapCaster_t& C = cap.casters[cc];
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
		const softcapCaster_t& cs = cap.casters[c];
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

// PROXY FALLOFF QUANTIFICATION. The gate is a thresholded defect detector (0 defects != box==RT). This
// measures the ACTUAL intensity difference the box proxy introduces vs the RT oracle: for exactly the
// casters the ENGINE boxes (r_softShadowProxyBox: >64 tris AND maxGap/diag < BOXGAP), it compares
// box-proxy coverage against high-sample ray-truth on the TRUE mesh (= the RT oracle) over the penumbra,
// and reports the error DISTRIBUTION and WHERE across the penumbra gradient it diverges + the signed bias
// (does the box over- or under-shadow). Coverage is fraction-of-disk-blocked = 1 lit .. 0 umbra, so
// |box-mesh| IS the intensity error at that receiver.
//
// Run:  SOFTCAP=/path/foo.softcap ./rbdoom3bfg_tests @study:SoftShadowProxyFalloff
//   env: BOXGAP (box-ness thresh, default 0.06 = the engine cvar) | PROXY_N (disk samples/side, default 64)
STUDY_TEST( SoftShadowProxyFalloff, quantify )
{
	const char* path = std::getenv( "SOFTCAP" );
	if( path == NULL ) { std::printf( "    [falloff] SOFTCAP unset; skipping\n" ); CHECK( true ); return; }
	SoftCap cap;
	if( !LoadSoftCap( path, cap ) ) { std::printf( "    [falloff] cannot load %s\n", path ); CHECK( false ); return; }

	auto envF = []( const char* k, float d ) { const char* s = std::getenv( k ); return s ? ( float )std::atof( s ) : d; };
	auto envI = []( const char* k, int d )   { const char* s = std::getenv( k ); return s ? std::atoi( s ) : d; };
	const float BOXGAP = envF( "BOXGAP", 0.06f );		// must match r_softShadowProxyBoxGap
	const int   N      = envI( "PROXY_N", 64 );			// HIGH-sample RT oracle
	const int   MINTRIS = 64;							// must match SW_PROXY_MIN_TRIS

	// per-light receiver-centroid P set
	std::vector<std::vector<float3>> lightRecv( cap.lights.size() );
	for( const softcapReceiver_t& r : cap.receivers )
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
		const softcapCaster_t& cs = cap.casters[c];
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
// precise falloff).  Run: SOFTCAP=... ./rbdoom3bfg_tests @study:SoftShadowPrimitivePop
STUDY_TEST( SoftShadowPrimitivePop, sizing )
{
	const char* path = std::getenv( "SOFTCAP" );
	if( path == NULL ) { std::printf( "    [primpop] SOFTCAP unset; skipping\n" ); CHECK( true ); return; }
	SoftCap cap;
	if( !LoadSoftCap( path, cap ) ) { std::printf( "    [primpop] cannot load %s\n", path ); CHECK( false ); return; }
	auto envF = []( const char* k, float d ) { const char* s = std::getenv( k ); return s ? ( float )std::atof( s ) : d; };
	auto envI = []( const char* k, int d )   { const char* s = std::getenv( k ); return s ? std::atoi( s ) : d; };
	const float TOL = envF( "PROXY_TOL", 0.05f );
	const int   N   = envI( "PROXY_N", 8 );		// coarse: population, not precise falloff
	const int   RCAP = 60;

	std::vector<std::vector<float3>> lightRecv( cap.lights.size() );
	for( const softcapReceiver_t& r : cap.receivers )
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
		const softcapCaster_t& cs = cap.casters[c];
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
// yield at 0 unsafe culls.  Run: SOFTCAP=... UMBRA_SHRINK=1.0 ./rbdoom3bfg_tests @study:SoftShadowOccluderCull
STUDY_TEST( SoftShadowOccluderCull, validate )
{
	const char* path = std::getenv( "SOFTCAP" );
	if( path == NULL ) { std::printf( "    [umbracull] SOFTCAP unset; skipping\n" ); CHECK( true ); return; }
	SoftCap cap;
	if( !LoadSoftCap( path, cap ) ) { std::printf( "    [umbracull] cannot load %s\n", path ); CHECK( false ); return; }
	auto envF = []( const char* k, float d ) { const char* s = std::getenv( k ); return s ? ( float )std::atof( s ) : d; };
	const float SHRINK = envF( "UMBRA_SHRINK", 1.0f );
	const int N = 8;

	// per-light receiver centroids
	std::vector<std::vector<float3>> lightRecv( cap.lights.size() );
	for( const softcapReceiver_t& r : cap.receivers )
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
	auto buildObjUmbra = [&]( const softcapCaster_t& C, float3 Lp, float R ) -> std::vector<Plane>
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
			const softcapCaster_t& C = cap.casters[c];
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
				const softcapCaster_t& A = cap.casters[occ[certifier].first];
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
// Run: SOFTCAP=... ./rbdoom3bfg_tests @study:SoftShadowNoShadow
STUDY_TEST( SoftShadowNoShadow, classify )
{
	const char* path = std::getenv( "SOFTCAP" );
	if( path == NULL ) { std::printf( "    [noshadow] SOFTCAP unset; skipping\n" ); CHECK( true ); return; }
	SoftCap cap;
	if( !LoadSoftCap( path, cap ) ) { std::printf( "    [noshadow] cannot load %s\n", path ); CHECK( false ); return; }
	const int N = 16;

	std::vector<std::vector<float3>> lightRecv( cap.lights.size() );
	for( const softcapReceiver_t& r : cap.receivers )
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
			const softcapCaster_t& C = cap.casters[c];
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
// Run: SOFTCAP=... ./rbdoom3bfg_tests @study:SoftShadowCullCeiling
STUDY_TEST( SoftShadowCullCeiling, enumerate )
{
	const char* path = std::getenv( "SOFTCAP" );
	if( path == NULL ) { std::printf( "    [ceiling] SOFTCAP unset; skipping\n" ); CHECK( true ); return; }
	SoftCap cap;
	if( !LoadSoftCap( path, cap ) ) { std::printf( "    [ceiling] cannot load %s\n", path ); CHECK( false ); return; }
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
	for( const softcapReceiver_t& r : cap.receivers )
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
		const softcapCaster_t& C = cap.casters[c];
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
			const softcapCaster_t& C = cap.casters[c];
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
			const softcapCaster_t& D = cap.casters[casIdx[dci]];
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
			const softcapCaster_t& D = cap.casters[oc.first];
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
// Run: SOFTCAP=... ./rbdoom3bfg_tests @study:SoftShadowUmbraAccum
STUDY_TEST( SoftShadowUmbraAccum, validate )
{
	const char* path = std::getenv( "SOFTCAP" );
	if( path == NULL ) { std::printf( "    [umbacc] SOFTCAP unset; skipping\n" ); CHECK( true ); return; }
	SoftCap cap;
	if( !LoadSoftCap( path, cap ) ) { std::printf( "    [umbacc] cannot load %s\n", path ); CHECK( false ); return; }
	const int KDISK = 8;			// ground-truth disk rays per surface sample
	const int KS = 6;				// ground-truth surface samples per triangle
	const int FN_STRIDE = 7;		// kept-triangle stride for the false-negative estimate

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
			const softcapCaster_t& C = cap.casters[c];
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
		auto makePolyCert = [&]( const float3* pv, int n, Cert& out ) -> bool
		{
			if( n < 3 || n > 12 ) { return false; }
			float3 nt = cross( pv[1] - pv[0], pv[2] - pv[0] );
			float ntl = std::sqrt( dot( nt, nt ) ); if( ntl <= 1e-6f ) { return false; }
			nt = nt * ( 1.0f / ntl );
			float dL = dot( nt, Lp - pv[0] ); if( dL < 0.0f ) { nt = nt * -1.0f; dL = -dL; }
			if( dL <= swR + 1e-3f ) { return false; }
			float3 cen( 0, 0, 0 ); for( int k = 0; k < n; k++ ) { cen = cen + pv[k]; } cen = cen * ( 1.0f / n );
			for( int e = 0; e < n; e++ )
			{
				float3 Va = pv[e], Vb = pv[( e + 1 ) % n];
				float3 ed = Vb - Va; float el2 = dot( ed, ed ); if( el2 < 1e-12f ) { return false; }
				float3 u2 = ed * ( 1.0f / std::sqrt( el2 ) );
				float3 w = Lp - Va; float3 wp = w - u2 * dot( w, u2 ); float W2 = dot( wp, wp );
				if( W2 <= swR * swR + 1e-6f ) { return false; }
				float invW = 1.0f / std::sqrt( W2 ); float3 n0 = wp * invW; float3 m = cross( u2, n0 );
				float sinT = swR * invW, cosT = std::sqrt( std::fmax( 1.0f - sinT * sinT, 0.0f ) );
				float sigma = ( dot( m, cen - Va ) >= 0.0f ) ? 1.0f : -1.0f;	// interior = polygon centroid side
				out.en[e] = n0 * sinT + m * ( sigma * cosT );
				out.ea[e] = Va;
			}
			out.ne = n;
			out.nt = nt; out.V0 = pv[0];
			out.src[0] = pv[0]; out.src[1] = pv[1]; out.src[2] = pv[2];
			float3 ax = cen - Lp; float axl = std::sqrt( dot( ax, ax ) ); if( axl < 1e-4f ) { return false; }
			out.axis = ax * ( 1.0f / axl );
			out.cosH = 1.0f;
			out.dMin = dL;			// nearest possible umbra point = the light's perpendicular distance to
									// the PLANE (a vertex min under-bounds when the light sits over the
									// polygon interior -> wrongly pruned containment, measured FN source)
			for( int k = 0; k < n; k++ )
			{
				float3 dv = pv[k] - Lp; float dl = std::sqrt( dot( dv, dv ) );
				if( dl > 1e-4f ) { out.cosH = std::fmin( out.cosH, dot( dv * ( 1.0f / dl ), out.axis ) ); }
			}
			out.cosH -= 1e-3f;
			return true;
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
				// FP on softcap0062 L6/c387: the hull covered a hole in the real geometry).
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
				if( g.size() >= 2 )
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
		std::vector<uint8_t> culled( tris.size(), 0 );
		std::vector<int> culledBy( tris.size(), -1 );
		// is triangle (a,b,c) contained in the UNION of accumulated umbras? whole-in-one-cert first; else
		// split at edge midpoints (each leaf inside ONE cert => leaf in the union; all leaves => whole tri).
		// Sound: every cert's polygon is KEPT geometry, so union containment = every ray blocked by kept tris.
		const int SUBDIV = 3;
		std::function<bool( float3, float3, float3, int, int& )> inUnion = [&]( float3 a, float3 b, float3 c, int depth, int& by ) -> bool
		{
			for( size_t ci = 0; ci < certs.size(); ci++ )
			{
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

		for( const Unit& U : units )
		{
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
				// certificate miss: sampled aggregate integral vs kept-so-far geometry, with DILATED corner
				// probes in the sample set (the audit localized escaping slivers at corners).
				cull = true;
				for( int m : U.members ) { if( !triFullyShadowedByKept( tris[m] ) ) { cull = false; break; } }
				if( cull ) { by = -2; }
			}
			if( cull )
			{
				for( int m : U.members ) { culled[m] = 1; culledBy[m] = by; nCulled++; culledRec += 1; }
				continue;
			}
			Cert nc;
			if( makePolyCert( U.poly.data(), ( int )U.poly.size(), nc ) )
			{
				certs.push_back( nc );
				for( int m : U.members ) { triCert[m] = ( int )certs.size() - 1; }
			}
			for( int m : U.members ) { int b = casToBlocker[tris[m].owner]; if( b >= 0 ) { keptOf[b].push_back( m ); } }
		}
		// Backface propagation DISABLED: measured on softcap0062 it produced the study's ONLY false
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
	CHECK( true );
}
