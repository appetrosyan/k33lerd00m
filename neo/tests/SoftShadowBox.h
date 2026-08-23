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

// Ground-truth machinery for the soft-shadow coverage tests: a real BOX caster (which, unlike a planar
// quad, can cross the receiver near-plane on OPPOSITE sides - the case that produces the deep-umbra
// artifact), its light-relative silhouette, and an INDEPENDENT ray-cast disk coverage. The analytic
// coverage under test never appears here, so the truth doesn't presuppose the math it validates.
//
// Depends on hlsl_compat.h (float3, dot/cross/normalize) being included first.

#ifndef __SOFTSHADOWBOX_H__
#define __SOFTSHADOWBOX_H__

#include <vector>
#include <array>
#include <map>
#include <cmath>
#include <cstdint>
#include <utility>

namespace swtest
{

inline float3 v3( float x, float y, float z ) { return float3( x, y, z ); }
inline float  len3( float3 a ) { return std::sqrt( dot( a, a ) ); }

struct Box
{
	float3 c[8];
	std::array<std::array<int, 4>, 6> f;	// 6 faces, each 4 corner indices, oriented outward
};

// axis-aligned box, then rotated by (yaw about +z, pitch about +x); faces auto-oriented outward.
inline Box MakeBox( float3 C, float3 h, float yaw = 0.0f, float pitch = 0.0f )
{
	float cy = std::cos( yaw ), sy = std::sin( yaw ), cp = std::cos( pitch ), sp = std::sin( pitch );
	float Rx[9] = { 1, 0, 0, 0, cp, -sp, 0, sp, cp };
	float Ry[9] = { cy, 0, sy, 0, 1, 0, -sy, 0, cy };
	float M[9];
	for( int r = 0; r < 3; r++ )
		for( int cc = 0; cc < 3; cc++ )
		{
			float s = 0;
			for( int k = 0; k < 3; k++ ) { s += Ry[r * 3 + k] * Rx[k * 3 + cc]; }
			M[r * 3 + cc] = s;
		}
	Box b;
	for( int i = 0; i < 8; i++ )
	{
		float3 s = v3( ( i & 1 ) ? h.x : -h.x, ( i & 2 ) ? h.y : -h.y, ( i & 4 ) ? h.z : -h.z );
		float3 r = v3( M[0] * s.x + M[1] * s.y + M[2] * s.z, M[3] * s.x + M[4] * s.y + M[5] * s.z, M[6] * s.x + M[7] * s.y + M[8] * s.z );
		b.c[i] = C + r;
	}
	std::array<std::array<int, 4>, 6> f = {{ {{0, 2, 6, 4}}, {{1, 3, 7, 5}}, {{0, 4, 5, 1}}, {{2, 3, 7, 6}}, {{0, 1, 3, 2}}, {{4, 5, 7, 6}} }};
	for( int i = 0; i < 6; i++ )
	{
		float3 v0 = b.c[f[i][0]], v1 = b.c[f[i][1]], v2 = b.c[f[i][2]];
		float3 n = cross( v1 - v0, v2 - v0 );
		float3 fc = ( b.c[f[i][0]] + b.c[f[i][1]] + b.c[f[i][2]] + b.c[f[i][3]] ) * 0.25f;
		if( dot( n, fc - C ) < 0 ) { std::swap( f[i][1], f[i][3] ); }
		b.f[i] = f[i];
	}
	return b;
}

// silhouette loop of the box as seen from viewpoint W (the LIGHT, matching R_CollectPenumbraEdges), as a
// single walk-ordered closed loop of world vertices, oriented by the front-face boundary.
inline std::vector<float3> Silhouette( const Box& b, float3 W )
{
	bool front[6];
	for( int i = 0; i < 6; i++ )
	{
		float3 v0 = b.c[b.f[i][0]], v1 = b.c[b.f[i][1]], v2 = b.c[b.f[i][2]];
		float3 n = cross( v1 - v0, v2 - v0 );
		float3 fc = ( v0 + v1 + v2 + b.c[b.f[i][3]] ) * 0.25f;
		front[i] = dot( n, W - fc ) > 0;
	}
	std::map<std::pair<int, int>, std::vector<int>> em;
	for( int i = 0; i < 6; i++ )
		for( int e = 0; e < 4; e++ )
		{
			int a = b.f[i][e], c = b.f[i][( e + 1 ) % 4];
			em[{std::min( a, c ), std::max( a, c )}].push_back( i );
		}
	std::vector<std::pair<int, int>> dir;
	for( int i = 0; i < 6; i++ )
	{
		if( !front[i] ) { continue; }
		for( int e = 0; e < 4; e++ )
		{
			int a = b.f[i][e], c = b.f[i][( e + 1 ) % 4];
			auto& fs = em[{std::min( a, c ), std::max( a, c )}];
			int o = fs[0] == i ? fs[1] : fs[0];
			if( !front[o] ) { dir.push_back( {a, c} ); }
		}
	}
	std::vector<float3> loop;
	if( dir.empty() ) { return loop; }
	std::vector<bool> used( dir.size(), false );
	used[0] = true;
	int startV = dir[0].first, v = dir[0].second;
	loop.push_back( b.c[dir[0].first] );
	for( size_t s = 0; s < dir.size(); s++ )
	{
		loop.push_back( b.c[v] );
		if( v == startV ) { break; }
		int nx = -1;
		for( size_t k = 0; k < dir.size(); k++ ) if( !used[k] && dir[k].first == v ) { nx = ( int )k; break; }
		if( nx < 0 ) { break; }
		used[nx] = true;
		v = dir[nx].second;
	}
	if( !loop.empty() ) { loop.pop_back(); }		// last push duplicates the start
	return loop;
}

// Does the open segment P -> P+dir (t in (0,1)) intersect the (convex) box SOLID? A segment that STARTS
// (or ends) inside the solid is blocked - the old `tmin > 1e-5` return reported a P inside the box as
// unblocked, which lit-biased every contact-shadow truth value (finding F8).
inline bool RayHitsBox( float3 P, float3 dir, const Box& b )
{
	float tmin = 0, tmax = 1;
	for( int i = 0; i < 6; i++ )
	{
		float3 v0 = b.c[b.f[i][0]], v1 = b.c[b.f[i][1]], v2 = b.c[b.f[i][2]];
		float3 n = cross( v1 - v0, v2 - v0 );
		float den = dot( n, dir ), num = dot( n, v0 - P );
		if( std::fabs( den ) < 1e-12f ) { if( num < 0 ) { return false; } continue; }
		float t = num / den;
		if( den < 0 ) { if( t > tmin ) { tmin = t; } } else { if( t < tmax ) { tmax = t; } }
		if( tmin > tmax ) { return false; }
	}
	return tmax > 1e-5f && tmin < 1.0f - 1e-5f;		// non-empty overlap of [tmin,tmax] with the open (0,1)
}

// ground-truth SHADOW (1=lit, 0=occluded): fraction of the light disk (centre L, radius r, facing P)
// whose ray from P is NOT blocked by the box.
inline float TruthShadow( float3 P, float3 L, float r, const Box& b, int N = 96 )
{
	float3 toL = L - P;
	float dist = len3( toL );
	float3 nrm = toL * ( 1.0f / dist );
	float3 up = ( std::fabs( nrm.z ) > 0.9f ) ? v3( 0, 1, 0 ) : v3( 0, 0, 1 );
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
			if( RayHitsBox( P, Dp - P, b ) ) { inside++; }
		}
	return total ? 1.0f - ( float )inside / total : 1.0f;
}

// flatten a set of closed world loops into the t_SoftEdges record layout (one caster: header + edges).
// The header sphere is the TIGHT half-diagonal bound (the old full-diagonal radius was 2x oversized,
// which made every cull-sensitivity failure untestable by construction - finding F13). Loops may be
// empty (a silhouette can come back empty, e.g. viewpoint inside the caster); if NO vertices exist at
// all the function returns an empty record list rather than a garbage +-1e30 header (finding F9).
inline std::vector<float4> BuildCaster( const std::vector<std::vector<float3>>& loops )
{
	float3 lo = v3( 1e30f, 1e30f, 1e30f ), hi = v3( -1e30f, -1e30f, -1e30f );
	int nVerts = 0;
	for( auto& l : loops )
		for( float3 p : l )
		{
			nVerts++;
			lo = v3( std::fmin( lo.x, p.x ), std::fmin( lo.y, p.y ), std::fmin( lo.z, p.z ) );
			hi = v3( std::fmax( hi.x, p.x ), std::fmax( hi.y, p.y ), std::fmax( hi.z, p.z ) );
		}
	std::vector<float4> rec;
	if( nVerts == 0 ) { return rec; }
	float3 c = ( lo + hi ) * 0.5f;
	float rad = 0.5f * len3( hi - lo );
	rec.push_back( float4( c.x, c.y, c.z, -1.0f ) );
	rec.push_back( float4( rad, ( float )nVerts, 0, 0 ) );	// e1.y = edgeCount (closed loops -> one edge per vertex), for shader jump-skip
	for( auto& l : loops )
	{
		int m = ( int )l.size();
		for( int i = 0; i < m; i++ )
		{
			float3 A = l[i], B = l[( i + 1 ) % m];
			rec.push_back( float4( A.x, A.y, A.z, 0 ) );
			rec.push_back( float4( B.x, B.y, B.z, 0 ) );
		}
	}
	return rec;
}

// call the LIVE shader coverage (softwedge_coverage.inc.hlsl) -> shadow in [0,1].
inline float LiveShadow( const std::vector<float4>& rec, float3 P, float3 L, float r )
{
	SoftEdgeBuffer buf{ rec.data(), ( int )rec.size() };
	return 1.0f - saturate( SoftShadow_WedgeOcclusion( P, L, r, 0, ( int )( rec.size() / 2 ), 0.0f, buf ) );
}

// per-triangle bounding radius (max |vertex - centroid|), a hair inflated so the shader's cone cull stays
// CONSERVATIVE (never drops a real occluder) even against GPU float rounding. Precomputed here and carried in
// recA.e0.w, so SoftShadow_FaceCoverage reads it instead of recomputing it every fragment. Must match the
// engine's R_CollectPenumbraFaces so the C++-compiled shader in the tests sees the same value the GPU would.
// v0-CENTERED radius (coarse), stored in r0.w: the fragment cull reads only r0 to reject, loading
// v1/v2 lazily for survivors. Matches Interaction.cpp's v0Rad. The v0-sphere contains the triangle.
inline float SoftTriRadV0( float3 v0, float3 v1, float3 v2 )
{
	float r = std::fmax( len3( v1 - v0 ), len3( v2 - v0 ) );
	return r * 1.00001f;
}

// CENTROID radius (tight), stored in r1.w: the exact original bound; survivors re-cull with it.
inline float SoftTriRad( float3 v0, float3 v1, float3 v2 )
{
	float3 cen = ( v0 + v1 + v2 ) * ( 1.0f / 3.0f );
	float r = std::fmax( len3( v0 - cen ), std::fmax( len3( v1 - cen ), len3( v2 - cen ) ) );
	return r * 1.00001f;
}

// ----------------------------------------------------------------------------------- FACE stream v2
// One caster's face-coverage data: bounding sphere + a pure triangle stream (3 float4 per triangle,
// ( v0, triRad ) ( v1, 0 ) ( v2, 0 )) - EXACTLY what R_CollectPenumbraFaces emits, so tests exercise
// the shipped buffer format, not a bespoke one.
struct FaceCasterCPU
{
	float4 sphere;										// ( centre.xyz, radius )
	std::vector<float4> tris;							// 3 float4 per triangle
	std::vector<std::pair<float3, float3>> shellEdges;	// mesh builder only (legacy band diagnostic)
	float3 centre;
};

// A light's COMBINED face stream in the PACKED v2 layout the shader consumes:
// buf = [ caster table: 2 float4 x nCasters ] [ tri stream: 3 float4 x tris ]. Caster table entry =
// ( centre, radius ) ( firstTri, numTris, 0, 0 ); triangle t lives at element 2*nCasters + t*3.
// Append() keeps the layout packed (table insert + tri append); the shipped flatten builds the same
// two regions in the joint buffer.
struct FaceStreamCPU
{
	std::vector<float4> buf;
	int nCasters = 0;

	void Append( const FaceCasterCPU& fc )
	{
		const int firstTri = ( ( int )buf.size() - nCasters * 2 ) / 3;
		const float4 tab1( ( float )firstTri, ( float )( fc.tris.size() / 3 ), 0, 0 );
		buf.insert( buf.begin() + nCasters * 2, { fc.sphere, tab1 } );
		buf.insert( buf.end(), fc.tris.begin(), fc.tris.end() );
		nCasters++;
	}
	bool empty() const
	{
		return nCasters == 0;
	}
	int triBase() const
	{
		return nCasters * 2;	// float4 element where the tri stream starts (caster table before it)
	}
};

// Pack a box into ONE caster's face data (two outward tris per box quad; MakeBox orients faces).
inline FaceCasterCPU BuildFaceCasterUnit( const Box& b )
{
	FaceCasterCPU fc;
	float3 c( 0, 0, 0 );
	for( int i = 0; i < 8; i++ ) { c = c + b.c[i]; }
	c = c * ( 1.0f / 8.0f );
	float rad = 0;
	for( int i = 0; i < 8; i++ ) { float3 d = b.c[i] - c; rad = std::fmax( rad, len3( d ) ); }
	fc.sphere = float4( c.x, c.y, c.z, rad );
	fc.centre = c;
	for( int f = 0; f < 6; f++ )
	{
		int idx[6] = { b.f[f][0], b.f[f][1], b.f[f][2], b.f[f][0], b.f[f][2], b.f[f][3] };	// two outward tris
		for( int t = 0; t < 2; t++ )
		{
			float3 v0 = b.c[idx[t * 3 + 0]], v1 = b.c[idx[t * 3 + 1]], v2 = b.c[idx[t * 3 + 2]];
			fc.tris.push_back( float4( v0.x, v0.y, v0.z, SoftTriRadV0( v0, v1, v2 ) ) );
			fc.tris.push_back( float4( v1.x, v1.y, v1.z, SoftTriRad( v0, v1, v2 ) ) );
			fc.tris.push_back( float4( v2.x, v2.y, v2.z, 0 ) );
		}
	}
	return fc;
}

// single-caster FACE stream from a box
inline FaceStreamCPU BuildFaceCaster( const Box& b )
{
	FaceStreamCPU s;
	s.Append( BuildFaceCasterUnit( b ) );
	return s;
}

// The LEGACY per-fragment rotation angle (position hash). The shipped pixel shader now samples blue
// noise instead (screen-anchored decorrelation - the hash left the 1/16 coverage quanta visible as
// contour bands); the tests keep the hash so every recorded expected value stays bit-exact, and
// because they have no screen position to sample noise at.
inline float SoftRotAngle( float3 P )
{
	float h = dot( P, float3( 12.9898f, 78.233f, 37.719f ) );
	return ( h - std::floor( h ) ) * 6.2831853f;
}

// front-face coverage (r_softShadowFaceCoverage path) as an occlusion in [0,1] from a face stream.
inline float FaceOcclusion( const FaceStreamCPU& s, float3 P, float3 L, float r )
{
	SoftEdgeBuffer buf{ s.buf.data(), ( int )s.buf.size() };
	return saturate( SoftShadow_FaceCoverage( P, L, r, s.triBase(), 0, s.nCasters, SoftRotAngle( P ), buf ) );
}

// Convert a LEGACY v1 record blob (inline caster headers e0.w<0 + triangle PAIRS, as stored in
// pre-v5 .cap edge sections) into the v2 stream. Coverage is IDENTICAL: caster culls are
// conservative, so the caster grouping only affects cost, never the union mask.
inline FaceStreamCPU FaceStreamFromV1Records( const float4* recs, int firstElem, int numRecords )
{
	FaceStreamCPU s;
	FaceCasterCPU cur;
	bool open = false;
	for( int i = 0; i < numRecords; i++ )
	{
		const float4& e0 = recs[firstElem + i * 2 + 0];
		const float4& e1 = recs[firstElem + i * 2 + 1];
		if( e0.w < 0.0f )		// header: ( centre, -1 ) ( radius, recordCount )
		{
			if( open ) { s.Append( cur ); }
			cur = FaceCasterCPU();
			cur.sphere = float4( e0.x, e0.y, e0.z, e1.x );
			cur.centre = float3( e0.x, e0.y, e0.z );
			open = true;
			continue;
		}
		if( i + 1 >= numRecords ) { break; }			// malformed tail (needs recB)
		const float4& g1 = recs[firstElem + ( i + 1 ) * 2 + 1];
		if( !open )										// records before any header: synthesize a caster
		{
			cur = FaceCasterCPU();
			cur.sphere = float4( e0.x, e0.y, e0.z, 1e9f );	// no sphere known: never culled (conservative)
			open = true;
		}
		cur.tris.push_back( float4( e0.x, e0.y, e0.z, e0.w ) );	// recA.e0 = ( v0, triRad )
		cur.tris.push_back( float4( e1.x, e1.y, e1.z, 0 ) );
		cur.tris.push_back( float4( g1.x, g1.y, g1.z, 0 ) );
		i++;											// consumed recB
	}
	if( open ) { s.Append( cur ); }
	return s;
}

// Build one caster's face data from mesh triangles (real capture geometry, as opposed to
// BuildFaceCaster's single box). shellEdges carries the tri edges (legacy band diagnostic).
// Shared by every pipeline test so they all feed the shipped stream, not a bespoke one.
inline FaceCasterCPU BuildFaceCasterFromMesh( const float* verts, const uint32_t* idx, uint32_t first, uint32_t num )
{
	FaceCasterCPU fc;
	float3 lo( 1e30f, 1e30f, 1e30f ), hi( -1e30f, -1e30f, -1e30f );
	for( uint32_t k = first; k < first + num; k++ )
	{
		const float* v = &verts[idx[k] * 3];
		lo = float3( std::fmin( lo.x, v[0] ), std::fmin( lo.y, v[1] ), std::fmin( lo.z, v[2] ) );
		hi = float3( std::fmax( hi.x, v[0] ), std::fmax( hi.y, v[1] ), std::fmax( hi.z, v[2] ) );
	}
	fc.centre = ( lo + hi ) * 0.5f;
	float rad = 0.5f * std::sqrt( dot( hi - lo, hi - lo ) );
	fc.sphere = float4( fc.centre.x, fc.centre.y, fc.centre.z, rad );
	for( uint32_t t = first; t + 2 < first + num; t += 3 )
	{
		const float* p0 = &verts[idx[t + 0] * 3];
		const float* p1 = &verts[idx[t + 1] * 3];
		const float* p2 = &verts[idx[t + 2] * 3];
		float3 v0( p0[0], p0[1], p0[2] ), v1( p1[0], p1[1], p1[2] ), v2( p2[0], p2[1], p2[2] );
		fc.tris.push_back( float4( v0.x, v0.y, v0.z, SoftTriRadV0( v0, v1, v2 ) ) );
		fc.tris.push_back( float4( v1.x, v1.y, v1.z, SoftTriRad( v0, v1, v2 ) ) );
		fc.tris.push_back( float4( v2.x, v2.y, v2.z, 0 ) );
		fc.shellEdges.push_back( { v0, v1 } );
		fc.shellEdges.push_back( { v1, v2 } );
	}
	return fc;
}

// deterministic LCG so fuzz is reproducible across platforms (no <random> variance)
struct Rng
{
	unsigned s;
	Rng( unsigned seed ) : s( seed ) {}
	float f( float lo, float hi ) { s = s * 1664525u + 1013904223u; return lo + ( hi - lo ) * ( ( s >> 8 ) & 0xFFFFFF ) / float( 0x1000000 ); }
};

} // namespace swtest

#endif // __SOFTSHADOWBOX_H__
