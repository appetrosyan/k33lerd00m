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

// GPU skinning to a buffer, for ray tracing animated actors. The regular render
// path skins in the vertex shader and never persists the posed vertices, so a
// BLAS built from a skinned mesh's cache would be its bind pose (T-pose). This
// compute pass reproduces the vertex-shader skinning (skinning.inc.hlsl) exactly
// and writes the posed MODEL-space positions to a buffer that the reflection
// acceleration-structure builder uses as BLAS input (the TLAS instance then
// applies the entity's model->world transform).
//
// Self-contained (no global_inc.hlsl), matching the other RT shaders.

#pragma pack_matrix(row_major)

struct SkinConstants
{
	uint	vertexByteOffset;	// base of this surface's bind-pose idDrawVert run (bytes)
	uint	jointFloat4Base;	// base of this surface's joint matrices (in float4 rows)
	uint	numVerts;
	uint	outVertBase;		// base of this surface's output run (in vertices / float3)
};

// *INDENT-OFF*
ByteAddressBuffer			t_Vertex	: register(t0);	// bind-pose idDrawVert cache
StructuredBuffer<float4>	t_Joints	: register(t1);	// idJointMat rows (3 float4 per joint)
RWByteAddressBuffer			u_Posed		: register(u0);	// posed float3 positions out

cbuffer c_Skin : register(b0)
{
	SkinConstants g_Skin;
};
// *INDENT-ON*

static const uint DRAWVERT_STRIDE = 32;
static const uint DRAWVERT_COLOR_OFFSET = 24;	// joint indices (ubyte4)
static const uint DRAWVERT_COLOR2_OFFSET = 28;	// joint weights (ubyte4)

[numthreads( 64, 1, 1 )]
void main( uint3 dispatchID : SV_DispatchThreadID )
{
	const uint v = dispatchID.x;
	if( v >= g_Skin.numVerts )
	{
		return;
	}

	const uint base = g_Skin.vertexByteOffset + v * DRAWVERT_STRIDE;
	const float3 pos = asfloat( t_Vertex.Load3( base ) );

	const uint idxPacked = t_Vertex.Load( base + DRAWVERT_COLOR_OFFSET );
	const uint wPacked = t_Vertex.Load( base + DRAWVERT_COLOR2_OFFSET );

	// joint index = byte value; each joint is 3 consecutive float4 rows. Matches the
	// vertex shader's int( color * 255.1 * 3.0 ) with color the normalised ubyte.
	const uint4 idx = uint4( idxPacked & 0xffu, ( idxPacked >> 8 ) & 0xffu,
							 ( idxPacked >> 16 ) & 0xffu, ( idxPacked >> 24 ) & 0xffu );
	const float4 w = float4( wPacked & 0xffu, ( wPacked >> 8 ) & 0xffu,
							 ( wPacked >> 16 ) & 0xffu, ( wPacked >> 24 ) & 0xffu ) * ( 1.0f / 255.0f );

	const uint4 j = idx * 3u + g_Skin.jointFloat4Base;

	const float4 matX = t_Joints[j.x + 0] * w.x + t_Joints[j.y + 0] * w.y + t_Joints[j.z + 0] * w.z + t_Joints[j.w + 0] * w.w;
	const float4 matY = t_Joints[j.x + 1] * w.x + t_Joints[j.y + 1] * w.y + t_Joints[j.z + 1] * w.z + t_Joints[j.w + 1] * w.w;
	const float4 matZ = t_Joints[j.x + 2] * w.x + t_Joints[j.y + 2] * w.y + t_Joints[j.z + 2] * w.z + t_Joints[j.w + 2] * w.w;

	const float4 p4 = float4( pos, 1.0f );
	const float3 posed = float3( dot( matX, p4 ), dot( matY, p4 ), dot( matZ, p4 ) );

	u_Posed.Store3( ( g_Skin.outVertBase + v ) * 12u, asuint( posed ) );
}
