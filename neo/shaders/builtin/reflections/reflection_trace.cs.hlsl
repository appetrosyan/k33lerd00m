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

// ---- glossy reflection: GGX VNDF importance sampling ----

// Van der Corput radical inverse + Hammersley low-discrepancy 2D point.
float RtRadicalInverse( uint bits )
{
	bits = reversebits( bits );
	return float( bits ) * 2.3283064365386963e-10f;
}
float2 RtHammersley( uint i, uint n )
{
	return float2( float( i ) / float( n ), RtRadicalInverse( i ) );
}

// Per-pixel decorrelation offset. Static (no frame index) so the reflection noise
// is a fixed spatial dither that does NOT crawl frame-to-frame - no temporal
// accumulation, matching the no-TAA rendering philosophy.
uint RtHashU( uint x )
{
	x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
	return x;
}
float2 RtPixelRand( int2 p )
{
	uint h = RtHashU( uint( p.x ) + RtHashU( uint( p.y ) ) );
	uint h2 = RtHashU( h );
	return float2( float( h ), float( h2 ) ) * 2.3283064365386963e-10f;
}

// Heitz 2018, "Sampling the GGX Distribution of Visible Normals". Ve in tangent
// space (z = surface normal); returns a microfacet normal H (tangent space).
float3 RtSampleGGXVNDF( float3 Ve, float alpha, float2 u )
{
	const float3 Vh = normalize( float3( alpha * Ve.x, alpha * Ve.y, Ve.z ) );
	const float lensq = Vh.x * Vh.x + Vh.y * Vh.y;
	const float3 T1 = ( lensq > 0.0f ) ? ( float3( -Vh.y, Vh.x, 0.0f ) * rsqrt( lensq ) ) : float3( 1.0f, 0.0f, 0.0f );
	const float3 T2 = cross( Vh, T1 );
	const float r = sqrt( u.x );
	const float phi = 2.0f * 3.14159265358979f * u.y;
	const float t1 = r * cos( phi );
	float t2 = r * sin( phi );
	const float s = 0.5f * ( 1.0f + Vh.z );
	t2 = ( 1.0f - s ) * sqrt( saturate( 1.0f - t1 * t1 ) ) + s * t2;
	const float3 Nh = t1 * T1 + t2 * T2 + sqrt( saturate( 1.0f - t1 * t1 - t2 * t2 ) ) * Vh;
	return normalize( float3( alpha * Nh.x, alpha * Nh.y, max( 0.0f, Nh.z ) ) );
}

// Smith masking term; VNDF-sampled GGX weight reduces to G2(V,L)/G1(V) (height-correlated).
float RtSmithLambda( float cosTheta, float alpha )
{
	const float c2 = cosTheta * cosTheta;
	const float tan2 = max( 0.0f, 1.0f - c2 ) / max( c2, 1e-6f );
	return 0.5f * ( -1.0f + sqrt( 1.0f + alpha * alpha * tan2 ) );
}
float RtSmithG2overG1( float NoV, float NoL, float alpha )
{
	const float lv = RtSmithLambda( NoV, alpha );
	const float ll = RtSmithLambda( NoL, alpha );
	return ( 1.0f + lv ) / ( 1.0f + lv + ll );
}

// Trace one reflection ray and return its incoming radiance: on a hit, re-shade the
// surface in world space (material + lights); on a miss, the environment probe.
// didHit reports geometry vs env (for the debug viz).
float3 TraceAndShade( float3 origin, float3 dir, float roughness, out bool didHit )
{
	didHit = false;

	RayDesc ray;
	ray.Origin = origin;
	ray.Direction = dir;
	ray.TMin = 0.0f;
	ray.TMax = g_Refl.params0.x;

	RayQuery<RAY_FLAG_CULL_NON_OPAQUE> q;
	q.TraceRayInline( t_TLAS, RAY_FLAG_CULL_NON_OPAQUE, 0xFF, ray );
	q.Proceed();

	if( q.CommittedStatus() != COMMITTED_TRIANGLE_HIT )
	{
		// Ray escaped: traced-only, no cubemap fallback (miss contributes nothing).
		didHit = false;
		return float3( 0.0f, 0.0f, 0.0f );
	}

	didHit = true;
	const float rayT = q.CommittedRayT();
	const float3 hitPos = origin + dir * rayT;
	const RtInstanceData inst = t_InstanceData[q.CommittedInstanceID()];

	// params0.w gates the full re-shade (diagnostic): 0 = flat average albedo.
	if( g_Refl.params0.w < 0.5f )
	{
		return inst.albedo.rgb;
	}

	float3 shadeN;
	float3 albedoRGB;
	if( inst.albedo.w < 0.5f )
	{
		// skinned / no-vertex instance: no UV or vertex normal, shade flat + face the ray
		shadeN = normalize( -dir );
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

		const float2 bc = q.CommittedTriangleBarycentrics();
		const float3 bw = float3( 1.0f - bc.x - bc.y, bc.x, bc.y );

		const float2 hitUV = RtLoadST( b0 ) * bw.x + RtLoadST( b1 ) * bw.y + RtLoadST( b2 ) * bw.z;
		const float3 nObj = RtLoadNormal( b0 ) * bw.x + RtLoadNormal( b1 ) * bw.y + RtLoadNormal( b2 ) * bw.z;
		const float4 tObj = RtLoadTangent( b0 ) * bw.x + RtLoadTangent( b1 ) * bw.y + RtLoadTangent( b2 ) * bw.z;

		const float3x4 o2w = q.CommittedObjectToWorld3x4();
		float3 Nw = normalize( mul( o2w, float4( nObj, 0.0f ) ) );
		if( dot( Nw, dir ) > 0.0f )
		{
			Nw = -Nw;	// face the incoming ray
		}

		const float lod = clamp( log2( 1.0f + rayT / 256.0f ), 0.0f, 8.0f );

		if( inst.normalIdx != RT_BINDLESS_INVALID )
		{
			float3 Tw = normalize( mul( o2w, float4( tObj.xyz, 0.0f ) ) );
			Tw = normalize( Tw - Nw * dot( Nw, Tw ) );			// Gram-Schmidt
			const float3 Bw = cross( Nw, Tw ) * tObj.w;			// tangent.w = bitangent sign
			const float3 nm = t_BindlessTex[NonUniformResourceIndex( inst.normalIdx )].SampleLevel( s_LinearWrap, hitUV, lod ).xyz;
			float3 tn;
			tn.xy = nm.xy * 2.0f - 1.0f;
			tn.z = sqrt( saturate( 1.0f - dot( tn.xy, tn.xy ) ) );
			Nw = normalize( Tw * tn.x + Bw * tn.y + Nw * tn.z );
		}

		shadeN = Nw;
		albedoRGB = ( inst.diffuseIdx != RT_BINDLESS_INVALID )
					? t_BindlessTex[NonUniformResourceIndex( inst.diffuseIdx )].SampleLevel( s_LinearWrap, hitUV, lod ).rgb
					: inst.albedo.rgb;
	}

	return albedoRGB * RtShadeHit( hitPos, shadeN );
}

[numthreads( 8, 8, 1 )]
void main( uint3 dispatchID : SV_DispatchThreadID )
{
	const int2 pixel = int2( dispatchID.xy );
	if( pixel.x >= g_Refl.screenSize.x || pixel.y >= g_Refl.screenSize.y )
	{
		return;
	}

	// This pass now outputs the RAW reflection (rgb) + composite weight (a); the denoise
	// pass filters it and composites over the scene. Non-reflective pixels write weight 0.

	// background / sky pixels have no reflective surface -> no reflection (weight 0)
	const float depth = t_Depth[pixel].r;
	if( depth >= 1.0f )
	{
		u_Reflection[pixel] = float4( 0.0f, 0.0f, 0.0f, 0.0f );	// no reflection here: weight 0
		return;
	}

	// world normal from the gbuffer; degenerate (unwritten) normals -> no reflection
	const float3 nEnc = t_GBufferNormal[pixel].xyz * 2.0f - 1.0f;
	if( dot( nEnc, nEnc ) < 1e-4f )
	{
		u_Reflection[pixel] = float4( 0.0f, 0.0f, 0.0f, 0.0f );	// no reflection here: weight 0
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
		u_Reflection[pixel] = float4( 0.0f, 0.0f, 0.0f, 0.0f );	// no reflection here: weight 0
		return;
	}

	const float2 uv = ( float2( pixel ) + 0.5f ) / float2( g_Refl.screenSize );
	const float3 worldP = ReconstructWorld( uv, depth );

	const float3 V = normalize( worldP - g_Refl.eyePos.xyz );	// eye -> surface (incident)
	const float3 Vo = -V;										// surface -> eye (BRDF view dir)

	// GLOSSY reflection via GGX VNDF importance sampling. A surface is not a perfect
	// mirror: its true reflection is the specular BRDF lobe integrated over incoming
	// directions. Sampling that lobe (instead of the single delta mirror ray) is both
	// the optically accurate answer AND the fix for the "teleporting" sharp highlight -
	// a small bright source becomes a soft lobe that glides instead of a sub-pixel dot
	// that pops. roughness -> 0 collapses to one near-mirror ray automatically.

	// orthonormal tangent basis around N (isotropic GGX needs no surface UV)
	const float3 T = ( abs( N.z ) < 0.999f ) ? normalize( cross( float3( 0.0f, 0.0f, 1.0f ), N ) ) : float3( 1.0f, 0.0f, 0.0f );
	const float3 B = cross( N, T );
	const float3 Vt = float3( dot( Vo, T ), dot( Vo, B ), dot( Vo, N ) );

	// GGX roughness -> alpha (Disney/UE: alpha = roughness^2), clamped off the delta
	// so the VNDF sampler stays well-defined.
	const float alpha = clamp( roughness * roughness, 1e-3f, 1.0f );

	// roughness-adaptive sample count: a mirror needs one ray, a wider lobe a few.
	// Variance stays bounded because the pass is roughness-gated to smooth surfaces
	// (broad lobes never reach here). maxSamples in worldToClip0.x (dead since the
	// screen-reprojection was removed with the re-shade).
	const int maxSamples = max( 1, ( int )g_Refl.worldToClip0.x );
	const int M = clamp( ( int )ceil( lerp( 1.0f, float( maxSamples ), saturate( roughness / max( g_Refl.params1.y, 1e-3f ) ) ) ), 1, maxSamples );

	const float2 pr = RtPixelRand( pixel );
	const float3 origin = worldP + N * g_Refl.params0.y;

	float3 sum = float3( 0.0f, 0.0f, 0.0f );
	int    hitCount = 0;
	int    validCount = 0;
	for( int i = 0; i < M; i++ )
	{
		const float2 u = frac( RtHammersley( ( uint )i, ( uint )M ) + pr );
		const float3 Ht = RtSampleGGXVNDF( Vt, alpha, u );
		const float3 Lt = reflect( -Vt, Ht );				// tangent-space reflected dir
		if( Lt.z <= 0.0f )
		{
			continue;										// below the surface: invalid
		}
		validCount++;
		const float3 wi = normalize( Lt.x * T + Lt.y * B + Lt.z * N );
		bool hit;
		const float3 Ls = TraceAndShade( origin, wi, roughness, hit );
		if( hit )
		{
			hitCount++;
			// VNDF-sampled GGX: the estimator reduces to G2(V,L)/G1(V) (Fresnel applied
			// once via the macro composite weight below, valid for these tight lobes).
			sum += Ls * RtSmithG2overG1( Vt.z, Lt.z, alpha );
		}
	}
	// Average the TRACED radiance over the rays that hit, and scale the reflection by the
	// hit fraction (coverage). Rays that escape contribute nothing, so a surface whose
	// reflection sees only open space shows its base shading - never a cubemap.
	const float coverage = ( validCount > 0 ) ? ( float( hitCount ) / float( validCount ) ) : 0.0f;
	float3 reflColor = ( hitCount > 0 ) ? ( sum / float( hitCount ) ) : float3( 0.0f, 0.0f, 0.0f );

	// debug mode 2: green = rays hit traced geometry, blue = escaped (no reflection)
	if( g_Refl.debugFlags.x == 2 )
	{
		const float3 dbg = ( hitCount > 0 ) ? float3( 0.0f, 1.0f, 0.0f ) : float3( 0.0f, 0.0f, 1.0f );
		u_Reflection[pixel] = float4( dbg, 1.0f );
		return;
	}

	// Fresnel-Schlick (dielectric) so reflections strengthen at grazing angles,
	// scaled by the roughness gate so only smooth surfaces mirror.
	const float NdotV = saturate( dot( N, -V ) );
	const float fres = 0.04f + 0.96f * pow( 1.0f - NdotV, 5.0f );
	// coverage scales the reflection by the traced-hit fraction: escaped rays add nothing,
	// so open-sky reflections fade to base shading instead of a cube.
	float weight = saturate( fres * g_Refl.params0.z ) * gate * coverage;

	// debug mode 1: show the traced reflection at full strength where rays actually hit
	if( g_Refl.debugFlags.x == 1 )
	{
		weight = ( gate > 0.0f ) ? coverage : 0.0f;
	}

	// Output RAW reflection (rgb) + composite weight (a). The denoise pass filters rgb
	// (roughness-scaled, edge-aware) and composites lerp( scene, refl, weight ).
	u_Reflection[pixel] = float4( reflColor, weight );
}
