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

// HDR 2D composite (HDR-3). In HDR output mode the 2D UI (HUD, menus, cursor,
// SWF GUIs) is rendered into an isolated buffer in sRGB/gamma space, then this
// pass composites it over the already-linear scRGB scene in ldrImage: the UI is
// un-premultiplied, converted sRGB -> linear, scaled to paper-white nits, and
// alpha-composited in linear light. Without this the gamma-space UI values read
// as linear on the scRGB swapchain and look bright and washed out.
//
// Runs in place: ldrImage holds the linear scene on input and the composited
// result on output. Only enabled when r_hdrOutput is set; the SDR path never
// binds this pass.

#pragma pack_matrix(row_major)

struct HdrGuiConstants
{
	float	paperScale;		// paper-white nits / 80 (scRGB white = 1.0 = 80 nits)
	int		width;
	int		height;
	int		pad;
};

// *INDENT-OFF*
Texture2D<float4>	t_Gui		: register(t0);	// isolated 2D UI, premultiplied sRGB, a = coverage
RWTexture2D<float4>	u_Ldr		: register(u0);	// linear scRGB scene in / composited out

cbuffer c_HdrGui : register(b0)
{
	HdrGuiConstants g_HdrGui;
};
// *INDENT-ON*

// Accurate sRGB -> linear (per component).
float3 SRGBToLinear( float3 c )
{
	float3 lo = c / 12.92f;
	float3 hi = pow( max( ( c + 0.055f ) / 1.055f, 0.0f ), 2.4f );
	return float3( c.x <= 0.04045f ? lo.x : hi.x,
				   c.y <= 0.04045f ? lo.y : hi.y,
				   c.z <= 0.04045f ? lo.z : hi.z );
}

[numthreads( 8, 8, 1 )]
void main( uint3 dispatchID : SV_DispatchThreadID )
{
	const int2 pixel = int2( dispatchID.xy );
	if( pixel.x >= g_HdrGui.width || pixel.y >= g_HdrGui.height )
	{
		return;
	}

	const float4 gui = t_Gui[pixel];		// PREMULTIPLIED sRGB, a = accumulated coverage
	const float3 bg = u_Ldr[pixel].rgb;		// linear scRGB scene

	// Composite the PREMULTIPLIED UI directly - do NOT un-premultiply. Dividing by a
	// explodes anti-aliased edges and additive/glow pixels (tiny a, non-zero rgb),
	// which shows up as haloes around glyphs and over-bright colour overflow. Instead
	// linearise the premultiplied colour as-is and add it over the scene weighted by
	// (1 - a); additive UI (a ~ 0, rgb > 0) then stays additive instead of blowing up.
	const float3 uiLin = SRGBToLinear( gui.rgb ) * g_HdrGui.paperScale;

	const float3 outc = bg * ( 1.0f - gui.a ) + uiLin;
	u_Ldr[pixel] = float4( outc, 1.0f );
}
