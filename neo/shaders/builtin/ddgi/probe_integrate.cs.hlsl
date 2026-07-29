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

// DDGI probe integrate (M3). Converts the per-ray radiance buffer from the trace
// pass into per-probe octahedral irradiance + distance atlases, temporally
// blended with the previous frame (hysteresis).
//
// One thread per probe octahedral texel. Border texels are left untouched for
// now (interior-only); the octahedral border copy for seamless bilinear comes
// with the M4 shading integration.
//
// Self-contained (no global_inc.hlsl) - compiled by the ShadersRT target.

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
StructuredBuffer<float4>	t_RayRadiance		: register(t0);
RWTexture2D<float4>			u_IrradianceAtlas	: register(u0);	// rgb = irradiance
RWTexture2D<float2>			u_DistanceAtlas		: register(u1);	// x = mean, y = mean^2

cbuffer c_Ddgi : register(b1)
{
	DdgiConstants g_Ddgi;
};
// *INDENT-ON*

static const float DDGI_PI = 3.14159265358979f;
static const float DDGI_DEPTH_SHARPNESS = 50.0f;

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

// Octahedral decode: [-1,1]^2 -> unit direction.
float3 OctDecode( float2 f )
{
	float3 n = float3( f.x, f.y, 1.0f - abs( f.x ) - abs( f.y ) );
	float t = saturate( -n.z );
	n.x += ( n.x >= 0.0f ) ? -t : t;
	n.y += ( n.y >= 0.0f ) ? -t : t;
	return normalize( n );
}

[numthreads( 8, 8, 1 )]
void main( uint3 dispatchID : SV_DispatchThreadID )
{
	const int probeIndex = int( dispatchID.z );
	const int totalProbes = g_Ddgi.probeGridCountZ_total.y;
	if( probeIndex >= totalProbes )
	{
		return;
	}

	const int probeSize = g_Ddgi.irradianceProbe.x;		// texels per side incl border
	const int border = 1;
	const int interior = probeSize - 2 * border;

	const int tx = int( dispatchID.x );
	const int ty = int( dispatchID.y );
	if( tx >= probeSize || ty >= probeSize )
	{
		return;
	}
	// interior texels only for now
	if( tx < border || ty < border || tx >= probeSize - border || ty >= probeSize - border )
	{
		return;
	}

	// texel -> octahedral direction for this probe texel
	float2 octUV = ( float2( tx - border, ty - border ) + 0.5f ) / float( interior );
	octUV = octUV * 2.0f - 1.0f;
	const float3 texelDir = OctDecode( octUV );

	// integrate all rays for this probe against this texel direction
	const int cx = g_Ddgi.probeGridCountsXY.x;
	const int cy = g_Ddgi.probeGridCountsXY.y;
	const int px = probeIndex % cx;
	const int py = ( probeIndex / cx ) % cy;
	const int pz = probeIndex / ( cx * cy );

	const int raysPerProbe = g_Ddgi.raysPerProbe;
	const int rayBase = probeIndex * raysPerProbe;

	float3 irradiance = float3( 0.0f, 0.0f, 0.0f );
	float sumWeight = 0.0f;
	float distMean = 0.0f;
	float distMean2 = 0.0f;
	float distWeight = 0.0f;

	for( int r = 0; r < raysPerProbe; r++ )
	{
		float3 rayDir = SphericalFibonacci( float( r ), float( raysPerProbe ) );
		rayDir = normalize( QuatRotate( g_Ddgi.rayRotation, rayDir ) );

		const float4 rr = t_RayRadiance[rayBase + r];
		const float cosw = max( 0.0f, dot( texelDir, rayDir ) );

		irradiance += rr.rgb * cosw;
		sumWeight += cosw;

		const float dw = pow( cosw, DDGI_DEPTH_SHARPNESS );
		distMean += rr.a * dw;
		distMean2 += rr.a * rr.a * dw;
		distWeight += dw;
	}

	irradiance *= 1.0f / max( sumWeight, 1e-6f );
	const float meanDist = distMean / max( distWeight, 1e-6f );
	const float meanDist2 = distMean2 / max( distWeight, 1e-6f );

	// atlas coordinate: tile (px + pz*cx, py), texel (tx, ty) within the tile
	const int tileCol = px + pz * cx;
	const int tileRow = py;
	const int2 atlasCoord = int2( tileCol * probeSize + tx, tileRow * probeSize + ty );

	// temporal blend with the previous frame; frame 0 has no history
	float hysteresis = ( g_Ddgi.frameIndex == 0 ) ? 0.0f : g_Ddgi.hysteresis;

	const float3 prevIrr = u_IrradianceAtlas[atlasCoord].rgb;
	const float2 prevDist = u_DistanceAtlas[atlasCoord];

	u_IrradianceAtlas[atlasCoord] = float4( lerp( irradiance, prevIrr, hysteresis ), 1.0f );
	u_DistanceAtlas[atlasCoord] = lerp( float2( meanDist, meanDist2 ), prevDist, hysteresis );
}
