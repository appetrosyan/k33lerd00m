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

// SW_FACE_LEGACY: 0 = the four LOSSLESS face-coverage hoists (precompute triRad from the stream, hoist the
// per-sample ray dirs and the triangle-constant qq/e2qq out of the loop, skip the crack-close on trivial masks);
// 1 = recompute everything in-loop as before. Bit-EXACT either way (same values); the toggle exists only to A/B
// the perf of the hoists (the shipped default is 0). See SoftShadowBench.face_vs_wedge_throughput.
#ifndef SW_FACE_LEGACY
	#define SW_FACE_LEGACY 0
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
#else
	#define SW_EDGEBUF_PARAM
	#define SW_TILEBUF_PARAM
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
// Stream layout reuses the edge records (softShadowEdge_t, 2 float4) so the whole flatten/cache/bind path
// is shared: a header (e0.w < 0) carries the caster centre in e0.xyz and (bounding radius, recordCount) in
// e1.xy; each triangle is TWO consecutive records - record A = ( v0.xyz, - )( v1.xyz, hdr ), record B =
// ( v1.xyz, - )( v2.xyz, hdr ) - so v0,v1 come from A and v2 from B.e1. Casters are cull-tested (shared with
// the edge path); the sample mask is global so casters union for free.

// portable 32-bit popcount (HLSL has countbits, but the C++ test build via hlsl_compat.h does not)
SW_FUNC int SoftPopcount32( uint x )
{
	x = x - ( ( x >> 1 ) & 0x55555555u );
	x = ( x & 0x33333333u ) + ( ( x >> 2 ) & 0x33333333u );
	x = ( x + ( x >> 4 ) ) & 0x0f0f0f0fu;
	return ( int )( ( x * 0x01010101u ) >> 24 );
}

#ifndef SW_FACE_SAMPLES
	#define SW_FACE_SAMPLES 16			// equal-area disk samples (8, 16 or 32); <=32 to pack the occlusion mask in one uint.
	//									   8 = perf tier, GATE-REJECTED as the default (2026-08-16): 2 EXTENT
	//									   defects (erebus1_10/11 - the 1/8 coverage quantum shrinks the penumbra
	//									   body past the 10% tolerance) for only ~12% frame time - the integral is
	//									   WALK-bound, not sample-bound. 16 = the accuracy set (shipped).
#endif


SW_FUNC float SoftShadow_FaceCoverage( float3 swP, float3 swL, float swR, int swFirstElem, int swN SW_EDGEBUF_PARAM )
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
	// neighbouring pixels into a visible fixed-pattern bias (measured worst meanAbs 0.099 on erebus5/6);
	// rotating by a hash of swP decorrelates neighbours so it averages out spatially - the CPU analogue of
	// the jittered rays the RT reference (and the shipped TAA path) already use. Stable in swP => no flicker.
	float  swHash = dot( swP, float3( 12.9898f, 78.233f, 37.719f ) );
	float  swAng  = ( swHash - floor( swHash ) ) * ( 2.0f * PI );
	float  swCa = cos( swAng );
	float  swSa = sin( swAng );

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
	bool  swSkip = false;
#if SW_FACE_PROFILE
	// keeps the probed stages LIVE: the accumulator feeds an unprovable branch at the end, so the
	// compiler cannot dead-code-eliminate the walk/culls the probe is supposed to time (it DID -
	// the first probe benched BELOW the soft-off floor).
	float swProbe = 0.0f;
#endif
	for( int se = 0; se < swN; se++ )
	{
		float4 e0 = t_SoftEdges[ swFirstElem + se * 2 + 0 ];
		if( e0.w < 0.0f )					// header: cull the next caster (mask persists => casters union)
		{
			float4 e1 = t_SoftEdges[ swFirstElem + se * 2 + 1 ];
			float3 dCv = float3( e0.x, e0.y, e0.z ) - swP;
			swSkip = SoftShadow_CullCaster( dCv, e1.x, swF, swSinA, swCosA, swEps );
			// NOTE: do NOT jump `se += e1.y` PER LANE on a culled caster. It looks like a free win over
			// per-record skipping, but it MEASURED SLOWER (24 -> 30 ms worst-scene): the data-dependent
			// jump diverges the wave's loop trip counts, and the whole wave then serialises on its
			// slowest lane, costing more than the uniform cheap skip iterations it saves.
			// The WAVE-UNIFORM jump below is the fix: only when EVERY lane culls the caster (the common
			// case - a wave covers ~8x8 px, a tiny world footprint, and most casters are far from it)
			// does the whole wave take one scalar branch over the span. No lane loses records it would
			// have walked, so it is bit-exact; lanes that disagree fall back to the per-record skip.
			if( WaveActiveAllTrue( swSkip ) )
			{
				se += ( int )e1.y;
			}
			continue;
		}
		if( se + 1 >= swN ) { break; }		// malformed tail (needs the triangle's second record)
		if( swSkip ) { se++; continue; }	// culled: consume both records of this triangle (safety)
#if SW_FACE_PROFILE == 1
		swProbe += e0.x; se++; continue;	// TIMING PROBE ONLY: walk + caster culls, no triangle work
#endif

		float4 e1 = t_SoftEdges[ swFirstElem + se * 2 + 1 ];
		float4 g1 = t_SoftEdges[ swFirstElem + ( se + 1 ) * 2 + 1 ];	// record B's e1 carries v2
		se++;								// consumed the triangle's second record
		float3 v0 = float3( e0.x, e0.y, e0.z );
		float3 v1 = float3( e1.x, e1.y, e1.z );
		float3 v2 = float3( g1.x, g1.y, g1.z );
		// PER-TRIANGLE CONE/SLAB REJECT (the big perf lever). The 32 sample rays form a cone: apex swP, axis
		// swF.nrm, cross-section radius swR at depth swDistPL. A triangle that cannot reach that cone can hit NO
		// sample, so skip its 32 ray tests entirely. Conservative (over-keeps) => bit-exact: never drops a real
		// occluder. Because the FACE stream carries ALL of a caster's triangles, not just its silhouette, this is
		// what keeps a big in-cone caster from costing O(all faces * 32) per fragment.
		float3 tcen = ( v0 + v1 + v2 ) * ( 1.0f / 3.0f );
		float3 rc   = tcen - swP;
		float  cd   = dot( rc, swF.nrm );							// centroid depth along the cone axis
		// triRad (max |vertex - centroid|) is pure triangle geometry - precomputed at stream-build time (a hair
		// inflated, so the cull stays conservative) and carried in recA.e0.w (already loaded as e0, and >= 0 so it
		// never trips the header test). Saves the 3 sub / 3 dot / 2 max / sqrt recompute every fragment × triangle.
#if SW_FACE_LEGACY
		float  triRad = sqrt( max( dot( v0 - tcen, v0 - tcen ), max( dot( v1 - tcen, v1 - tcen ), dot( v2 - tcen, v2 - tcen ) ) ) );
#else
		float  triRad = e0.w;
#endif
		if( cd + triRad < swEps ) { continue; }						// wholly behind the receiver
		if( cd - triRad > swDistPL ) { continue; }					// wholly beyond the light
		float3 perp = rc - cd * swF.nrm;
		float  coneR = swR * ( cd + triRad ) / swDistPL;			// max cone radius over the triangle's depth span
		if( sqrt( dot( perp, perp ) ) - triRad > coneR ) { continue; }	// outside the sample cone: cannot occlude
#if SW_FACE_PROFILE == 2
		swProbe += cd; continue;			// TIMING PROBE ONLY: + per-triangle cone culls, no setup/samples
#endif
		float3 edge1 = v1 - v0;
		float3 edge2 = v2 - v0;
		float3 sp = swP - v0;
		// Moller-Trumbore, double-sided (occlusion is facing-independent): a hit with t in (0,1] means the
		// triangle lies between the receiver and the light plane along this sample ray. All K rays share the
		// receiver origin, so qq = cross(sp,edge1) and e2qq = dot(edge2,qq) are triangle-constant - hoisted out of
		// the K-sample loop (they were recomputed identically every sample). Bit-exact; saves a cross + a dot per sample.
#if !SW_FACE_LEGACY
		float3 qq   = cross( sp, edge1 );
		float  e2qq = dot( edge2, qq );
#endif
		for( int i = 0; i < SW_FACE_SAMPLES; i++ )
		{
			if( ( swMask & ( 1u << i ) ) != 0u ) { continue; }		// sample already blocked: skip
#if SW_FACE_LEGACY || !SW_FACE_HOIST_DIRS
			// in-loop direction from unrolled immediates: identical expression to the hoisted array,
			// ~10 VALU per test, zero registers held across the triangle loop (occupancy win)
			float2 s0  = swDisk[i];
			float2 sc  = float2( s0.x * swCa - s0.y * swSa, s0.x * swSa + s0.y * swCa );
			float3 dir = swBase + swSu * sc.x + swSv * sc.y;
#else
			float3 dir = swDir[i];									// precomputed once per fragment (hoisted)
#endif
			float3 h   = cross( dir, edge2 );
			float  aa  = dot( edge1, h );
			if( abs( aa ) < 1e-12f ) { continue; }					// ray parallel to triangle
			float  inv = 1.0f / aa;
			float  u   = inv * dot( sp, h );
			if( u < 0.0f || u > 1.0f ) { continue; }
#if SW_FACE_LEGACY
			float3 qq  = cross( sp, edge1 );
			float  vv  = inv * dot( dir, qq );
			if( vv < 0.0f || u + vv > 1.0f ) { continue; }
			float  tt  = inv * dot( edge2, qq );
#else
			float  vv  = inv * dot( dir, qq );
			if( vv < 0.0f || u + vv > 1.0f ) { continue; }
			float  tt  = inv * e2qq;
#endif
			if( tt > 1e-4f && tt <= 1.0f ) { swMask |= ( 1u << i ); }
		}
		if( swMask == swAll ) { break; }	// every sample blocked: fully in umbra, no need to read on
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
		int swListBase, int swListCount SW_TILEBUF_PARAM SW_EDGEBUF_PARAM )
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
#else
	#error SoftShadow_FaceCoverageList supports SW_FACE_SAMPLES 8 or 16
#endif
	float3 swBase = swL - swP;
	float3 swSu = swF.u * swR;
	float3 swSv = swF.v * swR;
	float  swHash = dot( swP, float3( 12.9898f, 78.233f, 37.719f ) );
	float  swAng  = ( swHash - floor( swHash ) ) * ( 2.0f * PI );
	float  swCa = cos( swAng );
	float  swSa = sin( swAng );

	uint swMask = 0u;
	const uint swAll = 0xffffffffu >> ( 32 - SW_FACE_SAMPLES );
	for( int li = 0; li < swListCount; li++ )
	{
		int se = ( int )t_SoftTiles[ swListBase + li ];				// pair-start index of record A
		float4 e0 = t_SoftEdges[ swFirstElem + se * 2 + 0 ];
		float4 e1 = t_SoftEdges[ swFirstElem + se * 2 + 1 ];
		float4 g1 = t_SoftEdges[ swFirstElem + ( se + 1 ) * 2 + 1 ];
		float3 v0 = float3( e0.x, e0.y, e0.z );
		float3 v1 = float3( e1.x, e1.y, e1.z );
		float3 v2 = float3( g1.x, g1.y, g1.z );
		// per-FRAGMENT cone/slab reject still runs: the tile cull is the same test at tile grain, so
		// this prunes the tile list down to this fragment's true cone. Identical math to the full walk.
		float3 tcen = ( v0 + v1 + v2 ) * ( 1.0f / 3.0f );
		float3 rc   = tcen - swP;
		float  cd   = dot( rc, swF.nrm );
		float  triRad = e0.w;
		if( cd + triRad < swEps ) { continue; }
		if( cd - triRad > swDistPL ) { continue; }
		float3 perp = rc - cd * swF.nrm;
		float  coneR = swR * ( cd + triRad ) / swDistPL;
		if( sqrt( dot( perp, perp ) ) - triRad > coneR ) { continue; }
		float3 edge1 = v1 - v0;
		float3 edge2 = v2 - v0;
		float3 sp = swP - v0;
		float3 qq   = cross( sp, edge1 );
		float  e2qq = dot( edge2, qq );
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
		if( swMask == swAll ) { break; }
	}
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

// Dispatcher: the pixel shader picks the coverage path by the SIGN of the record count (rpJitterTexScale.z):
// negative = FRONT-FACE stream (r_softShadowFaceCoverage), positive = light-silhouette edge stream. The face
// path needs no centre-lit winding guard (front-face area is bounded by construction), so swCentreLit is
// ignored there. Callers pass abs(swN).
SW_FUNC float SoftShadow_Coverage( float3 swP, float3 swL, float swR, int swFirstElem, int swN, float swCentreLit, bool swFace SW_EDGEBUF_PARAM )
{
#ifdef __cplusplus
	return swFace ? SoftShadow_FaceCoverage( swP, swL, swR, swFirstElem, swN, t_SoftEdges )
		   : SoftShadow_WedgeOcclusion( swP, swL, swR, swFirstElem, swN, swCentreLit, t_SoftEdges );
#else
	return swFace ? SoftShadow_FaceCoverage( swP, swL, swR, swFirstElem, swN )
		   : SoftShadow_WedgeOcclusion( swP, swL, swR, swFirstElem, swN, swCentreLit );
#endif
}

#endif // __SOFTWEDGE_COVERAGE_INC__
