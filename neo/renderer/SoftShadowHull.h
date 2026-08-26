/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 2026 Aleksandr Petrosyan

This file is part of the Doom 3 BFG Edition GPL Source Code.

===========================================================================
*/

#ifndef __SOFTSHADOWHULL_H__
#define __SOFTSHADOWHULL_H__

// Brush-recovery soft shadows (docs/softshadow/brush-recovery-implementation-plan.md, Phase 0).
//
// A shadow-casting brush is convex, so its projected occlusion equals the projection of its convex
// hull. dmap (package A) emits one hull per (brush n area) into the .proc `shadowHulls` block; the
// loader (RenderWorld_load.cpp) stores them per area; the worldspawn soft-caster (tr_frontend_addmodels.cpp)
// emits ONE analytic record per hull instead of the area's triangle stream, gated by r_softShadowBrushHulls.
//
// RECORD CONTRACT (must match the shader decode at softterm.cs.hlsl:919-931, shared with the coplanar-poly
// and box paths): an analytic caster's FIRST float4 slot's .w discriminates the primitive - .w > 0 is a
// box (8 corners -> FillBox), .w = -N is an N-vertex convex hull (read N verts' .xyz -> FillHull/FillPoly).
// Verts occupy slots 0..N-1 (.xyz); the slot count is padded to a whole triangle (numTris*3) with the last
// vertex so the stream stays a triple stream. This is the SAME `.w = -N` encoding the coplanar merge uses
// (Interaction.cpp ~:1076-1082) - the shader already decodes it; a brush hull reuses it verbatim.

#define SW_HULL_MAX_VERTS 8			// == SW_POLY_MAX_VERTS in softwedge_coverage.inc.hlsl (shader loop cap)

// In-memory per-area hull (loader-owned). Plain floats so this header is includable by the standalone
// unit-test runner (no idlib link) as well as the engine. World/map space (area world models are identity).
struct areaShadowHull_t
{
	int   numVerts;							// 3..SW_HULL_MAX_VERTS
	float verts[SW_HULL_MAX_VERTS * 3];		// x,y,z per vertex, convex-hull (CCW) order
};

// Pure record emitter - no engine types, so the frontend and the unit test share ONE implementation of
// the `.w = -N` encoding. `verts` = n*3 floats (x,y,z); writes ((n+2)/3)*3 float4 slots into `outSlots`
// (16 floats max headroom for n<=8 -> 3 triangles -> 9 slots). Returns the slot count, or 0 if n is out
// of [3, SW_HULL_MAX_VERTS]. Slot 0's .w = -n (the analytic discriminator); other .w are 0. Slots past
// n-1 pad with the last vertex (the shader clamps its read to n anyway, but keep the stream well-formed).
static inline int SoftHull_EmitRecord( const float* verts, int n, float* outSlots )
{
	if( n < 3 || n > SW_HULL_MAX_VERTS )
	{
		return 0;
	}
	const int numSlots = ( ( n + 2 ) / 3 ) * 3;			// pad to whole triangles
	for( int i = 0; i < numSlots; i++ )
	{
		const int s = ( i < n ) ? i : ( n - 1 );		// pad tail with the last vertex
		outSlots[i * 4 + 0] = verts[s * 3 + 0];
		outSlots[i * 4 + 1] = verts[s * 3 + 1];
		outSlots[i * 4 + 2] = verts[s * 3 + 2];
		outSlots[i * 4 + 3] = 0.0f;
	}
	outSlots[3] = -( float )n;							// slot0 .w = -N  (FillHull discriminator)
	return numSlots;
}

// Per-area hull table lookup (defined in RenderWorld_load.cpp, populated at InitFromMap from the .proc
// `shadowHulls` block). Returns the area's hull array + count, or NULL/0 when the area has no hulls (old
// .proc, non-brush geometry, or fragmentation drop). ponytail: one active render world - the table is a
// file-scope global keyed by area index; a second simultaneous idRenderWorldLocal would alias it.
const areaShadowHull_t* R_GetAreaShadowHulls( int area, int* outNumHulls );

// PER-BRUSH RESIDUALS (brush-recovery package B). dmap partitions each area's shadow brushes into
// hull-emittable ones (the `shadowHulls` block above) and the residual, NON-emittable ones (>SW_HULL_MAX_VERTS
// or otherwise not hull-walkable), which it emits as an ordinary per-area TRIANGLE stream in a parallel .proc
// `shadowResiduals` block. The worldspawn soft-caster streams these residual triangles alongside the hulls, so
// hulls(emittable) + residuals(non-emittable) = every brush ONCE (lossless), while the area's normal per-surface
// stream stays skipped (no double-shadow). This is what the old all-or-nothing completeness rule was avoiding,
// now handled explicitly. Presence-gated: an area with no residuals => no entry => nothing streamed.
//
// Engine-only (idList/idDrawVert): the standalone unit-test runner (ID_UNIT_TEST_STANDALONE) mirrors the
// grammar with std::vector, so it must not see these engine types (SoftShadowResidual_test.cpp).
#ifndef ID_UNIT_TEST_STANDALONE
struct areaResidualTris_t
{
	idList<idDrawVert>	verts;		// MAP space (area world models are identity); only .xyz is read by the caster
	idList<triIndex_t>	indexes;	// triangle list into verts (multiple of 3)
};

// Per-area residual-triangle table lookup (defined in RenderWorld_load.cpp, populated at InitFromMap from the
// .proc `shadowResiduals` block or its .bproc mirror). Returns the area's residual tris, or NULL when the area
// has none. Same one-active-render-world caveat as R_GetAreaShadowHulls: a file-scope global keyed by area.
const areaResidualTris_t* R_GetAreaResidualTris( int area );
#endif // ID_UNIT_TEST_STANDALONE

#endif // __SOFTSHADOWHULL_H__
