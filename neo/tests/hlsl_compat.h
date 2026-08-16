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

// Minimal HLSL->C++ shim so a shader include (.inc.hlsl) can be compiled and unit-tested as plain C++.
// It defines the vector types, intrinsics, and buffer accessor the soft-shadow coverage function uses,
// with HLSL semantics (component-wise ops, atan2(y,x), saturate = clamp01). The shared coverage source
// is written WITHOUT multi-component swizzles (.xyz/.xy) - only single components .x/.y/.z/.w - so the
// same text is valid HLSL and valid C++. Include this BEFORE the .inc.hlsl in a C++ test.

#ifndef __HLSL_COMPAT_H__
#define __HLSL_COMPAT_H__

#include <cmath>

struct float2 { float x, y; float2() : x( 0 ), y( 0 ) {} float2( float X, float Y ) : x( X ), y( Y ) {} };
struct float3 { float x, y, z; float3() : x( 0 ), y( 0 ), z( 0 ) {} float3( float X, float Y, float Z ) : x( X ), y( Y ), z( Z ) {} };
struct float4 { float x, y, z, w; float4() : x( 0 ), y( 0 ), z( 0 ), w( 0 ) {} float4( float X, float Y, float Z, float W ) : x( X ), y( Y ), z( Z ), w( W ) {} };

// float3/float2 arithmetic (component-wise, HLSL semantics)
inline float3 operator+( float3 a, float3 b ) { return float3( a.x + b.x, a.y + b.y, a.z + b.z ); }
inline float3 operator-( float3 a, float3 b ) { return float3( a.x - b.x, a.y - b.y, a.z - b.z ); }
inline float3 operator*( float3 a, float  s ) { return float3( a.x * s, a.y * s, a.z * s ); }
inline float3 operator*( float  s, float3 a ) { return float3( a.x * s, a.y * s, a.z * s ); }
inline float2 operator+( float2 a, float2 b ) { return float2( a.x + b.x, a.y + b.y ); }
inline float2 operator-( float2 a, float2 b ) { return float2( a.x - b.x, a.y - b.y ); }
inline float2 operator*( float2 a, float  s ) { return float2( a.x * s, a.y * s ); }
inline float2 operator*( float  s, float2 a ) { return float2( a.x * s, a.y * s ); }

inline float  dot( float3 a, float3 b ) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float  dot( float2 a, float2 b ) { return a.x * b.x + a.y * b.y; }
inline float3 cross( float3 a, float3 b ) { return float3( a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x ); }
inline float  length( float3 a ) { return std::sqrt( dot( a, a ) ); }
inline float3 normalize( float3 a ) { float l = length( a ); return l > 0 ? a * ( 1.0f / l ) : a; }

// scalar intrinsics (HLSL names, global scope so the .inc's unqualified calls resolve).
// NaN semantics MATCH THE GPU, not C++ habit: D3D saturate(NaN) = 0, and min/max return the non-NaN
// operand when one input is NaN. The old shim propagated NaN through saturate and max(a,NaN) - so a
// NaN produced inside the shader math was LOUD in the C++ tests but a silent 0-contribution on the
// GPU; the tests were validating a different failure mode than the one that ships (finding F15/F7).
inline float saturate( float x ) { return x > 1.0f ? 1.0f : ( x >= 0.0f ? x : 0.0f ); }		// NaN -> 0 (both compares false)
inline float max( float a, float b ) { if( a != a ) { return b; } if( b != b ) { return a; } return a > b ? a : b; }
inline float min( float a, float b ) { if( a != a ) { return b; } if( b != b ) { return a; } return a < b ? a : b; }
inline int   max( int a, int b ) { return a > b ? a : b; }
inline int   min( int a, int b ) { return a < b ? a : b; }
inline float abs( float x ) { return x < 0.0f ? -x : x; }
inline float sqrt( float x ) { return std::sqrt( x ); }
inline float atan2( float y, float x ) { return std::atan2( y, x ); }

// wave intrinsics: the C++ test build is a single-lane wave, so the collective reductions are the
// identity - semantically exact for lane count 1 (the shader's wave-uniform jumps degrade to the
// per-lane skip, which walks the same records).
inline bool WaveActiveAllTrue( bool b ) { return b; }

// StructuredBuffer<float4> stand-in. The shared coverage function takes this as a trailing parameter
// (SW_EDGEBUF_PARAM) in C++; in HLSL the parameter is absent and t_SoftEdges is the global resource.
struct SoftEdgeBuffer
{
	const float4* p;
	int           n;
	float4 operator[]( int i ) const { return p[i]; }
};

// StructuredBuffer<uint> stand-in for the per-tile record-index lists (SW_TILEBUF_PARAM).
struct SoftTileBuffer
{
	const unsigned int* p;
	int                 n;
	unsigned int operator[]( int i ) const { return p[i]; }
};
typedef unsigned int uint;

#endif // __HLSL_COMPAT_H__
