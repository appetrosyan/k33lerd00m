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

// Spatial denoise + composite for the RT reflection. The trace pass writes the RAW
// per-pixel reflection (rgb) plus its composite weight (a). This pass runs a single
// edge-aware bilateral filter over the raw reflection and composites the result over
// the scene: out = lerp( scene, denoised, weight ).
//
// NON-temporal (no history buffer, no reprojection) - matches the no-TAA rendering
// philosophy. The blur radius is scaled by surface ROUGHNESS: a mirror (roughness at
// or below the gate low) is left razor-sharp (radius 0), and only the stochastic glossy
// pixels are filtered, with the blur widening as roughness rises to match the BRDF lobe.
// Edge stopping uses the g-buffer normal + hardware depth so a reflection never bleeds
// across a geometry boundary.

#pragma pack_matrix(row_major)

struct DenoiseConstants
{
	int2	screenSize;
	int2	pad0;
	float4	params;		// x = gateLo, y = gateHi, z = maxRadius (px), w = depthSigma
};

// *INDENT-OFF*
Texture2D<float4>	t_RawReflection	: register(t0);	// rgb = raw reflection, a = composite weight
Texture2D<float4>	t_SceneColor	: register(t1);	// resolved lit scene (composite base)
Texture2D<float4>	t_Depth			: register(t2);	// hardware depth (.r)
Texture2D<float4>	t_GBufferNormal	: register(t3);	// world normal .rgb (*2-1), roughness .a
RWTexture2D<float4>	u_Out			: register(u0);	// composited scene + reflection
SamplerState		s_LinearClamp	: register(s0);

cbuffer c_Denoise : register(b1)
{
	DenoiseConstants g_D;
};
// *INDENT-ON*

float3 DecodeNormal( int2 p )
{
	float3 n = t_GBufferNormal[p].xyz * 2.0f - 1.0f;
	return ( dot( n, n ) > 1e-4f ) ? normalize( n ) : float3( 0.0f, 0.0f, 0.0f );
}

[numthreads( 8, 8, 1 )]
void main( uint3 dispatchID : SV_DispatchThreadID )
{
	const int2 p = int2( dispatchID.xy );
	if( p.x >= g_D.screenSize.x || p.y >= g_D.screenSize.y )
	{
		return;
	}

	const float3 base = t_SceneColor[p].rgb;
	const float4 c = t_RawReflection[p];
	const float w = c.a;						// composite weight (0 on non-reflective pixels)

	if( w <= 0.0f )
	{
		u_Out[p] = float4( base, 1.0f );		// no reflection here
		return;
	}

	const float roughness = t_GBufferNormal[p].a;
	const float gateLo = g_D.params.x;
	const float gateHi = g_D.params.y;
	// 0 at the mirror end of the gate, 1 at the rough end
	const float rr = saturate( ( roughness - gateLo ) / max( gateHi - gateLo, 1e-3f ) );

	// mirrors are sharp, not noisy: leave them untouched
	if( rr <= 0.002f )
	{
		u_Out[p] = float4( lerp( base, c.rgb, w ), 1.0f );
		return;
	}

	const float maxRadius = g_D.params.z;
	const int   radius = clamp( ( int )ceil( maxRadius * rr ), 1, ( int )maxRadius );
	const float sigma = max( 1.0f, maxRadius * rr * 0.5f );

	const float3 cN = DecodeNormal( p );
	const float  cDepth = t_Depth[p].r;

	float3 accum = c.rgb;	// center sample
	float  wsum = 1.0f;

	for( int dy = -radius; dy <= radius; dy++ )
	{
		for( int dx = -radius; dx <= radius; dx++ )
		{
			if( dx == 0 && dy == 0 )
			{
				continue;
			}
			const int2 q = p + int2( dx, dy );
			if( q.x < 0 || q.y < 0 || q.x >= g_D.screenSize.x || q.y >= g_D.screenSize.y )
			{
				continue;
			}
			const float4 s = t_RawReflection[q];
			if( s.a <= 0.0f )
			{
				continue;						// neighbour is non-reflective - do not pull it in
			}
			// spatial gaussian
			const float d2 = float( dx * dx + dy * dy );
			const float ws = exp( -d2 / ( 2.0f * sigma * sigma ) );
			// normal edge stop (sharp)
			const float3 nN = DecodeNormal( q );
			const float wn = pow( saturate( dot( cN, nN ) ), 32.0f );
			// depth edge stop (relative to a tunable sigma)
			const float sd = t_Depth[q].r;
			const float wd = exp( -abs( sd - cDepth ) / max( g_D.params.w, 1e-6f ) );

			const float wt = ws * wn * wd;
			accum += s.rgb * wt;
			wsum += wt;
		}
	}

	const float3 denoised = accum / max( wsum, 1e-6f );
	u_Out[p] = float4( lerp( base, denoised, w ), 1.0f );
}
