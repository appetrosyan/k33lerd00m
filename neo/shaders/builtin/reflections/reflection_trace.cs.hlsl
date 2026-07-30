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

// Hybrid ray-traced reflections (RTR-2). One thread per screen pixel: reconstruct
// the world position + normal from the depth / gbuffer, reflect the view vector,
// and trace an inline ray query against the world TLAS. The ray HITS real geometry
// (including off-screen), then the hit is reprojected to the screen and the lit
// scene colour is sampled there (the "hybrid" part - reflection colour comes from
// the already-shaded framebuffer, not from a re-shade). Off-screen / occluded hits
// have no screen colour and get no reflection yet (env fallback is RTR-3).
//
// The pass composites in place: it outputs lerp( base lit colour, reflection,
// weight ) so a plain blit copies the result back over the HDR scene target.
//
// Requires shader model 6.5+ for inline ray query (compiled by the ShadersRT
// target - see shaders/CMakeLists.txt). Deliberately self-contained: it does NOT
// include global_inc.hlsl, whose implicit float->half narrowing is a -Werror under
// the 6.6 / HLSL 2021 rules this shader compiles with.

#pragma pack_matrix(row_major)

struct ReflectionConstants
{
	float4	unprojToWorld0;
	float4	unprojToWorld1;
	float4	unprojToWorld2;
	float4	unprojToWorld3;

	float4	worldToClip0;
	float4	worldToClip1;
	float4	worldToClip2;
	float4	worldToClip3;

	float4	eyePos;

	float4	params0;		// x = maxRayDist, y = normalBias, z = intensity, w = disoccEps
	int2	screenSize;
	int2	debugFlags;		// x = debug mode
};

// *INDENT-OFF*
RaytracingAccelerationStructure	t_TLAS			: register(t0);
Texture2D<float4>				t_SceneColor	: register(t1);	// resolved lit scene (HDR)
Texture2D<float4>				t_Depth			: register(t2);	// hardware depth (.r)
Texture2D<float4>				t_GBufferNormal	: register(t3);	// world normal in .rgb (*2-1)
RWTexture2D<float4>				u_Reflection	: register(u0);	// composited output
SamplerState					s_LinearClamp	: register(s0);

cbuffer c_Refl : register(b1)
{
	ReflectionConstants g_Refl;
};
// *INDENT-ON*

// clip4 = float4( ndc, 1 ); world = (rows . clip4).xyz / w. The overall clip-space
// scale cancels in the perspective divide, so we can feed w = 1 directly.
float3 ReconstructWorld( float2 uv, float depth )
{
	const float4 clip = float4( uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, depth, 1.0f );
	float4 w;
	w.x = dot( g_Refl.unprojToWorld0, clip );
	w.y = dot( g_Refl.unprojToWorld1, clip );
	w.z = dot( g_Refl.unprojToWorld2, clip );
	w.w = dot( g_Refl.unprojToWorld3, clip );
	return w.xyz / w.w;
}

[numthreads( 8, 8, 1 )]
void main( uint3 dispatchID : SV_DispatchThreadID )
{
	const int2 pixel = int2( dispatchID.xy );
	if( pixel.x >= g_Refl.screenSize.x || pixel.y >= g_Refl.screenSize.y )
	{
		return;
	}

	const float3 base = t_SceneColor[pixel].rgb;

	// background / sky pixels have no reflective surface - pass the lit colour through
	const float depth = t_Depth[pixel].r;
	if( depth >= 1.0f )
	{
		u_Reflection[pixel] = float4( base, 1.0f );
		return;
	}

	// world normal from the gbuffer; degenerate (unwritten) normals -> no reflection
	const float3 nEnc = t_GBufferNormal[pixel].xyz * 2.0f - 1.0f;
	if( dot( nEnc, nEnc ) < 1e-4f )
	{
		u_Reflection[pixel] = float4( base, 1.0f );
		return;
	}
	const float3 N = normalize( nEnc );

	const float2 uv = ( float2( pixel ) + 0.5f ) / float2( g_Refl.screenSize );
	const float3 worldP = ReconstructWorld( uv, depth );

	const float3 V = normalize( worldP - g_Refl.eyePos.xyz );	// eye -> surface (incident)
	const float3 R = normalize( reflect( V, N ) );

	// trace the reflection ray against the world TLAS
	RayDesc ray;
	ray.Origin = worldP + N * g_Refl.params0.y;
	ray.Direction = R;
	ray.TMin = 0.0f;
	ray.TMax = g_Refl.params0.x;

	RayQuery<RAY_FLAG_CULL_NON_OPAQUE> q;
	q.TraceRayInline( t_TLAS, RAY_FLAG_CULL_NON_OPAQUE, 0xFF, ray );
	q.Proceed();

	float3 reflColor = base;
	float  weight = 0.0f;
	bool   didHit = false;		// ray hit any geometry
	bool   validSample = false;	// hit reprojected to an on-screen, non-occluded pixel

	if( q.CommittedStatus() == COMMITTED_TRIANGLE_HIT )
	{
		didHit = true;
		const float rayT = q.CommittedRayT();
		const float3 hitPos = ray.Origin + R * rayT;

		// reproject the world hit back to the screen
		const float4 hp = float4( hitPos, 1.0f );
		float4 clip;
		clip.x = dot( g_Refl.worldToClip0, hp );
		clip.y = dot( g_Refl.worldToClip1, hp );
		clip.z = dot( g_Refl.worldToClip2, hp );
		clip.w = dot( g_Refl.worldToClip3, hp );

		if( clip.w > 0.0f )
		{
			const float2 huv = float2( 0.5f + 0.5f * clip.x / clip.w,
									   0.5f - 0.5f * clip.y / clip.w );
			if( huv.x >= 0.0f && huv.x <= 1.0f && huv.y >= 0.0f && huv.y <= 1.0f )
			{
				// disocclusion test in world space (convention-free): the pixel the
				// hit lands on must actually BE the hit, not something in front of it
				const int2 hpix = int2( huv * float2( g_Refl.screenSize ) );
				const float sceneDepth = t_Depth[hpix].r;
				const float3 sceneWorld = ReconstructWorld( huv, sceneDepth );

				const float eps = g_Refl.params0.w + 0.01f * rayT;
				if( distance( sceneWorld, hitPos ) <= eps )
				{
					validSample = true;
					reflColor = t_SceneColor.SampleLevel( s_LinearClamp, huv, 0 ).rgb;

					// Fresnel-Schlick (dielectric) so reflections strengthen at grazing
					const float NdotV = saturate( dot( N, -V ) );
					const float fres = 0.04f + 0.96f * pow( 1.0f - NdotV, 5.0f );
					weight = saturate( fres * g_Refl.params0.z );
				}
			}
		}
	}

	// debug mode 2: visualise the trace pipeline (brightness-independent proof).
	// red   = the reflection ray hit geometry but did not resolve to screen colour
	// green = the hit reprojected to a valid on-screen, non-occluded pixel
	if( g_Refl.debugFlags.x == 2 )
	{
		const float3 dbg = validSample ? float3( 0.0f, 1.0f, 0.0f )
					  : ( didHit ? float3( 1.0f, 0.0f, 0.0f ) : float3( 0.0f, 0.0f, 0.0f ) );
		u_Reflection[pixel] = float4( dbg, 1.0f );
		return;
	}

	// debug mode 1: show the reflection at full strength wherever it is valid
	if( g_Refl.debugFlags.x == 1 && weight > 0.0f )
	{
		weight = 1.0f;
	}

	u_Reflection[pixel] = float4( lerp( base, reflColor, weight ), 1.0f );
}
