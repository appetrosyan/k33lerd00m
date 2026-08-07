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

#include "precompiled.h"
#pragma hdrstop

#include "RenderCommon.h"
#include "RenderCapture.h"
#include "../framework/Common_local.h"

#include <sys/DeviceManager.h>

#include <vector>
#include <map>

extern DeviceManager* deviceManager;

// One-shot capture of a live soft-shadow view, reconstructable headless. See RenderCapture.h. The capture is
// two-phase within one frame: the FRONTEND half snapshots the view/lights/edges/caster-meshes (all CPU-side),
// the BACKEND half grabs the screenshot and writes the .softcap + .json. `captureSoftShadow` arms it; the
// halves fire on the next main view and disarm.

extern idCVar r_shadowPenumbraSize;

namespace
{
// ---- armed state ----
bool  s_armed = false;			// set by the command, cleared once both halves have run
bool  s_frontendDone = false;	// the frontend half snapshotted this frame

// ---- accumulators (frame-scoped; cleared when a new capture is armed) ----
softcapHeader_t         s_hdr;
std::vector<softcapLight_t>  s_lights;
std::vector<softcapEdge_t>   s_edges;
std::vector<softcapCaster_t> s_casters;
std::vector<float>           s_meshVerts;	// float3 packed (caster meshes)
std::vector<uint32_t>        s_meshIdx;
std::vector<float>           s_depth;		// per-pixel raw depth (R channel), screenW*screenH, top-left origin
std::vector<softcapReceiver_t> s_receivers;	// receiver interaction surfaces (the shaded surfaces)
std::vector<float>           s_recvVerts;	// float3 packed (receiver meshes)
std::vector<uint32_t>        s_recvIdx;

// append one drawSurf's world-space triangle mesh (frontEndGeo x modelMatrix) to verts/idx; return the range.
bool AppendSurfMesh( const drawSurf_t* cs, std::vector<float>& verts, std::vector<uint32_t>& idx,
					 uint32_t& firstVert, uint32_t& numVerts, uint32_t& firstIndex, uint32_t& numIndex )
{
	const srfTriangles_t* tri = cs->frontEndGeo;
	if( tri == NULL || tri->verts == NULL || tri->numVerts <= 0 || tri->indexes == NULL || tri->numIndexes <= 0 )
	{
		return false;
	}
	firstVert  = ( uint32_t )( verts.size() / 3 );
	numVerts   = ( uint32_t )tri->numVerts;
	firstIndex = ( uint32_t )idx.size();
	numIndex   = ( uint32_t )tri->numIndexes;
	for( int v = 0; v < tri->numVerts; v++ )
	{
		idVec3 w;
		R_LocalPointToGlobal( cs->space->modelMatrix, tri->verts[v].xyz, w );
		verts.push_back( w.x ); verts.push_back( w.y ); verts.push_back( w.z );
	}
	// store GLOBAL indices (offset by firstVert) so the flat vert array is one addressable soup - a ray-cast
	// over many surfaces indexes correctly, not just the first (whose firstVert is 0).
	for( int i = 0; i < tri->numIndexes; i++ ) { idx.push_back( firstVert + ( uint32_t )tri->indexes[i] ); }
	return true;
}

// edges retained per viewLight at flatten time, matched to the light in the frontend walk by pointer.
std::map<const viewLight_t*, std::pair<uint32_t, uint32_t>> s_edgeRange;	// vLight -> (firstEdge, count)

void ResetAccumulators()
{
	memset( &s_hdr, 0, sizeof( s_hdr ) );
	s_lights.clear();
	s_edges.clear();
	s_casters.clear();
	s_meshVerts.clear();
	s_meshIdx.clear();
	s_depth.clear();
	s_receivers.clear();
	s_recvVerts.clear();
	s_recvIdx.clear();
	s_edgeRange.clear();
	s_frontendDone = false;
}

// sequential base name "softcapNNNN" in the same spot screenshots go (fs_savepath).
void NextCaptureBaseName( idStr& out )
{
	for( int i = 0; i <= 9999; i++ )
	{
		idStr candidate = va( "softcap/softcap%04i.softcap", i );
		if( fileSystem->ReadFile( candidate, NULL, NULL ) == -1 )
		{
			out = va( "softcap/softcap%04i", i );
			return;
		}
	}
	out = "softcap/softcap9999";
}
} // namespace

bool R_SoftShadowCaptureArmed()
{
	return s_armed;
}

// ------------------------------------------------------------------- frontend half: edges (at flatten time)
void R_CaptureLightEdges( const viewLight_t* vLight, const softShadowEdge_t* flat, int records )
{
	if( !s_armed || s_frontendDone || vLight == NULL || flat == NULL || records <= 0 )
	{
		return;
	}
	const uint32_t first = ( uint32_t )s_edges.size();
	for( int i = 0; i < records; i++ )
	{
		softcapEdge_t e;
		e.e0[0] = flat[i].e0.x; e.e0[1] = flat[i].e0.y; e.e0[2] = flat[i].e0.z; e.e0[3] = flat[i].e0.w;
		e.e1[0] = flat[i].e1.x; e.e1[1] = flat[i].e1.y; e.e1[2] = flat[i].e1.z; e.e1[3] = flat[i].e1.w;
		s_edges.push_back( e );
	}
	s_edgeRange[vLight] = std::make_pair( first, ( uint32_t )records );
}

// ------------------------------------------------------------------- frontend half: view + lights + meshes
void R_CaptureFrontendView( const viewDef_t* viewDef )
{
	if( !s_armed || s_frontendDone || viewDef == NULL )
	{
		return;
	}

	// camera / view
	const renderView_t& rv = viewDef->renderView;
	s_hdr.magic = SOFTCAP_MAGIC;
	s_hdr.version = SOFTCAP_VERSION;
	s_hdr.screenW = ( uint32_t )( viewDef->viewport.x2 - viewDef->viewport.x1 + 1 );
	s_hdr.screenH = ( uint32_t )( viewDef->viewport.y2 - viewDef->viewport.y1 + 1 );
	for( int i = 0; i < 3; i++ ) { s_hdr.vieworg[i] = rv.vieworg[i]; }
	for( int r = 0; r < 3; r++ )
		for( int c = 0; c < 3; c++ ) { s_hdr.viewaxis[r * 3 + c] = rv.viewaxis[r][c]; }
	s_hdr.fovx = rv.fov_x;
	s_hdr.fovy = rv.fov_y;
	for( int i = 0; i < 16; i++ )
	{
		s_hdr.projectionMatrix[i]           = viewDef->projectionMatrix[i];
		s_hdr.unjitteredProjectionMatrix[i] = viewDef->unjitteredProjectionMatrix[i];
		s_hdr.unprojectionToWorldMatrix[i]  = viewDef->unprojectionToWorldMatrix[i];
		s_hdr.worldMVP[i]                   = viewDef->worldSpace.mvp[i / 4][i % 4];
	}
	s_hdr.viewport[0] = viewDef->viewport.x1; s_hdr.viewport[1] = viewDef->viewport.y1;
	s_hdr.viewport[2] = viewDef->viewport.x2; s_hdr.viewport[3] = viewDef->viewport.y2;
	s_hdr.taaFrameCount = tr.frameCount;

	// per soft-shadow light: assemble the final record (edge range came from the flatten half) + caster meshes
	for( const viewLight_t* vLight = viewDef->viewLights; vLight != NULL; vLight = vLight->next )
	{
		if( vLight->softEdgeCount <= 0 )
		{
			continue;
		}
		auto it = s_edgeRange.find( vLight );
		if( it == s_edgeRange.end() )
		{
			continue;	// armed after this light's flatten; skip rather than emit a light with no edges
		}
		softcapLight_t L;
		memset( &L, 0, sizeof( L ) );
		L.origin[0] = vLight->globalLightOrigin.x;
		L.origin[1] = vLight->globalLightOrigin.y;
		L.origin[2] = vLight->globalLightOrigin.z;
		L.penumbraSize = r_shadowPenumbraSize.GetFloat();
		L.scissor[0] = vLight->scissorRect.x1; L.scissor[1] = vLight->scissorRect.y1;
		L.scissor[2] = vLight->scissorRect.x2; L.scissor[3] = vLight->scissorRect.y2;
		L.firstEdge = it->second.first;
		L.edgeCount = it->second.second;
		L.firstCaster = ( uint32_t )s_casters.size();

		const uint32_t lightIndex = ( uint32_t )s_lights.size();

		// caster silhouette solids (for the ray-cast ground truth)
		for( const drawSurf_t* cs = vLight->softShadowWedges; cs != NULL; cs = cs->nextOnLight )
		{
			softcapCaster_t C;
			C.lightIndex = lightIndex;
			C.casterId   = 0.0f;	// (edge headers carry the real casterId; not needed for the ray-cast)
			if( !AppendSurfMesh( cs, s_meshVerts, s_meshIdx, C.firstVert, C.numVerts, C.firstIndex, C.numIndex ) )
			{
				continue;
			}
			s_casters.push_back( C );
		}
		L.casterCount = ( uint32_t )s_casters.size() - L.firstCaster;

		// receiver interaction surfaces (what the coverage shader shades). These give receiver world positions
		// with no depth reconstruction - coverage-vs-truth is evaluated at these surface points.
		for( int pass = 0; pass < 2; pass++ )
		{
			const drawSurf_t* list = ( pass == 0 ) ? vLight->globalInteractions : vLight->localInteractions;
			for( const drawSurf_t* rs = list; rs != NULL; rs = rs->nextOnLight )
			{
				softcapReceiver_t R;
				R.lightIndex = lightIndex;
				if( !AppendSurfMesh( rs, s_recvVerts, s_recvIdx, R.firstVert, R.numVerts, R.firstIndex, R.numIndex ) )
				{
					continue;
				}
				s_receivers.push_back( R );
			}
		}
		s_lights.push_back( L );
	}

	s_frontendDone = true;
}

// ------------------------------------------------------------------- backend half: screenshot + write files
void R_CaptureBackendFinish()
{
	if( !s_armed || !s_frontendDone )
	{
		return;
	}

	idStr base;
	NextCaptureBaseName( base );

	// screenshot (LDR) straight to PNG, reusing the engine path
	idStr png = base + ".png";
	R_ReadPixelsRGB8( deviceManager->GetDevice(), &backEnd.GetCommonPasses(),
					  globalImages->ldrImage->GetTextureHandle(), nvrhi::ResourceStates::RenderTarget, png.c_str() );

	// depth (full float) for per-pixel receiver world-position reconstruction on replay / ground truth
	const int pix = ( int )( s_hdr.screenW * s_hdr.screenH );
	float* depthPic = NULL;
	if( pix > 0 && R_ReadPixelsR32F( deviceManager->GetDevice(), &backEnd.GetCommonPasses(),
									 globalImages->currentDepthImage->GetTextureHandle(),
									 nvrhi::ResourceStates::ShaderResource, &depthPic, ( int )s_hdr.screenW, ( int )s_hdr.screenH ) && depthPic != NULL )
	{
		s_depth.assign( depthPic, depthPic + pix );
		R_StaticFree( depthPic );
	}

	// finalize header counts
	s_hdr.numLights     = ( uint32_t )s_lights.size();
	s_hdr.numEdges      = ( uint32_t )s_edges.size();
	s_hdr.numCasters    = ( uint32_t )s_casters.size();
	s_hdr.numMeshVerts  = ( uint32_t )( s_meshVerts.size() / 3 );
	s_hdr.numMeshIdx    = ( uint32_t )s_meshIdx.size();
	s_hdr.hasDepth      = ( uint32_t )( ( int )s_depth.size() == pix && pix > 0 ? 1 : 0 );
	s_hdr.numReceivers  = ( uint32_t )s_receivers.size();
	s_hdr.numRecvVerts  = ( uint32_t )( s_recvVerts.size() / 3 );
	s_hdr.numRecvIdx    = ( uint32_t )s_recvIdx.size();

	// write the binary blob
	idFile* f = fileSystem->OpenFileWrite( base + ".softcap", "fs_savepath" );
	if( f != NULL )
	{
		f->Write( &s_hdr, sizeof( s_hdr ) );
		if( !s_lights.empty() )    { f->Write( s_lights.data(),    ( int )( s_lights.size()    * sizeof( softcapLight_t ) ) ); }
		if( !s_edges.empty() )     { f->Write( s_edges.data(),     ( int )( s_edges.size()     * sizeof( softcapEdge_t ) ) ); }
		if( !s_casters.empty() )   { f->Write( s_casters.data(),   ( int )( s_casters.size()   * sizeof( softcapCaster_t ) ) ); }
		if( !s_meshVerts.empty() ) { f->Write( s_meshVerts.data(), ( int )( s_meshVerts.size() * sizeof( float ) ) ); }
		if( !s_meshIdx.empty() )   { f->Write( s_meshIdx.data(),   ( int )( s_meshIdx.size()   * sizeof( uint32_t ) ) ); }
		if( s_hdr.hasDepth )        { f->Write( s_depth.data(),     ( int )( s_depth.size()      * sizeof( float ) ) ); }
		if( !s_receivers.empty() )  { f->Write( s_receivers.data(), ( int )( s_receivers.size()  * sizeof( softcapReceiver_t ) ) ); }
		if( !s_recvVerts.empty() )  { f->Write( s_recvVerts.data(), ( int )( s_recvVerts.size()  * sizeof( float ) ) ); }
		if( !s_recvIdx.empty() )    { f->Write( s_recvIdx.data(),   ( int )( s_recvIdx.size()    * sizeof( uint32_t ) ) ); }
		fileSystem->CloseFile( f );
		common->Printf( "soft-shadow capture: %s.softcap  (%u lights, %u edges, %u casters/%u tris, %u recv/%u tris, depth=%u)\n",
						base.c_str(), s_hdr.numLights, s_hdr.numEdges, s_hdr.numCasters, s_hdr.numMeshIdx / 3,
						s_hdr.numReceivers, s_hdr.numRecvIdx / 3, s_hdr.hasDepth );
	}
	else
	{
		common->Warning( "soft-shadow capture: could not open %s.softcap for write", base.c_str() );
	}

	// human-readable / trimmable sidecar
	idFile* j = fileSystem->OpenFileWrite( base + ".softcap.json", "fs_savepath" );
	if( j != NULL )
	{
		j->Printf( "{\n  \"version\": %u,\n  \"screen\": [%u, %u],\n", s_hdr.version, s_hdr.screenW, s_hdr.screenH );
		j->Printf( "  \"vieworg\": [%.3f, %.3f, %.3f],\n", s_hdr.vieworg[0], s_hdr.vieworg[1], s_hdr.vieworg[2] );
		j->Printf( "  \"lights\": [\n" );
		for( size_t i = 0; i < s_lights.size(); i++ )
		{
			const softcapLight_t& L = s_lights[i];
			j->Printf( "    { \"index\": %u, \"origin\": [%.3f, %.3f, %.3f], \"penumbra\": %.3f, \"edges\": %u, \"casters\": %u }%s\n",
					   ( uint32_t )i, L.origin[0], L.origin[1], L.origin[2], L.penumbraSize, L.edgeCount, L.casterCount,
					   ( i + 1 < s_lights.size() ) ? "," : "" );
		}
		j->Printf( "  ]\n}\n" );
		fileSystem->CloseFile( j );
	}

	s_armed = false;
	ResetAccumulators();
}

// ------------------------------------------------------------------------------------------- console command
void R_CaptureSoftShadow_f( const idCmdArgs& args )
{
	ResetAccumulators();
	s_armed = true;

	// Render one clean frame and read it back with the GPU idle, mirroring idRenderSystemLocal::TakeScreenshot
	// (readback inside a live pass would nest command lists / read a half-written target). The FRONTEND half
	// (R_CaptureFrontendView) fires inside Draw() because we are armed; the backend half reads back afterwards.
	commonLocal.WaitGameThread();
	tr.SwapCommandBuffers( NULL, NULL, NULL, NULL, NULL, NULL );
	commonLocal.Draw();
	const emptyCommand_t* cmd = tr.SwapCommandBuffers( NULL, NULL, NULL, NULL, NULL, NULL );
	tr.RenderCommandBuffers( cmd );

	// Read back HERE, before the next SwapCommandBuffers: that swap presents and begins the next frame, whose
	// depth clear would wipe currentDepthImage (the LDR image survives it, but depth does not). Our readback
	// creates its own command list ordered after the frame and mapStagingTexture blocks, so the data is ready.
	if( s_frontendDone )
	{
		R_CaptureBackendFinish();		// screenshot + depth + write
	}
	else
	{
		common->Warning( "soft-shadow capture: no soft-shadow view was rendered (r_useSoftShadowVolumes off?)" );
	}

	tr.SwapCommandBuffers( NULL, NULL, NULL, NULL, NULL, NULL );
	s_armed = false;
	ResetAccumulators();
}
