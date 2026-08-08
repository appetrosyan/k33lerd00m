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
std::vector<float>           s_recvST;		// float2 packed per receiver vert (v5 TEXTURE TAIL)
std::vector<uint32_t>        s_recvMat;		// per receiver surface: index into s_matPtrs (v5)
std::vector<const idMaterial*> s_matPtrs;	// unique receiver materials, in first-seen order (v5)
idStr                        s_mapName;		// current map (self-identifying capture: reload + reconstruct, v4+)
std::vector<float>           s_shadowVerts;	// float3 packed, CAPPED shadow volumes (world space), v4+
std::vector<uint32_t>        s_shadowIdx;
std::vector<softcapShadowVol_t> s_shadowVols;
// the frontend records each shadow surf's GPU cache handles + transform; the backend reads them back once the
// GPU is idle (the shadow geometry lives only in shadowCache/shadowIndexCache - the drawSurf has no CPU tris).
struct ShadowSurfRec { vertCacheHandle_t vc, ic; int numIdx; float m[16]; float lgt[3]; uint32_t li; };
std::vector<ShadowSurfRec>   s_shadowSurfs;

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
	s_recvST.clear();
	s_recvMat.clear();
	s_matPtrs.clear();
	s_mapName.Clear();
	s_shadowVerts.clear();
	s_shadowIdx.clear();
	s_shadowVols.clear();
	s_shadowSurfs.clear();
	s_edgeRange.clear();
	s_frontendDone = false;
}

// sequential base name "softcapNNNN" in the same spot screenshots go (fs_savepath).
// Read a RESIDENT texture back from the GPU as RGBA8 (v5 texture tail). Blit-decodes any sampleable
// format (the shipped game has only BC-compressed .bimage data - there are no source TGAs to load, so
// disk loading yields nothing; the VRAM copy is the only real texel source). Mirrors R_ReadPixelsRGB8.
bool ReadImageRGBA8( idImage* img, std::vector<uint8_t>& out, int& w, int& h )
{
	if( img == NULL || img->GetTextureHandle() == NULL ) { return false; }
	nvrhi::IDevice* device = deviceManager->GetDevice();
	nvrhi::ITexture* texture = img->GetTextureHandle();
	nvrhi::TextureDesc desc = texture->getDesc();
	w = ( int )desc.width;
	h = ( int )desc.height;
	if( w <= 0 || h <= 0 ) { return false; }

	nvrhi::CommandListHandle commandList = device->createCommandList();
	commandList->open();
	commandList->beginTrackingTextureState( texture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource );

	desc.format = nvrhi::Format::SRGBA8_UNORM;
	desc.isRenderTarget = true;
	desc.initialState = nvrhi::ResourceStates::RenderTarget;
	desc.keepInitialState = true;
	desc.mipLevels = 1;
	nvrhi::TextureHandle tempTexture = device->createTexture( desc );
	nvrhi::FramebufferHandle tempFramebuffer = device->createFramebuffer( nvrhi::FramebufferDesc().addColorAttachment( tempTexture ) );
	backEnd.GetCommonPasses().BlitTexture( commandList, tempFramebuffer, texture );

	nvrhi::StagingTextureHandle stagingTexture = device->createStagingTexture( desc, nvrhi::CpuAccessMode::Read );
	commandList->copyTexture( stagingTexture, nvrhi::TextureSlice(), tempTexture, nvrhi::TextureSlice() );
	commandList->setTextureState( texture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource );
	commandList->commitBarriers();
	commandList->close();
	device->executeCommandList( commandList );

	size_t rowPitch = 0;
	void* pData = device->mapStagingTexture( stagingTexture, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch );
	if( pData == NULL ) { return false; }
	out.resize( ( size_t )w * h * 4 );
	for( int y = 0; y < h; y++ )
	{
		memcpy( out.data() + ( size_t )y * w * 4, ( const uint8_t* )pData + ( size_t )y * rowPitch, ( size_t )w * 4 );
	}
	device->unmapStagingTexture( stagingTexture );
	return true;
}

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
	s_hdr.reserved[0] = ( uint32_t )rv.time[0];					// game time (ms) - determinism for reconstruction
	s_mapName = commonLocal.GetCurrentMapName();				// map identity - reload + reconstruct from the engine
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

		// record this light's CAPPED shadow-volume surfs; geometry lives in the GPU caches, read back in backend
		for( int spass = 0; spass < 2; spass++ )
		{
			const drawSurf_t* slist = ( spass == 0 ) ? vLight->globalShadows : vLight->localShadows;
			for( const drawSurf_t* s = slist; s != NULL; s = s->nextOnLight )
			{
				if( s->numIndexes == 0 || s->shadowCache == 0 || s->indexCache == 0 || s->space == NULL ) { continue; }
				ShadowSurfRec rec;
				rec.vc = s->shadowCache; rec.ic = s->indexCache; rec.numIdx = s->numIndexes; rec.li = lightIndex;
				for( int m = 0; m < 16; m++ ) { rec.m[m] = s->space->modelMatrix[m]; }
				rec.lgt[0] = vLight->globalLightOrigin.x; rec.lgt[1] = vLight->globalLightOrigin.y; rec.lgt[2] = vLight->globalLightOrigin.z;
				s_shadowSurfs.push_back( rec );
			}
		}

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
				// v5 TEXTURE TAIL: per-vertex UVs + this surface's material (unique-listed; the diffuse
				// image is baked into the capture at write time so offline verification renders REAL
				// textures, not a stand-in pattern).
				const srfTriangles_t* rtri = rs->frontEndGeo;
				for( int v = 0; v < rtri->numVerts; v++ )
				{
					const idVec2 st = rtri->verts[v].GetTexCoord();
					s_recvST.push_back( st.x );
					s_recvST.push_back( st.y );
				}
				uint32_t mi = 0;
				for( ; mi < ( uint32_t )s_matPtrs.size(); mi++ )
				{
					if( s_matPtrs[mi] == rs->material ) { break; }
				}
				if( mi == ( uint32_t )s_matPtrs.size() ) { s_matPtrs.push_back( rs->material ); }
				s_recvMat.push_back( mi );
			}
		}
		s_lights.push_back( L );
	}

	s_frontendDone = true;
}

// ------------------------------------------------------------------- backend half: screenshot + write files
// Read a region of a GPU buffer back to the CPU (staging copy + map). Used for the shadow-volume caches,
// whose geometry exists only on the GPU (the shadow drawSurf has no CPU triangles).
static bool R_ReadbackGPUBuffer( nvrhi::IBuffer* src, uint64 srcOffset, uint64 size, void* dst )
{
	if( src == NULL || size == 0 || dst == NULL ) { return false; }
	nvrhi::IDevice* device = deviceManager->GetDevice();
	nvrhi::BufferDesc sd;
	sd.byteSize = size;
	sd.cpuAccess = nvrhi::CpuAccessMode::Read;
	sd.initialState = nvrhi::ResourceStates::CopyDest;	// let nvrhi auto-manage the state (source is keepInitialState)
	sd.keepInitialState = true;
	sd.debugName = "softcapReadback";
	nvrhi::BufferHandle staging = device->createBuffer( sd );
	if( staging == NULL ) { return false; }
	nvrhi::CommandListHandle cl = device->createCommandList();
	cl->open();
	cl->copyBuffer( staging, 0, src, srcOffset, size );
	cl->close();
	device->executeCommandList( cl );
	void* mapped = device->mapBuffer( staging, nvrhi::CpuAccessMode::Read );
	if( mapped == NULL ) { return false; }
	std::memcpy( dst, mapped, ( size_t )size );
	device->unmapBuffer( staging );
	return true;
}

void R_CaptureBackendFinish()
{
	if( !s_armed || !s_frontendDone )
	{
		return;
	}
	// read back the CAPPED shadow-volume geometry recorded in the frontend (GPU caches -> world-space tris)
	for( const ShadowSurfRec& rec : s_shadowSurfs )
	{
		idVertexBuffer vb; idIndexBuffer ib;
		if( !vertexCache.GetVertexBuffer( rec.vc, &vb ) || !vertexCache.GetIndexBuffer( rec.ic, &ib ) ) { continue; }
		int numVerts = vb.GetSize() / ( int )sizeof( idShadowVert );
		// Read the FULL index buffer, not rec.numIdx: the drawSurf's count is the engine's SELECTED variant
		// (numShadowIndexesNoCaps when the eye is outside = capless), but the buffer holds the full CAPPED set
		// (numShadowIndexes = sides + front/rear caps). Reading it captures a closed volume regardless of the
		// capture viewpoint, so the offline z-fail count is faithful (a capless volume has no robust inside/out).
		int numIdx = ib.GetSize() / ( int )sizeof( triIndex_t );
		if( numVerts <= 0 || numIdx <= 0 ) { continue; }
		std::vector<idShadowVert> verts( numVerts );
		std::vector<triIndex_t>   idxs( numIdx );
		if( !R_ReadbackGPUBuffer( vb.GetAPIObject(), ( uint64 )vb.GetOffset(), ( uint64 )numVerts * sizeof( idShadowVert ), verts.data() ) ) { continue; }
		if( !R_ReadbackGPUBuffer( ib.GetAPIObject(), ( uint64 )ib.GetOffset(), ( uint64 )numIdx * sizeof( triIndex_t ), idxs.data() ) ) { continue; }
		softcapShadowVol_t sv; sv.lightIndex = rec.li;
		sv.firstVert = ( uint32_t )( s_shadowVerts.size() / 3 );
		sv.firstIdx  = ( uint32_t )s_shadowIdx.size();
		idVec3 lLocal; R_GlobalPointToLocal( rec.m, idVec3( rec.lgt[0], rec.lgt[1], rec.lgt[2] ), lLocal );
		const float BIG = 16000.0f;
		for( int v = 0; v < numVerts; v++ )
		{
			const idVec4& p = verts[v].xyzw;
			idVec3 local( p.x, p.y, p.z );
			if( p.w < 0.5f ) { local = local + ( local - lLocal ) * BIG; }		// w==0: extrude to (far) infinity from the light
			idVec3 w; R_LocalPointToGlobal( rec.m, local, w );
			s_shadowVerts.push_back( w.x ); s_shadowVerts.push_back( w.y ); s_shadowVerts.push_back( w.z );
		}
		sv.numVert = ( uint32_t )numVerts;
		for( int i = 0; i < numIdx; i++ ) { s_shadowIdx.push_back( sv.firstVert + ( uint32_t )idxs[i] ); }
		sv.numIdx = ( uint32_t )numIdx;
		s_shadowVols.push_back( sv );
	}
	s_hdr.reserved[2] = ( uint32_t )s_shadowVols.size();
	s_hdr.reserved[3] = ( uint32_t )( s_shadowVerts.size() / 3 );
	s_hdr.reserved[4] = ( uint32_t )s_shadowIdx.size();

	idStr base;
	NextCaptureBaseName( base );

	// FRAME column (full shaded LDR) straight to PNG, reusing the engine path
	idStr png = base + ".png";
	R_ReadPixelsRGB8( deviceManager->GetDevice(), &backEnd.GetCommonPasses(),
					  globalImages->currentRenderHDRImage->GetTextureHandle(), nvrhi::ResourceStates::ShaderResource, png.c_str() );

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
	s_hdr.reserved[1]   = ( uint32_t )s_mapName.Length();		// MAPNAME block byte length (trailing, v4+)

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
		if( s_hdr.reserved[1] > 0 ) { f->Write( s_mapName.c_str(),  ( int )s_hdr.reserved[1] ); }	// MAPNAME block
		if( s_hdr.reserved[2] > 0 )		// SHADOWVOL section (v4+): vols table, then verts, then indices
		{
			f->Write( s_shadowVols.data(), ( int )( s_shadowVols.size() * sizeof( softcapShadowVol_t ) ) );
			if( !s_shadowVerts.empty() ) { f->Write( s_shadowVerts.data(), ( int )( s_shadowVerts.size() * sizeof( float ) ) ); }
			if( !s_shadowIdx.empty() )   { f->Write( s_shadowIdx.data(),   ( int )( s_shadowIdx.size()   * sizeof( uint32_t ) ) ); }
		}
		// v5 TEXTURE TAIL: self-describing, appended last - old readers stop before it. For each unique
		// receiver material, load its diffuse image from disk (the GPU copy is not CPU-readable), box-
		// downsample to <= SOFTCAP_TEX_MAX per side, store RGB8. Failures store a 1x1 grey.
		if( !s_recvMat.empty() && s_recvST.size() == s_recvVerts.size() / 3 * 2 )
		{
			std::vector<softcapMaterial_t> mats;
			std::vector<uint8_t> texels;
			for( const idMaterial* m : s_matPtrs )
			{
				softcapMaterial_t rec = {};
				idStr::Copynz( rec.name, m ? m->GetName() : "<null>", sizeof( rec.name ) );
				rec.firstTexel = ( uint32_t )texels.size();
				idImage* img = m ? const_cast<idImage*>( m->GetFastPathDiffuseImage() ) : NULL;
				if( img == NULL && m != NULL )
				{
					for( int st = 0; st < m->GetNumStages() && img == NULL; st++ )
					{
						const shaderStage_t* stage = m->GetStage( st );
						if( stage != NULL && stage->lighting == SL_DIFFUSE ) { img = stage->texture.image; }
					}
				}
				if( img == NULL && m != NULL )		// no diffuse stage (unlit/utility): first stage image
				{
					for( int st = 0; st < m->GetNumStages() && img == NULL; st++ )
					{
						const shaderStage_t* stage = m->GetStage( st );
						if( stage != NULL ) { img = stage->texture.image; }
					}
				}
				std::vector<uint8_t> pic;
				int pw = 0, ph = 0;
				if( ReadImageRGBA8( img, pic, pw, ph ) && pw > 0 && ph > 0 )
				{
					const int step = Max( 1, Max( pw, ph ) / SOFTCAP_TEX_MAX );
					rec.texW = ( uint32_t )Max( 1, pw / step );
					rec.texH = ( uint32_t )Max( 1, ph / step );
					for( uint32_t y = 0; y < rec.texH; y++ )
						for( uint32_t x = 0; x < rec.texW; x++ )
						{
							int r = 0, g = 0, b = 0, n = 0;
							for( int sy = 0; sy < step; sy++ )
								for( int sx = 0; sx < step; sx++ )
								{
									int px = ( int )x * step + sx, py = ( int )y * step + sy;
									if( px >= pw || py >= ph ) { continue; }
									const uint8_t* t = pic.data() + ( ( size_t )py * pw + px ) * 4;
									r += t[0]; g += t[1]; b += t[2]; n++;
								}
							n = Max( n, 1 );
							texels.push_back( ( uint8_t )( r / n ) );
							texels.push_back( ( uint8_t )( g / n ) );
							texels.push_back( ( uint8_t )( b / n ) );
						}
				}
				else
				{
					rec.texW = rec.texH = 1;
					texels.push_back( 128 ); texels.push_back( 128 ); texels.push_back( 128 );
				}
				mats.push_back( rec );
			}
			const uint32_t tail[5] = { SOFTCAP_TAIL_MAGIC, ( uint32_t )mats.size(), ( uint32_t )texels.size(),
									   ( uint32_t )s_recvST.size(), ( uint32_t )s_recvMat.size() };
			f->Write( tail, sizeof( tail ) );
			f->Write( mats.data(),     ( int )( mats.size()     * sizeof( softcapMaterial_t ) ) );
			f->Write( texels.data(),   ( int )texels.size() );
			f->Write( s_recvST.data(), ( int )( s_recvST.size() * sizeof( float ) ) );
			f->Write( s_recvMat.data(), ( int )( s_recvMat.size() * sizeof( uint32_t ) ) );
			common->Printf( "soft-shadow capture: v5 texture tail - %zu materials, %zu KB texels\n",
							mats.size(), texels.size() / 1024 );
		}
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

	// The .softcap above is the live-config analytic dump. Now capture EVERYTHING the comparison needs -
	// RT-ref + analytic-bandoff + analytic-bandon frame/term columns + the cvar manifest - forcing each config
	// itself so the result never depends on how the game was launched (an RT-off launch must still yield a
	// valid RT reference, not a black mask).
	R_CaptureShadowRefs_f( args );
}

// Render one clean frame with the GPU idle (mirrors R_CaptureSoftShadow_f) so a following readback sees a
// fully-written, non-nested target. Leaves the frame un-presented; the caller presents after the last readback.
static void R_RenderOneFrame()
{
	commonLocal.WaitGameThread();
	tr.SwapCommandBuffers( NULL, NULL, NULL, NULL, NULL, NULL );
	commonLocal.Draw();
	const emptyCommand_t* cmd = tr.SwapCommandBuffers( NULL, NULL, NULL, NULL, NULL, NULL );
	tr.RenderCommandBuffers( cmd );
}

// ------------------------------------------------------------- self-contained shadow-reference capture
// One command does everything: freezes game logic, cycles every shadow config, dumps both columns per config
// (FRAME = full shaded LDR, TERM = isolated shadow visibility, 1 = lit), then restores every touched cvar.
// All configs render from the same frozen viewpoint so the columns are pixel-aligned for diffing. The debug
// oracle is rt_ref (RT converged: 1024 rays, no denoise, no PCSS) with its area-light radius pinned to the
// analytic light-disk radius (r_shadowPenumbraSize) so RT and analytic amplitudes agree. Extend to another
// visual effect by adding a row to cfgs[]. NOTE: term columns pass through the LDR sRGB/tonemap transfer
// (analytic) and the R8->sRGBA8 blit (RT mask); both are monotonic - relative over/under-occlusion is honest,
// exact numeric agreement needs linearising both offline.
void R_CaptureShadowRefs_f( const idCmdArgs& args )
{
	static const char* const touched[] =
	{
		"g_stopTime", "r_useStencilShadows", "r_useSoftShadowVolumes", "r_useRTShadows",
		"r_rtShadowAnalyticPenumbra", "r_rtShadowDenoise", "r_rtShadowRays", "r_rtShadowSoftRadius",
		"r_softShadowBandMask", "r_softShadowDebugShader",
	};
	const int nTouched = ( int )( sizeof( touched ) / sizeof( touched[0] ) );
	idStrList prev;
	for( int i = 0; i < nTouched; i++ )
	{
		prev.Append( cvarSystem->GetCVarString( touched[i] ) );
	}

	// freeze animated content (flicker lights, particles, AI) so the per-config sub-frames align exactly
	cvarSystem->SetCVarInteger( "g_stopTime", 1 );
	// amplitude agreement: RT area-light radius == analytic light-disk radius
	cvarSystem->SetCVarFloat( "r_rtShadowSoftRadius", cvarSystem->GetCVarFloat( "r_shadowPenumbraSize" ) );

	struct refCfg_t { const char* label; const char* setup; bool termFromMask; };
	static const refCfg_t cfgs[] =
	{
		{ "rt_ref",      "r_useStencilShadows 0 ; r_useSoftShadowVolumes 0 ; r_useRTShadows 1 ; r_rtShadowAnalyticPenumbra 0 ; r_rtShadowDenoise 0 ; r_rtShadowRays 1024", true  },
		{ "ana_bandoff", "r_useRTShadows 0 ; r_useSoftShadowVolumes 1 ; r_softShadowBandMask 0",                                                                          false },
		{ "ana_bandon",  "r_useRTShadows 0 ; r_useSoftShadowVolumes 1 ; r_softShadowBandMask 1",                                                                          false },
	};
	const int nCfg = ( int )( sizeof( cfgs ) / sizeof( cfgs[0] ) );

	for( int c = 0; c < nCfg; c++ )
	{
		idStr setup = cfgs[c].setup;
		setup += "\n";
		cmdSystem->BufferCommandText( CMD_EXEC_NOW, setup.c_str() );

		// FRAME column: full shaded LDR (real coverage, no debug override)
		cvarSystem->SetCVarInteger( "r_softShadowDebugShader", 0 );
		R_RenderOneFrame();
		idStr frame;
		frame.Format( "shadowref_%s_frame.png", cfgs[c].label );
		R_ReadPixelsRGB8( deviceManager->GetDevice(), &backEnd.GetCommonPasses(),
						  globalImages->currentRenderHDRImage->GetTextureHandle(), nvrhi::ResourceStates::ShaderResource, frame.c_str() );

		// TERM column: isolated shadow visibility (1 = lit)
		idStr term;
		term.Format( "shadowref_%s_term.png", cfgs[c].label );
		if( cfgs[c].termFromMask )
		{
			// RT resolves visibility into rtShadowMaskImage (R8; value in the red channel after the RGB8 blit),
			// already produced by the FRAME render above.
			R_ReadPixelsRGB8( deviceManager->GetDevice(), &backEnd.GetCommonPasses(),
							  globalImages->rtShadowMaskImage->GetTextureHandle(), nvrhi::ResourceStates::ShaderResource, term.c_str() );
		}
		else
		{
			// Analytic wedge computes coverage inline; debug mode 8 outputs the raw shadow factor to LDR.
			cvarSystem->SetCVarInteger( "r_softShadowDebugShader", 8 );
			R_RenderOneFrame();
			R_ReadPixelsRGB8( deviceManager->GetDevice(), &backEnd.GetCommonPasses(),
							  globalImages->currentRenderHDRImage->GetTextureHandle(), nvrhi::ResourceStates::ShaderResource, term.c_str() );
		}
		common->Printf( "shadowref: wrote shadowref_%s_{frame,term}.png\n", cfgs[c].label );
	}

	tr.SwapCommandBuffers( NULL, NULL, NULL, NULL, NULL, NULL );	// present the last rendered frame

	// CVAR MANIFEST: everything needed to reproduce/interpret the comparison, independent of how the game was
	// launched. Dumps every renderer cvar (r_shadowPenumbraSize, the RT knobs, band mode, ...) as `set n v`.
	idFile* cf = fileSystem->OpenFileWrite( "shadowref.cvars.cfg", "fs_savepath" );
	if( cf != NULL )
	{
		cvarSystem->WriteFlaggedVariables( CVAR_RENDERER, "set", cf );
		fileSystem->CloseFile( cf );
	}

	// restore every touched cvar
	for( int i = 0; i < nTouched; i++ )
	{
		cvarSystem->SetCVarString( touched[i], prev[i].c_str() );
	}
	common->Printf( "shadowref: done (%d configs) -> shadowref_{rt_ref,ana_bandoff,ana_bandon}_{frame,term}.png + shadowref.cvars.cfg; restored cvars\n", nCfg );
}
