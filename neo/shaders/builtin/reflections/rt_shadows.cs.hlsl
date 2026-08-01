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
	float4	params;			// x = normalBias, y = umbra floor (min shadow term), z = rayCount, w = frameIndex
	int2	screenSize;
	int2	pad;
	int2	scissorMin;		// top-left pixel of this light's dispatch rect
	int2	pad2;
	float4	cameraOrigin;	// xyz = world-space eye (primary-ray TLAS coverage probe, mode 4)
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
float TraceVisibility( float3 origin, float3 dir, float tmin, float tmax )
{
	RayDesc ray;
	ray.Origin = origin;
	ray.Direction = dir;
	// Skip the first `tmin` world units along the ray so a receiver-coplanar surface
	// (Doom stacks decals / light panels z-fighting on walls) does not self-occlude the
	// flashlight cone. tmin is the slope-scaled bias from main() (larger at grazing
	// angles), matching the N*tmin origin offset there.
	ray.TMin = tmin;
	ray.TMax = tmax;

	// Trace TWO-SIDED by default (no face culling). Doom 3 world geometry is single-sided and
	// its winding is NOT consistent across surfaces, so ANY face-cull keeps shadow-ray hits on
	// only one facing: a blocker on the kept facing shadows, a blocker on the culled facing
	// leaks light - producing a hard straight seam across a continuous receiver (not a soft
	// geometry-shaped gap). Stencil occludes from the full silhouette regardless of facing;
	// two-sided rays are the RT equivalent - the unified "cull nothing" occluder set. Self-
	// occlusion is held off by ray.TMin = bias (origin is the un-offset worldP), tuned via
	// r_rtShadowBias. pad.x is a debug knob: 0 = two-sided (default), 1 = cull back, 2 = cull front.
	uint rayFlags = RAY_FLAG_CULL_NON_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH;
	if( g_Sh.pad.x == 1 )
	{
		rayFlags |= RAY_FLAG_CULL_BACK_FACING_TRIANGLES;
	}
	else if( g_Sh.pad.x == 2 )
	{
		rayFlags |= RAY_FLAG_CULL_FRONT_FACING_TRIANGLES;
	}

	RayQuery<RAY_FLAG_CULL_NON_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
	q.TraceRayInline( t_TLAS, rayFlags, 0xFF, ray );
	q.Proceed();

	return ( q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ) ? 0.0f : 1.0f;
}

[numthreads( 8, 8, 1 )]
void main( uint3 dispatchID : SV_DispatchThreadID )
{
	// The dispatch covers only this light's screen-space scissor rect, not the whole
	// screen - scissorMin is its top-left pixel. Pixels outside the rect are never lit by
	// this light (its interactions are scissored to the same rect), so we skip the rays.
	const int2 pixel = int2( dispatchID.xy ) + g_Sh.scissorMin;
	if( pixel.x >= g_Sh.screenSize.x || pixel.y >= g_Sh.screenSize.y )
	{
		return;
	}

	// debug force (g_Sh.pad.y): 1 = fully shadowed, 2 = fully lit. Lets us test whether a
	// wrongly-bright area is even lit by an RT-shadowed light (if forcing black does not
	// darken it, the light is elsewhere - ambient / DDGI / non-RT).
	if( g_Sh.pad.y == 1 )
	{
		u_ShadowMask[pixel] = 0.0f;
		return;
	}
	if( g_Sh.pad.y == 2 )
	{
		u_ShadowMask[pixel] = 1.0f;
		return;
	}

	// background / sky pixels have no receiver - fully lit (shadow term 1)
	const float depth = t_Depth[pixel].r;
	if( depth >= 1.0f )
	{
		u_ShadowMask[pixel] = 1.0f;
		return;
	}

	// gbuffer normal only used to detect degenerate (unwritten) texels -> treat as lit.
	// Front-face culling handles self-occlusion, so the normal is no longer needed to
	// offset the ray origin.
	const float3 nEnc = t_GBufferNormal[pixel].xyz * 2.0f - 1.0f;
	if( dot( nEnc, nEnc ) < 1e-4f )
	{
		u_ShadowMask[pixel] = 1.0f;
		return;
	}

	const float2 uv = ( float2( pixel ) + 0.5f ) / float2( g_Sh.screenSize );
	const float3 worldP = ReconstructWorld( uv, depth );

	// TLAS coverage probe (g_Sh.pad.y == 4): trace a PRIMARY ray from the camera eye to this
	// pixel's depth-reconstructed world point. A correct, complete TLAS holds every visible
	// surface at its real depth, so the ray hits at ~lengthToP. Output:
	//   GREEN  = hit at the expected depth (this surface IS in the TLAS, correctly placed)
	//   BLUE   = hit closer than expected (something else in the TLAS occludes the eye->P ray)
	//   RED    = no hit within the eye->P span (this VISIBLE surface is MISSING from the TLAS)
	// Red pixels are literal holes in the TLAS - exactly the geometry rays sail through.
	if( g_Sh.pad.y == 4 )
	{
		const float3 eye = g_Sh.cameraOrigin.xyz;
		const float3 d = worldP - eye;
		const float distToP = length( d );
		RayDesc pr;
		pr.Origin = eye;
		pr.Direction = d / max( distToP, 1e-4f );
		pr.TMin = 1.0f;
		pr.TMax = distToP * 1.02f + 4.0f;
		RayQuery<RAY_FLAG_CULL_NON_OPAQUE> pq;
		pq.TraceRayInline( t_TLAS, RAY_FLAG_CULL_NON_OPAQUE, 0xFF, pr );
		pq.Proceed();
		if( pq.CommittedStatus() != COMMITTED_TRIANGLE_HIT )
		{
			u_ShadowMask[pixel] = 0.15f;	// RED-ish via the debug blit (miss = TLAS hole)
			return;
		}
		const float t = pq.CommittedRayT();
		// encode: >=0.66 hit-at-surface (good), 0.4 too-near (occluded), 0.15 miss
		u_ShadowMask[pixel] = ( abs( t - distToP ) < 12.0f ) ? 1.0f : 0.4f;
		return;
	}

	// Distant / degenerate reconstruction: at the far plane w.w -> 0, so worldP blows up to
	// huge or NaN values and the shadow ray becomes garbage (the thrashing outdoor mask with
	// a hard seam at the window's near/far depth cliff). Skip such pixels - very distant
	// geometry - rather than trace nonsense. Same failure hits ReconstructWorld in reflections.
	if( !all( isfinite( worldP ) ) || dot( worldP, worldP ) > 1.0e14f )
	{
		u_ShadowMask[pixel] = 1.0f;
		return;
	}

	const float3 toLight = g_Sh.lightOrigin.xyz - worldP;
	const float lightDist = length( toLight );
	if( lightDist < 1e-3f )
	{
		u_ShadowMask[pixel] = 1.0f;
		return;
	}
	const float3 L = toLight / lightDist;

	// Trace from the reconstructed world position UNoffset - front-face culling (in
	// TraceVisibility) is what prevents self-occlusion, not an origin push. The earlier
	// N*bias offset was the wrong tool: it leaked over thin walls and could never cover
	// the depth-reconstruction error that grows with view distance (the outdoor stipple).
	// TMin still skips the first `bias` units so coincident decals / light panels
	// z-fighting on a wall do not register as occluders.
	const float bias = g_Sh.params.x;
	const float3 origin = worldP;

	const int rays = max( 1, int( g_Sh.params.z ) );

	// Umbra floor (g_Sh.params.y): the minimum shadow term, so full shadow is not pitch
	// black. RT visibility is binary (0/1); the shipped stencil shadows never fully
	// darkened, so a small floor gives back that gentler "less pronounced" umbra without
	// touching the edge. 0 = hard black umbra.
	const float umbraFloor = g_Sh.params.y;

	// Hit-distance debug (g_Sh.pad.y == 3): instead of 0/1 occlusion, write the shadow
	// ray's hit distance normalised to 2048 world units (room scale). WHITE = ray reached
	// the light unobstructed; darker = nearer hit. Purpose: show per-pixel WHERE rays hit.
	//   - DARK (T near 0)  -> the occluder is right on the receiver = self-intersection /
	//     reconstruction landing inside the surface / coincident geometry. Fix = bias.
	//   - BRIGHT (T large) -> a genuinely distant occluder = a real geometry / wrong-ray
	//     problem, not bias. WHITE = ray reached the light unobstructed (correctly lit).
	if( g_Sh.pad.y == 3 )
	{
		RayDesc dray;
		dray.Origin = origin;
		dray.Direction = L;
		dray.TMin = bias;
		dray.TMax = max( 0.0f, lightDist - bias );
		RayQuery<RAY_FLAG_CULL_NON_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> dq;
		dq.TraceRayInline( t_TLAS, RAY_FLAG_CULL_NON_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFF, dray );
		dq.Proceed();
		u_ShadowMask[pixel] = ( dq.CommittedStatus() == COMMITTED_TRIANGLE_HIT )
							  ? saturate( dq.CommittedRayT() / 2048.0f )
							  : 1.0f;
		return;
	}

	// Bisection probes (pad.y 5/6): plain binary occlusion with a GENEROUS t-range (no bias
	// subtract, huge TMax) so nothing is masked by a degenerate span. hit = 0 (dark hole in the
	// red blit), miss = 1 (red). Mode 5 traces toward the light; mode 6 traces straight UP
	// (+Z) from worldP. If UP hits (ceiling/occluders show as dark) but toward-light misses,
	// the light DIRECTION/origin is the fault. If UP also misses, traversal from worldP is
	// broken (origin off-surface / TLAS not reachable from this ray) regardless of direction.
	if( g_Sh.pad.y == 5 || g_Sh.pad.y == 6 )
	{
		RayDesc bray;
		bray.Origin = worldP;
		bray.Direction = ( g_Sh.pad.y == 6 ) ? float3( 0.0f, 0.0f, 1.0f ) : L;
		bray.TMin = 0.5f;
		bray.TMax = 100000.0f;
		RayQuery<RAY_FLAG_CULL_NON_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> bq;
		bq.TraceRayInline( t_TLAS, RAY_FLAG_CULL_NON_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFF, bray );
		bq.Proceed();
		u_ShadowMask[pixel] = ( bq.CommittedStatus() == COMMITTED_TRIANGLE_HIT ) ? 0.0f : 1.0f;
		return;
	}

	if( rays <= 1 )
	{
		// hard shadow: single ray straight at the light
		const float tmax = max( 0.0f, lightDist - bias );
		u_ShadowMask[pixel] = max( TraceVisibility( origin, L, bias, tmax ), umbraFloor );
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
		vis += TraceVisibility( origin, d / max( dist, 1e-4f ), bias, max( 0.0f, dist - bias ) );
	}

	u_ShadowMask[pixel] = max( vis / float( rays ), umbraFloor );
}
