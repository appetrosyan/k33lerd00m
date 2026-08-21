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

#include "precompiled.h"
#pragma hdrstop

#include "renderer/RenderCommon.h"
#include "SoftShadowClassify.h"

// cell budget: refuse to build (fall back to the full walk) rather than allocate an unbounded grid for a
// level-spanning light at a fine cell size. Dense is M1-only; M2 switches to a hash grid.
static const int SW_CLASSIFY_MAX_CELLS = 4 << 20;

bool R_SoftClassifyBuild( const float lightOrigin[3], float penumbraRadius,
						  const float domainMin[3], const float domainMax[3],
						  const idVec4* worldTris, int numTris, float cellSize,
						  idList<idVec4>& packed, softClassifyGrid_t& out )
{
	memset( &out, 0, sizeof( out ) );
	if( worldTris == NULL || numTris <= 0 || cellSize < 1e-2f )
	{
		return false;
	}

	const idVec3 Lp( lightOrigin[0], lightOrigin[1], lightOrigin[2] );
	const float  swR = Max( penumbraRadius, 1e-2f );
	const idVec3 mn( domainMin[0], domainMin[1], domainMin[2] );
	const idVec3 mx( domainMax[0], domainMax[1], domainMax[2] );
	const idVec3 ext = mx - mn;
	if( ext.x <= 0.0f || ext.y <= 0.0f || ext.z <= 0.0f )
	{
		return false;
	}

	const int dx = ( int )( ext.x / cellSize ) + 1;
	const int dy = ( int )( ext.y / cellSize ) + 1;
	const int dz = ( int )( ext.z / cellSize ) + 1;
	const int64_t nCells = ( int64_t )dx * dy * dz;
	if( nCells <= 0 || nCells > SW_CLASSIFY_MAX_CELLS )
	{
		return false;					// budget exceeded: caller uses the full walk
	}

	const float tR  = cellSize * 0.8660254f;		// cell half-diagonal (the tile-bin inflation term)
	const float eps = 1e-3f;

	// pack class bytes straight into a uint word array (4 cells/word), reinterpreted to float4 at the end.
	const int nWords = ( int )( ( nCells + 3 ) / 4 );
	idTempArray<uint32_t> words( nWords );
	memset( words.Ptr(), 0, ( size_t )nWords * sizeof( uint32_t ) );	// SW_CLASS_LIT = 0

	// GATHER cull: every cell tests every triangle. Same conservative cone reject as softtile_bin.cs
	// (caster sphere folded into the per-triangle centroid radius). A cell with any survivor is PENUMBRA
	// (walk); none -> LIT. Long-range along the light axis => no spatial shortcut; M1 eats the O(cells*tris).
	for( int iz = 0; iz < dz; iz++ )
	{
		for( int iy = 0; iy < dy; iy++ )
		{
			for( int ix = 0; ix < dx; ix++ )
			{
				const idVec3 Pc = mn + idVec3( ( ix + 0.5f ) * cellSize, ( iy + 0.5f ) * cellSize, ( iz + 0.5f ) * cellSize );
				idVec3 toL = Lp - Pc;
				const float distPL = Max( toL.Length(), 1e-4f );
				const idVec3 nrm = toL * ( 1.0f / distPL );

				bool anySurv = false;
				for( int t = 0; t < numTris; t++ )
				{
					const idVec4& r0 = worldTris[t * 3 + 0];
					const idVec4& r1 = worldTris[t * 3 + 1];
					const idVec4& r2 = worldTris[t * 3 + 2];
					const idVec3 tcen = ( idVec3( r0.x, r0.y, r0.z ) + idVec3( r1.x, r1.y, r1.z ) + idVec3( r2.x, r2.y, r2.z ) ) * ( 1.0f / 3.0f );
					const float  triRad = r1.w + tR;					// centroid radius + cell inflation
					const idVec3 rc = tcen - Pc;
					const float  cd = rc * nrm;
					if( cd + triRad < eps ) { continue; }				// wholly behind the cell
					if( cd - triRad > distPL + tR ) { continue; }		// wholly beyond the light
					const idVec3 perp = rc - nrm * cd;
					const float  coneR = swR * ( cd + triRad ) / Max( distPL - tR, 1e-4f );
					if( perp * perp > ( coneR + triRad ) * ( coneR + triRad ) ) { continue; }	// outside the sample cone
					anySurv = true;
					break;
				}
				if( anySurv )
				{
					const int64_t i = ( ( int64_t )iz * dy + iy ) * dx + ix;
					words[( int )( i >> 2 )] |= ( uint32_t )SW_CLASS_PEN << ( ( i & 3 ) * 8 );
					out.nPen++;
				}
				else
				{
					out.nLit++;
				}
			}
		}
	}

	// reinterpret the word array as float4 (16 class bytes / float4); term CS reads asuint(t_SoftEdges[..]).
	const int nVec4 = ( nWords + 3 ) / 4;
	packed.SetNum( nVec4 );
	memset( packed.Ptr(), 0, ( size_t )nVec4 * sizeof( idVec4 ) );
	memcpy( packed.Ptr(), words.Ptr(), ( size_t )nWords * sizeof( uint32_t ) );

	out.aabbMin[0] = mn.x; out.aabbMin[1] = mn.y; out.aabbMin[2] = mn.z;
	out.cellSize = cellSize;
	out.dims[0] = dx; out.dims[1] = dy; out.dims[2] = dz;
	out.valid = 1;
	return true;
}
