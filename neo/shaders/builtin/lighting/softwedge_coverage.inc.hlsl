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

// Analytic soft shadows by LIGHT-DISK COVERAGE. From the receiver, the caster's silhouette loop projects
// onto the area light's disk; the occluded fraction is area( disk INTERSECT projected-silhouette ) / area.
// That is a signed sum of per-edge circle-triangle areas around the loop: it is winding-correct, so it is
// exact for NON-CONVEX casters (where an intersection-of-half-spaces / MIN combine fails), and it varies
// smoothly 0..1 across the penumbra (contact hardening is intrinsic - a near receiver sees the silhouette
// large on the disk, a far one sees it small). The umbra is where coverage saturates to 1 - never stamped.
//
// The sum is order-INDEPENDENT (verified numerically), so the engine may hand edges in any order as long
// as they are consistently wound. Each caster's edges are summed separately (grouped by id) and casters
// are unioned by max.
//
// ALL float literals are f-suffixed so the C++ unit-test build computes in FLOAT like the GPU does
// (unsuffixed literals are double in C++ and silently promote whole expressions; the tests then validate
// numerically kinder math than the shader ships).

#ifndef __SOFTWEDGE_COVERAGE_INC__
#define __SOFTWEDGE_COVERAGE_INC__

// SW_FUNC: 'inline' when compiled as C++ (several test TUs include this file; ODR), empty in HLSL.
#ifndef SW_FUNC
	#ifdef __cplusplus
		#define SW_FUNC inline
	#else
		#define SW_FUNC
	#endif
#endif

// SW_FAST_ATAN: 1 = polynomial sector angle (~0.0038 rad err, cheaper on GPU); 0 = hardware atan2 (exact).
// Default on; flip to 0 for an A/B against the transcendental.
#ifndef SW_FAST_ATAN
	#define SW_FAST_ATAN 1
#endif

// SW_JUMP_SKIP: 1 = a culled caster jumps its whole edge span via the header's edgeCount (e1.y); 0 = the
// old per-edge idle 'continue'. Bit-exact either way (cull is conservative); flip to 0 for an A/B.
#ifndef SW_JUMP_SKIP
	#define SW_JUMP_SKIP 1
#endif

// SW_FACE_PROFILE: TIMING PROBE ONLY, never ship non-zero. 1 = walk + caster culls (no triangle
// work); 2 = + per-triangle cone culls (no setup/sample tests). Renders WRONG shadows by design;
// used to decompose the integral's frame cost into walk / cull / sample masses on the bench.
#ifndef SW_FACE_PROFILE
	#define SW_FACE_PROFILE 0
#endif

// SW_FACE_LEGACY: RETIRED with stream v2 (2026-08-17). The v1 in-loop recompute paths it toggled no
// longer exist - the stream itself now carries triRad, and qq/e2qq/crack-close hoists are unconditional.
#define SW_FACE_LEGACY 0

// SW_FACE_SCALAR_LIST: 1 = route the tile-binned list walk's loads through readfirstlane'd addresses
// so ACO can emit scalar s_ loads (the addresses ARE wave-uniform: 16x16 tiles align with the 8x8
// wave tiling). MEASURED NO GAIN (2026-08-17, VRS 0 heavy trio: 45.5/42.8/53.5 vs 46.5/44.1/51.6 -
// noise): lanes loading one shared address already broadcast from L0, so the per-lane loads were
// effectively free; the walk's ~10 ms is loop/latency overhead, not VMEM bandwidth. Default 0;
// kept only as the recorded experiment - do not retry without a different theory of the walk cost.
#ifndef SW_FACE_SCALAR_LIST
	#define SW_FACE_SCALAR_LIST 0
#endif

// SW_CULL_BEFORE_LOAD: 1 = the tile-list walk reads a parallel (centroid, triRad) buffer (t_SoftCull,
// global, same slot as the index) and runs the per-fragment cone cull BEFORE scatter-loading the 3 verts,
// so the culled majority skips the vertex gather. Only the term compute path defines it (and binds
// t_SoftCull); the PS fallback and the C++ test keep the load-then-cull walk. Stored centroid/radius are
// bit-identical to the load path's, so the surviving set - hence the term - is unchanged.
#ifndef SW_CULL_BEFORE_LOAD
	#define SW_CULL_BEFORE_LOAD 0
#endif
// SW_CBL_RT: the RUNTIME predicate (a cbuffer flag) that selects the cull-before-load path when the
// capability is compiled in. Term defines it as (g_surfCost.y != 0); everyone else leaves it false.
#ifndef SW_CBL_RT
	#define SW_CBL_RT false
#endif

// SW_FACE_HOIST_DIRS: 1 = precompute all K sample ray directions into a per-fragment swDir[] array;
// 0 = recompute each direction in-loop from the (unrolled-immediate) disk table + the hoisted rotation
// and basis vectors. Bit-exact either way - same expression, same order. The array costs K*3 VGPRs live
// across the whole triangle loop (48 at K=16), which capped the shipped interaction shader at 96 VGPRs
// = 16 waves/SIMD (RADV shaderstats); recomputing costs ~10 VALU per sample-test but frees the
// registers for occupancy. Distinct from SW_FACE_LEGACY: the triangle-constant qq/e2qq hoists STAY.
#ifndef SW_FACE_HOIST_DIRS
	#define SW_FACE_HOIST_DIRS 0
#endif

// SW_FACE_FP16: 1 = run the Moller-Trumbore 16-sample test loop as PACKED fp16 (two samples per iteration,
// one sample per half2 component) - on RDNA3 the v_pk_* packed ops retire two fp16 MADs per cycle even in
// wave64 fragment shaders, roughly halving the VALU cost of the dominant loop. NOT bit-exact vs fp32:
// world magnitudes overflow half, so every triangle's inputs are RESCALED into a normalized frame first -
// a per-fragment scale kD = 1/max|dir| for the ray directions and a per-triangle scale kT =
// 1/max(|edge1|,|edge2|,|sp|) for the triangle-local vectors. u and v are ratios of same-scaled dots, so
// the [0,1] tests keep their exact form; only the t test needs the cross-scale ratio rr = kT/kD folded
// into its bounds (t in (1e-4,1] becomes t_num in (1e-4*rr*|a|, rr*|a|]). The whole test is DIVISION-FREE
// (sign-folded numerator comparisons instead of 1/aa - fp16 rcp is neither packed nor precise), and
// aa == 0 (parallel ray) self-rejects through the empty sandwich, so the fp32 path's 1e-12 guard has no
// fp16 counterpart. Error shows up only as edge-adjacent sample misclassification (one 1/16 coverage step
// on boundary pixels); the corpus gate is the acceptance judge. Requires -enable-16bit-types (SPIR-V is
// built at SM 6.2); C++ unit tests and any SM < 6.2 build compile the fp32 loop unchanged.
#ifndef SW_FACE_FP16
	#define SW_FACE_FP16 1
#endif
#if SW_FACE_FP16 && !defined(__cplusplus) && defined(__HLSL_ENABLE_16_BIT)
	#define SW_FP16_LOOP 1
#else
	#define SW_FP16_LOOP 0
#endif

#if SW_FP16_LOOP
// largest absolute component - the normalization denominator for the fp16 rescale
float SoftMaxComp3( float3 v )
{
	float3 a = abs( v );
	return max( a.x, max( a.y, a.z ) );
}
#endif

// --------------------------------------------------------------------------- decomposed primitives
// Every intermediate step is a named pure function so each behaviour is unit-testable in isolation
// (neo/tests/SoftShadowPrimitives_test.cpp). Same source compiles as HLSL and C++.

// signed area of the plain triangle (origin, A, B)
SW_FUNC float SoftDisk_Tri( float2 A, float2 B )
{
	return 0.5f * ( A.x * B.y - A.y * B.x );
}

// Fast atan2 approximation (max abs error ~0.0038 rad ~= 0.22 deg) for the sector arc angle only. The
// sector area is 0.5*r^2*angle, so a sub-milliradian angle error is ~1e-3 of the disk area - invisible in
// the shadow - while being several times cheaper than the hardware transcendental. This is the ONLY place
// the coverage is approximated: the winding path uses an exact integer crossing count, not an angle.
SW_FUNC float SoftFastAtan2( float y, float x )
{
	float ax = abs( x );
	float ay = abs( y );
	float mx = max( ax, ay );
	float mn = min( ax, ay );
	float a  = mn / ( mx + 1e-20f );			// [0,1]
	float s  = a * a;
	float r  = ( ( -0.0464964749f * s + 0.15931422f ) * s - 0.327622764f ) * s * a + a;	// atan(a), |err|<2e-3
	if( ay > ax ) { r = 1.57079637f - r; }		// > 45 deg: reflect (pi/2 - r)
	if( x < 0.0f ) { r = 3.14159274f - r; }		// left half:  pi - r
	if( y < 0.0f ) { r = -r; }					// lower half: negate
	return r;
}

// signed area of the circular sector (origin, A..B) of the disk radius^2 = r2: 0.5*r^2*angle(A,B)
SW_FUNC float SoftDisk_Sector( float2 A, float2 B, float r2 )
{
#if SW_FAST_ATAN
	return 0.5f * r2 * SoftFastAtan2( A.x * B.y - A.y * B.x, dot( A, B ) );
#else
	return 0.5f * r2 * atan2( A.x * B.y - A.y * B.x, dot( A, B ) );
#endif
}

// signed angle subtended at the disk centre by the directed edge A->B, in (-pi, pi]. Summed around a
// caster's closed (clipped) loop this is 2*pi times the loop's WINDING NUMBER about the centre.
SW_FUNC float SoftDisk_EdgeAngle( float2 A, float2 B )
{
	return atan2( A.x * B.y - A.y * B.x, dot( A, B ) );
}

// Signed contribution of directed edge A->B to the loop's WINDING NUMBER about the disk centre: +1 if it
// crosses the +X axis upward, -1 downward, else 0. Summed around a closed loop this is the winding number
// as an EXACT INTEGER - the same value as round( sum(EdgeAngle)/2pi ) but with NO transcendental. The
// half-open y>0 test counts a vertex exactly on the axis once, never twice (standard ray-cast convention).
SW_FUNC float SoftDisk_Crossing( float2 A, float2 B )
{
	bool ay = ( A.y > 0.0f );
	bool by = ( B.y > 0.0f );
	if( ay == by )
	{
		return 0.0f;											// both endpoints same side of y=0: no crossing
	}
	float x = A.x + ( B.x - A.x ) * ( -A.y ) / ( B.y - A.y );	// x where the edge meets y=0
	if( x <= 0.0f )
	{
		return 0.0f;											// crossing is on the -X ray: ignore
	}
	return by ? 1.0f : -1.0f;									// upward crossing = +1, downward = -1
}

// intersection parameters of segment A + t*D with the circle |X|^2 = r2. disc <= 0 = no crossing;
// otherwise t1 <= t2 are the entry/exit parameters (on the infinite line; the caller range-checks).
struct softSegRoots_t
{
	float disc;
	float t1;
	float t2;
};
SW_FUNC softSegRoots_t SoftDisk_SegCircleRoots( float2 A, float2 D, float r2 )
{
	softSegRoots_t r;
	float qa = dot( D, D );
	float qb = 2.0f * dot( A, D );
	float qc = dot( A, A ) - r2;
	r.disc = qb * qb - 4.0f * qa * qc;
	r.t1 = 0.0f;
	r.t2 = 0.0f;
	if( r.disc > 0.0f )
	{
		float sq = sqrt( r.disc );
		r.t1 = ( -qb - sq ) / ( 2.0f * qa );
		r.t2 = ( -qb + sq ) / ( 2.0f * qa );
	}
	return r;
}

// Signed area of the disk (centre origin, radius r) intersected with the triangle (origin, A, B). Summed
// over a closed loop's directed edges this yields the signed area of disk INTERSECT polygon. Handles the
// four clip cases: both endpoints in; one in one out; segment crossing; segment entirely outside (a pure
// circular sector). A sector's signed area is 0.5*r^2*angle, the triangle's is 0.5*cross(A,B).
SW_FUNC float SoftDisk_CircleTriArea( float2 A, float2 B, float r2 )		// r2 = disk radius squared (fragment-invariant, hoisted)
{
	float a2 = dot( A, A );
	float b2 = dot( B, B );

	if( a2 <= r2 && b2 <= r2 )
	{
		return SoftDisk_Tri( A, B );							// both inside: plain triangle (checked first: no D/qa needed)
	}

	float2 D  = B - A;
	float  qa = dot( D, D );
	if( qa < 1e-9f )
	{
		return 0.0f;											// degenerate edge (endpoints coincide): no area, avoids /0
	}

	softSegRoots_t rt = SoftDisk_SegCircleRoots( A, D, r2 );

	bool ain = ( a2 <= r2 );
	bool bin = ( b2 <= r2 );

	if( rt.disc <= 0.0f )
	{
		return SoftDisk_Sector( A, B, r2 );						// entirely outside: sector
	}

	float t1 = rt.t1;
	float t2 = rt.t2;

	if( ain && !bin )
	{
		// A inside, B outside: the segment leaves through the EXIT root t2. When rounding pushes t2 just
		// outside [0,1], CLAMP it - substituting the entry root t1 (typically < 0) placed X on the far
		// side of the circle, off the segment entirely (finding F4: fired on 0.25% of rim-grazing edges).
		float  t = min( max( t2, 0.0f ), 1.0f );
		float2 X = A + t * D;
		return SoftDisk_Tri( A, X ) + SoftDisk_Sector( X, B, r2 );
	}
	if( !ain && bin )
	{
		float  t = min( max( t1, 0.0f ), 1.0f );					// entry root, clamped (F4, mirror case)
		float2 X = A + t * D;
		return SoftDisk_Sector( A, X, r2 ) + SoftDisk_Tri( X, B );
	}
	if( t1 >= 0.0f && t1 <= 1.0f && t2 >= 0.0f && t2 <= 1.0f )
	{
		float2 P1 = A + t1 * D;
		float2 P2 = A + t2 * D;
		return SoftDisk_Sector( A, P1, r2 ) + SoftDisk_Tri( P1, P2 ) + SoftDisk_Sector( P2, B, r2 );
	}
	return SoftDisk_Sector( A, B, r2 );							// both outside, no crossing: sector
}

// receiver-centred shading frame: nrm points at the light; (u,v) span the disk plane.
struct softFrame_t
{
	float3 nrm;
	float3 u;
	float3 v;
	float  distPL;
};
SW_FUNC softFrame_t SoftShadow_Frame( float3 swP, float3 swL )
{
	softFrame_t f;
	float3 toL = swL - swP;
	f.distPL = max( length( toL ), 1e-4f );
	f.nrm = toL * ( 1.0f / f.distPL );
	float3 up = ( abs( f.nrm.z ) > 0.9f ) ? float3( 0.0f, 1.0f, 0.0f ) : float3( 0.0f, 0.0f, 1.0f );
	f.u = normalize( cross( up, f.nrm ) );
	f.v = cross( f.nrm, f.u );
	return f;
}

// central projection of a receiver-relative point onto the light plane (depth dn along nrm precomputed)
SW_FUNC float2 SoftShadow_ProjectVert( float3 rel, float dn, softFrame_t f )
{
	return float2( ( f.distPL / dn ) * dot( rel, f.u ), ( f.distPL / dn ) * dot( rel, f.v ) );
}

// caster bounding-sphere cull: wholly behind the receiver, wholly beyond the light, or (when fully in
// front) outside the cone from P subtending the disk. true = skip the caster. Conservative by design.
SW_FUNC bool SoftShadow_CullCaster( float3 dCv, float cRad, softFrame_t f, float sinA, float cosA, float eps )
{
	float dCn = dot( dCv, f.nrm );
	if( dCn + cRad < eps || dCn - cRad > f.distPL )				// wholly behind receiver, or wholly beyond light
	{
		return true;
	}
	if( dCn - cRad > 1e-3f )									// sphere fully in front: angular reject is safe (AO-4)
	{
		float front = dot( dCv, dCv ) - cRad * cRad;
		return dCn < cosA * sqrt( front ) - sinA * cRad;
	}
	return false;
}

// clip the parameter interval of an edge with endpoint depths dnA, dnB to the slab [eps, distPL].
struct softClip_t
{
	float t0;
	float t1;
	bool  empty;
};
SW_FUNC softClip_t SoftShadow_ClipSlab( float dnA, float dnB, float eps, float distPL )
{
	softClip_t c;
	c.t0 = 0.0f;
	c.t1 = 1.0f;
	c.empty = false;
	float d = dnB - dnA;
	if( abs( d ) < 1e-12f )
	{
		if( dnA < eps ) { c.empty = true; }
	}
	else
	{
		float tc = ( eps - dnA ) / d;
		if( d > 0.0f ) { c.t0 = max( c.t0, tc ); }
		else           { c.t1 = min( c.t1, tc ); }
	}
	if( !c.empty )
	{
		if( abs( d ) < 1e-12f )
		{
			if( dnA > distPL ) { c.empty = true; }
		}
		else
		{
			float tc = ( distPL - dnA ) / d;
			if( d > 0.0f ) { c.t1 = min( c.t1, tc ); }
			else           { c.t0 = max( c.t0, tc ); }
		}
	}
	if( c.t0 > c.t1 ) { c.empty = true; }
	return c;
}

// ---------------------------------------------------------------------------------------------------
// Full per-fragment light-disk occlusion. Shared by the pixel shader (interactionSM.ps.hlsl) and the
// unit tests (neo/tests/SoftShadowCoverage_test.cpp, neo/tests/SoftShadowPrimitives_test.cpp), which
// compile this exact source as C++ via hlsl_compat.h - so the tests exercise the LIVE shader math, not
// a copy. Written with only single-component (.x/.y/.z/.w) access, no multi-swizzles, to stay valid in
// both languages. The edge buffer is the global t_SoftEdges in HLSL and a trailing parameter in C++
// (SW_EDGEBUF_PARAM).
//
// swP = receiver world pos, swL = light origin, swR = light-disk radius (penumbra size), swFirstElem =
// this light's first edge record, swN = record count (edges + one header per caster). Returns the
// occluded fraction 0(lit)..1(umbra); the caller forms shadow = 1 - saturate( occ ).
//
// NEAR-PLANE CLOSURE (Sutherland-Hodgman). The blocked region on the disk is the projection of the part
// of the caster IN FRONT of the receiver; the part behind P blocks nothing. So the silhouette is clipped
// to the depth slab [swEps, swDistPL] and only the clipped (front) contour is summed. A vertex behind the
// receiver is CLIPPED OFF, not projected (its projection blows up as dn->0). The near-plane clip inserts a
// straight connector edge (the central projection of the caster's planar near-plane cross-section - lines
// map to lines, so it is exactly straight), whose endpoints sit at radius ~swDistPL/swEps. swEps is kept
// small so those crossings project FAR outside the disk and the connector never cuts it (a connector that
// cut the disk was the semicircular-umbra bug: at a coarse eps two opposite crossings form a diameter).

#ifndef PI
	#define PI 3.14159265358979323846f
#endif

#ifndef SW_NEAR_EPS
	#define SW_NEAR_EPS 1e-3f		// near-plane clip depth; small so near-plane crossings project far off the disk
#endif

#ifdef __cplusplus
	#define SW_EDGEBUF_PARAM , SoftEdgeBuffer t_SoftEdges
	#define SW_TILEBUF_PARAM , SoftTileBuffer t_SoftTiles
	#define SW_UNROLL							// C++ (test) build: HLSL loop attributes have no C++ form
#else
	#define SW_EDGEBUF_PARAM
	#define SW_TILEBUF_PARAM
	#define SW_UNROLL [unroll]
#endif

// SW_ATTRIB: op-count instrumentation for the CPU walk-attribution study. TEXTUALLY EMPTY in HLSL
// (no GPU codegen, anaTerm hashes preserved). In C++ it increments one extern global counter set,
// gated by a RUNTIME flag g_swAttribOn so (a) every test TU has the IDENTICAL inline body - no ODR
// divergence - and (b) the microbench (flag off) pays only a predictable, out-of-line compare, not
// a memory write, so its timing stays clean. Counts live at caster/triangle granularity only (never
// per-FLOP), so even flag-on cost is O(triangles), not O(triangles x samples). The attribution test
// defines g_swAttrib / g_swAttribOn and flips the flag around the measured walk.
#if defined( __cplusplus )
	struct swAttrib_t
	{
		unsigned long long casterTest, casterCull;	// caster bounding-sphere tests / rejects
		unsigned long long coarseTest, coarseCull;	// per-triangle v0-sphere coarse tests / rejects
		unsigned long long tightTest, tightCull;	// per-triangle centroid-sphere tight tests / rejects
		unsigned long long mtTri;					// triangles reaching the Moller-Trumbore + 16-sample loop
	};
	extern swAttrib_t g_swAttrib;
	extern bool       g_swAttribOn;
	#define SW_ATTRIB_ADD( field, n ) do { if( g_swAttribOn ) { g_swAttrib.field += ( n ); } } while( 0 )
	#define SW_BKT_DECL
	#define SW_BKT_SURV
	#define SW_BKT_FLUSH( mask, all )
#elif defined( SW_GPU_WALK_COUNTERS ) && SW_GPU_WALK_COUNTERS
	// GPU counting permutation (softterm.cs built with -D SW_GPU_WALK_COUNTERS=1, bound only under
	// r_softShadowWalkCounters). The counting shader declares u_WalkCnt (register u1) BEFORE this
	// include. The SHIPPED softterm.cs (=0) and the pixel shader keep the empty macro -> byte-identical
	// codegen, so the anaTerm bit-exact contract holds. Field->slot map must match SoftShadowTermPass.
	#define SW_WALKIDX_casterTest	0
	#define SW_WALKIDX_casterCull	1
	#define SW_WALKIDX_coarseTest	2
	#define SW_WALKIDX_coarseCull	3
	#define SW_WALKIDX_tightTest	4
	#define SW_WALKIDX_tightCull	5
	#define SW_WALKIDX_mtTri		6
	#define SW_ATTRIB_ADD( field, n ) InterlockedAdd( u_WalkCnt[ SW_WALKIDX_##field ], ( uint )( n ) )
	// Walk-bucketing by FINAL swMask (lit / penumbra / umbra): count this fragment's MT survivors into a
	// local, flush to the bucket's slots at the walk's return so the walk WORK splits three ways. Slots
	// 8/9 lit frags/survivors, 10/11 penumbra, 12/13 umbra (must match SoftShadowTermPass byteSize).
	#define SW_BKT_DECL   uint swSurvLocal = 0u;
	#define SW_BKT_SURV   swSurvLocal++;
	#define SW_BKT_FLUSH( mask, all )  do { uint swBkt = ( ( mask ) == 0u ) ? 8u : ( ( ( mask ) == ( all ) ) ? 12u : 10u ); InterlockedAdd( u_WalkCnt[ swBkt ], 1u ); InterlockedAdd( u_WalkCnt[ swBkt + 1u ], swSurvLocal ); } while( 0 )
#else
	#define SW_ATTRIB_ADD( field, n )
	#define SW_BKT_DECL
	#define SW_BKT_SURV
	#define SW_BKT_FLUSH( mask, all )
#endif

// swCentreLit > 0.5: the caller GUARANTEES the light-disk centre is visible from swP (AAM penumbra-ring
// pass: the fragment is outside every hard shadow). Under that guarantee any nonzero winding of a
// caster's projected silhouette around the disk centre is geometrically impossible - it is the
// light-apex "illusory umbra" of a large concave caster seen from a far receiver - so the winding
// multiples of the full disk area are subtracted before normalisation. The instant real geometry does
// cover the centre the fragment is hard-blocked and leaves the ring, so the correction is exact where
// enabled. WITHOUT the guarantee (0) the subtraction would delete real umbra - callers outside an AAM
// ring pass must pass 0.
SW_FUNC float SoftShadow_WedgeOcclusion( float3 swP, float3 swL, float swR, int swFirstElem, int swN, float swCentreLit SW_EDGEBUF_PARAM )
{
	swR = max( swR, 1e-2f );
	softFrame_t swF = SoftShadow_Frame( swP, swL );
	float  swDistPL = swF.distPL;									// receiver->light distance
	float3 swNrm = swF.nrm;
	float  swR2  = swR * swR;
	float  swInvDiskArea = 1.0f / ( PI * swR2 );
	float  swSinA = saturate( swR / swDistPL );						// disk half-angle (caster-invariant cull)
	float  swCosA = sqrt( 1.0f - swSinA * swSinA );
	const float swEps = SW_NEAR_EPS;

	float swOcc = 0.0f;
	float swArea = 0.0f;
	float swCross = 0.0f;	// signed +X-axis crossings of the clipped loop = winding number (integer, swCentreLit)
	// haveCaster starts TRUE: edge records arriving before any header (an engine offset bug, or a caller
	// without cull data) are treated as a caster and still occlude. They used to be fully computed and
	// then silently DISCARDED at finalization - an invisible missing-shadow failure mode (finding F14).
	// For well-formed streams (header first) the first header finalizes an empty chain: occ 0, no change.
	bool  haveCaster = true;
	bool  swSkip = false;
	// per-chain shoelace state: swFirst/swPrev are the first/last clipped vertex of the current silhouette
	// chain. A caster concatenates several CLOSED walk-ordered chains (a hole wound the other way, one chain
	// per surface of a multi-part entity); a chain boundary is detected bit-exactly by e0 != previous e1.
	bool   swFirstValid = false;
	float2 swFirst = float2( 0.0f, 0.0f );
	float2 swPrev  = float2( 0.0f, 0.0f );
	bool   havePrevE1 = false;
	float3 prevE1w = float3( 0.0f, 0.0f, 0.0f );
	for( int se = 0; se < swN; se++ )
	{
		float4 e0 = t_SoftEdges[ swFirstElem + se * 2 + 0 ];
		float4 e1 = t_SoftEdges[ swFirstElem + se * 2 + 1 ];
		if( e0.w < 0.0f )		// header record = caster boundary: close+finalize the previous caster, cull the next
		{
			if( haveCaster && swFirstValid )
			{
				swArea += SoftDisk_CircleTriArea( swPrev, swFirst, swR2 );	// close the previous caster's open chain
				swCross += SoftDisk_Crossing( swPrev, swFirst );
			}
			if( haveCaster )
			{
				if( swCentreLit > 0.5f )
				{
					float swWind = swCross;										// crossing count IS the integer winding
					swArea -= swWind * ( PI * swR2 );							// centre visible => winding is illusory
					if( abs( swArea ) > 1.2f * ( PI * swR2 ) )					// beyond the physical bound |area| <= pi r^2:
					{
						swArea = 0.0f;											// near-plane chain-fragment debris, not geometry
					}
				}
				swOcc = max( swOcc, saturate( abs( swArea ) * swInvDiskArea ) );
				if( swOcc >= 0.999f ) { break; }
			}
			haveCaster = true;
			swArea = 0.0f;
			swCross = 0.0f;
			swSkip = false;
			swFirstValid = false;
			havePrevE1 = false;
			float3 dCv  = float3( e0.x, e0.y, e0.z ) - swP;			// caster bounding sphere: centre, radius e1.x
			swSkip = SoftShadow_CullCaster( dCv, e1.x, swF, swSinA, swCosA, swEps );
#if SW_JUMP_SKIP
			if( swSkip )											// culled: jump the whole edge span (e1.y = edgeCount) instead
			{														// of an idle 'continue' per edge - cull is conservative so
				se += ( int )e1.y;									// the skipped edges contribute exactly 0 (bit-exact).
			}
#endif
			continue;
		}
		if( swSkip ) { continue; }

		float3 A = float3( e0.x, e0.y, e0.z );
		float3 B = float3( e1.x, e1.y, e1.z );
		if( havePrevE1 && ( A.x != prevE1w.x || A.y != prevE1w.y || A.z != prevE1w.z ) && swFirstValid )
		{
			swArea += SoftDisk_CircleTriArea( swPrev, swFirst, swR2 );	// close a finished chain before the next
			swCross += SoftDisk_Crossing( swPrev, swFirst );
			swFirstValid = false;
		}
		havePrevE1 = true;
		prevE1w = B;

		// clip the edge's parameter interval to the depth slab [swEps, swDistPL]; an empty interval = the
		// whole edge is behind the receiver or beyond the light, contributes no vertex, and its span is
		// bridged by the connector against swPrev on the next kept edge - keeping the clipped loop closed.
		float3 a = A - swP;
		float3 b = B - swP;
		float  dnA = dot( a, swNrm );
		float  dnB = dot( b, swNrm );
		float  d   = dnB - dnA;
		softClip_t cl = SoftShadow_ClipSlab( dnA, dnB, swEps, swDistPL );
		if( cl.empty ) { continue; }

		float3 pa  = a + cl.t0 * ( b - a );
		float3 pb  = a + cl.t1 * ( b - a );
		float  dna = dnA + cl.t0 * d;
		float  dnb = dnA + cl.t1 * d;
		float2 q0 = SoftShadow_ProjectVert( pa, dna, swF );
		float2 q1 = SoftShadow_ProjectVert( pb, dnb, swF );

		if( swFirstValid ) { swArea += SoftDisk_CircleTriArea( swPrev, q0, swR2 ); swCross += SoftDisk_Crossing( swPrev, q0 ); }
		else               { swFirst = q0; swFirstValid = true; }
		swArea += SoftDisk_CircleTriArea( q0, q1, swR2 );
		swCross += SoftDisk_Crossing( q0, q1 );
		swPrev = q1;
	}
	if( haveCaster && swFirstValid )
	{
		swArea += SoftDisk_CircleTriArea( swPrev, swFirst, swR2 );		// close the last caster's open chain
		swCross += SoftDisk_Crossing( swPrev, swFirst );
	}
	if( haveCaster )
	{
		if( swCentreLit > 0.5f )
		{
			float swWind = swCross;										// crossing count IS the integer winding
			swArea -= swWind * ( PI * swR2 );
			if( abs( swArea ) > 1.2f * ( PI * swR2 ) )
			{
				swArea = 0.0f;
			}
		}
		swOcc = max( swOcc, saturate( abs( swArea ) * swInvDiskArea ) );
	}
	return swOcc;
}

// ===================================================================================================
// RECEIVER-APEX coverage for ONE caster (the cross-section fix). Fed the caster's CANDIDATE edges (all
// potential silhouette edges, NOT pre-filtered by light facing), each as 4 float4:
//   q0 = ( A.xyz, vidA )   q1 = ( B.xyz, vidB )   q2 = ( nA.xyz, boundaryFlag )   q3 = ( nB.xyz, 0 )
// where nA,nB are the two adjacent face normals and vidA,vidB are welded (position-canonical) vertex ids.
// Per fragment it selects the edges that are a silhouette FROM THE RECEIVER (boundary, or the two faces
// straddle P), chains them into loops by shared vertex id (a GLOBAL greedy walk with a used[] bitmask -
// the accurate result needs true chaining; every cheaper order-free form was measured to degrade), and
// sums the clipped circle-triangle areas with the near/far connector in loop order. raBlocksCentre = does
// THIS caster occlude the disk-centre ray (point-light-shadow membership); when false the illusory winding
// of a far caster wrapping the axis is subtracted. O(nEdges^2) by design: correct first, optimise later.
#ifndef SW_RA_MAX
	#define SW_RA_MAX 1024			// max candidate edges per caster (used-bitmask capacity); real casters reach ~940
#endif
SW_FUNC float SoftShadow_ProcCaster( float3 swP, float3 swL, float swR, bool raBlocksCentre, int raFirst, int raN SW_EDGEBUF_PARAM )
{
	swR = max( swR, 1e-2f );
	softFrame_t swF = SoftShadow_Frame( swP, swL );
	float swR2 = swR * swR;
	float swInv = 1.0f / ( PI * swR2 );
	if( raN > SW_RA_MAX ) { raN = SW_RA_MAX; }		// clamp (accuracy loss on huge casters; optimise later)
	uint raUsed[SW_RA_MAX / 32];
	for( int w = 0; w < SW_RA_MAX / 32; w++ ) { raUsed[w] = 0u; }
	float raArea = 0.0f;
	float raAng  = 0.0f;
	for( int s = 0; s < raN; s++ )
	{
		if( ( raUsed[s >> 5] & ( 1u << ( s & 31 ) ) ) != 0u ) { continue; }
		// select + orient edge s from P; skip if not a receiver silhouette
		float4 s0 = t_SoftEdges[ raFirst + s * 4 + 0 ];
		float4 s1 = t_SoftEdges[ raFirst + s * 4 + 1 ];
		float4 s2 = t_SoftEdges[ raFirst + s * 4 + 2 ];
		float4 s3 = t_SoftEdges[ raFirst + s * 4 + 3 ];
		float3 sA = float3( s0.x, s0.y, s0.z ), sB = float3( s1.x, s1.y, s1.z );
		float3 snA = float3( s2.x, s2.y, s2.z ), snB = float3( s3.x, s3.y, s3.z );
		bool sFaP = dot( snA, swP - sA ) > 0.0f, sFbP = dot( snB, swP - sA ) > 0.0f;
		bool sBnd = s2.w > 0.5f;
		if( !( sBnd || ( sFaP != sFbP ) ) ) { continue; }
		// directed start/end vertex ids (front face on the consistent side)
		bool sFlip = sBnd ? ( !sFaP ) : ( sFbP );
		float curEndVid = sFlip ? s0.w : s1.w;
		float3 dA = sFlip ? sB : sA, dB = sFlip ? sA : sB;
		raUsed[s >> 5] |= ( 1u << ( s & 31 ) );
		// project the (clipped) start edge; open the chain
		float2 chFirst = float2( 0.0f, 0.0f ), chPrev = float2( 0.0f, 0.0f );
		bool chValid = false;
		// inline: clip+project a directed edge (dA->dB); accumulate into raArea/raAng with connector-on-open
		// (repeated below for the walk; kept inline for HLSL - no closures)
		{
			float3 a = dA - swP, b = dB - swP;
			float dnA = dot( a, swF.nrm ), dnB = dot( b, swF.nrm ), d = dnB - dnA;
			softClip_t cl = SoftShadow_ClipSlab( dnA, dnB, SW_NEAR_EPS, swF.distPL );
			if( !cl.empty )
			{
				float2 q0 = SoftShadow_ProjectVert( a + cl.t0 * ( b - a ), dnA + cl.t0 * d, swF );
				float2 q1 = SoftShadow_ProjectVert( a + cl.t1 * ( b - a ), dnA + cl.t1 * d, swF );
				chFirst = q0; chPrev = q1; chValid = true;
				raArea += SoftDisk_CircleTriArea( q0, q1, swR2 );
				raAng  += SoftDisk_EdgeAngle( q0, q1 );
			}
		}
		// walk the chain: follow curEndVid to the next unused receiver-silhouette edge whose START == curEndVid
		for( int step = 0; step < raN; step++ )
		{
			int nx = -1;
			for( int j = 0; j < raN; j++ )
			{
				if( ( raUsed[j >> 5] & ( 1u << ( j & 31 ) ) ) != 0u ) { continue; }
				float4 j0 = t_SoftEdges[ raFirst + j * 4 + 0 ];
				float4 j1 = t_SoftEdges[ raFirst + j * 4 + 1 ];
				float4 j2 = t_SoftEdges[ raFirst + j * 4 + 2 ];
				float4 j3 = t_SoftEdges[ raFirst + j * 4 + 3 ];
				float3 jA = float3( j0.x, j0.y, j0.z );
				bool jFaP = dot( float3( j2.x, j2.y, j2.z ), swP - jA ) > 0.0f;
				bool jFbP = dot( float3( j3.x, j3.y, j3.z ), swP - jA ) > 0.0f;
				bool jBnd = j2.w > 0.5f;
				if( !( jBnd || ( jFaP != jFbP ) ) ) { continue; }
				bool jFlip = jBnd ? ( !jFaP ) : ( jFbP );
				float jStart = jFlip ? j1.w : j0.w;
				if( jStart == curEndVid ) { nx = j; break; }
			}
			if( nx < 0 ) { break; }
			raUsed[nx >> 5] |= ( 1u << ( nx & 31 ) );
			float4 n0 = t_SoftEdges[ raFirst + nx * 4 + 0 ];
			float4 n1 = t_SoftEdges[ raFirst + nx * 4 + 1 ];
			float4 n2 = t_SoftEdges[ raFirst + nx * 4 + 2 ];
			float4 n3 = t_SoftEdges[ raFirst + nx * 4 + 3 ];
			float3 nAp = float3( n0.x, n0.y, n0.z ), nBp = float3( n1.x, n1.y, n1.z );
			bool nFaP = dot( float3( n2.x, n2.y, n2.z ), swP - nAp ) > 0.0f;
			bool nFbP = dot( float3( n3.x, n3.y, n3.z ), swP - nAp ) > 0.0f;
			bool nBnd = n2.w > 0.5f;
			bool nFlip = nBnd ? ( !nFaP ) : ( nFbP );
			curEndVid = nFlip ? n0.w : n1.w;
			float3 wA = nFlip ? nBp : nAp, wB = nFlip ? nAp : nBp;
			float3 a = wA - swP, b = wB - swP;
			float dnA = dot( a, swF.nrm ), dnB = dot( b, swF.nrm ), d = dnB - dnA;
			softClip_t cl = SoftShadow_ClipSlab( dnA, dnB, SW_NEAR_EPS, swF.distPL );
			if( cl.empty ) { continue; }		// dropped edge: chain continues, next kept edge bridges from chPrev
			float2 q0 = SoftShadow_ProjectVert( a + cl.t0 * ( b - a ), dnA + cl.t0 * d, swF );
			float2 q1 = SoftShadow_ProjectVert( a + cl.t1 * ( b - a ), dnA + cl.t1 * d, swF );
			if( chValid ) { raArea += SoftDisk_CircleTriArea( chPrev, q0, swR2 ); raAng += SoftDisk_EdgeAngle( chPrev, q0 ); }
			else { chFirst = q0; chValid = true; }
			raArea += SoftDisk_CircleTriArea( q0, q1, swR2 );
			raAng  += SoftDisk_EdgeAngle( q0, q1 );
			chPrev = q1;
		}
		if( chValid ) { raArea += SoftDisk_CircleTriArea( chPrev, chFirst, swR2 ); raAng += SoftDisk_EdgeAngle( chPrev, chFirst ); }
	}
	if( !raBlocksCentre )		// centre visible: subtract illusory winding, drop near-plane chain-fragment debris
	{
		raArea -= floor( raAng * ( 0.5f / PI ) + 0.5f ) * ( PI * swR2 );
		if( abs( raArea ) > 1.2f * ( PI * swR2 ) ) { raArea = 0.0f; }
	}
	return saturate( abs( raArea ) * swInv );
}

// ===================================================================================================
// FRONT-FACE COVERAGE by DISK-SAMPLE UNION. Coverage = fraction of the area light's disk that is occluded
// by the caster, as seen from the receiver: cast SW_FACE_SAMPLES equal-area rays from swP through fixed
// sample points on the disk and count how many are blocked by ANY caster triangle. This is exactly a
// low-sample area-light ray query (the same quantity the RT oracle integrates), so the umbra EMERGES where
// every ray is blocked and the penumbra is the smooth fraction between - no stencil, no hard core stamp.
//
// WHY UNION SAMPLING, not the signed-area integral it replaces. The old body summed each front triangle's
// disk-clipped projected SIGNED area; by Green's theorem that equals the area enclosed by the projected
// front-face BOUNDARY, which is the true receiver silhouette ONLY for a closed, consistently-wound manifold.
// Real Doom3 casters are single-sided walls and non-manifold brush junctions: the front-face boundary does
// not enclose the disk and the signed contributions cancel, so coverage DRAINED to ~0 inside the umbra
// (measured 11.5% lit holes on erebus, worst 0.000 where truth = 1). A per-sample ray hit is a UNION, not a
// signed sum: any triangle covering a sample occludes it, so open/non-manifold/inconsistent-winding geometry
// saturates correctly. Ray tests are double-sided (facing is irrelevant to occlusion), so the front-facing
// select and the winding guard are both gone. Cost is O(samples * triangles) with a per-sample early-out
// (a blocked sample is never retested and a fully-occluded fragment breaks early - the umbra is the CHEAPEST
// case), and the whole fragment stops once every sample is blocked.
//
// ponytail: fixed SW_FACE_SAMPLES gives coverage in 1/N steps -> mild temporal shimmer as a caster crosses a
// sample; raise N or add a per-fragment blue-noise rotation of swDisk if it is visible. N=32 packs into one
// uint mask.
//
// STREAM V2 layout (2026-08-17): a PURE triangle stream - each triangle is THREE contiguous float4,
// ( v0.xyz, triRad ) ( v1.xyz, 0 ) ( v2.xyz, 0 ), triangle t at element swTriBase + t*3 - plus a separate
// per-caster table, 2 float4 per caster: ( centre.xyz, radius ) ( firstTri, numTris, 0, 0 ) at
// swCasterBase. The walk loops casters (sphere cull, then the caster's contiguous tri span), so the hot
// triangle loop has NO header branch and each triangle is one 48B contiguous load (v1's pair encoding was
// 64B with a dead quarter). A culled caster skips its whole span structurally: when every lane culls, the
// wave branches over it (the v1 wave-uniform jump, now for free); disagreeing lanes idle masked. The
// sample mask is global so casters union for free.

// portable 32-bit popcount (HLSL has countbits, but the C++ test build via hlsl_compat.h does not)
SW_FUNC int SoftPopcount32( uint x )
{
	x = x - ( ( x >> 1 ) & 0x55555555u );
	x = ( x & 0x33333333u ) + ( ( x >> 2 ) & 0x33333333u );
	x = ( x + ( x >> 4 ) ) & 0x0f0f0f0fu;
	return ( int )( ( x * 0x01010101u ) >> 24 );
}

// ============================ FUBINI SCANLINE COVERAGE (SW_SCANLINE) ============================
// Exact-union coverage by the Fubini identity: area( disk ∩ ⋃ tri ) = ∫ length( 1D interval-union along a
// horizontal chord ) dy. Discretised as SW_SCAN_CHORDS chords x 32 bits: each occluder triangle fills a
// CONTIGUOUS bit-run per chord, the union across triangles is a free bitwise OR (exactly the shipped
// SW_FACE_SAMPLES mask, just 8x32 = 256 interval-filled samples instead of 16 point tests), coverage =
// popcount( grid & diskMask ) / popcount( diskMask ). No sort, overlap-exact, no 1/N banding along a chord.
// Validated in neo/tests/SoftShadowUnionGap_test.cpp (ScanlineBitGridCov / box_matches_union): 96%/94% of
// penumbra samples within 0.06 of the ray union (best scalar 83%), matching the analytic scanline; the
// accuracy plateaus at 8 chords because the exactness lives ALONG each chord, not in the chord count.
#ifndef SW_SCANLINE
	#define SW_SCANLINE 0
#endif
#if SW_SCANLINE
#define SW_SCAN_CHORDS 8
#define SW_SCAN_BITS   32
// unit-disk half-chord widths: hc_m = sqrt( 1 - y_m^2 ), y_m = -1 + (m+0.5)*(2/SW_SCAN_CHORDS)
static const float SW_SCAN_HC[SW_SCAN_CHORDS] =
{
	0.48412292f, 0.78062475f, 0.92696970f, 0.99215674f,
	0.99215674f, 0.92696970f, 0.78062475f, 0.48412292f,
};
// [aN,bN] in disk-normalised [-1,1] -> a contiguous 32-bit column run (empty if b<a)
SW_FUNC uint SoftScan_Run( float aN, float bN )
{
	int c0 = ( int )floor( ( aN + 1.0f ) * ( 0.5f * SW_SCAN_BITS ) );
	int c1 = ( int )ceil( ( bN + 1.0f ) * ( 0.5f * SW_SCAN_BITS ) ) - 1;
	c0 = max( c0, 0 );
	c1 = min( c1, SW_SCAN_BITS - 1 );
	if( c1 < c0 ) { return 0u; }
	uint w = ( uint )( c1 - c0 + 1 );
	return ( w >= 32u ) ? 0xffffffffu : ( ( ( 1u << w ) - 1u ) << ( uint )c0 );
}
// project a SLAB-clipped triangle to the UNIT light disk and OR its per-chord bit-run into swGrid. The
// run is raw (disk-bbox columns [0,31]); the circular disk mask is applied ONCE at reduction, not per
// triangle. Only the chords the triangle's projected y-extent spans are touched (most triangles cover a
// couple of chords), which is the bulk of the per-triangle saving over walking all SW_SCAN_CHORDS.
SW_FUNC void SoftScan_FillTri( inout uint swGrid[SW_SCAN_CHORDS], float3 v0, float3 v1, float3 v2,
		float3 swP, softFrame_t swF, float swR, float swEps )
{
	float3 rel[3]; float dn[3];
	rel[0] = v0 - swP; rel[1] = v1 - swP; rel[2] = v2 - swP;
	dn[0] = dot( rel[0], swF.nrm ); dn[1] = dot( rel[1], swF.nrm ); dn[2] = dot( rel[2], swF.nrm );
	// Clip the triangle to the depth SLAB [swEps, distPL] BEFORE projecting: only geometry BETWEEN the
	// receiver and the light occludes it. The old code clipped ONLY the near plane (dn >= swEps), so large
	// world triangles that straddle or sit BEYOND the light survived the conservative cone cull and still
	// projected into the disk - painting false self-shadow across lit surfaces (the broad scanline
	// over-darkening). Every other coverage path clips this same slab, and the sampled MT path enforces it
	// with tt <= 1. Two-plane Sutherland-Hodgman in (rel,dn) space -> up to 5 verts.
	float3 nRel[4]; float nDn[4]; int nn = 0;							// after near clip (dn >= swEps)
	for( int e = 0; e < 3; e++ )
	{
		int i = e, k = ( e + 1 ) % 3;
		bool ai = dn[i] >= swEps, bi = dn[k] >= swEps;
		if( ai && nn < 4 ) { nRel[nn] = rel[i]; nDn[nn] = dn[i]; nn++; }
		if( ( ai != bi ) && nn < 4 ) { float t = ( swEps - dn[i] ) / ( dn[k] - dn[i] ); nRel[nn] = rel[i] + ( rel[k] - rel[i] ) * t; nDn[nn] = swEps; nn++; }
	}
	if( nn < 3 ) { return; }
	float3 fRel[5]; float fDn[5]; int fn = 0;							// after far clip (dn <= distPL = the light)
	for( int e = 0; e < nn; e++ )
	{
		int i = e, k = ( e + 1 ) % nn;
		bool ai = nDn[i] <= swF.distPL, bi = nDn[k] <= swF.distPL;
		if( ai && fn < 5 ) { fRel[fn] = nRel[i]; fDn[fn] = nDn[i]; fn++; }
		if( ( ai != bi ) && fn < 5 ) { float t = ( swF.distPL - nDn[i] ) / ( nDn[k] - nDn[i] ); fRel[fn] = nRel[i] + ( nRel[k] - nRel[i] ) * t; fDn[fn] = swF.distPL; fn++; }
	}
	if( fn < 3 ) { return; }
	float invR = 1.0f / swR;
	float2 q[5]; int qn = fn; float ymin = 1e30f, ymax = -1e30f; float xmin = 1e30f, xmax = -1e30f;
	for( int j = 0; j < fn; j++ ) { float2 p = SoftShadow_ProjectVert( fRel[j], fDn[j], swF ) * invR; q[j] = p; ymin = min( ymin, p.y ); ymax = max( ymax, p.y ); xmin = min( xmin, p.x ); xmax = max( xmax, p.x ); }
	// LOSSLESS DISK REJECT: if the nearest point of the projected triangle's AABB to the disk centre is
	// already outside the UNIT CIRCLE, the triangle covers no disk bit (the circular mask is applied at
	// reduction). The cone cull upstream is a loose bounding-SPHERE test, so ~99% of its survivors still
	// project clear of the small disk (grazing miss) and would otherwise pay the per-edge reciprocal setup
	// + chord sweep below - the bulk of FillTri (measured 31-53% of the scanline walk; -11..-20% walk on the
	// heavy caps). Circle-exact (not just the [-1,1]^2 box) so it also drops the corner-missers a box keeps:
	// measured no different on these dense caps, but the corner fraction is scene-dependent (a wide-penumbra
	// scene with survivors clustered in the disk corners gains where these do not), and the extra cost is a
	// few ALU. Conservative: never drops a triangle that touches the disk, so coverage is bit-identical.
	{
		float nx = ( xmin > 0.0f ) ? xmin : ( ( xmax < 0.0f ) ? xmax : 0.0f );
		float ny = ( ymin > 0.0f ) ? ymin : ( ( ymax < 0.0f ) ? ymax : 0.0f );
		if( nx * nx + ny * ny > 1.0f ) { return; }
	}
	const float halfC = SW_SCAN_CHORDS * 0.5f;							// chord index m for chord centre Y: m = (Y+1)*halfC - 0.5
	int mLo = max( ( int )ceil( ( ymin + 1.0f ) * halfC - 0.5f ), 0 );
	int mHi = min( ( int )floor( ( ymax + 1.0f ) * halfC - 0.5f ), SW_SCAN_CHORDS - 1 );
	if( mLo > mHi ) { return; }											// projects between chord centres / outside disk in Y
	// ALREADY-COVERED SKIP (lossless): the grid is an OR-union, so a triangle whose projected-AABB column
	// run is ALREADY fully set in swGrid on every chord it spans can add no new bit (its coverage is a
	// subset of its bbox, which is a subset of the already-set columns). Skip it before the per-edge
	// reciprocals + chord sweep. The disk reject above drops the ~34% of survivors that miss the disk
	// entirely; MEASURED the remaining ~66% mostly OVERLAP the disk but re-fill bits a closer occluder
	// already set (redundant) - this catches those, the bulk of the 44 ms chord-sweep mass. Operates on the
	// RAW grid (pre-disk-mask): raw coverage unchanged => popcount( grid & diskMask ) is bit-identical.
	{
		uint bboxRun = SoftScan_Run( xmin, xmax );
		bool newBits = false;
		for( int mc = mLo; mc <= mHi; mc++ ) { if( ( swGrid[mc] & bboxRun ) != bboxRun ) { newBits = true; break; } }
		if( !newBits ) { return; }
	}
	// per-edge line params hoisted OUT of the chord loop: the reciprocal 1/(B.y-A.y) is the expensive
	// term and is chord-invariant, so each chord evaluation is a single FMA x = A.x + slope*(Y - A.y).
	float eax[5], eay[5], eslope[5], elo[5], ehi[5];
	for( int e = 0; e < 5; e++ )
	{
		if( e >= qn ) { eax[e] = 0; eay[e] = 0; eslope[e] = 0; elo[e] = 1e30f; ehi[e] = -1e30f; continue; }
		float2 A = q[e], B = q[( e + 1 ) % qn];
		eax[e] = A.x; eay[e] = A.y; eslope[e] = ( B.x - A.x ) / ( B.y - A.y );
		elo[e] = min( A.y, B.y ); ehi[e] = max( A.y, B.y );
	}
	for( int m = mLo; m <= mHi; m++ )									// only the chords the triangle spans
	{
		float Y = -1.0f + ( ( float )m + 0.5f ) * ( 2.0f / SW_SCAN_CHORDS );
		float xlo = 1e30f, xhi = -1e30f; bool any = false;
		for( int e2 = 0; e2 < 5; e2++ )
		{
			if( Y >= elo[e2] && Y < ehi[e2] )						// chord crosses this edge (half-open = parity-exact)
			{
				float x = eax[e2] + eslope[e2] * ( Y - eay[e2] );
				xlo = min( xlo, x ); xhi = max( xhi, x ); any = true;
			}
		}
		if( any ) { swGrid[m] |= SoftScan_Run( xlo, xhi ); }
	}
}
#endif // SW_SCANLINE

#ifndef SW_FACE_SAMPLES
	#define SW_FACE_SAMPLES 16			// equal-area disk samples (8, 16 or 32); <=32 to pack the occlusion mask in one uint.
#endif
// MEASUREMENT (#3 magnitude): skip the per-fragment cone cull in the tile-list walk and MT every list tri
// directly. LOSSLESS (MT rejects the same tris the cull would). Tests whether the cull ALU is worth its
// cost or is free (load-dominated). Set to 1 to measure, 0 = shipped.
#ifndef SW_SKIP_CULL
	#define SW_SKIP_CULL 0
#endif
// ELIMINATION probe: 99% of survivors reaching MT set no bit (measured). Most lit fragments walk their
// whole list with the mask stuck at 0. Bail after this many survivors if still fully unblocked (assume
// lit). 0 = off. Approximate: a fragment blocked ONLY by a late-in-list occluder is wrongly lit.
#ifndef SW_LIT_EARLYOUT
	#define SW_LIT_EARLYOUT 0
#endif
// SURFACE-FOLD CACHE probe (r_softShadowSurfCache): compiles the cached-texel consume path (bilerp of
// per-texel corner values + exact residual/dynamic walk) into the term CS. 0 = shipped (the probe
// permutation is a separate pipeline; the default shader stays byte-identical).
#ifndef SW_SURF_CACHE
	#define SW_SURF_CACHE 0
#endif
//	8 = perf tier, GATE-REJECTED as the default (2026-08-16): 2 EXTENT defects (erebus1_10/11 - the 1/8
//	coverage quantum shrinks the penumbra body past the 10% tolerance) for only ~12% frame time - the
//	integral is WALK-bound, not sample-bound. 16 = the accuracy set (shipped).

#if SW_FP16_LOOP
// PACKED fp16 Moller-Trumbore for ONE triangle: tests all still-unblocked samples two at a time (one per
// half2 component) and returns the updated occlusion mask. Shared by both the full walk and the tile-binned
// walk so the two cannot drift. Triangle inputs arrive fp32 (including the hoisted qq/e2qq, which keep
// their fp32 cancellation accuracy); everything half-domain is rescaled by kT = 1/max(|edge1|,|edge2|,|sp|)
// and kD = 1/swMD. u,v are ratios of same-scaled dots so their tests keep the exact fp32 form; t's bounds
// absorb the cross-scale ratio rr = kT/kD. The test is division-free (sign-folded numerators, no fp16 rcp)
// and aa == 0 self-rejects via the empty sandwich. See the SW_FACE_FP16 doc block for the full numerics.
uint SoftShadow_FaceTriHitsFP16( uint swMask, float3 edge1, float3 edge2, float3 sp, float3 qq, float e2qq,
		float3 swBase, float3 swU2, float3 swV2, float swMD, float swKD,
		in float2 swDisk[SW_FACE_SAMPLES] )
{
	// KEY REDUCTION: dir = base + dx*U2 + dy*V2 (rotation pre-folded into U2/V2), and every MT numerator
	// is linear in dir, so aa/un/vn are AFFINE in the disk coords: aa = A0 + dx*A1 + dy*A2 etc. The nine
	// affine coefficients are triangle constants - computed here ONCE in fp32 (keeping the cancellation-
	// prone crosses/dots in full precision) and rounded to half - so the per-pair packed work is just two
	// v_pk_fma per numerator plus the folded interval tests.
	float mT  = max( max( SoftMaxComp3( edge1 ), SoftMaxComp3( edge2 ) ), max( SoftMaxComp3( sp ), 1e-19f ) );
	float kT  = 1.0f / mT;
	float ss  = swKD * kT * kT;			// shared scale of aa/un/vn: every term is (dir-deg-1)*(tri-deg-2)
	float3 c0 = cross( swBase, edge2 );
	float3 c1 = cross( swU2, edge2 );
	float3 c2 = cross( swV2, edge2 );
	float16_t A0 = float16_t( dot( edge1, c0 ) * ss );
	float16_t A1 = float16_t( dot( edge1, c1 ) * ss );
	float16_t A2 = float16_t( dot( edge1, c2 ) * ss );
	float16_t U0 = float16_t( dot( sp, c0 ) * ss );
	float16_t U1 = float16_t( dot( sp, c1 ) * ss );
	float16_t U2 = float16_t( dot( sp, c2 ) * ss );
	float16_t V0 = float16_t( dot( swBase, qq ) * ss );
	float16_t V1 = float16_t( dot( swU2, qq ) * ss );
	float16_t V2 = float16_t( dot( swV2, qq ) * ss );
	float16_t tnh = float16_t( e2qq * ( kT * kT * kT ) );
	float rr = swMD * kT;								// = kT/kD: cross-scale ratio for the t bounds
	float16_t tLo = float16_t( 1e-4f * rr );
	float16_t tHi = float16_t( min( rr, 3.0e4f ) );		// keep tHi finite in half; |t_num| <= ~8 so a
	//													   clamped bound only ever rejects degenerate sa
	const float16_t2 zz = float16_t2( 0.0, 0.0 );
	for( int i = 0; i < SW_FACE_SAMPLES; i += 2 )
	{
		if( ( ( swMask >> i ) & 3u ) == 3u ) { continue; }	// both samples of the pair already blocked
		float16_t2 dx  = float16_t2( swDisk[i].x, swDisk[i + 1].x );	// compile-time packed literals
		float16_t2 dy  = float16_t2( swDisk[i].y, swDisk[i + 1].y );
		float16_t2 aa  = A0 + dx * A1 + dy * A2;
		float16_t2 un  = U0 + dx * U1 + dy * U2;
		float16_t2 ff  = select( aa < zz, float16_t2( -1.0, -1.0 ), float16_t2( 1.0, 1.0 ) );	// double-sided sign fold
		float16_t2 sa  = ff * aa;
		float16_t2 su  = ff * un;
		// The interval tests run as packed min-arithmetic (a condition is one 'min partial >= 0' at the
		// end) instead of per-component compare/mask chains - v_pk_min_f16 runs at the packed 2x rate,
		// the scalar-cmp tail does not. Equality lands on the inclusive side (boundary hair, fp16 already
		// owns the boundary). Staged reject on the u test (u in [0,1] <=> su in [0,sa]): sample rays are
		// wave-coherent, so when this pair misses in u it usually misses across the whole wave and the
		// s_cbranch_execz skips the v/t work - the early-out shape that makes the fp32 loop cheap on the
		// all-miss majority.
		float16_t2 mU  = min( su, sa - su );
		if( !any( mU >= zz ) ) { continue; }
		float16_t2 vn  = V0 + dx * V1 + dy * V2;
		float16_t2 sv  = ff * vn;
		float16_t2 st  = ff * tnh;
		float16_t2 mV  = min( sv, sa - su - sv );
		float16_t2 mT2 = min( st - tLo * sa, tHi * sa - st );
		bool2 hit = min( mU, min( mV, mT2 ) ) >= zz;
		swMask |= ( ( hit.x ? 1u : 0u ) | ( hit.y ? 2u : 0u ) ) << i;
	}
	return swMask;
}
#endif


SW_FUNC float SoftShadow_FaceCoverage( float3 swP, float3 swL, float swR, int swTriBase, int swCasterBase, int swCasterCount, float swRotAng SW_EDGEBUF_PARAM )
{
	swR = max( swR, 1e-2f );
	softFrame_t swF = SoftShadow_Frame( swP, swL );
	float  swDistPL = swF.distPL;
	float  swSinA = saturate( swR / swDistPL );
	float  swCosA = sqrt( 1.0f - swSinA * swSinA );
	const float swEps = SW_NEAR_EPS;

	// unit-disk sample coords (golden-angle sunflower: equal area => coverage = occluded/N is unbiased) and the
	// 6 nearest disk-neighbours of each, packed 5 bits each (for the morphological crack-close below).
#if SW_FACE_SAMPLES == 8
	// PERF TIER (explicit 60-FPS directive): half the ray budget of the 16 set, same golden-angle
	// equal-area construction, so coverage stays unbiased - just coarser (1/8 quantum). The gate
	// prices the accuracy cost per build; regenerate via the neighbour-packing script in-tree.
	const float2 swDisk[8] =
	{
		float2( 0.250000f, 0.000000f), float2(-0.319290f, 0.292496f),
		float2( 0.048872f,-0.556877f), float2( 0.402444f, 0.524918f),
		float2(-0.738535f,-0.130636f), float2( 0.699605f,-0.445031f),
		float2(-0.234004f, 0.870484f), float2(-0.446271f,-0.859268f),
	};
	const uint swNbr[8] =
	{
		0x08609443u, 0x0e218086u, 0x06121407u, 0x082284c0u,
		0x066008e1u, 0x08138c40u, 0x0a220061u, 0x06508082u,
	};
#elif SW_FACE_SAMPLES == 16
	const float2 swDisk[16] =
	{
		float2( 0.176777f, 0.000000f), float2(-0.225772f, 0.206826f),
		float2( 0.034558f,-0.393771f), float2( 0.284571f, 0.371173f),
		float2(-0.522223f,-0.092374f), float2( 0.494695f,-0.314685f),
		float2(-0.165466f, 0.615525f), float2(-0.315561f,-0.607594f),
		float2( 0.684642f, 0.250030f), float2(-0.712256f, 0.294009f),
		float2( 0.343354f,-0.733729f), float2( 0.253730f, 0.808932f),
		float2(-0.764746f,-0.443186f), float2( 0.897134f,-0.197232f),
		float2(-0.547507f, 0.778772f), float2(-0.126487f,-0.976090f),
	};
	const uint swNbr[16] =
	{
		0x0c809443u, 0x04348086u, 0x08f2a807u, 0x0a132d00u,
		0x0023a581u, 0x0681014du, 0x0091adc1u, 0x00a231e2u,
		0x02b281a3u, 0x00c33824u, 0x1a03bc45u, 0x00e0a0c3u,
		0x02f124e4u, 0x04350105u, 0x06458526u, 0x08560947u,
	};
#else
	const float2 swDisk[32] =
	{
		float2( 0.125000f, 0.000000f), float2(-0.159645f, 0.146248f),
		float2( 0.024436f,-0.278438f), float2( 0.201222f, 0.262459f),
		float2(-0.369268f,-0.065318f), float2( 0.349802f,-0.222516f),
		float2(-0.117002f, 0.435242f), float2(-0.223136f,-0.429634f),
		float2( 0.484115f, 0.176798f), float2(-0.503641f, 0.207896f),
		float2( 0.242788f,-0.518824f), float2( 0.179414f, 0.572001f),
		float2(-0.540757f,-0.313380f), float2( 0.634370f,-0.139464f),
		float2(-0.387146f, 0.550675f), float2(-0.089440f,-0.690200f),
		float2( 0.549072f, 0.462758f), float2(-0.738878f, 0.030555f),
		float2( 0.538955f,-0.536332f), float2(-0.036058f, 0.779792f),
		float2(-0.512818f,-0.614527f), float2( 0.812360f, 0.109302f),
		float2(-0.688311f, 0.478909f), float2( 0.188086f,-0.836061f),
		float2( 0.435033f, 0.759191f), float2(-0.850448f,-0.271316f),
		float2( 0.826102f,-0.381680f), float2(-0.357888f, 0.855156f),
		float2(-0.319407f,-0.888034f), float2( 0.849909f, 0.446688f),
		float2(-0.944035f, 0.248845f), float2( 0.536596f,-0.834530f),
	};
	const uint swNbr[32] =
	{
		0x0c809443u, 0x04348086u, 0x08f2a807u, 0x20132d00u,
		0x0478a581u, 0x1121014du, 0x1239adc1u, 0x384a31e2u,
		0x0a06d470u, 0x3ce0d891u, 0x3ef15cb2u, 0x11036073u,
		0x1313e684u, 0x01246aa5u, 0x0334db66u, 0x28255f87u,
		0x2a35e3a8u, 0x2cc267c9u, 0x2ed2ebeau, 0x07871b6bu,
		0x09979f8cu, 0x0ba8750du, 0x0db8f92eu, 0x0fc97d4fu,
		0x103ece0bu, 0x13e2522cu, 0x15f2d64du, 0x12bb1a6eu,
		0x14cb9e8fu, 0x06dc22b0u, 0x08eca6d1u, 0x1e5d2af2u,
	};
#endif
	// ray to disk sample i = swBase + swSu*rot(swDisk[i]) (t=1 lands on the light plane)
	float3 swBase = swL - swP;
	float3 swSu = swF.u * swR;
	float3 swSv = swF.v * swR;
	// per-fragment rotation of the fixed sample set: a shared pattern correlates the sampling error across
	// neighbouring pixels into a visible fixed-pattern bias (measured worst meanAbs 0.099 on erebus5/6),
	// and a poorly-decorrelated rotation leaves the 1/N coverage quanta as MAP-CONTOUR BANDS across wide
	// penumbras (2026-08-17 play-test). The angle now comes from the CALLER: the pixel shader samples the
	// 512x512 blue-noise texture (screen-anchored, properly decorrelated neighbours); the C++ tests pass
	// the legacy position-hash (SoftRotAngle in SoftShadowBox.h) so their expected values stay bit-exact.
	float  swCa = cos( swRotAng );
	float  swSa = sin( swRotAng );

#if SW_FP16_LOOP
	// per-fragment fp16 frame: swMD bounds every sample ray component (|dir| <= |swBase| + |swSu| + |swSv|
	// componentwise, rotation-invariant bound). The per-fragment disk rotation is FOLDED INTO THE BASIS
	// ( dir = base + dx*(Su ca + Sv sa) + dy*(Sv ca - Su sa) ), so the packed loop consumes raw compile-
	// time disk constants - no per-sample rotate. These stay fp32: only the per-triangle affine
	// coefficients cross into half, inside SoftShadow_FaceTriHitsFP16.
	float  swMD = SoftMaxComp3( swBase ) + SoftMaxComp3( swSu ) + SoftMaxComp3( swSv );
	float  swKD = 1.0f / max( swMD, 1e-19f );
	float3 swU2 = swSu * swCa + swSv * swSa;
	float3 swV2 = swSv * swCa - swSu * swSa;
#endif

	// Each sample ray direction depends only on (fragment, sample) - NOT on the triangle - so precompute all K
	// once per fragment instead of recomputing the rotate + disk placement inside the triangle loop for every
	// triangle. Bit-exact (identical values). Trades a per-fragment swDir[] (register pressure) for removing that
	// redundant per-triangle work; the sample loop below just reads swDir[i].
#if !SW_FACE_LEGACY && SW_FACE_HOIST_DIRS
	float3 swDir[SW_FACE_SAMPLES];
	for( int di = 0; di < SW_FACE_SAMPLES; di++ )
	{
		float2 s0 = swDisk[di];
		float2 sc = float2( s0.x * swCa - s0.y * swSa, s0.x * swSa + s0.y * swCa );
		swDir[di] = swBase + swSu * sc.x + swSv * sc.y;
	}
#endif

	uint swMask = 0u;						// bit i set once sample i's ray is blocked by any triangle (union)
	const uint swAll = 0xffffffffu >> ( 32 - SW_FACE_SAMPLES );
	SW_BKT_DECL
#if SW_FACE_PROFILE
	// keeps the probed stages LIVE: the accumulator feeds an unprovable branch at the end, so the
	// compiler cannot dead-code-eliminate the walk/culls the probe is supposed to time (it DID -
	// the first probe benched BELOW the soft-off floor).
	float swProbe = 0.0f;
#endif
	for( int sc = 0; sc < swCasterCount; sc++ )
	{
		float4 c0 = t_SoftEdges[ swCasterBase + sc * 2 + 0 ];		// ( centre.xyz, radius )
		float4 c1 = t_SoftEdges[ swCasterBase + sc * 2 + 1 ];		// ( firstTri, numTris, 0, 0 )
		float3 dCv = float3( c0.x, c0.y, c0.z ) - swP;
		SW_ATTRIB_ADD( casterTest, 1 );
		if( SoftShadow_CullCaster( dCv, c0.w, swF, swSinA, swCosA, swEps ) )
		{
			// structural span skip: when EVERY lane culls, the wave branches over the whole span (the
			// v1 wave-uniform jump, now implicit); a disagreeing lane just idles masked - no lane ever
			// pays per-record skip iterations. Bit-exact: the cull is conservative.
			SW_ATTRIB_ADD( casterCull, 1 );
			continue;
		}
		const int swTriFirst = ( int )c1.x;
		const int swTriEnd   = swTriFirst + ( int )c1.y;
#if SW_FACE_PROFILE == 1
		swProbe += c0.x; continue;			// TIMING PROBE ONLY: caster walk + culls, no triangle work
#endif
		for( int t = swTriFirst; t < swTriEnd; t++ )
		{
			const int b = swTriBase + t * 3;
			float4 r0 = t_SoftEdges[ b + 0 ];						// ( v0.xyz, v0-radius ) - coarse reject reads ONLY this
			SW_ATTRIB_ADD( coarseTest, 1 );
			// COARSE v0-CENTERED REJECT: rejects far triangles from r0 alone, so r1/r2 (48B, the measured
			// dominant per-fragment cost) load lazily only for survivors. The v0-sphere contains the whole
			// triangle, so this only ever DEFERS a reject the tight centroid re-cull below also makes =>
			// coverage is BIT-EXACT. Squared, sqrt-free (coneR>0 after the slab tests, v0-radius>=0).
			{
				float3 rc0 = float3( r0.x, r0.y, r0.z ) - swP;
				float  cd0 = dot( rc0, swF.nrm );
				float  vr0 = r0.w;
				if( cd0 + vr0 < swEps ) { SW_ATTRIB_ADD( coarseCull, 1 ); continue; }
				if( cd0 - vr0 > swDistPL ) { SW_ATTRIB_ADD( coarseCull, 1 ); continue; }
				float3 pp0 = rc0 - cd0 * swF.nrm;
				float  cr0 = swR * ( cd0 + vr0 ) / swDistPL;
				if( dot( pp0, pp0 ) > ( cr0 + vr0 ) * ( cr0 + vr0 ) ) { SW_ATTRIB_ADD( coarseCull, 1 ); continue; }
			}
			float4 r1 = t_SoftEdges[ b + 1 ];						// ( v1.xyz, centroid-radius ) - lazy: survivors only
			float4 r2 = t_SoftEdges[ b + 2 ];						// ( v2.xyz, 0 )
			float3 v0 = float3( r0.x, r0.y, r0.z );
			float3 v1 = float3( r1.x, r1.y, r1.z );
			float3 v2 = float3( r2.x, r2.y, r2.z );
			// PER-TRIANGLE CONE/SLAB REJECT (the big perf lever). The sample rays form a cone: apex swP, axis
			// swF.nrm, cross-section radius swR at depth swDistPL. A triangle that cannot reach that cone can
			// hit NO sample, so skip its ray tests entirely. Conservative (over-keeps) => bit-exact. triRad is
			// precomputed at stream-build time (a hair inflated) and carried in r0.w.
			// The reject is squared (perp.perp > (coneR+triRad)^2) rather than sqrt(perp.perp)-triRad > coneR:
			// coneR>0 (cd+triRad>=swEps after the slab tests) and triRad>=0, so the square is monotone and the
			// reject set is unchanged. Drops one sqrt per triangle per fragment on the ~70%-of-term cull path.
			float3 tcen = ( v0 + v1 + v2 ) * ( 1.0f / 3.0f );
			float3 rc   = tcen - swP;
			float  cd   = dot( rc, swF.nrm );						// centroid depth along the cone axis
			float  triRad = r1.w;	// centroid radius (tight): exact original bound, so the sample-gated set is bit-exact
			SW_ATTRIB_ADD( tightTest, 1 );
			if( cd + triRad < swEps ) { SW_ATTRIB_ADD( tightCull, 1 ); continue; }					// wholly behind the receiver
			if( cd - triRad > swDistPL ) { SW_ATTRIB_ADD( tightCull, 1 ); continue; }				// wholly beyond the light
			float3 perp = rc - cd * swF.nrm;
			float  coneR = swR * ( cd + triRad ) / swDistPL;		// max cone radius over the triangle's depth span
			if( dot( perp, perp ) > ( coneR + triRad ) * ( coneR + triRad ) ) { SW_ATTRIB_ADD( tightCull, 1 ); continue; }	// outside the sample cone: cannot occlude
			SW_ATTRIB_ADD( mtTri, 1 );	// survivor: pays the Moller-Trumbore + 16-sample test
			SW_BKT_SURV
#if SW_FACE_PROFILE == 2
			swProbe += cd; continue;		// TIMING PROBE ONLY: + per-triangle cone culls, no setup/samples
#endif
			float3 edge1 = v1 - v0;
			float3 edge2 = v2 - v0;
			float3 sp = swP - v0;
			// Moller-Trumbore, double-sided (occlusion is facing-independent): a hit with t in (0,1] means the
			// triangle lies between the receiver and the light plane along this sample ray. All K rays share the
			// receiver origin, so qq = cross(sp,edge1) and e2qq = dot(edge2,qq) are triangle-constant - hoisted
			// out of the K-sample loop. Bit-exact; saves a cross + a dot per sample.
			float3 qq   = cross( sp, edge1 );
			float  e2qq = dot( edge2, qq );
#if SW_FP16_LOOP
			swMask = SoftShadow_FaceTriHitsFP16( swMask, edge1, edge2, sp, qq, e2qq,
												 swBase, swU2, swV2, swMD, swKD, swDisk );
#else
			for( int i = 0; i < SW_FACE_SAMPLES; i++ )
			{
				if( ( swMask & ( 1u << i ) ) != 0u ) { continue; }	// sample already blocked: skip
#if !SW_FACE_HOIST_DIRS
				// in-loop direction from unrolled immediates: identical expression to the hoisted array,
				// ~10 VALU per test, zero registers held across the triangle loop (occupancy win)
				float2 s0  = swDisk[i];
				float2 sc2 = float2( s0.x * swCa - s0.y * swSa, s0.x * swSa + s0.y * swCa );
				float3 dir = swBase + swSu * sc2.x + swSv * sc2.y;
#else
				float3 dir = swDir[i];								// precomputed once per fragment (hoisted)
#endif
				float3 h   = cross( dir, edge2 );
				float  aa  = dot( edge1, h );
				if( abs( aa ) < 1e-12f ) { continue; }				// ray parallel to triangle
				float  inv = 1.0f / aa;
				float  u   = inv * dot( sp, h );
				if( u < 0.0f || u > 1.0f ) { continue; }
				float  vv  = inv * dot( dir, qq );
				if( vv < 0.0f || u + vv > 1.0f ) { continue; }
				float  tt  = inv * e2qq;
				if( tt > 1e-4f && tt <= 1.0f ) { swMask |= ( 1u << i ); }
			}
#endif	// SW_FP16_LOOP
			if( swMask == swAll ) { break; }	// every sample blocked: fully in umbra
		}
		if( swMask == swAll ) { break; }		// umbra: no caster can add anything
	}
	// Morphological CLOSE: seal INTERIOR tessellation cracks without touching the penumbra. A sample ray that
	// threads a T-junction / brush-seam gap is reported lit even deep in the umbra. Uniform barycentric dilation
	// would seal them but also expands the OUTER silhouette, over-darkening the penumbra toe (measured +0.35 bias).
	// Instead: fill an unblocked sample only when >=5 of its 6 disk NEIGHBOURS are blocked - true for an interior
	// crack (nearly surrounded by umbra) but not for the penumbra boundary (there the unblocked region connects to
	// the lit outside, so a toe sample keeps >=2 lit neighbours). Streaming ALL casters into one call so the sample
	// mask unions across them removes the bulk of holes; this close mops up the last single-caster interior cracks.
	// Rotation-invariant: the neighbour topology survives the per-fragment disk rotation. The close only ever SETS
	// bits, so it is a no-op when no sample is blocked (fully lit) or all are (fully umbra) - guard it out there,
	// which skips it on the lit majority of the frame. Bit-exact: the guarded cases change nothing.
#if !SW_FACE_LEGACY
	if( swMask != 0u && swMask != swAll )
#endif
	{
		uint filled = swMask;
		for( int i = 0; i < SW_FACE_SAMPLES; i++ )
		{
			if( ( swMask & ( 1u << i ) ) != 0u ) { continue; }
			uint nb = swNbr[i];
			int blocked = 0;
			for( int k = 0; k < 6; k++ )
			{
				int j = ( int )( ( nb >> ( 5 * k ) ) & 31u );
				if( ( swMask & ( 1u << j ) ) != 0u ) { blocked++; }
			}
			if( blocked >= 5 ) { filled |= ( 1u << i ); }
		}
		swMask = filled;
	}
#if SW_FACE_PROFILE
	if( swProbe > 1e30f ) { swMask = swAll; }	// never true; makes the probe accumulator observable
#endif
	SW_BKT_FLUSH( swMask, swAll );
	return ( float )SoftPopcount32( swMask ) / ( float )SW_FACE_SAMPLES;
}

// ---------------------------------------------------------------------------------------------------
// TILE-BINNED face coverage (r_softShadowTileBin). A per-light compute prepass (softtile_bin.cs.hlsl)
// tests every triangle record against each 16x16 screen tile's depth-bounded receiver volume with the
// SAME cone/slab cull the full walk runs per fragment - conservatively inflated by the tile's world
// radius - and writes the surviving PAIR-START indices per tile. This walk then touches only those:
// the measured decomposition (SW_FACE_PROFILE, erebus1_09) put the per-fragment record walk at 51%
// and the per-triangle culls at 18% of the soft cost, all of it recomputing a result that is nearly
// identical across a tile's fragments. Headers never reach the list (face coverage unions ALL
// triangles; caster identity only ever mattered for culling), so there is no header/caster logic here.
// The tri test + close are duplicated from SoftShadow_FaceCoverage in its SHIPPED config (hoists on,
// in-loop dirs); SoftShadowTileBin_test.cpp holds the two walks bit-identical so they cannot drift.
SW_FUNC float SoftShadow_FaceCoverageList( float3 swP, float3 swL, float swR, int swFirstElem,
		int swListBase, int swListCount, float swRotAng SW_TILEBUF_PARAM SW_EDGEBUF_PARAM )
{
	swR = max( swR, 1e-2f );
	softFrame_t swF = SoftShadow_Frame( swP, swL );
	float  swDistPL = swF.distPL;
	const float swEps = SW_NEAR_EPS;
#if SW_FACE_SAMPLES == 8
	const float2 swDisk[8] =
	{
		float2( 0.250000f, 0.000000f), float2(-0.319290f, 0.292496f),
		float2( 0.048872f,-0.556877f), float2( 0.402444f, 0.524918f),
		float2(-0.738535f,-0.130636f), float2( 0.699605f,-0.445031f),
		float2(-0.234004f, 0.870484f), float2(-0.446271f,-0.859268f),
	};
	const uint swNbr[8] =
	{
		0x08609443u, 0x0e218086u, 0x06121407u, 0x082284c0u,
		0x066008e1u, 0x08138c40u, 0x0a220061u, 0x06508082u,
	};
#elif SW_FACE_SAMPLES == 16
	const float2 swDisk[16] =
	{
		float2( 0.176777f, 0.000000f), float2(-0.225772f, 0.206826f),
		float2( 0.034558f,-0.393771f), float2( 0.284571f, 0.371173f),
		float2(-0.522223f,-0.092374f), float2( 0.494695f,-0.314685f),
		float2(-0.165466f, 0.615525f), float2(-0.315561f,-0.607594f),
		float2( 0.684642f, 0.250030f), float2(-0.712256f, 0.294009f),
		float2( 0.343354f,-0.733729f), float2( 0.253730f, 0.808932f),
		float2(-0.764746f,-0.443186f), float2( 0.897134f,-0.197232f),
		float2(-0.547507f, 0.778772f), float2(-0.126487f,-0.976090f),
	};
	const uint swNbr[16] =
	{
		0x0c809443u, 0x04348086u, 0x08f2a807u, 0x0a132d00u,
		0x0023a581u, 0x0681014du, 0x0091adc1u, 0x00a231e2u,
		0x02b281a3u, 0x00c33824u, 0x1a03bc45u, 0x00e0a0c3u,
		0x02f124e4u, 0x04350105u, 0x06458526u, 0x08560947u,
	};
#elif SW_FACE_SAMPLES == 32
	const float2 swDisk[32] =
	{
		float2( 0.125000f, 0.000000f), float2(-0.159645f, 0.146248f),
		float2( 0.024436f,-0.278438f), float2( 0.201222f, 0.262459f),
		float2(-0.369268f,-0.065318f), float2( 0.349802f,-0.222516f),
		float2(-0.117002f, 0.435242f), float2(-0.223136f,-0.429634f),
		float2( 0.484115f, 0.176798f), float2(-0.503641f, 0.207896f),
		float2( 0.242788f,-0.518824f), float2( 0.179414f, 0.572001f),
		float2(-0.540757f,-0.313380f), float2( 0.634370f,-0.139464f),
		float2(-0.387146f, 0.550675f), float2(-0.089440f,-0.690200f),
		float2( 0.549072f, 0.462758f), float2(-0.738878f, 0.030555f),
		float2( 0.538955f,-0.536332f), float2(-0.036058f, 0.779792f),
		float2(-0.512818f,-0.614527f), float2( 0.812360f, 0.109302f),
		float2(-0.688311f, 0.478909f), float2( 0.188086f,-0.836061f),
		float2( 0.435033f, 0.759191f), float2(-0.850448f,-0.271316f),
		float2( 0.826102f,-0.381680f), float2(-0.357888f, 0.855156f),
		float2(-0.319407f,-0.888034f), float2( 0.849909f, 0.446688f),
		float2(-0.944035f, 0.248845f), float2( 0.536596f,-0.834530f),
	};
	const uint swNbr[32] =
	{
		0x0c809443u, 0x04348086u, 0x08f2a807u, 0x20132d00u,
		0x0478a581u, 0x1121014du, 0x1239adc1u, 0x384a31e2u,
		0x0a06d470u, 0x3ce0d891u, 0x3ef15cb2u, 0x11036073u,
		0x1313e684u, 0x01246aa5u, 0x0334db66u, 0x28255f87u,
		0x2a35e3a8u, 0x2cc267c9u, 0x2ed2ebeau, 0x07871b6bu,
		0x09979f8cu, 0x0ba8750du, 0x0db8f92eu, 0x0fc97d4fu,
		0x103ece0bu, 0x13e2522cu, 0x15f2d64du, 0x12bb1a6eu,
		0x14cb9e8fu, 0x06dc22b0u, 0x08eca6d1u, 0x1e5d2af2u,
	};
#else
	#error SoftShadow_FaceCoverageList supports SW_FACE_SAMPLES 8 or 16
#endif
	float3 swBase = swL - swP;
	float3 swSu = swF.u * swR;
	float3 swSv = swF.v * swR;
	float  swCa = cos( swRotAng );		// rotation from the caller (blue noise in the PS, legacy hash in tests)
	float  swSa = sin( swRotAng );

#if SW_FP16_LOOP
	// per-fragment fp16 frame - identical construction to SoftShadow_FaceCoverage (see doc there)
	float  swMD = SoftMaxComp3( swBase ) + SoftMaxComp3( swSu ) + SoftMaxComp3( swSv );
	float  swKD = 1.0f / max( swMD, 1e-19f );
	float3 swU2 = swSu * swCa + swSv * swSa;
	float3 swV2 = swSv * swCa - swSu * swSa;
#endif

	uint swMask = 0u;
	const uint swAll = 0xffffffffu >> ( 32 - SW_FACE_SAMPLES );
#if SW_SCANLINE
	uint swGrid[SW_SCAN_CHORDS];			// Fubini scanline grid: SW_SCAN_CHORDS chords x 32 bits, OR-unioned
	uint swDiskMask[SW_SCAN_CHORDS];		// circular disk mask per chord (fragment-invariant, hoisted)
	int  swDiskBits = 0;
	for( int gi = 0; gi < SW_SCAN_CHORDS; gi++ ) { swGrid[gi] = 0u; swDiskMask[gi] = SoftScan_Run( -SW_SCAN_HC[gi], SW_SCAN_HC[gi] ); swDiskBits += SoftPopcount32( swDiskMask[gi] ); }
#endif
	SW_BKT_DECL
#if SW_LIT_EARLYOUT
	int swMtCnt = 0;					// survivors reaching MT so far (for the lit early-out)
#endif
#if SW_FACE_PROFILE
	float swProbe = 0.0f;		// keeps probed stages live (see the SW_FACE_PROFILE doc at the top)
#endif
#if SW_FACE_SCALAR_LIST
	const int  swBaseU    = WaveReadLaneFirst( swListBase );
	const bool swScalarOK = WaveActiveAllTrue( swBaseU == swListBase );
#endif
	for( int li = 0; li < swListCount; li++ )
	{
		int    se = 0;
		float3 v0, v1, v2;
		float  cd = 0.0f;			// centroid depth along the receiver normal; used by the SW_FACE_PROFILE==2 probe
		bool   swLoaded = false;	// set once a path has fetched v0/v1/v2 (a cull-before-load survivor)
#if SW_CULL_BEFORE_LOAD && !SW_FACE_PROFILE
		// CULL BEFORE LOAD (RUNTIME toggle SW_CBL_RT = r_softShadowCullBeforeLoad): cull from the tile-bin's
		// stored (centroid, tight triRad) - a SEQUENTIAL read - and only scatter-load the 3 verts for
		// survivors. tcen/triRad match the load path's ((v0+v1+v2)/3 and r1.w, fp16 + conservative inflate),
		// so the surviving set (and the term) is bit-identical. When the toggle is OFF the bin SKIPS the
		// parallel write, so this branch must not run: swLoaded stays false and the load-then-cull path
		// below handles the entry. Kept off by default - measured net-negative on the current walk (the
		// vertex gather is L0-cheap, so the deferral saves nothing) but retained as a lever for when a
		// fewer-survivors change shortens the loop and could flip the balance.
		if( SW_CBL_RT )
		{
			{
				uint2  swCullP = t_SoftCull[ swListBase + li ];			// (centroid.xyz, tight triRad) as 4 fp16
				float3 tcen   = float3( f16tof32( swCullP.x & 0xffffu ), f16tof32( swCullP.x >> 16 ), f16tof32( swCullP.y & 0xffffu ) );
				float  triRad = f16tof32( swCullP.y >> 16 );
				float  swQErr = ( max( max( abs( tcen.x ), abs( tcen.y ) ), abs( tcen.z ) ) + triRad ) * ( 1.0f / 1024.0f );
				triRad += swQErr;										// widen for fp16 rounding: looser cull only keeps EXTRA tris (disk reject -> 0), stays bit-identical
				float3 rc     = tcen - swP;
				cd            = dot( rc, swF.nrm );
				SW_ATTRIB_ADD( tightTest, 1 );
#if !SW_SKIP_CULL
				if( cd + triRad < swEps ) { SW_ATTRIB_ADD( tightCull, 1 ); continue; }
				if( cd - triRad > swDistPL ) { SW_ATTRIB_ADD( tightCull, 1 ); continue; }
				float3 perp  = rc - cd * swF.nrm;
				float  coneR = swR * ( cd + triRad ) / swDistPL;
				if( dot( perp, perp ) > ( coneR + triRad ) * ( coneR + triRad ) ) { SW_ATTRIB_ADD( tightCull, 1 ); continue; }
#endif
			}
			se = ( int )t_SoftTiles[ swListBase + li ];				// survivor only: fetch index + verts now
			{
				const int b = swFirstElem + se * 3;
				v0 = t_SoftEdges[ b + 0 ].xyz;
				v1 = t_SoftEdges[ b + 1 ].xyz;
				v2 = t_SoftEdges[ b + 2 ].xyz;
			}
			SW_ATTRIB_ADD( mtTri, 1 );
			swLoaded = true;
		}
#endif
		if( !swLoaded )
		{
#if SW_FACE_SCALAR_LIST
			if( swScalarOK )
			{
				se = ( int )t_SoftTiles[ swBaseU + li ];			// uniform address => scalar load
			}
			else
			{
				se = ( int )t_SoftTiles[ swListBase + li ];
			}
#else
			se = ( int )t_SoftTiles[ swListBase + li ];				// TRIANGLE index (stream v2)
#endif
			const int b = swFirstElem + se * 3;
			float4 r0 = t_SoftEdges[ b + 0 ];						// ( v0.xyz, triRad )
#if SW_FACE_PROFILE == 1
			swProbe += r0.x;
			continue;												// TIMING PROBE: list walk + r0 loads only
#endif
			float4 r1 = t_SoftEdges[ b + 1 ];
			float4 r2 = t_SoftEdges[ b + 2 ];
			v0 = float3( r0.x, r0.y, r0.z );
			v1 = float3( r1.x, r1.y, r1.z );
			v2 = float3( r2.x, r2.y, r2.z );
			// per-FRAGMENT cone/slab reject: the tile cull is the same test at tile grain, so this prunes
			// the tile list down to this fragment's true cone. Identical math to the full walk.
			float3 tcen = ( v0 + v1 + v2 ) * ( 1.0f / 3.0f );
			float3 rc   = tcen - swP;
			cd          = dot( rc, swF.nrm );
			float  triRad = r1.w;	// centroid radius (tight): exact original bound, so the sample-gated set is bit-exact
			SW_ATTRIB_ADD( tightTest, 1 );	// tris in this fragment's tile list reaching the cone cull
#if !SW_SKIP_CULL
			if( cd + triRad < swEps ) { SW_ATTRIB_ADD( tightCull, 1 ); continue; }
			if( cd - triRad > swDistPL ) { SW_ATTRIB_ADD( tightCull, 1 ); continue; }
			float3 perp = rc - cd * swF.nrm;
			float  coneR = swR * ( cd + triRad ) / swDistPL;
			if( dot( perp, perp ) > ( coneR + triRad ) * ( coneR + triRad ) ) { SW_ATTRIB_ADD( tightCull, 1 ); continue; }
#endif
			SW_ATTRIB_ADD( mtTri, 1 );	// survivor: pays the Moller-Trumbore + 16-sample test (the cull-collapse signal)
		}
		SW_BKT_SURV
#if SW_LIT_EARLYOUT
		swMtCnt++;
#endif
#if SW_FACE_PROFILE == 2
		swProbe += cd;
		continue;													// TIMING PROBE: + per-triangle cone culls
#endif
#if SW_SCANLINE
		SoftScan_FillTri( swGrid, v0, v1, v2, swP, swF, swR, swEps );	// Fubini: fill interval bit-runs, union by OR
#if SW_FACE_PROFILE == 3
		swProbe += ( float )swGrid[0];	// TIMING PROBE: walk + cull + SoftScan_FillTri, skip the umbra-check
		continue;
#endif
		{															// UMBRA EARLY-OUT (coverage threshold, not all-bits-exact)
			// Once the OR-union covers ~all of the light disk the fragment is umbra (term ~0), and no
			// remaining occluder can reduce coverage (union is monotone), so stop walking. The old test
			// required EVERY disk bit set, which almost never fires - a sub-bit gap between two projected
			// runs leaves one bit unlit even in deep umbra - so umbra fragments kept walking the whole tile
			// list. A high threshold tolerates the 32-bit-per-chord discretisation. The popcount is free (the
			// term is memory-bound on the gather, so skipping the REMAINING gathers is the win); round the
			// >=99%-occluded fragment to exact umbra (return 1) - the <1% residual is discretisation noise and
			// reads as black regardless, so the visible penumbra gradient is untouched.
			int swCovE = 0;
			SW_UNROLL for( int fm = 0; fm < SW_SCAN_CHORDS; fm++ ) { swCovE += SoftPopcount32( swGrid[fm] & swDiskMask[fm] ); }
			if( swCovE * 100 >= swDiskBits * 99 ) { return 1.0f; }
		}
#else
		float3 edge1 = v1 - v0;
		float3 edge2 = v2 - v0;
		float3 sp = swP - v0;
		float3 qq   = cross( sp, edge1 );
		float  e2qq = dot( edge2, qq );
#if defined( SW_GPU_WALK_COUNTERS ) && SW_GPU_WALK_COUNTERS
		const uint swOldMask = swMask;		// did THIS survivor contribute, or is it tested-but-blocks-nothing?
#endif
#if SW_FP16_LOOP
		swMask = SoftShadow_FaceTriHitsFP16( swMask, edge1, edge2, sp, qq, e2qq,
											 swBase, swU2, swV2, swMD, swKD, swDisk );
#else
		for( int i = 0; i < SW_FACE_SAMPLES; i++ )
		{
			if( ( swMask & ( 1u << i ) ) != 0u ) { continue; }
			float2 s0  = swDisk[i];
			float2 sc  = float2( s0.x * swCa - s0.y * swSa, s0.x * swSa + s0.y * swCa );
			float3 dir = swBase + swSu * sc.x + swSv * sc.y;
			float3 h   = cross( dir, edge2 );
			float  aa  = dot( edge1, h );
			if( abs( aa ) < 1e-12f ) { continue; }
			float  inv = 1.0f / aa;
			float  u   = inv * dot( sp, h );
			if( u < 0.0f || u > 1.0f ) { continue; }
			float  vv  = inv * dot( dir, qq );
			if( vv < 0.0f || u + vv > 1.0f ) { continue; }
			float  tt  = inv * e2qq;
			if( tt > 1e-4f && tt <= 1.0f ) { swMask |= ( 1u << i ); }
		}
#endif	// SW_FP16_LOOP
#if !SW_SCANLINE && defined( SW_GPU_WALK_COUNTERS ) && SW_GPU_WALK_COUNTERS
		if( swMask != swOldMask ) { InterlockedAdd( u_WalkCnt[ 14 ], 1u ); }	// survivor HIT (set >=1 bit)
		else { InterlockedAdd( u_WalkCnt[ 15 ], 1u ); }						// survivor tested, blocks NOTHING
#endif
#endif	// SW_SCANLINE
#if SW_LIT_EARLYOUT
		if( swMask == 0u && swMtCnt >= SW_LIT_EARLYOUT ) { break; }	// still fully unblocked after K survivors -> assume lit
#endif
		if( swMask == swAll ) { break; }
	}
#if SW_SCANLINE
	// Fubini coverage from the OR-unioned interval grid; no morphological crack-close (interval fill leaves
	// no interior sample gaps, unlike point sampling).
	SW_BKT_FLUSH( swMask, swAll );
	int swCov = 0;
	for( int cm = 0; cm < SW_SCAN_CHORDS; cm++ ) { swCov += SoftPopcount32( swGrid[cm] & swDiskMask[cm] ); }
	return swDiskBits > 0 ? ( float )swCov / ( float )swDiskBits : 0.0f;
#else
	if( swMask != 0u && swMask != swAll )
	{
		uint filled = swMask;
		for( int i = 0; i < SW_FACE_SAMPLES; i++ )
		{
			if( ( swMask & ( 1u << i ) ) != 0u ) { continue; }
			uint nb = swNbr[i];
			int blocked = 0;
			for( int k = 0; k < 6; k++ )
			{
				int j = ( int )( ( nb >> ( 5 * k ) ) & 31u );
				if( ( swMask & ( 1u << j ) ) != 0u ) { blocked++; }
			}
			if( blocked >= 5 ) { filled |= ( 1u << i ); }
		}
		swMask = filled;
	}
#if SW_FACE_PROFILE
	if( swProbe > 1e30f ) { swMask = swAll; }	// never true; makes the probe accumulator observable
#endif
	SW_BKT_FLUSH( swMask, swAll );
	return ( float )SoftPopcount32( swMask ) / ( float )SW_FACE_SAMPLES;
#endif	// SW_SCANLINE
}

#if SW_SURF_CACHE && !SW_SURF_GRID
// SURFACE-FOLD CACHE probe (r_softShadowSurfCache): the EXACT part of a cached fragment's term.
// EXCLUDED in GRID mode: the grid hit path ORs a Fubini dynamic grid instead of walking the residual
// pool, so t_SurfPool is unbound in grid mode (its register t6 is reused for the static grid buffer).
// Walks the texel's RESIDUAL static occluders (triangle indices from the persistent pool t_SurfPool,
// declared by the term CS before this include) plus the frame's DYNAMIC casters (the caster-table
// suffix after the static prefix) into ONE shared sample mask (union), then the shipped crack-close.
// Per-fragment rotation, so the exact part keeps the shipped dither. The FOLDED static part arrives
// as the bilinear of the texel's 4 corner F values; the caller adds the two and saturates. Mirrors
// SoftShadow_FaceCoverageList / SoftShadow_FaceCoverage in their SHIPPED config (in-loop dirs, fp16
// MT) - duplication is house style here (the two sibling walks already duplicate; tests hold them).
SW_FUNC float SoftShadow_FaceCoverageSurfResidual( float3 swP, float3 swL, float swR, int swTriBase,
		int swResBase, int swResCount, int swCasterBase, int swDynFirst, int swCasterCount,
		int swListBase, int swListCount, int swDynFirstTri, float swRotAng )
{
	swR = max( swR, 1e-2f );
	softFrame_t swF = SoftShadow_Frame( swP, swL );
	float  swDistPL = swF.distPL;
	float  swSinA = saturate( swR / swDistPL );
	float  swCosA = sqrt( 1.0f - swSinA * swSinA );
	const float swEps = SW_NEAR_EPS;
#if SW_FACE_SAMPLES == 8
	const float2 swDisk[8] =
	{
		float2( 0.250000f, 0.000000f), float2(-0.319290f, 0.292496f),
		float2( 0.048872f,-0.556877f), float2( 0.402444f, 0.524918f),
		float2(-0.738535f,-0.130636f), float2( 0.699605f,-0.445031f),
		float2(-0.234004f, 0.870484f), float2(-0.446271f,-0.859268f),
	};
	const uint swNbr[8] =
	{
		0x08609443u, 0x0e218086u, 0x06121407u, 0x082284c0u,
		0x066008e1u, 0x08138c40u, 0x0a220061u, 0x06508082u,
	};
#elif SW_FACE_SAMPLES == 16
	const float2 swDisk[16] =
	{
		float2( 0.176777f, 0.000000f), float2(-0.225772f, 0.206826f),
		float2( 0.034558f,-0.393771f), float2( 0.284571f, 0.371173f),
		float2(-0.522223f,-0.092374f), float2( 0.494695f,-0.314685f),
		float2(-0.165466f, 0.615525f), float2(-0.315561f,-0.607594f),
		float2( 0.684642f, 0.250030f), float2(-0.712256f, 0.294009f),
		float2( 0.343354f,-0.733729f), float2( 0.253730f, 0.808932f),
		float2(-0.764746f,-0.443186f), float2( 0.897134f,-0.197232f),
		float2(-0.547507f, 0.778772f), float2(-0.126487f,-0.976090f),
	};
	const uint swNbr[16] =
	{
		0x0c809443u, 0x04348086u, 0x08f2a807u, 0x0a132d00u,
		0x0023a581u, 0x0681014du, 0x0091adc1u, 0x00a231e2u,
		0x02b281a3u, 0x00c33824u, 0x1a03bc45u, 0x00e0a0c3u,
		0x02f124e4u, 0x04350105u, 0x06458526u, 0x08560947u,
	};
#elif SW_FACE_SAMPLES == 32
	const float2 swDisk[32] =
	{
		float2( 0.125000f, 0.000000f), float2(-0.159645f, 0.146248f),
		float2( 0.024436f,-0.278438f), float2( 0.201222f, 0.262459f),
		float2(-0.369268f,-0.065318f), float2( 0.349802f,-0.222516f),
		float2(-0.117002f, 0.435242f), float2(-0.223136f,-0.429634f),
		float2( 0.484115f, 0.176798f), float2(-0.503641f, 0.207896f),
		float2( 0.242788f,-0.518824f), float2( 0.179414f, 0.572001f),
		float2(-0.540757f,-0.313380f), float2( 0.634370f,-0.139464f),
		float2(-0.387146f, 0.550675f), float2(-0.089440f,-0.690200f),
		float2( 0.549072f, 0.462758f), float2(-0.738878f, 0.030555f),
		float2( 0.538955f,-0.536332f), float2(-0.036058f, 0.779792f),
		float2(-0.512818f,-0.614527f), float2( 0.812360f, 0.109302f),
		float2(-0.688311f, 0.478909f), float2( 0.188086f,-0.836061f),
		float2( 0.435033f, 0.759191f), float2(-0.850448f,-0.271316f),
		float2( 0.826102f,-0.381680f), float2(-0.357888f, 0.855156f),
		float2(-0.319407f,-0.888034f), float2( 0.849909f, 0.446688f),
		float2(-0.944035f, 0.248845f), float2( 0.536596f,-0.834530f),
	};
	const uint swNbr[32] =
	{
		0x0c809443u, 0x04348086u, 0x08f2a807u, 0x20132d00u,
		0x0478a581u, 0x1121014du, 0x1239adc1u, 0x384a31e2u,
		0x0a06d470u, 0x3ce0d891u, 0x3ef15cb2u, 0x11036073u,
		0x1313e684u, 0x01246aa5u, 0x0334db66u, 0x28255f87u,
		0x2a35e3a8u, 0x2cc267c9u, 0x2ed2ebeau, 0x07871b6bu,
		0x09979f8cu, 0x0ba8750du, 0x0db8f92eu, 0x0fc97d4fu,
		0x103ece0bu, 0x13e2522cu, 0x15f2d64du, 0x12bb1a6eu,
		0x14cb9e8fu, 0x06dc22b0u, 0x08eca6d1u, 0x1e5d2af2u,
	};
#else
	#error SoftShadow_FaceCoverageSurfResidual supports SW_FACE_SAMPLES 8 or 16
#endif
	float3 swBase = swL - swP;
	float3 swSu = swF.u * swR;
	float3 swSv = swF.v * swR;
	float  swCa = cos( swRotAng );
	float  swSa = sin( swRotAng );

#if SW_FP16_LOOP
	float  swMD = SoftMaxComp3( swBase ) + SoftMaxComp3( swSu ) + SoftMaxComp3( swSv );
	float  swKD = 1.0f / max( swMD, 1e-19f );
	float3 swU2 = swSu * swCa + swSv * swSa;
	float3 swV2 = swSv * swCa - swSu * swSa;
#endif

	uint swMask = 0u;
	const uint swAll = 0xffffffffu >> ( 32 - SW_FACE_SAMPLES );
	// ---- residual static occluders: SELF-CONTAINED verts in the pool (12 uints/tri), per-fragment cone
	// cull + MT (List's body). No static-stream index: a warm texel is independent of the emitted prefix.
	for( int li = 0; li < swResCount; li++ )
	{
		const int po = swResBase + li * 12;
		float4 r0 = float4( asfloat( t_SurfPool[ po + 0 ] ), asfloat( t_SurfPool[ po + 1 ] ), asfloat( t_SurfPool[ po + 2 ] ), asfloat( t_SurfPool[ po + 3 ] ) );
		float4 r1 = float4( asfloat( t_SurfPool[ po + 4 ] ), asfloat( t_SurfPool[ po + 5 ] ), asfloat( t_SurfPool[ po + 6 ] ), asfloat( t_SurfPool[ po + 7 ] ) );
		float4 r2 = float4( asfloat( t_SurfPool[ po + 8 ] ), asfloat( t_SurfPool[ po + 9 ] ), asfloat( t_SurfPool[ po + 10 ] ), asfloat( t_SurfPool[ po + 11 ] ) );
		float3 v0 = float3( r0.x, r0.y, r0.z );
		float3 v1 = float3( r1.x, r1.y, r1.z );
		float3 v2 = float3( r2.x, r2.y, r2.z );
		float3 tcen = ( v0 + v1 + v2 ) * ( 1.0f / 3.0f );
		float3 rc   = tcen - swP;
		float  cd   = dot( rc, swF.nrm );
		float  triRad = r1.w;
		if( cd + triRad < swEps ) { continue; }
		if( cd - triRad > swDistPL ) { continue; }
		float3 perp = rc - cd * swF.nrm;
		float  coneR = swR * ( cd + triRad ) / swDistPL;
		if( dot( perp, perp ) > ( coneR + triRad ) * ( coneR + triRad ) ) { continue; }
		float3 edge1 = v1 - v0;
		float3 edge2 = v2 - v0;
		float3 sp = swP - v0;
		float3 qq   = cross( sp, edge1 );
		float  e2qq = dot( edge2, qq );
#if SW_FP16_LOOP
		swMask = SoftShadow_FaceTriHitsFP16( swMask, edge1, edge2, sp, qq, e2qq,
											 swBase, swU2, swV2, swMD, swKD, swDisk );
#else
		for( int i = 0; i < SW_FACE_SAMPLES; i++ )
		{
			if( ( swMask & ( 1u << i ) ) != 0u ) { continue; }
			float2 s0  = swDisk[i];
			float2 sc  = float2( s0.x * swCa - s0.y * swSa, s0.x * swSa + s0.y * swCa );
			float3 dir = swBase + swSu * sc.x + swSv * sc.y;
			float3 h   = cross( dir, edge2 );
			float  aa  = dot( edge1, h );
			if( abs( aa ) < 1e-12f ) { continue; }
			float  inv = 1.0f / aa;
			float  u   = inv * dot( sp, h );
			if( u < 0.0f || u > 1.0f ) { continue; }
			float  vv  = inv * dot( dir, qq );
			if( vv < 0.0f || u + vv > 1.0f ) { continue; }
			float  tt  = inv * e2qq;
			if( tt > 1e-4f && tt <= 1.0f ) { swMask |= ( 1u << i ); }
		}
#endif	// SW_FP16_LOOP
		if( swMask == swAll ) { break; }
	}
	// ---- dynamic casters ----
	// TILED (swListCount >= 0): walk THIS fragment's tile list, DYNAMIC tris only (se >= swDynFirstTri;
	// static tris are folded into F or sit in the residual pool, handled above). Measured 2026-08-22: the
	// old untiled all-caster loop (the else branch) cost a cache HIT more than a tiled MISS when dynamic
	// casters are many (the RoE intro cinematic) - it re-walked the whole dynamic suffix per fragment.
	// Fallback (swListCount < 0: spill/corrupt/untiled tile) keeps the exact all-caster walk.
	if( swMask != swAll && swListCount >= 0 )
	{
		for( int ld = 0; ld < swListCount; ld++ )
		{
			const int se = ( int )t_SoftTiles[ swListBase + ld ];
			if( se < swDynFirstTri ) { continue; }			// static tri: in F or the residual pool, not here
			const int bb = swTriBase + se * 3;
			float4 r0 = t_SoftEdges[ bb + 0 ];
			{
				float3 rc0 = float3( r0.x, r0.y, r0.z ) - swP;
				float  cd0 = dot( rc0, swF.nrm );
				float  vr0 = r0.w;
				if( cd0 + vr0 < swEps ) { continue; }
				if( cd0 - vr0 > swDistPL ) { continue; }
				float3 pp0 = rc0 - cd0 * swF.nrm;
				float  cr0 = swR * ( cd0 + vr0 ) / swDistPL;
				if( dot( pp0, pp0 ) > ( cr0 + vr0 ) * ( cr0 + vr0 ) ) { continue; }
			}
			float4 r1 = t_SoftEdges[ bb + 1 ];
			float4 r2 = t_SoftEdges[ bb + 2 ];
			float3 v0 = float3( r0.x, r0.y, r0.z );
			float3 v1 = float3( r1.x, r1.y, r1.z );
			float3 v2 = float3( r2.x, r2.y, r2.z );
			float3 tcen = ( v0 + v1 + v2 ) * ( 1.0f / 3.0f );
			float3 rc   = tcen - swP;
			float  cd   = dot( rc, swF.nrm );
			float  triRad = r1.w;
			if( cd + triRad < swEps ) { continue; }
			if( cd - triRad > swDistPL ) { continue; }
			float3 perp = rc - cd * swF.nrm;
			float  coneR = swR * ( cd + triRad ) / swDistPL;
			if( dot( perp, perp ) > ( coneR + triRad ) * ( coneR + triRad ) ) { continue; }
			float3 edge1 = v1 - v0;
			float3 edge2 = v2 - v0;
			float3 sp = swP - v0;
			float3 qq   = cross( sp, edge1 );
			float  e2qq = dot( edge2, qq );
#if SW_FP16_LOOP
			swMask = SoftShadow_FaceTriHitsFP16( swMask, edge1, edge2, sp, qq, e2qq,
												 swBase, swU2, swV2, swMD, swKD, swDisk );
#else
			for( int i = 0; i < SW_FACE_SAMPLES; i++ )
			{
				if( ( swMask & ( 1u << i ) ) != 0u ) { continue; }
				float2 s0  = swDisk[i];
				float2 sc  = float2( s0.x * swCa - s0.y * swSa, s0.x * swSa + s0.y * swCa );
				float3 dir = swBase + swSu * sc.x + swSv * sc.y;
				float3 h   = cross( dir, edge2 );
				float  aa  = dot( edge1, h );
				if( abs( aa ) < 1e-12f ) { continue; }
				float  inv = 1.0f / aa;
				float  u   = inv * dot( sp, h );
				if( u < 0.0f || u > 1.0f ) { continue; }
				float  vv  = inv * dot( dir, qq );
				if( vv < 0.0f || u + vv > 1.0f ) { continue; }
				float  tt  = inv * e2qq;
				if( tt > 1e-4f && tt <= 1.0f ) { swMask |= ( 1u << i ); }
			}
#endif	// SW_FP16_LOOP
			if( swMask == swAll ) { break; }
		}
	}
	else if( swMask != swAll )
	{
		for( int scc = swDynFirst; scc < swCasterCount; scc++ )
		{
			float4 c0 = t_SoftEdges[ swCasterBase + scc * 2 + 0 ];
			float4 c1 = t_SoftEdges[ swCasterBase + scc * 2 + 1 ];
			float3 dCv = float3( c0.x, c0.y, c0.z ) - swP;
			if( SoftShadow_CullCaster( dCv, c0.w, swF, swSinA, swCosA, swEps ) )
			{
				continue;
			}
			const int swTriFirst = ( int )c1.x;
			const int swTriEnd   = swTriFirst + ( int )c1.y;
			for( int t = swTriFirst; t < swTriEnd; t++ )
			{
				const int b = swTriBase + t * 3;
				float4 r0 = t_SoftEdges[ b + 0 ];
				{
					float3 rc0 = float3( r0.x, r0.y, r0.z ) - swP;
					float  cd0 = dot( rc0, swF.nrm );
					float  vr0 = r0.w;
					if( cd0 + vr0 < swEps ) { continue; }
					if( cd0 - vr0 > swDistPL ) { continue; }
					float3 pp0 = rc0 - cd0 * swF.nrm;
					float  cr0 = swR * ( cd0 + vr0 ) / swDistPL;
					if( dot( pp0, pp0 ) > ( cr0 + vr0 ) * ( cr0 + vr0 ) ) { continue; }
				}
				float4 r1 = t_SoftEdges[ b + 1 ];
				float4 r2 = t_SoftEdges[ b + 2 ];
				float3 v0 = float3( r0.x, r0.y, r0.z );
				float3 v1 = float3( r1.x, r1.y, r1.z );
				float3 v2 = float3( r2.x, r2.y, r2.z );
				float3 tcen = ( v0 + v1 + v2 ) * ( 1.0f / 3.0f );
				float3 rc   = tcen - swP;
				float  cd   = dot( rc, swF.nrm );
				float  triRad = r1.w;
				if( cd + triRad < swEps ) { continue; }
				if( cd - triRad > swDistPL ) { continue; }
				float3 perp = rc - cd * swF.nrm;
				float  coneR = swR * ( cd + triRad ) / swDistPL;
				if( dot( perp, perp ) > ( coneR + triRad ) * ( coneR + triRad ) ) { continue; }
				float3 edge1 = v1 - v0;
				float3 edge2 = v2 - v0;
				float3 sp = swP - v0;
				float3 qq   = cross( sp, edge1 );
				float  e2qq = dot( edge2, qq );
#if SW_FP16_LOOP
				swMask = SoftShadow_FaceTriHitsFP16( swMask, edge1, edge2, sp, qq, e2qq,
													 swBase, swU2, swV2, swMD, swKD, swDisk );
#else
				for( int i = 0; i < SW_FACE_SAMPLES; i++ )
				{
					if( ( swMask & ( 1u << i ) ) != 0u ) { continue; }
					float2 s0  = swDisk[i];
					float2 sc  = float2( s0.x * swCa - s0.y * swSa, s0.x * swSa + s0.y * swCa );
					float3 dir = swBase + swSu * sc.x + swSv * sc.y;
					float3 h   = cross( dir, edge2 );
					float  aa  = dot( edge1, h );
					if( abs( aa ) < 1e-12f ) { continue; }
					float  inv = 1.0f / aa;
					float  u   = inv * dot( sp, h );
					if( u < 0.0f || u > 1.0f ) { continue; }
					float  vv  = inv * dot( dir, qq );
					if( vv < 0.0f || u + vv > 1.0f ) { continue; }
					float  tt  = inv * e2qq;
					if( tt > 1e-4f && tt <= 1.0f ) { swMask |= ( 1u << i ); }
				}
#endif	// SW_FP16_LOOP
				if( swMask == swAll ) { break; }
			}
			if( swMask == swAll ) { break; }
		}
	}
	// shipped morphological crack-close on the EXACT mask only; the folded part is a scalar and gets
	// none (a playtest observation point - see the probe plan)
	if( swMask != 0u && swMask != swAll )
	{
		uint filled = swMask;
		for( int i = 0; i < SW_FACE_SAMPLES; i++ )
		{
			if( ( swMask & ( 1u << i ) ) != 0u ) { continue; }
			uint nb = swNbr[i];
			int blocked = 0;
			for( int k = 0; k < 6; k++ )
			{
				int j = ( int )( ( nb >> ( 5 * k ) ) & 31u );
				if( ( swMask & ( 1u << j ) ) != 0u ) { blocked++; }
			}
			if( blocked >= 5 ) { filled |= ( 1u << i ); }
		}
		swMask = filled;
	}
	return ( float )SoftPopcount32( swMask ) / ( float )SW_FACE_SAMPLES;
}
#endif	// SW_SURF_CACHE

// SPILL-tile walk (STREAM V3 clusters): the list entries are ABSOLUTE float4 element offsets of
// CLUSTER records - k-d spatial leaves of <=32 triangles built at stream time, 2 float4 each:
// ( centre.xyz, radius )( firstTri, numTris, 0, 0 ). One 16B sphere cull rejects a whole span (the
// measured ~3.6x cone-test amortization on dense receivers, tests/SoftShadowClusterProbe_test.cpp),
// which is exactly what the OVERFLOWED tiles need: their tri lists exceed SW_TILE_K, and walking
// thousands of triangles per fragment was the measured ~12 ms residual. Normal (fitting) tiles keep
// the flat tri-index walk above - at moderate density the cluster indirection measured NET WORSE
// (wave-union keeps most clusters, so the overhead has nothing to amortize; erebus1_05 +3.7 ms with
// all-cluster lists) - so clusters serve ONLY the spill. Conservative at every level: a kept
// cluster's triangles still run the coarse v0 + tight centroid culls + sample test below, so the
// sample-gated triangle set - and the coverage - is BIT-EXACT vs the full walk.
// Setup/tri-test/close duplicated from SoftShadow_FaceCoverageList per the house pattern above.
SW_FUNC float SoftShadow_FaceCoverageClusterList( float3 swP, float3 swL, float swR, int swFirstElem,
		int swListBase, int swListCount, float swRotAng SW_TILEBUF_PARAM SW_EDGEBUF_PARAM )
{
	swR = max( swR, 1e-2f );
	softFrame_t swF = SoftShadow_Frame( swP, swL );
	float  swDistPL = swF.distPL;
	const float swEps = SW_NEAR_EPS;
#if SW_FACE_SAMPLES == 8
	const float2 swDisk[8] =
	{
		float2( 0.250000f, 0.000000f), float2(-0.319290f, 0.292496f),
		float2( 0.048872f,-0.556877f), float2( 0.402444f, 0.524918f),
		float2(-0.738535f,-0.130636f), float2( 0.699605f,-0.445031f),
		float2(-0.234004f, 0.870484f), float2(-0.446271f,-0.859268f),
	};
	const uint swNbr[8] =
	{
		0x08609443u, 0x0e218086u, 0x06121407u, 0x082284c0u,
		0x066008e1u, 0x08138c40u, 0x0a220061u, 0x06508082u,
	};
#elif SW_FACE_SAMPLES == 16
	const float2 swDisk[16] =
	{
		float2( 0.176777f, 0.000000f), float2(-0.225772f, 0.206826f),
		float2( 0.034558f,-0.393771f), float2( 0.284571f, 0.371173f),
		float2(-0.522223f,-0.092374f), float2( 0.494695f,-0.314685f),
		float2(-0.165466f, 0.615525f), float2(-0.315561f,-0.607594f),
		float2( 0.684642f, 0.250030f), float2(-0.712256f, 0.294009f),
		float2( 0.343354f,-0.733729f), float2( 0.253730f, 0.808932f),
		float2(-0.764746f,-0.443186f), float2( 0.897134f,-0.197232f),
		float2(-0.547507f, 0.778772f), float2(-0.126487f,-0.976090f),
	};
	const uint swNbr[16] =
	{
		0x0c809443u, 0x04348086u, 0x08f2a807u, 0x0a132d00u,
		0x0023a581u, 0x0681014du, 0x0091adc1u, 0x00a231e2u,
		0x02b281a3u, 0x00c33824u, 0x1a03bc45u, 0x00e0a0c3u,
		0x02f124e4u, 0x04350105u, 0x06458526u, 0x08560947u,
	};
#elif SW_FACE_SAMPLES == 32
	const float2 swDisk[32] =
	{
		float2( 0.125000f, 0.000000f), float2(-0.159645f, 0.146248f),
		float2( 0.024436f,-0.278438f), float2( 0.201222f, 0.262459f),
		float2(-0.369268f,-0.065318f), float2( 0.349802f,-0.222516f),
		float2(-0.117002f, 0.435242f), float2(-0.223136f,-0.429634f),
		float2( 0.484115f, 0.176798f), float2(-0.503641f, 0.207896f),
		float2( 0.242788f,-0.518824f), float2( 0.179414f, 0.572001f),
		float2(-0.540757f,-0.313380f), float2( 0.634370f,-0.139464f),
		float2(-0.387146f, 0.550675f), float2(-0.089440f,-0.690200f),
		float2( 0.549072f, 0.462758f), float2(-0.738878f, 0.030555f),
		float2( 0.538955f,-0.536332f), float2(-0.036058f, 0.779792f),
		float2(-0.512818f,-0.614527f), float2( 0.812360f, 0.109302f),
		float2(-0.688311f, 0.478909f), float2( 0.188086f,-0.836061f),
		float2( 0.435033f, 0.759191f), float2(-0.850448f,-0.271316f),
		float2( 0.826102f,-0.381680f), float2(-0.357888f, 0.855156f),
		float2(-0.319407f,-0.888034f), float2( 0.849909f, 0.446688f),
		float2(-0.944035f, 0.248845f), float2( 0.536596f,-0.834530f),
	};
	const uint swNbr[32] =
	{
		0x0c809443u, 0x04348086u, 0x08f2a807u, 0x20132d00u,
		0x0478a581u, 0x1121014du, 0x1239adc1u, 0x384a31e2u,
		0x0a06d470u, 0x3ce0d891u, 0x3ef15cb2u, 0x11036073u,
		0x1313e684u, 0x01246aa5u, 0x0334db66u, 0x28255f87u,
		0x2a35e3a8u, 0x2cc267c9u, 0x2ed2ebeau, 0x07871b6bu,
		0x09979f8cu, 0x0ba8750du, 0x0db8f92eu, 0x0fc97d4fu,
		0x103ece0bu, 0x13e2522cu, 0x15f2d64du, 0x12bb1a6eu,
		0x14cb9e8fu, 0x06dc22b0u, 0x08eca6d1u, 0x1e5d2af2u,
	};
#else
	#error SoftShadow_FaceCoverageClusterList supports SW_FACE_SAMPLES 8 or 16
#endif
	float3 swBase = swL - swP;
	float3 swSu = swF.u * swR;
	float3 swSv = swF.v * swR;
	float  swCa = cos( swRotAng );
	float  swSa = sin( swRotAng );

#if SW_FP16_LOOP
	float  swMD = SoftMaxComp3( swBase ) + SoftMaxComp3( swSu ) + SoftMaxComp3( swSv );
	float  swKD = 1.0f / max( swMD, 1e-19f );
	float3 swU2 = swSu * swCa + swSv * swSa;
	float3 swV2 = swSv * swCa - swSu * swSa;
#endif

	uint swMask = 0u;
	const uint swAll = 0xffffffffu >> ( 32 - SW_FACE_SAMPLES );
#if SW_SCANLINE
	// Fubini scanline port for the SPILL/cluster path - same exact 1D-interval union as the tile-list
	// walker (SW_SCANLINE there). Before this, spill fragments stayed on 16-sample coverage while
	// listed fragments ran the scanline: two algorithms in one frame (and the contributor cache's
	// scanline serve measured 15.7% term mismatches against the sampled spill reference).
	uint swGrid[SW_SCAN_CHORDS];
	uint swDiskMask[SW_SCAN_CHORDS];
	int  swDiskBits = 0;
	for( int gi = 0; gi < SW_SCAN_CHORDS; gi++ ) { swGrid[gi] = 0u; swDiskMask[gi] = SoftScan_Run( -SW_SCAN_HC[gi], SW_SCAN_HC[gi] ); swDiskBits += SoftPopcount32( swDiskMask[gi] ); }
#endif
	SW_BKT_DECL
	for( int li = 0; li < swListCount; li++ )
	{
		const int se = ( int )t_SoftTiles[ swListBase + li ];		// ABSOLUTE cluster-record offset
		float4 q0 = t_SoftEdges[ se + 0 ];							// ( centre.xyz, radius )
		{
			float3 crc  = float3( q0.x, q0.y, q0.z ) - swP;
			float  ccd  = dot( crc, swF.nrm );
			float  crad = q0.w;
			if( ccd + crad < swEps ) { continue; }
			if( ccd - crad > swDistPL ) { continue; }
			float3 cperp = crc - ccd * swF.nrm;
			float  cconeR = swR * ( ccd + crad ) / swDistPL;
			if( dot( cperp, cperp ) > ( cconeR + crad ) * ( cconeR + crad ) ) { continue; }
		}
		float4 q1 = t_SoftEdges[ se + 1 ];							// ( firstTri, numTris, 0, 0 ) - survivors only
		const int swTriFirst = ( int )q1.x;
		const int swTriEnd   = swTriFirst + ( int )q1.y;
		for( int t = swTriFirst; t < swTriEnd; t++ )
		{
			const int b = swFirstElem + t * 3;
			float4 r0 = t_SoftEdges[ b + 0 ];						// ( v0.xyz, v0-radius ) - coarse reject reads ONLY this
			{
				float3 rc0 = float3( r0.x, r0.y, r0.z ) - swP;
				float  cd0 = dot( rc0, swF.nrm );
				float  vr0 = r0.w;
				if( cd0 + vr0 < swEps ) { continue; }
				if( cd0 - vr0 > swDistPL ) { continue; }
				float3 pp0 = rc0 - cd0 * swF.nrm;
				float  cr0 = swR * ( cd0 + vr0 ) / swDistPL;
				if( dot( pp0, pp0 ) > ( cr0 + vr0 ) * ( cr0 + vr0 ) ) { continue; }
			}
			float4 r1 = t_SoftEdges[ b + 1 ];
			float4 r2 = t_SoftEdges[ b + 2 ];
			float3 v0 = float3( r0.x, r0.y, r0.z );
			float3 v1 = float3( r1.x, r1.y, r1.z );
			float3 v2 = float3( r2.x, r2.y, r2.z );
			float3 tcen = ( v0 + v1 + v2 ) * ( 1.0f / 3.0f );
			float3 rc   = tcen - swP;
			float  cd   = dot( rc, swF.nrm );
			float  triRad = r1.w;	// centroid radius (tight): exact original bound => bit-exact gate
			if( cd + triRad < swEps ) { continue; }
			if( cd - triRad > swDistPL ) { continue; }
			float3 perp = rc - cd * swF.nrm;
			float  coneR = swR * ( cd + triRad ) / swDistPL;
			if( dot( perp, perp ) > ( coneR + triRad ) * ( coneR + triRad ) ) { continue; }
			SW_BKT_SURV
#if SW_SCANLINE
			SoftScan_FillTri( swGrid, v0, v1, v2, swP, swF, swR, swEps );	// Fubini interval union
			{
				// UMBRA EARLY-OUT + rounding, identical to the tile-list scanline walker
				int swCovE = 0;
				SW_UNROLL for( int fm = 0; fm < SW_SCAN_CHORDS; fm++ ) { swCovE += SoftPopcount32( swGrid[fm] & swDiskMask[fm] ); }
				if( swCovE * 100 >= swDiskBits * 99 ) { return 1.0f; }
			}
#else
			float3 edge1 = v1 - v0;
			float3 edge2 = v2 - v0;
			float3 sp = swP - v0;
			float3 qq   = cross( sp, edge1 );
			float  e2qq = dot( edge2, qq );
#if SW_FP16_LOOP
			swMask = SoftShadow_FaceTriHitsFP16( swMask, edge1, edge2, sp, qq, e2qq,
												 swBase, swU2, swV2, swMD, swKD, swDisk );
#else
			for( int i = 0; i < SW_FACE_SAMPLES; i++ )
			{
				if( ( swMask & ( 1u << i ) ) != 0u ) { continue; }
				float2 s0  = swDisk[i];
				float2 sc  = float2( s0.x * swCa - s0.y * swSa, s0.x * swSa + s0.y * swCa );
				float3 dir = swBase + swSu * sc.x + swSv * sc.y;
				float3 h   = cross( dir, edge2 );
				float  aa  = dot( edge1, h );
				if( abs( aa ) < 1e-12f ) { continue; }
				float  inv = 1.0f / aa;
				float  u   = inv * dot( sp, h );
				if( u < 0.0f || u > 1.0f ) { continue; }
				float  vv  = inv * dot( dir, qq );
				if( vv < 0.0f || u + vv > 1.0f ) { continue; }
				float  tt  = inv * e2qq;
				if( tt > 1e-4f && tt <= 1.0f ) { swMask |= ( 1u << i ); }
			}
#endif	// SW_FP16_LOOP
			if( swMask == swAll ) { break; }
#endif	// SW_SCANLINE
		}
#if !SW_SCANLINE
		if( swMask == swAll ) { break; }
#endif
	}
#if SW_SCANLINE
	int swCovC = 0;
	for( int cmc = 0; cmc < SW_SCAN_CHORDS; cmc++ ) { swCovC += SoftPopcount32( swGrid[cmc] & swDiskMask[cmc] ); }
	SW_BKT_FLUSH( swMask, swAll );
	return swDiskBits > 0 ? ( float )swCovC / ( float )swDiskBits : 0.0f;
#endif
	if( swMask != 0u && swMask != swAll )
	{
		uint filled = swMask;
		for( int i = 0; i < SW_FACE_SAMPLES; i++ )
		{
			if( ( swMask & ( 1u << i ) ) != 0u ) { continue; }
			uint nb = swNbr[i];
			int blocked = 0;
			for( int k = 0; k < 6; k++ )
			{
				int j = ( int )( ( nb >> ( 5 * k ) ) & 31u );
				if( ( swMask & ( 1u << j ) ) != 0u ) { blocked++; }
			}
			if( blocked >= 5 ) { filled |= ( 1u << i ); }
		}
		swMask = filled;
	}
	SW_BKT_FLUSH( swMask, swAll );
	return ( float )SoftPopcount32( swMask ) / ( float )SW_FACE_SAMPLES;
}

// Dispatcher: the pixel shader picks the coverage path by the SIGN of the count (rpJitterTexScale.z):
// negative = FRONT-FACE stream v2 (r_softShadowFaceCoverage; abs = CASTER count, swFirstElem = tri stream
// base, swCasterBase = caster table base), positive = light-silhouette edge stream (abs = record count;
// swCasterBase ignored). The face path needs no centre-lit winding guard (coverage is a bounded union),
// so swCentreLit is ignored there. Callers pass abs(swN).
SW_FUNC float SoftShadow_Coverage( float3 swP, float3 swL, float swR, int swFirstElem, int swCasterBase, int swN, float swCentreLit, bool swFace, float swRotAng SW_EDGEBUF_PARAM )
{
#ifdef __cplusplus
	return swFace ? SoftShadow_FaceCoverage( swP, swL, swR, swFirstElem, swCasterBase, swN, swRotAng, t_SoftEdges )
		   : SoftShadow_WedgeOcclusion( swP, swL, swR, swFirstElem, swN, swCentreLit, t_SoftEdges );
#else
	return swFace ? SoftShadow_FaceCoverage( swP, swL, swR, swFirstElem, swCasterBase, swN, swRotAng )
		   : SoftShadow_WedgeOcclusion( swP, swL, swR, swFirstElem, swN, swCentreLit );
#endif
}

#endif // __SOFTWEDGE_COVERAGE_INC__
