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

// a caster's triangle range in the mesh index stream + its world AABB (ray/frustum culling).
struct CasterRange { uint32_t first, num; float3 lo, hi; };

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

// ============================================================================================
// Shadow-map LOCATOR fidelity (closes the gap the ray-cast locator left): the engine's PCSS
// locator does NOT ray-cast - it projects the receiver into a RASTERISED depth map and blocker-
// searches it. Shadow-map failure modes (depth acne, insufficient bias, back/front-face storage,
// wrong face/UV) are invisible to a ray oracle. This renders the caster mesh from the light EXACTLY
// as ShadowMapPassFast does - FRONT-FACE CULLED, so the stored depth is the occluder's BACK face -
// then runs the identical blocker-fraction gate the shader (interactionSM.ps.hlsl) runs.
// ============================================================================================

// row-major world->clip look-at + D3D perspective (depth 0..1, cw>0 in front) - matches SwProject's
// convention (the same one c.hdr.worldMVP uses), so SwProject works unchanged on the result.
inline void SwLookAtPersp( float3 eye, float3 target, float3 upHint, float fovY, float aspect,
						   float nearZ, float farZ, float* mvp )
{
	float3 z = normalize( eye - target );			// camera looks down -z (view space)
	float3 x = normalize( cross( upHint, z ) );
	float3 y = cross( z, x );
	// view (world->view), row-major
	float V[16] =
	{
		x.x, x.y, x.z, -dot( x, eye ),
		y.x, y.y, y.z, -dot( y, eye ),
		z.x, z.y, z.z, -dot( z, eye ),
		0, 0, 0, 1
	};
	float g = 1.0f / std::tan( fovY * 0.5f );
	float A = farZ / ( nearZ - farZ );
	float B = nearZ * farZ / ( nearZ - farZ );
	float P[16] =
	{
		g / aspect, 0, 0, 0,
		0, g, 0, 0,
		0, 0, A, B,
		0, 0, -1, 0
	};
	for( int r = 0; r < 4; r++ )
		for( int col = 0; col < 4; col++ )
		{
			float s = 0;
			for( int k = 0; k < 4; k++ ) { s += P[r * 4 + k] * V[k * 4 + col]; }
			mvp[r * 4 + col] = s;
		}
}

struct SwShadowMap
{
	int res = 0;
	std::vector<float> depth;		// window z in [0,1], 1 = far/empty
	float mvp[16];					// world -> clip (SwProject-compatible)
	float3 eye;
};

// Render the caster mesh into a single square depth map from the light toward `target`. FRONT-FACE
// CULLED against the light (keep triangles whose outward normal faces AWAY from the light => stored
// depth is the BACK face), reproducing GLS_CULL_FRONTSIDED. `bias` pushes stored depth away from the
// light (the engine's GL_PolygonOffset on the shadow pass).
inline SwShadowMap SwRenderShadowMap( float3 L, float3 target, const float* verts, const uint32_t* idx,
									  const std::vector<CasterRange>& casters, int res, float bias = 0.0f )
{
	SwShadowMap sm;
	sm.res = res;
	sm.eye = L;
	sm.depth.assign( ( size_t )res * res, 1e30f );
	// frustum bounds from the caster AABBs (radius around the light->target axis)
	float3 up( 0, 0, 1 );
	if( std::fabs( dot( normalize( target - L ), up ) ) > 0.95f ) { up = float3( 0, 1, 0 ); }
	SwLookAtPersp( L, target, up, 1.5708f /*90deg*/, 1.0f, 1.0f, 8192.0f, sm.mvp );
	for( const CasterRange& cr : casters )
		for( uint32_t t = cr.first; t + 2 < cr.first + cr.num; t += 3 )
		{
			uint32_t ia = idx[t], ib = idx[t + 1], ic = idx[t + 2];
			float3 A( verts[ia * 3 + 0], verts[ia * 3 + 1], verts[ia * 3 + 2] );
			float3 B( verts[ib * 3 + 0], verts[ib * 3 + 1], verts[ib * 3 + 2] );
			float3 C( verts[ic * 3 + 0], verts[ic * 3 + 1], verts[ic * 3 + 2] );
			float3 fN = cross( B - A, C - A );
			float3 ctr( ( A.x + B.x + C.x ) / 3, ( A.y + B.y + C.y ) / 3, ( A.z + B.z + C.z ) / 3 );
			if( dot( fN, ctr - L ) <= 0.0f ) { continue; }	// front-facing to light -> culled (keep back faces)
			float ax, ay, aw, az, bx, by, bw, bz, cx, cy, cw, cz;
			if( !SwProject( sm.mvp, A, res, res, ax, ay, aw, az ) ) { continue; }
			if( !SwProject( sm.mvp, B, res, res, bx, by, bw, bz ) ) { continue; }
			if( !SwProject( sm.mvp, C, res, res, cx, cy, cw, cz ) ) { continue; }
			int minx = std::max( 0, ( int )std::floor( std::fmin( ax, std::fmin( bx, cx ) ) ) );
			int maxx = std::min( res - 1, ( int )std::ceil( std::fmax( ax, std::fmax( bx, cx ) ) ) );
			int miny = std::max( 0, ( int )std::floor( std::fmin( ay, std::fmin( by, cy ) ) ) );
			int maxy = std::min( res - 1, ( int )std::ceil( std::fmax( ay, std::fmax( by, cy ) ) ) );
			float area = ( bx - ax ) * ( cy - ay ) - ( by - ay ) * ( cx - ax );
			if( std::fabs( area ) < 1e-9f ) { continue; }
			float inv = 1.0f / area;
			for( int y = miny; y <= maxy; y++ )
				for( int x = minx; x <= maxx; x++ )
				{
					float px = x + 0.5f, py = y + 0.5f;
					float w0 = ( ( bx - px ) * ( cy - py ) - ( by - py ) * ( cx - px ) ) * inv;
					float w1 = ( ( cx - px ) * ( ay - py ) - ( cy - py ) * ( ax - px ) ) * inv;
					float w2 = 1.0f - w0 - w1;
					if( w0 < 0.0f || w1 < 0.0f || w2 < 0.0f ) { continue; }
					float z = w0 * az + w1 * bz + w2 * cz + bias;
					size_t di = ( size_t )y * res + x;
					if( z < sm.depth[di] ) { sm.depth[di] = z; }
				}
		}
	return sm;
}

// The EXACT shader locator (interactionSM.ps.hlsl USE_SHADOW_ATLAS branch): project P, 16-tap Vogel
// blocker search, fraction gate. Returns hits/16, or -1 if P falls outside this map's frustum.
inline float SwLocatorBlockerFrac( float3 P, const SwShadowMap& sm, float pcssScale, float recvBias = 0.999f )
{
	float sx, sy, sw, sz;
	if( !SwProject( sm.mvp, P, sm.res, sm.res, sx, sy, sw, sz ) ) { return -1.0f; }
	if( sx < 0 || sy < 0 || sx >= sm.res || sy >= sm.res ) { return -1.0f; }
	float recvZ = sz * recvBias;
	float searchPx = pcssScale;					// search radius in texels (pcssScale is a texel-count in the shader)
	const float golden = 2.4f, phi = 0.37f;
	float hits = 0.0f;
	for( int bi = 0; bi < 16; bi++ )
	{
		float rr = std::sqrt( ( bi + 0.5f ) / 16.0f );
		float th = bi * golden + phi;
		int tx = ( int )( sx + std::cos( th ) * rr * searchPx );
		int ty = ( int )( sy + std::sin( th ) * rr * searchPx );
		if( tx < 0 || ty < 0 || tx >= sm.res || ty >= sm.res ) { continue; }
		float bd = sm.depth[( size_t )ty * sm.res + tx];
		if( bd < recvZ ) { hits += 1.0f; }
	}
	return hits / 16.0f;
}

} // namespace swtest

#endif // __SOFTSHADOWRENDER_H__
