/*
===========================================================================
Doom 3 BFG Edition GPL Source Code
Copyright (C) 2026 Aleksandr Petrosyan
This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").
It is free software under the GNU GPL v3 (or later).
===========================================================================
*/

// OFFLINE RENDERER for a soft-shadow capture. Rasterises the captured receiver surfaces through the captured
// camera (worldMVP) into a per-pixel world-position + light-index buffer, then shades each covered pixel three
// ways at the SAME point: the analytical wedge, the AAM result (solid umbra via the hard shadow + wedge
// penumbra), and the RT reference (disk-sampled ray-cast). Emits three grayscale shadow-term images. Because
// it reproduces the captured viewpoint, matching the analytical image to the captured shadow-term screenshot
// self-validates the capture (a missing field shows up as a mismatch); and it puts the RT reference and the
// analytical result side by side so a human can judge BOTH (RT is itself only an approximation).

#ifndef __SOFTSHADOWRENDER_H__
#define __SOFTSHADOWRENDER_H__

#include "SoftShadowMesh.h"

namespace swtest
{

// world point -> screen (x,y in [0,W)x[0,H)), returns clip.w (>0 = in front). Fills ndcZ for the depth test.
inline bool SwProject( const float* mvp, float3 P, int W, int H, float& sx, float& sy, float& clipW, float& ndcZ )
{
	float cx = mvp[0] * P.x + mvp[1] * P.y + mvp[2] * P.z + mvp[3];
	float cy = mvp[4] * P.x + mvp[5] * P.y + mvp[6] * P.z + mvp[7];
	float cz = mvp[8] * P.x + mvp[9] * P.y + mvp[10] * P.z + mvp[11];
	float cw = mvp[12] * P.x + mvp[13] * P.y + mvp[14] * P.z + mvp[15];
	if( cw <= 1e-6f ) { return false; }
	clipW = cw; ndcZ = cz / cw;
	sx = ( cx / cw * 0.5f + 0.5f ) * W;
	sy = ( 1.0f - ( cy / cw * 0.5f + 0.5f ) ) * H;
	return true;
}

struct SwGBuffer
{
	int W, H;
	std::vector<float>  depth;		// ndcZ, smaller = nearer
	std::vector<float3> wpos;		// per-pixel world position (perspective-correct)
	std::vector<float3> nrm;		// face normal (unit), for shaded output
	std::vector<float2> st;			// perspective-correct texcoords (v5 captures), for textured output
	std::vector<int>    mat;		// material index (v5) or -1
	std::vector<int>    light;		// receiver's light index, or -1 if empty
	SwGBuffer( int w, int h ) : W( w ), H( h ), depth( w * h, 1e30f ), wpos( w * h ), nrm( w * h ), st( w * h ), mat( w * h, -1 ), light( w * h, -1 ) {}
};

// Rasterise one triangle (world verts A,B,C for light li) into the g-buffer with perspective-correct world
// pos and (optionally) perspective-correct texcoords + material index for textured output (v5 captures).
inline void SwRasterTri( SwGBuffer& g, const float* mvp, float3 A, float3 B, float3 C, int li,
						 float2 stA = float2( 0, 0 ), float2 stB = float2( 0, 0 ), float2 stC = float2( 0, 0 ), int mat = -1 )
{
	float ax, ay, aw, az, bx, by, bw, bz, cx, cy, cw, cz;
	if( !SwProject( mvp, A, g.W, g.H, ax, ay, aw, az ) ) { return; }
	if( !SwProject( mvp, B, g.W, g.H, bx, by, bw, bz ) ) { return; }
	if( !SwProject( mvp, C, g.W, g.H, cx, cy, cw, cz ) ) { return; }
	int minx = ( int )std::floor( std::fmin( ax, std::fmin( bx, cx ) ) ), maxx = ( int )std::ceil( std::fmax( ax, std::fmax( bx, cx ) ) );
	int miny = ( int )std::floor( std::fmin( ay, std::fmin( by, cy ) ) ), maxy = ( int )std::ceil( std::fmax( ay, std::fmax( by, cy ) ) );
	minx = std::max( 0, minx ); miny = std::max( 0, miny ); maxx = std::min( g.W - 1, maxx ); maxy = std::min( g.H - 1, maxy );
	float area = ( bx - ax ) * ( cy - ay ) - ( by - ay ) * ( cx - ax );
	if( std::fabs( area ) < 1e-6f ) { return; }
	float invArea = 1.0f / area;
	float3 faceN = normalize( cross( B - A, C - A ) );
	for( int y = miny; y <= maxy; y++ )
		for( int x = minx; x <= maxx; x++ )
		{
			float px = x + 0.5f, py = y + 0.5f;
			float w0 = ( ( bx - px ) * ( cy - py ) - ( by - py ) * ( cx - px ) ) * invArea;
			float w1 = ( ( cx - px ) * ( ay - py ) - ( cy - py ) * ( ax - px ) ) * invArea;
			float w2 = 1.0f - w0 - w1;
			if( w0 < 0.0f || w1 < 0.0f || w2 < 0.0f ) { continue; }
			float z = w0 * az + w1 * bz + w2 * cz;
			int idx = y * g.W + x;
			if( z >= g.depth[idx] ) { continue; }
			// perspective-correct world position
			float iw = w0 / aw + w1 / bw + w2 / cw;
			if( std::fabs( iw ) < 1e-9f ) { continue; }
			float3 P( ( w0 * A.x / aw + w1 * B.x / bw + w2 * C.x / cw ) / iw,
					  ( w0 * A.y / aw + w1 * B.y / bw + w2 * C.y / cw ) / iw,
					  ( w0 * A.z / aw + w1 * B.z / bw + w2 * C.z / cw ) / iw );
			float2 stP( ( w0 * stA.x / aw + w1 * stB.x / bw + w2 * stC.x / cw ) / iw,
						( w0 * stA.y / aw + w1 * stB.y / bw + w2 * stC.y / cw ) / iw );
			g.depth[idx] = z; g.wpos[idx] = P; g.nrm[idx] = faceN; g.st[idx] = stP; g.mat[idx] = mat; g.light[idx] = li;
		}
}

// write a grayscale PPM (P5). 0..1 -> 0..255.
inline void SwWritePGM( const char* file, int W, int H, const std::vector<float>& v )
{
	std::FILE* f = std::fopen( file, "wb" ); if( !f ) { return; }
	std::fprintf( f, "P5\n%d %d\n255\n", W, H );
	for( int i = 0; i < W * H; i++ ) { unsigned char c = ( unsigned char )( std::fmax( 0.0f, std::fmin( 1.0f, v[i] ) ) * 255.0f + 0.5f ); std::fwrite( &c, 1, 1, f ); }
	std::fclose( f );
}

} // namespace swtest

#endif // __SOFTSHADOWRENDER_H__
