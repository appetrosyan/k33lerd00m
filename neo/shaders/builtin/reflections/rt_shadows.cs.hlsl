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

// Ray-traced hard shadows. One thread per screen pixel: reconstruct the world
// position from depth, offset along the gbuffer normal, and trace an inline
// visibility ray toward the light against the world TLAS. A hit means the light is
// occluded at this pixel. The visibility fraction is written to a screen-space R8
// mask which the forward interaction shader multiplies into the light term (in
// place of the shadow-map PCF result).
//
// Hard shadows (r_rtShadowRays 1) trace a single ray: pixel-exact edges, no bias
// acne, temporally stable (deterministic, no reprojection). Soft shadows
// (r_rtShadowRays > 1) brute-force N rays to jittered points on a disc of the light
// radius facing the receiver - no denoiser, so the penumbra is honest per-frame
// noise that the abundant GPU budget can afford.
//
// Requires shader model 6.5+ for inline ray query (compiled by the ShadersRT
// target). Self-contained: does NOT include global_inc.hlsl (its implicit float->
// half narrowing is a -Werror under the 6.6 / HLSL 2021 rules this compiles with).

#pragma pack_matrix(row_major)

struct RtShadowConstants
{
	float4	unprojToWorld0;
	float4	unprojToWorld1;
	float4	unprojToWorld2;
	float4	unprojToWorld3;

	float4	lightOrigin;	// xyz = world light origin, w = light radius (soft)
	float4	params;			// x = normalBias, y = maxDist (0 = to light), z = rayCount, w = frameIndex
	int2	screenSize;
	int2	pad;
};

// *INDENT-OFF*
RaytracingAccelerationStructure	t_TLAS			: register(t0);
Texture2D<float4>				t_Depth			: register(t1);	// hardware depth (.r)
Texture2D<float4>				t_GBufferNormal	: register(t2);	// world normal .rgb (*2-1), roughness .a
RWTexture2D<float>				u_ShadowMask	: register(u0);	// visibility 0..1

cbuffer c_RtShadow : register(b1)
{
	RtShadowConstants g_Sh;
};
// *INDENT-ON*

// clip4 = float4( ndc, 1 ); world = (rows . clip4).xyz / w. The overall clip-space
// scale cancels in the perspective divide, so we can feed w = 1 directly.
float3 ReconstructWorld( float2 uv, float depth )
{
	const float4 clip = float4( uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, depth, 1.0f );
	float4 w;
	w.x = dot( g_Sh.unprojToWorld0, clip );
	w.y = dot( g_Sh.unprojToWorld1, clip );
	w.z = dot( g_Sh.unprojToWorld2, clip );
	w.w = dot( g_Sh.unprojToWorld3, clip );
	return w.xyz / w.w;
}

// Cheap per-pixel hash -> two uniform randoms, varied by frame for soft shadows.
float2 Hash23( uint2 p, uint frame )
{
	uint n = p.x * 1973u + p.y * 9277u + frame * 26699u;
	n = ( n << 13u ) ^ n;
	n = n * ( n * n * 15731u + 789221u ) + 1376312589u;
	const float inv = 1.0f / 4294967296.0f;
	uint m = n * 2654435761u;
	return float2( float( n & 0xffffu ) * ( 1.0f / 65536.0f ),
				   float( m >> 16u ) * ( 1.0f / 65536.0f ) );
}

// Two orthonormal tangents for a unit vector (Duff et al. branchless frame).
void OrthoBasis( float3 n, out float3 t, out float3 b )
{
	const float s = ( n.z >= 0.0f ) ? 1.0f : -1.0f;
	const float a = -1.0f / ( s + n.z );
	const float c = n.x * n.y * a;
	t = float3( 1.0f + s * n.x * n.x * a, s * c, -s * n.x );
	b = float3( c, s + n.y * n.y * a, -n.y );
}

// Trace one occlusion ray. Returns 1 when the light is visible, 0 when occluded.
float TraceVisibility( float3 origin, float3 dir, float tmax )
{
	RayDesc ray;
	ray.Origin = origin;
	ray.Direction = dir;
	ray.TMin = 0.0f;
	ray.TMax = tmax;

	RayQuery<RAY_FLAG_CULL_NON_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
	q.TraceRayInline( t_TLAS, RAY_FLAG_CULL_NON_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFF, ray );
	q.Proceed();

	return ( q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ) ? 0.0f : 1.0f;
}

[numthreads( 8, 8, 1 )]
void main( uint3 dispatchID : SV_DispatchThreadID )
{
	const int2 pixel = int2( dispatchID.xy );
	if( pixel.x >= g_Sh.screenSize.x || pixel.y >= g_Sh.screenSize.y )
	{
		return;
	}

	// background / sky pixels have no receiver - fully lit (shadow term 1)
	const float depth = t_Depth[pixel].r;
	if( depth >= 1.0f )
	{
		u_ShadowMask[pixel] = 1.0f;
		return;
	}

	// world normal from the gbuffer; degenerate (unwritten) normals -> treat as lit
	const float3 nEnc = t_GBufferNormal[pixel].xyz * 2.0f - 1.0f;
	if( dot( nEnc, nEnc ) < 1e-4f )
	{
		u_ShadowMask[pixel] = 1.0f;
		return;
	}
	const float3 N = normalize( nEnc );

	const float2 uv = ( float2( pixel ) + 0.5f ) / float2( g_Sh.screenSize );
	const float3 worldP = ReconstructWorld( uv, depth );

	const float bias = g_Sh.params.x;
	const float3 origin = worldP + N * bias;

	const float3 toLight = g_Sh.lightOrigin.xyz - worldP;
	const float lightDist = length( toLight );
	if( lightDist < 1e-3f )
	{
		u_ShadowMask[pixel] = 1.0f;
		return;
	}
	const float3 L = toLight / lightDist;

	const int rays = max( 1, int( g_Sh.params.z ) );

	if( rays <= 1 )
	{
		// hard shadow: single ray straight at the light
		const float tmax = max( 0.0f, lightDist - bias );
		u_ShadowMask[pixel] = TraceVisibility( origin, L, tmax );
		return;
	}

	// soft shadow: N rays to jittered points on a disc of the light radius, facing
	// the receiver. Brute force, no denoiser - honest per-frame penumbra noise.
	float3 t, b;
	OrthoBasis( L, t, b );
	const float radius = g_Sh.lightOrigin.w;
	const uint frame = uint( g_Sh.params.w );

	float vis = 0.0f;
	for( int i = 0; i < rays; i++ )
	{
		float2 r = Hash23( uint2( pixel ), frame * 17u + uint( i ) * 101u );
		// concentric-ish disc sample
		const float rr = sqrt( r.x ) * radius;
		const float ang = 6.2831853f * r.y;
		const float3 target = g_Sh.lightOrigin.xyz + ( t * cos( ang ) + b * sin( ang ) ) * rr;

		const float3 d = target - worldP;
		const float dist = length( d );
		vis += TraceVisibility( origin, d / max( dist, 1e-4f ), max( 0.0f, dist - bias ) );
	}

	u_ShadowMask[pixel] = vis / float( rays );
}
