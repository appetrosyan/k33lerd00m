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

// *INDENT-OFF*
RaytracingAccelerationStructure		t_TLAS			: register(t0);
RWStructuredBuffer<float4>			u_RayRadiance	: register(u0);

cbuffer c_Ddgi : register(b1)
{
	DdgiConstants g_Ddgi;
};
// *INDENT-ON*

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
		// M2: no surface bounce yet - treat hits as occluded.
		radiance = float3( 0.0f, 0.0f, 0.0f );
	}
	else
	{
		hitDistance = ray.TMax;
		// placeholder sky / ambient radiance
		radiance = float3( 0.30f, 0.45f, 0.60f );
	}

	u_RayRadiance[probeIndex * g_Ddgi.raysPerProbe + rayIndex] = float4( radiance, hitDistance );
}
