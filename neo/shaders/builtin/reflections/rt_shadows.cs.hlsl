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
	float4	lightDepthBounds;	// x = zmin, y = zmax (hardware depth): skip rays outside the slab
	int2	coarseSize;			// coarse+refine: used coarse subregion dims (render / ratio)
	int2	coarseScissorMin;	// top-left coarse texel of this light's coarse rect
	int2	coarseParams;		// x = passMode (0 legacy / 1 coarse / 2 refine), y = force-upsample
	//							// (r_rtShadowCoarse==1 diagnostic: always upsample, never trace)
	int2	coarsePad;

	float4	lightProject0;		// baseLightProject rows: inside the light volume iff
	float4	lightProject1;		// 0 < c.x,c.y,c.z < c.w where c[i] = lightProject[i] . (worldP,1).
	float4	lightProject2;		// pad2.x enables the cull; outside -> interaction gives zero light.
	float4	lightProject3;
};

// *INDENT-OFF*
RaytracingAccelerationStructure	t_TLAS			: register(t0);
Texture2D<float4>				t_Depth			: register(t1);	// hardware depth (.r)
Texture2D<float4>				t_GBufferNormal	: register(t2);	// world normal .rgb (*2-1), roughness .a
Texture2D<float4>				t_Coarse		: register(t3);	// coarse+refine: coarse visibility mask (refine pass only; dummy/unread otherwise)
RWTexture2D<float>				u_ShadowMask	: register(u0);	// visibility 0..1 (coarse pass writes the coarse image here instead)

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

// Light-volume cull: true when worldP is OUTSIDE the light's projection box, matching
// idRenderMatrix::CullPointToMVPbits (zeroToOne). The interaction's falloff/cookie is zero
// there, so its shadow value is discarded - we can skip the ray. Gated by g_Sh.pad2.x.
bool OutsideLightVolume( float3 worldP )
{
	if( g_Sh.pad2.x == 0 )
	{
		return false;
	}
	const float4 p = float4( worldP, 1.0f );
	const float cx = dot( g_Sh.lightProject0, p );
	const float cy = dot( g_Sh.lightProject1, p );
	const float cz = dot( g_Sh.lightProject2, p );
	const float cw = dot( g_Sh.lightProject3, p );
	// inside iff 0 < cx,cy,cz < cw (implies cw > 0)
	return !( cx > 0.0f && cx < cw && cy > 0.0f && cy < cw && cz > 0.0f && cz < cw );
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

// Res-agnostic single-sample hard-shadow visibility, used by the coarse (passMode 1) and refine
// (passMode 2) dispatches ONLY. samplePixel indexes t_Depth/t_GBufferNormal (the coarse pass derives
// a coarser sample pixel than its own dispatch pixel); uv is the matching depth-buffer UV used to
// reconstruct the world position. The driver (RtShadowsPass::RenderLight) forces the legacy
// passMode 0 single dispatch whenever r_rtShadowForce != 0 or soft shadows (rays > 1) are active,
// so this never needs the debug-force branches or the soft-shadow disc loop that main() still
// carries for passMode 0 - duplicating this logic here rather than sharing it with that block keeps
// the already-verified legacy/debug path untouched.
float ComputeVisibility( int2 samplePixel, float2 uv )
{
	const float depth = t_Depth[samplePixel].r;
	if( depth >= 1.0f )
	{
		return 1.0f;
	}

	// depth-bounds cull: see the pad.y==0 comment on the legacy path below - same rationale.
	if( g_Sh.lightDepthBounds.y > g_Sh.lightDepthBounds.x &&
		( depth < g_Sh.lightDepthBounds.x || depth > g_Sh.lightDepthBounds.y ) )
	{
		return 1.0f;
	}

	const float3 nEnc = t_GBufferNormal[samplePixel].xyz * 2.0f - 1.0f;
	if( dot( nEnc, nEnc ) < 1e-4f )
	{
		return 1.0f;
	}

	const float3 worldP = ReconstructWorld( uv, depth );
	if( !all( isfinite( worldP ) ) || dot( worldP, worldP ) > 1.0e14f )
	{
		return 1.0f;
	}

	// outside the light volume the interaction gives zero light -> skip the ray
	if( OutsideLightVolume( worldP ) )
	{
		return 1.0f;
	}

	const float3 toLight = g_Sh.lightOrigin.xyz - worldP;
	const float lightDist = length( toLight );
	if( lightDist < 1e-3f )
	{
		return 1.0f;
	}
	const float3 L = toLight / lightDist;

	// receiver faces away from the light -> zero diffuse -> the mask is discarded; skip the ray
	if( g_Sh.pad2.y != 0 && dot( nEnc, L ) <= 0.0f )
	{
		return 1.0f;
	}

	const float bias = g_Sh.params.x;
	const float umbraFloor = g_Sh.params.y;
	const float tmax = max( 0.0f, lightDist - bias );
	return max( TraceVisibility( worldP, L, bias, tmax ), umbraFloor );
}

[numthreads( 8, 8, 1 )]
void main( uint3 dispatchID : SV_DispatchThreadID )
{
	// Coarse + edge-refine (see r_rtShadowCoarse / RtShadowsPass::RenderLight). Both dispatches are
	// only ever issued when passMode != 0; the driver forces plain passMode 0 (below) whenever debug
	// force or soft shadows (rays > 1) are active, so these two branches never need to handle them.
	if( g_Sh.coarseParams.x == 1 )
	{
		// COARSE pass: dispatch covers the (over-expanded) coarse rect in COARSE-texel space, not
		// screen pixels. u_ShadowMask is bound to the coarse image for this dispatch.
		const int2 cpix = int2( dispatchID.xy ) + g_Sh.coarseScissorMin;
		if( cpix.x >= g_Sh.coarseSize.x || cpix.y >= g_Sh.coarseSize.y )
		{
			return;
		}
		const float2 uv = ( float2( cpix ) + 0.5f ) / float2( g_Sh.coarseSize );
		const int2 samplePixel = int2( uv * float2( g_Sh.screenSize ) );
		u_ShadowMask[cpix] = ComputeVisibility( samplePixel, uv );
		return;
	}

	if( g_Sh.coarseParams.x == 2 )
	{
		// REFINE pass: full render-res dispatch over the light's normal (render-space) scissor rect.
		// u_ShadowMask is bound to the final mask here; t_Coarse holds this light's just-written
		// coarse mask. Sample the bilinear-interpolation CELL the pixel falls in (the bracket
		// floor(c)/floor(c)+1, not a fixed NxN tap) - this stays the exact interpolation footprint
		// at any coarseDiv ratio. If the 4 texels agree, the pixel sits in a flat interior (lit or
		// umbra) with no boundary passing through - upsample with no ray. Otherwise trace.
		const int2 pixel = int2( dispatchID.xy ) + g_Sh.scissorMin;
		if( pixel.x >= g_Sh.screenSize.x || pixel.y >= g_Sh.screenSize.y )
		{
			return;
		}
		const float2 uv = ( float2( pixel ) + 0.5f ) / float2( g_Sh.screenSize );

		const float2 c = uv * float2( g_Sh.coarseSize ) - 0.5f;
		const int2 c0 = int2( floor( c ) );
		const float v00 = t_Coarse[c0 + int2( 0, 0 )].r;
		const float v10 = t_Coarse[c0 + int2( 1, 0 )].r;
		const float v01 = t_Coarse[c0 + int2( 0, 1 )].r;
		const float v11 = t_Coarse[c0 + int2( 1, 1 )].r;
		const float mn = min( min( v00, v10 ), min( v01, v11 ) );
		const float mx = max( max( v00, v10 ), max( v01, v11 ) );

		// coarseParams.y != 0 (r_rtShadowCoarse == 1, "coarse-only" diagnostic): always take the
		// upsample branch, never trace - isolates the coarse dispatch/binding/barrier from the
		// boundary-detection logic below (soft/blocky shadows are the EXPECTED result in this mode).
		if( g_Sh.coarseParams.y != 0 || mx - mn <= ( 1.0f / 255.0f ) )
		{
			// flat interior (all 4 agree, within R8 quantisation) - no boundary here, no ray
			u_ShadowMask[pixel] = v00;
			return;
		}

		u_ShadowMask[pixel] = ComputeVisibility( pixel, uv );
		return;
	}

	// passMode 0: legacy single full-res dispatch - unchanged, also the always-available fallback
	// (r_rtShadowCoarse 0), and what the debug-force / soft-shadow paths always use.
	//
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

	// Depth-bounds cull (normal mode only): scissorRect.zmin/zmax is this light's volume depth
	// extent. A receiver outside it is outside the light volume, so the light's projection/falloff
	// contributes ZERO there - the interaction multiplies that zero by the shadow mask, so the mask
	// value is irrelevant and forcing it to 1 (skip the ray) changes nothing visible. (The draw
	// side's hardware depth-bounds test is currently disabled, so we rely on the falloff, not it.)
	// Guarded on zmax > zmin so an unset (0,0) bounds disables it, and on pad.y==0 so the debug
	// visualisations keep full-screen coverage.
	if( g_Sh.pad.y == 0 && g_Sh.lightDepthBounds.y > g_Sh.lightDepthBounds.x &&
		( depth < g_Sh.lightDepthBounds.x || depth > g_Sh.lightDepthBounds.y ) )
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

	// Light-volume cull (g_Sh.pad2.x, disabled in the debug-force modes above): a receiver outside
	// the light's projection box gets zero light from the interaction's falloff/cookie, so its
	// shadow value is discarded - skip the ray. Culls the scissor-rect corners the sphere/cone
	// never fills. Lossless.
	if( OutsideLightVolume( worldP ) )
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

	// Facing cull (g_Sh.pad2.y, off in debug-force modes): a receiver whose shading normal faces
	// away from the light has zero diffuse (and ~zero specular), so the interaction's light term is
	// zero regardless of the shadow value - skip the ray. nEnc is the same gbuffer normal the
	// interaction lights with, so this matches it. Lossless.
	if( g_Sh.pad2.y != 0 && dot( nEnc, L ) <= 0.0f )
	{
		u_ShadowMask[pixel] = 1.0f;
		return;
	}

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
	// the receiver. The disc jitter is STATIC per pixel (no frame term) so the penumbra
	// noise is a fixed spatial dither that does NOT crawl frame-to-frame - which is what
	// lets the spatial denoise (rt_shadow_denoise) resolve it without any temporal
	// accumulation. (Frame-varied jitter would move under the filter and fight it.)
	float3 t, b;
	OrthoBasis( L, t, b );
	const float radius = g_Sh.lightOrigin.w;

	// Penumbra-adaptive early-out: only the thin penumbra band needs the full ray budget.
	// Fully-lit and fully-shadowed interiors - most of a light's screen rect - return the
	// same visibility for every disc sample, so tracing all N there is pure waste. Trace a
	// few spread probes first; if they UNANIMOUSLY agree, the pixel is a flat interior and
	// the remaining rays are skipped (16 rays -> 4 on interiors). The probes are the first
	// samples of the same sequence, so they still count toward the average when we continue
	// into the penumbra. A missed thin sliver (all probes land one side of a ~50/50 split)
	// only nudges an already near-interior pixel and is smoothed by the spatial denoise.
	const int probeCount = min( rays, 4 );
	float vis = 0.0f;
	int i = 0;
	for( ; i < probeCount; i++ )
	{
		float2 r = Hash23( uint2( pixel ), uint( i ) * 101u );
		const float rr = sqrt( r.x ) * radius;
		const float ang = 6.2831853f * r.y;
		const float3 target = g_Sh.lightOrigin.xyz + ( t * cos( ang ) + b * sin( ang ) ) * rr;
		const float3 d = target - worldP;
		const float dist = length( d );
		vis += TraceVisibility( origin, d / max( dist, 1e-4f ), bias, max( 0.0f, dist - bias ) );
	}
	if( vis == 0.0f || vis == float( probeCount ) )
	{
		// unanimous probes -> flat interior: skip the rest, write the agreed 0/1.
		u_ShadowMask[pixel] = max( ( vis > 0.0f ) ? 1.0f : 0.0f, umbraFloor );
		return;
	}

	// penumbra: probes disagreed, so trace the remaining rays for a smooth gradient.
	for( ; i < rays; i++ )
	{
		float2 r = Hash23( uint2( pixel ), uint( i ) * 101u );
		const float rr = sqrt( r.x ) * radius;
		const float ang = 6.2831853f * r.y;
		const float3 target = g_Sh.lightOrigin.xyz + ( t * cos( ang ) + b * sin( ang ) ) * rr;
		const float3 d = target - worldP;
		const float dist = length( d );
		vis += TraceVisibility( origin, d / max( dist, 1e-4f ), bias, max( 0.0f, dist - bias ) );
	}

	u_ShadowMask[pixel] = max( vis / float( rays ), umbraFloor );
}
