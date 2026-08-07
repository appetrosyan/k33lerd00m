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

// Offline reader + ray-cast ground truth for a soft-shadow scene capture (.softcap, see
// renderer/RenderCapture.h). This is the tests-side half: it loads the POD blob with zero engine dependency,
// ray-casts the captured caster triangle meshes for TRUE per-receiver occlusion, and reconstructs receiver
// world positions from the captured depth (mirroring the engine's ReconstructWorldPos). Diffing the true
// occlusion against the shipped analytic coverage (SoftShadow_WedgeOcclusion via hlsl_compat) is how a real
// Erebus artifact is measured rather than eyeballed. Include hlsl_compat.h before this.

#ifndef __SOFTSHADOWMESH_H__
#define __SOFTSHADOWMESH_H__

#define SOFTCAP_NO_ENGINE_API		// POD structs only, no engine forward-decls
#include "../renderer/RenderCapture.h"

#include <vector>
#include <cstdio>
#include <cmath>

namespace swtest
{

// the whole capture in memory
struct SoftCap
{
	softcapHeader_t              hdr;
	std::vector<softcapLight_t>  lights;
	std::vector<softcapEdge_t>   edges;
	std::vector<softcapCaster_t> casters;
	std::vector<float>           meshVerts;	// float3 packed (caster meshes)
	std::vector<uint32_t>        meshIdx;
	std::vector<float>           depth;
	std::vector<softcapReceiver_t> receivers;	// receiver interaction surfaces
	std::vector<float>           recvVerts;	// float3 packed (receiver meshes)
	std::vector<uint32_t>        recvIdx;
};

// ----------------------------------------------------------------------------------- binary IO (round-trip)
inline bool WriteSoftCap( const char* path, const SoftCap& c )
{
	FILE* f = std::fopen( path, "wb" );
	if( !f ) { return false; }
	std::fwrite( &c.hdr, sizeof( c.hdr ), 1, f );
	if( !c.lights.empty() )    { std::fwrite( c.lights.data(),    sizeof( softcapLight_t ),  c.lights.size(), f ); }
	if( !c.edges.empty() )     { std::fwrite( c.edges.data(),     sizeof( softcapEdge_t ),   c.edges.size(), f ); }
	if( !c.casters.empty() )   { std::fwrite( c.casters.data(),   sizeof( softcapCaster_t ), c.casters.size(), f ); }
	if( !c.meshVerts.empty() ) { std::fwrite( c.meshVerts.data(), sizeof( float ),           c.meshVerts.size(), f ); }
	if( !c.meshIdx.empty() )   { std::fwrite( c.meshIdx.data(),   sizeof( uint32_t ),        c.meshIdx.size(), f ); }
	if( !c.depth.empty() )     { std::fwrite( c.depth.data(),     sizeof( float ),           c.depth.size(), f ); }
	if( !c.receivers.empty() ) { std::fwrite( c.receivers.data(), sizeof( softcapReceiver_t ), c.receivers.size(), f ); }
	if( !c.recvVerts.empty() ) { std::fwrite( c.recvVerts.data(), sizeof( float ),           c.recvVerts.size(), f ); }
	if( !c.recvIdx.empty() )   { std::fwrite( c.recvIdx.data(),   sizeof( uint32_t ),        c.recvIdx.size(), f ); }
	std::fclose( f );
	return true;
}

inline bool LoadSoftCap( const char* path, SoftCap& c )
{
	FILE* f = std::fopen( path, "rb" );
	if( !f ) { return false; }
	if( std::fread( &c.hdr, sizeof( c.hdr ), 1, f ) != 1 ) { std::fclose( f ); return false; }
	if( c.hdr.magic != SOFTCAP_MAGIC || c.hdr.version < 2u || c.hdr.version > SOFTCAP_VERSION ) { std::fclose( f ); return false; }
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
	if( c.hdr.numLights )    { ok = ok && std::fread( c.lights.data(),    sizeof( softcapLight_t ),  c.lights.size(), f ) == c.lights.size(); }
	if( c.hdr.numEdges )     { ok = ok && std::fread( c.edges.data(),     sizeof( softcapEdge_t ),   c.edges.size(), f ) == c.edges.size(); }
	if( c.hdr.numCasters )   { ok = ok && std::fread( c.casters.data(),   sizeof( softcapCaster_t ), c.casters.size(), f ) == c.casters.size(); }
	if( c.meshVerts.size() ) { ok = ok && std::fread( c.meshVerts.data(), sizeof( float ),           c.meshVerts.size(), f ) == c.meshVerts.size(); }
	if( c.meshIdx.size() )   { ok = ok && std::fread( c.meshIdx.data(),   sizeof( uint32_t ),        c.meshIdx.size(), f ) == c.meshIdx.size(); }
	if( c.depth.size() )     { ok = ok && std::fread( c.depth.data(),     sizeof( float ),           c.depth.size(), f ) == c.depth.size(); }
	if( c.receivers.size() ) { ok = ok && std::fread( c.receivers.data(), sizeof( softcapReceiver_t ), c.receivers.size(), f ) == c.receivers.size(); }
	if( c.recvVerts.size() ) { ok = ok && std::fread( c.recvVerts.data(), sizeof( float ),           c.recvVerts.size(), f ) == c.recvVerts.size(); }
	if( c.recvIdx.size() )   { ok = ok && std::fread( c.recvIdx.data(),   sizeof( uint32_t ),        c.recvIdx.size(), f ) == c.recvIdx.size(); }
	std::fclose( f );

	// v2 stored per-surface LOCAL triangle indices; v3 stores them GLOBAL (offset by firstVert). Normalize
	// a v2 dump to global so all downstream code sees one convention.
	if( ok && c.hdr.version < 3u )
	{
		for( const softcapCaster_t& cs : c.casters )
			for( uint32_t k = cs.firstIndex; k < cs.firstIndex + cs.numIndex && k < c.meshIdx.size(); k++ ) { c.meshIdx[k] += cs.firstVert; }
		for( const softcapReceiver_t& R : c.receivers )
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
inline float MeshTruthShadow( const SoftCap& c, float3 P, float3 L, float r, int N = 64 )
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
inline float3 ReconstructReceiver( const SoftCap& c, int x, int y )
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
