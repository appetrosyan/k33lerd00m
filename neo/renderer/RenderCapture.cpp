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
#include "RenderWorld_local.h"
#include "RenderCapture.h"
#include "../framework/Common_local.h"

#include <sys/DeviceManager.h>

#include <vector>
#include <map>
#include <cstdio>
#include <algorithm>

extern DeviceManager* deviceManager;

// soft light's real atlas placement, published by the backend (RenderBackend.cpp) so the tile dump
// reads the tiles the locator actually samples instead of a stale hardcoded offset.
idVec2i g_softDbgAtlasOff[6] = { {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1} };
idVec2i g_softDbgAtlasSize = { -1, -1 };

// --- soft-shadow "goto" ---------------------------------------------------------------------------------------
// Positioning the view at a capture can't be done inside a command handler (re-entrant commonLocal.Frame() unloads
// the map), and a bare setviewpos is overridden by the map's OPENING CINEMATIC camera. So arm a countdown here and
// let the NORMAL frame loop (Common::Frame -> R_SoftShadowGotoTick) skip the cinematic and pin the viewpoint over
// several real frames before the A/B test renders. Usage: `softShadowGoto <cap>` ; `wait 90` ; `testSoftShadowLocator <cap>`.
static idVec3   s_gotoOrg;
static idAngles s_gotoAng;
static int      s_gotoFrames = 0;

// EXPLICIT soft-shadow test config. The self-test must NOT inherit archived cvars from D3BFGConfig.cfg: e.g.
// r_useRTShadows 1 silently makes the whole soft-wedge path INERT (frontend gate is `... && !r_useRTShadows`), so
// the test measures RT instead of what it claims to. Pin every shadow-relevant cvar to a documented value BEFORE
// the frontend runs (i.e. from softShadowGoto, which precedes the geometry-building frames), and LOUDLY log any
// that differed from the live/archived value so a stale config can never masquerade as a code result again.
struct SoftPin { const char* name; const char* value; };
static const SoftPin s_softTestConfig[] =
{
	{ "r_useRTShadows",          "0" },	// CRITICAL: RT off, else the soft-wedge path is inert and RT renders instead
	{ "r_useStencilShadows",     "0" },
	{ "r_useSoftShadowVolumes",  "1" },
	{ "r_useShadowMapping",      "0" },
	{ "r_useShadowAtlas",        "1" },
	{ "r_shadowMapPCSS",         "1" },
	{ "r_shadowMapPCSSScale",    "4" },
	{ "r_softShadowAAM",         "0" },	// PURE PCSS: no AAM band. The PCSS locator alone gates lit/penumbra/umbra.
	{ "r_softShadowBandMask",    "0" },
	{ "r_softShadowStencilOnly", "0" },
	{ "r_softShadowEmergentUmbra", "0" },
	{ "r_softShadowContinuous",  "0" },
	{ "r_useTemporalAA",         "0" },
	{ "r_softShadowDebugShader", "0" },
};

void R_SoftShadowPinTestConfig( bool verbose )
{
	int overridden = 0;
	if( verbose ) { common->Printf( "[softtest] ==== pinning soft-shadow test config (archived cvars are IGNORED) ====\n" ); }
	for( int i = 0; i < ( int )( sizeof( s_softTestConfig ) / sizeof( s_softTestConfig[0] ) ); i++ )
	{
		const char* was = cvarSystem->GetCVarString( s_softTestConfig[i].name );
		const bool diff = ( idStr::Cmp( was, s_softTestConfig[i].value ) != 0 );
		if( verbose )
		{
			if( diff )	{ common->Printf( "[softtest]   %-26s %s -> %s   <== ARCHIVED VALUE OVERRIDDEN\n", s_softTestConfig[i].name, was, s_softTestConfig[i].value ); }
			else		{ common->Printf( "[softtest]   %-26s = %s\n", s_softTestConfig[i].name, s_softTestConfig[i].value ); }
		}
		if( diff ) { overridden++; }
		cvarSystem->SetCVarString( s_softTestConfig[i].name, s_softTestConfig[i].value );
	}
	if( verbose ) { common->Printf( "[softtest] ==== %d archived cvar(s) overridden ====\n", overridden ); }
}

void R_SoftShadowGotoTick()
{
	if( s_gotoFrames <= 0 )
	{
		return;
	}
	// re-pin every frame the goto is active: the frontend that builds soft edges/occluders runs on THESE frames,
	// so the config must be correct now, not just at test time (a menu/console tick could otherwise re-archive one).
	R_SoftShadowPinTestConfig( false );
	if( common->Game() != NULL && common->Game()->CheckInCinematic() )
	{
		common->Game()->SkipCinematicScene();		// same path as pressing ESC during the intro
	}
	// EXEC_NOW (not APPEND): this runs right before the game think, so the teleport lands BEFORE the player rebuilds
	// its render view this frame - otherwise the pose is always one frame stale and the no-input usercmd reverts the
	// view angle, so every capture rendered the same (quicksave) view regardless of its yaw.
	cmdSystem->BufferCommandText( CMD_EXEC_NOW, va( "setviewpos %f %f %f %f %f\n",
							s_gotoOrg.x, s_gotoOrg.y, s_gotoOrg.z, s_gotoAng.yaw, s_gotoAng.pitch ) );
	s_gotoFrames--;
}

void R_SoftShadowGoto_f( const idCmdArgs& args )
{
	if( args.Argc() < 2 )
	{
		common->Warning( "usage: softShadowGoto <capture.softcap> | <x y z yaw pitch>  (then `wait 90` before testSoftShadowLocator)" );
		return;
	}
	// FREE-CAM form: 5+ args -> raw "x y z yaw pitch", so the harness can render ANY spot in the live map, not just
	// a captured viewpoint (dynamic props / elevated casters that no .softcap covers). testSoftShadowLocator takes
	// the same form. No file needed - the goto tick just teleports the player there.
	if( args.Argc() >= 6 )
	{
		s_gotoOrg.Set( atof( args.Argv( 1 ) ), atof( args.Argv( 2 ) ), atof( args.Argv( 3 ) ) );
		s_gotoAng.Set( atof( args.Argv( 5 ) ), atof( args.Argv( 4 ) ), 0.0f );	// (pitch, yaw, roll)
		s_gotoFrames = 120;
		R_SoftShadowPinTestConfig( true );
		common->Printf( "[softtest] goto armed (free-cam): (%.0f %.0f %.0f) yaw %.0f pitch %.0f - pinning for 120 frames\n",
						s_gotoOrg.x, s_gotoOrg.y, s_gotoOrg.z, s_gotoAng.yaw, s_gotoAng.pitch );
		return;
	}
	FILE* cf = fopen( args.Argv( 1 ), "rb" );
	softcapHeader_t hdr;
	if( cf == NULL || fread( &hdr, sizeof( hdr ), 1, cf ) != 1 || hdr.magic != SOFTCAP_MAGIC )
	{
		if( cf != NULL ) { fclose( cf ); }
		common->Warning( "softShadowGoto: cannot read capture %s", args.Argv( 1 ) );
		return;
	}
	fclose( cf );
	s_gotoOrg.Set( hdr.vieworg[0], hdr.vieworg[1], hdr.vieworg[2] );
	s_gotoAng = idVec3( hdr.viewaxis[0], hdr.viewaxis[1], hdr.viewaxis[2] ).ToAngles();
	s_gotoFrames = 120;
	R_SoftShadowPinTestConfig( true );	// pin+log the full config NOW, before the frontend builds geometry
	common->Printf( "[softtest] goto armed: (%.0f %.0f %.0f) yaw %.0f pitch %.0f - skipping cinematic + pinning for 120 frames\n",
					s_gotoOrg.x, s_gotoOrg.y, s_gotoOrg.z, s_gotoAng.yaw, s_gotoAng.pitch );
}

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

// ---------------------------------------------------------- in-engine AUTOMATED locator self-check
// Runs the REAL renderer twice from the same frozen viewpoint - once with the ray-traced oracle (1024 rays),
// once with the soft-shadow + PCSS-locator hybrid - and measures FALSE SHADOW: pixels the oracle leaves lit
// but the hybrid darkens. This exercises the actual frontend, shaders, shadow atlas and uniform plumbing (the
// exact surface the offline CPU test cannot reach), so a regression like "locator shadows everywhere" fails
// here automatically instead of needing a human to eyeball debug modes. Prints a PASS/FAIL verdict.
// Load a map first, then: `testSoftShadowLocator`. Needs ray-query hardware for the oracle.
static float SoftTestLum( const uint8_t* p )
{
	return ( 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2] ) / 255.0f;
}

// ---------------------------------------------------------- MINIMAL-INIT self-test (no game/sound/menu/player)
// The canonical game-free spawnargs->renderLight parser, copied VERBATIM from tools/compilers/dmap/map.cpp
// (its DMAP branch). Kept identical so the harness's lights match exactly what dmap and the editor build - the
// game's idGameEdit::ParseSpawnArgsToRenderLight is the same code, but lives in the game DLL we deliberately
// don't boot here.
static void SelfTestParseLight( const idDict* args, renderLight_t* renderLight )
{
	bool gotTarget, gotUp, gotRight;
	const char* texture;
	idVec3 color;

	memset( renderLight, 0, sizeof( *renderLight ) );

	if( !args->GetVector( "light_origin", "", renderLight->origin ) )
	{
		args->GetVector( "origin", "", renderLight->origin );
	}
	gotTarget = args->GetVector( "light_target", "", renderLight->target );
	gotUp = args->GetVector( "light_up", "", renderLight->up );
	gotRight = args->GetVector( "light_right", "", renderLight->right );
	args->GetVector( "light_start", "0 0 0", renderLight->start );
	if( !args->GetVector( "light_end", "", renderLight->end ) )
	{
		renderLight->end = renderLight->target;
	}
	if( ( gotTarget || gotUp || gotRight ) != ( gotTarget && gotUp && gotRight ) )
	{
		return;
	}
	if( !gotTarget )
	{
		renderLight->pointLight = true;
		args->GetVector( "light_center", "0 0 0", renderLight->lightCenter );
		if( !args->GetVector( "light_radius", "300 300 300", renderLight->lightRadius ) )
		{
			float radius;
			args->GetFloat( "light", "300", radius );
			renderLight->lightRadius[0] = renderLight->lightRadius[1] = renderLight->lightRadius[2] = radius;
		}
	}
	idAngles angles;
	idMat3 mat;
	if( !args->GetMatrix( "light_rotation", "1 0 0 0 1 0 0 0 1", mat ) )
	{
		if( !args->GetMatrix( "rotation", "1 0 0 0 1 0 0 0 1", mat ) )
		{
			if( args->GetAngles( "light_angles", "0 0 0", angles ) || args->GetAngles( "angles", "0 0 0", angles ) )
			{
				angles[0] = idMath::AngleNormalize360( angles[0] );
				angles[1] = idMath::AngleNormalize360( angles[1] );
				angles[2] = idMath::AngleNormalize360( angles[2] );
				mat = angles.ToMat3();
			}
			else
			{
				args->GetFloat( "angle", "0", angles[1] );
				angles[0] = 0;
				angles[1] = idMath::AngleNormalize360( angles[1] );
				angles[2] = 0;
				mat = angles.ToMat3();
			}
		}
	}
	mat[0].FixDegenerateNormal();
	mat[1].FixDegenerateNormal();
	mat[2].FixDegenerateNormal();
	renderLight->axis = mat;

	args->GetVector( "_color", "1 1 1", color );
	renderLight->shaderParms[SHADERPARM_RED]   = color[0];
	renderLight->shaderParms[SHADERPARM_GREEN] = color[1];
	renderLight->shaderParms[SHADERPARM_BLUE]  = color[2];
	args->GetFloat( "shaderParm3", "1", renderLight->shaderParms[SHADERPARM_TIMESCALE] );
	renderLight->shaderParms[SHADERPARM_TIMEOFFSET] = 0;
	args->GetFloat( "shaderParm5", "0", renderLight->shaderParms[5] );
	args->GetFloat( "shaderParm6", "0", renderLight->shaderParms[6] );
	args->GetFloat( "shaderParm7", "0", renderLight->shaderParms[SHADERPARM_MODE] );
	args->GetBool( "noshadows", "0", renderLight->noShadows );
	args->GetBool( "nospecular", "0", renderLight->noSpecular );
	args->GetBool( "parallel", "0", renderLight->parallel );
	args->GetString( "texture", "lights/squarelight1", &texture );
	renderLight->shader = declManager->FindMaterial( texture, false );
}

// RenderScene -> flush to GPU -> read back the LDR result (mirrors the envprobe bake path: pure tr.* calls, no
// commonLocal.Draw, so it works at minimal init with no game thread).
static bool SelfTestRenderReadback( idRenderWorld* rw, renderView_t* rv, std::vector<uint8_t>& out, int& w, int& h )
{
	fprintf( stderr, "[bread]  RenderScene...\n" ); fflush( stderr );
	rw->RenderScene( rv );
	fprintf( stderr, "[bread]  swap1...\n" ); fflush( stderr );
	const emptyCommand_t* cmd = tr.SwapCommandBuffers( NULL, NULL, NULL, NULL, NULL, NULL );
	fprintf( stderr, "[bread]  RenderCommandBuffers...\n" ); fflush( stderr );
	tr.RenderCommandBuffers( cmd );
	fprintf( stderr, "[bread]  swap2...\n" ); fflush( stderr );
	tr.SwapCommandBuffers( NULL, NULL, NULL, NULL, NULL, NULL );
	fprintf( stderr, "[bread]  readback...\n" ); fflush( stderr );
	bool ok = ReadImageRGBA8( globalImages->currentRenderHDRImage, out, w, h );
	fprintf( stderr, "[bread]  readback done\n" ); fflush( stderr );
	return ok;
}

// Minimal-init soft-shadow self-test. Common.cpp diverts here right after renderSystem->Init(), before the
// game/sound/menu/player boot - so ONLY device+shaders+images+vertexCache+decls are up. Loads the map's
// renderWorld + its real .map lights + the paired capture's camera, renders the RT oracle vs the soft+PCSS
// hybrid through the REAL backend, and prints a false-shadow verdict. Returns false-shadow pixel count (0=pass,
// -1=setup error).
int R_SoftShadowSelfTest( const char* mapName )
{
	// paired capture (neo/tests/data/<basename>.softcap) supplies the artifact CAMERA the user captured
	globalImages->LoadDeferredImages();		// splash/base images the render stack deferred at boot

	idRenderWorld* rw = renderSystem->AllocRenderWorld();
	if( !rw->InitFromMap( mapName ) )
	{
		common->Printf( "[selftest] FAIL: renderWorld InitFromMap(%s) failed\n", mapName );
		renderSystem->FreeRenderWorld( rw );
		return -1;
	}

	// Gather the map's REAL lights (game-free parse), and DERIVE the camera from them: the .softcap camera is
	// in a different coordinate frame than the loaded .proc here, so we instead sit the camera among the lights
	// (guaranteed to be in the lit, geometry-filled part of the world) and look into the cluster.
	idMapFile map;
	if( !map.Parse( mapName ) )
	{
		common->Printf( "[selftest] FAIL: could not parse %s.map\n", mapName );
		renderSystem->FreeRenderWorld( rw );
		return -1;
	}
	std::vector<renderLight_t> lights;
	idVec3 centroid( 0, 0, 0 );
	for( int e = 0; e < map.GetNumEntities(); e++ )
	{
		idMapEntity* ent = map.GetEntity( e );
		if( ent == NULL || idStr::Icmp( ent->epairs.GetString( "classname" ), "light" ) != 0 ) { continue; }
		renderLight_t rl;
		SelfTestParseLight( &ent->epairs, &rl );
		if( rl.shader == NULL ) { continue; }
		lights.push_back( rl );
		centroid += rl.origin;
	}
	if( lights.empty() )
	{
		common->Printf( "[selftest] FAIL: %s has no lights\n", mapName );
		renderSystem->FreeRenderWorld( rw );
		return -1;
	}
	centroid /= ( float )lights.size();

	// anchor = the light FARTHEST from the centroid (a corner of the lit area); camera sits just behind it and
	// looks toward the centroid, so the lit cluster fills the frame. Add every light within radius of the camera.
	int anchor = 0;
	float bestD = -1.0f;
	for( int i = 0; i < ( int )lights.size(); i++ )
	{
		float d = ( lights[i].origin - centroid ).LengthSqr();
		if( d > bestD ) { bestD = d; anchor = i; }
	}
	idVec3 look = centroid - lights[anchor].origin;
	if( look.LengthSqr() < 1.0f ) { look = idVec3( 1, 0, 0 ); }
	look.Normalize();
	idVec3 camOrg = lights[anchor].origin - look * 48.0f;		// just behind the corner light, not on top of it

	// the NEAREST few lights only - a couple of shadow-casting lights expose the systematic false-shadow bug,
	// and the stencil oracle (single-threaded, whole-world shadow volumes per light) is far too slow with more.
	std::vector<int> order;
	for( int i = 0; i < ( int )lights.size(); i++ ) { order.push_back( i ); }
	std::sort( order.begin(), order.end(), [&]( int a, int b )
	{
		return ( lights[a].origin - camOrg ).LengthSqr() < ( lights[b].origin - camOrg ).LengthSqr();
	} );
	int nLights = 0;
	for( int k = 0; k < ( int )order.size() && nLights < 3; k++ )
	{
		rw->AddLightDef( &lights[order[k]] );
		nLights++;
	}
	fprintf( stderr, "[selftest] BREAD: %d/%d lights, cam (%.0f %.0f %.0f) look (%.2f %.2f %.2f)\n",
			 nLights, ( int )lights.size(), camOrg.x, camOrg.y, camOrg.z, look.x, look.y, look.z ); fflush( stderr );

	renderView_t rv;
	memset( &rv, 0, sizeof( rv ) );
	rv.vieworg = camOrg;
	rv.viewaxis = look.ToMat3();		// idVec3::ToMat3 puts the vector on the forward (X) view axis
	rv.fov_x = 90.0f;
	rv.fov_y = 73.74f;

	cvarSystem->SetCVarInteger( "r_useTemporalAA", 0 );
	cvarSystem->SetCVarInteger( "r_softShadowDebugShader", 0 );
	cvarSystem->SetCVarFloat( "r_rtShadowSoftRadius", cvarSystem->GetCVarFloat( "r_shadowPenumbraSize" ) );

	// Never present: this is a hidden/headless render. GL_BlockingSwapBuffers would block forever on an
	// unmapped surface. The readback's own executeCommandList + mapStagingTexture provides GPU sync (the
	// envprobe bake path relies on exactly this).
	tr.InvalidateSwapBuffers();

	// ORACLE: exact HARD shadows via stencil z-fail (no RT accel structure needed - that is built by the game
	// map load, which minimal init skips). Stencil gives an exact lit/shadowed reference, which is all the
	// FALSE-SHADOW metric needs (hybrid darker than a fully-lit oracle pixel = false shadow). Then HYBRID
	// (soft-shadow volumes + PCSS locator).
	cmdSystem->BufferCommandText( CMD_EXEC_NOW,
								  "r_useRTShadows 0 ; r_useSoftShadowVolumes 0 ; r_shadowMapPCSS 0 ; r_useStencilShadows 1\n" );
	fprintf( stderr, "[selftest] BREAD: rendering ORACLE...\n" ); fflush( stderr );
	std::vector<uint8_t> ref;
	int rw2 = 0, rh2 = 0;
	SelfTestRenderReadback( rw, &rv, ref, rw2, rh2 );
	fprintf( stderr, "[selftest] BREAD: oracle done %dx%d, rendering HYBRID...\n", rw2, rh2 ); fflush( stderr );

	cmdSystem->BufferCommandText( CMD_EXEC_NOW, "r_useRTShadows 0 ; r_useSoftShadowVolumes 1 ; r_shadowMapPCSS 1\n" );
	std::vector<uint8_t> test;
	int tw2 = 0, th2 = 0;
	SelfTestRenderReadback( rw, &rv, test, tw2, th2 );
	fprintf( stderr, "[selftest] BREAD: hybrid done %dx%d\n", tw2, th2 ); fflush( stderr );

	renderSystem->FreeRenderWorld( rw );

	if( ref.empty() || test.empty() || rw2 != tw2 || rh2 != th2 || rw2 <= 0 )
	{
		common->Printf( "[selftest] FAIL: readback mismatch (%dx%d vs %dx%d)\n", rw2, rh2, tw2, th2 );
		return -1;
	}

	const float litThresh = 0.12f, darkenFrac = 0.5f;
	long litN = 0, falseN = 0;
	std::vector<uint8_t> diff( ( size_t )rw2 * rh2 * 3, 0 );
	for( int i = 0; i < rw2 * rh2; i++ )
	{
		float lr = SoftTestLum( &ref[( size_t )i * 4] ), ls = SoftTestLum( &test[( size_t )i * 4] );
		diff[( size_t )i * 3 + 0] = ( uint8_t )( lr * 255.0f );
		diff[( size_t )i * 3 + 1] = ( uint8_t )( ls * 255.0f );
		if( lr < litThresh ) { continue; }
		litN++;
		if( ls < darkenFrac * lr ) { falseN++; diff[( size_t )i * 3 + 2] = 255; }
	}
	double rate = litN ? ( double )falseN / litN : 0.0;
	const bool pass = ( litN > 1000 ) && ( rate < 0.02 );
	idFile* d = fileSystem->OpenFileWrite( "softtest_falseshadow.ppm", "fs_savepath" );
	if( d != NULL )
	{
		d->Printf( "P6\n%d %d\n255\n", rw2, rh2 );
		d->Write( diff.data(), ( int )diff.size() );
		fileSystem->CloseFile( d );
	}
	common->Printf( "[selftest] map=%s lights=%d  false-shadow %ld / %ld lit px = %.2f%%  ->  %s\n",
					mapName, nLights, falseN, litN, rate * 100.0, pass ? "PASS" : "FAIL" );
	return ( int )falseN;
}

// ---- captured-geometry replay ---------------------------------------------------------------------------------
// The harness renders the LIVE loadGame-quick world at the capture viewpoint, but a DYNAMIC caster present at
// capture time (a physics gib, a moved crate) is absent from that world - so its shadow simply cannot be
// reproduced (verified: softcap0019's lights show casters glob=n loc=n at replay though the capture recorded 37).
// To reproduce the EXACT captured frame we rebuild the caster meshes the .softcap embeds (MESHVERTS/MESHIDX, world
// space) into one static model and add it to the render world as a shadow-casting entity, so the normal atlas
// occluder path renders it. Cleared after the A/B renders.
static qhandle_t     s_capturedCasterEntity = -1;
static idRenderModel* s_capturedCasterModel = NULL;

static void R_SoftShadowClearCapturedCasters()
{
	if( s_capturedCasterEntity != -1 && tr.primaryWorld != NULL )
	{
		tr.primaryWorld->FreeEntityDef( s_capturedCasterEntity );
		s_capturedCasterEntity = -1;
	}
	if( s_capturedCasterModel != NULL )
	{
		renderModelManager->FreeModel( s_capturedCasterModel );
		s_capturedCasterModel = NULL;
	}
}

static void R_SoftShadowSpawnCapturedCasters( const char* path )
{
	R_SoftShadowClearCapturedCasters();

	FILE* cf = fopen( path, "rb" );
	if( cf == NULL ) { return; }
	softcapHeader_t hdr;
	if( fread( &hdr, sizeof( hdr ), 1, cf ) != 1 || hdr.magic != SOFTCAP_MAGIC || hdr.numMeshVerts == 0 || hdr.numMeshIdx == 0 )
	{
		fclose( cf );
		common->Printf( "[softtest] no captured caster geometry to replay\n" );
		return;
	}
	// block order after the header: lights, edges, casters, then MESHVERTS (float3), MESHIDX (uint32).
	const long meshVertsOff = ( long )sizeof( hdr )
							  + ( long )hdr.numLights  * ( long )sizeof( softcapLight_t )
							  + ( long )hdr.numEdges   * ( long )sizeof( softcapEdge_t )
							  + ( long )hdr.numCasters * ( long )sizeof( softcapCaster_t );
	std::vector<float>    mv( ( size_t )hdr.numMeshVerts * 3 );
	std::vector<uint32_t> mi( hdr.numMeshIdx );
	if( fseek( cf, meshVertsOff, SEEK_SET ) != 0
		|| fread( mv.data(), sizeof( float ), mv.size(), cf ) != mv.size()
		|| fread( mi.data(), sizeof( uint32_t ), mi.size(), cf ) != mi.size() )
	{
		fclose( cf );
		common->Warning( "testSoftShadowLocator: failed to read captured caster meshes" );
		return;
	}
	fclose( cf );

	srfTriangles_t* tri = R_AllocStaticTriSurf();
	R_AllocStaticTriSurfVerts( tri, ( int )hdr.numMeshVerts );
	R_AllocStaticTriSurfIndexes( tri, ( int )hdr.numMeshIdx );
	tri->numVerts = ( int )hdr.numMeshVerts;
	tri->numIndexes = ( int )hdr.numMeshIdx;
	for( int i = 0; i < tri->numVerts; i++ )
	{
		tri->verts[i].Clear();
		tri->verts[i].xyz.Set( mv[i * 3 + 0], mv[i * 3 + 1], mv[i * 3 + 2] );
	}
	for( int i = 0; i < tri->numIndexes; i++ )
	{
		tri->indexes[i] = ( triIndex_t )mi[i];
	}
	R_BoundTriSurf( tri );

	modelSurface_t surf;
	surf.id = 0;
	surf.shader = declManager->FindMaterial( "textures/common/shadow2" );	// forceshadows + noselfshadow: casts into the atlas but is NOT drawn (the live world stays visible)
	surf.geometry = tri;

	s_capturedCasterModel = renderModelManager->AllocModel();
	s_capturedCasterModel->InitEmpty( "_softShadowCapturedCasters" );
	s_capturedCasterModel->AddSurface( surf );		// model takes ownership of tri
	s_capturedCasterModel->FinishSurfaces( false );

	renderEntity_t re;
	memset( &re, 0, sizeof( re ) );
	re.hModel = s_capturedCasterModel;
	re.axis = mat3_identity;				// MESHVERTS are already world space
	re.origin.Zero();
	re.shaderParms[0] = re.shaderParms[1] = re.shaderParms[2] = re.shaderParms[3] = 1.0f;
	re.noShadow = false;
	s_capturedCasterEntity = tr.primaryWorld->AddEntityDef( &re );
	common->Printf( "[softtest] REPLAY captured casters: %u verts / %u tris -> entity %d\n",
					hdr.numMeshVerts, hdr.numMeshIdx / 3, s_capturedCasterEntity );
}

void R_TestSoftShadowLocator_f( const idCmdArgs& args )
{
	if( tr.primaryWorld == NULL )
	{
		common->Warning( "testSoftShadowLocator: no map loaded" );
		return;
	}
	if( args.Argc() < 2 )
	{
		common->Warning( "usage: testSoftShadowLocator <capture.softcap> | <x y z yaw pitch>  (load its map first - all erebusN are game/erebus1)" );
		return;
	}

	// Camera = the capture's EXACT artifact viewpoint (or a free-cam "x y z yaw pitch", see softShadowGoto). We move
	// the (noclipping) player there via the goto tick and render via the real Draw() path: Draw() applies shadow-cvar
	// changes between configs, whereas a bare RenderScene reuses the interactions cached at map load and renders every
	// config identically. The file/args only supply the camera for the log line; the actual pose is set by the goto.
	idVec3 camOrg;
	idAngles ang;
	if( args.Argc() >= 6 )
	{
		camOrg.Set( atof( args.Argv( 1 ) ), atof( args.Argv( 2 ) ), atof( args.Argv( 3 ) ) );
		ang.Set( atof( args.Argv( 5 ) ), atof( args.Argv( 4 ) ), 0.0f );
		common->Printf( "[softtest] free-cam (%.0f %.0f %.0f) yaw %.0f pitch %.0f\n", camOrg.x, camOrg.y, camOrg.z, ang.yaw, ang.pitch );
	}
	else
	{
		FILE* cf = fopen( args.Argv( 1 ), "rb" );
		softcapHeader_t hdr;
		if( cf == NULL || fread( &hdr, sizeof( hdr ), 1, cf ) != 1 || hdr.magic != SOFTCAP_MAGIC )
		{
			if( cf != NULL ) { fclose( cf ); }
			common->Warning( "testSoftShadowLocator: cannot read capture %s", args.Argv( 1 ) );
			return;
		}
		fclose( cf );
		camOrg.Set( hdr.vieworg[0], hdr.vieworg[1], hdr.vieworg[2] );
		ang = idVec3( hdr.viewaxis[0], hdr.viewaxis[1], hdr.viewaxis[2] ).ToAngles();
		common->Printf( "[softtest] capture camera (%.0f %.0f %.0f) yaw %.0f pitch %.0f from %s\n",
						camOrg.x, camOrg.y, camOrg.z, ang.yaw, ang.pitch, args.Argv( 1 ) );
		// reproduce the captured DYNAMIC casters (absent from the live loadGame-quick world) so their shadows render
		R_SoftShadowSpawnCapturedCasters( args.Argv( 1 ) );
	}

	// pin the full config again right before the A/B renders (in case the test is run WITHOUT softShadowGoto, or a
	// frame in between re-archived something). Verbose so the exact config under test is in the log every time.
	R_SoftShadowPinTestConfig( true );

	static const char* const touched[] =
	{
		"g_stopTime", "r_useStencilShadows", "r_useSoftShadowVolumes", "r_useRTShadows",
		"r_shadowMapPCSS", "r_softShadowDebugShader", "r_useTemporalAA", "r_skipShadows", "r_useShadowAtlas",
	};
	const int nTouched = ( int )( sizeof( touched ) / sizeof( touched[0] ) );
	idStrList prev;
	for( int i = 0; i < nTouched; i++ ) { prev.Append( cvarSystem->GetCVarString( touched[i] ) ); }
	cvarSystem->SetCVarInteger( "r_useTemporalAA", 0 );
	cvarSystem->SetCVarInteger( "r_softShadowDebugShader", 0 );

	// move the player to the capture viewpoint (issue `noclip` yourself once before sweeping many captures, so
	// the camera can sit inside geometry). setviewpos only moves the PLAYER ENTITY - a bare Draw() then re-renders
	// the STALE view built by the last game tick (every capture came out identical: the spawn view). Run several
	// FULL game frames (sim unfrozen) so the player teleports and the game rebuilds the render view AT the capture
	// position; only THEN freeze the sim and do the A/B renders.
	// the VIEWPOINT + cinematic-skip must already be applied by `softShadowGoto <cap>` + a `wait` ahead of this
	// command (it runs through the normal frame loop; doing it here re-entrantly unloads the map). We only render
	// the current view. One Draw settles the latest view, then freeze the sim for the deterministic A/B pair.
	R_RenderOneFrame();
	cvarSystem->SetCVarInteger( "g_stopTime", 1 );

	// ORACLE: exact stencil hard shadows (deterministic; RT is unsuitable - real-time-optimised, temporally
	// noisy). Its lit region is a conservative "should be lit" set, so it never false-accuses a real penumbra.
	cmdSystem->BufferCommandText( CMD_EXEC_NOW, "r_skipShadows 0 ; r_useRTShadows 0 ; r_useStencilShadows 0 ; r_useShadowAtlas 1 ; r_useSoftShadowVolumes 1 ; r_shadowMapPCSS 0\n" );	// DIAG: soft-wedge, locator OFF
	R_RenderOneFrame();
	std::vector<uint8_t> ref;
	int rw = 0, rh = 0;
	ReadImageRGBA8( globalImages->currentRenderHDRImage, ref, rw, rh );
	R_ReadPixelsRGB8( deviceManager->GetDevice(), &backEnd.GetCommonPasses(),
					  globalImages->currentRenderHDRImage->GetTextureHandle(), nvrhi::ResourceStates::ShaderResource, "softtest_oracle.png" );

	// HYBRID: soft-shadow volumes + PCSS locator gating the analytic
	cmdSystem->BufferCommandText( CMD_EXEC_NOW, "r_skipShadows 0 ; r_useStencilShadows 0 ; r_useRTShadows 0 ; r_useShadowAtlas 1 ; r_useSoftShadowVolumes 1 ; r_shadowMapPCSS 1\n" );
	R_RenderOneFrame();
	std::vector<uint8_t> test;
	int tw = 0, th = 0;
	ReadImageRGBA8( globalImages->currentRenderHDRImage, test, tw, th );
	R_ReadPixelsRGB8( deviceManager->GetDevice(), &backEnd.GetCommonPasses(),
					  globalImages->currentRenderHDRImage->GetTextureHandle(), nvrhi::ResourceStates::ShaderResource, "softtest_hybrid.png" );

	// CONFIG + why soft edges did/didn't generate (the frontend tallies caster acceptance/rejection)
	{
		extern int fe_stencilBuilt, fe_softEdgesCollected, fe_rejSilEdges, fe_rejSurfInter, fe_rejNumIdx, fe_rejIdxStale, fe_rejShadowCache;
		int softEdges = 0;
		idRenderWorldLocal* rwl2 = ( idRenderWorldLocal* )tr.primaryWorld;
		for( viewLight_t* vl = tr.viewDef ? tr.viewDef->viewLights : NULL; vl != NULL; vl = vl->next ) { softEdges += vl->softEdgeCount; }
		( void )rwl2;
		common->Printf( "[softtest] cfg soft=%d pcss=%d atlas=%d RT=%d stencil=%d shadowMapping=%d band=%d AAM=%d penumbra=%.1f\n",
						cvarSystem->GetCVarInteger( "r_useSoftShadowVolumes" ), cvarSystem->GetCVarInteger( "r_shadowMapPCSS" ),
						cvarSystem->GetCVarInteger( "r_useShadowAtlas" ), cvarSystem->GetCVarInteger( "r_useRTShadows" ),
						cvarSystem->GetCVarInteger( "r_useStencilShadows" ), cvarSystem->GetCVarInteger( "r_useShadowMapping" ),
						cvarSystem->GetCVarInteger( "r_softShadowBandMask" ), cvarSystem->GetCVarInteger( "r_softShadowAAM" ),
						cvarSystem->GetCVarFloat( "r_shadowPenumbraSize" ) );
		common->Printf( "[softtest] frontend softEdges(view)=%d  softEdgesCollected(cumulative)=%d  stencilBuilt=%d rejSil=%d rejNumIdx=%d\n",
						softEdges, fe_softEdgesCollected, fe_stencilBuilt, fe_rejSilEdges, fe_rejNumIdx );
	}

	// dump the locator's own inputs/outputs (soft+PCSS config): mode 10 = classification (red=umbra/false
	// shadow, green=lit), 11 = receiver depth, 12 = shadow-atlas depth the blocker search samples. These reveal
	// whether the atlas holds real occluders or volume/garbage, and whether the projection is sane.
	{
		struct DbgDump { int mode; const char* file; };
		static const DbgDump dumps[] = { { 10, "softtest_dbg10_locator.png" }, { 11, "softtest_dbg11_recvdepth.png" }, { 12, "softtest_dbg12_atlasdepth.png" }, { 13, "softtest_dbg13_uv.png" }, { 14, "softtest_dbg14_w.png" }, { 15, "softtest_dbg15_face.png" } };
		for( int di = 0; di < 6; di++ )
		{
			cvarSystem->SetCVarInteger( "r_softShadowDebugShader", dumps[di].mode );
			R_RenderOneFrame();
			R_ReadPixelsRGB8( deviceManager->GetDevice(), &backEnd.GetCommonPasses(),
							  globalImages->currentRenderHDRImage->GetTextureHandle(), nvrhi::ResourceStates::ShaderResource, dumps[di].file );
			// numeric probe: mean R (and for locator, red-fraction) over the lit central band, so recvZ (11) vs
			// atlas sample (12) can be compared directly - a big gap is the depth-encoding (plumbing) mismatch.
			std::vector<uint8_t> pb; int pw = 0, ph = 0;
			ReadImageRGBA8( globalImages->currentRenderHDRImage, pb, pw, ph );
			if( pw > 0 && ph > 0 )
			{
				double sumR = 0, sumG = 0; long n = 0;
				for( int y = ph / 4; y < 3 * ph / 4; y++ )
					for( int x = pw / 4; x < 3 * pw / 4; x++ )
					{
						sumR += pb[( ( size_t )y * pw + x ) * 4 + 0]; sumG += pb[( ( size_t )y * pw + x ) * 4 + 1]; n++;
					}
				common->Printf( "[softtest] dbg%d central meanR=%.3f meanG=%.3f (of 255)\n", dumps[di].mode, n ? sumR / n : 0.0, n ? sumG / n : 0.0 );
			}
		}
		cvarSystem->SetCVarInteger( "r_softShadowDebugShader", 0 );
	}

	// dump the soft light's ATLAS TILE (what the blocker search actually samples): near depth = dark. Real
	// occluders show as small dark silhouettes on a far/white field; volume geometry fills the tile dark.
	{
		extern int fe_occludersBuilt;
		common->Printf( "[softtest] occludersBuilt(cumulative)=%d  softDbgAtlasSize=(%d,%d) off0=(%d,%d)\n",
						fe_occludersBuilt, g_softDbgAtlasSize.x, g_softDbgAtlasSize.y, g_softDbgAtlasOff[0].x, g_softDbgAtlasOff[0].y );
		const int AS = cvarSystem->GetCVarInteger( "r_shadowMapAtlasSize" );
		float* atlas = NULL;
		const bool readOK = AS > 0 && R_ReadPixelsR32F( deviceManager->GetDevice(), &backEnd.GetCommonPasses(),
						globalImages->shadowAtlasImage->GetTextureHandle(), nvrhi::ResourceStates::ShaderResource, &atlas, AS, AS ) && atlas != NULL;
		if( !readOK )
		{
			common->Printf( "[softtest] atlas readback FAILED (AS=%d)\n", AS );
		}
		if( readOK )
		{
			extern idVec2i g_softDbgAtlasOff[6];
			extern idVec2i g_softDbgAtlasSize;
			const int sz = g_softDbgAtlasSize.x;
			for( int face = 0; face < 6; face++ )
			{
				const int ox = g_softDbgAtlasOff[face].x, oy = g_softDbgAtlasOff[face].y;
				if( ox < 0 || oy < 0 || sz <= 0 || ox + sz > AS || oy + sz > AS ) { continue; }
				idStr name = va( "softtest_atlastile_f%d.pgm", face );
				idFile* f = fileSystem->OpenFileWrite( name, "fs_savepath" );
				if( f != NULL )
				{
					f->Printf( "P5\n%d %d\n255\n", sz, sz );
					double sum = 0;
					int nnear = 0;
					for( int y = 0; y < sz; y++ )
						for( int x = 0; x < sz; x++ )
						{
							float d = atlas[( size_t )( oy + y ) * AS + ( ox + x )];
							sum += d;
							if( d < 0.99f ) { nnear++; }
							unsigned char c = ( unsigned char )( Max( 0.0f, Min( 1.0f, d ) ) * 255.0f );
							f->Write( &c, 1 );
						}
					fileSystem->CloseFile( f );
					common->Printf( "[softtest] atlas tile f%d (%d,%d,%d): meanDepth=%.4f  nearPx(<0.99)=%d/%d\n", face, ox, oy, sz, sum / ( sz * sz ), nnear, sz * sz );
				}
			}
			R_StaticFree( atlas );
		}
	}

	tr.SwapCommandBuffers( NULL, NULL, NULL, NULL, NULL, NULL );	// present the last frame
	for( int i = 0; i < nTouched; i++ ) { cvarSystem->SetCVarString( touched[i], prev[i].c_str() ); }

	if( ref.empty() || test.empty() || rw != tw || rh != th || rw <= 0 )
	{
		common->Warning( "testSoftShadowLocator: readback failed or size mismatch (ref %dx%d, test %dx%d)", rw, rh, tw, th );
		return;
	}

	// FALSE SHADOW = the oracle leaves the pixel lit but the hybrid darkens it by more than half. Self-masking:
	// unlit/background pixels (low oracle luminance) are excluded, so texture darkness never reads as a shadow.
	const float litThresh = 0.12f;		// oracle luminance above which the pixel is "meaningfully lit"
	const float darkenFrac = 0.5f;		// hybrid < this * oracle => a false shadow
	long litN = 0, falseN = 0, deltaN = 0;
	std::vector<uint8_t> diff( ( size_t )rw * rh * 3, 0 );
	for( int i = 0; i < rw * rh; i++ )
	{
		float lr = SoftTestLum( &ref[( size_t )i * 4] );
		float ls = SoftTestLum( &test[( size_t )i * 4] );
		diff[( size_t )i * 3 + 0] = ( uint8_t )( lr * 255.0f );
		diff[( size_t )i * 3 + 1] = ( uint8_t )( ls * 255.0f );
		if( idMath::Fabs( lr - ls ) > 0.1f ) { deltaN++; }	// oracle vs hybrid differ at all (shadow present in view)
		if( lr < litThresh ) { continue; }
		litN++;
		if( ls < darkenFrac * lr ) { falseN++; diff[( size_t )i * 3 + 2] = 255; }	// mark false-shadow pixels blue
	}
	double rate = litN ? ( double )falseN / litN : 0.0;
	const bool pass = ( litN > 1000 ) && ( rate < 0.02 );

	// write a diff image (R=oracle lum, G=hybrid lum, B=false-shadow mask) for eyeballing a failure
	idFile* d = fileSystem->OpenFileWrite( "softtest_falseshadow.ppm", "fs_savepath" );
	if( d != NULL )
	{
		d->Printf( "P6\n%d %d\n255\n", rw, rh );
		d->Write( diff.data(), ( int )diff.size() );
		fileSystem->CloseFile( d );
	}

	if( litN <= 1000 )
	{
		common->Warning( "testSoftShadowLocator: only %ld lit px - no usable oracle (ray-query hardware? map loaded?)", litN );
	}
	common->Printf( "[softtest] %s : false-shadow %ld / %ld lit = %.2f%%  shadowDelta %ld px  ->  %s\n",
					args.Argv( 1 ), falseN, litN, rate * 100.0, deltaN, pass ? "PASS" : "FAIL" );

	R_SoftShadowClearCapturedCasters();		// remove the replayed captured casters from the render world
}
