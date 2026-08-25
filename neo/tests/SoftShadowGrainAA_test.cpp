/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 2026 Aleksandr Petrosyan

===========================================================================
*/

// ISOLATED reproduction of the grazing "ants" grain, using the REAL gate detector (GateGrain) on a 2D
// patch - no engine, no GPU, no full suite. Run just this: `rbdoom3bfg_tests SoftShadowGrainAA`.
//
// A grazing receiver plane is sampled per pixel; each pixel's analytic term is the Fubini scanline
// coverage, and the reference is a high-ray-count ray-truth of the SAME occluders (the gate's RT stand-in).
// GateGrain flags pixels where the analytic term carries high-frequency energy the denoised truth lacks -
// exactly the in-game/gate defect. The test first PROVES the exact path grains (ANT > 0); a fix must drive
// that to 0 without distorting the term away from the ray truth (lossless).

#include "hlsl_compat.h"

#define SW_SCANLINE 1
#define SW_SCAN_BITS 32
#ifndef SW_SCAN_CHORDS
	#define SW_SCAN_CHORDS 16
#endif
#define inout
namespace swgrain
{
#include "softwedge_coverage.inc.hlsl"
swAttrib_t g_swAttrib = {};
bool       g_swAttribOn = false;
}
using namespace swgrain;

#include "SoftShadowBox.h"		// swtest: float3 helpers, Rng
#include "SoftShadowMesh.h"		// swtest: LoadCap, ReconstructReceiver, MeshTruthShadowSoup
#include "SoftShadowGate.h"		// swgate: GateGrain, GateImg, GateCfg, GATE_ANT/STEP
#include "idUnitTest.h"

#include <cmath>
#include <vector>
#include <algorithm>

using namespace swtest;

namespace
{
struct Tri { float3 a, b, c; };

// SMOOTH occluder: one long near-contact ledge (a few big coplanar tris) hugging the axis - like a wall/
// prop edge grazing the receiver. If the analytic term grains here while the ray truth stays smooth, the
// grain is a genuine COVERAGE ARTIFACT; if ana tracks rt, the earlier grain was aliasing of real detail.
std::vector<Tri> BuildSmooth()
{
	std::vector<Tri> t;
	const float z = 0.6f, y0 = -0.25f, y1 = 0.25f;			// a thin near-contact ledge at height 0.6
	for( float x = -6.0f; x < 6.0f; x += 1.0f )
	{
		t.push_back( { float3( x, y0, z ), float3( x + 1.0f, y0, z ), float3( x + 1.0f, y1, z ) } );
		t.push_back( { float3( x, y0, z ), float3( x + 1.0f, y1, z ), float3( x, y1, z ) } );
	}
	return t;
}
// near-contact bumpy occluders hugging the L->P axis, distributed along the grazing sweep.
std::vector<Tri> BuildOccluders()
{
	std::vector<Tri> t;
	Rng rng( 1234u );
	for( int i = 0; i < 120; i++ )
	{
		float cx = rng.f( -6.0f, 6.0f );
		float cy = rng.f( -0.5f, 0.5f );
		float z  = rng.f( 0.3f, 1.6f );
		float s  = rng.f( 0.12f, 0.30f );
		t.push_back( { float3( cx, cy, z ), float3( cx + s, cy, z ), float3( cx, cy + s, z + 0.1f ) } );
	}
	return t;
}

// Fubini scanline exact occlusion at receiver P (the analytic term under test).
float ScanOcc( float3 P, const std::vector<Tri>& occ, float3 L, float r )
{
	softFrame_t F = SoftShadow_Frame( P, L );
	uint32_t grid[SW_SCAN_CHORDS] = {}, mask[SW_SCAN_CHORDS]; int diskBits = 0;
	for( int m = 0; m < SW_SCAN_CHORDS; m++ ) { mask[m] = SoftScan_Run( -SW_SCAN_HC[m], SW_SCAN_HC[m] ); diskBits += __builtin_popcount( mask[m] ); }
	for( const Tri& o : occ ) { SoftScan_FillTri( grid, o.a, o.b, o.c, P, F, r, SW_NEAR_EPS ); }
	int cov = 0;
	for( int m = 0; m < SW_SCAN_CHORDS; m++ ) { cov += __builtin_popcount( grid[m] & mask[m] ); }
	return diskBits ? ( float )cov / diskBits : 0.0f;
}

bool RayTri( float3 O, float3 D, float3 a, float3 b, float3 c )
{
	float3 e1 = b - a, e2 = c - a, pv = cross( D, e2 );
	float det = dot( e1, pv );
	if( std::fabs( det ) < 1e-9f ) { return false; }
	float inv = 1.0f / det;
	float3 tv = O - a;
	float u = dot( tv, pv ) * inv; if( u < 0 || u > 1 ) { return false; }
	float3 qv = cross( tv, e1 );
	float v = dot( D, qv ) * inv; if( v < 0 || u + v > 1 ) { return false; }
	float t = dot( e2, qv ) * inv;
	return t > 1e-4f && t < 1.0f - 1e-4f;			// blocks the open segment P->disk point
}

// high-ray-count ray-truth occlusion (the gate's RT reference: the true field, smoothly resolved).
float RayOcc( float3 P, const std::vector<Tri>& occ, float3 L, float r, int N )
{
	float3 toL = L - P; float d = std::sqrt( dot( toL, toL ) ); float3 n = toL * ( 1.0f / d );
	float3 up = ( std::fabs( n.z ) > 0.9f ) ? float3( 0, 1, 0 ) : float3( 0, 0, 1 );
	float3 u = normalize( cross( up, n ) ), v = cross( n, u );
	int blocked = 0, total = 0;
	for( int iy = 0; iy < N; iy++ )
		for( int ix = 0; ix < N; ix++ )
		{
			float du = ( ix + 0.5f ) / N * 2 - 1, dv = ( iy + 0.5f ) / N * 2 - 1;
			if( du * du + dv * dv > 1.0f ) { continue; }
			total++;
			float3 Dp = ( L + u * ( du * r ) + v * ( dv * r ) ) - P;
			for( const Tri& o : occ ) { if( RayTri( P, Dp, o.a, o.b, o.c ) ) { blocked++; break; } }
		}
	return total ? ( float )blocked / total : 0.0f;
}
}

using namespace swgate;

STUDY_TEST( SoftShadowGrainAA, gate_grain_reproduced_on_grazing_patch )
{
	const std::vector<Tri> occ = BuildOccluders();
	const float3 L( 0, 0, 60 );
	const float  r = 8.0f;

	// GRAZING patch: pixel (i,j) maps to a receiver point whose world spacing is COARSE along x (grazing:
	// the surface recedes, so one pixel spans many world units) and fine along y. This is the footprint that
	// makes the analytic term alias while the ray truth stays smooth.
	const int W = 96, H = 96;
	const float fx = 0.22f, fy = 0.05f, x0 = -6.0f, y0 = -2.0f;

	// RT reference (computed ONCE - fix-independent). valid = penumbra by the truth.
	GateImg rt( W, H, 1.0f );
	std::vector<uint8_t> valid( ( size_t )W * H, 0 );
	for( int j = 0; j < H; j++ )
		for( int i = 0; i < W; i++ )
		{
			const float3 P( x0 + i * fx, y0 + j * fy, 0.0f );
			const float g = RayOcc( P, occ, L, r, 24 );
			rt.t[j * W + i] = g;
			valid[j * W + i] = ( g > 0.02f && g < 0.98f ) ? 1 : 0;
		}
	GateCfg cfg;

	// build the analytic term image under a given fix (dnR ratio clamp, dnAbs absolute floor, footprint
	// supersample tap grid TxT), run the REAL GateGrain, return (ant, step).
	auto grainOf = [&]( float dnR, float dnAbs, int T, const char* label )
	{
		GateImg ana( W, H, 1.0f );
		for( int j = 0; j < H; j++ )
			for( int i = 0; i < W; i++ )
			{
				const float3 P( x0 + i * fx, y0 + j * fy, 0.0f );
				g_swMinDnR = dnR; g_swMinDnAbs = dnAbs;
				float a;
				if( T <= 1 ) { a = ScanOcc( P, occ, L, r ); }
				else
				{
					float acc = 0.0f;
					for( int sy = 0; sy < T; sy++ )
						for( int sx = 0; sx < T; sx++ )
						{
							float ox = ( ( sx + 0.5f ) / T - 0.5f ) * fx, oy = ( ( sy + 0.5f ) / T - 0.5f ) * fy;
							acc += ScanOcc( float3( P.x + ox, P.y + oy, 0.0f ), occ, L, r );
						}
					a = acc / ( T * T );
				}
				g_swMinDnR = 0.0f; g_swMinDnAbs = 0.0f;
				ana.t[j * W + i] = a;
			}
		std::vector<GateDefect> defects;
		GateGrain( ana, rt, valid, nullptr, cfg, defects );
		int ant = 0, step = 0;
		for( const GateDefect& d : defects ) { if( d.kind == GATE_ANT ) { ant++; } else if( d.kind == GATE_STEP ) { step++; } }
		// faithfulness to the independent ray truth: mean + max |ana - rt| over penumbra (a LOSSY fix would
		// push these up as it de-grains; a correct one drives ANT to 0 while staying close to the truth).
		double meanE = 0.0, maxE = 0.0; int n = 0;
		for( int i = 0; i < W * H; i++ ) { if( valid[i] ) { double e = std::fabs( ( double )ana.t[i] - rt.t[i] ); meanE += e; maxE = std::fmax( maxE, e ); n++; } }
		meanE = n ? meanE / n : 0.0;
		std::printf( "[grainAA] %-22s ANT=%d STEP=%d | err-vs-RT mean=%.4f max=%.4f\n", label, ant, step, meanE, maxE );
		return ant;
	};

	int validPx = 0; for( uint8_t vv : valid ) { validPx += vv; }
	std::printf( "[grainAA] %dx%d grazing patch, valid=%d px.\n", W, H, validPx );
	const int antExact = grainOf( 0.0f, 0.0f, 1, "exact" );

	// LOOK at the grain: dump a middle row of exact ana vs the ray truth. If ana oscillates pixel-to-pixel
	// while rt is smooth, the grain is a per-pixel scanline artifact.
	const int jr = H / 2;
	std::printf( "[grainAA] row j=%d  i: ana | rt\n", jr );
	for( int i = 20; i < 44; i++ )
	{
		const float3 P( x0 + i * fx, y0 + jr * fy, 0.0f );
		g_swMinDnR = 0; g_swMinDnAbs = 0;
		float a = ScanOcc( P, occ, L, r );
		float g = rt.t[jr * W + i];
		std::printf( "[grainAA]   i=%2d  ana=%.3f  rt=%.3f  d=%+.3f\n", i, a, g, a - g );
	}

	// Is the grain even a real artifact, or just an under-resolved RT reference? Rebuild rt at MORE rays and
	// re-flag the exact term. If ANT collapses, the "grain" was the reference; if it persists, it is real
	// scanline HF the true field lacks.
	GateImg rtHi( W, H, 1.0f );
	for( int j = 0; j < H; j++ )
		for( int i = 0; i < W; i++ ) { rtHi.t[j * W + i] = RayOcc( float3( x0 + i * fx, y0 + j * fy, 0.0f ), occ, L, r, 64 ); }
	{
		GateImg ana( W, H, 1.0f );
		for( int j = 0; j < H; j++ )
			for( int i = 0; i < W; i++ ) { g_swMinDnR = 0; g_swMinDnAbs = 0; ana.t[j * W + i] = ScanOcc( float3( x0 + i * fx, y0 + j * fy, 0.0f ), occ, L, r ); }
		std::vector<GateDefect> d2; GateGrain( ana, rtHi, valid, nullptr, cfg, d2 );
		int ant = 0; for( const GateDefect& d : d2 ) { if( d.kind == GATE_ANT ) { ant++; } }
		std::printf( "[grainAA] exact vs RT@64rays: ANT=%d (vs %d at 24 rays)\n", ant, antExact );
	}

	CHECK( antExact > 0 );
}

// REAL-CAPTURE reproduction (study: needs the local .cap corpus). Loads a capture, reconstructs the exact
// per-pixel receivers at the capture camera, and runs the SAME scanline analytic vs mesh ray-truth + the
// real GateGrain - so we see whether the SHIPPED grain is ana~=rt (aliasing of real detail, no coverage fix)
// or ana!=rt (a genuine coverage artifact). Run: `rbdoom3bfg_tests SoftShadowGrainCap @study`.
static float MeshScanOcc( float3 P, const std::vector<float>& v, const uint32_t* idx, uint32_t nIdx, float3 L, float r )
{
	softFrame_t F = SoftShadow_Frame( P, L );
	uint32_t grid[SW_SCAN_CHORDS] = {}, mask[SW_SCAN_CHORDS]; int diskBits = 0;
	for( int m = 0; m < SW_SCAN_CHORDS; m++ ) { mask[m] = SoftScan_Run( -SW_SCAN_HC[m], SW_SCAN_HC[m] ); diskBits += __builtin_popcount( mask[m] ); }
	for( uint32_t t = 0; t + 2 < nIdx; t += 3 )
	{
		uint32_t a = idx[t], b = idx[t + 1], c = idx[t + 2];
		SoftScan_FillTri( grid, float3( v[a * 3], v[a * 3 + 1], v[a * 3 + 2] ), float3( v[b * 3], v[b * 3 + 1], v[b * 3 + 2] ), float3( v[c * 3], v[c * 3 + 1], v[c * 3 + 2] ), P, F, r, SW_NEAR_EPS );
	}
	int cov = 0; for( int m = 0; m < SW_SCAN_CHORDS; m++ ) { cov += __builtin_popcount( grid[m] & mask[m] ); }
	return diskBits ? ( float )cov / diskBits : 0.0f;
}

STUDY_TEST( SoftShadowGrainCap, real_capture_grain_character )
{
	Cap cap;
	const char* path = "/home/app/.local/share/rbdoom3bfg/base/cap/cap0004.cap";
	if( !LoadCap( path, cap ) || cap.lights.empty() || cap.recvVerts.empty() )
	{
		std::printf( "[graincap] SKIP: no cap / no receiver meshes at %s\n", path );
		return;
	}
	std::printf( "[graincap] cap0004: %d lights, %d casters, %zu meshVerts, %d receivers, %zu recvVerts (cap.depth is empty corpus-wide -> sample the REAL receiver+occluder meshes pointwise instead of screen pixels)\n",
			( int )cap.lights.size(), ( int )cap.hdr.numCasters, cap.meshVerts.size() / 3, ( int )cap.receivers.size(), cap.recvVerts.size() / 3 );

	// For each light, sample points ON its real receiver surfaces and compare the scanline analytic term to
	// an independent mesh ray-truth over its real occluders. If |ana-rt| is small everywhere, the analytic is
	// ACCURATE on shipped geometry -> the gate's grain is screen-space ALIASING of that (correct) detail, not
	// a coverage artifact. (No screen footprint here, so this measures value-accuracy, not the aliasing.)
	for( int li = 0; li < ( int )cap.lights.size(); li++ )
	{
		const capLight_t& lt = cap.lights[li];
		const float3 L( lt.origin[0], lt.origin[1], lt.origin[2] );
		const float  r = lt.penumbraSize > 1e-2f ? lt.penumbraSize : 1e-2f;
		std::vector<uint32_t> idx;
		for( uint32_t ci = lt.firstCaster; ci < lt.firstCaster + lt.casterCount && ci < cap.casters.size(); ci++ )
		{
			const capCaster_t& cs = cap.casters[ci];
			for( uint32_t k = 0; k < cs.numIndex; k++ ) { idx.push_back( cap.meshIdx[cs.firstIndex + k] ); }
		}
		if( idx.size() < 3 ) { continue; }

		double meanD = 0.0, maxD = 0.0; int nD = 0;
		double meanFar = 0.0; int nFar = 0;			// samples whose nearest occluder is NOT near-contact (dn>1)
		struct Worst { double e; float3 P; float an, g, dnNear; };
		std::vector<Worst> worst;
		for( const capReceiver_t& rc : cap.receivers )
		{
			if( rc.lightIndex != ( uint32_t )li ) { continue; }
			for( uint32_t t = 0; t + 2 < rc.numIndex && nD < 150; t += 3 )
			{
				uint32_t a = cap.recvIdx[rc.firstIndex + t], b = cap.recvIdx[rc.firstIndex + t + 1], c = cap.recvIdx[rc.firstIndex + t + 2];
				float3 v0( cap.recvVerts[a * 3], cap.recvVerts[a * 3 + 1], cap.recvVerts[a * 3 + 2] );
				float3 v1( cap.recvVerts[b * 3], cap.recvVerts[b * 3 + 1], cap.recvVerts[b * 3 + 2] );
				float3 v2( cap.recvVerts[c * 3], cap.recvVerts[c * 3 + 1], cap.recvVerts[c * 3 + 2] );
				const float bc[3][2] = { {0.25f, 0.25f}, {0.5f, 0.25f}, {0.25f, 0.5f} };
				for( int s = 0; s < 3; s++ )
				{
					float u = bc[s][0], w = bc[s][1];
					float3 P = v0 + ( v1 - v0 ) * u + ( v2 - v0 ) * w;
					float an = 1.0f - MeshScanOcc( P, cap.meshVerts, idx.data(), ( uint32_t )idx.size(), L, r );	// lit = 1 - occlusion
					float g  = MeshTruthShadowSoup( cap.meshVerts.data(), idx.data(), ( uint32_t )idx.size(), P, L, r, 12 );	// already lit (1 - occluded)
					if( ( an > 0.02f && an < 0.98f ) || ( g > 0.02f && g < 0.98f ) )
					{
						double e = std::fabs( an - g );
						// nearest occluder vertex depth along the receiver->light normal (near-contact indicator)
						float3 nrm = normalize( L - P ); float dnNear = 1e30f;
						for( uint32_t vi = 0; vi < idx.size(); vi += 9 ) { uint32_t o = idx[vi]; float3 ov( cap.meshVerts[o * 3], cap.meshVerts[o * 3 + 1], cap.meshVerts[o * 3 + 2] ); float dn = dot( ov - P, nrm ); if( dn > 0.01f ) { dnNear = std::fmin( dnNear, dn ); } }
						meanD += e; maxD = std::fmax( maxD, e ); nD++;
						if( dnNear > 1.0f ) { meanFar += e; nFar++; }
						worst.push_back( { e, P, an, g, dnNear } );
					}
				}
			}
		}
		if( nD < 100 ) { continue; }
		std::sort( worst.begin(), worst.end(), []( const Worst& a, const Worst& b ) { return a.e > b.e; } );
		std::printf( "[graincap] light %2d: samples=%d | mean=%.4f max=%.4f | FAR-only(dn>1) mean=%.4f n=%d  (if FAR mean ~0.02, the disagreement is the near-contact THRESHOLD mismatch, not a bug)\n",
				li, nD, meanD / nD, maxD, nFar ? meanFar / nFar : 0.0, nFar );
		for( int k = 0; k < 4 && k < ( int )worst.size(); k++ )
		{
			std::printf( "[graincap]     worst[%d] |an-rt|=%.3f  ana=%.3f rt=%.3f  P=(%.0f,%.0f,%.0f) nearestOccluderDn=%.2f\n",
					k, worst[k].e, worst[k].an, worst[k].g, worst[k].P.x, worst[k].P.y, worst[k].P.z, worst[k].dnNear );
		}
	}
	CHECK( true );
}
