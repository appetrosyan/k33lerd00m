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
	float4	params1;		// x = gateLo, y = gateHi, z = envMipScale, w unused
	int2	screenSize;
	int2	debugFlags;		// x = debug mode
};

// Per-TLAS-instance shading data. Layout must match DdgiInstanceData in
// DdgiAccelStructures.h. Offsets are byte offsets into the shared static cache.
struct RtInstanceData
{
	uint	vertexByteOffset;
	uint	indexByteOffset;
	uint	diffuseIdx;		// bindless index of the diffuse image (0xFFFFFFFF = none)
	uint	normalIdx;		// bindless index of the bump/normal image (0xFFFFFFFF = none)
	float4	albedo;			// average diffuse rgb; .w = 1 static (real verts), 0 skinned/flat
};

// Per-light shading data. Layout must match ReflLight in ReflectionsPass.cpp
// (and DdgiLight). The four planes are Doom's classic light-projection texgen planes.
struct RtLight
{
	float4	projectS;
	float4	projectT;
	float4	projectQ;		// projection divide
	float4	projectFalloff;	// distance falloff coord (0..1)
	float4	color;			// rgb (+ pad)
	float4	origin;			// world origin xyz (+ pad)
};

// *INDENT-OFF*
RaytracingAccelerationStructure	t_TLAS			: register(t0);
Texture2D<float4>				t_SceneColor	: register(t1);	// resolved lit scene (base / passthrough)
Texture2D<float4>				t_Depth			: register(t2);	// hardware depth (.r)
Texture2D<float4>				t_GBufferNormal	: register(t3);	// world normal .rgb (*2-1), roughness .a
Texture2D<float4>				t_EnvRadiance	: register(t4);	// octahedral env radiance (miss fallback)
StructuredBuffer<RtInstanceData>	t_InstanceData	: register(t5);
ByteAddressBuffer				t_Vertex		: register(t6);	// static idDrawVert cache
ByteAddressBuffer				t_Index			: register(t7);	// static R16 index cache
StructuredBuffer<RtLight>		t_Lights		: register(t8);	// projected lights
RWTexture2D<float4>				u_Reflection	: register(u0);	// composited output
SamplerState					s_LinearClamp	: register(s0);	// env / gbuffers
SamplerState					s_LinearWrap	: register(s1);	// material textures

// bindless material textures (diffuse + normal maps), indexed per hit instance
Texture2D						t_BindlessTex[]	: register(t0, space1);

cbuffer c_Refl : register(b1)
{
	ReflectionConstants g_Refl;
};
// *INDENT-ON*

static const uint RT_DRAWVERT_STRIDE = 32;
static const uint RT_BINDLESS_INVALID = 0xFFFFFFFFu;

// Fetch one 16-bit index from the raw index buffer (4-byte aligned loads).
uint RtLoadIndex16( uint byteAddr )
{
	uint word = t_Index.Load( byteAddr & ~3u );
	uint shift = ( byteAddr & 2u ) * 8u;
	return ( word >> shift ) & 0xFFFFu;
}

// idDrawVert channel decode (see DrawVert.h): st = 2x float16 @ +12; normal = 4x
// unsigned byte @ +16 (x*2/255-1); tangent = 4x unsigned byte @ +20 (.w = sign).
float2 RtLoadST( uint vbase )
{
	uint w = t_Vertex.Load( vbase + 12u );
	return float2( f16tof32( w & 0xFFFFu ), f16tof32( w >> 16u ) );
}
float3 RtLoadNormal( uint vbase )
{
	uint w = t_Vertex.Load( vbase + 16u );
	float3 n = float3( w & 0xFFu, ( w >> 8u ) & 0xFFu, ( w >> 16u ) & 0xFFu );
	return n * ( 2.0f / 255.0f ) - 1.0f;
}
float4 RtLoadTangent( uint vbase )
{
	uint w = t_Vertex.Load( vbase + 20u );
	float4 t = float4( w & 0xFFu, ( w >> 8u ) & 0xFFu, ( w >> 16u ) & 0xFFu, ( w >> 24u ) & 0xFFu );
	return t * ( 2.0f / 255.0f ) - 1.0f;
}

// Octahedral encode matching the engine's octEncode (global_inc.hlsl) so the
// baked radiance probes are addressed with the same convention they were packed.
float2 SignNotZero( float2 v )
{
	return float2( ( v.x >= 0.0f ) ? 1.0f : -1.0f, ( v.y >= 0.0f ) ? 1.0f : -1.0f );
}

float2 OctEncode( float3 v )
{
	float l1 = abs( v.x ) + abs( v.y ) + abs( v.z );
	float2 oct = v.xy * ( 1.0f / max( l1, 1e-6f ) );
	if( v.z < 0.0f )
	{
		oct = ( 1.0f - abs( oct.yx ) ) * SignNotZero( oct.xy );
	}
	return oct;
}

// Sample the environment radiance probe along a world direction, roughness -> mip.
float3 SampleEnv( float3 dir, float roughness )
{
	const float2 octUV = OctEncode( dir ) * 0.5f + 0.5f;
	const float mip = roughness * g_Refl.params1.z;
	return t_EnvRadiance.SampleLevel( s_LinearClamp, octUV, mip ).rgb;
}

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

// Evaluate all projected lights at a world hit point, with a shadow ray per light
// against the world TLAS. Ported from DdgiShadeHit (probe_trace.cs.hlsl): faithful
// to Doom's light shapes via the texgen planes; projection cookie deferred (analytic
// edge fade). numLights = debugFlags.y, shadow-ray bias = params1.w.
float3 RtShadeHit( float3 P, float3 N )
{
	float3 lit = float3( 0.0f, 0.0f, 0.0f );
	const float4 P4 = float4( P, 1.0f );
	const int numLights = g_Refl.debugFlags.y;
	const float bias = g_Refl.params1.w;

	for( int i = 0; i < numLights; i++ )
	{
		RtLight L = t_Lights[i];

		float q = dot( L.projectQ, P4 );
		if( q <= 0.0f )
		{
			continue;
		}
		float s = dot( L.projectS, P4 ) / q;
		float t = dot( L.projectT, P4 ) / q;
		float fall = dot( L.projectFalloff, P4 );
		if( s < 0.0f || s > 1.0f || t < 0.0f || t > 1.0f || fall < 0.0f || fall > 1.0f )
		{
			continue;	// outside the light's projected volume
		}

		float3 toLight = L.origin.xyz - P;
		float dist = length( toLight );
		if( dist <= 0.0001f )
		{
			continue;
		}
		float3 Ldir = toLight / dist;
		float ndl = saturate( dot( N, Ldir ) );
		if( ndl <= 0.0f )
		{
			continue;
		}

		// shadow ray: any opaque hit before the light occludes this sample.
		// eyePos.w gates it (diagnostic / perf): 0 = treat as unshadowed.
		if( g_Refl.eyePos.w > 0.5f )
		{
			RayDesc sray;
			sray.Origin = P + N * bias;
			sray.Direction = Ldir;
			sray.TMin = 0.0f;
			sray.TMax = max( 0.0f, dist - bias );

			RayQuery<RAY_FLAG_CULL_NON_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> sq;
			sq.TraceRayInline( t_TLAS, RAY_FLAG_CULL_NON_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFF, sray );
			sq.Proceed();
			if( sq.CommittedStatus() == COMMITTED_TRIANGLE_HIT )
			{
				continue;	// shadowed
			}
		}

		float fadeS = smoothstep( 0.0f, 0.1f, s ) * ( 1.0f - smoothstep( 0.9f, 1.0f, s ) );
		float fadeT = smoothstep( 0.0f, 0.1f, t ) * ( 1.0f - smoothstep( 0.9f, 1.0f, t ) );
		float atten = ndl * ( 1.0f - fall ) * fadeS * fadeT;

		lit += L.color.rgb * atten;
	}

	return lit;
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

	// roughness gate: only smooth surfaces get a mirror reflection. Rough surfaces
	// keep their existing (env-probe) shading untouched. Kept alive in debug so the
	// trace visualisation still covers the whole frame.
	const float roughness = t_GBufferNormal[pixel].a;
	const float gate = 1.0f - smoothstep( g_Refl.params1.x, g_Refl.params1.y, roughness );
	if( gate <= 0.0f && g_Refl.debugFlags.x == 0 )
	{
		u_Reflection[pixel] = float4( base, 1.0f );
		return;
	}

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

	float3 reflColor;
	bool   didHit = false;	// ray hit real geometry (re-shaded), vs miss (env fallback)

	if( q.CommittedStatus() == COMMITTED_TRIANGLE_HIT )
	{
		// STRUCTURAL re-shade: the reflection colour is a pure function of the world
		// hit (material texture + world lighting), NOT the camera's framebuffer. This
		// is what removes the camera-coupled "teleport" of the old screen-colour reuse.
		didHit = true;
		const float rayT = q.CommittedRayT();
		const float3 hitPos = ray.Origin + R * rayT;
		const RtInstanceData inst = t_InstanceData[q.CommittedInstanceID()];

		float3 shadeN;
		float3 albedoRGB;

		// params0.w gates the full re-shade (diagnostic): 0 = flat average albedo,
		// no bindless sampling and no lights - isolates the re-shade cost/correctness.
		if( g_Refl.params0.w < 0.5f )
		{
			reflColor = inst.albedo.rgb;
		}
		else
		{
		if( inst.albedo.w < 0.5f )
		{
			// skinned / no-vertex instance: no UV or vertex normal available, so shade
			// flat with a face-the-ray normal (see DdgiAccelStructures skinned path).
			shadeN = normalize( -R );
			albedoRGB = inst.albedo.rgb;
		}
		else
		{
			// reconstruct the hit triangle's vertex attributes from the static cache
			const uint triBase = inst.indexByteOffset + q.CommittedPrimitiveIndex() * 6u;	// 3 * R16
			const uint i0 = RtLoadIndex16( triBase + 0u );
			const uint i1 = RtLoadIndex16( triBase + 2u );
			const uint i2 = RtLoadIndex16( triBase + 4u );
			const uint b0 = inst.vertexByteOffset + i0 * RT_DRAWVERT_STRIDE;
			const uint b1 = inst.vertexByteOffset + i1 * RT_DRAWVERT_STRIDE;
			const uint b2 = inst.vertexByteOffset + i2 * RT_DRAWVERT_STRIDE;

			// barycentric interpolation of UV, object-space normal + tangent
			const float2 bc = q.CommittedTriangleBarycentrics();
			const float3 bw = float3( 1.0f - bc.x - bc.y, bc.x, bc.y );

			const float2 uv = RtLoadST( b0 ) * bw.x + RtLoadST( b1 ) * bw.y + RtLoadST( b2 ) * bw.z;
			const float3 nObj = RtLoadNormal( b0 ) * bw.x + RtLoadNormal( b1 ) * bw.y + RtLoadNormal( b2 ) * bw.z;
			const float4 tObj = RtLoadTangent( b0 ) * bw.x + RtLoadTangent( b1 ) * bw.y + RtLoadTangent( b2 ) * bw.z;

			// object -> world (w = 0: rotate the direction, drop translation). TLAS
			// instance transforms are rigid so the 3x3 needs no inverse-transpose.
			const float3x4 o2w = q.CommittedObjectToWorld3x4();
			float3 Nw = normalize( mul( o2w, float4( nObj, 0.0f ) ) );
			if( dot( Nw, R ) > 0.0f )
			{
				Nw = -Nw;	// face the incoming reflection ray
			}

			// distance-driven mip so far reflections do not alias/shimmer.
			// ponytail: simple footprint heuristic (doubles every 256u), tune if needed.
			const float lod = clamp( log2( 1.0f + rayT / 256.0f ), 0.0f, 8.0f );

			if( inst.normalIdx != RT_BINDLESS_INVALID )
			{
				float3 Tw = normalize( mul( o2w, float4( tObj.xyz, 0.0f ) ) );
				Tw = normalize( Tw - Nw * dot( Nw, Tw ) );			// Gram-Schmidt
				const float3 Bw = cross( Nw, Tw ) * tObj.w;			// tangent.w = bitangent sign
				float3 nm = t_BindlessTex[NonUniformResourceIndex( inst.normalIdx )].SampleLevel( s_LinearWrap, uv, lod ).xyz;
				float3 tn;
				tn.xy = nm.xy * 2.0f - 1.0f;
				tn.z = sqrt( saturate( 1.0f - dot( tn.xy, tn.xy ) ) );
				Nw = normalize( Tw * tn.x + Bw * tn.y + Nw * tn.z );
			}

			shadeN = Nw;
			albedoRGB = ( inst.diffuseIdx != RT_BINDLESS_INVALID )
						? t_BindlessTex[NonUniformResourceIndex( inst.diffuseIdx )].SampleLevel( s_LinearWrap, uv, lod ).rgb
						: inst.albedo.rgb;
		}

		reflColor = albedoRGB * RtShadeHit( hitPos, shadeN );
		}
	}
	else
	{
		// ray escaped into open space: environment radiance probe along R
		reflColor = SampleEnv( R, roughness );
	}

	// debug mode 2: visualise the trace pipeline (brightness-independent proof).
	// green = hit geometry (world re-shade), blue = miss (env fallback)
	if( g_Refl.debugFlags.x == 2 )
	{
		const float3 dbg = didHit ? float3( 0.0f, 1.0f, 0.0f ) : float3( 0.0f, 0.0f, 1.0f );
		u_Reflection[pixel] = float4( dbg, 1.0f );
		return;
	}

	// Fresnel-Schlick (dielectric) so reflections strengthen at grazing angles,
	// scaled by the roughness gate so only smooth surfaces mirror.
	const float NdotV = saturate( dot( N, -V ) );
	const float fres = 0.04f + 0.96f * pow( 1.0f - NdotV, 5.0f );
	float weight = saturate( fres * g_Refl.params0.z ) * gate;

	// debug mode 1: show the reflection at full strength on every gated surface
	if( g_Refl.debugFlags.x == 1 )
	{
		weight = ( gate > 0.0f ) ? 1.0f : 0.0f;
	}

	u_Reflection[pixel] = float4( lerp( base, reflColor, weight ), 1.0f );
}
