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

// SHARED surface-fold CLASSIFIER - the SINGLE source of the fold decision, compiled as HLSL (the
// softsurf_build compute shader) AND as C++ (neo/tests/SoftShadowSurfFold_test.cpp via hlsl_compat.h).
// Neither copy can drift from the other: the unit test exercises the EXACT shipped classifier code,
// structurally - it is not possible to run the classifier in a test without running the shipped path.
//
// Requires softwedge_coverage.inc.hlsl to be included FIRST (softFrame_t, SoftShadow_ClipSlab /
// SoftShadow_ProjectVert / SoftDisk_CircleTriArea, PI, SW_NEAR_EPS, and the SW_FUNC macro).

#ifndef __SOFTSURF_CLASSIFY_INC__
#define __SOFTSURF_CLASSIFY_INC__

// CONTINUOUS closed-form SOLO coverage of ONE triangle seen from P: the signed area of
// ( light disk INTERSECT the triangle's central projection ) / disk area, summed around the triangle's
// 3-edge loop with the shipped circle-triangle primitive and the same near-plane slab clip the walk
// uses. Unlike the sampled mask this is CONTINUOUS in P - no 1/N coverage quantum - so its curvature
// across the texel is a QUANTIZATION-FREE linear-vs-non-linear classifier: a linearly-separable
// occluder reads a 2nd difference at float epsilon, not at 1/16, so it folds at a tight threshold
// instead of being mis-shunted to the residual pool. Used ONLY for the fold decision; the folded value
// F stays union-sampled to match the runtime walk exactly.
SW_FUNC float SurfBuild_SoloCovExact( float3 swP, float3 v0, float3 v1, float3 v2, softFrame_t f, float swR2 )
{
	float3 vs[3] = { v0, v1, v2 };
	float  area  = 0.0f;
	float2 first = float2( 0.0f, 0.0f );
	float2 prev  = float2( 0.0f, 0.0f );
	bool   valid = false;
	for( int e = 0; e < 3; e++ )
	{
		float3 a   = vs[e] - swP;
		float3 b   = vs[( e + 1 ) % 3] - swP;
		float  dnA = dot( a, f.nrm );
		float  dnB = dot( b, f.nrm );
		float  d   = dnB - dnA;
		softClip_t cl = SoftShadow_ClipSlab( dnA, dnB, SW_NEAR_EPS, f.distPL );
		if( cl.empty )
		{
			continue;					// whole edge behind receiver / beyond light: no projected vertex
		}
		float2 q0 = SoftShadow_ProjectVert( a + cl.t0 * ( b - a ), dnA + cl.t0 * d, f );
		float2 q1 = SoftShadow_ProjectVert( a + cl.t1 * ( b - a ), dnA + cl.t1 * d, f );
		if( valid )
		{
			area += SoftDisk_CircleTriArea( prev, q0, swR2 );	// connector across the clipped-off vertex
		}
		else
		{
			first = q0;
			valid = true;
		}
		area += SoftDisk_CircleTriArea( q0, q1, swR2 );
		prev  = q1;
	}
	if( valid )
	{
		area += SoftDisk_CircleTriArea( prev, first, swR2 );		// close the loop
	}
	return saturate( abs( area ) / ( PI * swR2 ) );
}

// 2nd difference of an occluder's solo coverage sampled at the 9 texel probes (corners 0-3, center 4,
// edge midpoints 5-8): the two diagonals through the center plus the four EDGES through their midpoints.
// Edge curvature is exactly what becomes a cross-texel border seam if folded, and the diagonals alone
// cannot see it. The caller folds the occluder when this is <= the fold threshold (coverage units).
SW_FUNC float SurfBuild_FoldD2( float c00, float c10, float c01, float c11, float cC,
								float mB, float mL, float mR, float mT )
{
	float d2 = max( abs( c00 + c11 - 2.0f * cC ), abs( c10 + c01 - 2.0f * cC ) );
	d2 = max( d2, max( abs( c00 + c10 - 2.0f * mB ), abs( c00 + c01 - 2.0f * mL ) ) );
	d2 = max( d2, max( abs( c10 + c11 - 2.0f * mR ), abs( c01 + c11 - 2.0f * mT ) ) );
	return d2;
}

// ANALYTIC local classifier data for ONE triangle at receiver point P: its solo coverage plus the LOCAL
// gradient and curvature of that coverage along the texel in-plane axes du,dv (unit vectors), by tight
// central differences of the continuous closed-form area (SurfBuild_SoloCovExact). The occlusion term is
// an AREA INTEGRAL, hence smooth and differentiable; its behaviour over the whole texel is fixed by these
// LOCAL derivatives at the center (Taylor), NOT by scattered point samples - so there is no sub-probe
// blind spot. eps is a small world-length step (<< texel); the frame is recomputed per sample so the
// projection's full P-dependence is captured. Register-light (a 4-field struct, no arrays).
struct surfLocal_t
{
	float c0;		// solo coverage at P
	float gu;		// d(coverage)/d(u)  along du, per unit world length
	float gv;		// d(coverage)/d(v)  along dv
	float curv;		// max( |d2/du2|, |d2/dv2| ), per unit world length^2 - a curvature bound
};
SW_FUNC surfLocal_t SurfBuild_SoloLocal( float3 swP, float3 swL, float swR2, float3 v0, float3 v1, float3 v2,
										 float3 du, float3 dv, float eps )
{
	surfLocal_t o;
	o.c0 = SurfBuild_SoloCovExact( swP, v0, v1, v2, SoftShadow_Frame( swP, swL ), swR2 );
	float3 pu = swP + du * eps, mu = swP - du * eps;
	float3 pv = swP + dv * eps, mv = swP - dv * eps;
	float cup = SurfBuild_SoloCovExact( pu, v0, v1, v2, SoftShadow_Frame( pu, swL ), swR2 );
	float cun = SurfBuild_SoloCovExact( mu, v0, v1, v2, SoftShadow_Frame( mu, swL ), swR2 );
	float cvp = SurfBuild_SoloCovExact( pv, v0, v1, v2, SoftShadow_Frame( pv, swL ), swR2 );
	float cvn = SurfBuild_SoloCovExact( mv, v0, v1, v2, SoftShadow_Frame( mv, swL ), swR2 );
	const float inv2e = 0.5f / eps;
	const float inve2 = 1.0f / ( eps * eps );
	o.gu = ( cup - cun ) * inv2e;
	o.gv = ( cvp - cvn ) * inv2e;
	o.curv = max( abs( cup - 2.0f * o.c0 + cun ), abs( cvp - 2.0f * o.c0 + cvn ) ) * inve2;
	return o;
}

// Sub-probe-safe Taylor bound on the MAX of a smooth coverage sum over a texel of half-extent h, from its
// value/gradient/curvature at the center: f(center+d) <= f0 + (|gu|+|gv|)*h + curvSum*h^2 over |d|<=h.
// (Linear term maxed at a corner in L1; quadratic bounded by the summed axis curvature.) Used two ways:
//   - UMBRA gate: pass the sum over ALL occluders; bound >= 1 => the union may reach the disk (umbra) in
//     the texel => WALK-ALWAYS. Sound because union <= sum of solo coverages, and the sum is smooth.
//   - BILERP-accuracy gate: pass the sum over FOLDED occluders; curvSum*h^2 > errTol => the folded F is
//     too curved for the 4-corner bilerp => WALK-ALWAYS. No interior probes in either case.
SW_FUNC float SurfBuild_TaylorMax( float c0Sum, float guSum, float gvSum, float curvSum, float h )
{
	return c0Sum + ( abs( guSum ) + abs( gvSum ) ) * h + curvSum * h * h;
}

#endif // __SOFTSURF_CLASSIFY_INC__
