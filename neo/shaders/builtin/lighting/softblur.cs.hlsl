/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 2026 Aleksandr Petrosyan

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").

Doom 3 BFG Edition Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

===========================================================================
*/

// Depth-proportional Gaussian blur of the soft-shadow TERM atlas (r_softShadowTermBlur), a temporal-
// stability pass. The 16-sample coverage quantum shows as a fixed-pattern "fingerprint" that swims
// under camera motion - the umbra of a wide penumbra ramps in 1/16 steps and the steps crawl. A
// Gaussian blur whose radius is proportional to the LOCAL PENUMBRA WIDTH dissolves the steps without
// softening hard shadow edges: the penumbra width is read from the term's own gradient (a wide/shallow
// ramp -> small |grad| -> large radius; a hard edge -> large |grad| -> small radius), so the blur is
// self-scaling and never bleeds a hard contact edge. The gather is clamped to the pixel's own atlas
// SLOT so one light's term never leaks into the neighbour slot. OFF by default (bit-changing; the
// gate anaTerm hash moves); the interaction reads the blurred atlas only when the pass ran.

Texture2D<float>			t_Term	: register( t0 );	// the term atlas the term CS wrote (SRV this pass)
RWTexture2D<float>			u_Blur	: register( u0 );	// blurred output atlas (same layout)

// *INDENT-OFF*
cbuffer c_Blur : register( b0 )
{
	int4	g_rect;		// this light's atlas rect: origin x, y (pixels) + width, height (the gather is clamped here)
	float4	g_blur;		// x = max radius (pixels), y = width->radius scale, z = penumbra lo, w = penumbra hi
};
// *INDENT-ON*

[numthreads( 8, 8, 1 )]
void main( uint3 tid : SV_DispatchThreadID )
{
	if( ( int )tid.x >= g_rect.z || ( int )tid.y >= g_rect.w )
	{
		return;
	}
	const int2 P = int2( g_rect.x + ( int )tid.x, g_rect.y + ( int )tid.y );

	const float c = t_Term.Load( int3( P, 0 ) );

	// RECT bounds: the atlas is a shelf-packed set of scissor-sized rects; keep the gather inside THIS
	// light's rect so a light's penumbra never blurs into a neighbouring light's rect.
	const int sx0 = g_rect.x;
	const int sy0 = g_rect.y;
	const int sx1 = g_rect.x + g_rect.z - 1;
	const int sy1 = g_rect.y + g_rect.w - 1;

	// FLAT-REGION skip: fully-lit (1) and deep-umbra (0) interiors carry no banding; only the penumbra
	// ramp does. If the 4-neighbour term is flat, pass the centre through untouched (cheap, and it keeps
	// hard umbra/lit crisp). penumbra band = ( lo, hi ) from the cvars; sample a wider guard so a pixel
	// just outside the band that borders it still smooths.
	const float up = t_Term.Load( int3( int2( P.x, max( P.y - 1, sy0 ) ), 0 ) );
	const float dn = t_Term.Load( int3( int2( P.x, min( P.y + 1, sy1 ) ), 0 ) );
	const float lf = t_Term.Load( int3( int2( max( P.x - 1, sx0 ), P.y ), 0 ) );
	const float rt = t_Term.Load( int3( int2( min( P.x + 1, sx1 ), P.y ), 0 ) );
	const float gx = ( rt - lf ) * 0.5f;
	const float gy = ( dn - up ) * 0.5f;
	const float grad = sqrt( gx * gx + gy * gy );
	const float flat = max( max( abs( c - up ), abs( c - dn ) ), max( abs( c - lf ), abs( c - rt ) ) );
	const bool  inBand = ( c > g_blur.z && c < g_blur.w ) || ( up > g_blur.z && up < g_blur.w ) ||
						 ( dn > g_blur.z && dn < g_blur.w ) || ( lf > g_blur.z && lf < g_blur.w ) || ( rt > g_blur.z && rt < g_blur.w );
	if( flat < 1e-4f || !inBand )
	{
		u_Blur[ P ] = c;
		return;
	}

	// PENUMBRA-WIDTH radius: the ramp spans ~1/grad pixels; a fraction of that (g_blur.y) is the band
	// spacing to dissolve. Clamp to [1, maxRadius]. A flatter (wider) penumbra gets a bigger radius.
	float radiusF = g_blur.y / max( grad, 1e-3f );
	int   radius  = ( int )clamp( radiusF, 1.0f, g_blur.x );
	const float sigma = max( ( float )radius * 0.5f, 0.5f );
	const float inv2s2 = 1.0f / ( 2.0f * sigma * sigma );

	float acc = 0.0f, wsum = 0.0f;
	for( int dy = -radius; dy <= radius; dy++ )
	{
		const int sy = clamp( P.y + dy, sy0, sy1 );
		for( int dx = -radius; dx <= radius; dx++ )
		{
			const int sx = clamp( P.x + dx, sx0, sx1 );
			const float w = exp( -( float )( dx * dx + dy * dy ) * inv2s2 );
			acc  += t_Term.Load( int3( int2( sx, sy ), 0 ) ) * w;
			wsum += w;
		}
	}
	u_Blur[ P ] = ( wsum > 0.0f ) ? ( acc / wsum ) : c;
}
