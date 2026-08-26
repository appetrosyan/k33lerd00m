/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2015-2024 Robert Beckebans

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
#include "SoftShadowHull.h"

// Brush-recovery soft shadows: per-area convex hull table, parsed from the .proc `shadowHulls` block (or
// the .bproc mirror) at InitFromMap. Outer index = BSP area; inner = that area's hulls. File-scope global
// (see R_GetAreaShadowHulls in SoftShadowHull.h) - one active render world. The worldspawn soft-caster
// reads it via R_GetAreaShadowHulls when r_softShadowBrushHulls is set. Absent block => empty => tris.
static idList< idList<areaShadowHull_t> > s_areaShadowHulls;

const areaShadowHull_t* R_GetAreaShadowHulls( int area, int* outNumHulls )
{
	if( area < 0 || area >= s_areaShadowHulls.Num() || s_areaShadowHulls[area].Num() == 0 )
	{
		if( outNumHulls != NULL )
		{
			*outNumHulls = 0;
		}
		return NULL;
	}
	if( outNumHulls != NULL )
	{
		*outNumHulls = s_areaShadowHulls[area].Num();
	}
	return s_areaShadowHulls[area].Ptr();
}

// Brush-recovery soft shadows, package B: per-area residual TRIANGLES (the non-hull-emittable brushes), parsed
// from the .proc `shadowResiduals` block. Parallel to s_areaShadowHulls; same one-active-render-world caveat.
static idList< areaResidualTris_t > s_areaResidualTris;

const areaResidualTris_t* R_GetAreaResidualTris( int area )
{
	if( area < 0 || area >= s_areaResidualTris.Num() || s_areaResidualTris[area].indexes.Num() == 0 )
	{
		return NULL;
	}
	return &s_areaResidualTris[area];
}

/*
================
R_ParseShadowHulls

Additive, presence-gated .proc block (grammar in brush-recovery-implementation-plan.md Phase 0):
  shadowHulls { N  areaIdx { M  { K ( x y z )... } ... } ... }
Missing block => s_areaShadowHulls stays empty => triangle fallback (old .proc still loads). Oversized
hulls (K outside [3, SW_HULL_MAX_VERTS]) are dropped, never stored (the shader can only walk <=8 verts).
When fileOut != NULL the parsed table is mirrored into the .bproc so cached reloads keep the hulls.
================
*/
static void R_ParseShadowHulls( idLexer* src, idFile* fileOut )
{
	s_areaShadowHulls.Clear();

	src->ExpectTokenString( "{" );
	const int numAreas = src->ParseInt();
	if( numAreas > 0 )
	{
		s_areaShadowHulls.SetNum( numAreas );
	}

	for( int a = 0; a < numAreas; a++ )
	{
		const int areaIdx = src->ParseInt();
		src->ExpectTokenString( "{" );
		const int numHulls = src->ParseInt();
		for( int h = 0; h < numHulls; h++ )
		{
			src->ExpectTokenString( "{" );
			const int numVerts = src->ParseInt();
			areaShadowHull_t hull;
			hull.numVerts = numVerts;
			for( int v = 0; v < numVerts; v++ )
			{
				float xyz[3];
				src->Parse1DMatrix( 3, xyz );
				if( v < SW_HULL_MAX_VERTS )
				{
					hull.verts[v * 3 + 0] = xyz[0];
					hull.verts[v * 3 + 1] = xyz[1];
					hull.verts[v * 3 + 2] = xyz[2];
				}
			}
			src->ExpectTokenString( "}" );
			// keep only walkable hulls; drop degenerate/oversized to the triangle path
			if( numVerts >= 3 && numVerts <= SW_HULL_MAX_VERTS && areaIdx >= 0 && areaIdx < s_areaShadowHulls.Num() )
			{
				s_areaShadowHulls[areaIdx].Append( hull );
			}
		}
		src->ExpectTokenString( "}" );
	}
	src->ExpectTokenString( "}" );

	if( fileOut != NULL )
	{
		fileOut->WriteString( "shadowHulls" );
		fileOut->WriteBig( s_areaShadowHulls.Num() );
		for( int a = 0; a < s_areaShadowHulls.Num(); a++ )
		{
			fileOut->WriteBig( s_areaShadowHulls[a].Num() );
			for( int h = 0; h < s_areaShadowHulls[a].Num(); h++ )
			{
				fileOut->WriteBig( s_areaShadowHulls[a][h].numVerts );
				fileOut->WriteBigArray( s_areaShadowHulls[a][h].verts, SW_HULL_MAX_VERTS * 3 );
			}
		}
	}
}

/*
================
R_ReadBinaryShadowHulls

Read the .bproc mirror of the `shadowHulls` block written by R_ParseShadowHulls.
================
*/
static void R_ReadBinaryShadowHulls( idFile* file )
{
	s_areaShadowHulls.Clear();
	int numAreas = 0;
	file->ReadBig( numAreas );
	if( numAreas > 0 )
	{
		s_areaShadowHulls.SetNum( numAreas );
	}
	for( int a = 0; a < numAreas; a++ )
	{
		int numHulls = 0;
		file->ReadBig( numHulls );
		for( int h = 0; h < numHulls; h++ )
		{
			areaShadowHull_t hull;
			file->ReadBig( hull.numVerts );
			file->ReadBigArray( hull.verts, SW_HULL_MAX_VERTS * 3 );
			s_areaShadowHulls[a].Append( hull );
		}
	}
}

/*
================
R_ParseShadowResiduals

Additive, presence-gated .proc block (package B; grammar mirrors dmap's WriteShadowResiduals):
  shadowResiduals { N  <a> { V I  ( x y z  s t  nx ny nz )...V  i0 i1 i2...I  } ... }
Verts are 8 floats each (MAP space); indexes are a flat triangle list into that area's vert block. Missing
block => s_areaResidualTris stays empty => nothing streamed (old .proc still loads). Only .xyz is read by the
caster (R_CollectPenumbraFaces), but the full vert is parsed so the .proc grammar stays the normal one. When
fileOut != NULL the parsed table is mirrored (xyz + indexes) into the .bproc so cached reloads keep it.
================
*/
static void R_ParseShadowResiduals( idLexer* src, idFile* fileOut )
{
	s_areaResidualTris.Clear();

	src->ExpectTokenString( "{" );
	const int numAreas = src->ParseInt();
	if( numAreas > 0 )
	{
		s_areaResidualTris.SetNum( numAreas );
	}

	for( int a = 0; a < numAreas; a++ )
	{
		const int areaIdx = src->ParseInt();
		src->ExpectTokenString( "{" );
		const int numVerts = src->ParseInt();
		const int numIndexes = src->ParseInt();
		const bool store = ( areaIdx >= 0 && areaIdx < s_areaResidualTris.Num() );
		for( int v = 0; v < numVerts; v++ )
		{
			float f[8];
			src->Parse1DMatrix( 8, f );
			if( store )
			{
				idDrawVert dv;
				dv.Clear();
				dv.xyz.Set( f[0], f[1], f[2] );
				dv.SetTexCoord( f[3], f[4] );
				dv.SetNormal( f[5], f[6], f[7] );
				s_areaResidualTris[areaIdx].verts.Append( dv );
			}
		}
		for( int i = 0; i < numIndexes; i++ )
		{
			const int idx = src->ParseInt();
			if( store )
			{
				s_areaResidualTris[areaIdx].indexes.Append( ( triIndex_t )idx );
			}
		}
		src->ExpectTokenString( "}" );
	}
	src->ExpectTokenString( "}" );

	if( fileOut != NULL )
	{
		fileOut->WriteString( "shadowResiduals" );
		fileOut->WriteBig( s_areaResidualTris.Num() );
		for( int a = 0; a < s_areaResidualTris.Num(); a++ )
		{
			const areaResidualTris_t& r = s_areaResidualTris[a];
			fileOut->WriteBig( r.verts.Num() );
			fileOut->WriteBig( r.indexes.Num() );
			for( int v = 0; v < r.verts.Num(); v++ )
			{
				const idVec3& p = r.verts[v].xyz;		// only .xyz is consumed; st/normal reconstruct as 0 on read
				fileOut->WriteBig( p.x );
				fileOut->WriteBig( p.y );
				fileOut->WriteBig( p.z );
			}
			for( int i = 0; i < r.indexes.Num(); i++ )
			{
				fileOut->WriteBig( ( int )r.indexes[i] );
			}
		}
	}
}

/*
================
R_ReadBinaryShadowResiduals

Read the .bproc mirror of the `shadowResiduals` block written by R_ParseShadowResiduals (xyz + indexes only;
st/normal are not read by the caster, so they are not stored).
================
*/
static void R_ReadBinaryShadowResiduals( idFile* file )
{
	s_areaResidualTris.Clear();
	int numAreas = 0;
	file->ReadBig( numAreas );
	if( numAreas > 0 )
	{
		s_areaResidualTris.SetNum( numAreas );
	}
	for( int a = 0; a < numAreas; a++ )
	{
		int numVerts = 0, numIndexes = 0;
		file->ReadBig( numVerts );
		file->ReadBig( numIndexes );
		for( int v = 0; v < numVerts; v++ )
		{
			float x, y, z;
			file->ReadBig( x );
			file->ReadBig( y );
			file->ReadBig( z );
			idDrawVert dv;
			dv.Clear();
			dv.xyz.Set( x, y, z );
			s_areaResidualTris[a].verts.Append( dv );
		}
		for( int i = 0; i < numIndexes; i++ )
		{
			int idx = 0;
			file->ReadBig( idx );
			s_areaResidualTris[a].indexes.Append( ( triIndex_t )idx );
		}
	}
}

/*
================
idRenderWorldLocal::FreeWorld
================
*/
void idRenderWorldLocal::FreeWorld()
{
	// this will free all the lightDefs and entityDefs
	FreeDefs();

	// free all the portals and check light/model references
	for( int i = 0; i < numPortalAreas; i++ )
	{
		portalArea_t*	area;
		portal_t*		portal, *nextPortal;

		area = &portalAreas[i];
		for( portal = area->portals; portal; portal = nextPortal )
		{
			nextPortal = portal->next;
			delete portal->w;
			R_StaticFree( portal );
		}

		// SRS - release the lightGridPoints idList or it will leak
		area->lightGrid.lightGridPoints.Clear();

		// there shouldn't be any remaining lightRefs or entityRefs
		if( area->lightRefs.areaNext != &area->lightRefs )
		{
			common->Error( "FreeWorld: unexpected remaining lightRefs" );
		}
		if( area->entityRefs.areaNext != &area->entityRefs )
		{
			common->Error( "FreeWorld: unexpected remaining entityRefs" );
		}
	}

	if( portalAreas )
	{
		R_StaticFree( portalAreas );
		portalAreas = NULL;
		numPortalAreas = 0;
		R_StaticFree( areaScreenRect );
		areaScreenRect = NULL;
	}

	if( doublePortals )
	{
		R_StaticFree( doublePortals );
		doublePortals = NULL;
		numInterAreaPortals = 0;
	}

	if( areaNodes )
	{
		R_StaticFree( areaNodes );
		areaNodes = NULL;
	}

	// free all the inline idRenderModels
	for( int i = 0; i < localModels.Num(); i++ )
	{
		renderModelManager->RemoveModel( localModels[i] );
		delete localModels[i];
	}
	localModels.Clear();

	areaReferenceAllocator.Shutdown();
	interactionAllocator.Shutdown();

	// brush-recovery soft-shadow hulls + residual triangles belong to this map
	s_areaShadowHulls.Clear();
	s_areaResidualTris.Clear();

	mapName = "<FREED>";
}

/*
================
idRenderWorldLocal::TouchWorldModels
================
*/
void idRenderWorldLocal::TouchWorldModels()
{
	for( int i = 0; i < localModels.Num(); i++ )
	{
		renderModelManager->CheckModel( localModels[i]->Name() );
	}
}

/*
================
idRenderWorldLocal::ReadBinaryShadowModel
================
*/
idRenderModel* idRenderWorldLocal::ReadBinaryModel( idFile* fileIn )
{
	idStrStatic< MAX_OSPATH > name;
	fileIn->ReadString( name );
	idRenderModel* model = renderModelManager->AllocModel();
	model->InitEmpty( name );

	// RB: declSourceTimeStamp is not important here
	if( model->LoadBinaryModel( fileIn, mapTimeStamp, 0 ) )
	{
		return model;
	}
	return NULL;
}

extern idCVar binaryLoadRenderModels;

/*
================
idRenderWorldLocal::ParseModel
================
*/
idRenderModel* idRenderWorldLocal::ParseModel( idLexer* src, const char* mapName, ID_TIME_T mapTimeStamp, idFile* fileOut )
{
	idToken token;

	src->ExpectTokenString( "{" );

	// parse the name
	src->ExpectAnyToken( &token );

	idRenderModel* model = renderModelManager->AllocModel();
	model->InitEmpty( token );

	if( fileOut != NULL )
	{
		// write out the type so the binary reader knows what to instantiate
		fileOut->WriteString( "model" );
		fileOut->WriteString( token );
	}

	int numSurfaces = src->ParseInt();
	if( numSurfaces < 0 )
	{
		src->Error( "R_ParseModel: bad numSurfaces" );
	}

	for( int i = 0; i < numSurfaces; i++ )
	{
		src->ExpectTokenString( "{" );

		src->ExpectAnyToken( &token );

		modelSurface_t surf;
		surf.shader = declManager->FindMaterial( token );

		( ( idMaterial* )surf.shader )->AddReference();

		srfTriangles_t* tri = R_AllocStaticTriSurf();
		surf.geometry = tri;

		tri->numVerts = src->ParseInt();
		tri->numIndexes = src->ParseInt();

		// parse the vertices
		idTempArray<float> verts( tri->numVerts * 8 );
		for( int j = 0; j < tri->numVerts; j++ )
		{
			src->Parse1DMatrix( 8, &verts[j * 8] );
		}

		// parse the indices
		idTempArray<triIndex_t> indexes( tri->numIndexes );
		for( int j = 0; j < tri->numIndexes; j++ )
		{
			indexes[j] = src->ParseInt();
		}

#if 1
		// find the island that each vertex belongs to
		idTempArray<int> vertIslands( tri->numVerts );
		idTempArray<bool> trisVisited( tri->numIndexes );
		vertIslands.Zero();
		trisVisited.Zero();
		int numIslands = 0;
		for( int j = 0; j < tri->numIndexes; j += 3 )
		{
			if( trisVisited[j] )
			{
				continue;
			}

			int islandNum = ++numIslands;
			vertIslands[indexes[j + 0]] = islandNum;
			vertIslands[indexes[j + 1]] = islandNum;
			vertIslands[indexes[j + 2]] = islandNum;
			trisVisited[j] = true;

			idList<int> queue;
			queue.Append( j );
			for( int n = 0; n < queue.Num(); n++ )
			{
				int t = queue[n];
				for( int k = 0; k < tri->numIndexes; k += 3 )
				{
					if( trisVisited[k] )
					{
						continue;
					}
					bool connected =	indexes[t + 0] == indexes[k + 0] || indexes[t + 0] == indexes[k + 1] || indexes[t + 0] == indexes[k + 2] ||
										indexes[t + 1] == indexes[k + 0] || indexes[t + 1] == indexes[k + 1] || indexes[t + 1] == indexes[k + 2] ||
										indexes[t + 2] == indexes[k + 0] || indexes[t + 2] == indexes[k + 1] || indexes[t + 2] == indexes[k + 2];
					if( connected )
					{
						vertIslands[indexes[k + 0]] = islandNum;
						vertIslands[indexes[k + 1]] = islandNum;
						vertIslands[indexes[k + 2]] = islandNum;
						trisVisited[k] = true;
						queue.Append( k );
					}
				}
			}
		}

		// center the texture coordinates for each island for maximum 16-bit precision
		for( int j = 1; j <= numIslands; j++ )
		{
			float minS = idMath::INFINITUM;
			float minT = idMath::INFINITUM;
			float maxS = -idMath::INFINITUM;
			float maxT = -idMath::INFINITUM;
			for( int k = 0; k < tri->numVerts; k++ )
			{
				if( vertIslands[k] == j )
				{
					minS = Min( minS, verts[k * 8 + 3] );
					maxS = Max( maxS, verts[k * 8 + 3] );
					minT = Min( minT, verts[k * 8 + 4] );
					maxT = Max( maxT, verts[k * 8 + 4] );
				}
			}
			const float averageS = idMath::Ftoi( ( minS + maxS ) * 0.5f );
			const float averageT = idMath::Ftoi( ( minT + maxT ) * 0.5f );
			for( int k = 0; k < tri->numVerts; k++ )
			{
				if( vertIslands[k] == j )
				{
					verts[k * 8 + 3] -= averageS;
					verts[k * 8 + 4] -= averageT;
				}
			}
		}
#endif

		R_AllocStaticTriSurfVerts( tri, tri->numVerts );
		for( int j = 0; j < tri->numVerts; j++ )
		{
			tri->verts[j].xyz[0] = verts[j * 8 + 0];
			tri->verts[j].xyz[1] = verts[j * 8 + 1];
			tri->verts[j].xyz[2] = verts[j * 8 + 2];
			tri->verts[j].SetTexCoord( verts[j * 8 + 3], verts[j * 8 + 4] );
			tri->verts[j].SetNormal( verts[j * 8 + 5], verts[j * 8 + 6], verts[j * 8 + 7] );
		}

		R_AllocStaticTriSurfIndexes( tri, tri->numIndexes );
		for( int j = 0; j < tri->numIndexes; j++ )
		{
			tri->indexes[j] = indexes[j];
		}
		src->ExpectTokenString( "}" );

		// add the completed surface to the model
		model->AddSurface( surf );
	}

	src->ExpectTokenString( "}" );

	// RB: FIXME add check for mikktspace
	model->FinishSurfaces( false );

	if( fileOut != NULL && model->SupportsBinaryModel() && binaryLoadRenderModels.GetBool() )
	{
		model->WriteBinaryModel( fileOut, &mapTimeStamp );
	}

	return model;
}

/*
================
idRenderWorldLocal::SetupAreaRefs
================
*/
void idRenderWorldLocal::SetupAreaRefs()
{
	connectedAreaNum = 0;
	for( int i = 0; i < numPortalAreas; i++ )
	{
		portalAreas[i].areaNum = i;

		portalAreas[i].lightRefs.areaNext =
			portalAreas[i].lightRefs.areaPrev = &portalAreas[i].lightRefs;

		portalAreas[i].entityRefs.areaNext =
			portalAreas[i].entityRefs.areaPrev = &portalAreas[i].entityRefs;

		portalAreas[i].envprobeRefs.areaNext =
			portalAreas[i].envprobeRefs.areaPrev = &portalAreas[i].envprobeRefs;
	}
}

/*
================
idRenderWorldLocal::ParseInterAreaPortals
================
*/
void idRenderWorldLocal::ParseInterAreaPortals( idLexer* src, idFile* fileOut )
{
	src->ExpectTokenString( "{" );

	numPortalAreas = src->ParseInt();
	if( numPortalAreas < 0 )
	{
		src->Error( "R_ParseInterAreaPortals: bad numPortalAreas" );
		return;
	}

	if( fileOut != NULL )
	{
		// write out the type so the binary reader knows what to instantiate
		fileOut->WriteString( "interAreaPortals" );
	}


	portalAreas = ( portalArea_t* )R_ClearedStaticAlloc( numPortalAreas * sizeof( portalAreas[0] ) );
	areaScreenRect = ( idScreenRect* ) R_ClearedStaticAlloc( numPortalAreas * sizeof( idScreenRect ) );

	// set the doubly linked lists
	SetupAreaRefs();

	numInterAreaPortals = src->ParseInt();
	if( numInterAreaPortals < 0 )
	{
		src->Error( "R_ParseInterAreaPortals: bad numInterAreaPortals" );
		return;
	}

	if( fileOut != NULL )
	{
		fileOut->WriteBig( numPortalAreas );
		fileOut->WriteBig( numInterAreaPortals );
	}

	doublePortals = ( doublePortal_t* )R_ClearedStaticAlloc( numInterAreaPortals *
					sizeof( doublePortals [0] ) );

	for( int i = 0; i < numInterAreaPortals; i++ )
	{
		int		numPoints, a1, a2;
		idWinding*	w;
		portal_t*	p;

		numPoints = src->ParseInt();
		a1 = src->ParseInt();
		a2 = src->ParseInt();

		if( fileOut != NULL )
		{
			fileOut->WriteBig( numPoints );
			fileOut->WriteBig( a1 );
			fileOut->WriteBig( a2 );
		}

		w = new( TAG_RENDER_WINDING ) idWinding( numPoints );
		w->SetNumPoints( numPoints );
		for( int j = 0; j < numPoints; j++ )
		{
			src->Parse1DMatrix( 3, ( *w )[j].ToFloatPtr() );

			if( fileOut != NULL )
			{
				fileOut->WriteBig( ( *w )[j].x );
				fileOut->WriteBig( ( *w )[j].y );
				fileOut->WriteBig( ( *w )[j].z );
			}
			// no texture coordinates
			( *w )[j][3] = 0;
			( *w )[j][4] = 0;
		}

		// add the portal to a1
		p = ( portal_t* )R_ClearedStaticAlloc( sizeof( *p ) );
		p->intoArea = a2;
		p->doublePortal = &doublePortals[i];
		p->w = w;
		p->w->GetPlane( p->plane );

		p->next = portalAreas[a1].portals;
		portalAreas[a1].portals = p;

		doublePortals[i].portals[0] = p;

		// reverse it for a2
		p = ( portal_t* )R_ClearedStaticAlloc( sizeof( *p ) );
		p->intoArea = a1;
		p->doublePortal = &doublePortals[i];
		p->w = w->Reverse();
		p->w->GetPlane( p->plane );

		p->next = portalAreas[a2].portals;
		portalAreas[a2].portals = p;

		doublePortals[i].portals[1] = p;
	}

	src->ExpectTokenString( "}" );
}

/*
================
idRenderWorldLocal::ParseInterAreaPortals
================
*/
void idRenderWorldLocal::ReadBinaryAreaPortals( idFile* file )
{

	file->ReadBig( numPortalAreas );
	file->ReadBig( numInterAreaPortals );

	portalAreas = ( portalArea_t* )R_ClearedStaticAlloc( numPortalAreas * sizeof( portalAreas[0] ) );
	areaScreenRect = ( idScreenRect* ) R_ClearedStaticAlloc( numPortalAreas * sizeof( idScreenRect ) );

	// set the doubly linked lists
	SetupAreaRefs();

	doublePortals = ( doublePortal_t* )R_ClearedStaticAlloc( numInterAreaPortals * sizeof( doublePortals [0] ) );

	for( int i = 0; i < numInterAreaPortals; i++ )
	{
		int		numPoints, a1, a2;
		idWinding*	w;
		portal_t*	p;

		file->ReadBig( numPoints );
		file->ReadBig( a1 );
		file->ReadBig( a2 );

		w = new( TAG_RENDER_WINDING ) idWinding( numPoints );
		w->SetNumPoints( numPoints );

		for( int j = 0; j < numPoints; j++ )
		{
			file->ReadBig( ( *w )[ j ][ 0 ] );
			file->ReadBig( ( *w )[ j ][ 1 ] );
			file->ReadBig( ( *w )[ j ][ 2 ] );

			// no texture coordinates
			( *w )[ j ][ 3 ] = 0;
			( *w )[ j ][ 4 ] = 0;
		}

		// add the portal to a1
		p = ( portal_t* )R_ClearedStaticAlloc( sizeof( *p ) );
		p->intoArea = a2;
		p->doublePortal = &doublePortals[i];
		p->w = w;
		p->w->GetPlane( p->plane );

		p->next = portalAreas[a1].portals;
		portalAreas[a1].portals = p;

		doublePortals[i].portals[0] = p;

		// reverse it for a2
		p = ( portal_t* )R_ClearedStaticAlloc( sizeof( *p ) );
		p->intoArea = a1;
		p->doublePortal = &doublePortals[i];
		p->w = w->Reverse();
		p->w->GetPlane( p->plane );

		p->next = portalAreas[a2].portals;
		portalAreas[a2].portals = p;

		doublePortals[i].portals[1] = p;
	}
}


/*
================
idRenderWorldLocal::ParseNodes
================
*/
void idRenderWorldLocal::ParseNodes( idLexer* src, idFile* fileOut )
{
	src->ExpectTokenString( "{" );

	numAreaNodes = src->ParseInt();
	if( numAreaNodes < 0 )
	{
		src->Error( "R_ParseNodes: bad numAreaNodes" );
	}
	areaNodes = ( areaNode_t* )R_ClearedStaticAlloc( numAreaNodes * sizeof( areaNodes[0] ) );

	if( fileOut != NULL )
	{
		// write out the type so the binary reader knows what to instantiate
		fileOut->WriteString( "nodes" );
	}

	if( fileOut != NULL )
	{
		fileOut->WriteBig( numAreaNodes );
	}

	for( int i = 0; i < numAreaNodes; i++ )
	{
		areaNode_t*	node;

		node = &areaNodes[i];

		src->Parse1DMatrix( 4, node->plane.ToFloatPtr() );

		node->children[0] = src->ParseInt();
		node->children[1] = src->ParseInt();

		if( fileOut != NULL )
		{
			fileOut->WriteBig( node->plane[ 0 ] );
			fileOut->WriteBig( node->plane[ 1 ] );
			fileOut->WriteBig( node->plane[ 2 ] );
			fileOut->WriteBig( node->plane[ 3 ] );
			fileOut->WriteBig( node->children[ 0 ] );
			fileOut->WriteBig( node->children[ 1 ] );
		}

	}

	src->ExpectTokenString( "}" );
}

/*
================
idRenderWorldLocal::ReadBinaryNodes
================
*/
void idRenderWorldLocal::ReadBinaryNodes( idFile* file )
{
	file->ReadBig( numAreaNodes );
	areaNodes = ( areaNode_t* )R_ClearedStaticAlloc( numAreaNodes * sizeof( areaNodes[0] ) );
	for( int i = 0; i < numAreaNodes; i++ )
	{
		areaNode_t* node = &areaNodes[ i ];
		file->ReadBig( node->plane[ 0 ] );
		file->ReadBig( node->plane[ 1 ] );
		file->ReadBig( node->plane[ 2 ] );
		file->ReadBig( node->plane[ 3 ] );
		file->ReadBig( node->children[ 0 ] );
		file->ReadBig( node->children[ 1 ] );
	}
}

/*
================
idRenderWorldLocal::CommonChildrenArea_r
================
*/
int idRenderWorldLocal::CommonChildrenArea_r( areaNode_t* node )
{
	int	nums[2];

	for( int i = 0; i < 2; i++ )
	{
		if( node->children[i] <= 0 )
		{
			nums[i] = -1 - node->children[i];
		}
		else
		{
			nums[i] = CommonChildrenArea_r( &areaNodes[ node->children[i] ] );
		}
	}

	// solid nodes will match any area
	if( nums[0] == AREANUM_SOLID )
	{
		nums[0] = nums[1];
	}
	if( nums[1] == AREANUM_SOLID )
	{
		nums[1] = nums[0];
	}

	int	common;
	if( nums[0] == nums[1] )
	{
		common = nums[0];
	}
	else
	{
		common = CHILDREN_HAVE_MULTIPLE_AREAS;
	}

	node->commonChildrenArea = common;

	return common;
}

/*
=================
idRenderWorldLocal::ClearWorld

Sets up for a single area world
=================
*/
void idRenderWorldLocal::ClearWorld()
{
	numPortalAreas = 1;
	portalAreas = ( portalArea_t* )R_ClearedStaticAlloc( sizeof( portalAreas[0] ) );
	areaScreenRect = ( idScreenRect* ) R_ClearedStaticAlloc( sizeof( idScreenRect ) );

	SetupAreaRefs();

	// even though we only have a single area, create a node
	// that has both children pointing at it so we don't need to
	//
	areaNodes = ( areaNode_t* )R_ClearedStaticAlloc( sizeof( areaNodes[0] ) );
	areaNodes[0].plane[3] = 1;
	areaNodes[0].children[0] = -1;
	areaNodes[0].children[1] = -1;
}

/*
=================
idRenderWorldLocal::FreeDefs

dump all the interactions
=================
*/
void idRenderWorldLocal::FreeDefs()
{
	generateAllInteractionsCalled = false;

	if( interactionTable )
	{
		R_StaticFree( interactionTable );
		interactionTable = NULL;
	}

	// free all lightDefs
	for( int i = 0; i < lightDefs.Num(); i++ )
	{
		idRenderLightLocal* light = lightDefs[i];
		if( light != NULL && light->world == this )
		{
			FreeLightDef( i );
			lightDefs[i] = NULL;
		}
	}

	// free all entityDefs
	for( int i = 0; i < entityDefs.Num(); i++ )
	{
		idRenderEntityLocal*	 mod = entityDefs[i];
		if( mod != NULL && mod->world == this )
		{
			FreeEntityDef( i );
			entityDefs[i] = NULL;
		}
	}

	// RB: free all envprobeDefs
	for( int i = 0; i < envprobeDefs.Num(); i++ )
	{
		RenderEnvprobeLocal* ep = envprobeDefs[i];
		if( ep != NULL && ep->world == this )
		{
			FreeEnvprobeDef( i );
			envprobeDefs[i] = NULL;
		}
	}
	// RB end

	// Reset decals and overlays
	for( int i = 0; i < decals.Num(); i++ )
	{
		decals[i].entityHandle = -1;
		decals[i].lastStartTime = 0;
	}
	for( int i = 0; i < overlays.Num(); i++ )
	{
		overlays[i].entityHandle = -1;
		overlays[i].lastStartTime = 0;
	}
}

/*
=================
idRenderWorldLocal::InitFromMap

A NULL or empty name will make a world without a map model, which
is still useful for displaying a bare model
=================
*/
bool idRenderWorldLocal::InitFromMap( const char* name )
{
	idLexer* 		src;
	idToken			token;
	idRenderModel* 	lastModel;

	// if this is an empty world, initialize manually
	if( !name || !name[0] )
	{
		FreeWorld();
		mapName.Clear();
		ClearWorld();
		return true;
	}

	// load it
	idStrStatic< MAX_OSPATH > filename = name;
	filename.SetFileExtension( PROC_FILE_EXT );

	// check for generated file
	idStrStatic< MAX_OSPATH > generatedFileName = filename;
	generatedFileName.Insert( "generated/", 0 );
	generatedFileName.SetFileExtension( "bproc" );

	// if we are reloading the same map, check the timestamp
	// and try to skip all the work
	ID_TIME_T currentTimeStamp = fileSystem->GetTimestamp( filename );

	if( name == mapName )
	{
		if( fileSystem->InProductionMode() || ( currentTimeStamp != FILE_NOT_FOUND_TIMESTAMP && currentTimeStamp == mapTimeStamp ) )
		{
			common->Printf( "idRenderWorldLocal::InitFromMap: retaining existing map\n" );
			FreeDefs();
			TouchWorldModels();
			AddWorldModelEntities();
			ClearPortalStates();
			SetupLightGrid();
			return true;
		}
		common->Printf( "idRenderWorldLocal::InitFromMap: timestamp has changed, reloading.\n" );
	}

	FreeWorld();

	// see if we have a generated version of this
	static const byte BPROC_VERSION_BFG = 1;
	static const byte BPROC_VERSION_MOC_DATA = 2;
	static const byte BPROC_VERSION = BPROC_VERSION_MOC_DATA;


	static const unsigned int BPROC_MAGIC_BFG = ( 'P' << 24 ) | ( 'R' << 16 ) | ( 'O' << 8 ) | BPROC_VERSION_BFG;
	static const unsigned int BPROC_MAGIC = ( 'P' << 24 ) | ( 'R' << 16 ) | ( 'O' << 8 ) | BPROC_VERSION;
	bool loaded = false;
	idFileLocal file( fileSystem->OpenFileReadMemory( generatedFileName ) );
	if( file != NULL )
	{
		int numEntries = 0;
		int magic = 0;
		file->ReadBig( magic );
		if( magic == BPROC_MAGIC_BFG || magic == BPROC_MAGIC )
		{
			file->ReadBig( numEntries );
			file->ReadString( mapName );
			file->ReadBig( mapTimeStamp );
			loaded = true;
			for( int i = 0; i < numEntries; i++ )
			{
				idStrStatic< MAX_OSPATH > type;
				file->ReadString( type );
				type.ToLower();
				if( type == "model" )
				{
					idRenderModel* lastModel = ReadBinaryModel( file );
					if( lastModel == NULL )
					{
						loaded = false;
						break;
					}
					renderModelManager->AddModel( lastModel );
					localModels.Append( lastModel );
				}
				else if( type == "shadowmodel" && magic == BPROC_MAGIC_BFG )
				{
					// RB: the original BFG .bproc just saved all models as "shadowmodel"
					idRenderModel* lastModel = ReadBinaryModel( file );
					if( lastModel == NULL )
					{
						loaded = false;
						break;
					}
					renderModelManager->AddModel( lastModel );
					localModels.Append( lastModel );
				}
				else if( type == "interareaportals" )
				{
					ReadBinaryAreaPortals( file );
				}
				else if( type == "nodes" )
				{
					ReadBinaryNodes( file );
				}
				else if( type == "shadowhulls" )
				{
					// brush-recovery soft-shadow hulls (mirror of the .proc `shadowHulls` block)
					R_ReadBinaryShadowHulls( file );
				}
				else if( type == "shadowresiduals" )
				{
					// brush-recovery residual triangles (mirror of the .proc `shadowResiduals` block)
					R_ReadBinaryShadowResiduals( file );
				}
				else
				{
					idLib::Error( "Binary proc file failed, unexpected type %s\n", type.c_str() );
				}
			}
		}
	}

	if( !loaded )
	{
		src = new( TAG_RENDER ) idLexer( filename, LEXFL_NOSTRINGCONCAT | LEXFL_NODOLLARPRECOMPILE );
		if( !src->IsLoaded() )
		{
			common->Printf( "idRenderWorldLocal::InitFromMap: %s not found\n", filename.c_str() );
			ClearWorld();
			return false;
		}


		mapName = name;
		mapTimeStamp = currentTimeStamp;

		if( !src->ReadToken( &token ) || token.Icmp( PROC_FILE_ID ) )
		{
			common->Printf( "idRenderWorldLocal::InitFromMap: bad id '%s' instead of '%s'\n", token.c_str(), PROC_FILE_ID );
			delete src;
			return false;
		}

		int numEntries = 0;
		idFileLocal outputFile( fileSystem->OpenFileWrite( generatedFileName, "fs_basepath" ) );
		if( outputFile != NULL )
		{
			int magic = BPROC_MAGIC;
			outputFile->WriteBig( magic );
			outputFile->WriteBig( numEntries );
			outputFile->WriteString( mapName );
			outputFile->WriteBig( mapTimeStamp );
		}

		// parse the file
		while( 1 )
		{
			if( !src->ReadToken( &token ) )
			{
				break;
			}

			common->UpdateLevelLoadPacifier();


			if( token == "model" )
			{
				lastModel = ParseModel( src, name, currentTimeStamp, outputFile );

				// add it to the model manager list
				renderModelManager->AddModel( lastModel );

				// save it in the list to free when clearing this map
				localModels.Append( lastModel );

				numEntries++;

				continue;
			}

			if( token == "shadowModel" )
			{
				// RB: just parse the model but don't do anything with it
				//lastModel = ParseShadowModel( src, outputFile );
				src->SkipBracedSection();
				lastModel = NULL;
				continue;
			}

			if( token == "interAreaPortals" )
			{
				ParseInterAreaPortals( src, outputFile );

				numEntries++;
				continue;
			}

			if( token == "nodes" )
			{
				ParseNodes( src, outputFile );

				numEntries++;
				continue;
			}

			// brush-recovery soft shadows: additive, presence-gated hull block (Phase 0). Absent on
			// unrelated/old maps => no hulls => triangle fallback. Mirrored into the .bproc via outputFile.
			if( token == "shadowHulls" )
			{
				R_ParseShadowHulls( src, outputFile );

				numEntries++;
				continue;
			}

			// brush-recovery residual triangles (the non-hull-emittable brushes), streamed alongside the hulls.
			if( token == "shadowResiduals" )
			{
				R_ParseShadowResiduals( src, outputFile );

				numEntries++;
				continue;
			}

			src->Error( "idRenderWorldLocal::InitFromMap: bad token \"%s\"", token.c_str() );
		}

		delete src;

		if( outputFile != NULL )
		{
			outputFile->Seek( 0, FS_SEEK_SET );
			int magic = BPROC_MAGIC;
			outputFile->WriteBig( magic );
			outputFile->WriteBig( numEntries );
		}

	}



	// if it was a trivial map without any areas, create a single area
	if( !numPortalAreas )
	{
		ClearWorld();
	}

	// find the points where we can early-our of reference pushing into the BSP tree
	CommonChildrenArea_r( &areaNodes[0] );

	AddWorldModelEntities();
	ClearPortalStates();
	SetupLightGrid();

	// done!
	return true;
}

/*
=====================
idRenderWorldLocal::ClearPortalStates
=====================
*/
void idRenderWorldLocal::ClearPortalStates()
{
	// all portals start off open
	for( int i = 0; i < numInterAreaPortals; i++ )
	{
		doublePortals[i].blockingBits = PS_BLOCK_NONE;
	}

	// flood fill all area connections
	for( int i = 0; i < numPortalAreas; i++ )
	{
		for( int j = 0; j < NUM_PORTAL_ATTRIBUTES; j++ )
		{
			connectedAreaNum++;
			FloodConnectedAreas( &portalAreas[i], j );
		}
	}
}

/*
=====================
idRenderWorldLocal::AddWorldModelEntities
=====================
*/
void idRenderWorldLocal::AddWorldModelEntities()
{
	// add the world model for each portal area
	// we can't just call AddEntityDef, because that would place the references
	// based on the bounding box, rather than explicitly into the correct area
	for( int i = 0; i < numPortalAreas; i++ )
	{
		common->UpdateLevelLoadPacifier();

		idRenderEntityLocal*	 def = new( TAG_RENDER_ENTITY ) idRenderEntityLocal;

		// try and reuse a free spot
		int index = entityDefs.FindNull();
		if( index == -1 )
		{
			index = entityDefs.Append( def );
		}
		else
		{
			entityDefs[index] = def;
		}

		def->index = index;
		def->world = this;

		def->parms.hModel = renderModelManager->FindModel( va( "_area%i", i ) );
		if( def->parms.hModel->IsDefaultModel() || !def->parms.hModel->IsStaticWorldModel() )
		{
			common->Error( "idRenderWorldLocal::InitFromMap: bad area model lookup" );
		}

		idRenderModel* hModel = def->parms.hModel;

		for( int j = 0; j < hModel->NumSurfaces(); j++ )
		{
			const modelSurface_t* surf = hModel->Surface( j );

			if( surf->shader->GetName() == idStr( "textures/smf/portal_sky" ) ||
					surf->shader->IsPortalSky() )
			{
				def->needsPortalSky = true;
			}
		}

		// the local and global reference bounds are the same for area models
		def->localReferenceBounds = def->parms.hModel->Bounds();
		def->globalReferenceBounds = def->parms.hModel->Bounds();

		def->parms.axis[0][0] = 1.0f;
		def->parms.axis[1][1] = 1.0f;
		def->parms.axis[2][2] = 1.0f;

		// in case an explicit shader is used on the world, we don't
		// want it to have a 0 alpha or color
		def->parms.shaderParms[0] = 1.0f;
		def->parms.shaderParms[1] = 1.0f;
		def->parms.shaderParms[2] = 1.0f;
		def->parms.shaderParms[3] = 1.0f;

		R_DeriveEntityData( def );

		portalArea_t* area = &portalAreas[i];
		AddEntityRefToArea( def, area );

		// RB: remember BSP area AABB for quick lookup later
		area->globalBounds = def->globalReferenceBounds;
	}
}

/*
=====================
CheckAreaForPortalSky
=====================
*/
bool idRenderWorldLocal::CheckAreaForPortalSky( int areaNum )
{
	assert( areaNum >= 0 && areaNum < numPortalAreas );

	for( areaReference_t* ref = portalAreas[areaNum].entityRefs.areaNext; ref->entity; ref = ref->areaNext )
	{
		assert( ref->area == &portalAreas[areaNum] );

		if( ref->entity && ref->entity->needsPortalSky )
		{
			return true;
		}
	}

	return false;
}

/*
=====================
ResetLocalRenderModels
=====================
*/
void idRenderWorldLocal::ResetLocalRenderModels()
{
	localModels.Clear();	// Clear out the list when switching between expansion packs, so InitFromMap doesn't try to delete the list whose content has already been deleted by the model manager being re-started
}
