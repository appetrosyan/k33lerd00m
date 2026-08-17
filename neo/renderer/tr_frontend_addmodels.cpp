/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2014-2024 Robert Beckebans
Copyright (C) 2014-2016 Kot in Action Creative Artel

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

#if defined(USE_INTRINSICS_SSE)
	#if MOC_MULTITHREADED
		#include "CullingThreadPool.h"
	#else
		#include "../libs/moc/MaskedOcclusionCulling.h"
	#endif
#endif

#include "RenderCommon.h"
#include "RenderCapture.h"
#include "Model_local.h"

extern idCVar r_useRTShadows;	// RT shadows need occluders whose shadow is off-view (RenderSystem_init.cpp)
extern idCVar r_useSoftShadowVolumes;	// soft shadow volumes reuse the stencil shadow-volume geometry (RenderSystem_init.cpp)
extern idCVar r_softShadowKeepOffViewCasters;	// keep off-view casters whose PENUMBRA reaches the view (RenderSystem_init.cpp)
extern idCVar r_rtShadowCullOffView;	// trim the shadow TLAS: cull casters whose shadow misses the view (RtShadowsPass.cpp)
// dynamic stencil shadow volume for moved / non-static casters, built per-frame from the
// model's static silEdges + doubled shadowCache against the current light (defined in Interaction.cpp).
srfTriangles_t* R_CreateInteractionShadowVolume( const idRenderEntityLocal* ent, const srfTriangles_t* tri, const idRenderLightLocal* light );
// analytic soft shadows: per-silhouette-edge world-space edges for per-fragment coverage (Interaction.cpp).
void R_CollectPenumbraEdges( const idRenderEntityLocal* ent, const srfTriangles_t* tri, const idRenderLightLocal* light,
							 float penumbraSize, const float* modelToWorld,
							 softShadowEdge_t** outEdges, int* outNumEdges );
// analytic soft shadows: FRONT-FACE coverage stream v2 (3 float4 per caster triangle) - accurate receiver-disk
// coverage, temporally stable; the light-silhouette path above undershoots off-axis (Interaction.cpp).
void R_CollectPenumbraFaces( const idRenderEntityLocal* ent, const srfTriangles_t* tri, const idRenderLightLocal* light,
							 float penumbraSize, const float* modelToWorld,
							 idVec4** outElems, int* outNumElems );
extern idCVar r_shadowPenumbraSize;	// soft shadow volumes: light source radius (RenderSystem_init.cpp)
extern idCVar r_softShadowFaceCoverage;	// 1 = stream caster faces + front-face coverage instead of light silhouette

idCVar r_skipStaticShadows( "r_skipStaticShadows", "0", CVAR_RENDERER | CVAR_BOOL, "skip static shadows" );
idCVar r_skipDynamicShadows( "r_skipDynamicShadows", "0", CVAR_RENDERER | CVAR_BOOL, "skip dynamic shadows" );
idCVar r_useParallelAddModels( "r_useParallelAddModels", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_NOCHEAT, "add all models in parallel with jobs" );
idCVar r_useParallelAddShadows( "r_useParallelAddShadows", "1", CVAR_RENDERER | CVAR_INTEGER | CVAR_NOCHEAT, "0 = off, 1 = threaded", 0, 1 );
idCVar r_forceShadowCaps( "r_forceShadowCaps", "0", CVAR_RENDERER | CVAR_BOOL, "0 = skip rendering shadow caps if view is outside shadow volume, 1 = always render shadow caps" );
// WIP: restore stencil shadow volumes. When on, static shadow-casting surfaces build a
// stencil shadow-volume drawSurf (into vLight->globalShadows/localShadows) instead of a
// shadow-map caster. M4 will fold this into an r_shadowMethod selector, default stencil.
idCVar r_useStencilShadows( "r_useStencilShadows", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL | CVAR_NEW, "use stencil shadow volumes instead of shadow maps (WIP)" );

// M2 diagnostics: cumulative tallies of stencil-volume caster gate outcomes (see R_AddSingleModel).
// Printed by DrawInteractions when r_showShadows is set; names the missing prerequisite when 0 volumes build.
int fe_stencilBuilt = 0;
int fe_softEdgesCollected = 0;	// cumulative soft-shadow silhouette edges collected (diagnostic: 0 => soft-wedge inert)
int fe_occludersBuilt = 0;		// cumulative PCSS-locator atlas OCCLUDER surfs built (diagnostic: 0 => atlas empty for soft lights)
int fe_rejSilEdges = 0;
int fe_rejSurfInter = 0;
int fe_rejNumIdx = 0;
int fe_rejIdxStale = 0;
int fe_rejShadowCache = 0;
// RB begin
idCVar r_forceShadowMapsOnAlphaTestedSurfaces( "r_forceShadowMapsOnAlphaTestedSurfaces", "1", CVAR_RENDERER | CVAR_BOOL, "0 = same shadowing as with stencil shadows, 1 = ignore noshadows for alpha tested materials" );
// RB end
// foresthale 2014-11-24: cvar to control the material lod flags - this is the distance at which a mesh switches from lod1 to lod2, where lod3 will appear at this distance *2, lod4 at *4, and persistentLOD keyword will disable the max distance check (thus extending this LOD to all further distances, rather than disappearing)
idCVar r_lodMaterialDistance( "r_lodMaterialDistance", "500", CVAR_RENDERER | CVAR_FLOAT, "surfaces further than this distance will use lower quality versions (if their material uses the lod1-4 keywords, persistentLOD disables the max distance checks)" );

static const float CHECK_BOUNDS_EPSILON = 1.0f;

// Stencil shadow volumes: how far to stretch the near-clip expansion of the inside test.
// (In theory should vary with FOV.) Restored from DOOM-3-BFG jobs/ShadowShared.
static const float INSIDE_SHADOW_VOLUME_EXTRA_STRETCH = 4.0f;

/*
======================
R_ViewPotentiallyInsideInfiniteShadowVolume

If we know that we are "off to the side" of an infinite shadow volume, we can draw it without
caps in Z-pass mode - which avoids the projected-to-infinity far cap entirely. Only when the
view might be inside the volume do we need Z-fail with caps. Restored verbatim from the deleted
DOOM-3-BFG jobs/ShadowShared.cpp so the stencil pass can choose Z-pass at a distance (the common
case) instead of forcing Z-fail everywhere - forcing Z-fail draws the far cap for every caster,
and against distant scene depth that cap paints phantom shadows down long corridors.
======================
*/
static bool R_ViewPotentiallyInsideInfiniteShadowVolume( const idBounds& occluderBounds, const idVec3& localLight, const idVec3& localView, const float zNear )
{
	// Expand the bounds to account for the near clip plane, because the view could be
	// mathematically outside, but if the near clip plane chops a volume edge then the
	// Z-pass rendering would fail.
	const idBounds expandedBounds = occluderBounds.Expand( zNear );

	// If the view is inside the geometry bounding box then the view is also inside the shadow projection.
	if( expandedBounds.ContainsPoint( localView ) )
	{
		return true;
	}

	// If the light is inside the geometry bounding box then the shadow is projected in all
	// directions and any view position is inside the infinite shadow projection.
	if( expandedBounds.ContainsPoint( localLight ) )
	{
		return true;
	}

	// If the line from localLight to localView intersects the geometry bounding box then the
	// view is inside the infinite shadow projection.
	if( expandedBounds.LineIntersection( localLight, localView ) )
	{
		return true;
	}

	// The view is definitely not inside the projected shadow.
	return false;
}



/*
==================
R_ClearEntityDefDynamicModel

If we know the reference bounds stays the same, we
only need to do this on entity update, not the full
R_FreeEntityDefDerivedData
==================
*/
void R_ClearEntityDefDynamicModel( idRenderEntityLocal* def )
{
	// free all the interaction surfaces
	for( idInteraction* inter = def->firstInteraction; inter != NULL && !inter->IsEmpty(); inter = inter->entityNext )
	{
		inter->FreeSurfaces();
	}

	// clear the dynamic model if present
	if( def->dynamicModel )
	{
		// this is copied from cachedDynamicModel, so it doesn't need to be freed
		def->dynamicModel = NULL;
	}
	def->dynamicModelFrameCount = 0;
}

/*
==================
R_IssueEntityDefCallback
==================
*/
bool R_IssueEntityDefCallback( idRenderEntityLocal* def )
{
	idBounds oldBounds = def->localReferenceBounds;

	bool update;
	if( tr.viewDef != NULL )
	{
		update = def->parms.callback( &def->parms, &tr.viewDef->renderView );
	}
	else
	{
		update = def->parms.callback( &def->parms, NULL );
	}
	tr.pc.c_entityDefCallbacks++;

	if( def->parms.hModel == NULL )
	{
		common->Error( "R_IssueEntityDefCallback: dynamic entity callback didn't set model" );
	}

	if( r_checkBounds.GetBool() )
	{
		if(	oldBounds[0][0] > def->localReferenceBounds[0][0] + CHECK_BOUNDS_EPSILON ||
				oldBounds[0][1] > def->localReferenceBounds[0][1] + CHECK_BOUNDS_EPSILON ||
				oldBounds[0][2] > def->localReferenceBounds[0][2] + CHECK_BOUNDS_EPSILON ||
				oldBounds[1][0] < def->localReferenceBounds[1][0] - CHECK_BOUNDS_EPSILON ||
				oldBounds[1][1] < def->localReferenceBounds[1][1] - CHECK_BOUNDS_EPSILON ||
				oldBounds[1][2] < def->localReferenceBounds[1][2] - CHECK_BOUNDS_EPSILON )
		{
			common->Printf( "entity %i callback extended reference bounds\n", def->index );
		}
	}

	return update;
}

/*
===================
R_EntityDefDynamicModel

This is also called by the game code for idRenderWorldLocal::ModelTrace(), and idRenderWorldLocal::Trace() which is bad for performance...

Issues a deferred entity callback if necessary.
If the model isn't dynamic, it returns the original.
Returns the cached dynamic model if present, otherwise creates it.
===================
*/
idRenderModel* R_EntityDefDynamicModel( idRenderEntityLocal* def )
{
	if( def->dynamicModelFrameCount == tr.frameCount )
	{
		return def->dynamicModel;
	}

	// allow deferred entities to construct themselves
	bool callbackUpdate;
	if( def->parms.callback != NULL )
	{
		SCOPED_PROFILE_EVENT( "R_IssueEntityDefCallback" );
		callbackUpdate = R_IssueEntityDefCallback( def );
	}
	else
	{
		callbackUpdate = false;
	}

	idRenderModel* model = def->parms.hModel;

	if( model == NULL )
	{
		common->Error( "R_EntityDefDynamicModel: NULL model" );
		return NULL;
	}

	if( model->IsDynamicModel() == DM_STATIC )
	{
		def->dynamicModel = NULL;
		def->dynamicModelFrameCount = 0;
		return model;
	}

	// continously animating models (particle systems, etc) will have their snapshot updated every single view
	if( callbackUpdate || ( model->IsDynamicModel() == DM_CONTINUOUS && def->dynamicModelFrameCount != tr.frameCount ) )
	{
		R_ClearEntityDefDynamicModel( def );
	}

	// if we don't have a snapshot of the dynamic model, generate it now
	if( def->dynamicModel == NULL )
	{
		SCOPED_PROFILE_EVENT( "InstantiateDynamicModel" );

		// instantiate the snapshot of the dynamic model, possibly reusing memory from the cached snapshot
		def->cachedDynamicModel = model->InstantiateDynamicModel( &def->parms, tr.viewDef, def->cachedDynamicModel );

		if( def->cachedDynamicModel != NULL && r_checkBounds.GetBool() )
		{
			idBounds b = def->cachedDynamicModel->Bounds();
			if(	b[0][0] < def->localReferenceBounds[0][0] - CHECK_BOUNDS_EPSILON ||
					b[0][1] < def->localReferenceBounds[0][1] - CHECK_BOUNDS_EPSILON ||
					b[0][2] < def->localReferenceBounds[0][2] - CHECK_BOUNDS_EPSILON ||
					b[1][0] > def->localReferenceBounds[1][0] + CHECK_BOUNDS_EPSILON ||
					b[1][1] > def->localReferenceBounds[1][1] + CHECK_BOUNDS_EPSILON ||
					b[1][2] > def->localReferenceBounds[1][2] + CHECK_BOUNDS_EPSILON )
			{
				common->Printf( "entity %i dynamic model exceeded reference bounds\n", def->index );
			}
		}

		def->dynamicModel = def->cachedDynamicModel;
		def->dynamicModelFrameCount = tr.frameCount;
	}

	// set model depth hack value
	if( def->dynamicModel != NULL && model->DepthHack() != 0.0f && tr.viewDef != NULL )
	{
		idPlane eye, clip;
		idVec3 ndc;
		R_TransformModelToClip( def->parms.origin, tr.viewDef->worldSpace.modelViewMatrix, tr.viewDef->projectionMatrix, eye, clip );
		R_TransformClipToDevice( clip, ndc );
		def->parms.modelDepthHack = model->DepthHack() * ( 1.0f - ndc.z );
	}
	else
	{
		def->parms.modelDepthHack = 0.0f;
	}

	return def->dynamicModel;
}

/*
===================
R_SetupDrawSurfShader
===================
*/
void R_SetupDrawSurfShader( drawSurf_t* drawSurf, const idMaterial* shader, const renderEntity_t* renderEntity )
{
	drawSurf->material = shader;
	drawSurf->sort = shader->GetSort();

	// process the shader expressions for conditionals / color / texcoords
	const float*	constRegs = shader->ConstantRegisters();
	if( likely( constRegs != NULL ) )
	{
		// shader only uses constant values
		drawSurf->shaderRegisters = constRegs;
	}
	else
	{
		// by default evaluate with the entityDef's shader parms
		const float* shaderParms = renderEntity->shaderParms;

		// a reference shader will take the calculated stage color value from another shader
		// and use that for the parm0-parm3 of the current shader, which allows a stage of
		// a light model and light flares to pick up different flashing tables from
		// different light shaders
		float generatedShaderParms[MAX_ENTITY_SHADER_PARMS];
		if( unlikely( renderEntity->referenceShader != NULL ) )
		{
			// evaluate the reference shader to find our shader parms
			float refRegs[MAX_EXPRESSION_REGISTERS];
			renderEntity->referenceShader->EvaluateRegisters( refRegs, renderEntity->shaderParms,
					tr.viewDef->renderView.shaderParms,
					tr.viewDef->renderView.time[renderEntity->timeGroup] * 0.001f, renderEntity->referenceSound );

			const shaderStage_t* pStage = renderEntity->referenceShader->GetStage( 0 );

			memcpy( generatedShaderParms, renderEntity->shaderParms, sizeof( generatedShaderParms ) );
			generatedShaderParms[0] = refRegs[ pStage->color.registers[0] ];
			generatedShaderParms[1] = refRegs[ pStage->color.registers[1] ];
			generatedShaderParms[2] = refRegs[ pStage->color.registers[2] ];

			shaderParms = generatedShaderParms;
		}

		// allocate frame memory for the shader register values
		float* regs = ( float* )R_FrameAlloc( shader->GetNumRegisters() * sizeof( float ), FRAME_ALLOC_SHADER_REGISTER );
		drawSurf->shaderRegisters = regs;

		// process the shader expressions for conditionals / color / texcoords
		shader->EvaluateRegisters( regs, shaderParms, tr.viewDef->renderView.shaderParms,
								   tr.viewDef->renderView.time[renderEntity->timeGroup] * 0.001f, renderEntity->referenceSound );
	}
}

/*
===================
R_SetupDrawSurfJoints
===================
*/
void R_SetupDrawSurfJoints( drawSurf_t* drawSurf, const srfTriangles_t* tri, const idMaterial* shader, nvrhi::ICommandList* commandList )
{
	// RB: added check wether GPU skinning is available at all
	if( tri->staticModelWithJoints == NULL || !r_useGPUSkinning.GetBool() )
	{
		drawSurf->jointCache = 0;
		return;
	}
	// RB end

	idRenderModelStatic* model = tri->staticModelWithJoints;
	assert( model->jointsInverted != NULL );

	if( !vertexCache.CacheIsCurrent( model->jointsInvertedBuffer ) )
	{
		model->jointsInvertedBuffer = vertexCache.AllocJoint( model->jointsInverted, model->numInvertedJoints, sizeof( idJointMat ), commandList );
	}
	drawSurf->jointCache = model->jointsInvertedBuffer;
}

/*
===================
R_AddSingleModel

May be run in parallel.

Here is where dynamic models actually get instantiated, and necessary
interaction surfaces get created. This is all done on a sort-by-model
basis to keep source data in cache (most likely L2) as any interactions
and shadows are generated, since dynamic models will typically be lit by
two or more lights.
===================
*/
void R_AddSingleModel( viewEntity_t* vEntity )
{
	// we will add all interaction surfs here, to be chained to the lights in later serial code
	vEntity->drawSurfs = NULL;

	// globals we really should pass in...
	const viewDef_t* viewDef = tr.viewDef;

	idRenderEntityLocal* entityDef = vEntity->entityDef;
	const renderEntity_t* renderEntity = &entityDef->parms;
	const idRenderWorldLocal* world = entityDef->world;

	if( viewDef->isXraySubview && entityDef->parms.xrayIndex == 1 )
	{
		return;
	}
	else if( !viewDef->isXraySubview && entityDef->parms.xrayIndex == 2 )
	{
		return;
	}

	SCOPED_PROFILE_EVENT( renderEntity->hModel == NULL ? "Unknown Model" : renderEntity->hModel->Name() );

	// calculate the znear for testing whether or not the view is inside a shadow projection
	const float znear = ( viewDef->renderView.cramZNear ) ? ( r_znear.GetFloat() * 0.25f ) : r_znear.GetFloat();

	// if the entity wasn't seen through a portal chain, it was added just for light shadows
	const bool modelIsVisible = !vEntity->scissorRect.IsEmpty();
	const bool addInteractions = modelIsVisible && ( !viewDef->isXraySubview || entityDef->parms.xrayIndex == 2 );
	const int entityIndex = entityDef->index;

	//---------------------------
	// Find which of the visible lights contact this entity
	//
	// If the entity doesn't accept light or cast shadows from any surface,
	// this can be skipped.
	//
	// OPTIMIZE: world areas can assume all referenced lights are used
	//---------------------------
	int	numContactedLights = 0;
	static const int MAX_CONTACTED_LIGHTS = 128;
	viewLight_t* contactedLights[MAX_CONTACTED_LIGHTS];
	idInteraction* staticInteractions[MAX_CONTACTED_LIGHTS];

	if( renderEntity->hModel == NULL ||
			renderEntity->hModel->ModelHasInteractingSurfaces() ||
			renderEntity->hModel->ModelHasShadowCastingSurfaces() )
	{
		SCOPED_PROFILE_EVENT( "Find lights" );
		for( viewLight_t* vLight = viewDef->viewLights; vLight != NULL; vLight = vLight->next )
		{
			if( vLight->scissorRect.IsEmpty() )
			{
				continue;
			}
			if( vLight->entityInteractionState != NULL )
			{
				// new code path, everything was done in AddLight
				if( vLight->entityInteractionState[entityIndex] == viewLight_t::INTERACTION_YES )
				{
					contactedLights[numContactedLights] = vLight;
					staticInteractions[numContactedLights] = world->interactionTable[vLight->lightDef->index * world->interactionTableWidth + entityIndex];
					if( ++numContactedLights == MAX_CONTACTED_LIGHTS )
					{
						break;
					}
				}
				continue;
			}

			const idRenderLightLocal* lightDef = vLight->lightDef;

			if( !lightDef->globalLightBounds.IntersectsBounds( entityDef->globalReferenceBounds ) )
			{
				continue;
			}

			if( R_CullModelBoundsToLight( lightDef, entityDef->localReferenceBounds, entityDef->modelRenderMatrix ) )
			{
				continue;
			}

			if( !modelIsVisible )
			{
				// some lights have their center of projection outside the world
				if( lightDef->areaNum != -1 )
				{
					// if no part of the model is in an area that is connected to
					// the light center (it is behind a solid, closed door), we can ignore it
					bool areasConnected = false;
					for( areaReference_t* ref = entityDef->entityRefs; ref != NULL; ref = ref->ownerNext )
					{
						if( world->AreasAreConnected( lightDef->areaNum, ref->area->areaNum, PS_BLOCK_VIEW ) )
						{
							areasConnected = true;
							break;
						}
					}
					if( areasConnected == false )
					{
						// can't possibly be seen or shadowed
						continue;
					}
				}

				// check more precisely for shadow visibility
				idBounds shadowBounds;
				R_ShadowBounds( entityDef->globalReferenceBounds, lightDef->globalLightBounds, lightDef->globalLightOrigin, shadowBounds );

				// this doesn't say that the shadow can't effect anything, only that it can't
				// effect anything in the view. Only ray-traced shadows need off-view casters
				// (their TLAS gathers occluders behind the view); stencil shadow volumes render
				// into the view, so a caster whose shadow bounds miss the frustum can't affect it
				// and MUST be culled - otherwise its volume's far cap paints phantom shadows on
				// distant surfaces that vanish as the caster comes into view. So keep off-view
				// casters only when RT shadows are the active method, not merely when the archived
				// r_useRTShadows cvar lingers on alongside stencil.
				// Shadow-method precedence: RT wins when on, else stencil, else shadow maps. RT is the
				// only method that needs off-view casters (its TLAS traces them), so keep them exactly
				// when RT is the active method - stencil renders volumes into the view and must cull.
				// TLAS trim (r_rtShadowCullOffView): a caster shadowing an in-view receiver has that
				// receiver inside shadowBounds, so shadowBounds intersects the frustum and survives the
				// cull below (incl. behind-camera casters) - the only casters dropped are those whose
				// shadow lands nowhere visible, which RT does not need either. So RT can honour the same
				// cull stencil uses instead of keeping EVERY off-view caster. Set the cvar 0 to restore
				// the keep-everything behaviour if a real off-view shadow goes missing.
				// R_ShadowBounds bounds the HARD shadow (umbra). A soft penumbra flares BEYOND it, so an
					// off-view caster whose umbra bounds miss the frustum but whose penumbra reaches into the
					// view is wrongly culled here - and as the camera moves such casters cross this coarse
					// boundary and their whole penumbra wedge pops in/out (the "flicker all over the screen").
					// Soft shadows, like RT, therefore need off-view casters; the wedge pixel shader gates
					// coverage analytically, so keeping them paints no phantom on distant surfaces. 0 to A/B.
					const bool keepOffViewCasters = ( r_useRTShadows.GetBool() && !r_rtShadowCullOffView.GetBool() )
							|| ( r_useSoftShadowVolumes.GetBool() && r_softShadowKeepOffViewCasters.GetBool() );
				if( !keepOffViewCasters && idRenderMatrix::CullBoundsToMVP( viewDef->worldSpace.mvp, shadowBounds ) )
				{
					continue;
				}
			}
			contactedLights[numContactedLights] = vLight;
			staticInteractions[numContactedLights] = world->interactionTable[vLight->lightDef->index * world->interactionTableWidth + entityIndex];
			if( ++numContactedLights == MAX_CONTACTED_LIGHTS )
			{
				break;
			}
		}
	}

	// if we aren't visible and none of the shadows stretch into the view,
	// we don't need to do anything else
	if( !modelIsVisible && numContactedLights == 0 )
	{
		return;
	}

	//---------------------------
	// create a dynamic model if the geometry isn't static
	//---------------------------
	idRenderModel* model = R_EntityDefDynamicModel( entityDef );
	if( model == NULL || model->NumSurfaces() <= 0 )
	{
		return;
	}

	// add the lightweight blood decal surfaces if the model is directly visible
	if( modelIsVisible )
	{
		assert( !vEntity->scissorRect.IsEmpty() );

		if( entityDef->decals != NULL && !r_skipDecals.GetBool() )
		{
			entityDef->decals->CreateDeferredDecals( model );

			unsigned int numDrawSurfs = entityDef->decals->GetNumDecalDrawSurfs();
			for( unsigned int i = 0; i < numDrawSurfs; i++ )
			{
				drawSurf_t* decalDrawSurf = entityDef->decals->CreateDecalDrawSurf( vEntity, i );
				if( decalDrawSurf != NULL )
				{
					decalDrawSurf->linkChain = NULL;
					decalDrawSurf->nextOnLight = vEntity->drawSurfs;
					vEntity->drawSurfs = decalDrawSurf;
				}
			}
		}

		if( entityDef->overlays != NULL && !r_skipOverlays.GetBool() )
		{
			entityDef->overlays->CreateDeferredOverlays( model );

			unsigned int numDrawSurfs = entityDef->overlays->GetNumOverlayDrawSurfs();
			for( unsigned int i = 0; i < numDrawSurfs; i++ )
			{
				drawSurf_t* overlayDrawSurf = entityDef->overlays->CreateOverlayDrawSurf( vEntity, model, i );
				if( overlayDrawSurf != NULL )
				{
					overlayDrawSurf->linkChain = NULL;
					overlayDrawSurf->nextOnLight = vEntity->drawSurfs;
					vEntity->drawSurfs = overlayDrawSurf;
				}
			}
		}
	}

	//---------------------------
	// copy matrix related stuff for back-end use
	// and setup a render matrix for faster culling
	//---------------------------
	vEntity->modelDepthHack = renderEntity->modelDepthHack;
	vEntity->weaponDepthHack = renderEntity->weaponDepthHack;
	vEntity->skipMotionBlur = renderEntity->skipMotionBlur;

	memcpy( vEntity->modelMatrix, entityDef->modelMatrix, sizeof( vEntity->modelMatrix ) );
	R_MatrixMultiply( entityDef->modelMatrix, viewDef->worldSpace.modelViewMatrix, vEntity->modelViewMatrix );

	idRenderMatrix viewMat;
	idRenderMatrix::Transpose( *( idRenderMatrix* )vEntity->modelViewMatrix, viewMat );
	idRenderMatrix::Multiply( viewDef->projectionRenderMatrix, viewMat, vEntity->mvp );
	idRenderMatrix::Multiply( viewDef->unjitteredProjectionRenderMatrix, viewMat, vEntity->unjitteredMVP );
	if( renderEntity->weaponDepthHack )
	{
		idRenderMatrix::ApplyDepthHack( vEntity->mvp );
	}
	if( renderEntity->modelDepthHack != 0.0f )
	{
		idRenderMatrix::ApplyModelDepthHack( vEntity->mvp, renderEntity->modelDepthHack );
	}

	// local light and view origins are used to determine if the view is definitely outside
	// an extruded shadow volume, which means we can skip drawing the end caps
	idVec3 localViewOrigin;
	R_GlobalPointToLocal( vEntity->modelMatrix, viewDef->renderView.vieworg, localViewOrigin );

	//---------------------------
	// add all the model surfaces
	//---------------------------
	for( int surfaceNum = 0; surfaceNum < model->NumSurfaces(); surfaceNum++ )
	{
		const modelSurface_t* surf = model->Surface( surfaceNum );

		// for debugging, only show a single surface at a time
		if( r_singleSurface.GetInteger() >= 0 && surfaceNum != r_singleSurface.GetInteger() )
		{
			continue;
		}

		srfTriangles_t* tri = surf->geometry;
		if( tri == NULL )
		{
			continue;
		}
		if( tri->numIndexes == 0 )
		{
			continue;		// happens for particles
		}
		const idMaterial* shader = surf->shader;
		if( shader == NULL )
		{
			continue;
		}

		// motorsep 11-24-2014; checking for LOD surface for LOD1 iteration
		if( shader->IsLOD() )
		{
			// foresthale 2014-11-24: calculate the bounds and get the distance from camera to bounds
			idBounds& localBounds = tri->bounds;
			if( tri->staticModelWithJoints )
			{
				// skeletal models have difficult to compute bounds for surfaces, so use the whole entity
				localBounds = vEntity->entityDef->localReferenceBounds;
			}
			const float* bounds = localBounds.ToFloatPtr();
			idVec3 nearestPointOnBounds = localViewOrigin;
			nearestPointOnBounds.x = Max( nearestPointOnBounds.x, bounds[0] );
			nearestPointOnBounds.x = Min( nearestPointOnBounds.x, bounds[3] );
			nearestPointOnBounds.y = Max( nearestPointOnBounds.y, bounds[1] );
			nearestPointOnBounds.y = Min( nearestPointOnBounds.y, bounds[4] );
			nearestPointOnBounds.z = Max( nearestPointOnBounds.z, bounds[2] );
			nearestPointOnBounds.z = Min( nearestPointOnBounds.z, bounds[5] );
			idVec3 delta = nearestPointOnBounds - localViewOrigin;
			float distance = delta.LengthFast();

			if( !shader->IsLODVisibleForDistance( distance, r_lodMaterialDistance.GetFloat() ) )
			{
				continue;
			}
		}

		// foresthale 2014-09-01: don't skip surfaces that use the "forceShadows" flag
		if( !shader->IsDrawn() && !shader->SurfaceCastsShadow() )
		{
			continue;		// collision hulls, etc
		}

		// RemapShaderBySkin
		if( entityDef->parms.customShader != NULL )
		{
			// this is sort of a hack, but causes deformed surfaces to map to empty surfaces,
			// so the item highlight overlay doesn't highlight the autosprite surface
			if( shader->Deform() )
			{
				continue;
			}
			shader = entityDef->parms.customShader;
		}
		else if( entityDef->parms.customSkin )
		{
			shader = entityDef->parms.customSkin->RemapShaderBySkin( shader );
			if( shader == NULL )
			{
				continue;
			}
			// foresthale 2014-09-01: don't skip surfaces that use the "forceShadows" flag
			if( !shader->IsDrawn() && !shader->SurfaceCastsShadow() )
			{
				continue;
			}
		}

		// optionally override with the renderView->globalMaterial
		if( tr.primaryRenderView.globalMaterial != NULL )
		{
			shader = tr.primaryRenderView.globalMaterial;
		}

		SCOPED_PROFILE_EVENT( shader->GetName() );

		// debugging tool to make sure we have the correct pre-calculated bounds
		if( r_checkBounds.GetBool() )
		{
			for( int j = 0; j < tri->numVerts; j++ )
			{
				int k;
				for( k = 0; k < 3; k++ )
				{
					if( tri->verts[j].xyz[k] > tri->bounds[1][k] + CHECK_BOUNDS_EPSILON
							|| tri->verts[j].xyz[k] < tri->bounds[0][k] - CHECK_BOUNDS_EPSILON )
					{
						common->Printf( "bad tri->bounds on %s:%s\n", entityDef->parms.hModel->Name(), shader->GetName() );
						break;
					}
					if( tri->verts[j].xyz[k] > entityDef->localReferenceBounds[1][k] + CHECK_BOUNDS_EPSILON
							|| tri->verts[j].xyz[k] < entityDef->localReferenceBounds[0][k] - CHECK_BOUNDS_EPSILON )
					{
						common->Printf( "bad referenceBounds on %s:%s\n", entityDef->parms.hModel->Name(), shader->GetName() );
						break;
					}
				}
				if( k != 3 )
				{
					break;
				}
			}
		}

		// view frustum culling for the precise surface bounds, which is tighter
		// than the entire entity reference bounds
		// If the entire model wasn't visible, there is no need to check the
		// individual surfaces.
		bool surfaceDirectlyVisible = modelIsVisible && !idRenderMatrix::CullBoundsToMVP( vEntity->mvp, tri->bounds );

		// RB: added check wether GPU skinning is available at all
		const bool gpuSkinned = ( tri->staticModelWithJoints != NULL && r_useGPUSkinning.GetBool() );

		//const char* shaderName = shader->GetName();
		//if( idStr::Cmp( shaderName, "textures/rock/sharprock_dark") == 0 )
		//{
		//	tr.pc.c_mocTests += 0;
		//}

#if defined(USE_INTRINSICS_SSE)

		const bool viewInsideSurface = tri->bounds.ContainsPoint( localViewOrigin );

		//if( viewInsideSurface && idStr::Cmp( shaderName, "models/weapons/berserk/fist") != 0 )
		//{
		//	tr.pc.c_mocTests += 1;
		//
		//	tr.viewDef->renderWorld->DebugBounds( colorCyan, tri->bounds, renderEntity->origin );
		//}

		// RB: test surface visibility by drawing the triangles of the bounds
		if( r_useMaskedOcclusionCulling.GetBool() && !viewInsideSurface && !viewDef->isMirror && !viewDef->isSubview )
		{
			if( //!model->IsStaticWorldModel() &&
				!renderEntity->weaponDepthHack && renderEntity->modelDepthHack == 0.0f )
			{
				idVec4 triVerts[8];

				tr.pc.c_mocIndexes += 36;
				tr.pc.c_mocVerts += 8;

				idBounds surfaceBounds;
#if 1
				if( gpuSkinned )
				{
					surfaceBounds = vEntity->entityDef->localReferenceBounds;
				}
				else
#endif
				{
					surfaceBounds = tri->bounds;
				}

				idRenderMatrix modelRenderMatrix;
				idRenderMatrix::CreateFromOriginAxis( renderEntity->origin, renderEntity->axis, modelRenderMatrix );

				idRenderMatrix inverseBaseModelProject;
				idRenderMatrix::OffsetScaleForBounds( modelRenderMatrix, surfaceBounds, inverseBaseModelProject );

				idRenderMatrix invProjectMVPMatrix;
				idRenderMatrix::Multiply( viewDef->worldSpace.unjitteredMVP, inverseBaseModelProject, invProjectMVPMatrix );

				tr.pc.c_mocTests += 1;

				// NOTE: unit cube instead of zeroToOne cube
				idVec4* verts = tr.maskedUnitCubeVerts;
				for( int i = 0; i < 8; i++ )
				{
					// transform to clip space
					invProjectMVPMatrix.TransformPoint( verts[i], triVerts[i] );
				}


				// backface none so objects are still visible where we run into
#if MOC_MULTITHREADED
				tr.maskedOcclusionThreaded->SetMatrix( NULL );
				MaskedOcclusionCulling::CullingResult result = tr.maskedOcclusionThreaded->TestTriangles( ( float* )triVerts, tr.maskedZeroOneCubeIndexes, 12, MaskedOcclusionCulling::BACKFACE_NONE );
#else
				MaskedOcclusionCulling::CullingResult result = tr.maskedOcclusionCulling->TestTriangles( ( float* )triVerts, tr.maskedZeroOneCubeIndexes, 12, NULL, MaskedOcclusionCulling::BACKFACE_NONE );
#endif
				if( result != MaskedOcclusionCulling::VISIBLE )
				{
					tr.pc.c_mocCulledSurfaces += 1;
					surfaceDirectlyVisible = false;
				}
			}
		}
#endif // #if defined(USE_INTRINSICS_SSE)

		//--------------------------
		// base drawing surface
		//--------------------------
		const float* shaderRegisters = NULL;
		drawSurf_t* baseDrawSurf = NULL;
		if( surfaceDirectlyVisible && shader->IsDrawn() )
		{
			// make sure we have an ambient cache and all necessary normals / tangents
			if( !vertexCache.CacheIsCurrent( tri->indexCache ) )
			{
				tri->indexCache = vertexCache.AllocIndex( tri->indexes, tri->numIndexes );
			}

			if( !vertexCache.CacheIsCurrent( tri->ambientCache ) )
			{
				// we are going to use it for drawing, so make sure we have the tangents and normals
				if( shader->ReceivesLighting() && !tri->tangentsCalculated )
				{
					assert( tri->staticModelWithJoints == NULL );
					R_DeriveTangents( tri );

					// RB: this was hit by parametric particle models ..
					//assert( false );	// this should no longer be hit
					// RB end
				}
				tri->ambientCache = vertexCache.AllocVertex( tri->verts, tri->numVerts );
			}

			// add the surface for drawing
			// we can re-use some of the values for light interaction surfaces
			baseDrawSurf = ( drawSurf_t* )R_FrameAlloc( sizeof( *baseDrawSurf ), FRAME_ALLOC_DRAW_SURFACE );
			baseDrawSurf->frontEndGeo = tri;
			baseDrawSurf->space = vEntity;
			baseDrawSurf->scissorRect = vEntity->scissorRect;
			baseDrawSurf->extraGLState = 0;

			R_SetupDrawSurfShader( baseDrawSurf, shader, renderEntity );

			shaderRegisters = baseDrawSurf->shaderRegisters;

			// Check for deformations (eyeballs, flares, etc)
			const deform_t shaderDeform = shader->Deform();
			if( shaderDeform != DFRM_NONE )
			{
				drawSurf_t* deformDrawSurf = R_DeformDrawSurf( baseDrawSurf );
				if( deformDrawSurf != NULL )
				{
					// any deforms may have created multiple draw surfaces
					for( drawSurf_t* surf = deformDrawSurf, * next = NULL; surf != NULL; surf = next )
					{
						next = surf->nextOnLight;

						surf->linkChain = NULL;
						surf->nextOnLight = vEntity->drawSurfs;
						vEntity->drawSurfs = surf;
					}
				}
			}

			// Most deform source surfaces do not need to be rendered.
			// However, particles are rendered in conjunction with the source surface.
			if( shaderDeform == DFRM_NONE || shaderDeform == DFRM_PARTICLE || shaderDeform == DFRM_PARTICLE2 )
			{
				// copy verts and indexes to this frame's hardware memory if they aren't already there
				if( !vertexCache.CacheIsCurrent( tri->ambientCache ) )
				{
					tri->ambientCache = vertexCache.AllocVertex( tri->verts, tri->numVerts );
				}
				if( !vertexCache.CacheIsCurrent( tri->indexCache ) )
				{
					tri->indexCache = vertexCache.AllocIndex( tri->indexes, tri->numIndexes );
				}

				R_SetupDrawSurfJoints( baseDrawSurf, tri, shader );

				baseDrawSurf->numIndexes = tri->numIndexes;
				baseDrawSurf->ambientCache = tri->ambientCache;
				baseDrawSurf->indexCache = tri->indexCache;

				baseDrawSurf->linkChain = NULL;		// link to the view
				baseDrawSurf->nextOnLight = vEntity->drawSurfs;
				vEntity->drawSurfs = baseDrawSurf;
			}

			// RB: use area the surface is in because a model can span multiple areas #965
			baseDrawSurf->area = NULL;

			if( shader->ReceivesLighting() )
			{
				idVec3 surfaceCenter;
				idVec3 triCenter = tri->bounds.GetCenter();

				idRenderMatrix modelRenderMatrix;
				idRenderMatrix::CreateFromOriginAxis( renderEntity->origin, renderEntity->axis, modelRenderMatrix );
				modelRenderMatrix.TransformPoint( triCenter, surfaceCenter );

				int surfaceArea = tr.primaryWorld->PointInArea( surfaceCenter );

				for( areaReference_t* ref = entityDef->entityRefs; ref != NULL; ref = ref->ownerNext )
				{
					idImage* lightGridImage = ref->area->lightGrid.GetIrradianceImage();

					if( surfaceArea == ref->area->areaNum && ref->area->lightGrid.lightGridPoints.Num() && lightGridImage != NULL && !lightGridImage->IsDefaulted() )
					{
						baseDrawSurf->area = ref->area;
						break;
					}
				}

				// RB: use first valid lightgrid
				// this would be wrong but less wrong than a flickering env_probe fallback
				if( baseDrawSurf->area == NULL )
				{
					for( areaReference_t* ref = entityDef->entityRefs; ref != NULL; ref = ref->ownerNext )
					{
						idImage* lightGridImage = ref->area->lightGrid.GetIrradianceImage();

						if( ref->area->lightGrid.lightGridPoints.Num() && lightGridImage != NULL && !lightGridImage->IsDefaulted() )
						{
							baseDrawSurf->area = ref->area;
							break;
						}
					}
				}

#if 0
				// show which area the surface is coming from
				//if( baseDrawSurf->area == NULL )
				{
					idBounds surfaceBounds;
					if( gpuSkinned )
					{
						surfaceBounds = vEntity->entityDef->localReferenceBounds;
					}
					else
					{
						surfaceBounds = tri->bounds;
					}

					idRenderMatrix modelRenderMatrix;
					idRenderMatrix::CreateFromOriginAxis( renderEntity->origin, renderEntity->axis, modelRenderMatrix );

					idRenderMatrix inverseBaseModelProject;
					idRenderMatrix::OffsetScaleForBounds( modelRenderMatrix, surfaceBounds, inverseBaseModelProject );

					// NOTE: unit cube instead of zeroToOne cube
					idVec4* verts = tr.maskedUnitCubeVerts;
					idVec4 triVerts[8];

					for( int i = 0; i < 8; i++ )
					{
						// transform to clip space
						inverseBaseModelProject.TransformPoint( verts[i], triVerts[i] );
					}

					static idVec4 colors[] = { colorBrown, colorBlue, colorCyan, colorGreen, colorYellow, colorRed, colorWhite };
					idVec4 color = colors[surfaceArea & 7];

					if( baseDrawSurf->area == NULL )
					{
						color = colorPurple;
					}

					// same as idRenderWorldLocal::DebugBox
					const int lifetime = 0;
					for( int i = 0; i < 4; i++ )
					{
						tr.viewDef->renderWorld->DebugLine( color, triVerts[i].ToVec3(), triVerts[( i + 1 ) & 3].ToVec3(), lifetime );
						tr.viewDef->renderWorld->DebugLine( color, triVerts[4 + i].ToVec3(), triVerts[4 + ( ( i + 1 ) & 3 )].ToVec3(), lifetime );
						tr.viewDef->renderWorld->DebugLine( color, triVerts[i].ToVec3(), triVerts[4 + i].ToVec3(), lifetime );
					}

					tr.viewDef->renderWorld->DebugAxis( surfaceCenter, renderEntity->axis );
				}
#endif
			}
		}

		//----------------------------------------
		// add all light interactions
		//----------------------------------------
		for( int contactedLight = 0; contactedLight < numContactedLights; contactedLight++ )
		{
			viewLight_t* vLight = contactedLights[contactedLight];
			const idRenderLightLocal* lightDef = vLight->lightDef;
			const idInteraction* interaction = staticInteractions[contactedLight];

			// check for a static interaction
			surfaceInteraction_t* surfInter = NULL;
			if( interaction > INTERACTION_EMPTY && interaction->staticInteraction )
			{
				// we have a static interaction that was calculated accurately
				assert( model->NumSurfaces() == interaction->numSurfaces );
				surfInter = &interaction->surfaces[surfaceNum];
			}
			else
			{
				// try to do a more precise cull of this model surface to the light
				if( R_CullModelBoundsToLight( lightDef, tri->bounds, entityDef->modelRenderMatrix ) )
				{
					continue;
				}
			}

			// "invisible ink" lights and shaders (imp spawn drawing on walls, etc)
			if( shader->Spectrum() != lightDef->lightShader->Spectrum() )
			{
				continue;
			}

			// Calculate the local light origin to determine if the view is inside the shadow
			// projection and to calculate the triangle facing for dynamic shadow volumes.
			idVec3 localLightOrigin;
			R_GlobalPointToLocal( vEntity->modelMatrix, lightDef->globalLightOrigin, localLightOrigin );


			//--------------------------
			// surface light interactions
			//--------------------------

			if( addInteractions && surfaceDirectlyVisible && shader->ReceivesLighting() )
			{
				// static interactions can commonly find that no triangles from a surface
				// contact the light, even when the total model does
				if( surfInter == NULL || surfInter->lightTrisIndexCache > 0 )
				{
					// make sure we have a valid shader register even if we didn't generate a drawn mesh above
					if( shaderRegisters == NULL )
					{
						drawSurf_t scratchSurf;
						R_SetupDrawSurfShader( &scratchSurf, shader, renderEntity );
						shaderRegisters = scratchSurf.shaderRegisters;
					}

					if( shaderRegisters != NULL )
					{
						// create a drawSurf for this interaction
						drawSurf_t* lightDrawSurf = ( drawSurf_t* )R_FrameAlloc( sizeof( *lightDrawSurf ), FRAME_ALLOC_DRAW_SURFACE );

						if( surfInter != NULL )
						{
							// optimized static interaction
							lightDrawSurf->numIndexes = surfInter->numLightTrisIndexes;
							lightDrawSurf->indexCache = surfInter->lightTrisIndexCache;
						}
						else
						{
							// throw the entire source surface at it without any per-triangle culling
							lightDrawSurf->numIndexes = tri->numIndexes;
							lightDrawSurf->indexCache = tri->indexCache;
						}

						lightDrawSurf->ambientCache = tri->ambientCache;
						lightDrawSurf->frontEndGeo = tri;
						lightDrawSurf->space = vEntity;
						lightDrawSurf->material = shader;
						lightDrawSurf->extraGLState = 0;
						lightDrawSurf->scissorRect = vLight->scissorRect; // interactionScissor;
						lightDrawSurf->sort = 0.0f;
						lightDrawSurf->shaderRegisters = shaderRegisters;

						R_SetupDrawSurfJoints( lightDrawSurf, tri, shader );

						// Determine which linked list to add the light surface to.
						// There will only be localSurfaces if the light casts shadows and
						// there are surfaces with NOSELFSHADOW.
						if( shader->Coverage() == MC_TRANSLUCENT )
						{
							lightDrawSurf->linkChain = &vLight->translucentInteractions;
						}
						else if( !lightDef->parms.noShadows && shader->TestMaterialFlag( MF_NOSELFSHADOW ) )
						{
							lightDrawSurf->linkChain = &vLight->localInteractions;
						}
						else
						{
							lightDrawSurf->linkChain = &vLight->globalInteractions;
						}
						lightDrawSurf->nextOnLight = vEntity->drawSurfs;
						vEntity->drawSurfs = lightDrawSurf;
					}
				}
			}

			//--------------------------
			// surface shadows
			//--------------------------

#if 1
			if( !shader->SurfaceCastsShadow() && !( r_forceShadowMapsOnAlphaTestedSurfaces.GetBool() && shader->Coverage() == MC_PERFORATED ) )
			{
				continue;
			}
#else
			// Steel Storm 2 behaviour - this destroys many alpha tested shadows in vanilla BFG

			// motorsep 11-08-2014; if r_forceShadowMapsOnAlphaTestedSurfaces is 0 when shadow mapping is on,
			// don't render shadows from all alphaTest surfaces.
			// Useful as global performance booster for old GPUs to disable shadows from grass/foliage/etc.
			if( r_useShadowMapping.GetBool() )
			{
				if( shader->Coverage() == MC_PERFORATED )
				{
					if( !r_forceShadowMapsOnAlphaTestedSurfaces.GetBool() )
					{
						continue;
					}
				}
			}

			// if material has "noShadows" global key
			if( !shader->SurfaceCastsShadow() )
			{
				// motorsep 11-08-2014; if r_forceShadowMapsOnAlphaTestedSurfaces is 1 when shadow mapping is on,
				// check if a surface IS NOT alphaTested and has "noShadows" global key;
				// or if a surface IS alphaTested and has "noShadows" global key;
				// if either is true, don't make surfaces cast shadow maps.
				if( r_useShadowMapping.GetBool() )
				{
					if( shader->Coverage() != MC_PERFORATED && shader->TestMaterialFlag( MF_NOSHADOWS ) )
					{
						continue;
					}
					else if( shader->Coverage() == MC_PERFORATED && shader->TestMaterialFlag( MF_NOSHADOWS ) )
					{
						continue;
					}
				}
				else
				{
					continue;
				}
			}
#endif

			if( !lightDef->LightCastsShadows() )
			{
				continue;
			}

			// some entities, like view weapons, don't cast any shadows
			if( entityDef->parms.noShadow )
			{
				continue;
			}

			// No shadow if it's suppressed for this light.
			if( entityDef->parms.suppressShadowInLightID && entityDef->parms.suppressShadowInLightID == lightDef->parms.lightId )
			{
				continue;
			}

			// stencil shadow volumes: build a shadow-volume drawSurf from the surface's static
			// shadow volume (indexes in surfInter->shadowIndexCache reference the surface's doubled
			// static shadowCache, both built at load) and link it into the light's global/local
			// shadow chain, then skip the shadow-map occluder path. M2: static casters only (their
			// shadowCache is a static buffer); always z-fail with caps (z-pass + cap selection is a
			// later optimisation). Dynamic / GPU-skinned casters fall through to shadow maps for now.
			// Soft shadow volumes reuse the very same shadow-VOLUME geometry (the umbra is stamped from
			// it into the light visibility buffer), so build it for either method.
			if( ( r_useStencilShadows.GetBool() || r_useSoftShadowVolumes.GetBool() ) && !r_useRTShadows.GetBool() )	// RT takes precedence when both are on
			{
				// stencil mode: build a shadow-VOLUME drawSurf if this static caster has one. Crucially
				// we NEVER build a shadow-MAP occluder in this mode - those carry ambientCache, not
				// shadowCache, and the stencil pass (which walks vLight->globalShadows/localShadows)
				// would try to draw them as volumes and fail. So continue regardless. M2: static
				// casters only; always z-fail with caps (z-pass + cap selection is a later opt).
				// M2 diagnostics: tally why casters are accepted/rejected for a stencil volume.
				extern int fe_stencilBuilt, fe_rejSilEdges, fe_rejSurfInter, fe_rejNumIdx, fe_rejIdxStale, fe_rejShadowCache;
				if( tri->silEdges == NULL )			fe_rejSilEdges++;
				else if( surfInter == NULL )			fe_rejSurfInter++;
				else if( surfInter->numShadowIndexes <= 0 )	fe_rejNumIdx++;
				else if( !vertexCache.CacheIsCurrent( surfInter->shadowIndexCache ) )	fe_rejIdxStale++;
				else if( !vertexCache.CacheIsStatic( tri->shadowCache ) )	fe_rejShadowCache++;
				else						fe_stencilBuilt++;

				// diagnostics: with r_singleLight + r_listViewLights, name every accepted stencil
				// caster so an enveloping/broken volume can be traced to its source surface.
				extern idCVar r_listViewLights;
				if( r_listViewLights.GetBool() && r_singleLight.GetInteger() == vLight->lightDef->index &&
						tri->silEdges != NULL && surfInter != NULL && surfInter->numShadowIndexes > 0 )
				{
					common->Printf( "stencilCaster light %i: ent %i model '%s' shader '%s' tris %i shadowIdx %i bounds (%.0f %.0f %.0f)-(%.0f %.0f %.0f)\n",
									vLight->lightDef->index, entityDef->index,
									entityDef->parms.hModel ? entityDef->parms.hModel->Name() : "?",
									shader->GetName(), tri->numIndexes / 3, surfInter->numShadowIndexes,
									tri->bounds[0].x, tri->bounds[0].y, tri->bounds[0].z,
									tri->bounds[1].x, tri->bounds[1].y, tri->bounds[1].z );
				}

				if( tri->silEdges != NULL && surfInter != NULL && surfInter->numShadowIndexes > 0 &&
						vertexCache.CacheIsCurrent( surfInter->shadowIndexCache ) &&
						vertexCache.CacheIsStatic( tri->shadowCache ) )
				{
					drawSurf_t* shadowDrawSurf = ( drawSurf_t* )R_FrameAlloc( sizeof( *shadowDrawSurf ), FRAME_ALLOC_DRAW_SURFACE );

					// Z-pass vs Z-fail: force Z-fail (full front+rear caps) only when the view might be
						// inside this caster's infinite shadow volume; otherwise Z-pass with NO caps. Z-pass
						// omits the projected-to-infinity far cap - forcing that cap for every caster is what
						// paints phantom shadows on distant geometry down long corridors (dark at range, lit
						// up close). localView/localLightOrigin are in this entity's model space.
						const bool viewMaybeInside = R_ViewPotentiallyInsideInfiniteShadowVolume(
								tri->bounds, localLightOrigin, localViewOrigin,
								r_znear.GetFloat() * INSIDE_SHADOW_VOLUME_EXTRA_STRETCH );
						shadowDrawSurf->numIndexes = viewMaybeInside ? surfInter->numShadowIndexes : surfInter->numShadowIndexesNoCaps;
					shadowDrawSurf->indexCache = surfInter->shadowIndexCache;
					shadowDrawSurf->shadowCache = tri->shadowCache;
					shadowDrawSurf->ambientCache = 0;
					shadowDrawSurf->jointCache = 0;
					shadowDrawSurf->frontEndGeo = NULL;
					shadowDrawSurf->space = vEntity;
					shadowDrawSurf->material = NULL;
					shadowDrawSurf->extraGLState = 0;
					shadowDrawSurf->scissorRect = vLight->scissorRect;
					shadowDrawSurf->sort = 0.0f;
					shadowDrawSurf->renderZFail = viewMaybeInside ? 1 : 0;	// Z-fail (with caps) only when possibly inside
					shadowDrawSurf->shaderRegisters = NULL;

					shadowDrawSurf->linkChain = shader->TestMaterialFlag( MF_NOSELFSHADOW ) ? &vLight->localShadows : &vLight->globalShadows;
					shadowDrawSurf->nextOnLight = vEntity->drawSurfs;
					vEntity->drawSurfs = shadowDrawSurf;
				}
				else if( surfInter == NULL && tri->silEdges != NULL )
				{
					// Moved / dynamic / animated caster: no baked static interaction exists (surfInter
					// == NULL), so this surface's silhouette against the light changes every frame.
					// Rebuild the shadow-volume index set on the fly from the model's silEdges + doubled
					// shadowCache and upload it to the frame index cache. This is what makes moving props
					// AND animated characters cast a stencil shadow that tracks them, instead of silently
					// dropping out (the old "static casters only" punt that left moved objects shadowless).
					// ponytail: per-frame R_CreateInteractionShadowVolume does a malloc+free; fine for
					// the moving casters in a scene, revisit if one throws hundreds.

					// Ensure a shadow vertex cache for this pose. Rigid movers carry a static shadowCache
					// (always current). Animated (MD5) casters get PERSISTENT posed verts in UpdateSurface;
					// rebuild the per-frame shadow cache from them whenever it's stale - crucially this
					// covers a settled ragdoll, whose cached dynamic model stops calling UpdateSurface, so
					// its frame shadow cache from an earlier frame has expired (that was the corpse whose
					// shadow vanished on settling). facing/cull use the posed override transparently.
					if( !vertexCache.CacheIsCurrent( tri->shadowCache ) )
					{
						const idDrawVert* posed = tri->posedShadowVerts != NULL ? tri->posedShadowVerts : tri->verts;
						if( posed != NULL && tri->numVerts > 0 )
						{
							const int numShadowVerts = tri->numVerts * 2;
							idShadowVert* shadowVerts = ( idShadowVert* )R_FrameAlloc( numShadowVerts * ( int )sizeof( idShadowVert ), FRAME_ALLOC_UNKNOWN );
							idShadowVert::CreateShadowCache( shadowVerts, posed, tri->numVerts );
							tri->shadowCache = vertexCache.AllocVertex( shadowVerts, numShadowVerts, sizeof( idShadowVert ) );
						}
					}

					srfTriangles_t* shadowTri = vertexCache.CacheIsCurrent( tri->shadowCache ) ? R_CreateInteractionShadowVolume( entityDef, tri, lightDef ) : NULL;
					if( shadowTri != NULL )
					{
						vertCacheHandle_t shadowIndexCache = vertexCache.AllocIndex( shadowTri->indexes, shadowTri->numIndexes );

						drawSurf_t* shadowDrawSurf = ( drawSurf_t* )R_FrameAlloc( sizeof( *shadowDrawSurf ), FRAME_ALLOC_DRAW_SURFACE );

						const bool viewMaybeInside = R_ViewPotentiallyInsideInfiniteShadowVolume(
								tri->bounds, localLightOrigin, localViewOrigin,
								r_znear.GetFloat() * INSIDE_SHADOW_VOLUME_EXTRA_STRETCH );
						// opaque casters can drop the caps when the view is safely outside the volume;
						// perforated ones already fold caps in (numShadowIndexesNoCaps == numIndexes there).
						shadowDrawSurf->numIndexes = viewMaybeInside ? shadowTri->numIndexes
								: ( shader->Coverage() == MC_OPAQUE ? shadowTri->numShadowIndexesNoCaps : shadowTri->numIndexes );
						shadowDrawSurf->indexCache = shadowIndexCache;
						shadowDrawSurf->shadowCache = tri->shadowCache;
						shadowDrawSurf->ambientCache = 0;
						shadowDrawSurf->jointCache = 0;
						shadowDrawSurf->frontEndGeo = NULL;
						shadowDrawSurf->space = vEntity;
						shadowDrawSurf->material = NULL;
						shadowDrawSurf->extraGLState = 0;
						shadowDrawSurf->scissorRect = vLight->scissorRect;
						shadowDrawSurf->sort = 0.0f;
						shadowDrawSurf->renderZFail = viewMaybeInside ? 1 : 0;
						shadowDrawSurf->shaderRegisters = NULL;

						shadowDrawSurf->linkChain = shader->TestMaterialFlag( MF_NOSELFSHADOW ) ? &vLight->localShadows : &vLight->globalShadows;
						shadowDrawSurf->nextOnLight = vEntity->drawSurfs;
						vEntity->drawSurfs = shadowDrawSurf;

						R_FreeStaticTriSurf( shadowTri );
					}
				}

				// Analytic soft shadows: collect this caster's silhouette EDGES in world space and link a
				// carrier surf into the light's chain. The interaction pixel shader evaluates coverage per
				// fragment against the exact receiver position (no screen-space reconstruction); the umbra
				// emerges from summing the edges. Same silhouette/winding as the stencil path.
				//
				// silEdges are a WEDGE-path requirement (the light-silhouette walk). FACE coverage streams the
				// raw triangles and needs no adjacency - and silEdges creation FAILS on non-2-manifold meshes
				// (rails, grates, decor), which silently dropped those casters from the soft stream and left
				// their shadows missing entirely (softgate: the erebus1_05 far-floor EXTENT defects).
				if( r_useSoftShadowVolumes.GetBool() && ( tri->silEdges != NULL || r_softShadowFaceCoverage.GetBool() ) )
				{
					const int swCollectStart = Sys_Microseconds();
					softShadowEdge_t* sedges = NULL;
					int nedges = 0;
					// FRONT-FACE coverage streams the caster's triangles (accurate + stable); the default streams
					// the light silhouette (undershoots off-axis). Face mode uses the v2 float4-triple layout,
					// stored through the same drawSurf fields (count in FLOAT4 elements; see drawSurf_t).
					if( r_softShadowFaceCoverage.GetBool() )
					{
						idVec4* faceElems = NULL;
						R_CollectPenumbraFaces( entityDef, tri, lightDef, r_shadowPenumbraSize.GetFloat(),
												vEntity->modelMatrix, &faceElems, &nedges );
						sedges = ( softShadowEdge_t* )faceElems;
					}
					else
					{
						R_CollectPenumbraEdges( entityDef, tri, lightDef, r_shadowPenumbraSize.GetFloat(),
												vEntity->modelMatrix, &sedges, &nedges );
					}
					tr.pc.softShadowMicroSec += Sys_Microseconds() - swCollectStart;
					extern int fe_softEdgesCollected;
					fe_softEdgesCollected += nedges;
					if( nedges > 0 )
					{
						drawSurf_t* edgeSurf = ( drawSurf_t* )R_FrameAlloc( sizeof( *edgeSurf ), FRAME_ALLOC_DRAW_SURFACE );
						memset( edgeSurf, 0, sizeof( *edgeSurf ) );
						edgeSurf->softEdges = sedges;
						edgeSurf->numSoftEdges = nedges;
						edgeSurf->frontEndGeo = tri;		// caster solid, for the scene-capture ground-truth mesh
						edgeSurf->space = vEntity;
						edgeSurf->scissorRect = vLight->scissorRect;

						edgeSurf->linkChain = &vLight->softShadowWedges;
						edgeSurf->nextOnLight = vEntity->drawSurfs;
						vEntity->drawSurfs = edgeSurf;
					}
				}

				// PCSS LOCATOR ATLAS: the soft-wedge PCSS locator (r_shadowMapPCSS) samples the shadow ATLAS to
				// classify lit / penumbra / umbra, but the volume surf built above is material==NULL, which
				// ShadowMapPassFast skips - leaving the soft light's atlas EMPTY, so the locator reads far
				// everywhere and (with receiver depth beyond the far plane) false-shadows the whole frame. Also
				// build a real OCCLUDER surf (ambientCache + material, shadowCache 0) into globalShadows so the
				// atlas gets true occluder depth. The two passes over globalShadows separate cleanly by cache:
				// StencilShadowPass skips shadowCache==0 (this occluder), ShadowMapPassFast skips material==NULL
				// (the volume). Only needed when the atlas-locator is actually in use.
				extern idCVar r_useShadowAtlas;
				extern idCVar r_shadowMapPCSS;
				if( r_useShadowAtlas.GetBool() && r_shadowMapPCSS.GetBool() &&
						( surfInter == NULL || surfInter->lightTrisIndexCache > 0 ) )
				{
					drawSurf_t* occ = ( drawSurf_t* )R_FrameAlloc( sizeof( *occ ), FRAME_ALLOC_DRAW_SURFACE );
					memset( occ, 0, sizeof( *occ ) );
					if( surfInter != NULL )
					{
						occ->numIndexes = surfInter->numLightTrisIndexes;
						occ->indexCache = surfInter->lightTrisIndexCache;
					}
					else
					{
						if( !vertexCache.CacheIsCurrent( tri->indexCache ) )
						{
							tri->indexCache = vertexCache.AllocIndex( tri->indexes, tri->numIndexes );
						}
						occ->numIndexes = tri->numIndexes;
						occ->indexCache = tri->indexCache;
					}
					if( !vertexCache.CacheIsCurrent( tri->ambientCache ) )
					{
						if( shader->ReceivesLighting() && !tri->tangentsCalculated )
						{
							R_DeriveTangents( tri );
						}
						tri->ambientCache = vertexCache.AllocVertex( tri->verts, tri->numVerts );
					}
					if( occ->numIndexes > 0 && vertexCache.CacheIsCurrent( occ->indexCache ) && vertexCache.CacheIsCurrent( tri->ambientCache ) )
					{
						occ->ambientCache = tri->ambientCache;
						occ->shadowCache = 0;			// NOT a volume: StencilShadowPass skips shadowCache==0; the atlas draws it
						occ->frontEndGeo = tri;
						occ->space = vEntity;
						occ->material = shader;
						occ->scissorRect = vLight->scissorRect;
						occ->sort = 0.0f;
						if( shader->Coverage() == MC_PERFORATED )
						{
							R_SetupDrawSurfShader( occ, shader, renderEntity );
						}
						R_SetupDrawSurfJoints( occ, tri, shader );
						occ->linkChain = &vLight->globalShadows;
						occ->nextOnLight = vEntity->drawSurfs;
						vEntity->drawSurfs = occ;
						extern int fe_occludersBuilt;
						fe_occludersBuilt++;
					}
				}

				continue;	// stencil mode never uses the shadow-map occluder path
			}

			// RT shadows: put into the world TLAS EXACTLY the casters stencil shadows this light
			// from - gated on surfInter->numShadowIndexes > 0 (a precomputed shadow interaction
			// exists for THIS surface+light pair), the same criterion the stencil branch above uses.
			// This is the unification: same occluder set, so RT and stencil agree. Do NOT add every
			// shadow-casting surface unconditionally - that throws the whole level at each light
			// (surfaces it neither lights nor casts for become false occluders), which drowned the
			// flashlight in spurious shadow. Use the caster's FULL static geometry (frontEndGeo +
			// full ambient/index cache, NOT the light-frustum-culled lightTrisIndexCache subset,
			// whose missing triangles let shadow rays through -> cut-off shadows). AppendLightShadow-
			// Casters builds one BLAS per unique ambientCache and dedupes against the flood gather.
			// Static caches only: skinned/dynamic casters are handled by BuildSkinnedInstances /
			// out of M-scope (matches stencil, which also needs surfInter != NULL). Then skip the
			// shadow-map occluder path (RT visibility masks replace it).
			if( r_useRTShadows.GetBool() )
			{
				if( surfInter != NULL && surfInter->numShadowIndexes > 0 && tri->numIndexes > 0 &&
						vertexCache.CacheIsStatic( tri->ambientCache ) && vertexCache.CacheIsStatic( tri->indexCache ) )
				{
					drawSurf_t* shadowDrawSurf = ( drawSurf_t* )R_FrameAlloc( sizeof( *shadowDrawSurf ), FRAME_ALLOC_DRAW_SURFACE );
					shadowDrawSurf->numIndexes = tri->numIndexes;
					shadowDrawSurf->indexCache = tri->indexCache;
					shadowDrawSurf->ambientCache = tri->ambientCache;
					shadowDrawSurf->shadowCache = 0;
					shadowDrawSurf->jointCache = 0;
					shadowDrawSurf->frontEndGeo = tri;
					shadowDrawSurf->space = vEntity;
					shadowDrawSurf->material = shader;
					shadowDrawSurf->extraGLState = 0;
					shadowDrawSurf->scissorRect = vLight->scissorRect;
					shadowDrawSurf->sort = 0.0f;
					shadowDrawSurf->renderZFail = 0;
					shadowDrawSurf->shaderRegisters = NULL;

					// The shadow-MAP atlas pass still runs for these lights (RT does not yet
					// suppress it) and walks globalShadows: an alpha-tested caster reaches
					// ShadowMapPassPerforated, which dereferences shaderRegisters. Set the shader
					// (perforated) and joints exactly as the shadow-map occluder path below does,
					// or that pass segfaults on our bespoke drawSurf.
					if( shader->Coverage() == MC_PERFORATED )
					{
						R_SetupDrawSurfShader( shadowDrawSurf, shader, renderEntity );
					}
					R_SetupDrawSurfJoints( shadowDrawSurf, tri, shader );

					shadowDrawSurf->linkChain = &vLight->globalShadows;
					shadowDrawSurf->nextOnLight = vEntity->drawSurfs;
					vEntity->drawSurfs = shadowDrawSurf;
				}
				continue;
			}


			// RB: draw shadow occluder using shadow mapping
			// OPTIMIZE: check if projected occluder box intersects the view
			//
			//if( addInteractions && surfaceDirectlyVisible && shader->ReceivesLighting() )
			{
				// static interactions can commonly find that no triangles from a surface
				// contact the light, even when the total model does
				if( surfInter == NULL || surfInter->lightTrisIndexCache > 0 )
				{
					// create a drawSurf for this interaction
					drawSurf_t* shadowDrawSurf = ( drawSurf_t* )R_FrameAlloc( sizeof( *shadowDrawSurf ), FRAME_ALLOC_DRAW_SURFACE );

					if( surfInter != NULL )
					{
						// optimized static interaction
						shadowDrawSurf->numIndexes = surfInter->numLightTrisIndexes;
						shadowDrawSurf->indexCache = surfInter->lightTrisIndexCache;
					}
					else
					{
						// make sure we have an ambient cache and all necessary normals / tangents
						if( !vertexCache.CacheIsCurrent( tri->indexCache ) )
						{
							tri->indexCache = vertexCache.AllocIndex( tri->indexes, tri->numIndexes );
						}

						// throw the entire source surface at it without any per-triangle culling
						shadowDrawSurf->numIndexes = tri->numIndexes;
						shadowDrawSurf->indexCache = tri->indexCache;
					}

					if( !vertexCache.CacheIsCurrent( tri->ambientCache ) )
					{
						// we are going to use it for drawing, so make sure we have the tangents and normals
						if( shader->ReceivesLighting() && !tri->tangentsCalculated )
						{
							assert( tri->staticModelWithJoints == NULL );
							R_DeriveTangents( tri );

							// RB: this was hit by parametric particle models ..
							//assert( false );	// this should no longer be hit
							// RB end
						}
						tri->ambientCache = vertexCache.AllocVertex( tri->verts, tri->numVerts );
					}

					shadowDrawSurf->ambientCache = tri->ambientCache;
					shadowDrawSurf->frontEndGeo = tri;
					shadowDrawSurf->space = vEntity;
					shadowDrawSurf->material = shader;
					shadowDrawSurf->extraGLState = 0;
					shadowDrawSurf->scissorRect = vLight->scissorRect; // interactionScissor;
					shadowDrawSurf->sort = 0.0f;
					//shadowDrawSurf->shaderRegisters = baseDrawSurf->shaderRegisters; // TODO FIXME

					if( shader->Coverage() == MC_PERFORATED )
					{
						R_SetupDrawSurfShader( shadowDrawSurf, shader, renderEntity );
					}

					R_SetupDrawSurfJoints( shadowDrawSurf, tri, shader );

					// determine which linked list to add the shadow surface to

					//shadowDrawSurf->linkChain = shader->TestMaterialFlag( MF_NOSELFSHADOW ) ? &vLight->localShadows : &vLight->globalShadows;

					shadowDrawSurf->linkChain = &vLight->globalShadows;
					shadowDrawSurf->nextOnLight = vEntity->drawSurfs;

					vEntity->drawSurfs = shadowDrawSurf;

				}
			}
			// RB end
		}
	}
}

REGISTER_PARALLEL_JOB( R_AddSingleModel, "R_AddSingleModel" );

/*
=================
R_LinkDrawSurfToView

Als called directly by GuiModel
=================
*/
void R_LinkDrawSurfToView( drawSurf_t* drawSurf, viewDef_t* viewDef )
{
	// if it doesn't fit, resize the list
	if( viewDef->numDrawSurfs == viewDef->maxDrawSurfs )
	{
		drawSurf_t** old = viewDef->drawSurfs;
		int count;

		if( viewDef->maxDrawSurfs == 0 )
		{
			viewDef->maxDrawSurfs = INITIAL_DRAWSURFS;
			count = 0;
		}
		else
		{
			count = viewDef->maxDrawSurfs * sizeof( viewDef->drawSurfs[0] );
			viewDef->maxDrawSurfs *= 2;
		}
		viewDef->drawSurfs = ( drawSurf_t** )R_FrameAlloc( viewDef->maxDrawSurfs * sizeof( viewDef->drawSurfs[0] ), FRAME_ALLOC_DRAW_SURFACE_POINTER );
		memcpy( viewDef->drawSurfs, old, count );
	}

	viewDef->drawSurfs[viewDef->numDrawSurfs] = drawSurf;
	viewDef->numDrawSurfs++;
}

/*
===================
R_AddModels

The end result of running this is the addition of drawSurf_t to the
tr.viewDef->drawSurfs[] array and light link chains, along with
frameData and vertexCache allocations to support the drawSurfs.
===================
*/
void R_AddModels()
{
	SCOPED_PROFILE_EVENT( "R_AddModels" );

	// RB: already done in R_FillMaskedOcclusionBufferWithModels
	// tr.viewDef->viewEntitys = R_SortViewEntities( tr.viewDef->viewEntitys );

	// Diagnostic (r_softShadowDebugHash): the fingerprint must accumulate across ALL views of a frame
	// (there are typically 2: world + weapon), so reset/compare only at a FRAME boundary, not per
	// R_AddModels call. Comparing per-call would just alternate between the two views' partial sums.
	extern idSysInterlockedInteger tr_softWedgeGeomHash;
	extern idSysInterlockedInteger tr_softWedgeEdgeCount;
	extern idCVar r_softShadowDebugHash;
	if( r_softShadowDebugHash.GetInteger() >= 1 )
	{
		static int s_lastFrame = -1;
		static int s_prevTotal = 0;
		static int s_prevCount = 0;
		if( tr.frameCount != s_lastFrame )
		{
			const int total = tr_softWedgeGeomHash.GetValue();	// completed frame (all its views)
			const int count = tr_softWedgeEdgeCount.GetValue();
			if( s_lastFrame >= 0 && total != s_prevTotal )
			{
				// count delta => the SET of contributing edges changed (caster/light/view set);
				// count same => the per-edge WORLD values changed (pose/matrix), which is camera-independent
				// by construction and so should never move under a still scene + moving camera.
				common->Printf( "softwedge fingerprint DRIFT frame %i: %08x -> %08x  edges %i -> %i  (%s)\n",
								tr.frameCount, ( unsigned )s_prevTotal, ( unsigned )total, s_prevCount, count,
								count != s_prevCount ? "SET changed" : "VALUES changed" );
			}
			s_prevTotal = total;
			s_prevCount = count;
			tr_softWedgeGeomHash.SetValue( 0 );	// reset for the new frame
			tr_softWedgeEdgeCount.SetValue( 0 );
			s_lastFrame = tr.frameCount;
		}
	}

	//-------------------------------------------------
	// Go through each view entity that is either visible to the view, or to
	// any light that intersects the view (for shadows).
	//-------------------------------------------------

	if( r_useParallelAddModels.GetBool() )
	{
		for( viewEntity_t* vEntity = tr.viewDef->viewEntitys; vEntity != NULL; vEntity = vEntity->next )
		{
			tr.frontEndJobList->AddJob( ( jobRun_t )R_AddSingleModel, vEntity );
		}
		tr.frontEndJobList->Submit();
		tr.frontEndJobList->Wait();
	}
	else
	{
		for( viewEntity_t* vEntity = tr.viewDef->viewEntitys; vEntity != NULL; vEntity = vEntity->next )
		{
			R_AddSingleModel( vEntity );
		}
	}

	//-------------------------------------------------
	// Move the draw surfs to the view.
	//-------------------------------------------------

	tr.viewDef->numDrawSurfs = 0;	// clear the ambient surface list
	tr.viewDef->maxDrawSurfs = 0;	// will be set to INITIAL_DRAWSURFS on R_LinkDrawSurfToView

	for( viewEntity_t* vEntity = tr.viewDef->viewEntitys; vEntity != NULL; vEntity = vEntity->next )
	{
		// RB
		if( vEntity->drawSurfs != NULL )
		{
			tr.pc.c_visibleViewEntities++;
		}

		for( drawSurf_t* ds = vEntity->drawSurfs; ds != NULL; )
		{
			drawSurf_t* next = ds->nextOnLight;
			if( ds->linkChain == NULL )
			{
				R_LinkDrawSurfToView( ds, tr.viewDef );
			}
			else
			{
				ds->nextOnLight = *ds->linkChain;
				*ds->linkChain = ds;
			}
			ds = next;
		}

		vEntity->drawSurfs = NULL;
	}

	// Analytic soft shadows: flatten each light's per-caster silhouette edges into one contiguous
	// vertex-cache buffer. The interaction pixel shader loops these against the exact receiver world
	// position, evaluating penumbra coverage without any screen-space depth reconstruction.
	if( r_useSoftShadowVolumes.GetBool() )
	{
		// Hard per-frame budget so we can NEVER overflow the joint buffer (that overflow is a fatal
		// idLib::Error). softShadowEdge_t = 32 bytes; keep well under the 64MB joint buffer, leaving room
		// for skinning. Lights past the budget simply get no soft shadow this frame (approximation).
		const int SOFT_EDGE_FRAME_BUDGET = 400000;	// ~12.8 MB of edges, well under the joint buffer
		int edgesUsed = 0;
		const int swFlattenStart = Sys_Microseconds();

		extern idCVar r_softShadowFaceCoverage;
		const bool swFaceMode = r_softShadowFaceCoverage.GetBool();

		for( viewLight_t* vLight = tr.viewDef->viewLights; vLight != NULL; vLight = vLight->next )
		{
			vLight->softEdgeCache = 0;
			vLight->softEdgeCount = 0;
			vLight->softCasterCache = 0;
			vLight->softCasterCount = 0;

			// Perf: prepend each caster's edge block with a HEADER record carrying the caster's
			// world-space bounding sphere, so the pixel shader can cheaply reject a whole caster that
			// cannot occlude a given fragment's light disk (a distant world surface then skips its
			// entire edge loop instead of an atan2 per edge). One header per caster; the shader marks
			// a header by e0.w < 0 (edges carry e0.w = silWeight >= 0). Records = edges + one header/caster.
			// One caster PER MODEL ENTITY, not per surface. A caster built from several surfaces (a tripod's
			// legs, a rock's faces) would otherwise become several casters, which the coverage shader unions by
			// max( occlusion ). At a joint no single surface occludes the whole light disk, so max() reports a
			// half-covered disk - a hairline lit crack running through solid umbra. Grouping every surface of
			// one entity under a single header makes the shader SUM their coverage integrals (each surface is a
			// separate closed chain; disjoint areas add, shared seams cancel), reconstructing the union. An
			// entity's edge surfaces are linked contiguously (all off one vEntity->drawSurfs), so a caster is a
			// run of consecutive surfaces with the same space.
			//
			// EXCEPTION: the static world model is a SINGLE space holding the whole level's surfaces, spread
			// far apart and not one coherent occluder. Grouping it would (a) give one caster a level-sized
			// bounding sphere so the shader-side cull never rejects it - every fragment loops every world edge
			// (huge perf hit) - and (b) SUM spatially-unrelated world surfaces, over-darkening large lit areas.
			// So each world surface stays its own caster (tight sphere, max-combine), exactly as before.
			int total = 0, numCasters = 0;
			const void* prevSpace = NULL;
			for( const drawSurf_t* s = vLight->softShadowWedges; s != NULL; s = s->nextOnLight )
			{
				total += s->numSoftEdges;
				const bool isWorld = s->space->entityDef != NULL && s->space->entityDef->parms.hModel != NULL
									 && s->space->entityDef->parms.hModel->IsStaticWorldModel();
				if( isWorld || s->space != prevSpace ) { numCasters++; prevSpace = s->space; }
			}
			if( total <= 0 )
			{
				continue;	// no edges for this light
			}

			if( swFaceMode )
			{
				// ---- FACE stream v2: pure triangle stream (3 float4/tri) + separate caster table ----
				// No inline headers: the per-fragment walk loops the caster table (sphere cull, then the
				// caster's contiguous tri span) so the hot triangle loop carries no header branch, the
				// bin pass can pre-cull whole casters, and each triangle is one contiguous 48B load.
				// 'total' is already in float4 elements (R_CollectPenumbraFaces v2). Budget is bytes-
				// equivalent to the wedge path's: records are 32B, float4s are 16B.
				const int casterElems = numCasters * 2;
				if( edgesUsed + ( total + casterElems + 1 ) / 2 > SOFT_EDGE_FRAME_BUDGET )
				{
					tr.pc.c_softShadowDroppedEdges += total;	// over budget -> this light gets no soft shadow
					continue;
				}
				edgesUsed += ( total + casterElems + 1 ) / 2;

				idVec4* triFlat = ( idVec4* )R_FrameAlloc( ( total + 1 ) * sizeof( idVec4 ), FRAME_ALLOC_UNKNOWN );	// +1: zero pad for the pair-based capture copy
				idVec4* casFlat = ( idVec4* )R_FrameAlloc( casterElems * sizeof( idVec4 ), FRAME_ALLOC_UNKNOWN );
				int nElems = 0;
				int nCas = 0;
				const void* curSpace = NULL;
				int  openFirstTri = 0;
				bool casterOpen = false;
				idVec3 gmn( 1e30f, 1e30f, 1e30f ), gmx( -1e30f, -1e30f, -1e30f );
				for( const drawSurf_t* s = vLight->softShadowWedges; s != NULL; s = s->nextOnLight )
				{
					const bool isWorld = s->space->entityDef != NULL && s->space->entityDef->parms.hModel != NULL
										 && s->space->entityDef->parms.hModel->IsStaticWorldModel();
					if( isWorld || s->space != curSpace )	// world surfaces never group (each is its own caster)
					{
						if( casterOpen )
						{
							const idVec3 c = ( gmn + gmx ) * 0.5f;
							const float  rad = ( gmx - gmn ).Length() * 0.5f;
							casFlat[nCas * 2 + 0] = idVec4( c.x, c.y, c.z, rad );
							casFlat[nCas * 2 + 1] = idVec4( ( float )openFirstTri, ( float )( nElems / 3 - openFirstTri ), 0.0f, 0.0f );
							nCas++;
						}
						openFirstTri = nElems / 3;
						casterOpen = true;
						gmn.Set( 1e30f, 1e30f, 1e30f );
						gmx.Set( -1e30f, -1e30f, -1e30f );
						curSpace = s->space;
					}
					const idVec4* src = ( const idVec4* )s->softEdges;
					for( int i = 0; i < s->numSoftEdges; i++ )
					{
						const idVec4& v = src[i];
						gmn.x = Min( gmn.x, v.x );	gmx.x = Max( gmx.x, v.x );	// every float4's xyz is a vertex; w (triRad/0) ignored
						gmn.y = Min( gmn.y, v.y );	gmx.y = Max( gmx.y, v.y );
						gmn.z = Min( gmn.z, v.z );	gmx.z = Max( gmx.z, v.z );
						triFlat[nElems++] = v;
					}
				}
				if( casterOpen )
				{
					const idVec3 c = ( gmn + gmx ) * 0.5f;
					const float  rad = ( gmx - gmn ).Length() * 0.5f;
					casFlat[nCas * 2 + 0] = idVec4( c.x, c.y, c.z, rad );
					casFlat[nCas * 2 + 1] = idVec4( ( float )openFirstTri, ( float )( nElems / 3 - openFirstTri ), 0.0f, 0.0f );
					nCas++;
				}

				vLight->softEdgeCache = vertexCache.AllocJoint( triFlat, total, sizeof( idVec4 ) );
				vLight->softEdgeCount = total;					// FACE mode: count in FLOAT4 elements
				vLight->softCasterCache = vertexCache.AllocJoint( casFlat, casterElems, sizeof( idVec4 ) );
				vLight->softCasterCount = nCas;

				if( R_SoftShadowCaptureArmed() )
				{
					triFlat[total].Zero();						// pad the odd tail for the pair-based copy
					R_CaptureLightEdges( vLight, ( const softShadowEdge_t* )triFlat, ( total + 1 ) / 2 );
				}

				tr.pc.c_softShadowLights++;
				tr.pc.c_softShadowCasters += nCas;
				tr.pc.c_softShadowEdges += total;
				tr.pc.c_softShadowMaxEdgesPerLight = Max( tr.pc.c_softShadowMaxEdgesPerLight, total );
				continue;
			}

			// ---- legacy WEDGE stream: inline caster headers + silhouette edge records ----
			const int records = total + numCasters;
			if( edgesUsed + records > SOFT_EDGE_FRAME_BUDGET )
			{
				tr.pc.c_softShadowDroppedEdges += total;	// over budget -> this light gets no soft shadow
				continue;
			}
			edgesUsed += records;

			softShadowEdge_t* flat = ( softShadowEdge_t* )R_FrameAlloc( records * sizeof( softShadowEdge_t ), FRAME_ALLOC_UNKNOWN );
			int n = 0;
			float casterId = 0.0f;	// tag each caster's edges so the shader can group + combine per caster
			const void* curSpace = NULL;
			int   headerIdx = -1;			// record index of the current entity's header (reserved, backfilled on close)
			idVec3 gmn( 1e30f, 1e30f, 1e30f ), gmx( -1e30f, -1e30f, -1e30f );	// entity bounding box (for the sphere)
			for( const drawSurf_t* s = vLight->softShadowWedges; s != NULL; s = s->nextOnLight )
			{
				const bool isWorld = s->space->entityDef != NULL && s->space->entityDef->parms.hModel != NULL
									 && s->space->entityDef->parms.hModel->IsStaticWorldModel();
				if( isWorld || s->space != curSpace )	// world surfaces never group (each is its own caster)
				{
					if( headerIdx >= 0 )	// close the previous entity: its sphere is now known
					{
						const idVec3 c = ( gmn + gmx ) * 0.5f;
						const float  rad = ( gmx - gmn ).Length() * 0.5f;
						flat[headerIdx].e0 = idVec4( c.x, c.y, c.z, -1.0f );		// e0 = ( centre, -1 marker )
						flat[headerIdx].e1 = idVec4( rad, ( float )( n - headerIdx - 1 ), 0.0f, casterId );	// e1 = ( radius, edgeCount, 0, casterId )
						casterId += 1.0f;
					}
					headerIdx = n++;		// reserve this entity's header slot
					gmn.Set( 1e30f, 1e30f, 1e30f );
					gmx.Set( -1e30f, -1e30f, -1e30f );
					curSpace = s->space;
				}

				for( int i = 0; i < s->numSoftEdges; i++ )
				{
					const idVec4& e0 = s->softEdges[i].e0;
					const idVec4& e1 = s->softEdges[i].e1;
					gmn.x = Min( gmn.x, Min( e0.x, e1.x ) );	gmx.x = Max( gmx.x, Max( e0.x, e1.x ) );
					gmn.y = Min( gmn.y, Min( e0.y, e1.y ) );	gmx.y = Max( gmx.y, Max( e0.y, e1.y ) );
					gmn.z = Min( gmn.z, Min( e0.z, e1.z ) );	gmx.z = Max( gmx.z, Max( e0.z, e1.z ) );

					flat[n] = s->softEdges[i];
					// e1.w carries this caster's HEADER record index so the band prepass VS (softband.vs.hlsl)
					// can fetch the caster's bounding-sphere centre for the near/far volume caps. The coverage
					// pixel shader detects caster boundaries by the e0.w<0 header, not by e1.w, so this is free.
					flat[n].e1.w = ( float )headerIdx;
					n++;
				}
			}
			if( headerIdx >= 0 )	// close the final entity
			{
				const idVec3 c = ( gmn + gmx ) * 0.5f;
				const float  rad = ( gmx - gmn ).Length() * 0.5f;
				flat[headerIdx].e0 = idVec4( c.x, c.y, c.z, -1.0f );
				flat[headerIdx].e1 = idVec4( rad, ( float )( n - headerIdx - 1 ), 0.0f, casterId );	// e1.y = edgeCount (shader jump-skip)
				casterId += 1.0f;
			}
			// AllocJoint (not AllocVertex): the joint buffer is the SRV-capable StructuredBuffer the
			// interaction pixel shader can read; the vertex buffer is not bound as an SRV.
			vLight->softEdgeCache = vertexCache.AllocJoint( flat, records, sizeof( softShadowEdge_t ) );
			vLight->softEdgeCount = records;

			if( R_SoftShadowCaptureArmed() )
			{
				R_CaptureLightEdges( vLight, flat, records );	// retain a CPU copy before flat[] is dropped
			}

			tr.pc.c_softShadowLights++;
			tr.pc.c_softShadowCasters += numCasters;
			tr.pc.c_softShadowEdges += total;
			tr.pc.c_softShadowMaxEdgesPerLight = Max( tr.pc.c_softShadowMaxEdgesPerLight, total );
		}

		tr.pc.softShadowMicroSec += Sys_Microseconds() - swFlattenStart;
	}
}
