/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 2026 Robert Beckebans (RBDOOM-3-BFG contributors)

This program is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version. See <http://www.gnu.org/licenses/>.

===========================================================================
*/

// DDGI probe trace (M2). One thread per (probe, ray): cast an inline ray query
// against the world TLAS and write raw radiance + hit distance to a per-ray
// buffer that M3 integrates into the octahedral irradiance/distance atlases.
//
// M2 shading is sky-visibility only: a miss returns the sky/ambient colour, a
// hit returns black (occluded) plus the hit distance. Surface-bounce shading
// (sampling the hit material + previous irradiance) is a later milestone.
//
// Requires shader model 6.5+ for inline ray query (compiled by the ShadersRT
// target - see shaders/CMakeLists.txt). Deliberately self-contained: it does
// NOT include global_inc.hlsl, whose implicit float->half narrowing is a
// -Werror under the 6.6 / HLSL 2021 rules this shader compiles with.

#pragma pack_matrix(row_major)

struct DdgiConstants
{
	float4	probeGridOrigin;
	float4	probeGridSpacing;
	int2	probeGridCountsXY;		// x = count X, y = count Y
	int2	probeGridCountZ_total;	// x = count Z, y = total probes

	int2	irradianceProbe;		// x = texels/side incl border, y = border
	int2	distanceProbe;

	int		raysPerProbe;
	float	hysteresis;
	float	normalBias;
	float	viewBias;

	float4	rayRotation;			// quaternion

	int		frameIndex;
	int		numLights;				// active projected lights in t_Lights
	float	bounceGain;				// multi-bounce feedback gain
	int		pad2;
};

// Per-light shading data. Layout must match DdgiLight in DdgiPass.cpp. The four
// planes are Doom's classic light-projection texgen planes.
struct DdgiLight
{
	float4	projectS;
	float4	projectT;
	float4	projectQ;		// projection divide
	float4	projectFalloff;	// distance falloff coord (0..1)
	float4	color;			// rgb (+ pad)
	float4	origin;			// world origin xyz (+ pad)
};

// Per-TLAS-instance shading data. Layout must match DdgiInstanceData in
// DdgiAccelStructures.h. Offsets are byte offsets into the shared static cache.
struct DdgiInstanceData
{
	uint	vertexByteOffset;
	uint	indexByteOffset;
	uint	pad0;
	uint	pad1;
	float4	albedo;			// average diffuse rgb (+ pad)
};

// *INDENT-OFF*
RaytracingAccelerationStructure		t_TLAS			: register(t0);
StructuredBuffer<DdgiInstanceData>	t_InstanceData	: register(t1);
ByteAddressBuffer					t_Vertex		: register(t2);	// static idDrawVert cache
ByteAddressBuffer					t_Index			: register(t3);	// static R16 index cache
StructuredBuffer<DdgiLight>			t_Lights		: register(t4);	// projected lights
Texture2D<float4>					t_IrradianceAtlas	: register(t5);	// prev-frame irradiance
Texture2D<float2>					t_DistanceAtlas		: register(t6);	// prev-frame distance moments
RWStructuredBuffer<float4>			u_RayRadiance	: register(u0);
SamplerState						s_LinearClamp	: register(s0);

cbuffer c_Ddgi : register(b1)
{
	DdgiConstants g_Ddgi;
};
// *INDENT-ON*

// idDrawVert is a 32-byte vertex with the float3 position at offset 0.
static const uint DDGI_DRAWVERT_STRIDE = 32;

// Fetch one 16-bit index from the raw index buffer (4-byte aligned loads).
uint DdgiLoadIndex16( uint byteAddr )
{
	uint word = t_Index.Load( byteAddr & ~3u );
	uint shift = ( byteAddr & 2u ) * 8u;
	return ( word >> shift ) & 0xFFFFu;
}

static const float DDGI_PI = 3.14159265358979f;

// Evenly distributed directions on the unit sphere.
float3 SphericalFibonacci( float i, float n )
{
	const float PHI = sqrt( 5.0f ) * 0.5f + 0.5f;
	float madfrac = ( i * ( PHI - 1.0f ) ) - floor( i * ( PHI - 1.0f ) );
	float phi = 2.0f * DDGI_PI * madfrac;
	float cosTheta = 1.0f - ( 2.0f * i + 1.0f ) * ( 1.0f / n );
	float sinTheta = sqrt( saturate( 1.0f - cosTheta * cosTheta ) );
	return float3( cos( phi ) * sinTheta, sin( phi ) * sinTheta, cosTheta );
}

float3 QuatRotate( float4 q, float3 v )
{
	return v + 2.0f * cross( q.xyz, cross( q.xyz, v ) + q.w * v );
}

// Evaluate all projected lights at a world-space hit point, with a shadow ray
// per light against the world TLAS. Faithful to Doom's light shapes via the
// texgen planes; the projection cookie texture is deferred (analytic edge fade).
float3 DdgiShadeHit( float3 P, float3 N )
{
	float3 lit = float3( 0.0f, 0.0f, 0.0f );
	const float4 P4 = float4( P, 1.0f );

	for( int i = 0; i < g_Ddgi.numLights; i++ )
	{
		DdgiLight L = t_Lights[i];

		// project the hit point into the light's volume (classic texgen)
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

		// shadow ray: any opaque hit before the light occludes this sample
		RayDesc sray;
		sray.Origin = P + N * g_Ddgi.normalBias;
		sray.Direction = Ldir;
		sray.TMin = 0.0f;
		sray.TMax = max( 0.0f, dist - g_Ddgi.normalBias );

		RayQuery<RAY_FLAG_CULL_NON_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> sq;
		sq.TraceRayInline( t_TLAS, RAY_FLAG_CULL_NON_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFF, sray );
		sq.Proceed();
		if( sq.CommittedStatus() == COMMITTED_TRIANGLE_HIT )
		{
			continue;	// shadowed
		}

		// analytic falloff (cookie deferred): fade toward the volume edges and
		// with the distance falloff coordinate.
		float fadeS = smoothstep( 0.0f, 0.1f, s ) * ( 1.0f - smoothstep( 0.9f, 1.0f, s ) );
		float fadeT = smoothstep( 0.0f, 0.1f, t ) * ( 1.0f - smoothstep( 0.9f, 1.0f, t ) );
		float atten = ndl * ( 1.0f - fall ) * fadeS * fadeT;

		lit += L.color.rgb * atten;
	}

	return lit;
}

// Octahedral encode: unit direction -> [-1,1]^2 (inverse of the integrate decode).
float2 OctEncode( float3 n )
{
	float l1 = abs( n.x ) + abs( n.y ) + abs( n.z );
	float2 res = n.xy / max( l1, 1e-6f );
	if( n.z < 0.0f )
	{
		float2 s = float2( ( res.x >= 0.0f ) ? 1.0f : -1.0f, ( res.y >= 0.0f ) ? 1.0f : -1.0f );
		res = ( 1.0f - abs( res.yx ) ) * s;
	}
	return res;
}

// Atlas UV for probe (px,py,pz) sampling octahedral direction `dir`. Matches the
// tile layout written by probe_integrate; relies on the border copy for seamless
// bilinear across tile edges.
float2 DdgiProbeAtlasUV( int px, int py, int pz, float3 dir, int probeSize, int cx, int cy, int cz )
{
	const int border = 1;
	const int interior = probeSize - 2 * border;
	const float2 octUV = OctEncode( dir ) * 0.5f + 0.5f;	// [0,1]
	const int2 tile = int2( px + pz * cx, py );
	const float2 texel = float2( tile ) * probeSize + border + octUV * interior;
	const float2 atlasDim = float2( ( cx * cz ) * probeSize, cy * probeSize );
	return texel / atlasDim;
}

// Sample the previous frame's probe field at a world point, Chebyshev-weighted
// (leak control) and trilinearly blended over the 8 surrounding probes.
float3 DdgiSampleIrradiance( float3 P, float3 N )
{
	const int cx = g_Ddgi.probeGridCountsXY.x;
	const int cy = g_Ddgi.probeGridCountsXY.y;
	const int cz = g_Ddgi.probeGridCountZ_total.x;
	const int probeSize = g_Ddgi.irradianceProbe.x;
	const float3 origin = g_Ddgi.probeGridOrigin.xyz;
	const float3 spacing = g_Ddgi.probeGridSpacing.xyz;

	// bias toward the normal to reduce self-occlusion / leaking
	const float3 biasedP = P + N * g_Ddgi.normalBias;
	const float3 gridF = ( biasedP - origin ) / spacing;
	const int3 baseCoord = int3( floor( gridF ) );
	const float3 frac = saturate( gridF - float3( baseCoord ) );

	float3 sumIrr = float3( 0.0f, 0.0f, 0.0f );
	float sumW = 0.0f;

	for( int i = 0; i < 8; i++ )
	{
		const int3 off = int3( i & 1, ( i >> 1 ) & 1, ( i >> 2 ) & 1 );
		const int3 c = baseCoord + off;
		if( c.x < 0 || c.y < 0 || c.z < 0 || c.x >= cx || c.y >= cy || c.z >= cz )
		{
			continue;
		}

		const float3 probePos = origin + float3( c ) * spacing;
		const float3 trilinear = lerp( 1.0f - frac, frac, float3( off ) );
		float weight = trilinear.x * trilinear.y * trilinear.z;

		const float3 toProbe = probePos - P;
		const float distToProbe = length( toProbe );
		const float3 dirToProbe = ( distToProbe > 1e-5f ) ? ( toProbe / distToProbe ) : N;

		// smooth backface weight
		const float wrap = ( dot( dirToProbe, N ) + 1.0f ) * 0.5f;
		weight *= wrap * wrap + 0.2f;

		// Chebyshev visibility using the distance moments in the probe->point dir
		const float2 dUV = DdgiProbeAtlasUV( c.x, c.y, c.z, -dirToProbe, probeSize, cx, cy, cz );
		const float2 moments = t_DistanceAtlas.SampleLevel( s_LinearClamp, dUV, 0 );
		const float meanDist = moments.x;
		if( distToProbe > meanDist )
		{
			const float variance = abs( meanDist * meanDist - moments.y );
			const float d = distToProbe - meanDist;
			float cheb = variance / ( variance + d * d );
			cheb = max( cheb * cheb * cheb, 0.0f );
			weight *= cheb;
		}

		if( weight < 1e-4f )
		{
			continue;
		}

		const float2 iUV = DdgiProbeAtlasUV( c.x, c.y, c.z, N, probeSize, cx, cy, cz );
		const float3 irr = t_IrradianceAtlas.SampleLevel( s_LinearClamp, iUV, 0 ).rgb;

		sumIrr += irr * weight;
		sumW += weight;
	}

	return ( sumW > 0.0f ) ? ( sumIrr / sumW ) : float3( 0.0f, 0.0f, 0.0f );
}

[numthreads( 32, 1, 1 )]
void main( uint3 dispatchID : SV_DispatchThreadID )
{
	const int rayIndex = int( dispatchID.x );
	const int probeIndex = int( dispatchID.y );

	const int totalProbes = g_Ddgi.probeGridCountZ_total.y;
	if( probeIndex >= totalProbes || rayIndex >= g_Ddgi.raysPerProbe )
	{
		return;
	}

	// unflatten the probe index into 3D grid coordinates
	const int cx = g_Ddgi.probeGridCountsXY.x;
	const int cy = g_Ddgi.probeGridCountsXY.y;
	const int px = probeIndex % cx;
	const int py = ( probeIndex / cx ) % cy;
	const int pz = probeIndex / ( cx * cy );

	const float3 probePos = g_Ddgi.probeGridOrigin.xyz + float3( px, py, pz ) * g_Ddgi.probeGridSpacing.xyz;

	float3 dir = SphericalFibonacci( float( rayIndex ), float( g_Ddgi.raysPerProbe ) );
	dir = normalize( QuatRotate( g_Ddgi.rayRotation, dir ) );

	RayDesc ray;
	ray.Origin = probePos;
	ray.Direction = dir;
	ray.TMin = 0.0f;
	ray.TMax = 10000.0f;

	RayQuery<RAY_FLAG_CULL_NON_OPAQUE> q;
	q.TraceRayInline( t_TLAS, RAY_FLAG_CULL_NON_OPAQUE, 0xFF, ray );
	q.Proceed();

	float3 radiance;
	float hitDistance;
	if( q.CommittedStatus() == COMMITTED_TRIANGLE_HIT )
	{
		hitDistance = q.CommittedRayT();

		// reconstruct the hit triangle from the shared static cache
		DdgiInstanceData inst = t_InstanceData[q.CommittedInstanceID()];
		const uint triBase = inst.indexByteOffset + q.CommittedPrimitiveIndex() * 6u;	// 3 * R16
		const uint i0 = DdgiLoadIndex16( triBase + 0u );
		const uint i1 = DdgiLoadIndex16( triBase + 2u );
		const uint i2 = DdgiLoadIndex16( triBase + 4u );

		const float3 p0 = asfloat( t_Vertex.Load3( inst.vertexByteOffset + i0 * DDGI_DRAWVERT_STRIDE ) );
		const float3 p1 = asfloat( t_Vertex.Load3( inst.vertexByteOffset + i1 * DDGI_DRAWVERT_STRIDE ) );
		const float3 p2 = asfloat( t_Vertex.Load3( inst.vertexByteOffset + i2 * DDGI_DRAWVERT_STRIDE ) );

		// object -> world, then geometric normal faced toward the ray origin
		const float3x4 o2w = q.CommittedObjectToWorld3x4();
		const float3 w0 = mul( o2w, float4( p0, 1.0f ) );
		const float3 w1 = mul( o2w, float4( p1, 1.0f ) );
		const float3 w2 = mul( o2w, float4( p2, 1.0f ) );
		float3 N = normalize( cross( w1 - w0, w2 - w0 ) );
		if( dot( N, dir ) > 0.0f )
		{
			N = -N;
		}

		// STAGE 2: shade the hit with the real projected lights (shadowed).
		const float3 hitPos = ray.Origin + dir * hitDistance;
		float3 shade = DdgiShadeHit( hitPos, N );

		// STAGE 3: add the previous frame's irradiance at the hit for multi-bounce
		// (infinite bounce via the temporal feedback), Chebyshev-weighted.
		if( g_Ddgi.frameIndex > 0 && g_Ddgi.bounceGain > 0.0f )
		{
			shade += g_Ddgi.bounceGain * DdgiSampleIrradiance( hitPos, N );
		}

		radiance = inst.albedo.rgb * shade;
	}
	else
	{
		hitDistance = ray.TMax;
		// placeholder sky / ambient radiance for rays that escape the geometry
		radiance = float3( 0.30f, 0.45f, 0.60f );
	}

	u_RayRadiance[probeIndex * g_Ddgi.raysPerProbe + rayIndex] = float4( radiance, hitDistance );
}
