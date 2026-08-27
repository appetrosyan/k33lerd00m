/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2026 Aleksandr Petrosyan

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").

Doom 3 BFG Edition Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

===========================================================================
*/

// SHARED CONTRACT for the soft-shadow coverage microbench. One synthetic scene of M overlapping convex
// BOXES is emitted in TWO representations of the SAME geometry so the coverage primitives can be compared
// apples-to-apples on the GPU:
//   - edges[] : the WEDGE edge stream (t_SoftEdges layout - header pair + closed silhouette-edge pairs per
//               caster), consumed by SoftShadow_WedgeOcclusion.
//   - boxes[] : the SCANLINE box-corner stream (8 world corners per box, bit convention bit0=x/bit1=y/bit2=z,
//               0=min side), consumed by SoftScan_FillBox.
// plus a fragment grid (receiver positions) that sweeps umbra -> penumbra -> lit. BenchCB mirrors the HLSL
// cbuffer byte-for-byte.
//
// Three agents share this header: this generator (Boxer), the shader (Shea), and the harness (Harold). The
// STRUCT LAYOUTS below are the frozen interface - do not reorder or repad.

#ifndef __SOFTSHADOWCOVERAGEBENCH_SCENE_H__
#define __SOFTSHADOWCOVERAGEBENCH_SCENE_H__

#include <cstdint>

// idVec4 / idList are idlib types in the engine build (pulled by the PCH before this header). The
// dependency-free unit-test build (ID_UNIT_TEST_STANDALONE, no idlib on the include path) gets POD shims
// with the same call surface, so this contract and the generator compile UNCHANGED in both worlds.
#ifdef ID_UNIT_TEST_STANDALONE
#include <vector>
struct idVec4
{
	float x, y, z, w;
	idVec4() : x( 0 ), y( 0 ), z( 0 ), w( 0 ) {}
	idVec4( float X, float Y, float Z, float W ) : x( X ), y( Y ), z( Z ), w( W ) {}
};
template<class T>
class idList
{
public:
	int			Num() const { return ( int )v.size(); }
	int			Append( const T& o ) { v.push_back( o ); return ( int )v.size() - 1; }
	const T&	operator[]( int i ) const { return v[i]; }
	T&			operator[]( int i ) { return v[i]; }
	const T*	Ptr() const { return v.data(); }
	T*			Ptr() { return v.data(); }
	void		Clear() { v.clear(); }
	void		SetNum( int n ) { v.resize( n ); }
private:
	std::vector<T> v;
};
#endif // ID_UNIT_TEST_STANDALONE

// Mirrors the HLSL cbuffer byte layout EXACTLY: float4 + int4 + int4 = 48 bytes. Every member is 4-byte,
// so the struct is tightly packed with no padding (checked by a static_assert in the .cpp).
struct BenchCB
{
	float	swL[3];			float swR;			// float4 swL_R : light origin xyz, disk radius w
	int32_t	method;			int32_t loopN;		int32_t casterCount;	int32_t fragCount;	// int4 ctl
	int32_t	edgesFirstElem;	int32_t edgesN;		int32_t boxesBase;		int32_t boxesCount;	// int4 streams
};

struct BenchScene
{
	idList<idVec4>	edges;	// wedge edge stream (t_SoftEdges: header pair + closed silhouette-edge pairs)
	idList<idVec4>	boxes;	// box-corner stream: 8 corners per box, flat (SoftScan_FillBox convention)
	idList<idVec4>	frags;	// per-fragment receiver pos in .xyz (w unused)
	BenchCB			cb;
};

// Build the synthetic scene for M overlapping boxes (M in {2,4,8,16}). Clears and fills `out`.
void BuildBenchScene( int M, BenchScene& out );

#endif // __SOFTSHADOWCOVERAGEBENCH_SCENE_H__
