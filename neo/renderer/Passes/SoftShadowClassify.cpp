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

// UMBRA certificate for a cell ball (Pc, tR) w.r.t. ONE occluder triangle and the light SPHERE
// (centre Lp, radius swR >= the sample disk, so sphere occlusion implies disk occlusion). This is a
// direct C++ port of the SHIPPED, gate-validated per-tile umbra sentinel (softtile_bin.cs.hlsl
// SW_TILE_UMBRA, the Assarsson inner-penumbra-wedge construction): the cell is umbra iff (1) the tri
// plane separates the whole ball from the whole sphere, and (2) for each edge the ball lies fully
// behind the plane through the edge tangent to the sphere on its far side - then every ray from any
// receiver in the ball to any point of the sphere crosses the tri plane inside all three edges, i.e.
// through the triangle. CONSERVATIVE (single occluder only; a cell shadowed by a UNION of triangles
// but no single one just fails to certify and walks) and never false-positive: any receiver on the
// penumbra side fails an edge test. Returns false = not certified (walk).
static bool SwCellUmbra( const idVec3& Pc, float tR, float swR, const idVec3& Lp, float distPL,
						 const idVec3& v0, const idVec3& v1, const idVec3& v2 )
{
	idVec3 nt = ( v1 - v0 ).Cross( v2 - v0 );
	const float ntl = nt.Length();
	if( ntl <= 1e-6f )
	{
		return false;
	}
	nt *= ( 1.0f / ntl );
	float dL = nt * ( Lp - v0 );
	if( dL < 0.0f ) { nt = -nt; dL = -dL; }			// orient toward the light
	const float dP = nt * ( Pc - v0 );
	const float slack = tR + 2e-4f * ( distPL + tR );	// covers the walk's t > 1e-4 ray floor
	if( !( dL > swR + 1e-3f && dP < -slack ) )
	{
		return false;								// plane does not separate the whole ball from the whole sphere
	}
	const idVec3 V[3] = { v0, v1, v2 };
	for( int e = 0; e < 3; e++ )
	{
		const idVec3& Va = V[e];
		const idVec3& Vc = V[( e + 2 ) % 3];
		const idVec3 ed = V[( e + 1 ) % 3] - Va;
		const float el2 = ed * ed;
		if( el2 < 1e-12f )
		{
			return false;
		}
		const idVec3 u2 = ed * idMath::InvSqrt( el2 );
		const idVec3 w = Lp - Va;
		const idVec3 wp = w - ( w * u2 ) * u2;
		const float W2 = wp * wp;
		if( W2 <= swR * swR + 1e-6f )
		{
			return false;							// sphere touches the edge line: no clean wedge
		}
		const float invW = idMath::InvSqrt( W2 );
		const idVec3 n0 = wp * invW;
		const idVec3 m = u2.Cross( n0 );
		const float sinT = swR * invW;
		const float cosT = idMath::Sqrt( Max( 1.0f - sinT * sinT, 0.0f ) );
		const float sigma = ( m * ( Vc - Va ) >= 0.0f ) ? 1.0f : -1.0f;
		const idVec3 ne = n0 * sinT + m * ( sigma * cosT );
		if( ne * ( Pc - Va ) < tR )
		{
			return false;							// cell ball not fully inside this edge's umbra wedge
		}
	}
	return true;
}

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
				bool umbra = false;
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
					// Umbra test on the FIRST cone survivor only, then break (keeps the LIT/PEN scan
					// O(first-survivor) - scanning ALL tris per penumbra cell to hunt a later umbra
					// certifier regressed the per-frame build ~22x, measured 230->5040 ms/frame on
					// erebus1 cap0004, for ~0 receiver-frag payoff: the 32u cell ball is too coarse to
					// certify receiver surfaces, so full-scan umbra cells are air pockets no fragment
					// lands in. CONSERVATIVE: a cell whose umbra certifier is not its first survivor
					// just walks (correct). The tile-bin's SW_TILE_UMBRA already captures receiver
					// umbra at the tight receiver-AABB grain; this is the classifier's air-cell dual.
					if( SwCellUmbra( Pc, tR, swR, Lp, distPL,
									 idVec3( r0.x, r0.y, r0.z ), idVec3( r1.x, r1.y, r1.z ), idVec3( r2.x, r2.y, r2.z ) ) )
					{
						umbra = true;
					}
					break;
				}
				if( anySurv )
				{
					const int64_t i = ( ( int64_t )iz * dy + iy ) * dx + ix;
					words[( int )( i >> 2 )] |= ( uint32_t )( umbra ? SW_CLASS_UMBRA : SW_CLASS_PEN ) << ( ( i & 3 ) * 8 );
					out.nPen++;					// nPen = all survivors (penumbra + umbra)
					if( umbra ) { out.nUmbra++; }
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
