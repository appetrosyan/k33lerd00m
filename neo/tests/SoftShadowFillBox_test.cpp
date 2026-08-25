/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 2026 Aleksandr Petrosyan

This file is part of the Doom 3 BFG Edition GPL Source Code.

===========================================================================
*/

// SoftScan_FillBox parity: the analytic box fills its receiver-silhouette into the coverage grid ONCE,
// and must produce (a) bit-identical coverage to walking the box's 12 triangles through SoftScan_FillTri
// (the shipped triangle path it replaces), and (b) the same disk shadow as an INDEPENDENT ray-cast oracle
// (TruthShadow) within the chord-quantization tolerance. (a) is the exact contract - the analytic caster
// must be a drop-in for the triangle union; (b) proves both agree with ground truth.

#include "hlsl_compat.h"

// Compile the live coverage source as C++ with the Fubini scanline enabled, isolated in a namespace so it
// does not ODR-collide with the SW_SCANLINE 0 bodies other test TUs compile. Mirror SoftShadowWasteProof.
#define SW_SCANLINE 1
#define SW_SCAN_BITS 32			// CPU emulation uses uint32 grids; pin the word (SwGridWord = uint). GPU ships 64.
#ifndef SW_SCAN_CHORDS
	#define SW_SCAN_CHORDS 16
#endif
#define inout					// HLSL inout on the grid param: C++ array params decay to pointers, so mutation matches
namespace swbox
{
#include "softwedge_coverage.inc.hlsl"
swAttrib_t g_swAttrib = {};
bool       g_swAttribOn = false;
}
using namespace swbox;			// bring the shader symbols into scope BEFORE SoftShadowBox.h parses its callers

#include "SoftShadowBox.h"
#include "idUnitTest.h"

#include <cmath>

using namespace swtest;

namespace
{
int MaskedBits( const uint32_t g[SW_SCAN_CHORDS], const uint32_t m[SW_SCAN_CHORDS] )
{
	int n = 0;
	for( int i = 0; i < SW_SCAN_CHORDS; i++ ) { n += __builtin_popcount( g[i] & m[i] ); }
	return n;
}

// 12-triangle union of a box (the path FillBox replaces), for the bit-parity reference.
void FillBoxTris( uint32_t grid[SW_SCAN_CHORDS], const Box& b, float3 P, softFrame_t F, float swR, float swEps )
{
	for( int f = 0; f < 6; f++ )
	{
		int idx[6] = { b.f[f][0], b.f[f][1], b.f[f][2], b.f[f][0], b.f[f][2], b.f[f][3] };
		for( int t = 0; t < 2; t++ )
		{
			SoftScan_FillTri( grid, b.c[idx[t * 3 + 0]], b.c[idx[t * 3 + 1]], b.c[idx[t * 3 + 2]], P, F, swR, swEps );
		}
	}
}

struct Cfg { float3 C, h; float yaw, pitch; float3 P, L; float r; };
}

TEST( SoftShadowFillBox, parity )
{
	// receiver below, light above, box between them: the box shadows P. Includes an off-centre receiver,
	// a near-plane straddle (box bottom at the receiver plane), a thin slab, and a rotated (OBB) box.
	const Cfg cfgs[] =
	{
		{ float3( 0, 0, 0 ), float3( 10, 10, 10 ), 0.0f, 0.0f, float3(  3,  2, -30 ), float3( 0, 0, 60 ), 8.0f },
		{ float3( 0, 0, 0 ), float3( 12,  8, 10 ), 0.0f, 0.0f, float3( 18, -6, -24 ), float3( 4, 3, 70 ), 12.0f },
		{ float3( 0, 0, 0 ), float3( 10, 10, 30 ), 0.0f, 0.0f, float3(  5,  5, -30 ), float3( 0, 0, 40 ), 6.0f },  // near-plane straddle (box reaches z=-30 = P plane)
		{ float3( 0, 0, 0 ), float3( 20, 20,  3 ), 0.0f, 0.0f, float3(  2, -3, -25 ), float3( 1, 1, 55 ), 10.0f }, // thin slab
		{ float3( 0, 0, 0 ), float3( 10, 14,  9 ), 0.6f, 0.3f, float3(  6,  4, -28 ), float3( 2, 1, 62 ), 9.0f },  // OBB (rotated), full cover
		{ float3( 0, 0, 0 ), float3(  9, 12, 10 ), 0.7f, 0.35f, float3( 26, 20, -26 ), float3( 0, 0, 60 ), 14.0f }, // OBB rotated, PARTIAL (off-axis grazing edge)
		{ float3( 0, 0, 0 ), float3( 11,  7, 13 ), 1.1f, 0.5f, float3( -22, 15, -24 ), float3( 3, -2, 58 ), 12.0f }, // OBB rotated, partial, other axis
	};

	for( int ci = 0; ci < ( int )( sizeof( cfgs ) / sizeof( cfgs[0] ) ); ci++ )
	{
		const Cfg& g = cfgs[ci];
		Box b = MakeBox( g.C, g.h, g.yaw, g.pitch );
		const float3 P = g.P, L = g.L;
		const float swR = g.r >= 1e-2f ? g.r : 1e-2f;
		const float swEps = SW_NEAR_EPS;
		softFrame_t F = SoftShadow_Frame( P, L );

		uint32_t diskMask[SW_SCAN_CHORDS]; int diskBits = 0;
		for( int m = 0; m < SW_SCAN_CHORDS; m++ )
		{
			diskMask[m] = SoftScan_Run( -SW_SCAN_HC[m], SW_SCAN_HC[m] );
			diskBits += __builtin_popcount( diskMask[m] );
		}

		uint32_t gBox[SW_SCAN_CHORDS] = {}, gTri[SW_SCAN_CHORDS] = {};
		SoftScan_FillBox( gBox, b.c, P, F, swR, swEps );
		FillBoxTris( gTri, b, P, F, swR, swEps );

		// (a) EXACT: analytic box coverage == 12-triangle union coverage, bit for bit under the disk mask.
		for( int m = 0; m < SW_SCAN_CHORDS; m++ )
		{
			CHECK( ( gBox[m] & diskMask[m] ) == ( gTri[m] & diskMask[m] ) );
		}

		// (b) ground truth: both must match the ray-cast disk shadow within chord quantization.
		const float shadowBox = 1.0f - ( diskBits > 0 ? ( float )MaskedBits( gBox, diskMask ) / diskBits : 0.0f );
		const float truth = TruthShadow( P, L, swR, b );
		CHECK_NEAR( shadowBox, truth, 0.06 );
	}
}
