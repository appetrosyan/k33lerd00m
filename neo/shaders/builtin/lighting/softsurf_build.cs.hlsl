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

// SURFACE-FOLD CACHE probe (r_softShadowSurfCache) - the budgeted BUILD pass (plan
// noble-sniffing-rose). One thread per REQUESTED texel from the request queue the term CS filled
// last frame. Per texel it walks the light's STATIC caster prefix once, evaluating each surviving
// occluder's SOLO coverage at 5 probe points (texel center + 4 corners, all on the anchor plane the
// claiming fragment recorded, ONE fixed rotation from the texel-center hash so the differences
// measure geometry rather than sampling), classifies each occluder by the 2nd difference of that
// solo coverage across the texel (<= threshold -> FOLDED, else RESIDUAL -> its triangle index goes
// to the persistent pool), and stores per corner F = coverage(all static) - coverage(residual):
// the folded static part, EXACT at the corners. A cached fragment then computes
//     term = 1 - saturate( bilerp(F corners) + exactWalk(residual + dynamic casters) )
// (softterm.cs.hlsl SW_SURF_CACHE block). The FOLD DECISION uses the CONTINUOUS closed-form solo
// coverage (SurfBuild_SoloCovExact = signed area of disk INTERSECT projected triangle / disk area),
// NOT the 1/N-quantized sample mask: a linearly-separable occluder then reads a 2nd difference at
// float epsilon and folds at a tight r_softShadowSurfCacheSecondThr, instead of the popcount
// quantum (1/N) spuriously shunting it to the residual pool. The stored F stays union-sampled so it
// matches the runtime walk exactly; only the linear-vs-non-linear test is de-quantized.
//
// Two passes over the casters per texel (classify+fold, then re-classify+emit): re-walking is
// cheaper than buffering up to caps.w residual indices per thread in registers/LDS. Build cost is
// ~5 full walks per texel, amortized once per static texel across the whole play session.

// *INDENT-OFF*
StructuredBuffer<float4>	t_SoftEdges	: register( t0 );	// tri stream + caster table (joint buffer)
StructuredBuffer<uint>		t_SoftTiles	: register( t1 );	// UNUSED dummy: the shared include declares walks that read it
#include "softwedge_coverage.inc.hlsl"
#include "softsurf_classify.inc.hlsl"	// SHARED fold classifier (compiled identically into the unit test)

RWStructuredBuffer<uint>	u_SurfTable	: register( u0 );	// texel records, 8 uints each (layout: softterm.cs.hlsl)
RWStructuredBuffer<uint>	u_SurfPool	: register( u1 );	// [0] alloc counter, then residual tri indices
RWStructuredBuffer<uint>	u_SurfQueue	: register( u2 );	// [0] count, then requested slot indices

cbuffer c_SurfBuild : register( b0 )
{
	float4	g_lightR;	// light origin xyz, disk radius w
	int4	g_range;	// x = tri stream base (float4 elems), y = static caster count, z = caster table base (float4 elems), w = light key
	int4	g_caps;		// x = table capacity (slots), y = pool capacity (uints), z = build budget (threads), w = max residual per texel
	float4	g_params;	// x = texel size G, y = 2nd-derivative fold threshold, z = queue capacity (uints), w unused
	int4	g_seed;		// x = mode (0 = consume the request queue, 1 = SCAN the table window - prewarm),
						//   y = scan start slot, z = static tri count (seed CS only), w unused
};
// *INDENT-ON*

// solo occlusion mask of ONE triangle seen from P: scalar double-sided Moller-Trumbore over the
// SW_FACE_SAMPLES sample directions (same test as the walks; no union skip - solo needs every
// bit). Directions are formed in-loop from the point's frame + the SHARED rotated disk coords
// (swScr) - a 9-point x 16-dir precomputed array would be ~430 registers of spill.
uint SurfBuild_SoloMask( float3 swP, float3 v0, float3 v1, float3 v2,
						 float3 swBase, float3 swSu, float3 swSv, in float2 swScr[SW_FACE_SAMPLES] )
{
	float3 edge1 = v1 - v0;
	float3 edge2 = v2 - v0;
	float3 sp = swP - v0;
	float3 qq   = cross( sp, edge1 );
	float  e2qq = dot( edge2, qq );
	uint m = 0u;
	for( int i = 0; i < SW_FACE_SAMPLES; i++ )
	{
		float3 dir = swBase + swSu * swScr[i].x + swSv * swScr[i].y;
		float3 h   = cross( dir, edge2 );
		float  aa  = dot( edge1, h );
		if( abs( aa ) < 1e-12f )
		{
			continue;
		}
		float  inv = 1.0f / aa;
		float  u   = inv * dot( sp, h );
		if( u < 0.0f || u > 1.0f )
		{
			continue;
		}
		float  vv  = inv * dot( dir, qq );
		if( vv < 0.0f || u + vv > 1.0f )
		{
			continue;
		}
		float  tt  = inv * e2qq;
		if( tt > 1e-4f && tt <= 1.0f )
		{
			m |= ( 1u << i );
		}
	}
	return m;
}

// the shipped morphological crack-close (softwedge_coverage.inc.hlsl) on a finished union mask
uint SurfBuild_CrackClose( uint swMask, in uint swNbr[SW_FACE_SAMPLES], uint swAll )
{
	if( swMask == 0u || swMask == swAll )
	{
		return swMask;
	}
	uint filled = swMask;
	for( int i = 0; i < SW_FACE_SAMPLES; i++ )
	{
		if( ( swMask & ( 1u << i ) ) != 0u )
		{
			continue;
		}
		uint nb = swNbr[i];
		int blocked = 0;
		for( int k = 0; k < 6; k++ )
		{
			int j = ( int )( ( nb >> ( 5 * k ) ) & 31u );
			if( ( swMask & ( 1u << j ) ) != 0u )
			{
				blocked++;
			}
		}
		if( blocked >= 5 )
		{
			filled |= ( 1u << i );
		}
	}
	return filled;
}

[numthreads( 64, 1, 1 )]
void main( uint3 tid : SV_DispatchThreadID )
{
	const uint i = tid.x;
	if( i >= ( uint )g_caps.z )
	{
		return;
	}
	uint slot;
	if( g_seed.x == 1 )
	{
		// PREWARM table-scan mode: sweep the window [scanStart, scanStart+budget) of the table and
		// build every REQUESTED slot in it (the seed CS claimed them without the queue). Every
		// light's dispatch scans the same window; the light-key check below routes each slot to its
		// owning light's dispatch. Deterministic: slot order is the table order.
		slot = ( uint )g_seed.y + i;
	}
	else
	{
		const uint qCap = ( uint )g_params.z;
		uint qc = u_SurfQueue[ 0 ];
		if( qc > qCap - 1u )
		{
			qc = qCap - 1u;	// the count word can over-increment past capacity; entries beyond it are invalid
		}
		if( i >= qc )
		{
			return;
		}
		slot = u_SurfQueue[ 1u + i ];
	}
	if( slot >= ( uint )g_caps.x )
	{
		return;
	}
	const uint sBase = slot * 8u;
	const uint curGen = ( uint )g_seed.w;		// per-light generation (matches the term/seed claim tag)
	const uint bs2 = u_SurfTable[ sBase + 2 ];
	if( ( bs2 & 3u ) != 1u || ( bs2 >> 2u ) != curGen )
	{
		return;				// not REQUESTED for this generation: duplicate / reverted / stale-gen claim
	}
	const uint keyLo = u_SurfTable[ sBase ];
	const uint keyHi = u_SurfTable[ sBase + 1 ];
	if( ( ( keyHi >> 19 ) & 0x1FFFu ) != ( uint )g_range.w )
	{
		return;				// another light's texel: that light's own dispatch builds it
	}

	// decode the texel: cell coords + normal axis + the claiming fragment's anchor height
	const int   cu = ( int )( keyLo & 0xFFFFu ) - 32768;
	const int   cv = ( int )( keyLo >> 16 ) - 32768;
	const uint  axis = ( keyHi >> 16 ) & 7u;
	const int   d  = ( int )( axis & 3u );
	const float g  = g_params.x;
	// anchor arrives flip-encoded (order-preserving uint; accumulated via InterlockedMin by the
	// seed pass / claiming fragments - the MIN height over all contributors, claim-race-independent)
	const uint  anchorEnc = u_SurfTable[ sBase + 7 ];
	const float anchor = asfloat( ( anchorEnc & 0x80000000u ) ? ( anchorEnc ^ 0x80000000u ) : ~anchorEnc );

	// 4 CORNER probe points (0,0)(1,0)(0,1)(1,1) - these define F (union masks, exact by construction) -
	// plus the texel CENTER Pc and the unit in-plane axes duAx,dvAx. The ANALYTIC classifier evaluates
	// each occluder's coverage and its LOCAL gradient/curvature at the center (SurfBuild_SoloLocal): the
	// occlusion term is an area integral, so its Taylor data at the center fixes its behaviour over the
	// whole texel - NO interior probe grid, hence no sub-probe blind spot and no VGPR-spilling array.
	// Axis placement matches the term CS key derivation: d=0 -> (u,v)=(y,z), d=1 -> (z,x), d=2 -> (x,y).
	const float PU[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
	const float PV[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
	float3 pts[4];
	for( int p = 0; p < 4; p++ )
	{
		const float pu = ( ( float )cu + PU[p] ) * g;
		const float pv = ( ( float )cv + PV[p] ) * g;
		pts[p] = ( d == 0 ) ? float3( anchor, pu, pv )
				 : ( ( d == 1 ) ? float3( pv, anchor, pu ) : float3( pu, pv, anchor ) );
	}
	const float cuC = ( ( float )cu + 0.5f ) * g;
	const float cvC = ( ( float )cv + 0.5f ) * g;
	const float3 Pc   = ( d == 0 ) ? float3( anchor, cuC, cvC ) : ( ( d == 1 ) ? float3( cvC, anchor, cuC ) : float3( cuC, cvC, anchor ) );
	const float3 duAx = ( d == 0 ) ? float3( 0, 1, 0 ) : ( ( d == 1 ) ? float3( 0, 0, 1 ) : float3( 1, 0, 0 ) );
	const float3 dvAx = ( d == 0 ) ? float3( 0, 0, 1 ) : ( ( d == 1 ) ? float3( 1, 0, 0 ) : float3( 0, 1, 0 ) );

	const float3 swL = g_lightR.xyz;
	const float  swR = max( g_lightR.w, 1e-2f );

	// ONE fixed rotation for all 5 points (texel-center hash, same formula as the term CS): the solo
	// coverage DIFFERENCES between the points then measure geometry, not sampling noise. Determinism:
	// the same texel always rebuilds bit-identically.
	const float rotHash = dot( Pc, float3( 12.9898f, 78.233f, 37.719f ) );
	const float rotAng  = ( rotHash - floor( rotHash ) ) * 6.28318531f;
	const float swCa = cos( rotAng );
	const float swSa = sin( rotAng );

#if SW_FACE_SAMPLES == 8
	const float2 swDisk[8] =
	{
		float2( 0.250000f, 0.000000f ), float2( -0.319290f, 0.292496f ),
		float2( 0.048872f, -0.556877f ), float2( 0.402444f, 0.524918f ),
		float2( -0.738535f, -0.130636f ), float2( 0.699605f, -0.445031f ),
		float2( -0.234004f, 0.870484f ), float2( -0.446271f, -0.859268f ),
	};
	const uint swNbr[8] =
	{
		0x08609443u, 0x0e218086u, 0x06121407u, 0x082284c0u,
		0x066008e1u, 0x08138c40u, 0x0a220061u, 0x06508082u,
	};
#elif SW_FACE_SAMPLES == 16
	const float2 swDisk[16] =
	{
		float2( 0.176777f, 0.000000f ), float2( -0.225772f, 0.206826f ),
		float2( 0.034558f, -0.393771f ), float2( 0.284571f, 0.371173f ),
		float2( -0.522223f, -0.092374f ), float2( 0.494695f, -0.314685f ),
		float2( -0.165466f, 0.615525f ), float2( -0.315561f, -0.607594f ),
		float2( 0.684642f, 0.250030f ), float2( -0.712256f, 0.294009f ),
		float2( 0.343354f, -0.733729f ), float2( 0.253730f, 0.808932f ),
		float2( -0.764746f, -0.443186f ), float2( 0.897134f, -0.197232f ),
		float2( -0.547507f, 0.778772f ), float2( -0.126487f, -0.976090f ),
	};
	const uint swNbr[16] =
	{
		0x0c809443u, 0x04348086u, 0x08f2a807u, 0x0a132d00u,
		0x0023a581u, 0x0681014du, 0x0091adc1u, 0x00a231e2u,
		0x02b281a3u, 0x00c33824u, 0x1a03bc45u, 0x00e0a0c3u,
		0x02f124e4u, 0x04350105u, 0x06458526u, 0x08560947u,
	};
#else
	// 32-sample set (matches softwedge_coverage.inc.hlsl's #else branch verbatim so the F union
	// masks and the runtime walk agree bit-for-bit at r_softShadowSamples 32)
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
	const uint swAll = 0xffffffffu >> ( 32 - SW_FACE_SAMPLES );

	// rotated disk coords are shared across the 9 points; per-point frame + ray basis (directions
	// are formed in-loop inside SurfBuild_SoloMask - see its doc)
	float2 scr[SW_FACE_SAMPLES];
	for( int si = 0; si < SW_FACE_SAMPLES; si++ )
	{
		const float2 s0 = swDisk[si];
		scr[si] = float2( s0.x * swCa - s0.y * swSa, s0.x * swSa + s0.y * swCa );
	}
	softFrame_t fr[4];
	float3 pBase[4], pSu[4], pSv[4];
	float  sinA[4], cosA[4];
	for( int p2 = 0; p2 < 4; p2++ )
	{
		fr[p2] = SoftShadow_Frame( pts[p2], swL );
		sinA[p2] = saturate( swR / fr[p2].distPL );
		cosA[p2] = sqrt( 1.0f - sinA[p2] * sinA[p2] );
		pBase[p2] = swL - pts[p2];
		pSu[p2] = fr[p2].u * swR;
		pSv[p2] = fr[p2].v * swR;
	}

	softFrame_t frC = SoftShadow_Frame( Pc, swL );			// center frame for the caster/derivative culls
	const float sinAC = saturate( swR / frC.distPL );
	const float cosAC = sqrt( 1.0f - sinAC * sinAC );

	// SAMPLED interior umbra probes (center + 4 quadrant centers). F and the runtime reconstruction use the
	// 1/N sampled union, so umbra must be detected in the SAME measure - the analytic signed-area Sum
	// disagrees by a hair on some geometry (a small triangle contributing between the corners) and eroded a
	// pixel. These catch interior umbra the corner masks miss, in the sampled measure that F uses.
	const float QU[5] = { 0.5f, 0.25f, 0.75f, 0.25f, 0.75f };
	const float QV[5] = { 0.5f, 0.25f, 0.25f, 0.75f, 0.75f };
	float3 ptsI[5]; softFrame_t fi[5]; float3 iBase[5], iSu[5], iSv[5];
	uint intMask[5] = { 0u, 0u, 0u, 0u, 0u };
	for( int qi = 0; qi < 5; qi++ )
	{
		const float qu = ( ( float )cu + QU[qi] ) * g;
		const float qv = ( ( float )cv + QV[qi] ) * g;
		ptsI[qi] = ( d == 0 ) ? float3( anchor, qu, qv ) : ( ( d == 1 ) ? float3( qv, anchor, qu ) : float3( qu, qv, anchor ) );
		fi[qi] = SoftShadow_Frame( ptsI[qi], swL );
		iBase[qi] = swL - ptsI[qi];
		iSu[qi] = fi[qi].u * swR;
		iSv[qi] = fi[qi].v * swR;
	}

	const float thr = g_params.y;
	const float eps = SW_NEAR_EPS;
	// REDUCED-SET mode (r_softShadowSurfCacheReduced): the CPU study (grid-or-fold-study) found a
	// translate-only grid caps ~80% (umbra breaks) but a per-texel top-K REPROJECTABLE occluder set walked
	// exactly at P reaches the discretisation ceiling (~94% at K=32-64). So instead of folding affine
	// occluders into a bilinear F scalar (the mid-penumbra error), keep EVERY occluder whose solo coverage
	// clears a cutoff as residual and walk them exactly (F falls out to 0 since resMask==allMask). Signalled
	// by a NEGATIVE caps.w (magnitude = K cap); the errTol slot params.w carries the solo cutoff (the errTol
	// bilerp gate is skipped here - nothing folds). No new cbuffer field.
	const bool  reduced = ( g_caps.w < 0 );
	const uint  redCap  = reduced ? ( uint )( -g_caps.w ) : ( uint )g_caps.w;
	const float redCut  = g_params.w;

	// ---- two passes: 0 = classify + fold (count residuals, accumulate masks), 1 = emit residual indices
	uint allMask[4] = { 0u, 0u, 0u, 0u };
	uint resMask[4] = { 0u, 0u, 0u, 0u };
	// analytic accumulators (over the whole caster set) for the sub-probe-safe gates below
	float scCornSum[4] = { 0.0f, 0.0f, 0.0f, 0.0f };	// Σ continuous solo coverage at each CORNER -> umbra gate
	float scCtrSum = 0.0f;								// Σ continuous solo coverage at the CENTER  -> umbra gate
	float devFold = 0.0f;								// Σ bilinear deviation over FOLDED occluders -> bilerp gate
	float devAll  = 0.0f;								// Σ bilinear deviation over ALL occluders -> umbra interior margin
	const float swR2c = swR * swR;
	uint resCount = 0u;
	uint foldCnt = 0u;			// static occluders folded into F = the per-texel saving (walk-units), stored in word 4 hi
	uint resOfs = 0u;
	for( int pass = 0; pass < 2; pass++ )
	{
		uint emitted = 0u;
		for( int sc = 0; sc < g_range.y; sc++ )		// STATIC caster prefix only
		{
			const float4 c0 = t_SoftEdges[ g_range.z + sc * 2 + 0 ];
			const float4 c1 = t_SoftEdges[ g_range.z + sc * 2 + 1 ];
			// conservative caster cull vs the texel CENTER with the caster radius inflated by the texel
			// diagonal: any point of the texel that could see this caster keeps it
			const float3 dCv = float3( c0.x, c0.y, c0.z ) - Pc;
			if( SoftShadow_CullCaster( dCv, c0.w + g * 1.42f, frC, sinAC, cosAC, eps ) )
			{
				continue;
			}
			const int triFirst = ( int )c1.x;
			const int triEnd   = triFirst + ( int )c1.y;
			for( int t = triFirst; t < triEnd; t++ )
			{
				const int b = g_range.x + t * 3;
				const float4 r0 = t_SoftEdges[ b + 0 ];
				const float4 r1 = t_SoftEdges[ b + 1 ];
				const float4 r2 = t_SoftEdges[ b + 2 ];
				const float3 v0 = float3( r0.x, r0.y, r0.z );
				const float3 v1 = float3( r1.x, r1.y, r1.z );
				const float3 v2 = float3( r2.x, r2.y, r2.z );
				const float3 tcen = ( v0 + v1 + v2 ) * ( 1.0f / 3.0f );
				const float  triRad = r1.w;
				// ANALYTIC classifier. Per occluder, its CONTINUOUS solo coverage at the 4 CORNERS (reusing
				// the corner frames) + the CENTER. The fold test is the occluder's own bilinearity:
				// |center - mean(corners)| (quantization-free, and it SAMPLES the corners so a corner-localized
				// curvature cannot hide the way a center-only derivative let it). The corner sums feed the umbra
				// gate: a LINEAR occluder's solo coverage is affine, so its texel-max is exactly at a corner -
				// no interior blind spot. The corner MASKS (sampled, for F) come from the same 4 points.
				uint solo[4]; uint anyHit = 0u; float scC[4];
				for( int p3 = 0; p3 < 4; p3++ )
				{
					solo[p3] = 0u; scC[p3] = 0.0f;
					const float3 rc = tcen - pts[p3];
					const float  cd = dot( rc, fr[p3].nrm );
					if( cd + triRad < eps ) { continue; }
					if( cd - triRad > fr[p3].distPL ) { continue; }
					const float3 perp = rc - cd * fr[p3].nrm;
					const float  coneR = swR * ( cd + triRad ) / fr[p3].distPL;
					if( dot( perp, perp ) > ( coneR + triRad ) * ( coneR + triRad ) ) { continue; }
					solo[p3] = SurfBuild_SoloMask( pts[p3], v0, v1, v2, pBase[p3], pSu[p3], pSv[p3], scr );
					scC[p3]  = SurfBuild_SoloCovExact( pts[p3], v0, v1, v2, fr[p3], swR2c );
					anyHit |= solo[p3];
				}
				// center continuous coverage (texel-inflated center cull to skip far triangles cheaply)
				float scCtr = 0.0f;
				{
					const float3 rcC = tcen - Pc;
					const float  cdC = dot( rcC, frC.nrm );
					const float3 perpC = rcC - cdC * frC.nrm;
					const float  coneRC = swR * ( cdC + triRad ) / frC.distPL + triRad + g;	// inflated by a texel
					if( !( cdC + triRad < eps || cdC - triRad > frC.distPL || dot( perpC, perpC ) > coneRC * coneRC ) )
					{
						scCtr = SurfBuild_SoloCovExact( Pc, v0, v1, v2, frC, swR2c );
					}
				}
				// REDUCED mode: drop occluders whose max solo coverage across the texel (4 corners + centre,
				// = the study's cell-ranked key) is below the cutoff. The union is dominated by a handful of
				// large occluders; the tiny-solo tail is redundant. This bounds the kept set to the few that
				// matter WITHOUT a GPU sort, and keeps them out of allMask/intMask so F stays 0.
				if( reduced )
				{
					float msolo = max( max( scC[0], scC[1] ), max( scC[2], scC[3] ) );
					msolo = max( msolo, scCtr );
					if( msolo <= redCut ) { continue; }
				}
				// SAMPLED interior union masks (pass 0 only) - accumulate this triangle's occlusion at the
				// 5 interior probes into the running union, for the umbra gate below.
				uint intHit = 0u;
				if( pass == 0 )
				{
					for( int ii = 0; ii < 5; ii++ )
					{
						const float3 rci = tcen - ptsI[ii];
						const float  cdi = dot( rci, fi[ii].nrm );
						if( cdi + triRad < eps ) { continue; }
						if( cdi - triRad > fi[ii].distPL ) { continue; }
						const float3 perpi = rci - cdi * fi[ii].nrm;
						const float  coneRi = swR * ( cdi + triRad ) / fi[ii].distPL;
						if( dot( perpi, perpi ) > ( coneRi + triRad ) * ( coneRi + triRad ) ) { continue; }
						const uint m = SurfBuild_SoloMask( ptsI[ii], v0, v1, v2, iBase[ii], iSu[ii], iSv[ii], scr );
						intMask[ii] |= m;
						intHit |= m;
					}
				}
				if( anyHit == 0u && intHit == 0u && scCtr <= 1e-6f )
				{
					continue;		// blocks nothing in the texel: fold as zero (free)
				}
				const float scMean = 0.25f * ( scC[0] + scC[1] + scC[2] + scC[3] );
				const float dev  = abs( scCtr - scMean );	// this occluder's deviation from bilinear over the texel
				const bool  fold = reduced ? false : ( dev <= thr );	// reduced: walk EVERY kept occluder exactly (no bilinear fold)
				if( pass == 0 )
				{
					for( int cc = 0; cc < 4; cc++ ) { scCornSum[cc] += scC[cc]; allMask[cc] |= solo[cc]; }
					scCtrSum += scCtr;
					devAll += dev;						// every occluder's non-affinity (folded OR residual)
					if( fold )
					{
						devFold += dev;								// Sum bilinear deviation over FOLDED (bilerp gate)
						foldCnt++;									// the saving: this occluder leaves the runtime walk
					}
					else
					{
						resCount++;
						for( int p5 = 0; p5 < 4; p5++ ) { resMask[p5] |= solo[p5]; }
					}
				}
				else if( !fold )
				{
					// SELF-CONTAINED residual: store the triangle's 3 verts (r0/r1/r2, 12 uints) instead of
					// its stream index, so the cached-hit walk reads them straight from the pool and never
					// touches the static tri stream. This decouples a warm texel from the emitted static
					// prefix - the cache survives view-cull churn / a reordered stream without rebuilding.
					const uint po = resOfs + emitted * 12u;
					u_SurfPool[ po +  0u ] = asuint( r0.x );	u_SurfPool[ po +  1u ] = asuint( r0.y );
					u_SurfPool[ po +  2u ] = asuint( r0.z );	u_SurfPool[ po +  3u ] = asuint( r0.w );
					u_SurfPool[ po +  4u ] = asuint( r1.x );	u_SurfPool[ po +  5u ] = asuint( r1.y );
					u_SurfPool[ po +  6u ] = asuint( r1.z );	u_SurfPool[ po +  7u ] = asuint( r1.w );
					u_SurfPool[ po +  8u ] = asuint( r2.x );	u_SurfPool[ po +  9u ] = asuint( r2.y );
					u_SurfPool[ po + 10u ] = asuint( r2.z );	u_SurfPool[ po + 11u ] = asuint( r2.w );
					emitted++;
					if( emitted >= resCount )
					{
						break;		// pass-1 early-out: everything emitted
					}
				}
			}
			if( pass == 1 && emitted >= resCount )
			{
				break;
			}
		}
		if( pass == 0 )
		{
			if( resCount > redCap )
			{
				u_SurfTable[ sBase ] = 0xFFFFFFFFu; u_SurfTable[ sBase + 7 ] = 0xFFFFFFFFu;	// over the K cap (reduced) / max residual (fold): WALK-ALWAYS -> FREE the slot (empty key). Empty reads as miss = exact walk (same result), keeps table load low so BUILT records stay reachable.
				return;
			}
			if( resCount > 0u )
			{
				uint ofs;
				const uint resWords = resCount * 12u;		// 12 uints per residual triangle (self-contained verts)
				InterlockedAdd( u_SurfPool[ 0 ], resWords, ofs );
				if( ofs + resWords + 1u > ( uint )g_caps.y )
				{
					u_SurfTable[ sBase ] = 0xFFFFFFFFu; u_SurfTable[ sBase + 7 ] = 0xFFFFFFFFu;	// WALK-ALWAYS -> FREE the slot (empty key). Was code 3: measured 77% of the table = dead weight oversubscribing it -> collision-misses. Empty reads as miss = exact walk (same result), keeps load low so BUILT records stay reachable. //	// pool full: WALK-ALWAYS (exact); reservation leaks, cache clear reclaims
					return;
				}
				resOfs = 1u + ofs;					// entries start after the counter word
			}
			else
			{
				break;								// nothing residual: skip pass 1
			}
		}
	}

	// per-corner F = coverage(all static) - coverage(residual), both through the shipped crack-close;
	// >= 0 because close is monotone and resMask is a subset of allMask. k/N values: exact in fp16.
	float F[4];
	float minCornerCov = 1.0f, maxCornerCov = 0.0f;
	for( int c = 0; c < 4; c++ )
	{
		const float covAll = ( float )SoftPopcount32( SurfBuild_CrackClose( allMask[c], swNbr, swAll ) ) / ( float )SW_FACE_SAMPLES;
		const float covRes = ( float )SoftPopcount32( SurfBuild_CrackClose( resMask[c], swNbr, swAll ) ) / ( float )SW_FACE_SAMPLES;
		F[c] = max( covAll - covRes, 0.0f );
		minCornerCov = min( minCornerCov, covAll );
		maxCornerCov = max( maxCornerCov, covAll );
	}

	// ANALYTIC sub-probe-safe gates - NO interior probe grid. The occlusion term is an area integral so its
	// per-occluder solo coverage is smooth; sampling each occluder at the 4 corners + center catches
	// corner-localized features a single center derivative missed:
	//  - UMBRA STRADDLE gate: union coverage <= Sum of solo coverages. A LINEAR (folded) occluder's solo is
	//    affine, so the max of the folded-set Sum is exactly at a corner; the max of Sum over the 4 corners +
	//    center bounds the union. If it reaches 1 (umbra) while NOT every corner is saturated the texel
	//    STRADDLES the umbra clamp where F = covAll - covRes is non-bilinear and bilerp erodes it ->
	//    WALK-ALWAYS. The exact corner union coverage (maxCornerCov, free) catches a corner already in umbra;
	//    fully-umbra texels (all corners saturated) keep F bilinear -> fold.
	//  - BILERP gate: devFold = Sum over FOLDED occluders of |center - mean(corners)| bounds the 4-corner
	//    bilerp error of F; over the error tolerance -> too curved to interpolate -> WALK-ALWAYS.
	// SAMPLED interior union coverage at the 5 interior probes (same 1/N measure as F/reconstruction).
	float maxIntCov = 0.0f;
	for( int ik = 0; ik < 5; ik++ )
	{
		const float ci = ( float )SoftPopcount32( SurfBuild_CrackClose( intMask[ik], swNbr, swAll ) ) / ( float )SW_FACE_SAMPLES;
		maxIntCov = max( maxIntCov, ci );
	}
	// analytic solo-coverage Sum max over the 4 corners + center, with a 3x devAll margin for the smooth
	// interior bulge - a belt-and-suspenders catch for umbra hiding BETWEEN the sampled points (Sum >= union
	// so it only ever over-walks). umbra somewhere in the texel + not everywhere (a lit corner) => STRADDLE
	// => the reconstruction F = covAll - covRes is a non-bilinear clamp there => WALK-ALWAYS (exact).
	float sMax = scCtrSum;
	for( int cc2 = 0; cc2 < 4; cc2++ ) { sMax = max( sMax, scCornSum[cc2] ); }
	const bool umbraSomewhere = ( maxCornerCov >= 0.999f || maxIntCov >= 0.999f || sMax + 3.0f * devAll >= 1.0f );
	const bool umbraStraddle  = ( umbraSomewhere && minCornerCov < 0.999f );
	// reduced mode caches umbra + curved texels too (it walks the exact set, no bilinear F to erode), so the
	// fold-only abandon gates are skipped; only the K-cap overflow above can abandon a reduced texel.
	if( !reduced && ( umbraStraddle || ( devFold > g_params.w ) ) )
	{
		u_SurfTable[ sBase ] = 0xFFFFFFFFu; u_SurfTable[ sBase + 7 ] = 0xFFFFFFFFu;	// WALK-ALWAYS -> FREE the slot (empty key). Was code 3: measured 77% of the table = dead weight oversubscribing it -> collision-misses. Empty reads as miss = exact walk (same result), keeps load low so BUILT records stay reachable. //		// umbra-straddle / too-curved fold: WALK-ALWAYS (exact)
		return;
	}
	u_SurfTable[ sBase + 3 ] = resOfs;
	u_SurfTable[ sBase + 4 ] = ( resCount & 0xFFFFu ) | ( min( foldCnt, 0xFFFFu ) << 16 );	// lo: residual walked, hi: folded saving
	u_SurfTable[ sBase + 5 ] = f32tof16( F[0] ) | ( f32tof16( F[1] ) << 16 );
	u_SurfTable[ sBase + 6 ] = f32tof16( F[2] ) | ( f32tof16( F[3] ) << 16 );
	u_SurfTable[ sBase + 2 ] = ( curGen << 2 ) | 2u;	// BUILT - written last
}
