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
#else
	#define SW_EDGEBUF_PARAM
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

#endif // __SOFTWEDGE_COVERAGE_INC__
