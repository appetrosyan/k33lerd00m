/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").

Doom 3 BFG Edition Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 BFG Edition Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 BFG Edition Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 BFG Edition Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 BFG Edition Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/

#include "precompiled.h"
#pragma hdrstop

#include "RenderCommon.h"

/*
===========================================================================

idInteraction implementation

===========================================================================
*/

/*
================
R_CalcInteractionFacing

Determines which triangles of the surface are facing towards the light origin.

The facing array should be allocated with one extra index than
the number of surface triangles, which will be used to handle dangling
edge silhouettes.
================
*/
void R_CalcInteractionFacing( const idRenderEntityLocal* ent, const srfTriangles_t* tri, const idRenderLightLocal* light, srfCullInfo_t& cullInfo )
{
	SCOPED_PROFILE_EVENT( "R_CalcInteractionFacing" );

	if( cullInfo.facing != NULL )
	{
		return;
	}

	idVec3 localLightOrigin;
	R_GlobalPointToLocal( ent->modelMatrix, light->globalLightOrigin, localLightOrigin );

	const int numFaces = tri->numIndexes / 3;
	cullInfo.facing = ( byte* ) R_StaticAlloc( ( numFaces + 1 ) * sizeof( cullInfo.facing[0] ), TAG_RENDER_INTERACTION );

	// animated casters carry bind-pose 'verts' (GPU skinning) but a CPU-posed override for shadows
	const idDrawVert* verts = tri->posedShadowVerts != NULL ? tri->posedShadowVerts : tri->verts;

	// exact geometric cull against face
	for( int i = 0, face = 0; i < tri->numIndexes; i += 3, face++ )
	{
		const idDrawVert& v0 = verts[tri->indexes[i + 0]];
		const idDrawVert& v1 = verts[tri->indexes[i + 1]];
		const idDrawVert& v2 = verts[tri->indexes[i + 2]];

		const idPlane plane( v0.xyz, v1.xyz, v2.xyz );
		const float d = plane.Distance( localLightOrigin );

		cullInfo.facing[face] = ( d >= 0.0f );
	}
	cullInfo.facing[numFaces] = 1;	// for dangling edges to reference
}

/*
=====================
R_CalcInteractionCullBits

We want to cull a little on the sloppy side, because the pre-clipping
of geometry to the lights in dmap will give many cases that are right
at the border. We throw things out on the border, because if any one
vertex is clearly inside, the entire triangle will be accepted.
=====================
*/
void R_CalcInteractionCullBits( const idRenderEntityLocal* ent, const srfTriangles_t* tri, const idRenderLightLocal* light, srfCullInfo_t& cullInfo )
{
	SCOPED_PROFILE_EVENT( "R_CalcInteractionCullBits" );

	if( cullInfo.cullBits != NULL )
	{
		return;
	}

	idPlane frustumPlanes[6];
	idRenderMatrix::GetFrustumPlanes( frustumPlanes, light->baseLightProject, true, true );

	int frontBits = 0;

	// cull the triangle surface bounding box
	for( int i = 0; i < 6; i++ )
	{
		R_GlobalPlaneToLocal( ent->modelMatrix, frustumPlanes[i], cullInfo.localClipPlanes[i] );

		// get front bits for the whole surface
		if( tri->bounds.PlaneDistance( cullInfo.localClipPlanes[i] ) >= LIGHT_CLIP_EPSILON )
		{
			frontBits |= 1 << i;
		}
	}

	// if the surface is completely inside the light frustum
	if( frontBits == ( ( 1 << 6 ) - 1 ) )
	{
		cullInfo.cullBits = LIGHT_CULL_ALL_FRONT;
		return;
	}

	cullInfo.cullBits = ( byte* ) R_StaticAlloc( tri->numVerts * sizeof( cullInfo.cullBits[0] ), TAG_RENDER_INTERACTION );
	memset( cullInfo.cullBits, 0, tri->numVerts * sizeof( cullInfo.cullBits[0] ) );

	// animated casters carry bind-pose 'verts' (GPU skinning) but a CPU-posed override for shadows
	const idDrawVert* verts = tri->posedShadowVerts != NULL ? tri->posedShadowVerts : tri->verts;

	for( int i = 0; i < 6; i++ )
	{
		// if completely infront of this clipping plane
		if( frontBits & ( 1 << i ) )
		{
			continue;
		}
		for( int j = 0; j < tri->numVerts; j++ )
		{
			float d = cullInfo.localClipPlanes[i].Distance( verts[j].xyz );
			cullInfo.cullBits[j] |= ( d < LIGHT_CLIP_EPSILON ) << i;
		}
	}
}

/*
================
R_FreeInteractionCullInfo
================
*/
void R_FreeInteractionCullInfo( srfCullInfo_t& cullInfo )
{
	if( cullInfo.facing != NULL )
	{
		R_StaticFree( cullInfo.facing );
		cullInfo.facing = NULL;
	}
	if( cullInfo.cullBits != NULL )
	{
		if( cullInfo.cullBits != LIGHT_CULL_ALL_FRONT )
		{
			R_StaticFree( cullInfo.cullBits );
		}
		cullInfo.cullBits = NULL;
	}
}

/*
=====================
R_CreateInteractionShadowVolume

Creates a static shadow volume triangle surface (indexes into the doubled
idShadowVert cache of the ambient surface). Restored verbatim from DOOM-3-BFG.
=====================
*/
srfTriangles_t* R_CreateInteractionShadowVolume( const idRenderEntityLocal* ent,
		const srfTriangles_t* tri, const idRenderLightLocal* light )
{
	SCOPED_PROFILE_EVENT( "R_CreateInteractionShadowVolume" );

	srfCullInfo_t cullInfo = {};

	R_CalcInteractionFacing( ent, tri, light, cullInfo );
	R_CalcInteractionCullBits( ent, tri, light, cullInfo );

	int numFaces = tri->numIndexes / 3;
	int	numShadowingFaces = 0;
	const byte* facing = cullInfo.facing;

	// if all the triangles are inside the light frustum
	if( cullInfo.cullBits == LIGHT_CULL_ALL_FRONT )
	{
		// count the number of shadowing faces
		for( int i = 0; i < numFaces; i++ )
		{
			numShadowingFaces += facing[i];
		}
		numShadowingFaces = numFaces - numShadowingFaces;
	}
	else
	{
		// make all triangles that are outside the light frustum "facing", so they won't cast shadows
		const triIndex_t* indexes = tri->indexes;
		byte* modifyFacing = cullInfo.facing;
		const byte* cullBits = cullInfo.cullBits;
		for( int i = 0, j = 0; i < tri->numIndexes; i += 3, j++ )
		{
			if( !modifyFacing[j] )
			{
				int	i1 = indexes[i + 0];
				int	i2 = indexes[i + 1];
				int	i3 = indexes[i + 2];
				if( cullBits[i1] & cullBits[i2] & cullBits[i3] )
				{
					modifyFacing[j] = 1;
				}
				else
				{
					numShadowingFaces++;
				}
			}
		}
	}

	if( !numShadowingFaces )
	{
		// no faces are inside the light frustum and still facing the right way
		R_FreeInteractionCullInfo( cullInfo );
		return NULL;
	}

	// shadowVerts will be NULL on these surfaces, so the shadowVerts will be taken from the ambient surface
	srfTriangles_t* newTri = R_AllocStaticTriSurf();

	newTri->numVerts = tri->numVerts * 2;

	// alloc the max possible size
	R_AllocStaticTriSurfIndexes( newTri, ( numShadowingFaces + tri->numSilEdges ) * 6 );
	triIndex_t* tempIndexes = newTri->indexes;
	triIndex_t* shadowIndexes = newTri->indexes;

	// create new triangles along sil planes
	const silEdge_t* sil = tri->silEdges;
	for( int i = tri->numSilEdges; i > 0; i--, sil++ )
	{
		int f1 = facing[sil->p1];
		int f2 = facing[sil->p2];

		if( !( f1 ^ f2 ) )
		{
			continue;
		}

		int v1 = sil->v1 << 1;
		int v2 = sil->v2 << 1;

		// set the two triangle winding orders based on facing
		// without using a poorly-predictable branch
		shadowIndexes[0] = v1;
		shadowIndexes[1] = v2 ^ f1;
		shadowIndexes[2] = v2 ^ f2;
		shadowIndexes[3] = v1 ^ f2;
		shadowIndexes[4] = v1 ^ f1;
		shadowIndexes[5] = v2 ^ 1;

		shadowIndexes += 6;
	}

	int	numShadowIndexes = shadowIndexes - tempIndexes;

	// we aren't bothering to separate front and back caps on these
	newTri->numIndexes = newTri->numShadowIndexesNoFrontCaps = numShadowIndexes + numShadowingFaces * 6;
	newTri->numShadowIndexesNoCaps = numShadowIndexes;
	newTri->shadowCapPlaneBits = SHADOW_CAP_INFINITE;

	// these have no effect, because they extend to infinity
	newTri->bounds.Clear();

	// put some faces on the model and some on the distant projection
	const triIndex_t* indexes = tri->indexes;
	shadowIndexes = newTri->indexes + numShadowIndexes;
	for( int i = 0, j = 0; i < tri->numIndexes; i += 3, j++ )
	{
		if( facing[j] )
		{
			continue;
		}

		int i0 = indexes[i + 0] << 1;
		int i1 = indexes[i + 1] << 1;
		int i2 = indexes[i + 2] << 1;

		shadowIndexes[0] = i2;
		shadowIndexes[1] = i1;
		shadowIndexes[2] = i0;
		shadowIndexes[3] = i0 ^ 1;
		shadowIndexes[4] = i1 ^ 1;
		shadowIndexes[5] = i2 ^ 1;

		shadowIndexes += 6;
	}

	R_FreeInteractionCullInfo( cullInfo );

	return newTri;
}

/*
=====================
R_CreatePenumbraWedges

Analytic soft shadow volumes: for each silhouette edge emit a penumbra wedge - a prism from the
edge, extruded away from the light and flared by the penumbra width, that conservatively covers the
edge's penumbra band on the receivers. Each vertex carries its clip-space position (model->clip, so
the vertex shader is a passthrough) and the edge endpoints in WORLD space (model->world) that the
pixel shader needs to evaluate the analytic coverage. The pixel shader clamps coverage to zero
outside the band, so the geometry only has to be conservative, not exact.

Frame-allocated; outputs are NULL / zero if the caster has no silhouette against this light.
=====================
*/
// Diagnostic (r_softShadowDebugHash): order-independent, world-space-only fingerprint of a frame's
// penumbra-wedge geometry. Reset each frame before model processing (R_AddModels), compared after.
// A static scene must reproduce the same total every frame; drift => non-deterministic generation.
idSysInterlockedInteger tr_softWedgeGeomHash;
// Companion count of emitted silhouette edges this frame. Disambiguates a fingerprint drift: if the
// count changes the SET of contributing edges changed (view culling / caster set); if the count holds
// but the hash drifts the per-edge WORLD values changed (pose / matrix), camera-independent by design.
idSysInterlockedInteger tr_softWedgeEdgeCount;

void R_CreatePenumbraWedges( const idRenderEntityLocal* ent, const srfTriangles_t* tri,
		const idRenderLightLocal* light, float penumbraSize,
		const idRenderMatrix& modelToClip, const float* modelToWorld,
		idSoftWedgeVert** outVerts, triIndex_t** outIndexes, int* outNumVerts, int* outNumIndexes )
{
	*outVerts = NULL;
	*outIndexes = NULL;
	*outNumVerts = 0;
	*outNumIndexes = 0;

	if( tri->silEdges == NULL || tri->numSilEdges == 0 || penumbraSize <= 0.0f )
	{
		return;
	}

	srfCullInfo_t cullInfo = {};
	R_CalcInteractionFacing( ent, tri, light, cullInfo );
	const byte* facing = cullInfo.facing;

	idVec3 localLight;
	R_GlobalPointToLocal( ent->modelMatrix, light->globalLightOrigin, localLight );

	const idDrawVert* verts = tri->posedShadowVerts != NULL ? tri->posedShadowVerts : tri->verts;

	int numSil = 0;
	for( int i = 0; i < tri->numSilEdges; i++ )
	{
		if( facing[tri->silEdges[i].p1] ^ facing[tri->silEdges[i].p2] )
		{
			numSil++;
		}
	}
	if( numSil == 0 )
	{
		R_FreeInteractionCullInfo( cullInfo );
		return;
	}

	const int vertsPerEdge = 6;
	const int indexesPerEdge = 24;		// 8 triangles forming the prism boundary
	idSoftWedgeVert* wv = ( idSoftWedgeVert* )R_FrameAlloc( numSil * vertsPerEdge * sizeof( idSoftWedgeVert ), FRAME_ALLOC_UNKNOWN );
	triIndex_t* wi = ( triIndex_t* )R_FrameAlloc( numSil * indexesPerEdge * sizeof( triIndex_t ), FRAME_ALLOC_UNKNOWN );

	// How far to extrude the wedge along the light rays (model units). The pixel shader now gates
	// coverage analytically (occlusion test + umbra plane), so the geometry only has to conservatively
	// COVER the penumbra's screen pixels - it does not need to reach 4000 units. Bounding it to the
	// light's actual reach is what stops the "wedges flicker in and out" pop: a fixed 4000-unit
	// extrusion swings the far cap behind the camera, the near plane clips the back face this pass
	// culls to, and the wedge vanishes for a frame. Receivers only exist inside the light volume, so
	// extrude no farther than its diagonal (with a small floor so tiny lights still get a band).
	const float lightReach = 2.0f * light->globalLightBounds.GetRadius();
	const float wedgeFar = Max( lightReach, penumbraSize * 4.0f );

	// prism boundary: verts 0=E0 1=E1 2=inner0 3=outer0 4=inner1 5=outer1
	static const triIndex_t prism[24] =
	{
		0, 1, 4,  0, 4, 2,		// inner side
		0, 3, 5,  0, 5, 1,		// outer side
		2, 4, 5,  2, 5, 3,		// far cap
		0, 2, 3,				// end at E0
		1, 5, 4					// end at E1
	};

	int v = 0, idx = 0;
	for( int i = 0; i < tri->numSilEdges; i++ )
	{
		const silEdge_t& sil = tri->silEdges[i];
		if( !( facing[sil.p1] ^ facing[sil.p2] ) )
		{
			continue;
		}

		idVec3 E0 = verts[sil.v1].xyz;
		idVec3 E1 = verts[sil.v2].xyz;

		// Consistent silhouette winding. The stencil path orders each sil quad by which adjacent
		// face is lit (see the v2^f1 / v1^f2 index tricks above); we must match it, so the pixel
		// shader's plane normal cross(edge0-P, edge1-P) points to the SAME side of every silhouette
		// edge. The coverage delta is antisymmetric in that normal (flipping the edge negates it),
		// so an inconsistent order gives each edge a random sign -> the accumulator is noise ->
		// the mask flickers across the whole shadow. facing[sil.p1] picks the canonical orientation.
		if( !facing[sil.p1] )
		{
			idVec3 tmp = E0;
			E0 = E1;
			E1 = tmp;
		}

		idVec3 d0 = E0 - localLight;
		idVec3 d1 = E1 - localLight;
		const float dist0 = d0.Normalize();
		const float dist1 = d1.Normalize();

		// Skip silhouette edges sitting right on top of the light (typically the light-fixture geometry
		// itself). Closer than a light radius the penumbra model is undefined and the extrusion
		// direction normalize(E-light) is hypersensitive to any light motion - the flicker that stays
		// pinned to the light sources. A real occluder is at least a light-radius away.
		if( dist0 < penumbraSize || dist1 < penumbraSize )
		{
			continue;
		}

		idVec3 edgeDir = E1 - E0;
		edgeDir.Normalize();
		idVec3 perp0 = edgeDir.Cross( d0 );
		perp0.Normalize();
		idVec3 perp1 = edgeDir.Cross( d1 );
		perp1.Normalize();

		// Penumbra flare grows with distance along the ray (contact hardening). Clamp it to the
		// extrusion length: for a silhouette edge very close to the light the raw 1/dist term
		// explodes into a far cap thousands of units to the side, which projects wildly and pops.
		const float w0 = Min( penumbraSize * wedgeFar / Max( dist0, 1.0f ), wedgeFar );
		const float w1 = Min( penumbraSize * wedgeFar / Max( dist1, 1.0f ), wedgeFar );

		const idVec3 cf0 = E0 + d0 * wedgeFar;
		const idVec3 cf1 = E1 + d1 * wedgeFar;
		const idVec3 mpos[6] =
		{
			E0, E1,
			cf0 + perp0 * w0, cf0 - perp0 * w0,
			cf1 + perp1 * w1, cf1 - perp1 * w1
		};

		idVec3 worldE0, worldE1;
		R_LocalPointToGlobal( modelToWorld, E0, worldE0 );
		R_LocalPointToGlobal( modelToWorld, E1, worldE1 );

		// Continuous silhouette weight. The silhouette test is binary (one adjacent face lit, the other
		// not), so an edge snaps in and out of the set as it crosses grazing - and its whole penumbra
		// wedge pops with it. Weight the wedge by how firmly this edge is a silhouette: |plane distance
		// from the light| / (light->edge distance) is ~sin(grazing angle) for each adjacent face; take
		// the smaller (the face nearer to flipping) and ramp 0..1 over the last ~9 degrees. An edge just
		// entering the silhouette contributes ~0 and grows in smoothly instead of appearing all at once.
		extern idCVar r_softShadowWedgeFade;
		float silWeight = 1.0f;
		const int numFaces = tri->numIndexes / 3;
		if( r_softShadowWedgeFade.GetBool() && sil.p1 < numFaces && sil.p2 < numFaces )
		{
			const idPlane pa( verts[tri->indexes[sil.p1 * 3 + 0]].xyz, verts[tri->indexes[sil.p1 * 3 + 1]].xyz, verts[tri->indexes[sil.p1 * 3 + 2]].xyz );
			const idPlane pb( verts[tri->indexes[sil.p2 * 3 + 0]].xyz, verts[tri->indexes[sil.p2 * 3 + 1]].xyz, verts[tri->indexes[sil.p2 * 3 + 2]].xyz );
			const float distL = 0.5f * ( dist0 + dist1 );
			const float graze = Min( idMath::Fabs( pa.Distance( localLight ) ), idMath::Fabs( pb.Distance( localLight ) ) ) / Max( distL, 1.0f );
			silWeight = idMath::ClampFloat( 0.0f, 1.0f, graze / 0.15f );
		}

		for( int k = 0; k < 6; k++ )
		{
			idVec4 clip;
			modelToClip.TransformPoint( mpos[k], clip );
			wv[v + k].clipPos = clip;
			wv[v + k].edge0 = idVec4( worldE0.x, worldE0.y, worldE0.z, silWeight );
			wv[v + k].edge1 = idVec4( worldE1.x, worldE1.y, worldE1.z, 0.0f );
		}

		// Diagnostic fingerprint: fold this edge's WORLD-space data (camera excluded) into the frame
		// total. Order-independent (a sum), so parallel generation order can't change it. A static
		// scene must reproduce the same total every frame; drift = non-deterministic generation.
		{
			const float fp[7] = { worldE0.x, worldE0.y, worldE0.z, worldE1.x, worldE1.y, worldE1.z, silWeight };
			const byte* fpb = ( const byte* )fp;
			uint32 h = 2166136261u;
			for( int b = 0; b < ( int )sizeof( fp ); b++ )
			{
				h = ( h ^ fpb[b] ) * 16777619u;
			}
			tr_softWedgeGeomHash.Add( ( int )h );
			tr_softWedgeEdgeCount.Add( 1 );
		}

		for( int k = 0; k < 24; k++ )
		{
			wi[idx + k] = ( triIndex_t )( v + prism[k] );
		}

		v += 6;
		idx += 24;
	}

	R_FreeInteractionCullInfo( cullInfo );

	// Diagnostic (r_softShadowDebugHash 2): identify which caster/light feeds the per-frame drift.
	extern idCVar r_softShadowDebugHash;
	if( r_softShadowDebugHash.GetInteger() >= 2 && v > 0 )
	{
		uint32 vh = 2166136261u;
		const byte* vb = ( const byte* )verts;
		for( int b = 0; b < tri->numVerts * ( int )sizeof( idDrawVert ); b++ )
		{
			vh = ( vh ^ vb[b] ) * 16777619u;
		}
		uint32 mh = 2166136261u;
		const byte* mb = ( const byte* )modelToWorld;
		for( int b = 0; b < 16 * ( int )sizeof( float ); b++ )
		{
			mh = ( mh ^ mb[b] ) * 16777619u;
		}
		common->Printf( "wedge ent=%i light=%i nv=%i numSil=%i vertHash=%08x mtxHash=%08x org=(%.3f %.3f %.3f)\n",
						ent->index, light->index, tri->numVerts, numSil, ( unsigned )vh, ( unsigned )mh,
						modelToWorld[12], modelToWorld[13], modelToWorld[14] );
	}

	*outVerts = wv;
	*outIndexes = wi;
	*outNumVerts = v;
	*outNumIndexes = idx;
}

/*
====================
R_CreateInteractionLightTris

This is only used for the static interaction case, dynamic interactions
just draw everything and let the GPU deal with it.

The resulting surface will be a subset of the original triangles,
it will never clip triangles, but it may cull on a per-triangle basis.
====================
*/
static srfTriangles_t* R_CreateInteractionLightTris( const idRenderEntityLocal* ent,
		const srfTriangles_t* tri, const idRenderLightLocal* light,
		const idMaterial* shader )
{

	SCOPED_PROFILE_EVENT( "R_CreateInteractionLightTris" );

	int			i;
	int			numIndexes;
	triIndex_t*	indexes;
	srfTriangles_t*	newTri;
	int			c_backfaced;
	int			c_distance;
	idBounds	bounds;
	bool		includeBackFaces;
	int			faceNum;

	c_backfaced = 0;
	c_distance = 0;

	numIndexes = 0;
	indexes = NULL;

	// it is debatable if non-shadowing lights should light back faces. we aren't at the moment
	// RB: now we do, so don't cull back faces if we have smooth shadowing enabled
	if( r_lightAllBackFaces.GetBool() || light->lightShader->LightEffectsBackSides()
			|| shader->ReceivesLightingOnBackSides() || ent->parms.noSelfShadow || ent->parms.noShadow )
	{
		includeBackFaces = true;
	}
	else
	{
		includeBackFaces = false;
	}

	// allocate a new surface for the lit triangles
	newTri = R_AllocStaticTriSurf();

	// save a reference to the original surface
	newTri->ambientSurface = const_cast<srfTriangles_t*>( tri );

	// the light surface references the verts of the ambient surface
	newTri->numVerts = tri->numVerts;
	R_ReferenceStaticTriSurfVerts( newTri, tri );

	// calculate cull information
	srfCullInfo_t cullInfo = {};

	if( !includeBackFaces )
	{
		R_CalcInteractionFacing( ent, tri, light, cullInfo );
	}
	R_CalcInteractionCullBits( ent, tri, light, cullInfo );

	// if the surface is completely inside the light frustum
	if( cullInfo.cullBits == LIGHT_CULL_ALL_FRONT )
	{

		// if we aren't self shadowing, let back facing triangles get
		// through so the smooth shaded bump maps light all the way around
		if( includeBackFaces )
		{

			// the whole surface is lit so the light surface just references the indexes of the ambient surface
			newTri->indexes = tri->indexes;
			newTri->indexCache = tri->indexCache;
//			R_ReferenceStaticTriSurfIndexes( newTri, tri );

			numIndexes = tri->numIndexes;
			bounds = tri->bounds;

		}
		else
		{

			// the light tris indexes are going to be a subset of the original indexes so we generally
			// allocate too much memory here but we decrease the memory block when the number of indexes is known
			R_AllocStaticTriSurfIndexes( newTri, tri->numIndexes );

			// back face cull the individual triangles
			indexes = newTri->indexes;
			const byte* facing = cullInfo.facing;
			for( faceNum = i = 0; i < tri->numIndexes; i += 3, faceNum++ )
			{
				if( !facing[ faceNum ] )
				{
					c_backfaced++;
					continue;
				}
				indexes[numIndexes + 0] = tri->indexes[i + 0];
				indexes[numIndexes + 1] = tri->indexes[i + 1];
				indexes[numIndexes + 2] = tri->indexes[i + 2];
				numIndexes += 3;
			}

			// get bounds for the surface
			SIMDProcessor->MinMax( bounds[0], bounds[1], tri->verts, indexes, numIndexes );

			// decrease the size of the memory block to the size of the number of used indexes
			newTri->numIndexes = numIndexes;
			R_ResizeStaticTriSurfIndexes( newTri, numIndexes );
		}

	}
	else
	{

		// the light tris indexes are going to be a subset of the original indexes so we generally
		// allocate too much memory here but we decrease the memory block when the number of indexes is known
		R_AllocStaticTriSurfIndexes( newTri, tri->numIndexes );

		// cull individual triangles
		indexes = newTri->indexes;
		const byte* facing = cullInfo.facing;
		const byte* cullBits = cullInfo.cullBits;
		for( faceNum = i = 0; i < tri->numIndexes; i += 3, faceNum++ )
		{
			int i1, i2, i3;

			// if we aren't self shadowing, let back facing triangles get
			// through so the smooth shaded bump maps light all the way around
			if( !includeBackFaces )
			{
				// back face cull
				if( !facing[ faceNum ] )
				{
					c_backfaced++;
					continue;
				}
			}

			i1 = tri->indexes[i + 0];
			i2 = tri->indexes[i + 1];
			i3 = tri->indexes[i + 2];

			// fast cull outside the frustum
			// if all three points are off one plane side, it definately isn't visible
			if( cullBits[i1] & cullBits[i2] & cullBits[i3] )
			{
				c_distance++;
				continue;
			}

			// add to the list
			indexes[numIndexes + 0] = i1;
			indexes[numIndexes + 1] = i2;
			indexes[numIndexes + 2] = i3;
			numIndexes += 3;
		}

		// get bounds for the surface
		SIMDProcessor->MinMax( bounds[0], bounds[1], tri->verts, indexes, numIndexes );

		// decrease the size of the memory block to the size of the number of used indexes
		newTri->numIndexes = numIndexes;
		R_ResizeStaticTriSurfIndexes( newTri, numIndexes );
	}

	// free the cull information when it's no longer needed
	R_FreeInteractionCullInfo( cullInfo );

	if( !numIndexes )
	{
		R_FreeStaticTriSurf( newTri );
		return NULL;
	}

	newTri->numIndexes = numIndexes;

	newTri->bounds = bounds;

	return newTri;
}

/*
===============
idInteraction::idInteraction
===============
*/
idInteraction::idInteraction()
{
	numSurfaces				= 0;
	surfaces				= NULL;
	entityDef				= NULL;
	lightDef				= NULL;
	lightNext				= NULL;
	lightPrev				= NULL;
	entityNext				= NULL;
	entityPrev				= NULL;
	staticInteraction		= false;
}

/*
===============
idInteraction::AllocAndLink
===============
*/
idInteraction* idInteraction::AllocAndLink( idRenderEntityLocal* edef, idRenderLightLocal* ldef )
{
	if( edef == NULL || ldef == NULL )
	{
		common->Error( "idInteraction::AllocAndLink: NULL parm" );
		return NULL;
	}

	idRenderWorldLocal* renderWorld = edef->world;

	idInteraction* interaction = renderWorld->interactionAllocator.Alloc();

	// link and initialize
	interaction->lightDef = ldef;
	interaction->entityDef = edef;

	interaction->numSurfaces = -1;		// not checked yet
	interaction->surfaces = NULL;

	// link at the start of the entity's list
	interaction->lightNext = ldef->firstInteraction;
	interaction->lightPrev = NULL;
	ldef->firstInteraction = interaction;
	if( interaction->lightNext != NULL )
	{
		interaction->lightNext->lightPrev = interaction;
	}
	else
	{
		ldef->lastInteraction = interaction;
	}

	// link at the start of the light's list
	interaction->entityNext = edef->firstInteraction;
	interaction->entityPrev = NULL;
	edef->firstInteraction = interaction;
	if( interaction->entityNext != NULL )
	{
		interaction->entityNext->entityPrev = interaction;
	}
	else
	{
		edef->lastInteraction = interaction;
	}

	// update the interaction table
	if( renderWorld->interactionTable != NULL )
	{
		int index = ldef->index * renderWorld->interactionTableWidth + edef->index;
		if( renderWorld->interactionTable[index] != NULL )
		{
			common->Error( "idInteraction::AllocAndLink: non NULL table entry" );
		}
		renderWorld->interactionTable[ index ] = interaction;
	}

	return interaction;
}

/*
===============
idInteraction::FreeSurfaces

Frees the surfaces, but leaves the interaction linked in, so it
will be regenerated automatically
===============
*/
void idInteraction::FreeSurfaces()
{
	// anything regenerated is no longer an optimized static version
	this->staticInteraction = false;

	if( this->surfaces != NULL )
	{
		R_StaticFree( this->surfaces );
		this->surfaces = NULL;
	}
	this->numSurfaces = -1;
}

/*
===============
idInteraction::Unlink
===============
*/
void idInteraction::Unlink()
{

	// unlink from the entity's list
	if( this->entityPrev )
	{
		this->entityPrev->entityNext = this->entityNext;
	}
	else
	{
		this->entityDef->firstInteraction = this->entityNext;
	}
	if( this->entityNext )
	{
		this->entityNext->entityPrev = this->entityPrev;
	}
	else
	{
		this->entityDef->lastInteraction = this->entityPrev;
	}
	this->entityNext = this->entityPrev = NULL;

	// unlink from the light's list
	if( this->lightPrev )
	{
		this->lightPrev->lightNext = this->lightNext;
	}
	else
	{
		this->lightDef->firstInteraction = this->lightNext;
	}
	if( this->lightNext )
	{
		this->lightNext->lightPrev = this->lightPrev;
	}
	else
	{
		this->lightDef->lastInteraction = this->lightPrev;
	}
	this->lightNext = this->lightPrev = NULL;
}

/*
===============
idInteraction::UnlinkAndFree

Removes links and puts it back on the free list.
===============
*/
void idInteraction::UnlinkAndFree()
{
	// clear the table pointer
	idRenderWorldLocal* renderWorld = this->lightDef->world;

	// RB: added check for NULL
	if( renderWorld->interactionTable != NULL )
	{
		int index = this->lightDef->index * renderWorld->interactionTableWidth + this->entityDef->index;
		if( renderWorld->interactionTable[index] != this && renderWorld->interactionTable[index] != INTERACTION_EMPTY )
		{
			common->Error( "idInteraction::UnlinkAndFree: interactionTable wasn't set" );
		}
		renderWorld->interactionTable[index] = NULL;
	}
	// RB end

	Unlink();

	FreeSurfaces();

	// put it back on the free list
	renderWorld->interactionAllocator.Free( this );
}

/*
===============
idInteraction::MakeEmpty

Relinks the interaction at the end of both the light and entity chains
and adds the INTERACTION_EMPTY marker to the interactionTable.

It is necessary to keep the empty interaction so when entities or lights move
they can set all the interactionTable values to NULL.
===============
*/
void idInteraction::MakeEmpty()
{
	// an empty interaction has no surfaces
	numSurfaces = 0;

	Unlink();

	// relink at the end of the entity's list
	this->entityNext = NULL;
	this->entityPrev = this->entityDef->lastInteraction;
	this->entityDef->lastInteraction = this;
	if( this->entityPrev )
	{
		this->entityPrev->entityNext = this;
	}
	else
	{
		this->entityDef->firstInteraction = this;
	}

	// relink at the end of the light's list
	this->lightNext = NULL;
	this->lightPrev = this->lightDef->lastInteraction;
	this->lightDef->lastInteraction = this;
	if( this->lightPrev )
	{
		this->lightPrev->lightNext = this;
	}
	else
	{
		this->lightDef->firstInteraction = this;
	}

	// store the special marker in the interaction table
	const int interactionIndex = lightDef->index * entityDef->world->interactionTableWidth + entityDef->index;
	assert( entityDef->world->interactionTable[ interactionIndex ] == this );
	entityDef->world->interactionTable[ interactionIndex ] = INTERACTION_EMPTY;
}

/*
===============
idInteraction::HasShadows
===============
*/
bool idInteraction::HasShadows() const
{
	return !entityDef->parms.noShadow && lightDef->LightCastsShadows();
}

/*
======================
CreateStaticInteraction

Called by idRenderWorldLocal::GenerateAllInteractions
======================
*/
void idInteraction::CreateStaticInteraction( nvrhi::ICommandList* commandList )
{
	// note that it is a static interaction
	staticInteraction = true;
	const idRenderModel* model = entityDef->parms.hModel;
	if( model == NULL || model->NumSurfaces() <= 0 || model->IsDynamicModel() != DM_STATIC )
	{
		MakeEmpty();
		return;
	}

	const idBounds bounds = model->Bounds( &entityDef->parms );

	// if it doesn't contact the light frustum, none of the surfaces will
	if( R_CullModelBoundsToLight( lightDef, bounds, entityDef->modelRenderMatrix ) )
	{
		MakeEmpty();
		return;
	}

	//
	// create slots for each of the model's surfaces
	//
	numSurfaces = model->NumSurfaces();
	surfaces = ( surfaceInteraction_t* )R_ClearedStaticAlloc( sizeof( *surfaces ) * numSurfaces );

	bool interactionGenerated = false;

	// check each surface in the model
	for( int c = 0 ; c < model->NumSurfaces() ; c++ )
	{
		const modelSurface_t* surf = model->Surface( c );
		const srfTriangles_t* tri = surf->geometry;
		if( tri == NULL )
		{
			continue;
		}

		// determine the shader for this surface, possibly by skinning
		// Note that this will be wrong if customSkin/customShader are
		// changed after map load time without invalidating the interaction!
		const idMaterial* const shader = R_RemapShaderBySkin( surf->shader,
										 entityDef->parms.customSkin, entityDef->parms.customShader );
		if( shader == NULL )
		{
			continue;
		}

		// try to cull each surface
		if( R_CullModelBoundsToLight( lightDef, tri->bounds, entityDef->modelRenderMatrix ) )
		{
			continue;
		}

		surfaceInteraction_t* sint = &surfaces[c];

		// generate a set of indexes for the lit surfaces, culling away triangles that are
		// not at least partially inside the light
		if( shader->ReceivesLighting() )
		{
			srfTriangles_t* lightTris = R_CreateInteractionLightTris( entityDef, tri, lightDef, shader );
			if( lightTris != NULL )
			{
				// make a static index cache
				sint->numLightTrisIndexes = lightTris->numIndexes;
				sint->lightTrisIndexCache = vertexCache.AllocStaticIndex( lightTris->indexes, lightTris->numIndexes * sizeof( lightTris->indexes[0] ), commandList );

				interactionGenerated = true;
				R_FreeStaticTriSurf( lightTris );
			}
		}

		// stencil shadow volumes: build a static shadow volume for this surface and cache its
		// indexes (they reference the ambient surface's doubled shadowCache). Restored from
		// DOOM-3-BFG; the prelight-model optimisation is omitted for now (always build).
		if( HasShadows() && shader->SurfaceCastsShadow() && tri->silEdges != NULL )
		{
			srfTriangles_t* shadowTris = R_CreateInteractionShadowVolume( entityDef, tri, lightDef );
			if( shadowTris != NULL )
			{
				sint->shadowIndexCache = vertexCache.AllocStaticIndex( shadowTris->indexes, shadowTris->numIndexes * sizeof( shadowTris->indexes[0] ), commandList );
				sint->numShadowIndexes = shadowTris->numIndexes;
				if( shader->Coverage() != MC_OPAQUE )
				{
					// perforated / translucent casters can be seen through, so the caps are always needed
					sint->numShadowIndexesNoCaps = shadowTris->numIndexes;
				}
				else
				{
					sint->numShadowIndexesNoCaps = shadowTris->numShadowIndexesNoCaps;
				}
				R_FreeStaticTriSurf( shadowTris );
			}
			interactionGenerated = true;
		}
	}

	// if none of the surfaces generated anything, don't even bother checking?
	if( !interactionGenerated )
	{
		MakeEmpty();
	}
}

/*
===================
R_ShowInteractionMemory_f
===================
*/
void R_ShowInteractionMemory_f( const idCmdArgs& args )
{
	int entities = 0;
	int interactions = 0;
	int deferredInteractions = 0;
	int emptyInteractions = 0;
	int lightTris = 0;
	int lightTriIndexes = 0;
	int shadowTris = 0;
	int shadowTriIndexes = 0;
	int maxInteractionsForEntity = 0;
	int maxInteractionsForLight = 0;

	for( int i = 0; i < tr.primaryWorld->lightDefs.Num(); i++ )
	{
		idRenderLightLocal* light = tr.primaryWorld->lightDefs[i];
		if( light == NULL )
		{
			continue;
		}
		int numInteractionsForLight = 0;
		for( idInteraction* inter = light->firstInteraction; inter != NULL; inter = inter->lightNext )
		{
			if( !inter->IsEmpty() )
			{
				numInteractionsForLight++;
			}
		}
		if( numInteractionsForLight > maxInteractionsForLight )
		{
			maxInteractionsForLight = numInteractionsForLight;
		}
	}

	for( int i = 0; i < tr.primaryWorld->entityDefs.Num(); i++ )
	{
		idRenderEntityLocal*	def = tr.primaryWorld->entityDefs[i];
		if( def == NULL )
		{
			continue;
		}
		if( def->firstInteraction == NULL )
		{
			continue;
		}
		entities++;

		int numInteractionsForEntity = 0;
		for( idInteraction* inter = def->firstInteraction; inter != NULL; inter = inter->entityNext )
		{
			interactions++;

			if( !inter->IsEmpty() )
			{
				numInteractionsForEntity++;
			}

			if( inter->IsDeferred() )
			{
				deferredInteractions++;
				continue;
			}
			if( inter->IsEmpty() )
			{
				emptyInteractions++;
				continue;
			}

			for( int j = 0; j < inter->numSurfaces; j++ )
			{
				surfaceInteraction_t* srf = &inter->surfaces[j];

				if( srf->numLightTrisIndexes )
				{
					lightTris++;
					lightTriIndexes += srf->numLightTrisIndexes;
				}
			}
		}
		if( numInteractionsForEntity > maxInteractionsForEntity )
		{
			maxInteractionsForEntity = numInteractionsForEntity;
		}
	}

	common->Printf( "%i entities with %i total interactions\n", entities, interactions );
	common->Printf( "%i deferred interactions, %i empty interactions\n", deferredInteractions, emptyInteractions );
	common->Printf( "%5i indexes in %5i light tris\n", lightTriIndexes, lightTris );
	common->Printf( "%5i indexes in %5i shadow tris\n", shadowTriIndexes, shadowTris );
	common->Printf( "%i maxInteractionsForEntity\n", maxInteractionsForEntity );
	common->Printf( "%i maxInteractionsForLight\n", maxInteractionsForLight );
}
