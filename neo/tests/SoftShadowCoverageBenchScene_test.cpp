/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 2026 Aleksandr Petrosyan

This file is part of the Doom 3 BFG Edition GPL Source Code.

===========================================================================
*/

// Self-test for the soft-shadow coverage microbench scene generator (BuildBenchScene). Proves the two
// representations describe the SAME geometry, that the wedge edge stream is a closed silhouette per caster,
// and that the fragment grid spans umbra -> penumbra -> lit. An INDEPENDENT point-in-polygon check on the
// projected silhouettes (borrowed from SoftShadowRadial_test) classifies each fragment, and a ray-cast over
// the box-corner stream cross-checks the same three regimes - so BOTH streams are exercised.

#include "hlsl_compat.h"
#include "softwedge_coverage.inc.hlsl"		// SoftShadow_Frame / SoftShadow_ProjectVert (projection math)
#include "SoftShadowBox.h"					// swtest::Box / MakeBox face table / RayHitsBox / v3
#include "idUnitTest.h"

// pull in the generator directly: the .cpp is NOT in the tests glob, so #including it compiles BuildBenchScene
// into this TU (relative include paths inside it resolve from renderer/Passes/).
#include "../renderer/Passes/SoftShadowCoverageBench_scene.cpp"

#include <vector>
#include <cstdio>
#include <cmath>

using namespace swtest;

namespace
{
inline float3 F3( const idVec4& v )
{
	return v3( v.x, v.y, v.z );
}

// 2D crossing-number point-in-polygon (borrowed from SoftShadowRadial_test).
bool PtInPoly( const std::vector<float2>& p, float x, float y )
{
	bool in = false;
	int n = ( int )p.size();
	for( int a = 0, bpt = n - 1; a < n; bpt = a++ )
	{
		if( ( ( p[a].y > y ) != ( p[bpt].y > y ) ) &&
				( x < ( p[bpt].x - p[a].x ) * ( y - p[a].y ) / ( p[bpt].y - p[a].y ) + p[a].x ) )
		{
			in = !in;
		}
	}
	return in;
}

// One caster's decoded edge record: the header sphere + the silhouette as (A,B) world pairs.
struct DecodedCaster
{
	float3 hc;					// header sphere centre
	float  hr;					// header sphere radius
	std::vector<std::pair<float3, float3>> pairs;	// consecutive silhouette edges
};

// Walk the wedge edge stream into per-caster records (header pair + edgeCount edge pairs, per BuildCaster).
std::vector<DecodedCaster> DecodeEdges( const BenchScene& sc )
{
	std::vector<DecodedCaster> out;
	int i = 0;								// PAIR index
	while( i < sc.cb.edgesN )
	{
		const idVec4& e0 = sc.edges[i * 2 + 0];
		const idVec4& e1 = sc.edges[i * 2 + 1];
		if( !( e0.w < 0.0f ) ) { break; }	// not a header: malformed
		DecodedCaster c;
		c.hc = F3( e0 );
		c.hr = e1.x;
		int nEdges = ( int )( e1.y + 0.5f );
		for( int j = 0; j < nEdges; j++ )
		{
			const idVec4& a0 = sc.edges[( i + 1 + j ) * 2 + 0];
			const idVec4& a1 = sc.edges[( i + 1 + j ) * 2 + 1];
			c.pairs.push_back( { F3( a0 ), F3( a1 ) } );
		}
		out.push_back( c );
		i += 1 + nEdges;
	}
	return out;
}

// reconstruct a swtest::Box from its 8 corners (bit convention == MakeBox), re-orienting faces outward
// exactly as MakeBox does, so RayHitsBox works on the box-corner stream.
Box BoxFromCorners( const float3 c[8] )
{
	Box b;
	float3 C( 0, 0, 0 );
	for( int i = 0; i < 8; i++ ) { b.c[i] = c[i]; C = C + c[i]; }
	C = C * ( 1.0f / 8.0f );
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

// closest of the 8 corners to V, and its distance (for the subset-membership check).
float NearestCornerDist( const float3 corners[8], float3 V )
{
	float best = 1e30f;
	for( int k = 0; k < 8; k++ ) { best = std::fmin( best, len3( corners[k] - V ) ); }
	return best;
}

// project a world silhouette loop to the light-disk frame of receiver P (no clip; casters are fully in
// front of the receiver here), exactly as SoftShadowRadial_test's ProjectLoop.
std::vector<float2> ProjectLoop( const std::vector<float3>& loop, float3 P, softFrame_t F )
{
	std::vector<float2> q;
	for( float3 V : loop ) { q.push_back( SoftShadow_ProjectVert( V - P, dot( V - P, F.nrm ), F ) ); }
	return q;
}

// ----------------------------------------------------------------------------- the shared scene checks
void CheckScene( int M, idTestResult& _tr )
{
	BenchScene sc;
	BuildBenchScene( M, sc );

	// cb consistency
	CHECK( sc.cb.casterCount == M );
	CHECK( sc.cb.boxesCount == M );
	CHECK( sc.cb.boxesBase == 0 );
	CHECK( sc.cb.edgesFirstElem == 0 );
	CHECK( sc.cb.fragCount == 65536 );
	CHECK( sc.frags.Num() == 65536 );
	CHECK( sc.boxes.Num() == M * 8 );
	CHECK( sc.edges.Num() == sc.cb.edgesN * 2 );

	const float3 L = v3( sc.cb.swL[0], sc.cb.swL[1], sc.cb.swL[2] );

	std::vector<DecodedCaster> casters = DecodeEdges( sc );
	CHECK( ( int )casters.size() == M );	// exactly M headers walked from the stream

	// ---- (1) SAME GEOMETRY + (2) CLOSED LOOPS, per box ----
	std::vector<Box> boxes;
	for( int b = 0; b < M && b < ( int )casters.size(); b++ )
	{
		float3 corners[8];
		for( int k = 0; k < 8; k++ ) { corners[k] = F3( sc.boxes[b * 8 + k] ); }
		boxes.push_back( BoxFromCorners( corners ) );

		// box actual bounding sphere (centre = corner centroid, radius = max corner distance)
		float3 bc( 0, 0, 0 );
		for( int k = 0; k < 8; k++ ) { bc = bc + corners[k]; }
		bc = bc * ( 1.0f / 8.0f );
		float brad = 0;
		for( int k = 0; k < 8; k++ ) { brad = std::fmax( brad, len3( corners[k] - bc ) ); }

		const DecodedCaster& c = casters[b];
		CHECK( ( int )c.pairs.size() >= 3 );	// a real silhouette loop

		// (2) closed chain: B_k == A_{k+1}, last B == first A
		int m = ( int )c.pairs.size();
		float3 lo( 1e30f, 1e30f, 1e30f ), hi( -1e30f, -1e30f, -1e30f );
		for( int k = 0; k < m; k++ )
		{
			float3 A = c.pairs[k].first, B = c.pairs[k].second;
			float3 An = c.pairs[( k + 1 ) % m].first;
			CHECK( len3( B - An ) < 1e-3f );			// B_k joins A_{k+1} (wraps closed)
			// (1) every edge vertex is one of the 8 box corners (silhouette is a subset of the corners)
			CHECK( NearestCornerDist( corners, A ) < 1e-3f );
			lo = v3( std::fmin( lo.x, A.x ), std::fmin( lo.y, A.y ), std::fmin( lo.z, A.z ) );
			hi = v3( std::fmax( hi.x, A.x ), std::fmax( hi.y, A.y ), std::fmax( hi.z, A.z ) );
		}

		// (1) header sphere honestly bounds THIS caster's silhouette: it equals the silhouette's AABB-diagonal
		// sphere (BuildCaster's construction), and its centre sits within the box's actual bounding sphere.
		// (The AABB-diagonal radius is a LOOSE bound, so it can exceed the tight corner-sphere radius - the
		// meaningful "same box" tie is the exact silhouette-sphere match + the subset-membership check above.)
		float3 sc_c = ( lo + hi ) * 0.5f;
		float  sc_r = 0.5f * len3( hi - lo );
		CHECK_NEAR( c.hc.x, sc_c.x, 1e-3 );
		CHECK_NEAR( c.hc.y, sc_c.y, 1e-3 );
		CHECK_NEAR( c.hc.z, sc_c.z, 1e-3 );
		CHECK_NEAR( c.hr, sc_r, 1e-3 );
		CHECK( len3( c.hc - bc ) <= brad + 1e-2f );		// header centre inside the box's bounding sphere
	}

	// ---- (3) FRAGMENT SPAN: umbra (O inside ALL M) / penumbra (exactly 1) / lit (0) ----
	// silhouette loops (A verts, light-relative) per box for the projected point-in-poly test.
	std::vector<std::vector<float3>> loops;
	for( const DecodedCaster& c : casters )
	{
		std::vector<float3> lp;
		for( auto& pr : c.pairs ) { lp.push_back( pr.first ); }
		loops.push_back( lp );
	}

	// sample the grid on a coarse lattice (step 8): the three regions are all wide, so a 32x32 probe finds a
	// representative of each without the 65536-fragment full scan (keeps this a fast unit test).
	int umbraIdx = -1, penumbraIdx = -1, litIdx = -1;
	const int G = 256, S = 16;
	for( int gy = 0; gy < G && !( umbraIdx >= 0 && penumbraIdx >= 0 && litIdx >= 0 ); gy += S )
	{
		for( int gx = 0; gx < G; gx += S )
		{
			int fi = gy * G + gx;
			float3 P = F3( sc.frags[fi] );
			softFrame_t F = SoftShadow_Frame( P, L );
			int cnt = 0;
			for( const auto& lp : loops )
			{
				std::vector<float2> q = ProjectLoop( lp, P, F );
				if( PtInPoly( q, 0.0f, 0.0f ) ) { cnt++; }
			}
			if( cnt == M && umbraIdx < 0 ) { umbraIdx = fi; }
			if( cnt == 1 && penumbraIdx < 0 ) { penumbraIdx = fi; }
			if( cnt == 0 && litIdx < 0 ) { litIdx = fi; }
		}
	}
	CHECK( umbraIdx >= 0 );		// a deep-umbra fragment (light centre blocked by every box)
	CHECK( penumbraIdx >= 0 );	// a penumbra fragment (exactly one box)
	CHECK( litIdx >= 0 );		// a fully-lit fragment (no box)

	// cross-check the SAME three regimes through the box-corner stream (independent ray cast), tying the two
	// representations together: the count via projected silhouettes must match the count via the box solids.
	auto RayCount = [&]( int fi ) -> int
	{
		if( fi < 0 ) { return -1; }
		float3 P = F3( sc.frags[fi] );
		int n = 0;
		for( const Box& bx : boxes ) { if( RayHitsBox( P, L - P, bx ) ) { n++; } }
		return n;
	};
	CHECK( RayCount( umbraIdx ) == M );
	CHECK( RayCount( penumbraIdx ) == 1 );
	CHECK( RayCount( litIdx ) == 0 );

	std::printf( "  [M=%2d] edges=%d pairs (%d headers)  boxes=%d corners  umbra@%d penumbra@%d lit@%d\n",
			M, sc.cb.edgesN, ( int )casters.size(), sc.boxes.Num(), umbraIdx, penumbraIdx, litIdx );
}
} // namespace

TEST( SoftShadowCoverageBenchScene, twoRepsSameGeometryAndSpanRegimes )
{
	for( int M : { 2, 4, 8, 16 } )
	{
		CheckScene( M, _tr );
	}
}
