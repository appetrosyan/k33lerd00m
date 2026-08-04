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

// Spatial denoise of the soft RT shadow penumbra. rt_shadows writes the raw (noisy)
// per-pixel visibility from N jittered disc rays; this pass runs a single edge-aware
// scalar bilateral over it and writes the clean mask the interaction reads. Per light,
// over that light's scissor rect (same mapping rt_shadows uses: pixel = tid + scissorMin).
//
// NON-temporal (matches the static-jitter soft shadows - no history, no reprojection).
// The filter blurs the visibility SPATIALLY, guided only by g-buffer normal + depth
// (geometry edges), NOT by the shadow value: a flat umbra (0) or lit region (1) is
// uniform so it survives untouched, and only the noisy penumbra gradient is smoothed.
// The radius is the per-light penumbra scale (from the light size, set by the pass);
// a hard contact (radius 0) is left razor-sharp.

#pragma pack_matrix(row_major)

struct ShadowDenoiseConstants
{
	int2	screenSize;
	int2	scissorMin;		// top-left pixel of this light's rect (matches rt_shadows)
	int2	scissorMax;		// inclusive bottom-right pixel of this light's rect
	int2	pad0;
	float4	params;			// x = radius (px), y = depthSigma, z/w unused
};

// *INDENT-OFF*
Texture2D<float>	t_RawMask		: register(t0);	// raw per-pixel visibility 0..1
Texture2D<float4>	t_Depth			: register(t1);	// hardware depth (.r)
Texture2D<float4>	t_GBufferNormal	: register(t2);	// world normal .rgb (*2-1), roughness .a
RWTexture2D<float>	u_Out			: register(u0);	// denoised visibility

cbuffer c_ShDenoise : register(b1)
{
	ShadowDenoiseConstants g_D;
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
	const int2 pixel = int2( dispatchID.xy ) + g_D.scissorMin;
	if( pixel.x > g_D.scissorMax.x || pixel.y > g_D.scissorMax.y ||
			pixel.x >= g_D.screenSize.x || pixel.y >= g_D.screenSize.y )
	{
		return;
	}

	const float centerVis = t_RawMask[pixel];

	// Analytic (deterministic PCSS) mode (g_D.pad0.x): the raw mask is NOT a noisy visibility but
	// an ENCODED penumbra - lit = 1.0, occluded = penumbra radius (px) normalised by maxRadiusPx.
	// Scatter each occluded pixel's shadow over a disc of its own penumbra radius: the umbra core
	// (radius reached at distance 0) stays dark, the shadow fades out to lit at the radius edge.
	// Deterministic (no random taps) -> stable in motion; width tracks blocker distance ->
	// contact-hardening. Geometry edge-stop (normal + depth) keeps it from bleeding across cliffs.
	if( g_D.pad0.x >= 1 )
	{
		const int maxR = ( int )g_D.params.z;
		const float umbra = g_D.params.w;
		if( maxR <= 0 )
		{
			u_Out[pixel] = ( centerVis >= 0.995f ) ? 1.0f : umbra;	// no window: hard decode
			return;
		}
		const float cDepth = t_Depth[pixel].r;
		const float3 cN = DecodeNormal( pixel );
		const float depthSigma = g_D.params.y;

		float shadowCov = 0.0f;
		for( int dy = -maxR; dy <= maxR; dy++ )
		{
			for( int dx = -maxR; dx <= maxR; dx++ )
			{
				const int2 q = pixel + int2( dx, dy );
				if( q.x < g_D.scissorMin.x || q.y < g_D.scissorMin.y ||
						q.x > g_D.scissorMax.x || q.y > g_D.scissorMax.y )
				{
					continue;
				}
				const float r = t_RawMask[q];
				if( r >= 0.995f )
				{
					continue;					// lit tap casts no shadow
				}
				const float P = max( r * g_D.params.z, 0.5f );		// this occluder's penumbra radius (px)
				const float d = sqrt( float( dx * dx + dy * dy ) );
				if( d > P )
				{
					continue;					// outside this occluder's penumbra
				}
				// geometry edge-stop: do not spread shadow onto surfaces at a different depth/orientation
				const float3 nN = DecodeNormal( q );
				const float wn = pow( saturate( dot( cN, nN ) ), 32.0f );
				const float sd = t_Depth[q].r;
				const float wd = exp( -abs( sd - cDepth ) / max( depthSigma, 1e-6f ) );
				const float cov = ( 1.0f - smoothstep( 0.0f, P, d ) ) * wn * wd;
				shadowCov = max( shadowCov, cov );	// union of penumbrae (darkest wins)
			}
		}
		u_Out[pixel] = max( 1.0f - shadowCov, umbra );
		return;
	}

	const int radius = ( int )g_D.params.x;
	if( radius <= 0 )
	{
		u_Out[pixel] = centerVis;		// denoise off / hard contact: passthrough
		return;
	}

	const float cDepth = t_Depth[pixel].r;
	const float3 cN = DecodeNormal( pixel );
	const float depthSigma = g_D.params.y;
	const float sigma = max( 1.0f, float( radius ) * 0.5f );

	float sum = centerVis;
	float wsum = 1.0f;

	for( int dy = -radius; dy <= radius; dy++ )
	{
		for( int dx = -radius; dx <= radius; dx++ )
		{
			if( dx == 0 && dy == 0 )
			{
				continue;
			}
			const int2 q = pixel + int2( dx, dy );
			// clamp to THIS light's rect: neighbours outside it hold another light's
			// (or stale) visibility and must not be pulled into the penumbra.
			if( q.x < g_D.scissorMin.x || q.y < g_D.scissorMin.y ||
					q.x > g_D.scissorMax.x || q.y > g_D.scissorMax.y )
			{
				continue;
			}
			// spatial gaussian
			const float d2 = float( dx * dx + dy * dy );
			const float ws = exp( -d2 / ( 2.0f * sigma * sigma ) );
			// geometry edge stops (normal sharp, depth relative) - NOT the shadow value
			const float3 nN = DecodeNormal( q );
			const float wn = pow( saturate( dot( cN, nN ) ), 32.0f );
			const float sd = t_Depth[q].r;
			const float wd = exp( -abs( sd - cDepth ) / max( depthSigma, 1e-6f ) );

			const float wt = ws * wn * wd;
			sum += t_RawMask[q] * wt;
			wsum += wt;
		}
	}

	u_Out[pixel] = sum / max( wsum, 1e-6f );
}
