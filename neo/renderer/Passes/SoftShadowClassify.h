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

#ifndef __SOFT_SHADOW_CLASSIFY_H__
#define __SOFT_SHADOW_CLASSIFY_H__

// Analytic soft shadows: per-light WORLD-CELL umbra/lit/penumbra CLASSIFIER. Measurement chain (plan
// noble-sniffing-rose / [[softshadow-perf-status]]): the per-fragment coverage walk spends 81-86% of its
// work in LIT fragments that walk ~90 occluder triangles and block NOTHING - the 16x16 screen tile bin is
// too coarse to prove them lit, so they walk. A conservative world-cell classifier proves most of them
// lit up front (16-32u cells capture 82-91% of the lit-work) and short-circuits:
//   LIT      (no occluder penumbra-cone reaches the cell)  -> integral is 0, skip the walk.
//   PENUMBRA (some occluder cone overlaps)                 -> walk exactly as today (also the out-of-grid
//                                                             default: unclassified is never skipped).
//   UMBRA    (M2)                                          -> integral saturates to 1, skip.
// Uses the SAME conservative cull the tile bin uses (per-triangle centroid cone vs the cell AABB, the cone
// already encoding the light-disk radius), so a LIT cell provably blocks no disk-sample ray for ANY
// receiver in it. Exact and lossless: lit is all-or-nothing (rotation-independent, no dither concession).
//
// M1 (validation): a DENSE class grid (byte/cell) built on the CPU per light, packed 16 classes per float4
// and ridden into the vertex-cache joint buffer (read back via t_SoftEdges in the term CS - no new
// binding). Rebuilt every frame; the term CS skips LIT fragments. M2 adds the static/dynamic split, a HASH
// grid (dense measured 20-40x empty air), cached penumbra cluster lists, UMBRA, and move-invalidated
// per-light caching.

class idVec4;

// cell classes (byte grid, packed into the float4 stream). Keep in sync with softterm.cs.hlsl.
static const unsigned char SW_CLASS_LIT = 0;	// integral 0, skip the walk
static const unsigned char SW_CLASS_PEN = 1;	// walk exactly (and the out-of-grid default)
static const unsigned char SW_CLASS_UMBRA = 2;	// integral saturates to 1, skip (term 0). Conservative
												// single-occluder wedge certificate (the tile-bin's proven
												// SW_TILE_UMBRA math, ported per-cell): if ONE triangle's
												// inner-penumbra wedge fully contains the cell ball, every
												// disk-sample ray of every receiver in the cell hits it.

struct softClassifyGrid_t
{
	float	aabbMin[3];		// grid origin (world)
	float	cellSize;
	int		dims[3];		// cells per axis
	int		valid;			// 0 = build failed / disabled -> full walk
	int		nLit, nPen;		// diagnostic: cell-class counts (nPen counts UMBRA cells too)
	int		nUmbra;			// diagnostic: umbra-certified cells (subset of nPen; walk skipped, term 0)
};

// CPU-build the class grid for one light. worldTris = 3 float4/triangle (r0=v0.xyz+v0rad, r1=v1.xyz+
// centroidRad, r2=v2.xyz), world space - exactly the light's assembled soft tri stream. domainMin/Max is
// the grid AABB (light world bounds; cells outside are unclassified -> walked). Appends the packed class
// grid (16 bytes/float4) to 'packed' and fills 'out'. Returns false (out.valid=0 -> full walk) on
// degenerate input or if the cell budget is exceeded.
bool R_SoftClassifyBuild( const float lightOrigin[3], float penumbraRadius,
						  const float domainMin[3], const float domainMax[3],
						  const idVec4* worldTris, int numTris, float cellSize,
						  idList<idVec4>& packed, softClassifyGrid_t& out );

#endif // __SOFT_SHADOW_CLASSIFY_H__
