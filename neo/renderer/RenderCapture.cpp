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

#include "../tests/SoftShadowGate.h"	// dependency-free defect analyzer, shared with rbdoom3bfg_tests

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
	{ "r_shadowMapPCSSAnalyticContact", "1" },	// the SHIPPED term: exact analytic over the penumbra, PCSS only as edge-less fallback (archived 0 = pure-PCSS acne)
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

// ---------------------------------------------------------------------------------------------------
// HEADLESS CORPUS RENDER. `softShadowShots <cap1> [cap2 ...]` renders each capture and quits, entirely
// from CODE - no bash orchestration, no `+wait`, no `timeout`, no per-command races (which segfaulted).
// It is a state machine driven by the normal frame loop (R_SoftShadowBatchTick, called next to the goto
// tick): per capture, re-use `softShadowGoto` to pin+build the view for a settle window (real frontend
// work, not idle waiting), then a buffered `dumpHDR` writes shot_<name>.png of that settled frame, then
// advance; after the last, `quit`. The map/save load is paid ONCE up front.
namespace
{
idStrList s_batchCaps;
int  s_batchIdx = -1;
int  s_batchState = 0;			// 0 = settling, 1 = captured (advance next tick)
int  s_batchSettle = 0;
const int SOFT_BATCH_SETTLE = 45;	// frames for the teleport to land + the frontend to (re)build soft edges
idStr R_BatchName( const idStr& path )
{
	idStr n = path;
	n.StripPath();
	n.StripFileExtension();
	return n;
}
}

void R_SoftShadowBatchTick()
{
	if( s_batchIdx < 0 )
	{
		return;
	}
	if( s_batchState == 0 )				// settling: let the pinned view + frontend build, then capture
	{
		if( --s_batchSettle > 0 )
		{
			return;
		}
		cmdSystem->BufferCommandText( CMD_EXEC_APPEND, va( "dumpHDR shot_%s\n", R_BatchName( s_batchCaps[s_batchIdx] ).c_str() ) );
		s_batchState = 1;
		return;
	}
	s_batchIdx++;						// captured: advance to the next capture (or quit)
	if( s_batchIdx < s_batchCaps.Num() )
	{
		// reproduce this capture's DYNAMIC casters (the rock etc.) THEN pin the view - both from the command buffer.
		cmdSystem->BufferCommandText( CMD_EXEC_APPEND, va( "softShadowSpawnCasters %s\n", s_batchCaps[s_batchIdx].c_str() ) );
		cmdSystem->BufferCommandText( CMD_EXEC_APPEND, va( "softShadowGoto %s\n", s_batchCaps[s_batchIdx].c_str() ) );
		s_batchSettle = SOFT_BATCH_SETTLE;
		s_batchState = 0;
	}
	else
	{
		common->Printf( "[softbatch] all captures rendered - quitting\n" );
		cmdSystem->BufferCommandText( CMD_EXEC_APPEND, "quit\n" );
		s_batchIdx = -1;
	}
}

void R_SoftShadowShots_f( const idCmdArgs& args )
{
	if( args.Argc() < 2 )
	{
		common->Warning( "usage: softShadowShots <cap1.softcap> [cap2 ...] - renders each headless -> shot_<name>.png, then quits" );
		return;
	}
	s_batchCaps.Clear();
	for( int i = 1; i < args.Argc(); i++ )
	{
		s_batchCaps.Append( idStr( args.Argv( i ) ) );
	}
	s_batchIdx = 0;
	s_batchState = 0;
	s_batchSettle = SOFT_BATCH_SETTLE;
	cmdSystem->BufferCommandText( CMD_EXEC_APPEND, va( "softShadowSpawnCasters %s\n", s_batchCaps[0].c_str() ) );
	cmdSystem->BufferCommandText( CMD_EXEC_APPEND, va( "softShadowGoto %s\n", s_batchCaps[0].c_str() ) );
	common->Printf( "[softbatch] armed %d captures, settle=%d frames each\n", s_batchCaps.Num(), SOFT_BATCH_SETTLE );
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
	rw->RenderScene( rv );
	const emptyCommand_t* cmd = tr.SwapCommandBuffers( NULL, NULL, NULL, NULL, NULL, NULL );
	tr.RenderCommandBuffers( cmd );
	tr.SwapCommandBuffers( NULL, NULL, NULL, NULL, NULL, NULL );
	return ReadImageRGBA8( globalImages->currentRenderHDRImage, out, w, h );
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
	std::vector<uint8_t> ref;
	int rw2 = 0, rh2 = 0;
	SelfTestRenderReadback( rw, &rv, ref, rw2, rh2 );

	cmdSystem->BufferCommandText( CMD_EXEC_NOW, "r_useRTShadows 0 ; r_useSoftShadowVolumes 1 ; r_shadowMapPCSS 1\n" );
	std::vector<uint8_t> test;
	int tw2 = 0, th2 = 0;
	SelfTestRenderReadback( rw, &rv, test, tw2, th2 );

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
static idRenderWorld* s_capturedCasterWorld = NULL;		// the world the entity was added to (primary OR a gate world)

static void R_SoftShadowClearCapturedCasters()
{
	if( s_capturedCasterEntity != -1 && s_capturedCasterWorld != NULL )
	{
		s_capturedCasterWorld->FreeEntityDef( s_capturedCasterEntity );
	}
	s_capturedCasterEntity = -1;
	s_capturedCasterWorld = NULL;
	if( s_capturedCasterModel != NULL )
	{
		renderModelManager->FreeModel( s_capturedCasterModel );
		s_capturedCasterModel = NULL;
	}
}

idCVar r_softShadowReplayCaster( "r_softShadowReplayCaster", "-1", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW,
								 "harness: replay ONLY this caster index from the .softcap (isolates the dynamic crate/gib); -1 = all casters" );

static void R_SoftShadowSpawnCapturedCasters( const char* path, idRenderWorld* world = NULL )
{
	R_SoftShadowClearCapturedCasters();
	if( world == NULL )
	{
		world = tr.primaryWorld;
	}
	if( world == NULL )
	{
		common->Warning( "softShadowSpawnCasters: no render world" );
		return;
	}

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
	const long castersOff = ( long )sizeof( hdr )
							+ ( long )hdr.numLights * ( long )sizeof( softcapLight_t )
							+ ( long )hdr.numEdges  * ( long )sizeof( softcapEdge_t );
	const long meshVertsOff = castersOff + ( long )hdr.numCasters * ( long )sizeof( softcapCaster_t );
	std::vector<softcapCaster_t> casters( hdr.numCasters );
	std::vector<float>    mv( ( size_t )hdr.numMeshVerts * 3 );
	std::vector<uint32_t> mi( hdr.numMeshIdx );
	if( fseek( cf, castersOff, SEEK_SET ) != 0
		|| ( hdr.numCasters > 0 && fread( casters.data(), sizeof( softcapCaster_t ), casters.size(), cf ) != casters.size() )
		|| fseek( cf, meshVertsOff, SEEK_SET ) != 0
		|| fread( mv.data(), sizeof( float ), mv.size(), cf ) != mv.size()
		|| fread( mi.data(), sizeof( uint32_t ), mi.size(), cf ) != mi.size() )
	{
		fclose( cf );
		common->Warning( "testSoftShadowLocator: failed to read captured caster meshes" );
		return;
	}
	fclose( cf );

	// HARNESS ISOLATION: `r_softShadowReplayCaster` picks ONE caster mesh to replay (>= 0), instead of the whole
	// scene's caster soup (which floods the frame - the plate, walls and every gib are all in here). The dynamic
	// object of interest (crate/gib) is one caster; find its index by projecting each caster to the capture view
	// (see peterpan_geometric.py), then replay just it so its shadow is isolated and measurable. -1 = all.
	extern idCVar r_softShadowReplayCaster;
	const int only = r_softShadowReplayCaster.GetInteger();
	uint32_t firstV = 0, numV = hdr.numMeshVerts, firstI = 0, numI = hdr.numMeshIdx;
	if( only >= 0 && only < ( int )hdr.numCasters )
	{
		firstV = casters[only].firstVert;  numV = casters[only].numVerts;
		firstI = casters[only].firstIndex; numI = casters[only].numIndex;
		common->Printf( "[softtest] replaying ONLY caster %d: verts[%u..%u) idx[%u..%u)\n", only, firstV, firstV + numV, firstI, firstI + numI );
	}

	srfTriangles_t* tri = R_AllocStaticTriSurf();
	R_AllocStaticTriSurfVerts( tri, ( int )numV );
	R_AllocStaticTriSurfIndexes( tri, ( int )numI );
	tri->numVerts = ( int )numV;
	tri->numIndexes = ( int )numI;
	for( uint32_t i = 0; i < numV; i++ )
	{
		tri->verts[i].Clear();
		tri->verts[i].xyz.Set( mv[( firstV + i ) * 3 + 0], mv[( firstV + i ) * 3 + 1], mv[( firstV + i ) * 3 + 2] );
	}
	// MESHIDX stores GLOBAL vert indices; rebase to this caster's local vert window.
	for( uint32_t i = 0; i < numI; i++ )
	{
		tri->indexes[i] = ( triIndex_t )( mi[firstI + i] - firstV );
	}
	R_BoundTriSurf( tri );

	modelSurface_t surf;
	surf.id = 0;
	// VISIBLE + shadow-casting replay: a dynamic caster (crate/gib) is absent from `loadGame quick`, so to
	// reconstruct the captured scene headless we must both DRAW the object (to confirm placement and see it) and
	// have it cast (into the atlas for PCSS, and - once traced - for RT). shadow2 was invisible + atlas-only,
	// which hid whether the object was even placed. `_white` draws and casts shadows by default (no noshadows).
	// _white is UNLIT and NON-CASTING (castsShadow=0, receivesLighting=0 - measured), which silently
	// drops the whole replayed model from every light list (ModelHasShadowCastingSurfaces()==false).
	// _default is a normal LIT material that both receives and casts, so the replayed casters shadow
	// and are shadowed like the gameplay originals.
	surf.shader = declManager->FindMaterial( "_default" );
	surf.geometry = tri;

	s_capturedCasterModel = renderModelManager->AllocModel();
	s_capturedCasterModel->InitEmpty( "_softShadowCapturedCasters" );
	s_capturedCasterModel->AddSurface( surf );		// model takes ownership of tri
	s_capturedCasterModel->FinishSurfaces( false );

	// STATIC vertex/index buffers for the replayed mesh: the RT shadow TLAS only accepts
	// static-cache surfaces, so without this the captured caster (the rock) silently vanishes
	// from the ray-traced reference while the analytic path still shadows it.
	{
		nvrhi::CommandListHandle cl = deviceManager->GetDevice()->createCommandList();
		cl->open();
		R_CreateStaticBuffersForTri( *tri, cl );
		cl->close();
		deviceManager->GetDevice()->executeCommandList( cl );
	}

	renderEntity_t re;
	memset( &re, 0, sizeof( re ) );
	re.hModel = s_capturedCasterModel;
	re.axis = mat3_identity;				// MESHVERTS are already world space
	re.origin.Zero();
	re.shaderParms[0] = re.shaderParms[1] = re.shaderParms[2] = re.shaderParms[3] = 1.0f;
	re.noShadow = false;
	s_capturedCasterEntity = world->AddEntityDef( &re );
	s_capturedCasterWorld = world;
	common->Printf( "[softtest] REPLAY captured casters: %u verts / %u tris -> entity %d\n",
					hdr.numMeshVerts, hdr.numMeshIdx / 3, s_capturedCasterEntity );
}

// command wrapper so the headless batch (softShadowShots) can reproduce a capture's DYNAMIC casters (the
// scripted rock/crate/gibs that loadGame-quick does not spawn) from the command buffer - a safe point,
// unlike the mid-frame batch tick. Without this the batch rendered the view but an empty floor.
void R_SoftShadowSpawnCasters_f( const idCmdArgs& args )
{
	if( args.Argc() < 2 )
	{
		common->Warning( "usage: softShadowSpawnCasters <capture.softcap>" );
		return;
	}
	R_SoftShadowSpawnCapturedCasters( args.Argv( 1 ) );
}

// =========================================================== com_softShadowGate: the GPU DEFECT GATE
// Minimal-init (no game/sound/menu) corpus gate: every .softcap is reconstructed into a real render
// world (its map + its captured dynamic casters + its captured lights), rendered through the SHIPPED
// GPU soft-shadow path at the live resolution (>= 1920x1080 enforced), and the frames are analyzed
// in-process by the shared defect counter (tests/SoftShadowGate.h) against three references:
//   - itself, re-rendered (temporal stability: identical state must give identical frames),
//   - itself, from slightly displaced views (continuity: world-space shadows must not pop),
//   - the in-engine ray-traced oracle (agreement: turds/ants/lit-in-umbra/steps/extent).
// EVERY defect instance is counted individually; the gate is green iff the grand total is ZERO.
namespace
{

struct gateCap_t
{
	softcapHeader_t hdr;
	std::vector<softcapLight_t> lights;
	idStr mapName;
	idStr path, name;
};

// header + lights + trailing MAPNAME block only - the caster spawn re-reads its own blocks.
bool GateLoadCap( const char* path, gateCap_t& cap )
{
	FILE* f = fopen( path, "rb" );
	if( f == NULL )
	{
		return false;
	}
	if( fread( &cap.hdr, sizeof( cap.hdr ), 1, f ) != 1 || cap.hdr.magic != SOFTCAP_MAGIC || cap.hdr.version < 4 )
	{
		fclose( f );
		return false;
	}
	const softcapHeader_t& h = cap.hdr;
	cap.lights.resize( h.numLights );
	if( h.numLights > 0 && fread( cap.lights.data(), sizeof( softcapLight_t ), h.numLights, f ) != h.numLights )
	{
		fclose( f );
		return false;
	}
	const long mapOff = ( long )sizeof( h )
						+ ( long )h.numLights   * ( long )sizeof( softcapLight_t )
						+ ( long )h.numEdges    * ( long )sizeof( softcapEdge_t )
						+ ( long )h.numCasters  * ( long )sizeof( softcapCaster_t )
						+ ( long )h.numMeshVerts * 3L * ( long )sizeof( float )
						+ ( long )h.numMeshIdx  * ( long )sizeof( uint32_t )
						+ ( h.hasDepth ? ( long )h.screenW * h.screenH * ( long )sizeof( float ) : 0L )
						+ ( long )h.numReceivers * ( long )sizeof( softcapReceiver_t )
						+ ( long )h.numRecvVerts * 3L * ( long )sizeof( float )
						+ ( long )h.numRecvIdx  * ( long )sizeof( uint32_t );
	const uint32_t mapLen = h.reserved[1];
	if( mapLen > 0 && mapLen < 1024 && fseek( f, mapOff, SEEK_SET ) == 0 )
	{
		std::vector<char> buf( mapLen + 1, 0 );
		if( fread( buf.data(), 1, mapLen, f ) == mapLen )
		{
			cap.mapName = buf.data();
			// captures store the session name ("game/erebus1"); InitFromMap/idMapFile want "maps/game/erebus1"
			if( cap.mapName.Icmpn( "maps/", 5 ) != 0 )
			{
				cap.mapName = "maps/" + cap.mapName;
			}
		}
	}
	fclose( f );
	cap.path = path;
	cap.name = path;
	cap.name.StripPath();
	cap.name.StripFileExtension();
	return true;
}

void GateRenderFrame( idRenderWorld* rw, renderView_t* rv )
{
	rw->RenderScene( rv );
	const emptyCommand_t* cmd = tr.SwapCommandBuffers( NULL, NULL, NULL, NULL, NULL, NULL );
	tr.RenderCommandBuffers( cmd );
	tr.SwapCommandBuffers( NULL, NULL, NULL, NULL, NULL, NULL );
}

// full-precision single-channel readback into the analyzer's image type (R = the shadow term)
bool GateReadR32F( idImage* img, swgate::GateImg& out )
{
	if( img == NULL || img->GetTextureHandle() == NULL )
	{
		return false;
	}
	const int w = img->GetUploadWidth(), h = img->GetUploadHeight();
	float* pic = NULL;
	if( !R_ReadPixelsR32F( deviceManager->GetDevice(), &backEnd.GetCommonPasses(), img->GetTextureHandle(),
						   nvrhi::ResourceStates::ShaderResource, &pic, w, h ) || pic == NULL )
	{
		return false;
	}
	out.W = w;
	out.H = h;
	out.t.assign( pic, pic + ( size_t )w * h );
	R_StaticFree( pic );
	return true;
}

// The per-light slice of idRenderWorldLocal::GenerateAllInteractions: create the STATIC interactions
// (light tris + static shadow-volume indexes) for a JUST-ADDED light against every entity in its areas.
// Runtime-added lights otherwise only get lazy dynamic interactions with numShadowIndexes==0, and the
// frontend's RT caster branch keys on numShadowIndexes>0 - so without this the RT reference sees no
// casters for the probe light (measured: mask never dispatched, all-dark).
void GateCreateStaticInteractionsForLight( idRenderWorld* world, qhandle_t lightHandle )
{
	idRenderWorldLocal* rwl = static_cast<idRenderWorldLocal*>( world );
	if( lightHandle < 0 || lightHandle >= rwl->lightDefs.Num() || rwl->lightDefs[lightHandle] == NULL )
	{
		return;
	}
	idRenderLightLocal* ldef = rwl->lightDefs[lightHandle];
	tr.viewDef = NULL;		// no view-specific optimizations, same as GenerateAllInteractions
	tr.commandList->open();
	int made = 0, seen = 0;
	bool sawSpawned = false;
	for( areaReference_t* lref = ldef->references; lref != NULL; lref = lref->ownerNext )
	{
		portalArea_t* area = lref->area;
		for( areaReference_t* eref = area->entityRefs.areaNext; eref != &area->entityRefs; eref = eref->areaNext )
		{
			idRenderEntityLocal* edef = eref->entity;
			seen++;
			if( edef->parms.hModel != NULL && idStr::Icmp( edef->parms.hModel->Name(), "_softShadowCapturedCasters" ) == 0 )
			{
				// leave the replayed caster soup DYNAMIC: its non-manifold triangles have no silEdges and
				// _white makes no static light tris, so a forced static interaction resolves to EMPTY -
				// which the frontend treats as "statically proven no interaction" and drops the entity
				// (and its shadows) from every list. The lazy dynamic path handles it, as in-game.
				sawSpawned = true;
				continue;
			}
			idInteraction* inter;
			for( inter = edef->firstInteraction; inter != NULL; inter = inter->entityNext )
			{
				if( inter->lightDef == ldef )
				{
					break;
				}
			}
			if( inter != NULL )
			{
				continue;
			}
			inter = idInteraction::AllocAndLink( edef, ldef );
			inter->CreateStaticInteraction( tr.commandList );
			made++;
		}
	}
	tr.commandList->close();
	deviceManager->GetDevice()->executeCommandList( tr.commandList );
	if( cvarSystem->GetCVarBool( "r_rtAccelDebug" ) )
	{
		common->Printf( "[softgate] light interactions: %d entities in light areas, %d created, spawned-casters %s\n",
						seen, made, sawSpawned ? "PRESENT" : "ABSENT" );
	}
}

// float64 ground-truth visibility at a world point vs the light's RECORD triangles (the exact caster
// set the shader consumed, retained by the capture hook): 16 Hammersley disk samples, double-precision
// Moller-Trumbore. This is the ARBITER for reference-vs-analytic disagreements - the RT reference has a
// world-units ray bias that blinds it to contact shadows in seams/cracks, which the exact trace sees.
float GateTruthVisibility( const idVec3& P, const idVec3& L, float diskR )
{
	// disk basis
	idVec3 ld = L - P;
	double dist = ld.Length();
	if( dist < 1e-3 )
	{
		return -1.0f;
	}
	idVec3 lz = ld * ( float )( 1.0 / dist );
	idVec3 lx = ( idMath::Fabs( lz.x ) < 0.9f ) ? idVec3( 1, 0, 0 ).Cross( lz ) : idVec3( 0, 1, 0 ).Cross( lz );
	lx.Normalize();
	idVec3 ly = lz.Cross( lx );
	int blocked = 0;
	const int NS = 16;
	for( int s = 0; s < NS; s++ )
	{
		// Hammersley radical-inverse angle + equal-area radius (matches the test oracles)
		uint32_t bits = ( uint32_t )s;
		bits = ( bits << 16 ) | ( bits >> 16 );
		bits = ( ( bits & 0x55555555u ) << 1 ) | ( ( bits & 0xAAAAAAAAu ) >> 1 );
		bits = ( ( bits & 0x33333333u ) << 2 ) | ( ( bits & 0xCCCCCCCCu ) >> 2 );
		bits = ( ( bits & 0x0F0F0F0Fu ) << 4 ) | ( ( bits & 0xF0F0F0F0u ) >> 4 );
		bits = ( ( bits & 0x00FF00FFu ) << 8 ) | ( ( bits & 0xFF00FF00u ) >> 8 );
		double ri = ( double )bits * 2.3283064365386963e-10;
		double r = sqrt( ( s + 0.5 ) / NS ) * diskR;
		double th = ri * 6.283185307179586;
		idVec3 tgt = L + lx * ( float )( r * cos( th ) ) + ly * ( float )( r * sin( th ) );
		double Pd[3] = { P.x, P.y, P.z };
		double Dd[3] = { tgt.x - P.x, tgt.y - P.y, tgt.z - P.z };
		bool hit = false;
		// STREAM V2 (softcap v5): each light's captured blob range is its pure-tri float4 stream
		// (3 float4 per triangle, zero-padded to an even float4 count so it stores as pair records).
		// The pad breaks 3-alignment ACROSS lights, so iterate per light range: triangle tri of a
		// range starting at record 'first' lives at float4s first*2 + 3*tri .. +2.
		auto F4 = []( size_t j ) -> const float*
		{
			return ( j & 1 ) ? s_edges[j >> 1].e1 : s_edges[j >> 1].e0;
		};
		for( const auto& er : s_edgeRange )
		{
			if( hit )
			{
				break;
			}
			const size_t base4 = ( size_t )er.second.first * 2;
			const size_t nTris = ( ( size_t )er.second.second * 2 ) / 3;	// floor() drops the zero pad
		for( size_t tri = 0; tri < nTris && !hit; tri++ )
		{
			const float* A0 = F4( base4 + tri * 3 + 0 );
			const float* A1 = F4( base4 + tri * 3 + 1 );
			const float* A2 = F4( base4 + tri * 3 + 2 );
			double a[3] = { A0[0], A0[1], A0[2] };
			double b[3] = { A1[0], A1[1], A1[2] };
			double c[3] = { A2[0], A2[1], A2[2] };
			double e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
			double e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
			double pv[3] = { Dd[1] * e2[2] - Dd[2] * e2[1], Dd[2] * e2[0] - Dd[0] * e2[2], Dd[0] * e2[1] - Dd[1] * e2[0] };
			double det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2];
			if( fabs( det ) < 1e-14 )
			{
				continue;
			}
			double inv = 1.0 / det;
			double tv[3] = { Pd[0] - a[0], Pd[1] - a[1], Pd[2] - a[2] };
			double u = ( tv[0] * pv[0] + tv[1] * pv[1] + tv[2] * pv[2] ) * inv;
			if( u < -1e-9 || u > 1.0 + 1e-9 )
			{
				continue;
			}
			double q[3] = { tv[1] * e1[2] - tv[2] * e1[1], tv[2] * e1[0] - tv[0] * e1[2], tv[0] * e1[1] - tv[1] * e1[0] };
			double v = ( Dd[0] * q[0] + Dd[1] * q[1] + Dd[2] * q[2] ) * inv;
			if( v < -1e-9 || u + v > 1.0 + 1e-9 )
			{
				continue;
			}
			double t = ( e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2] ) * inv;
			// near-clip MUST match the shipped shader's contact-shadow bias (softwedge_coverage.inc.hlsl
			// 'tt > 1e-4f && tt <= 1.0f'): the shader suppresses occluders within ~1e-4 of the ray length
			// of the receiver (anti-acne - a 1e-6 clip mints self-shadow acne on every near-contact
			// surface). An arbiter with a 100x tighter clip sees "umbra" the shader intentionally (and
			// correctly) does not cast, minting phantom LIT_IN_UMBRA at sub-0.05-unit near-contacts. Trace
			// the SAME contact model the shader ships so the truth judges the shader's real output.
			if( t > 1e-4 && t <= 1.0 )
			{
				hit = true;
			}
		}
		}
		if( hit )
		{
			blocked++;
		}
	}
	return 1.0f - ( float )blocked / NS;
}

void GateSetup( const gateCap_t& cap, const softcapLight_t& light )
{
	// pinned baseline first, then the per-probe overrides on top of it
	R_SoftShadowPinTestConfig( false );
	cvarSystem->SetCVarInteger( "r_skipAmbient", 1 );		// interaction term only: no emissive/ambient pollution
	cvarSystem->SetCVarFloat( "r_shadowPenumbraSize", light.penumbraSize );
	cvarSystem->SetCVarFloat( "r_rtShadowSoftRadius", light.penumbraSize );
}

} // namespace

// Iterates the corpus, prints one defect line per capture x light and the grand total; returns the
// total defect count (the process exit code, clamped by the caller).
int R_SoftShadowGate( const char* arg )
{
	using namespace swgate;

	globalImages->LoadDeferredImages();
	tr.InvalidateSwapBuffers();		// headless: never present (blocking swap would hang on a hidden surface)

	// ---- corpus --------------------------------------------------------------------------------
	extern int Sys_ListFiles( const char* directory, const char* extension, idStrList& list );
	idStr dir = ( arg == NULL || arg[0] == '\0' || idStr::Icmp( arg, "corpus" ) == 0 ) ? "../tests/data" : arg;
	idStrList files;
	Sys_ListFiles( dir, ".softcap", files );
	if( files.Num() > 1 )
	{
		std::sort( &files[0], &files[0] + files.Num(), []( const idStr& a, const idStr& b )
		{
			return idStr::Icmp( a, b ) < 0;
		} );
	}
	common->Printf( "[softgate] corpus: %s  (%d captures)\n", dir.c_str(), files.Num() );

	// every shadow-relevant cvar the gate touches is restored afterwards so an archived config is never polluted
	static const char* const touched[] =
	{
		"r_useRTShadows", "r_useStencilShadows", "r_useSoftShadowVolumes", "r_useShadowMapping", "r_useShadowAtlas",
		"r_shadowMapPCSS", "r_shadowMapPCSSScale", "r_softShadowAAM", "r_softShadowBandMask", "r_softShadowStencilOnly",
		"r_softShadowEmergentUmbra", "r_softShadowContinuous", "r_useTemporalAA", "r_softShadowDebugShader",
		"r_skipAmbient", "r_skipShadows", "r_shadowPenumbraSize", "r_rtShadowSoftRadius", "r_rtShadowRays",
		"r_rtShadowDenoise", "r_rtShadowAnalyticPenumbra", "r_rtShadowBias",
	};
	const int nTouched = ( int )( sizeof( touched ) / sizeof( touched[0] ) );
	idStrList prev;
	for( int i = 0; i < nTouched; i++ )
	{
		prev.Append( cvarSystem->GetCVarString( touched[i] ) );
	}

	GateCfg cfg;
	std::vector<GateDefect> all;
	int capsRun = 0, lightsRun = 0;

	// LAUNCH-time shadow config, restored for the BENCH renders: the probes pin their own baseline, but
	// the bench must honour +set overrides so shadow methods can be A/B-timed from the command line.
	const int launchSoft    = cvarSystem->GetCVarInteger( "r_useSoftShadowVolumes" );
	const int launchPCSS    = cvarSystem->GetCVarInteger( "r_shadowMapPCSS" );
	const int launchStencil = cvarSystem->GetCVarInteger( "r_useStencilShadows" );
	const int launchMapping = cvarSystem->GetCVarInteger( "r_useShadowMapping" );
	const int launchContact = cvarSystem->GetCVarInteger( "r_shadowMapPCSSAnalyticContact" );
	// The probes set r_shadowPenumbraSize to EACH capture light's stored radius (GateSetup) and never
	// restore it, so the bench inherited whichever light was probed LAST - a different, arbitrary disk
	// radius per capture. That taints every A/B: the coverage cost scales with the disk, and the
	// subdivision threshold is a multiple of it. Bench with the LAUNCH radius, like the shipped game.
	const float launchPenumbra = cvarSystem->GetCVarFloat( "r_shadowPenumbraSize" );

	// map world cache: consecutive captures share the map, load it once
	idStr loadedMap;
	idRenderWorld* rw = NULL;
	std::vector<renderLight_t> mapLights;
	std::vector<renderEntity_t> mapModels;		// static model entities (func_static etc.) - the live game's casters

	for( int fi = 0; fi < files.Num(); fi++ )
	{
		gateCap_t cap;
		idStr full = dir + "/" + files[fi];
		if( !GateLoadCap( full.c_str(), cap ) )
		{
			common->Printf( "[softgate] %s: UNREADABLE capture -> SETUP defect\n", files[fi].c_str() );
			GateDefect d;
			d.kind = GATE_SETUP;
			all.push_back( d );
			continue;
		}
		if( cap.mapName.IsEmpty() )
		{
			common->Printf( "[softgate] %s: capture names no map -> SETUP defect\n", cap.name.c_str() );
			GateDefect d;
			d.kind = GATE_SETUP;
			all.push_back( d );
			continue;
		}

		// ---- world ----------------------------------------------------------------------------
		if( rw == NULL || loadedMap != cap.mapName )
		{
			R_SoftShadowClearCapturedCasters();
			if( rw != NULL )
			{
				renderSystem->FreeRenderWorld( rw );
				rw = NULL;
			}
			mapLights.clear();
			mapModels.clear();
			// complete the world the way a REAL map load does (Common_load.cpp order):
			//  - Begin/EndLevelLoad pins every loaded model into the STATIC vertex cache; the RT
			//    shadow TLAS only accepts static-cache surfaces, so without this the reference is
			//    an empty TLAS (all-dark mask) and the gate measures nothing;
			//  - GenerateAllInteractions allocates interactionTable, which the frontend's
			//    R_AddSingleLight reads UNGUARDED once any entity exists (the minimal-init
			//    self-test survived without it purely because it never added an entity).
			renderSystem->BeginLevelLoad();
			idRenderWorld* nw = renderSystem->AllocRenderWorld();
			const bool mapOk = nw->InitFromMap( cap.mapName.c_str() );
			renderSystem->EndLevelLoad();
			if( !mapOk )
			{
				renderSystem->FreeRenderWorld( nw );
				common->Printf( "[softgate] %s: InitFromMap(%s) FAILED -> SETUP defect\n", cap.name.c_str(), cap.mapName.c_str() );
				GateDefect d;
				d.kind = GATE_SETUP;
				all.push_back( d );
				continue;
			}
			rw = nw;
			loadedMap = cap.mapName;
			rw->GenerateAllInteractions();
			idMapFile mapFile;
			if( mapFile.Parse( cap.mapName.c_str() ) )
			{
				for( int e = 0; e < mapFile.GetNumEntities(); e++ )
				{
					idMapEntity* ent = mapFile.GetEntity( e );
					if( idStr::Icmp( ent->epairs.GetString( "classname" ), "light" ) == 0 )
					{
						renderLight_t rl;
						SelfTestParseLight( &ent->epairs, &rl );
						mapLights.push_back( rl );
						continue;
					}
					// STATIC MODEL ENTITIES (func_static, mover machinery, ...) are the live game's
					// casters and roughly DOUBLE the soft-record stream vs the bare worldspawn
					// (measured erebus1 softcap0061: 114k records in-game vs 56k without them - the
					// gate bench under-read the shipped soft cost ~3x). Add every entity whose
					// spawnargs resolve to a loadable non-animated model; the bench places them so
					// its frame carries the live caster density. Animated md5 meshes are skipped
					// (their live pose is gameplay state the gate cannot know).
					const char* mdl = ent->epairs.GetString( "model" );
					if( mdl == NULL || mdl[0] == '\0' || idStr::Icmp( ent->epairs.GetString( "classname" ), "worldspawn" ) == 0 )
					{
						continue;
					}
					if( idStr( mdl ).Find( ".md5mesh", false ) >= 0 || ent->epairs.GetBool( "hide" ) || ent->epairs.GetBool( "noshadows" ) )
					{
						continue;
					}
					// minimal spawn-arg parse: gameEdit->ParseSpawnArgsToRenderEntity needs the
					// GAME-registered modelDef decl type, which the gate's minimal init never
					// registers (Sys_Error "bad type"). Static casters need only model + placement.
					renderEntity_t re;
					memset( &re, 0, sizeof( re ) );
					re.hModel = renderModelManager->FindModel( mdl );
					if( re.hModel == NULL || re.hModel->IsDefaultModel() )
					{
						continue;
					}
					re.bounds = re.hModel->Bounds( NULL );
					ent->epairs.GetVector( "origin", "0 0 0", re.origin );
					if( !ent->epairs.GetMatrix( "rotation", "1 0 0 0 1 0 0 0 1", re.axis ) )
					{
						float angle = ent->epairs.GetFloat( "angle" );
						if( angle != 0.0f )
						{
							re.axis = idAngles( 0.0f, angle, 0.0f ).ToMat3();
						}
						else
						{
							re.axis.Identity();
						}
					}
					re.shaderParms[0] = re.shaderParms[1] = re.shaderParms[2] = re.shaderParms[3] = 1.0f;
					mapModels.push_back( re );
				}
			}
			common->Printf( "[softgate] loaded %s (%d map lights, %d model entities)\n",
							cap.mapName.c_str(), ( int )mapLights.size(), ( int )mapModels.size() );
		}

		// NOTE: the captured DYNAMIC casters (the scripted rock etc.) are deliberately NOT replayed here.
		// Duplicating gameplay-state geometry over a fresh-loaded map makes the probe scene inconsistent
		// (the soup model paints unlit checkerboard over real receivers, and its RT-vs-analytic plumbing
		// asymmetries mint defects that are capture-fidelity problems, not renderer problems). The gate
		// tests the RENDERER on a self-consistent scene: the live map + its real lights. In-game replay
		// via `softShadowSpawnCasters` still exists for scene-reconstruction work.

		// ---- camera (straight from the capture; same map => same world frame) ------------------
		renderView_t rv;
		memset( &rv, 0, sizeof( rv ) );
		rv.vieworg.Set( cap.hdr.vieworg[0], cap.hdr.vieworg[1], cap.hdr.vieworg[2] );
		rv.viewaxis[0].Set( cap.hdr.viewaxis[0], cap.hdr.viewaxis[1], cap.hdr.viewaxis[2] );
		rv.viewaxis[1].Set( cap.hdr.viewaxis[3], cap.hdr.viewaxis[4], cap.hdr.viewaxis[5] );
		rv.viewaxis[2].Set( cap.hdr.viewaxis[6], cap.hdr.viewaxis[7], cap.hdr.viewaxis[8] );
		rv.fov_x = cap.hdr.fovx;
		rv.fov_y = cap.hdr.fovy;

		capsRun++;

		// ---- rank the captured lights by soft-edge count and probe EVERY one that draws. The
		// single-dominant-light shortcut was a measured blind spot: the erebus1_14 halo the user saw
		// in-game lived under a secondary light and the gate passed. Lights that draw <1000 px at
		// this camera are skipped silently (a matched light can sit behind a closed door); only if
		// NONE draws is it a SETUP defect.
		std::vector<int> probeLights;
		// bench-only mode: skip every per-light probe (they cost ~40-60 s per capture) and go straight
		// to the timed full frames - the fast loop for perf-config sweeps. NOT a correctness verdict:
		// defect counting needs the probes, so PASS from a bench-only run means nothing.
		extern idCVar com_softShadowGateBenchOnly;
		if( !com_softShadowGateBenchOnly.GetBool() )
		for( int li = 0; li < ( int )cap.lights.size(); li++ )
		{
			if( cap.lights[li].edgeCount > 0 )
			{
				probeLights.push_back( li );
			}
		}
		std::sort( probeLights.begin(), probeLights.end(), [&]( int a, int b )
		{
			return cap.lights[a].edgeCount > cap.lights[b].edgeCount;
		} );
		if( probeLights.empty() && !com_softShadowGateBenchOnly.GetBool() )
		{
			common->Printf( "[softgate] %s: no light with soft edges -> SETUP defect (degenerate capture)\n", cap.name.c_str() );
			GateDefect d;
			d.kind = GATE_SETUP;
			all.push_back( d );
		}
		bool capProbed = false;

		// ---- per light (every drawing light is probed) -----------------------------------------
		for( int pi = 0; pi < ( int )probeLights.size(); pi++ )
		{
			const int li = probeLights[pi];
			const softcapLight_t& cl = cap.lights[li];
			idVec3 clOrg( cl.origin[0], cl.origin[1], cl.origin[2] );

			// match the real .map light whose global origin is the captured one (globalLightOrigin
			// includes light_center, so try both origin and origin+center)
			int best = -1;
			float bestD = 16.0f * 16.0f;
			for( int m = 0; m < ( int )mapLights.size(); m++ )
			{
				float d0 = ( mapLights[m].origin - clOrg ).LengthSqr();
				float d1 = ( mapLights[m].origin + mapLights[m].lightCenter - clOrg ).LengthSqr();
				float d = Min( d0, d1 );
				if( d < bestD )
				{
					bestD = d;
					best = m;
				}
			}
			renderLight_t rl;
			if( best >= 0 )
			{
				rl = mapLights[best];
			}
			else
			{
				// purely dynamic light (no .map source): synthesize a point light at the captured
				// origin. The probes are self-consistent (analytic and RT see the same light), so
				// this still gates the renderer - just log the approximation loudly.
				memset( &rl, 0, sizeof( rl ) );
				rl.pointLight = true;
				rl.origin = clOrg;
				rl.axis = mat3_identity;
				rl.lightRadius.Set( 300, 300, 300 );
				rl.shaderParms[SHADERPARM_RED] = rl.shaderParms[SHADERPARM_GREEN] = rl.shaderParms[SHADERPARM_BLUE] = 1.0f;
				rl.shaderParms[SHADERPARM_TIMESCALE] = 1.0f;
				rl.shader = declManager->FindMaterial( "lights/squarelight1", false );
				common->Printf( "[softgate] %s L%d: no .map light at (%.0f %.0f %.0f) - SYNTHESIZED point light\n",
								cap.name.c_str(), li, clOrg.x, clOrg.y, clOrg.z );
			}

			qhandle_t lh = rw->AddLightDef( &rl );
			GateCreateStaticInteractionsForLight( rw, lh );
			GateSetup( cap, cl );
			lightsRun++;

			std::vector<GateDefect> defects;
			swgate::GateImg mask1, rt, anaA, anaB, depthA;

			// R0 - interaction-coverage mask: shadows skipped, term==1 exactly where this light's
			// interaction draws. Everything outside is excluded from every probe.
			cvarSystem->SetCVarInteger( "r_skipShadows", 1 );
			cvarSystem->SetCVarInteger( "r_useRTShadows", 0 );
			cvarSystem->SetCVarInteger( "r_useSoftShadowVolumes", 1 );
			cvarSystem->SetCVarInteger( "r_softShadowDebugShader", 8 );
			GateRenderFrame( rw, &rv );
			GateReadR32F( globalImages->currentRenderHDRImage, mask1 );

			// R1 - the ray-traced reference (converged, no denoise); the mask image IS the term.
			// r_useShadowMapping 1 is LOAD-BEARING: with RT on, the frontend's soft/stencil branch is
			// skipped and only the shadow-map occluder path still fills vLight->globalShadows - which
			// the RT dispatch gate (R_LightUsesRTShadows) requires as its "light has casters" signal.
			// Without it the trace never dispatches and the mask reads all-dark (measured).
			cvarSystem->SetCVarInteger( "r_skipShadows", 0 );
			cvarSystem->SetCVarInteger( "r_useSoftShadowVolumes", 0 );
			cvarSystem->SetCVarInteger( "r_useShadowMapping", 1 );
			cvarSystem->SetCVarInteger( "r_softShadowDebugShader", 0 );
			cvarSystem->SetCVarInteger( "r_useRTShadows", 1 );
			cvarSystem->SetCVarInteger( "r_rtShadowRays", 512 );
			cvarSystem->SetCVarInteger( "r_rtShadowDenoise", 0 );
			cvarSystem->SetCVarInteger( "r_rtShadowAnalyticPenumbra", 0 );
			// reference ray bias stays at the mature default (1.5, slope-scaled): tightening it to see
			// sub-unit contact shadows just trades seam blindness for the reference's own reconstruction
			// acne (measured: LIT_IN_UMBRA 15 -> 206 at 0.25). The analyzer instead EXCLUDES agreement
			// defects at depth creases, where the reference is structurally untrustworthy either way.
			GateRenderFrame( rw, &rv );
			GateReadR32F( globalImages->rtShadowMaskImage, rt );

			// diagnostic mode (r_rtAccelDebug): dump what each reference render actually produced,
			// so a degenerate probe is inspectable as an image instead of argued about from counts
			extern idCVar r_rtAccelDebug;
			if( r_rtAccelDebug.GetBool() )
			{
				R_ReadPixelsRGB8( deviceManager->GetDevice(), &backEnd.GetCommonPasses(), globalImages->currentRenderHDRImage->GetTextureHandle(),
								  nvrhi::ResourceStates::ShaderResource, va( "softgate_dbg_%s_L%d_rtframe.png", cap.name.c_str(), li ) );
				R_ReadPixelsRGB8( deviceManager->GetDevice(), &backEnd.GetCommonPasses(), globalImages->rtShadowMaskImage->GetTextureHandle(),
								  nvrhi::ResourceStates::ShaderResource, va( "softgate_dbg_%s_L%d_rtmask.png", cap.name.c_str(), li ) );
			}

			// R2/R3 - the SHIPPED analytic term, twice back to back (temporal probe)
			cvarSystem->SetCVarInteger( "r_useRTShadows", 0 );
			cvarSystem->SetCVarInteger( "r_useShadowMapping", 0 );		// back to the pinned soft-path baseline
			cvarSystem->SetCVarInteger( "r_useSoftShadowVolumes", 1 );
			cvarSystem->SetCVarInteger( "r_softShadowDebugShader", 8 );
			// ALWAYS retain this render's edge records (the exact caster triangles the shader consumed)
			// via the capture hook - the defect arbiter float64-traces against them. Diagnostic mode
			// additionally writes the full .softcap for offline interrogation.
			ResetAccumulators();
			s_armed = true;
			GateRenderFrame( rw, &rv );
			if( r_rtAccelDebug.GetBool() )
			{
				R_CaptureBackendFinish();
			}
			s_armed = false;
			GateReadR32F( globalImages->currentRenderHDRImage, anaA );
			// bit-exactness instrument: an FNV hash of the raw analytic term, printed per light, lets
			// two gate runs under different perf configs (tile binning on/off, wave jump, ...) prove
			// "no pixel changed" across processes - the probes' tolerances can't see sub-threshold
			// differences, a hash can.
			{
				uint32_t swHashSum = 2166136261u;
				for( size_t hi = 0; hi < anaA.t.size(); hi++ )
				{
					uint32_t b;
					memcpy( &b, &anaA.t[hi], 4 );
					swHashSum = ( swHashSum ^ b ) * 16777619u;
				}
				common->Printf( "[softgate] %s L%d anaTerm hash %08x\n", cap.name.c_str(), li, swHashSum );
			}
			// diagnostic mode: ALWAYS dump the analytic term as a PPM (not just on defects) so the
			// LOOK of the term - banding, grain, plateaus - is inspectable offline. The probes only
			// count classified defects; "gate green but visually banded" is exactly the blind spot.
			{
				extern idCVar r_rtAccelDebug;
				if( r_rtAccelDebug.GetBool() )
				{
					std::vector<uint8_t> allValid( ( size_t )anaA.W * anaA.H, 1 );
					idStr ppm = va( "softgate_term_%s_L%d.ppm", cap.name.c_str(), li );
					GateWritePPM( ppm.c_str(), anaA, allValid, std::vector<uint8_t>() );
				}
			}
			GateReadR32F( globalImages->currentDepthImage, depthA );
			GateRenderFrame( rw, &rv );
			GateReadR32F( globalImages->currentRenderHDRImage, anaB );
			if( r_rtAccelDebug.GetBool() )
			{
				R_ReadPixelsRGB8( deviceManager->GetDevice(), &backEnd.GetCommonPasses(), globalImages->currentRenderHDRImage->GetTextureHandle(),
								  nvrhi::ResourceStates::ShaderResource, va( "softgate_dbg_%s_L%d_anaterm.png", cap.name.c_str(), li ) );
				cvarSystem->SetCVarInteger( "r_softShadowDebugShader", 0 );
				GateRenderFrame( rw, &rv );
				R_ReadPixelsRGB8( deviceManager->GetDevice(), &backEnd.GetCommonPasses(), globalImages->currentRenderHDRImage->GetTextureHandle(),
								  nvrhi::ResourceStates::ShaderResource, va( "softgate_dbg_%s_L%d_anaframe.png", cap.name.c_str(), li ) );
				cvarSystem->SetCVarInteger( "r_softShadowDebugShader", 8 );
			}

			if( !mask1.Valid() || !rt.Valid() || !anaA.Valid() || !anaB.Valid() || !depthA.Valid()
					|| rt.W != anaA.W || rt.H != anaA.H )
			{
				common->Printf( "[softgate] %s L%d: readback FAILED -> SETUP defect\n", cap.name.c_str(), li );
				GateDefect d;
				d.kind = GATE_SETUP;
				defects.push_back( d );
				all.insert( all.end(), defects.begin(), defects.end() );
				rw->FreeLightDef( lh );
				continue;
			}
			const int W = anaA.W, H = anaA.H;
			if( W < 1920 || H < 1080 )
			{
				common->Printf( "[softgate] FATAL: render %dx%d is below 1920x1080 - defects are invisible at this size. "
								"Launch with +set r_windowWidth 1920 +set r_windowHeight 1080.\n", W, H );
				GateDefect d;
				d.kind = GATE_SETUP;
				all.push_back( d );
				rw->FreeLightDef( lh );
				break;
			}

			std::vector<uint8_t> valid( ( size_t )W * H, 0 );
			long validN = 0;
			for( size_t i = 0; i < valid.size(); i++ )
			{
				if( std::fabs( mask1.t[i] - 1.0f ) <= 1e-3f )
				{
					valid[i] = 1;
					validN++;
				}
			}
			if( validN < 1000 )
			{
				// the matched map light doesn't reach this camera (closed door, tiny scissor) -
				// not a defect on its own; only if NO light draws does the capture flag SETUP below
				common->Printf( "[softgate] %s L%d: light drew only %ld px - skipped\n",
								cap.name.c_str(), li, validN );
				rw->FreeLightDef( lh );
				if( pi + 1 >= ( int )probeLights.size() && !capProbed )
				{
					common->Printf( "[softgate] %s: NO candidate light draws at this camera -> SETUP defect\n", cap.name.c_str() );
					GateDefect d;
					d.kind = GATE_SETUP;
					all.push_back( d );
				}
				continue;
			}
			capProbed = true;
			{
				// a starved RT reference (empty TLAS -> fully-lit mask) must never silently pass the gate
				long anaSh = 0, rtSh = 0;
				for( size_t i = 0; i < valid.size(); i++ )
				{
					if( !valid[i] )
					{
						continue;
					}
					if( anaA.t[i] < 0.5f )
					{
						anaSh++;
					}
					if( rt.t[i] < 0.5f )
					{
						rtSh++;
					}
				}
				// a dead reference must never pass OR flood the gate: all-lit (empty TLAS fell back
				// to lit) or all-dark (mask never written, cleared to 0) while the analytic term
				// disagrees wholesale is an oracle failure, not 2000 renderer defects.
				const bool rtAllLit  = ( rtSh * 1000 < validN && anaSh * 20 > validN );
				const bool rtAllDark = ( ( validN - rtSh ) * 1000 < validN && anaSh * 2 < validN );
				if( rtAllLit || rtAllDark )
				{
					common->Printf( "[softgate] %s L%d: RT reference is degenerate (%s: rtShadow=%ld ana=%ld of %ld px) "
									"-> SETUP defect, agreement probes skipped\n", cap.name.c_str(), li,
									rtAllLit ? "all-lit" : "all-dark", rtSh, anaSh, validN );
					GateDefect d;
					d.kind = GATE_SETUP;
					defects.push_back( d );
				}

				// ---- probes -------------------------------------------------------------------
				GateTemporal( anaA, anaB, valid, cfg, defects );
				std::vector<uint8_t> defectPx( ( size_t )W * H, 0 );
				std::vector<uint8_t> crease = GateCreaseMask( depthA, cfg.guard );
				if( !rtAllLit && !rtAllDark )
				{
					// float64 truth arbiter over the retained record triangles (see GateTruthVisibility)
					idMat4 mvpArb, invArb;
					memcpy( mvpArb.ToFloatPtr(), cap.hdr.worldMVP, sizeof( float ) * 16 );
					invArb = mvpArb.Inverse();
					const idVec3 gLightOrg = !s_lights.empty()
											 ? idVec3( s_lights[0].origin[0], s_lights[0].origin[1], s_lights[0].origin[2] )
											 : clOrg;
					const float diskR = cl.penumbraSize;
						// EXACT softpos receiver positions (RGBA32F: xyz world, w=1 valid) - the point the
						// shipped shader actually shaded. The arbiter judges truth HERE, not at a
						// depth-unprojected point: depth reconstruction is grazing-unstable (the very reason
						// softpos exists), so unprojecting reintroduces that error and mints false
						// LIT_IN_UMBRA at thin/grazing pixels where the reconstructed point lands in umbra
						// while the shaded surface point is lit. Falls back to unprojection where softpos is
						// unavailable (never rasterised, or the readback failed).
						std::vector<float> posBuf;
						{
							float* pp = NULL;
							if( globalImages->softShadowPosImage != NULL && globalImages->softShadowPosImage->GetTextureHandle() != NULL
									&& R_ReadPixelsRGBA32F( deviceManager->GetDevice(), &backEnd.GetCommonPasses(),
											globalImages->softShadowPosImage->GetTextureHandle(),
											nvrhi::ResourceStates::ShaderResource, &pp, W, H ) && pp != NULL )
							{
								posBuf.assign( pp, pp + ( size_t )W * H * 4 );
								R_StaticFree( pp );
							}
						}
					std::function<float( int, int )> truthAt = [&]( int x, int y ) -> float
					{
						size_t i = ( size_t )y * W + x;
						float dep = depthA.t[i];
						if( dep <= 1e-6f || dep >= 1.0f - 1e-6f || s_edges.empty() )
						{
							return -1.0f;
						}
						if( !posBuf.empty() && posBuf[i * 4 + 3] != 0.0f )
							{
								return GateTruthVisibility( idVec3( posBuf[i * 4 + 0], posBuf[i * 4 + 1], posBuf[i * 4 + 2] ), gLightOrg, diskR );
							}
							float wp[3];
						GateUnproject( invArb.ToFloatPtr(), x, y, W, H, dep, wp );
						return GateTruthVisibility( idVec3( wp[0], wp[1], wp[2] ), gLightOrg, diskR );
					};
					GateAgreement( anaA, rt, valid, cfg, defects, &defectPx, &crease, truthAt );
				}

				// continuity needs the capture matrices to hold for THIS render: unproject->reproject
				// must return to the same pixel (validated on a sample grid before trusting it).
				// The unprojection is the TRUE inverse of the capture worldMVP, computed here - the
				// capture's stored unprojectionToWorldMatrix is transposed relative to worldMVP and
				// reconstructs garbage (measured), so it is deliberately not used.
				idMat4 mvpM, invMvpM;
				memcpy( mvpM.ToFloatPtr(), cap.hdr.worldMVP, sizeof( float ) * 16 );
				invMvpM = mvpM.Inverse();
				// sample the VALID pixels themselves (a fixed grid misses small interaction regions
				// entirely and starved the check into a false SETUP defect)
				int reprojTested = 0, reprojOk = 0;
				const size_t reprojStride = ( validN > 64 ) ? ( size_t )( validN / 64 ) : 1;
				size_t validSeen = 0;
				for( size_t i = 0; i < valid.size() && reprojTested < 64; i++ )
				{
					if( !valid[i] )
					{
						continue;
					}
					if( ( validSeen++ % reprojStride ) != 0 )
					{
						continue;
					}
					{
						int x = ( int )( i % W ), y = ( int )( i / W );
						float dep = depthA.t[i];
						if( dep <= 1e-6f || dep >= 1.0f - 1e-6f )
						{
							continue;
						}
						float wp[3], sx, sy, nz;
						GateUnproject( invMvpM.ToFloatPtr(), x, y, W, H, dep, wp );
						if( !GateProject( cap.hdr.worldMVP, wp, W, H, sx, sy, nz ) )
						{
							continue;
						}
						reprojTested++;
						if( std::fabs( sx - x ) <= 2.0f && std::fabs( sy - y ) <= 2.0f )
						{
							reprojOk++;
						}
						else if( r_rtAccelDebug.GetBool() && reprojTested <= 5 )
						{
							common->Printf( "[softgate]   reproj (%d,%d) d=%.5f -> world (%.1f %.1f %.1f) -> (%.1f,%.1f)\n",
											x, y, depthA.t[i], wp[0], wp[1], wp[2], sx, sy );
						}
					}
				}
				const bool matricesHold = ( reprojTested >= 8 && reprojOk * 10 >= reprojTested * 7 );
				if( !matricesHold )
				{
					common->Printf( "[softgate] %s L%d: capture matrices do not reproject (%d/%d) -> SETUP defect, continuity skipped\n",
									cap.name.c_str(), li, reprojOk, reprojTested );
					GateDefect d;
					d.kind = GATE_SETUP;
					defects.push_back( d );
				}
				else
				{
					// displaced-view MVPs derived algebraically: V = P^-1 * MVP, then the translation
					// column shifts by -R*delta (delta in world). Convention: row-major, clip = M*(P,1).
					idMat4 P, MVP;
					memcpy( P.ToFloatPtr(), cap.hdr.projectionMatrix, sizeof( float ) * 16 );
					memcpy( MVP.ToFloatPtr(), cap.hdr.worldMVP, sizeof( float ) * 16 );
					idMat4 V = P.Inverse() * MVP;
					// ponytail: one horizontal + one vertical slide (not +-both) keeps the run fast and
					// still catches view-dependent pops; add the mirrored pair if flips ever slip through.
					const float DISP = 4.0f;
					const idVec3 deltas[2] = { rv.viewaxis[1] * DISP, rv.viewaxis[2] * DISP };
					for( int dv = 0; dv < 2; dv++ )
					{
						const idVec3& delta = deltas[dv];
						renderView_t drv = rv;
						drv.vieworg = rv.vieworg + delta;
						// coverage mask of the DISPLACED view first (shadows skipped, term==1 where this
						// light draws): the light's interaction region is screen-space and moves with the
						// camera, so without it the region's edge reads as a giant lit->umbra "flip"
						cvarSystem->SetCVarInteger( "r_skipShadows", 1 );
						GateRenderFrame( rw, &drv );
						swgate::GateImg dMask;
						GateReadR32F( globalImages->currentRenderHDRImage, dMask );
						cvarSystem->SetCVarInteger( "r_skipShadows", 0 );
						GateRenderFrame( rw, &drv );
						swgate::GateImg dTerm, dDepth;
						GateReadR32F( globalImages->currentRenderHDRImage, dTerm );
						GateReadR32F( globalImages->currentDepthImage, dDepth );
						if( !dTerm.Valid() || !dDepth.Valid() || !dMask.Valid() )
						{
							continue;
						}
						std::vector<uint8_t> dispValid( ( size_t )W * H, 0 );
						for( size_t vi = 0; vi < dispValid.size(); vi++ )
						{
							if( std::fabs( dMask.t[vi] - 1.0f ) <= 1e-3f )
							{
								dispValid[vi] = 1;
							}
						}
						float rd[3];		// R*delta: rotation part of V applied to the world displacement
						for( int r = 0; r < 3; r++ )
						{
							rd[r] = V[r][0] * delta.x + V[r][1] * delta.y + V[r][2] * delta.z;
						}
						GateView gv;
						memcpy( gv.mvp, cap.hdr.worldMVP, sizeof( gv.mvp ) );
						for( int r = 0; r < 4; r++ )
						{
							gv.mvp[r * 4 + 3] = cap.hdr.worldMVP[r * 4 + 3] - ( P[r][0] * rd[0] + P[r][1] * rd[1] + P[r][2] * rd[2] );
						}
						GateContinuity( anaA, depthA, valid, invMvpM.ToFloatPtr(), dTerm, dDepth, gv, cfg, defects, &crease, &defectPx, &dispValid );
					}
				}

				if( !defects.empty() )
				{
					idStr ppm = va( "softgate_%s_L%d.ppm", cap.name.c_str(), li );
					GateWritePPM( ppm.c_str(), anaA, valid, defectPx );
				}
			}

			// endgame diagnostics: once a light is down to a handful of defects, print each one
			if( !defects.empty() && defects.size() <= 24 )
			{
				for( const GateDefect& d : defects )
				{
					common->Printf( "[softgate]   %s area=%d bbox=(%d,%d)-(%d,%d)\n",
									GateKindName( d.kind ), d.area, d.x0, d.y0, d.x1, d.y1 );
				}
			}
			int counts[GATE_KIND_COUNT];
			GateTally( defects, counts );
			common->Printf( "[softgate] %-14s L%d (%dx%d, valid %ld px): TURD=%d ANT=%d LIT_IN_UMBRA=%d STEP=%d EXTENT=%d TEMPORAL=%d CONTINUITY=%d SETUP=%d\n",
							cap.name.c_str(), li, W, H, validN,
							counts[GATE_TURD], counts[GATE_ANT], counts[GATE_LIT_IN_UMBRA], counts[GATE_STEP],
							counts[GATE_EXTENT], counts[GATE_TEMPORAL], counts[GATE_CONTINUITY], counts[GATE_SETUP] );
			all.insert( all.end(), defects.begin(), defects.end() );

			rw->FreeLightDef( lh );
		}

		// ---- BENCH (com_softShadowGateBench N): the FULL shipped frame at this scenario ----------
		// Every parsed map light goes into the world (the frontend culls to the view, exactly like
		// gameplay), full shading (no term debug, ambient on), and N pipelined frames are timed.
		// This is the 60-FPS instrument: same launch, one ms/FPS line per capture.
		const int benchFrames = cvarSystem->GetCVarInteger( "com_softShadowGateBench" );
		if( benchFrames > 0 )
		{
			std::vector<qhandle_t> benchLights;
			benchLights.reserve( mapLights.size() );
			for( const renderLight_t& ml : mapLights )
			{
				benchLights.push_back( rw->AddLightDef( &ml ) );
			}
			// static model entities: without them the bench frame carries ~half the live soft-record
			// stream and under-reads the shipped soft cost ~3x (see the map-parse comment above)
			std::vector<qhandle_t> benchModels;
			benchModels.reserve( mapModels.size() );
			for( const renderEntity_t& me : mapModels )
			{
				benchModels.push_back( rw->AddEntityDef( &me ) );
			}
			R_SoftShadowPinTestConfig( false );
			cvarSystem->SetCVarInteger( "r_skipAmbient", 0 );
			cvarSystem->SetCVarInteger( "r_skipShadows", 0 );
			cvarSystem->SetCVarInteger( "r_useRTShadows", 0 );
			cvarSystem->SetCVarInteger( "r_softShadowDebugShader", 0 );
			// honour the LAUNCH shadow method so configs can be A/B-timed via +set
			cvarSystem->SetCVarInteger( "r_useSoftShadowVolumes", launchSoft );
			cvarSystem->SetCVarInteger( "r_shadowMapPCSS", launchPCSS );
			cvarSystem->SetCVarInteger( "r_useStencilShadows", launchStencil );
			cvarSystem->SetCVarInteger( "r_useShadowMapping", launchMapping );
			cvarSystem->SetCVarInteger( "r_shadowMapPCSSAnalyticContact", launchContact );
			cvarSystem->SetCVarFloat( "r_shadowPenumbraSize", launchPenumbra );
			for( int wu = 0; wu < 3; wu++ )		// warm-up: caches, atlas, pipelines
			{
				GateRenderFrame( rw, &rv );
			}
			const int t0 = Sys_Microseconds();
			int benchRecords = 0, benchDropped = 0;
			int benchSoftLights = 0, benchTermLights = 0, benchBinnedLights = 0;
			for( int f = 0; f < benchFrames; f++ )
			{
				// pipelined like the game loop: frontend builds frame f while the GPU draws f-1
				rw->RenderScene( &rv );
				// sample the stream counters BEFORE SwapCommandBuffers resets tr.pc for the next frame
				benchRecords = Max( benchRecords, tr.pc.c_softShadowEdges );
				benchDropped = Max( benchDropped, tr.pc.c_softShadowDroppedEdges );
				const emptyCommand_t* cmd = tr.SwapCommandBuffers( NULL, NULL, NULL, NULL, NULL, NULL );
				tr.RenderCommandBuffers( cmd );
				// PATH PROVENANCE, sampled from the backend counters of the frame just rendered:
				// which evaluation path each soft light's term actually took. term < total or
				// binned < total is not an error (slot budget, ineligible lights) but it must be
				// VISIBLE - a silent bit-exact fallback is a perf leak the gate cannot see (the
				// tile-rect Y-flip bug hid exactly this way).
				benchSoftLights   = Max( benchSoftLights,   backEnd.pc.c_softLightsTotal );
				benchTermLights   = Max( benchTermLights,   backEnd.pc.c_softLightsTerm );
				benchBinnedLights = Max( benchBinnedLights, backEnd.pc.c_softLightsBinned );
			}
			tr.SwapCommandBuffers( NULL, NULL, NULL, NULL, NULL, NULL );	// drain the last frame
			const double ms = ( Sys_Microseconds() - t0 ) / 1000.0 / benchFrames;
			// stream accounting from the LAST bench frame: dropped>0 means the frame BUDGET silently
			// erased whole lights' soft shadows - such a bench time is a lie (faster because shadows
			// are missing), so the drop count must be printed next to the ms it taints.
			common->Printf( "[softgate] BENCH %-14s %6.2f ms/frame (%4.0f FPS) over %d frames, %d map lights, "
							"%d soft records (%d dropped), %d soft lights (%d term, %d binned)\n",
							cap.name.c_str(), ms, 1000.0 / ms, benchFrames, ( int )mapLights.size(),
							benchRecords, benchDropped, benchSoftLights, benchTermLights, benchBinnedLights );
			for( qhandle_t bh : benchLights )
			{
				rw->FreeLightDef( bh );
			}
			for( qhandle_t bh : benchModels )
			{
				rw->FreeEntityDef( bh );
			}
		}

		R_SoftShadowClearCapturedCasters();
	}

	if( rw != NULL )
	{
		R_SoftShadowClearCapturedCasters();
		renderSystem->FreeRenderWorld( rw );
	}

	for( int i = 0; i < nTouched; i++ )
	{
		cvarSystem->SetCVarString( touched[i], prev[i].c_str() );
	}

	int counts[GATE_KIND_COUNT];
	GateTally( all, counts );
	common->Printf( "[softgate] ==============================================================\n" );
	common->Printf( "[softgate] TOTAL: TURD=%d ANT=%d LIT_IN_UMBRA=%d STEP=%d EXTENT=%d TEMPORAL=%d CONTINUITY=%d SETUP=%d\n",
					counts[GATE_TURD], counts[GATE_ANT], counts[GATE_LIT_IN_UMBRA], counts[GATE_STEP],
					counts[GATE_EXTENT], counts[GATE_TEMPORAL], counts[GATE_CONTINUITY], counts[GATE_SETUP] );
	common->Printf( "[softgate] TOTAL DEFECTS: %d across %d captures, %d lights -> %s\n",
					( int )all.size(), capsRun, lightsRun, all.empty() ? "PASS" : "FAIL" );
	return ( int )all.size();
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

	// ORACLE: ray-traced shadows - peter-pan IMMUNE (traces the actual scene geometry, no shadow-map projection),
	// rendered through the real Draw()->game->Draw()->RenderScene frontend that rebuilds per cvar, exactly like
	// toggling r_useRTShadows in-game. RT casts the LIVE world's own casters at their TRUE contact point, so the
	// PCSS gap against it IS the peter-panning. Converged (many rays, denoise off) for a stable reference; soft
	// radius matched to the analytic light-disk (r_shadowPenumbraSize) so RT and PCSS penumbra amplitudes agree.
	// (Stencil is a dead no-op in this build - proven identical to PCSS - so RT is the only working immune oracle.)
	cmdSystem->BufferCommandText( CMD_EXEC_NOW, "r_skipShadows 0 ; r_useStencilShadows 0 ; r_useSoftShadowVolumes 0 ; r_useShadowAtlas 0 ; r_shadowMapPCSS 0 ; r_useRTShadows 1 ; r_rtShadowDenoise 0 ; r_rtShadowRays 512 ; r_rtShadowAnalyticPenumbra 0\n" );	// RT oracle (immune)
	cvarSystem->SetCVarFloat( "r_rtShadowSoftRadius", cvarSystem->GetCVarFloat( "r_shadowPenumbraSize" ) );
	R_RenderOneFrame();		// NOTE: RT will NOT engage from here (accel structure is map-load time); the deltaN==0 guard below catches it
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

	cvarSystem->SetCVarInteger( "g_stopTime", 1 );	// freeze only now, for the (config-invariant) dbg dumps

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

	// PETER-PANNING METRIC. Oracle = RT (immune: shadow at TRUE contact). Hybrid = PCSS. Peter-panning displaces
	// the PCSS shadow AWAY from the object, so it shows as a signed disagreement, measured two ways:
	//   MISSED  = oracle-shadowed but hybrid-lit  -> the contact region PCSS wrongly leaves lit (the detach gap)
	//   FALSE   = hybrid-shadowed but oracle-lit  -> where the displaced PCSS shadow landed instead
	//   SHIFT   = |centroid(hybrid shadow) - centroid(oracle shadow)| in px -> the raw peter-pan displacement,
	//             with its direction, independent of shadow area (the single number the user asked for).
	// A pixel is a shadow-relevant SURFACE if either config shows it lit (excludes background); within that set a
	// pixel is "shadowed" in a config when its luminance is < darkenFrac of the brighter-of-the-two (the local
	// unshadowed estimate), so plain texture darkness that both configs share is never counted.
	const float litThresh = 0.12f;
	const float darkenFrac = 0.65f;
	long litN = 0, missedN = 0, falseN = 0, bothN = 0, deltaN = 0;
	double oSx = 0, oSy = 0, hSx = 0, hSy = 0;
	long oShad = 0, hShad = 0;
	std::vector<uint8_t> diff( ( size_t )rw * rh * 3, 0 );
	for( int i = 0; i < rw * rh; i++ )
	{
		float lr = SoftTestLum( &ref[( size_t )i * 4] );
		float ls = SoftTestLum( &test[( size_t )i * 4] );
		diff[( size_t )i * 3 + 0] = ( uint8_t )( lr * 255.0f );
		diff[( size_t )i * 3 + 1] = ( uint8_t )( ls * 255.0f );
		if( idMath::Fabs( lr - ls ) > 0.1f ) { deltaN++; }
		float bright = Max( lr, ls );
		if( bright < litThresh ) { continue; }			// background / unlit surface
		litN++;
		bool shadO = lr < darkenFrac * bright;			// RT says shadowed here
		bool shadH = ls < darkenFrac * bright;			// PCSS says shadowed here
		const int x = i % rw, y = i / rw;
		if( shadO ) { oShad++; oSx += x; oSy += y; }
		if( shadH ) { hShad++; hSx += x; hSy += y; }
		if( shadO && !shadH ) { missedN++; diff[( size_t )i * 3 + 2] = 255; }			// blue = detach gap
		else if( shadH && !shadO ) { falseN++; diff[( size_t )i * 3 + 0] = 255; }		// red-boost = misplaced shadow
		else if( shadO && shadH ) { bothN++; }
	}
	const double shiftX = ( oShad && hShad ) ? ( hSx / hShad - oSx / oShad ) : 0.0;
	const double shiftY = ( oShad && hShad ) ? ( hSy / hShad - oSy / oShad ) : 0.0;
	const double shift = sqrt( shiftX * shiftX + shiftY * shiftY );
	const long unionShad = missedN + falseN + bothN;
	const double iou = unionShad ? ( double )bothN / unionShad : 1.0;		// shadow overlap; peter-pan drives it down
	const double missedRate = oShad ? ( double )missedN / oShad : 0.0;
	// peter-pan verdict: RT and PCSS shadows should sit on top of each other. A big centroid shift or a large
	// fraction of the RT shadow that PCSS misses at contact = peter-panning.
	const bool pass = ( litN > 1000 ) && ( shift < 2.0 ) && ( missedRate < 0.15 );

	// diff image: R=oracle lum (red-boosted where PCSS over-shadows), G=hybrid lum, B=detach-gap mask
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
	if( deltaN == 0 )
	{
		// RT builds its acceleration structure at MAP LOAD, so enabling r_useRTShadows here (mid-session, from a
		// console command) is a no-op and the RT oracle renders identically to the PCSS hybrid - a false PASS.
		// The objective metric must be run as TWO launches with the config set on the command line:
		common->Warning( "testSoftShadowLocator: oracle == hybrid (RT did NOT engage mid-run). Use the autonomous "
						 "two-launch harness for a valid peter-pan measurement: neo/tools/softshadow/run_peterpan.sh <capture.softcap>" );
	}
	common->Printf( "[softtest] %s : PETER-PAN shift=%.2f px (dx=%.2f dy=%.2f)  missed=%ld/%ld RTshadow (%.1f%%)  false=%ld  IoU=%.3f  shadowDelta=%ld  ->  %s\n",
					args.Argv( 1 ), shift, shiftX, shiftY, missedN, oShad, missedRate * 100.0, falseN, iou, deltaN, pass ? "PASS" : "FAIL" );

	R_SoftShadowClearCapturedCasters();		// remove the replayed captured casters from the render world
}
