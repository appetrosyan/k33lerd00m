/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2026 Aleksandr Petrosyan

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").

Doom 3 BFG Edition Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

===========================================================================
*/

// Offline reader + ray-cast ground truth for a soft-shadow scene capture (.cap, see
// renderer/RenderCapture.h). This is the tests-side half: it loads the POD blob with zero engine dependency,
// ray-casts the captured caster triangle meshes for TRUE per-receiver occlusion, and reconstructs receiver
// world positions from the captured depth (mirroring the engine's ReconstructWorldPos). Diffing the true
// occlusion against the shipped analytic coverage (SoftShadow_WedgeOcclusion via hlsl_compat) is how a real
// Erebus artifact is measured rather than eyeballed. Include hlsl_compat.h before this.

#ifndef __SOFTSHADOWMESH_H__
#define __SOFTSHADOWMESH_H__

#define CAP_NO_ENGINE_API		// POD structs only, no engine forward-decls
#include "../renderer/RenderCapture.h"

#include <vector>
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <string>

namespace swtest
{

// the whole capture in memory
struct Cap
{
	capHeader_t              hdr;
	std::vector<capLight_t>  lights;
	std::vector<capEdge_t>   edges;
	std::vector<capCaster_t> casters;
	std::vector<float>           meshVerts;	// float3 packed (caster meshes)
	std::vector<uint32_t>        meshIdx;
	std::vector<float>           depth;
	std::vector<capReceiver_t> receivers;	// receiver interaction surfaces
	std::vector<float>           recvVerts;	// float3 packed (receiver meshes)
	std::vector<uint32_t>        recvIdx;
	std::string                  mapName;	// v4+: the map this was captured on (for reload/reconstruct)
	int                          gameTimeMs = 0;	// v4+: hdr.reserved[0]
	std::vector<capShadowVol_t> shadowVols;	// v4+: capped shadow-volume surfs (per light)
	std::vector<float>           shadowVerts;	// v4+: float3 packed (world space, w=0 verts pre-extruded)
	std::vector<uint32_t>        shadowIdx;
	std::vector<capMaterial_t> materials;	// v5 texture tail: unique receiver materials (baked diffuse)
	std::vector<uint8_t>         texels;		// v5: RGB8 blob, indexed by capMaterial_t::firstTexel
	std::vector<float>           recvST;		// v5: float2 packed per receiver vert
	std::vector<uint32_t>        recvMat;		// v5: per receiver surface -> materials index

	// bilinear, wrapping sample of a baked material's diffuse (v5); grey when absent.
	float3 SampleAlbedo( int mat, float s, float t ) const
	{
		if( mat < 0 || mat >= ( int )materials.size() ) { return float3( 0.55f, 0.52f, 0.48f ); }
		const capMaterial_t& mrec = materials[mat];
		if( mrec.texW == 0 || mrec.texH == 0 ) { return float3( 0.5f, 0.5f, 0.5f ); }
		float fx = ( s - std::floor( s ) ) * mrec.texW - 0.5f;
		float fy = ( t - std::floor( t ) ) * mrec.texH - 0.5f;
		int x0 = ( int )std::floor( fx ), y0 = ( int )std::floor( fy );
		float wx = fx - x0, wy = fy - y0;
		float3 acc( 0, 0, 0 );
		for( int dy = 0; dy < 2; dy++ )
			for( int dx = 0; dx < 2; dx++ )
			{
				int px = ( ( x0 + dx ) % ( int )mrec.texW + mrec.texW ) % mrec.texW;
				int py = ( ( y0 + dy ) % ( int )mrec.texH + mrec.texH ) % mrec.texH;
				size_t o = ( size_t )mrec.firstTexel + ( ( size_t )py * mrec.texW + px ) * 3;
				if( o + 2 >= texels.size() ) { continue; }
				float w = ( dx ? wx : 1 - wx ) * ( dy ? wy : 1 - wy );
				acc = acc + float3( texels[o] / 255.0f, texels[o + 1] / 255.0f, texels[o + 2] / 255.0f ) * w;
			}
		return acc;
	}
};

// ----------------------------------------------------------------------------------- binary IO (round-trip)
inline bool WriteCap( const char* path, const Cap& c )
{
	FILE* f = std::fopen( path, "wb" );
	if( !f ) { return false; }
	std::fwrite( &c.hdr, sizeof( c.hdr ), 1, f );
	if( !c.lights.empty() )    { std::fwrite( c.lights.data(),    sizeof( capLight_t ),  c.lights.size(), f ); }
	if( !c.edges.empty() )     { std::fwrite( c.edges.data(),     sizeof( capEdge_t ),   c.edges.size(), f ); }
	if( !c.casters.empty() )   { std::fwrite( c.casters.data(),   sizeof( capCaster_t ), c.casters.size(), f ); }
	if( !c.meshVerts.empty() ) { std::fwrite( c.meshVerts.data(), sizeof( float ),           c.meshVerts.size(), f ); }
	if( !c.meshIdx.empty() )   { std::fwrite( c.meshIdx.data(),   sizeof( uint32_t ),        c.meshIdx.size(), f ); }
	if( !c.depth.empty() )     { std::fwrite( c.depth.data(),     sizeof( float ),           c.depth.size(), f ); }
	if( !c.receivers.empty() ) { std::fwrite( c.receivers.data(), sizeof( capReceiver_t ), c.receivers.size(), f ); }
	if( !c.recvVerts.empty() ) { std::fwrite( c.recvVerts.data(), sizeof( float ),           c.recvVerts.size(), f ); }
	if( !c.recvIdx.empty() )   { std::fwrite( c.recvIdx.data(),   sizeof( uint32_t ),        c.recvIdx.size(), f ); }
	std::fclose( f );
	return true;
}

inline bool LoadCap( const char* path, Cap& c )
{
	FILE* f = std::fopen( path, "rb" );
	if( !f ) { return false; }
	if( std::fread( &c.hdr, sizeof( c.hdr ), 1, f ) != 1 ) { std::fclose( f ); return false; }
	if( c.hdr.magic != CAP_MAGIC || c.hdr.version < 2u || c.hdr.version > CAP_VERSION ) { std::fclose( f ); return false; }
	c.lights.resize( c.hdr.numLights );
	c.edges.resize( c.hdr.numEdges );
	c.casters.resize( c.hdr.numCasters );
	c.meshVerts.resize( ( size_t )c.hdr.numMeshVerts * 3 );
	c.meshIdx.resize( c.hdr.numMeshIdx );
	c.depth.resize( c.hdr.hasDepth ? ( size_t )c.hdr.screenW * c.hdr.screenH : 0 );
	c.receivers.resize( c.hdr.numReceivers );
	c.recvVerts.resize( ( size_t )c.hdr.numRecvVerts * 3 );
	c.recvIdx.resize( c.hdr.numRecvIdx );
	bool ok = true;
	if( c.hdr.numLights )    { ok = ok && std::fread( c.lights.data(),    sizeof( capLight_t ),  c.lights.size(), f ) == c.lights.size(); }
	if( c.hdr.numEdges )     { ok = ok && std::fread( c.edges.data(),     sizeof( capEdge_t ),   c.edges.size(), f ) == c.edges.size(); }
	if( c.hdr.numCasters )   { ok = ok && std::fread( c.casters.data(),   sizeof( capCaster_t ), c.casters.size(), f ) == c.casters.size(); }
	if( c.meshVerts.size() ) { ok = ok && std::fread( c.meshVerts.data(), sizeof( float ),           c.meshVerts.size(), f ) == c.meshVerts.size(); }
	if( c.meshIdx.size() )   { ok = ok && std::fread( c.meshIdx.data(),   sizeof( uint32_t ),        c.meshIdx.size(), f ) == c.meshIdx.size(); }
	if( c.depth.size() )     { ok = ok && std::fread( c.depth.data(),     sizeof( float ),           c.depth.size(), f ) == c.depth.size(); }
	if( c.receivers.size() ) { ok = ok && std::fread( c.receivers.data(), sizeof( capReceiver_t ), c.receivers.size(), f ) == c.receivers.size(); }
	if( c.recvVerts.size() ) { ok = ok && std::fread( c.recvVerts.data(), sizeof( float ),           c.recvVerts.size(), f ) == c.recvVerts.size(); }
	if( c.recvIdx.size() )   { ok = ok && std::fread( c.recvIdx.data(),   sizeof( uint32_t ),        c.recvIdx.size(), f ) == c.recvIdx.size(); }
	c.gameTimeMs = ( int )c.hdr.reserved[0];
	if( ok && c.hdr.version >= 4u && c.hdr.reserved[1] > 0 )		// v4+: trailing MAPNAME block
	{
		c.mapName.resize( c.hdr.reserved[1] );
		ok = ok && std::fread( &c.mapName[0], 1, c.hdr.reserved[1], f ) == c.hdr.reserved[1];
	}
	if( ok && c.hdr.version >= 4u && c.hdr.reserved[2] > 0 )		// v4+: SHADOWVOL section (vols, verts, indices)
	{
		c.shadowVols.resize( c.hdr.reserved[2] );
		c.shadowVerts.resize( ( size_t )c.hdr.reserved[3] * 3 );
		c.shadowIdx.resize( c.hdr.reserved[4] );
		ok = ok && std::fread( c.shadowVols.data(), sizeof( capShadowVol_t ), c.shadowVols.size(), f ) == c.shadowVols.size();
		if( c.shadowVerts.size() ) { ok = ok && std::fread( c.shadowVerts.data(), sizeof( float ), c.shadowVerts.size(), f ) == c.shadowVerts.size(); }
		if( c.shadowIdx.size() )   { ok = ok && std::fread( c.shadowIdx.data(), sizeof( uint32_t ), c.shadowIdx.size(), f ) == c.shadowIdx.size(); }
	}
	// v5 TEXTURE TAIL (optional, self-describing): magic + counts + materials/texels/STs/matIndices
	if( ok )
	{
		uint32_t tail[5];
		if( std::fread( tail, sizeof( uint32_t ), 5, f ) == 5 && tail[0] == CAP_TAIL_MAGIC )
		{
			c.materials.resize( tail[1] );
			c.texels.resize( tail[2] );
			c.recvST.resize( tail[3] );
			c.recvMat.resize( tail[4] );
			bool tok = true;
			if( c.materials.size() ) { tok = tok && std::fread( c.materials.data(), sizeof( capMaterial_t ), c.materials.size(), f ) == c.materials.size(); }
			if( c.texels.size() )    { tok = tok && std::fread( c.texels.data(), 1, c.texels.size(), f ) == c.texels.size(); }
			if( c.recvST.size() )    { tok = tok && std::fread( c.recvST.data(), sizeof( float ), c.recvST.size(), f ) == c.recvST.size(); }
			if( c.recvMat.size() )   { tok = tok && std::fread( c.recvMat.data(), sizeof( uint32_t ), c.recvMat.size(), f ) == c.recvMat.size(); }
			if( !tok )		// truncated tail: drop it, the capture core is still valid
			{
				c.materials.clear(); c.texels.clear(); c.recvST.clear(); c.recvMat.clear();
			}
		}
	}
	std::fclose( f );

	// v2 stored per-surface LOCAL triangle indices; v3 stores them GLOBAL (offset by firstVert). Normalize
	// a v2 dump to global so all downstream code sees one convention.
	if( ok && c.hdr.version < 3u )
	{
		for( const capCaster_t& cs : c.casters )
			for( uint32_t k = cs.firstIndex; k < cs.firstIndex + cs.numIndex && k < c.meshIdx.size(); k++ ) { c.meshIdx[k] += cs.firstVert; }
		for( const capReceiver_t& R : c.receivers )
			for( uint32_t k = R.firstIndex; k < R.firstIndex + R.numIndex && k < c.recvIdx.size(); k++ ) { c.recvIdx[k] += R.firstVert; }
	}
	return ok;
}

// -------------------------------------------------------------------------- ray-cast against a triangle soup
// Moller-Trumbore: does the segment P + t*dir (0<t<1) hit any triangle of [verts,idx]? Generalizes RayHitsBox
// (SoftShadowBox.h) to an arbitrary indexed mesh. verts is float3-packed; idx are triangle vertex indices.
inline bool RayHitsMesh( float3 P, float3 dir, const float* verts, const uint32_t* idx, uint32_t numIdx )
{
	for( uint32_t i = 0; i + 2 < numIdx; i += 3 )
	{
		const uint32_t ia = idx[i], ib = idx[i + 1], ic = idx[i + 2];
		float3 A( verts[ia * 3 + 0], verts[ia * 3 + 1], verts[ia * 3 + 2] );
		float3 B( verts[ib * 3 + 0], verts[ib * 3 + 1], verts[ib * 3 + 2] );
		float3 C( verts[ic * 3 + 0], verts[ic * 3 + 1], verts[ic * 3 + 2] );
		float3 e1 = B - A, e2 = C - A;
		float3 pv = cross( dir, e2 );
		float det = dot( e1, pv );
		if( std::fabs( det ) < 1e-12f ) { continue; }
		float inv = 1.0f / det;
		float3 tv = P - A;
		float u = dot( tv, pv ) * inv;
		if( u < -1e-6f || u > 1.0f + 1e-6f ) { continue; }
		float3 qv = cross( tv, e1 );
		float v = dot( dir, qv ) * inv;
		if( v < -1e-6f || u + v > 1.0f + 1e-6f ) { continue; }
		float t = dot( e2, qv ) * inv;
		if( t > 1e-5f && t < 1.0f - 1e-5f ) { return true; }
	}
	return false;
}

// -------------------------------------------------------- stencil shadow-volume count (the eye-inside dropout)
// Reproduces the engine's stencil shadow-volume test so the harness can EXHIBIT the eye-inside failure. The
// silhouette edges, extruded away from the light L to ~infinity, form the (capless) shadow-volume walls. Along
// the camera ray C->P we accumulate signed wall crossings: Z-PASS counts crossings in front of the fragment
// (nearD < t < tP) - it MISCOUNTS when the camera sits inside the volume, because the entry wall is behind the
// near plane and gets clipped, so the umbra drops. Z-FAIL counts crossings behind the fragment (t > tP) - it is
// immune to the near plane (Carmack's reverse), which is why the z-fail core keeps the umbra solid. "In shadow"
// = count != 0. This is stencil geometry, NOT occlusion math - it needs the captured camera, which the harness
// has, so the dropout is measurable rather than eyeballed.
inline void ShadowVolStencil( const std::vector<capEdge_t>& edges, uint32_t first, uint32_t count,
							  float3 L, float3 C, float3 P, float nearD, int& zpass, int& zfail )
{
	zpass = 0; zfail = 0;
	float3 d = P - C; float tP = std::sqrt( dot( d, d ) ); if( tP < 1e-4f ) { return; } d = d * ( 1.0f / tP );
	const float BIG = 1e5f;
	for( uint32_t r = first; r < first + count && r < edges.size(); r++ )
	{
		const capEdge_t& e = edges[r];
		if( e.e0[3] < 0.0f ) { continue; }							// header record: skip
		float3 A( e.e0[0], e.e0[1], e.e0[2] ), B( e.e1[0], e.e1[1], e.e1[2] );
		float3 Ai = A + ( A - L ) * BIG, Bi = B + ( B - L ) * BIG;	// extrude away from the light to ~infinity
		float3 tri[2][3] = { { A, B, Bi }, { A, Bi, Ai } };			// wall quad as two triangles
		for( int t = 0; t < 2; t++ )
		{
			float3 v0 = tri[t][0], v1 = tri[t][1], v2 = tri[t][2];
			float3 e1v = v1 - v0, e2v = v2 - v0, pv = cross( d, e2v ); float det = dot( e1v, pv );
			if( std::fabs( det ) < 1e-9f ) { continue; }
			float inv = 1.0f / det; float3 tv = C - v0;
			float uu = dot( tv, pv ) * inv; if( uu < 0.0f || uu > 1.0f ) { continue; }
			float3 qv = cross( tv, e1v ); float vv = dot( d, qv ) * inv; if( vv < 0.0f || uu + vv > 1.0f ) { continue; }
			float tt = dot( e2v, qv ) * inv; if( tt <= 0.0f ) { continue; }
			int s = ( dot( cross( e1v, e2v ), d ) < 0.0f ) ? +1 : -1;	// front-facing crossing = +1 (entering)
			if( tt > nearD && tt < tP ) { zpass += s; }
			else if( tt >= tP ) { zfail += s; }
		}
	}
}

// Stencil count against the REAL captured capped shadow-volume triangles (verts/idx world space, w=0 verts
// already extruded). Same z-pass / z-fail split as ShadowVolStencil but on the exact geometry the engine
// rasterises - so the eye-inside dropout and the z-fail-core fix are faithfully reproduced, not approximated.
inline void ShadowVolStencilTris( const float* verts, const uint32_t* idx, uint32_t first, uint32_t numIdx,
								  float3 C, float3 P, float nearD, int& zpass, int& zfail )
{
	zpass = 0; zfail = 0;
	float3 d = P - C; float tP = std::sqrt( dot( d, d ) ); if( tP < 1e-4f ) { return; } d = d * ( 1.0f / tP );
	for( uint32_t k = first; k + 2 < first + numIdx; k += 3 )
	{
		uint32_t ia = idx[k], ib = idx[k + 1], ic = idx[k + 2];
		float3 v0( verts[ia * 3 + 0], verts[ia * 3 + 1], verts[ia * 3 + 2] );
		float3 v1( verts[ib * 3 + 0], verts[ib * 3 + 1], verts[ib * 3 + 2] );
		float3 v2( verts[ic * 3 + 0], verts[ic * 3 + 1], verts[ic * 3 + 2] );
		float3 e1v = v1 - v0, e2v = v2 - v0, pv = cross( d, e2v ); float det = dot( e1v, pv );
		if( std::fabs( det ) < 1e-9f ) { continue; }
		float inv = 1.0f / det; float3 tv = C - v0;
		float uu = dot( tv, pv ) * inv; if( uu < 0.0f || uu > 1.0f ) { continue; }
		float3 qv = cross( tv, e1v ); float vv = dot( d, qv ) * inv; if( vv < 0.0f || uu + vv > 1.0f ) { continue; }
		float tt = dot( e2v, qv ) * inv; if( tt <= 0.0f ) { continue; }
		int s = ( dot( cross( e1v, e2v ), d ) < 0.0f ) ? +1 : -1;
		if( tt > nearD && tt < tP ) { zpass += s; }
		else if( tt >= tP ) { zfail += s; }
	}
}

// -------------------------------------------------------- receiver-apex silhouette (the illusory-umbra proof)
// The wedge sums the caster's LIGHT-apex silhouette (the stencil-volume edges). The physically-correct contour
// is the caster's silhouette as seen from the RECEIVER. These helpers extract that receiver-apex silhouette
// from the caster triangle soup and sum it through the SAME projection + circle-triangle area the shader uses
// (SoftDisk_CircleTriArea), so occ_receiverSil can be compared directly against occ_lightSil and the ray-cast
// truth. If occ_receiverSil ~ truth while occ_lightSil ~ 1 at a lit point, the illusory umbra IS the wrong
// contour, and the in-shader fix is a per-edge silhouette-from-P test (needs adjacent face normals per edge).
// ------------------------------------------------------------ adjacent-normal reconstruction for the fix (path 2)
// The shipped fix keeps the engine's LIGHT-silhouette edges (robust closed loops) and, per fragment, drops the
// ones that are not a silhouette from the RECEIVER - a per-edge test needing each edge's two adjacent face
// normals. The engine has those at flatten time; here we reconstruct them offline by matching each captured
// edge to the caster mesh triangles it borders, keyed by welded endpoint positions (the captured edge
// endpoints ARE mesh vertex positions). Then we can filter the captured edges and run the LIVE game function
// on the survivors - testing exactly what would ship, with no rebuilt contour.
struct EdgeNormals { float3 n0, n1; int count; };

inline uint64_t SoftPosKey( float3 p )		// weld to a 1/16-unit grid (verts are spaced far wider than that)
{
	int64_t x = ( int64_t )std::llround( p.x * 16.0 ), y = ( int64_t )std::llround( p.y * 16.0 ), z = ( int64_t )std::llround( p.z * 16.0 );
	return ( uint64_t )( x * 73856093LL ) ^ ( uint64_t )( y * 19349663LL ) ^ ( uint64_t )( z * 83492791LL );
}
inline uint64_t SoftEdgeKey( float3 a, float3 b )
{
	uint64_t ka = SoftPosKey( a ), kb = SoftPosKey( b );
	return ka < kb ? ( ka * 1000003ULL ^ kb ) : ( kb * 1000003ULL ^ ka );
}
// Map every mesh edge -> its up-to-two adjacent face normals, keyed by welded endpoint positions.
inline void BuildEdgeNormalMap( const float* verts, const uint32_t* idx, uint32_t numIdx, std::unordered_map<uint64_t, EdgeNormals>& m )
{
	for( uint32_t i = 0; i + 2 < numIdx; i += 3 )
	{
		const uint32_t v[3] = { idx[i], idx[i + 1], idx[i + 2] };
		float3 T[3] = { float3( verts[v[0] * 3 + 0], verts[v[0] * 3 + 1], verts[v[0] * 3 + 2] ),
						float3( verts[v[1] * 3 + 0], verts[v[1] * 3 + 1], verts[v[1] * 3 + 2] ),
						float3( verts[v[2] * 3 + 0], verts[v[2] * 3 + 1], verts[v[2] * 3 + 2] ) };
		float3 n = normalize( cross( T[1] - T[0], T[2] - T[0] ) );
		for( int e = 0; e < 3; e++ )
		{
			uint64_t key = SoftEdgeKey( T[e], T[( e + 1 ) % 3] );
			auto it = m.find( key );
			if( it == m.end() ) { EdgeNormals en; en.n0 = n; en.n1 = float3( 0, 0, 0 ); en.count = 1; m[key] = en; }
			else { it->second.n1 = n; it->second.count++; }
		}
	}
}

#ifdef __SOFTWEDGE_COVERAGE_INC__		// needs SoftDisk_CircleTriArea + PI (include softwedge_coverage.inc.hlsl first)
struct TriEdgeAdj
{
	uint32_t va, vb;		// edge's two global vertex indices (for chaining silhouette edges into loops)
	float3   A, B;			// edge endpoints (world), directed as wound in the FIRST adjacent triangle
	float3   nA, nB;		// the two adjacent triangle normals (nA winds A->B; nB is the opposite triangle)
	int      count;			// adjacent triangle count (2 = interior/manifold edge)
};

// Build undirected-edge adjacency (each edge -> its up-to-two adjacent face normals) for a triangle soup,
// keyed by GLOBAL vertex index. Call PER CASTER (each object's own index range is manifold; distinct objects
// have disjoint indices, so this never fuses two objects into a non-manifold junction).
inline void BuildTriEdgeAdj( const float* verts, const uint32_t* idx, uint32_t numIdx, std::vector<TriEdgeAdj>& out )
{
	// Adjacency MUST weld by POSITION, not vertex index: game meshes duplicate verts at UV/normal seams, so
	// keying by index reports genuinely-interior edges as boundary (count 1). That inflated the receiver-apex
	// silhouette with hundreds of false boundary edges -> gross over-enclosure. Map each welded position to a
	// canonical id and key edges on that (the shipped engine's tri->silEdges are already properly welded).
	std::unordered_map<uint64_t, uint32_t> vid;
	auto canon = [&]( float3 p ) -> uint32_t
	{
		uint64_t k = SoftPosKey( p );
		auto it = vid.find( k );
		if( it != vid.end() ) { return it->second; }
		uint32_t id = ( uint32_t )vid.size(); vid[k] = id; return id;
	};
	std::unordered_map<uint64_t, int> m;
	m.reserve( numIdx );
	for( uint32_t i = 0; i + 2 < numIdx; i += 3 )
	{
		const uint32_t v[3] = { idx[i], idx[i + 1], idx[i + 2] };
		float3 T[3] = { float3( verts[v[0] * 3 + 0], verts[v[0] * 3 + 1], verts[v[0] * 3 + 2] ),
						float3( verts[v[1] * 3 + 0], verts[v[1] * 3 + 1], verts[v[1] * 3 + 2] ),
						float3( verts[v[2] * 3 + 0], verts[v[2] * 3 + 1], verts[v[2] * 3 + 2] ) };
		uint32_t cv[3] = { canon( T[0] ), canon( T[1] ), canon( T[2] ) };
		float3 n = normalize( cross( T[1] - T[0], T[2] - T[0] ) );
		for( int e = 0; e < 3; e++ )
		{
			uint32_t a = cv[e], b = cv[( e + 1 ) % 3];
			uint64_t key = a < b ? ( ( uint64_t )a << 32 | b ) : ( ( uint64_t )b << 32 | a );
			auto it = m.find( key );
			if( it == m.end() )
			{
				TriEdgeAdj t; t.va = a; t.vb = b; t.A = T[e]; t.B = T[( e + 1 ) % 3]; t.nA = n; t.nB = float3( 0, 0, 0 ); t.count = 1;
				m[key] = ( int )out.size(); out.push_back( t );
			}
			else { out[it->second].nB = n; out[it->second].count++; }
		}
	}
}

// Emit ONE caster's RECEIVER-apex silhouette as game-format edge records (a header record, e0.w<0, carrying
// the bounding sphere for the cull; then walk-ordered closed loops of edge records) APPENDED to `out`. The
// point: the occlusion is then computed by the LIVE game function SoftShadow_WedgeOcclusion run on `out` -
// projection, depth-slab clip, near-plane connector and area math are all game code; only the CONTOUR differs
// from the shipped light-apex edges. Edges are the silhouette from P (one adjacent face front, one back),
// oriented by the front face, then chained into loops by shared vertex index so the game's chain logic (which
// closes a chain when e0 != previous e1) walks each loop as one closed contour.
inline void AppendReceiverSilhouetteRecords( const std::vector<TriEdgeAdj>& adj, float3 P, std::vector<float4>& out )
{
	struct DEdge { uint32_t a, b; float3 pa, pb; };
	std::vector<DEdge> es;
	float3 lo( 1e30f, 1e30f, 1e30f ), hi( -1e30f, -1e30f, -1e30f );
	for( const TriEdgeAdj& e : adj )
	{
		DEdge d;
		if( e.count < 2 )
		{
			// BOUNDARY edge of an OPEN surface (world walls/floors are single quads): always on the
			// receiver-apex silhouette - the surface outline - regardless of facing (a thin surface occludes
			// from either side). Orient by its one face so the loop winds consistently with interior edges.
			bool fa = dot( e.nA, P - e.A ) > 0.0f;
			if( fa ) { d.a = e.va; d.b = e.vb; d.pa = e.A; d.pb = e.B; }
			else     { d.a = e.vb; d.b = e.va; d.pa = e.B; d.pb = e.A; }
		}
		else
		{
			bool fa = dot( e.nA, P - e.A ) > 0.0f, fb = dot( e.nB, P - e.A ) > 0.0f;
			if( fa == fb ) { continue; }									// not a silhouette from P
			if( !fb ) { d.a = e.va; d.b = e.vb; d.pa = e.A; d.pb = e.B; }	// orient by the front-facing triangle
			else      { d.a = e.vb; d.b = e.va; d.pa = e.B; d.pb = e.A; }
		}
		es.push_back( d );
		for( int k = 0; k < 3; k++ ) { float v = ( &d.pa.x )[k]; ( &lo.x )[k] = std::fmin( ( &lo.x )[k], v ); ( &hi.x )[k] = std::fmax( ( &hi.x )[k], v ); }
	}
	if( es.empty() ) { return; }
	float3 ctr( ( lo.x + hi.x ) * 0.5f, ( lo.y + hi.y ) * 0.5f, ( lo.z + hi.z ) * 0.5f );
	float3 ext( hi.x - lo.x, hi.y - lo.y, hi.z - lo.z ); float rad = 0.5f * std::sqrt( dot( ext, ext ) );	// tight half-diagonal (F13)
	out.push_back( float4( ctr.x, ctr.y, ctr.z, -1.0f ) );			// header: caster boundary (e0.w<0)
	out.push_back( float4( rad, 0.0f, 0.0f, 0.0f ) );				// header: bounding-sphere radius in e1.x

	// next-edge tie-break MUST match SoftShadow_ProcCaster: at a non-manifold vertex (world corner, >2
	// silhouette edges) the continuation is ambiguous, and the clipped-connector area + chain-closure winding
	// depend on WHICH loop decomposition results. ProcCaster scans the candidate buffer in adjacency-index
	// order; mirror that here (first unused es with a==cur.b, ascending index) so the chained reference and the
	// shader walk decompose loops identically - an unordered_multimap's bucket order diverged and tipped the
	// debris-drop cliff (|area|>1.2 pi r^2 -> 0) on the divergent caster.
	std::vector<char> used( es.size(), 0 );
	for( int s = 0; s < ( int )es.size(); s++ )
	{
		int cur = s;
		while( cur >= 0 && !used[cur] )								// walk one loop: cur.b -> next edge starting there
		{
			used[cur] = 1;
			out.push_back( float4( es[cur].pa.x, es[cur].pa.y, es[cur].pa.z, 1.0f ) );	// edge e0 = A (e0.w>=0)
			out.push_back( float4( es[cur].pb.x, es[cur].pb.y, es[cur].pb.z, 0.0f ) );	// edge e1 = B
			uint32_t endv = es[cur].b; int nxt = -1;
			for( int j = 0; j < ( int )es.size(); j++ ) { if( !used[j] && es[j].a == endv ) { nxt = j; break; } }
			cur = nxt;
		}
	}
}
#endif // __SOFTWEDGE_COVERAGE_INC__

// TRUE shadow over an explicit triangle soup (a specific light's caster subset), verts float3-packed, idx
// GLOBAL into verts. 1 = lit, 0 = fully occluded.
inline float MeshTruthShadowSoup( const float* verts, const uint32_t* idx, uint32_t numIdx, float3 P, float3 L, float r, int N )
{
	float3 toL = L - P;
	float dist = std::sqrt( dot( toL, toL ) );
	if( dist < 1e-6f ) { return 1.0f; }
	float3 nrm = toL * ( 1.0f / dist );
	float3 up = ( std::fabs( nrm.z ) > 0.9f ) ? float3( 0, 1, 0 ) : float3( 0, 0, 1 );
	float3 u = normalize( cross( up, nrm ) );
	float3 v = cross( nrm, u );
	int inside = 0, total = 0;
	for( int iy = 0; iy < N; iy++ )
		for( int ix = 0; ix < N; ix++ )
		{
			float du = ( ix + 0.5f ) / N * 2 - 1, dv = ( iy + 0.5f ) / N * 2 - 1;
			if( du * du + dv * dv > 1.0f ) { continue; }
			total++;
			float3 Dp = L + u * ( du * r ) + v * ( dv * r );
			if( RayHitsMesh( P, Dp - P, verts, idx, numIdx ) ) { inside++; }
		}
	return total ? 1.0f - ( float )inside / total : 1.0f;
}

// TRUE shadow (1=lit, 0=occluded): fraction of the light disk (centre L, radius r, facing P) whose ray from
// P is NOT blocked by any captured caster mesh. Mirrors TruthShadow (SoftShadowBox.h) but over the soup.
inline float MeshTruthShadow( const Cap& c, float3 P, float3 L, float r, int N = 64 )
{
	float3 toL = L - P;
	float dist = std::sqrt( dot( toL, toL ) );
	float3 nrm = toL * ( 1.0f / dist );
	float3 up = ( std::fabs( nrm.z ) > 0.9f ) ? float3( 0, 1, 0 ) : float3( 0, 0, 1 );
	float3 u = normalize( cross( up, nrm ) );
	float3 v = cross( nrm, u );
	int inside = 0, total = 0;
	for( int iy = 0; iy < N; iy++ )
		for( int ix = 0; ix < N; ix++ )
		{
			float du = ( ix + 0.5f ) / N * 2 - 1, dv = ( iy + 0.5f ) / N * 2 - 1;
			if( du * du + dv * dv > 1.0f ) { continue; }
			total++;
			float3 Dp = L + u * ( du * r ) + v * ( dv * r );
			if( RayHitsMesh( P, Dp - P, c.meshVerts.data(), c.meshIdx.data(), ( uint32_t )c.meshIdx.size() ) ) { inside++; }
		}
	return total ? 1.0f - ( float )inside / total : 1.0f;
}

// --------------------------------------------------------- receiver world position from captured depth (WIP)
// Mirrors softwedge.ps.hlsl ReconstructWorldPos: uv -> ndc -> clip (clipW from the projection Z row) ->
// world via unprojectionToWorldMatrix. NOTE: the row/column convention of the captured matrices must be
// validated against a real capture before this is trusted; kept here so Phase 4 can wire it once confirmed.
inline float3 ReconstructReceiver( const Cap& c, int x, int y )
{
	const float w = ( float )c.hdr.screenW, h = ( float )c.hdr.screenH;
	float depth = c.depth.empty() ? 0.0f : c.depth[( size_t )y * c.hdr.screenW + x];
	float uvx = ( x + 0.5f ) / w, uvy = ( y + 0.5f ) / h;
	float ndcx = uvx * 2.0f - 1.0f, ndcy = 1.0f - uvy * 2.0f, ndcz = depth;
	float projZz = c.hdr.projectionMatrix[10], projZw = c.hdr.projectionMatrix[11];
	float clipW = -projZw / ( -projZz - ndcz );
	float clip[4] = { ndcx * clipW, ndcy * clipW, ndcz * clipW, clipW };
	const float* M = c.hdr.unprojectionToWorldMatrix;
	float wx = M[0] * clip[0] + M[1] * clip[1] + M[2] * clip[2] + M[3] * clip[3];
	float wy = M[4] * clip[0] + M[5] * clip[1] + M[6] * clip[2] + M[7] * clip[3];
	float wz = M[8] * clip[0] + M[9] * clip[1] + M[10] * clip[2] + M[11] * clip[3];
	float ww = M[12] * clip[0] + M[13] * clip[1] + M[14] * clip[2] + M[15] * clip[3];
	float inv = ( std::fabs( ww ) > 1e-12f ) ? 1.0f / ww : 0.0f;
	return float3( wx * inv, wy * inv, wz * inv );
}

} // namespace swtest

#endif // __SOFTSHADOWMESH_H__
