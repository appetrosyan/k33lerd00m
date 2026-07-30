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
	int		pad0;
	int		pad1;
	int		pad2;
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
RWStructuredBuffer<float4>			u_RayRadiance	: register(u0);

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

		// STAGE 1: temporary shade proving hit geometry + normal reconstruction.
		// albedo lit by a fixed overhead term (Doom up axis is +Z); a non-black
		// atlas confirms the pipeline. Real light evaluation is Stage 2.
		const float ndl = saturate( dot( N, float3( 0.0f, 0.0f, 1.0f ) ) );
		radiance = inst.albedo.rgb * ( 0.2f + 0.8f * ndl );
	}
	else
	{
		hitDistance = ray.TMax;
		// placeholder sky / ambient radiance for rays that escape the geometry
		radiance = float3( 0.30f, 0.45f, 0.60f );
	}

	u_RayRadiance[probeIndex * g_Ddgi.raysPerProbe + rayIndex] = float4( radiance, hitDistance );
}
