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
	rec.push_back( float4( rad, 0, 0, 0 ) );
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

// deterministic LCG so fuzz is reproducible across platforms (no <random> variance)
struct Rng
{
	unsigned s;
	Rng( unsigned seed ) : s( seed ) {}
	float f( float lo, float hi ) { s = s * 1664525u + 1013904223u; return lo + ( hi - lo ) * ( ( s >> 8 ) & 0xFFFFFF ) / float( 0x1000000 ); }
};

} // namespace swtest

#endif // __SOFTSHADOWBOX_H__
