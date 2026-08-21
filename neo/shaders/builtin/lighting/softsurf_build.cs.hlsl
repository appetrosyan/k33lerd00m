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
// (softterm.cs.hlsl SW_SURF_CACHE block). Solo coverage here is 1/16-quantized, so the threshold
// cvar's practical steps are 0 (identical masks = the freeze) and 1/16 (one-quantum sliders = the
// linear fold); see r_softShadowSurfCacheSecondThr.
//
// Two passes over the casters per texel (classify+fold, then re-classify+emit): re-walking is
// cheaper than buffering up to caps.w residual indices per thread in registers/LDS. Build cost is
// ~5 full walks per texel, amortized once per static texel across the whole play session.

// *INDENT-OFF*
StructuredBuffer<float4>	t_SoftEdges	: register( t0 );	// tri stream + caster table (joint buffer)
StructuredBuffer<uint>		t_SoftTiles	: register( t1 );	// UNUSED dummy: the shared include declares walks that read it
#include "softwedge_coverage.inc.hlsl"

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

	// the 9 probe points, all on the anchor plane: 0-3 = corners (0,0)(1,0)(0,1)(1,1), 4 = center,
	// 5-8 = EDGE MIDPOINTS (0.5,0)(0,0.5)(1,0.5)(0.5,1). Corners define F (exact by construction);
	// center + midpoints validate the reconstruction INSIDE the texel and along its BORDERS - the
	// midpoint checks are what turns cross-texel seams into a build-time acceptance criterion.
	// Axis placement matches the term CS key derivation: d=0 -> (u,v)=(y,z), d=1 -> (z,x), d=2 -> (x,y).
	const float PU[9] = { 0.0f, 1.0f, 0.0f, 1.0f, 0.5f, 0.5f, 0.0f, 1.0f, 0.5f };
	const float PV[9] = { 0.0f, 0.0f, 1.0f, 1.0f, 0.5f, 0.0f, 0.5f, 0.5f, 1.0f };
	float3 pts[9];
	for( int p = 0; p < 9; p++ )
	{
		const float pu = ( ( float )cu + PU[p] ) * g;
		const float pv = ( ( float )cv + PV[p] ) * g;
		pts[p] = ( d == 0 ) ? float3( anchor, pu, pv )
				 : ( ( d == 1 ) ? float3( pv, anchor, pu ) : float3( pu, pv, anchor ) );
	}

	const float3 swL = g_lightR.xyz;
	const float  swR = max( g_lightR.w, 1e-2f );

	// ONE fixed rotation for all 5 points (texel-center hash, same formula as the term CS): the solo
	// coverage DIFFERENCES between the points then measure geometry, not sampling noise. Determinism:
	// the same texel always rebuilds bit-identically.
	const float rotHash = dot( pts[4], float3( 12.9898f, 78.233f, 37.719f ) );
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
	#error softsurf_build supports SW_FACE_SAMPLES 8 or 16
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
	softFrame_t fr[9];
	float3 pBase[9], pSu[9], pSv[9];
	float  sinA[9], cosA[9];
	for( int p2 = 0; p2 < 9; p2++ )
	{
		fr[p2] = SoftShadow_Frame( pts[p2], swL );
		sinA[p2] = saturate( swR / fr[p2].distPL );
		cosA[p2] = sqrt( 1.0f - sinA[p2] * sinA[p2] );
		pBase[p2] = swL - pts[p2];
		pSu[p2] = fr[p2].u * swR;
		pSv[p2] = fr[p2].v * swR;
	}

	const float thr = g_params.y;
	const float eps = SW_NEAR_EPS;

	// ---- two passes: 0 = classify + fold (count residuals, accumulate masks), 1 = emit residual indices
	uint allMask[9] = { 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u };
	uint resMask[9] = { 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u };
	uint resCount = 0u;
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
			const float3 dCv = float3( c0.x, c0.y, c0.z ) - pts[4];
			if( SoftShadow_CullCaster( dCv, c0.w + g * 1.42f, fr[4], sinA[4], cosA[4], eps ) )
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
				// solo coverage at the 9 points (per-point cone cull, then the scalar 16-sample MT)
				uint solo[9];
				uint anyHit = 0u;
				for( int p3 = 0; p3 < 9; p3++ )
				{
					solo[p3] = 0u;
					const float3 rc = tcen - pts[p3];
					const float  cd = dot( rc, fr[p3].nrm );
					if( cd + triRad < eps )
					{
						continue;
					}
					if( cd - triRad > fr[p3].distPL )
					{
						continue;
					}
					const float3 perp = rc - cd * fr[p3].nrm;
					const float  coneR = swR * ( cd + triRad ) / fr[p3].distPL;
					if( dot( perp, perp ) > ( coneR + triRad ) * ( coneR + triRad ) )
					{
						continue;
					}
					solo[p3] = SurfBuild_SoloMask( pts[p3], v0, v1, v2, pBase[p3], pSu[p3], pSv[p3], scr );
					anyHit |= solo[p3];
				}
				if( anyHit == 0u )
				{
					continue;		// blocks nothing anywhere in the texel: fold as zero (free)
				}
				// 2nd differences of the solo coverage: the two diagonals through the center plus the
				// four EDGES through their midpoints - edge curvature is exactly what turns into a
				// border seam if folded, and the diagonals alone cannot see it
				const float c00 = ( float )SoftPopcount32( solo[0] );
				const float c10 = ( float )SoftPopcount32( solo[1] );
				const float c01 = ( float )SoftPopcount32( solo[2] );
				const float c11 = ( float )SoftPopcount32( solo[3] );
				const float cC  = ( float )SoftPopcount32( solo[4] );
				const float mB  = ( float )SoftPopcount32( solo[5] );	// (0.5,0) bottom edge
				const float mL  = ( float )SoftPopcount32( solo[6] );	// (0,0.5) left edge
				const float mR  = ( float )SoftPopcount32( solo[7] );	// (1,0.5) right edge
				const float mT  = ( float )SoftPopcount32( solo[8] );	// (0.5,1) top edge
				float d2 = max( abs( c00 + c11 - 2.0f * cC ), abs( c10 + c01 - 2.0f * cC ) );
				d2 = max( d2, max( abs( c00 + c10 - 2.0f * mB ), abs( c00 + c01 - 2.0f * mL ) ) );
				d2 = max( d2, max( abs( c10 + c11 - 2.0f * mR ), abs( c01 + c11 - 2.0f * mT ) ) );
				d2 /= ( float )SW_FACE_SAMPLES;
				const bool fold = ( d2 <= thr );
				if( pass == 0 )
				{
					for( int p4 = 0; p4 < 9; p4++ )
					{
						allMask[p4] |= solo[p4];
					}
					if( !fold )
					{
						resCount++;
						for( int p5 = 0; p5 < 9; p5++ )
						{
							resMask[p5] |= solo[p5];
						}
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
			if( resCount > ( uint )g_caps.w )
			{
				u_SurfTable[ sBase + 2 ] = ( curGen << 2 ) | 3u;		// too curved to be worth caching: WALK-ALWAYS (exact)
				return;
			}
			if( resCount > 0u )
			{
				uint ofs;
				const uint resWords = resCount * 12u;		// 12 uints per residual triangle (self-contained verts)
				InterlockedAdd( u_SurfPool[ 0 ], resWords, ofs );
				if( ofs + resWords + 1u > ( uint )g_caps.y )
				{
					u_SurfTable[ sBase + 2 ] = ( curGen << 2 ) | 3u;	// pool full: WALK-ALWAYS (exact); reservation leaks, cache clear reclaims
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
	// >= 0 because close is monotone and resMask is a subset of allMask. k/16 values: exact in fp16.
	float F[4];
	for( int c = 0; c < 4; c++ )
	{
		const float covAll = ( float )SoftPopcount32( SurfBuild_CrackClose( allMask[c], swNbr, swAll ) ) / ( float )SW_FACE_SAMPLES;
		const float covRes = ( float )SoftPopcount32( SurfBuild_CrackClose( resMask[c], swNbr, swAll ) ) / ( float )SW_FACE_SAMPLES;
		F[c] = max( covAll - covRes, 0.0f );
	}

	// SELF-GATE (r_softShadowSurfCacheErrTol): the corners are exact by construction; validate the
	// reconstruction at the CENTER and at all 4 EDGE MIDPOINTS against the exact union coverage the
	// build already evaluated there. The midpoint checks bound the BORDER error - the source of
	// cross-texel seams (and of the view-displacement CONTINUITY flips, which are seams resampled):
	// two adjacent texels that both pass midpoint validation can only disagree along the shared
	// edge by ~2x the tolerance. Any point over tolerance -> WALK-ALWAYS, exact. The pool
	// reservation leaks like the overflow path (a cache clear reclaims it).
	{
		// bilerp weights of the 4 corner F at the validated points (center, then midpoints in the
		// PU/PV order): center = mean; edge midpoint = mean of its edge's two corners
		float worst = 0.0f;
		for( int vp = 4; vp < 9; vp++ )
		{
			const float wu = PU[vp];
			const float wv = PV[vp];
			const float fLo = F[0] + ( F[1] - F[0] ) * wu;
			const float fHi = F[2] + ( F[3] - F[2] ) * wu;
			const float reconFold = fLo + ( fHi - fLo ) * wv;
			const float covAllP = ( float )SoftPopcount32( SurfBuild_CrackClose( allMask[vp], swNbr, swAll ) ) / ( float )SW_FACE_SAMPLES;
			const float covResP = ( float )SoftPopcount32( SurfBuild_CrackClose( resMask[vp], swNbr, swAll ) ) / ( float )SW_FACE_SAMPLES;
			worst = max( worst, abs( reconFold + covResP - covAllP ) );
		}
		if( worst > g_params.w )
		{
			u_SurfTable[ sBase + 2 ] = ( curGen << 2 ) | 3u;		// provably bad fold: WALK-ALWAYS (exact)
			return;
		}
	}
	u_SurfTable[ sBase + 3 ] = resOfs;
	u_SurfTable[ sBase + 4 ] = resCount;
	u_SurfTable[ sBase + 5 ] = f32tof16( F[0] ) | ( f32tof16( F[1] ) << 16 );
	u_SurfTable[ sBase + 6 ] = f32tof16( F[2] ) | ( f32tof16( F[3] ) << 16 );
	u_SurfTable[ sBase + 2 ] = ( curGen << 2 ) | 2u;	// BUILT - written last
}
