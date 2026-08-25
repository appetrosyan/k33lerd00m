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

// SURFACE-FOLD CACHE prewarm SEED pass (r_softShadowSurfCachePrewarm): claim every texel of a
// light's STATIC geometry up front, so the cache is BUILT before the player ever sees the area -
// the lazy miss->build->pop warm-up never happens on static world surfaces. One thread per static
// caster triangle (in a Doom3 map the static caster set IS the world receiver set - walls and
// floors both cast and receive; noshadow receivers fall back to the lazy claim path): rasterize
// the triangle's texel footprint on its dominant-axis plane and claim each covered texel with
// anchor = the TRIANGLE PLANE's height at the texel center - exact, deterministic, maximum
// precision (no fragment claim-race anchor). Claims here do NOT ride the request queue: the build
// CS's table-SCAN mode sweeps the whole table over the following frames (budget-paced, GPU-headroom
// governed) and builds every REQUESTED slot it passes.

// *INDENT-OFF*
StructuredBuffer<float4>	t_SoftEdges	: register( t0 );	// tri stream (joint buffer)
RWStructuredBuffer<uint>	u_SurfTable	: register( u0 );	// texel records, 8 uints each (layout: softterm.cs.hlsl)
RWStructuredBuffer<uint>	u_SurfQueue	: register( u1 );	// [0]=count, then claimed slot indices (warm build consumes)

cbuffer c_SurfSeed : register( b0 )
{
	float4	g_lightR;	// unused (shared CB layout with softsurf_build)
	int4	g_range;	// x = tri stream base (float4 elems), y = static caster count (unused), z = caster table base (unused), w = light key
	int4	g_caps;		// x = table capacity (slots), y/z unused here, w unused
	float4	g_params;	// x = texel size G, y unused, z unused, w unused
	int4	g_seed;		// x = mode (unused), y = scan start (unused), z = STATIC TRI COUNT, w unused
};
// *INDENT-ON*

// max texels one triangle may claim; larger footprints are clamped (the un-seeded remainder is
// picked up by the lazy fragment-claim path with its fragment anchor - correct, just less exact)
// 2026-08-26 (task #87): was 16 - at G=8 that seeds only a 128x128-unit corner of each triangle, and
// since the runtime term is READ-ONLY (no lazy claims), the clamped remainder was PERMANENTLY
// unbuildable: measured 0.0-1.5% serve hit on the big-room heavy caps (cap0006 1678 hits of 3.7M
// frags) with the prewarm cursor complete - the fragments' keys were simply never planted. 128 spans
// 1024x1024 units, covering any realistic BSP polygon; worst case 16k texel iterations for ONE seed
// thread, paid once at the load burst.
#define SW_SEED_MAX_SPAN	128

// order-preserving float->uint encoding so the texel anchor can accumulate via InterlockedMin:
// min in encoded space == min float. DETERMINISM: several different-plane triangles covering one
// texel race the claim; a first-writer anchor would be run-dependent, the MIN over all covering
// planes is order-independent. The cleared table value 0xFFFFFFFF is +inf in this encoding, so
// Min works straight off a cleared slot.
uint SwSurfFlipF( float f )
{
	const uint u = asuint( f );
	return ( u & 0x80000000u ) ? ~u : ( u | 0x80000000u );
}

[numthreads( 64, 1, 1 )]
void main( uint3 tid : SV_DispatchThreadID )
{
	const int t = ( int )tid.x;
	if( t >= g_seed.z )
	{
		return;
	}
	const int b = g_range.x + t * 3;
	const float3 v0 = t_SoftEdges[ b + 0 ].xyz;
	const float3 v1 = t_SoftEdges[ b + 1 ].xyz;
	const float3 v2 = t_SoftEdges[ b + 2 ].xyz;
	const float3 n = cross( v1 - v0, v2 - v0 );
	const float3 an = abs( n );
	const int   d  = ( an.x >= an.y && an.x >= an.z ) ? 0 : ( ( an.y >= an.z ) ? 1 : 2 );
	const float nd = ( d == 0 ) ? n.x : ( ( d == 1 ) ? n.y : n.z );
	if( abs( nd ) < 1e-6f )
	{
		return;					// degenerate triangle
	}
	// SIGN-AGNOSTIC dominant axis (no |4 sign bit): must match the term CS read key. The geometric
	// normal's sign is winding-dependent and disagreed with the shading normal the term reads, so the
	// sign bit made every warm read miss. Anchor height h below is sign-invariant, so this is lossless.
	const uint axis = ( uint )d;

	// project to the tangent axes - the SAME mapping as the term CS key derivation:
	// d=0 -> (u,v)=(y,z), d=1 -> (z,x), d=2 -> (x,y)
	float2 a, bb, c;
	float  nu, nv;
	if( d == 0 )
	{
		a = v0.yz;	bb = v1.yz;	c = v2.yz;	nu = n.y;	nv = n.z;
	}
	else if( d == 1 )
	{
		a = v0.zx;	bb = v1.zx;	c = v2.zx;	nu = n.z;	nv = n.x;
	}
	else
	{
		a = v0.xy;	bb = v1.xy;	c = v2.xy;	nu = n.x;	nv = n.y;
	}
	const float g = g_params.x;
	const float ndv0 = dot( n, v0 );	// plane: n . X = n . v0

	const float2 mn = min( a, min( bb, c ) );
	const float2 mx = max( a, max( bb, c ) );
	const int lu = ( int )floor( mn.x / g );
	const int lv = ( int )floor( mn.y / g );
	int hu = ( int )floor( mx.x / g );
	int hv = ( int )floor( mx.y / g );
	if( hu - lu >= SW_SEED_MAX_SPAN )
	{
		hu = lu + SW_SEED_MAX_SPAN - 1;    // safety clamp only: the runtime term is read-only, so an
	}									   // un-seeded remainder is PERMANENTLY unbuildable (see #define)
	if( hv - lv >= SW_SEED_MAX_SPAN )
	{
		hv = lv + SW_SEED_MAX_SPAN - 1;
	}

	// 2D edge functions, orientation-normalized, with a texel-diagonal slack so any texel the
	// triangle touches is claimed (conservative)
	const float wsign = ( ( bb.x - a.x ) * ( c.y - a.y ) - ( bb.y - a.y ) * ( c.x - a.x ) ) < 0.0f ? -1.0f : 1.0f;
	const float slack = g * 0.71f;
	const uint  capM = ( uint )g_caps.x - 1u;
	const uint  curGen = ( uint )g_seed.w;		// per-light generation (matches term/build claim tag)
	const float l0 = length( bb - a ), l1 = length( c - bb ), l2 = length( a - c );	// loop-invariant edge lengths

	for( int cv = lv; cv <= hv; cv++ )
	{
		for( int cu = lu; cu <= hu; cu++ )
		{
			const float uc = ( ( float )cu + 0.5f ) * g;
			const float vc = ( ( float )cv + 0.5f ) * g;
			// inside-with-slack: signed distance of the texel center from each edge line
			const float e0 = wsign * ( ( bb.x - a.x ) * ( vc - a.y ) - ( bb.y - a.y ) * ( uc - a.x ) );
			const float e1 = wsign * ( ( c.x - bb.x ) * ( vc - bb.y ) - ( c.y - bb.y ) * ( uc - bb.x ) );
			const float e2 = wsign * ( ( a.x - c.x ) * ( vc - c.y ) - ( a.y - c.y ) * ( uc - c.x ) );
			if( e0 < -slack * l0 || e1 < -slack * l1 || e2 < -slack * l2 )
			{
				continue;			// texel does not touch the triangle
			}
			// anchor: the triangle plane's dominant-axis height at the texel center
			const float h = ( ndv0 - nu * uc - nv * vc ) / nd;
			// kw HALF-CELL BIAS (0%-hit audit): Doom3 floors sit at multiples of 8/16 = exactly on
			// G-cell boundaries. Unbiased floor(h/g) put the seed's plane-height and the serve's
			// rasterized world-pos on a float knife edge (63.9999 vs 64.0 -> different cw -> the KEY
			// differs -> systematic empty-slot on flat floors, the majority receiver). +g/2 moves the
			// knife edge to mid-cell heights where geometry rarely sits. MUST match the serve's cw.
			const int ku = cu + 32768, kv = cv + 32768, kw = ( int )floor( ( h + 0.5f * g ) / g ) + 32768;
			if( ku < 0 || ku > 65535 || kv < 0 || kv > 65535 || kw < 0 || kw > 65535 )
			{
				continue;
			}
			const uint keyLo = ( uint )ku | ( ( uint )kv << 16 );
			const uint keyHi = ( uint )kw | ( axis << 16 ) | ( ( uint )g_range.w << 19 );
			if( keyLo >= 0xFFFFFFFEu )
			{
				continue;			// empty/tombstone-sentinel alias: never cached (lazy exact path)
			}
			const uint hh = keyLo * 0x9E3779B1u ^ keyHi * 0x85EBCA77u;
			uint slot = hh & capM;
			for( int pr = 0; pr < 16; pr++ )
			{
				const uint sBase = slot * 8u;
				const uint w0 = u_SurfTable[ sBase ];
				if( w0 == keyLo && u_SurfTable[ sBase + 1 ] == keyHi )
				{
					const uint ss2 = u_SurfTable[ sBase + 2 ];
					if( ( ss2 >> 2u ) != curGen )
					{
						// stale generation of this same cell+light: reclaim for the current generation
						// so the table-scan build re-fills it (else the light's abandoned-gen slots block
						// their own cells from ever re-caching until a GC clear).
						uint prevSS;
						InterlockedCompareExchange( u_SurfTable[ sBase + 2 ], ss2, ( curGen << 2 ) | 1u, prevSS );
						if( prevSS == ss2 )
						{
							u_SurfTable[ sBase + 7 ] = SwSurfFlipF( h );
							uint qi; InterlockedAdd( u_SurfQueue[ 0 ], 1u, qi );	// enqueue for the warm build
							if( qi + 1u < ( uint )g_params.z ) { u_SurfQueue[ 1u + qi ] = slot; }
						}
						else { InterlockedMin( u_SurfTable[ sBase + 7 ], SwSurfFlipF( h ) ); }
					}
					else
					{
						// already claimed by a sibling this generation: contribute our min-anchor plane
						InterlockedMin( u_SurfTable[ sBase + 7 ], SwSurfFlipF( h ) );
					}
					break;
				}
				if( w0 == 0xFFFFFFFFu || w0 == 0xFFFFFFFEu )	// empty OR tombstone (build-freed): both claimable
				{
					uint prev;
					InterlockedCompareExchange( u_SurfTable[ sBase ], w0, keyLo, prev );
					if( prev == w0 )
					{
						u_SurfTable[ sBase + 1 ] = keyHi;
						InterlockedMin( u_SurfTable[ sBase + 7 ], SwSurfFlipF( h ) );	// cleared/freed = +inf: Min just works
						u_SurfTable[ sBase + 2 ] = ( curGen << 2 ) | 1u;	// REQUESTED (gen-tagged)
						uint qie; InterlockedAdd( u_SurfQueue[ 0 ], 1u, qie );	// enqueue for the warm build (queue mode)
						if( qie + 1u < ( uint )g_params.z ) { u_SurfQueue[ 1u + qie ] = slot; }
						break;		// claimed by us
					}
					if( prev == keyLo && u_SurfTable[ sBase + 1 ] == keyHi )
					{
						InterlockedMin( u_SurfTable[ sBase + 7 ], SwSurfFlipF( h ) );	// lost the race to a SIBLING: still contribute
						break;
					}
					// lost the race to a FOREIGN key (audit finding #5): our key is NOT planted here -
					// keep probing (the old unconditional break silently dropped the texel forever).
				}
				slot = ( slot + 1u ) & capM;
			}
		}
	}
}
