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

// Generator for the soft-shadow coverage microbench scene (see SoftShadowCoverageBench_scene.h).
//
// SELF-CONTAINED by design: the box geometry, light-relative silhouette, and t_SoftEdges record layout are
// ported here verbatim from the algorithm in neo/tests/SoftShadowBox.h (swtest::MakeBox / Silhouette /
// BuildCaster) rather than #included from it. The test headers declare global scalar intrinsics
// (abs/sqrt/atan2) and shader-coupled helpers (LiveShadow/FaceOcclusion) that collide with the engine PCH,
// so this engine translation unit carries its own copy. The offline self-test (which IS standalone and CAN
// include the test headers) cross-checks that both stay byte-identical. A tiny local `bvec3` is used for the
// math so the file compiles in BOTH the engine build (idlib present) and the standalone test build (idlib
// shimmed by the header) without depending on idVec3 or hlsl_compat's float3.

#ifndef ID_UNIT_TEST_STANDALONE
#include "precompiled.h"
#pragma hdrstop
#endif

#include "SoftShadowCoverageBench_scene.h"

#include <cstdint>
#include <cmath>
#include <vector>
#include <array>
#include <map>
#include <algorithm>

static_assert( sizeof( BenchCB ) == 48, "BenchCB must be 48 bytes to match the HLSL cbuffer" );

namespace
{
// minimal self-contained vec3 (avoids idVec3 in the standalone build and float3/hlsl_compat in the engine).
struct bvec3
{
	float x, y, z;
};
inline bvec3  operator+( bvec3 a, bvec3 b ) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline bvec3  operator-( bvec3 a, bvec3 b ) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline bvec3  operator*( bvec3 a, float s ) { return { a.x * s, a.y * s, a.z * s }; }
inline float  bdot( bvec3 a, bvec3 b )      { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline bvec3  bcross( bvec3 a, bvec3 b )    { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
inline float  blen( bvec3 a )               { return std::sqrt( bdot( a, a ) ); }

struct BBox
{
	bvec3 c[8];
	std::array<std::array<int, 4>, 6> f;		// 6 faces, each 4 corner indices, oriented outward
};

// axis-aligned box, then rotated by (yaw about +z, pitch about +x); faces auto-oriented outward. Corner bit
// convention (bit0=x, bit1=y, bit2=z; 0=min) is identical to SoftScan_FillBox's corner[] order.
BBox MakeBox( bvec3 C, bvec3 h, float yaw, float pitch )
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
	BBox b;
	for( int i = 0; i < 8; i++ )
	{
		bvec3 s = { ( i & 1 ) ? h.x : -h.x, ( i & 2 ) ? h.y : -h.y, ( i & 4 ) ? h.z : -h.z };
		bvec3 r = { M[0] * s.x + M[1] * s.y + M[2] * s.z, M[3] * s.x + M[4] * s.y + M[5] * s.z, M[6] * s.x + M[7] * s.y + M[8] * s.z };
		b.c[i] = C + r;
	}
	std::array<std::array<int, 4>, 6> f = {{ {{0, 2, 6, 4}}, {{1, 3, 7, 5}}, {{0, 4, 5, 1}}, {{2, 3, 7, 6}}, {{0, 1, 3, 2}}, {{4, 5, 7, 6}} }};
	for( int i = 0; i < 6; i++ )
	{
		bvec3 v0 = b.c[f[i][0]], v1 = b.c[f[i][1]], v2 = b.c[f[i][2]];
		bvec3 n = bcross( v1 - v0, v2 - v0 );
		bvec3 fc = ( b.c[f[i][0]] + b.c[f[i][1]] + b.c[f[i][2]] + b.c[f[i][3]] ) * 0.25f;
		if( bdot( n, fc - C ) < 0 ) { std::swap( f[i][1], f[i][3] ); }
		b.f[i] = f[i];
	}
	return b;
}

// silhouette loop of the box as seen from viewpoint W (the LIGHT, matching R_CollectPenumbraEdges), a single
// walk-ordered closed loop of world vertices oriented by the front-face boundary.
std::vector<bvec3> Silhouette( const BBox& b, bvec3 W )
{
	bool front[6];
	for( int i = 0; i < 6; i++ )
	{
		bvec3 v0 = b.c[b.f[i][0]], v1 = b.c[b.f[i][1]], v2 = b.c[b.f[i][2]];
		bvec3 n = bcross( v1 - v0, v2 - v0 );
		bvec3 fc = ( v0 + v1 + v2 + b.c[b.f[i][3]] ) * 0.25f;
		front[i] = bdot( n, W - fc ) > 0;
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
			if( !front[o] ) { dir.push_back( { a, c } ); }
		}
	}
	std::vector<bvec3> loop;
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

// flatten ONE closed world loop into the t_SoftEdges record layout (header pair + one edge pair per vertex).
// Header sphere = tight half-diagonal AABB bound (e0.w = -1 tag; e1.x = radius, e1.y = vertex/edge count).
void BuildCaster( const std::vector<bvec3>& loop, idList<idVec4>& rec )
{
	int m = ( int )loop.size();
	if( m == 0 ) { return; }
	bvec3 lo = { 1e30f, 1e30f, 1e30f }, hi = { -1e30f, -1e30f, -1e30f };
	for( const bvec3& p : loop )
	{
		lo = { std::fmin( lo.x, p.x ), std::fmin( lo.y, p.y ), std::fmin( lo.z, p.z ) };
		hi = { std::fmax( hi.x, p.x ), std::fmax( hi.y, p.y ), std::fmax( hi.z, p.z ) };
	}
	bvec3 c = ( lo + hi ) * 0.5f;
	float rad = 0.5f * blen( hi - lo );
	rec.Append( idVec4( c.x, c.y, c.z, -1.0f ) );
	rec.Append( idVec4( rad, ( float )m, 0.0f, 0.0f ) );	// e1.y = edge count (closed loop -> one edge per vertex)
	for( int i = 0; i < m; i++ )
	{
		bvec3 A = loop[i], B = loop[( i + 1 ) % m];
		rec.Append( idVec4( A.x, A.y, A.z, 0.0f ) );
		rec.Append( idVec4( B.x, B.y, B.z, 0.0f ) );
	}
}

// deterministic LCG (== swtest::Rng) so the axis-varied box jitter is reproducible run to run.
struct Rng
{
	uint32_t s;
	explicit Rng( uint32_t seed ) : s( seed ) {}
	float f( float lo, float hi ) { s = s * 1664525u + 1013904223u; return lo + ( hi - lo ) * ( ( s >> 8 ) & 0xFFFFFF ) / float( 0x1000000 ); }
};

// The scene: M axis-varied overlapping boxes at z~+20 between a receiver plane at z=-30 and an area light at
// z=+60. Box 0 is the FAR-REACH box (far +x, long x-halfwidth) so a +x band is covered by it ALONE
// (penumbra); every box straddles the central axis, so the receiver directly under the centre has its
// light-centre ray blocked by ALL boxes (deep umbra); receivers swept far off-axis miss every box (lit).
struct BoxSpec { bvec3 c, h; float yaw, pitch; };

const bvec3 kLightOrigin = { 0.0f, 0.0f, 60.0f };
const float kLightRadius = 8.0f;
const float kRecvZ       = -30.0f;
const float kBoxZ        = 20.0f;
const float kSweep       = 48.0f;
const int   kGrid        = 256;

void BenchBoxSpecs( int M, std::vector<BoxSpec>& specs )
{
	specs.clear();
	specs.push_back( { { 10.0f, 0.0f, kBoxZ }, { 13.0f, 6.0f, 4.0f }, 0.0f, 0.0f } );
	Rng rng( 0x50F7B0Cu );
	for( int i = 1; i < M; i++ )
	{
		double a = 6.283185307179586 * double( i - 1 ) / double( M - 1 );
		float cx = 4.0f * ( float )std::cos( a );
		float cy = 4.0f * ( float )std::sin( a );
		bvec3 h = { 7.0f + rng.f( -1.0f, 1.0f ), 7.0f + rng.f( -1.0f, 1.0f ), 4.0f };
		float yaw = rng.f( -0.25f, 0.25f );
		specs.push_back( { { cx, cy, kBoxZ }, h, yaw, 0.0f } );
	}
}
} // namespace

void BuildBenchScene( int M, BenchScene& out )
{
	out.edges.Clear();
	out.boxes.Clear();
	out.frags.Clear();

	std::vector<BoxSpec> specs;
	BenchBoxSpecs( M, specs );

	int edgePairs = 0;
	for( int b = 0; b < M; b++ )
	{
		const BoxSpec& s = specs[b];
		BBox box = MakeBox( s.c, s.h, s.yaw, s.pitch );

		// box-corner stream: 8 corners in MakeBox's bit convention (== SoftScan_FillBox's corner[] order).
		for( int k = 0; k < 8; k++ )
		{
			out.boxes.Append( idVec4( box.c[k].x, box.c[k].y, box.c[k].z, 0.0f ) );
		}

		// wedge edge stream: the LIGHT-relative silhouette in the shipped header+edge record layout.
		std::vector<bvec3> sil = Silhouette( box, kLightOrigin );
		int before = out.edges.Num();
		BuildCaster( sil, out.edges );
		edgePairs += ( out.edges.Num() - before ) / 2;			// header pair + edge pairs, all counted as PAIRS
	}

	// fragment grid: receiver plane sweep, umbra (centre) -> penumbra (mid) -> lit (outside).
	for( int y = 0; y < kGrid; y++ )
	{
		for( int x = 0; x < kGrid; x++ )
		{
			float fx = -kSweep + ( 2.0f * kSweep ) * float( x ) / float( kGrid - 1 );
			float fy = -kSweep + ( 2.0f * kSweep ) * float( y ) / float( kGrid - 1 );
			out.frags.Append( idVec4( fx, fy, kRecvZ, 0.0f ) );
		}
	}

	out.cb.swL[0] = kLightOrigin.x;
	out.cb.swL[1] = kLightOrigin.y;
	out.cb.swL[2] = kLightOrigin.z;
	out.cb.swR = kLightRadius;
	out.cb.method = 0;
	out.cb.loopN = 1;
	out.cb.casterCount = M;
	out.cb.fragCount = kGrid * kGrid;
	out.cb.edgesFirstElem = 0;
	out.cb.edgesN = edgePairs;
	out.cb.boxesBase = 0;
	out.cb.boxesCount = M;
}
