/*
===========================================================================
Doom 3 BFG Edition GPL Source Code
Copyright (C) 2026 Aleksandr Petrosyan

Soft-shadow COVERAGE MICROBENCH compute shader. Runs ONE of three coverage
methods (compile-time BENCH_METHOD) over a synthetic caster set, so a host
harness can time each method's per-fragment cost in isolation. It includes the
LIVE coverage source (softwedge_coverage.inc.hlsl) and calls the real
functions - it is not a copy.

  BENCH_METHOD 0 : wedge, MAX-combine union   (SW_RADIAL 0)
  BENCH_METHOD 1 : wedge + radial-max union   (SW_RADIAL 1)
  BENCH_METHOD 2 : Fubini scanline grid, box-only manual fill (SW_SCANLINE 1)
===========================================================================
*/

#ifndef BENCH_METHOD
	#define BENCH_METHOD 0
#endif

// --- select the coverage path BEFORE including the source (the .inc reads these) ---
#if BENCH_METHOD == 0
	#define SW_RADIAL 0
#elif BENCH_METHOD == 1
	#define SW_RADIAL 1
#elif BENCH_METHOD == 2
	#define SW_SCANLINE 1
#else
	#error "BENCH_METHOD must be 0, 1 or 2"
#endif

// --- fixed bench binding layout (shared with the host harness) ---
StructuredBuffer<float4>	t_BenchEdges	: register( t0 );	// wedge edge stream (t_SoftEdges format: float4 pairs)
StructuredBuffer<float4>	t_BenchBoxes	: register( t1 );	// box corners, 8 per box, flat
StructuredBuffer<float4>	t_BenchFrags	: register( t2 );	// receiver pos in .xyz (65536 entries)
RWStructuredBuffer<float>	u_BenchOut		: register( u0 );	// per-fragment coverage output

// The coverage include reads two globals unconditionally in HLSL (SW_EDGEBUF_PARAM /
// SW_TILEBUF_PARAM expand empty): t_SoftEdges and t_SoftTiles. Alias the edge global onto our
// bench edges; t_SoftTiles is only touched by the tile-list walkers we never dispatch (M2 fills
// boxes manually), so it is a dummy declaration purely to let those uncalled functions typecheck
// - mirrors softsurf_build.cs.hlsl's "UNUSED dummy" t_SoftTiles.
#define t_SoftEdges t_BenchEdges
StructuredBuffer<uint>		t_SoftTiles		: register( t3 );	// UNUSED dummy (never bound/accessed)

cbuffer BenchCB : register( b0 )
{
	float4	swL_R;		// xyz = light origin, w = disk radius
	int4	ctl;		// x=method(info), y=loopN, z=casterCount, w=fragCount
	int4	streams;	// x=edgesFirstElem, y=edgesN(PAIRS), z=boxesBase, w=boxesCount
};

#include "softwedge_coverage.inc.hlsl"

[numthreads( 8, 8, 1 )]
void main( uint3 dtid : SV_DispatchThreadID )
{
	uint fi = dtid.y * 256 + dtid.x;
	if( ( int )fi >= ctl.w ) { return; }

	const float3 swP  = t_BenchFrags[fi].xyz;
	const float3 swL  = swL_R.xyz;
	const float  swR  = max( swL_R.w, 1e-2f );
	const float3 axis = float3( 1.0f, 0.0f, 0.0f );		// perturbation axis (defeats loop hoisting)

	float acc = 0.0f;
	[loop]
	for( int it = 0; it < ctl.y; it++ )
	{
		// slight per-iteration perturbation so no iteration is provably identical -> no hoist, no DCE
		const float3 swPit = swP + ( ( float )it * 1e-6f ) * axis;

#if BENCH_METHOD == 2
		// -------- M2: Fubini scanline grid, box silhouettes only (manual, NOT via FaceCoverageList) --------
		// Setup mirrors SoftShadow_FaceCoverageList's SW_SCANLINE block exactly (SW_ADAPT_CHORDS off).
		softFrame_t swF = SoftShadow_Frame( swPit, swL );
		SwGridWord swGrid[SW_SCAN_CHORDS];
		float2     swEnv[SW_SCAN_CHORDS];
		[unroll]
		for( int gi = 0; gi < SW_SCAN_CHORDS; gi++ ) { swGrid[gi] = SwGridZero(); swEnv[gi] = SwEnvZero(); }
		const int swDiskBits = SW_SCAN_DISKBITS;			// compile-time denominator (SW_ADAPT_CHORDS==0)

		[loop]
		for( int b = 0; b < streams.w; b++ )
		{
			float3 corner[8];
			[unroll]
			for( int k = 0; k < 8; k++ ) { corner[k] = t_BenchBoxes[streams.z + b * 8 + k].xyz; }
			SoftScan_FillBox( swGrid, swEnv, corner, swPit, swF, swR, SW_NEAR_EPS );
		}
		const float cov = SoftScan_ReduceCov( swGrid, swEnv, SW_SCAN_MASK ) / ( float )swDiskBits;
#else
		// -------- M0 / M1: wedge coverage (SW_RADIAL selects max-combine vs radial union) --------
		const float cov = SoftShadow_WedgeOcclusion( swPit, swL, swR, streams.x, streams.y, 0.0f );
#endif
		acc += cov;
	}

	u_BenchOut[fi] = acc / ( float )max( ctl.y, 1 );
}
