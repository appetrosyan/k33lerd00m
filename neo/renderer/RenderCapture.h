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

// Soft-shadow SCENE CAPTURE (.cap). Captures one main view's soft-shadow state - camera, the per-light
// analytic soft-shadow edge records the coverage shader consumes, each caster's triangle mesh (for an offline
// ray-cast ground truth), and the depth buffer - so a problem spot can be reconstructed HEADLESS and its
// coverage measured against truth, then simplified to a minimal repro. See .claude/plans/concurrent-kindling.
//
// The on-disk layout below is PLAIN POD (little-endian float/uint, NO idlib types) so the offline reader in
// neo/tests can consume it with zero engine dependency. The engine-side capture API is declared at the bottom
// (forward-declared types only), so this header is safe to include from the unit tests too.

#ifndef __RENDERCAPTURE_H__
#define __RENDERCAPTURE_H__

#include <stdint.h>

// bump on any layout change; the reader rejects mismatches.
#define CAP_MAGIC   0x50434653u			// 'SFCP' little-endian
#define CAP_VERSION 6u					// v2: receiver meshes; v3: global mesh indices; v4: mapName + gameTime (self-identifying for reconstruction). reserved[0]=gameTimeMs, reserved[1]=mapName byte length (a trailing MAPNAME block follows recvIdx). v5: FACE-mode edge sections hold the STREAM V2 pure-tri float4 triples (zero-padded to pair records), no inline caster headers; pre-v5 face sections are v1 header+pair records (tests convert via FaceStreamFromV1Records). v6: EMBEDDED SAVEGAME tail block (CAP_SAVE_MAGIC) after the v5 texture tail - the engine's full save serialization (health/ammo/weapons/inventory/all entities) so a capture reconstructs the EXACT live game state, not just the render state.

// v5 TEXTURE TAIL: appended AFTER every v4 block, self-describing (magic + counts), so the header
// layout never changes and old readers simply stop before it. Carries what the offline verification
// renders REAL textures from: per-receiver-vertex UVs, a per-receiver material index, and each unique
// material's diffuse image baked down to <=CAP_TEX_MAX on the long side (RGB8).
#define CAP_TAIL_MAGIC 0x35544653u		// 'SFT5' little-endian
#define CAP_TEX_MAX    128

// v6 SAVEGAME TAIL: appended AFTER the v5 texture tail. Carries the engine's full save serialization
// (idGameLocal::SaveGame over the session's in-memory idFile_SaveGame buffers) so a capture reconstructs
// the EXACT live game state - health, ammo, weapons, inventory, every entity - not just the render state.
// Two blobs: the main save buffer and the string-table buffer. Self-describing (magic + lengths), so a v5
// reader simply stops before it.
#define CAP_SAVE_MAGIC 0x36544653u		// 'SFT6' little-endian

// v7 LIGHT-PARMS TAIL: the LIVE renderLight_t essentials per captured light, appended after the v6
// save tail (magic + count + array). Cutscenes script-move and retint lights, so the defect gate's
// map-parse reconstruction diverges from the captured frame (cap0010: matched light 187u away,
// pre-cutscene state, drew 0 px). With this tail the gate rebuilds each probe light from the
// CAPTURED state and needs no map match at all. Older caps simply lack the tail (map-match path).
#define CAP_LPARM_MAGIC 0x37544653u		// 'SFT7' little-endian

#pragma pack( push, 1 )

struct capLightParms_t
{
	float    origin[3];
	float    axis[9];
	float    lightRadius[3];
	float    lightCenter[3];
	float    target[3];			// spot projection (all zero for point lights)
	float    right[3];
	float    up[3];
	float    start[3];
	float    end[3];
	float    shaderParms[12];
	uint32_t pointLight;
	uint32_t parallel;
	uint32_t noShadows;
	char     shaderName[64];
};

// One flattened soft-shadow edge record = the coverage shader's t_SoftEdges element (softShadowEdge_t).
// Header records carry e0w < 0 and the caster bounding sphere; edge records carry world endpoints.
struct capEdge_t
{
	float e0[4];		// xyz endpoint 0 / (centre, -1 marker) for a header; w = silhouette weight
	float e1[4];		// xyz endpoint 1 / (radius,0,0,casterId) for a header; w = header record index
};

// Per soft-shadow light: origin + the range of its edge records in the EDGES block.
struct capLight_t
{
	float    origin[3];			// vLight->globalLightOrigin
	float    penumbraSize;		// r_shadowPenumbraSize at capture time (disk radius)
	int32_t  scissor[4];		// x1,y1,x2,y2
	uint32_t firstEdge;			// index into the EDGES block
	uint32_t edgeCount;			// number of records (edges + headers)
	uint32_t firstCaster;		// index into the CASTERS table
	uint32_t casterCount;		// number of caster meshes for this light
};

// Per caster mesh (world space), for the offline ray-cast ground truth. Verts/indices live in the MESHVERTS
// / MESHIDX blocks; firstVert/firstIndex are element offsets there.
struct capCaster_t
{
	uint32_t lightIndex;		// which capLight_t this caster belongs to
	float    casterId;			// matches the header record's e1.w for correlation
	uint32_t firstVert;			// index into MESHVERTS (float3 elements)
	uint32_t numVerts;
	uint32_t firstIndex;		// index into MESHIDX (uint32 elements); triangles = numIndex/3
	uint32_t numIndex;
};

// Per CAPPED shadow-volume surface (world space), read back from the GPU shadowCache/shadowIndexCache - the
// exact geometry the stencil pass rasterises. w==0 verts already extruded to (far) infinity from the light.
// A separate tagged array (NOT fields on capLight_t) so the existing structs keep their size/ABI. v4+.
// Header: reserved[2]=numShadowVols, reserved[3]=numShadowVerts(float3), reserved[4]=numShadowIdx(uint32).
struct capShadowVol_t
{
	uint32_t lightIndex;
	uint32_t firstVert;			// index into SHADOWVERTS (float3)
	uint32_t numVert;
	uint32_t firstIdx;			// index into SHADOWIDX (uint32); triangles = numIdx/3
	uint32_t numIdx;
};

// v5 TEXTURE TAIL: one entry per unique receiver material; texels are RGB8, row-major, firstTexel is a
// BYTE offset into the tail's texel blob. Tail layout after the magic:
//   uint32 numMaterials, numTexelBytes, numStFloats (= 2 * numRecvVerts), numRecvMats (= numReceivers)
//   capMaterial_t[numMaterials] ; uint8 texels[numTexelBytes] ; float st[numStFloats] ; uint32 recvMat[numRecvMats]
struct capMaterial_t
{
	char     name[64];			// material name, for diagnostics
	uint32_t texW, texH;		// baked diffuse dimensions (<= CAP_TEX_MAX per side)
	uint32_t firstTexel;		// byte offset of this material's RGB8 texels in the tail blob
};

// Per RECEIVER interaction surface (world space): the surfaces the coverage shader shades. The coverage uses
// the receiver's surface position, so evaluating coverage-vs-truth at these surface points reproduces the
// artifact WITHOUT depth reconstruction. Verts/indices live in the RECVVERTS / RECVIDX blocks.
struct capReceiver_t
{
	uint32_t lightIndex;		// which light this receiver surface interacts with
	uint32_t firstVert;			// index into RECVVERTS (float3 elements)
	uint32_t numVerts;
	uint32_t firstIndex;		// index into RECVIDX (uint32 elements)
	uint32_t numIndex;
};

// File header. Fixed-size; followed by the variable blocks in this order:
//   lights[numLights]  edges[numEdges]  casters[numCasters]  meshVerts[numMeshVerts*3]
//   meshIdx[numMeshIdx]  depth[screenW*screenH] (float32, row-major top-left origin)
// Each block's element count is here, so a reader seeks by accumulation; a newer version only appends.
struct capHeader_t
{
	uint32_t magic;
	uint32_t version;
	uint32_t screenW, screenH;

	// camera / view (see viewDef_t + renderView_t)
	float    vieworg[3];
	float    viewaxis[9];					// idMat3, row-major
	float    fovx, fovy;
	float    projectionMatrix[16];
	float    unjitteredProjectionMatrix[16];
	float    unprojectionToWorldMatrix[16];	// depth -> world reconstruction
	float    worldMVP[16];
	int32_t  viewport[4];					// x1,y1,x2,y2 (real pixels, Y-flipped as stored by the engine)
	int32_t  taaFrameCount;

	uint32_t numLights;
	uint32_t numEdges;						// total capEdge_t across all lights
	uint32_t numCasters;
	uint32_t numMeshVerts;					// float3 count (caster meshes)
	uint32_t numMeshIdx;					// uint32 count (caster meshes)
	uint32_t hasDepth;						// 1 if a depth block follows, else 0
	uint32_t numReceivers;					// capReceiver_t count
	uint32_t numRecvVerts;					// float3 count (receiver meshes)
	uint32_t numRecvIdx;					// uint32 count (receiver meshes)
	uint32_t reserved[5];
};

#pragma pack( pop )

#ifdef __cplusplus
#ifndef CAP_NO_ENGINE_API

// ---- engine-side capture API (defined in RenderCapture.cpp) --------------------------------------
struct viewDef_t;
struct viewLight_t;
struct softShadowEdge_t;
class  idCmdArgs;

// console command: `captureSoftShadow` arms a one-shot capture.
void  R_CaptureSoftShadow_f( const idCmdArgs& args );
void  R_CaptureShadowRefs_f( const idCmdArgs& args );
// Automated in-engine self-check: renders the RT oracle vs the soft+PCSS-locator hybrid from one frozen view
// and prints a PASS/FAIL false-shadow verdict (exercises the real frontend/shader/atlas/uniform plumbing).
void  R_TestSoftShadowLocator_f( const idCmdArgs& args );

// Skip the intro cinematic and pin the view at a .cap camera over the next ~120 frames, driven by the NORMAL
// frame loop (Common::Frame calls R_SoftShadowGotoTick). Run `softShadowGoto <cap>` + `wait 90` before the test.
void  R_SoftShadowGoto_f( const idCmdArgs& args );
void  R_SoftShadowGotoTick();
// Headless corpus render: `softShadowShots <cap...>` renders each capture (shot_<name>.png) and quits,
// driven from the frame loop by R_SoftShadowBatchTick - no bash/+wait/timeout orchestration.
void  R_SoftShadowShots_f( const idCmdArgs& args );
void  R_SoftShadowBatchTick();
// Warn on contradictory shadow-technique cvar combos (silent-no-op guard). R_CheckShadowConflicts returns
// the active-conflict count; the _f is the `checkShadowConfig` console command; the Tick polls each frame.
int   R_CheckShadowConflicts( bool verbose );
void  R_CheckShadowConflicts_f( const idCmdArgs& args );
void  R_ShadowConflictTick();

// REPRO HARNESS: `softShadowRepro <cap...>` restores each capture's embedded savegame (exact game state),
// freezes, A/Bs scanline-vs-sampled at one identical pose, diffs in-engine with a differential canary, then
// quits. Driven by R_SoftShadowReproTick (frame loop); the per-capture score is the synchronous _Shot.
void  R_SoftShadowRepro_f( const idCmdArgs& args );
// RECAPTURE: `softShadowRecapture <cap...>` regenerates each cap in place (v7 tail) via the same load/goto path.
void  R_SoftShadowRecapture_f( const idCmdArgs& args );
void  R_SoftShadowReproShot_f( const idCmdArgs& args );
void  R_SoftShadowReproTick();
void  R_SoftShadowSpawnCasters_f( const idCmdArgs& args );	// reproduce a capture's dynamic casters (the rock)
// REAL-GAME SMOKE gate: `softShadowGateSmoke <map> [frames]` devmaps the map through the FULL shipped
// load+present path, then R_SoftShadowGateSmokeTick (frame loop) counts genuine game-view frames and asserts
// the lit HDR frame is not black. Catches load-path stalls / black-frame-no-crash that the minimal-init
// com_softShadowGate is structurally blind to. Sets the process exit code (nonzero=FAIL), then quits.
void  R_SoftShadowGateSmoke_f( const idCmdArgs& args );
void  R_SoftShadowGateSmokeTick( int viewLights );	// viewLights = completed frame's stats_frontend.c_viewLights
// Pin every shadow-relevant cvar to the explicit soft-shadow test baseline (RT off, soft on, atlas+PCSS, etc.),
// so the self-test NEVER inherits an archived D3BFGConfig value (e.g. r_useRTShadows 1 silently disabling the
// soft-wedge path). verbose = log each value + WARN on any archived override.
void  R_SoftShadowPinTestConfig( bool verbose );

// MINIMAL-INIT variant: Common.cpp calls this right after renderSystem->Init() (before game/sound/menu/player)
// when com_softShadowSelfTest names a map. Loads that map's renderWorld + real .map lights + the paired
// capture camera, renders RT-oracle vs soft+PCSS hybrid, prints the verdict. Returns false-shadow px (0=pass).
int   R_SoftShadowSelfTest( const char* mapName );

// MINIMAL-INIT corpus DEFECT GATE (com_softShadowGate): reconstructs every .cap scene (map + captured
// casters + captured lights), renders the SHIPPED GPU soft-shadow path at >=1920x1080 plus an in-engine RT
// reference, and counts image defects individually (turds/ants/lit-in-umbra/penumbra steps/extent/temporal
// jitter/view-continuity flips) via tests/SoftShadowGate.h. Green iff the returned total is ZERO.
int   R_SoftShadowGate( const char* arg );

// True when a one-shot capture has been armed by the `captureSoftShadow` console command.
bool  R_SoftShadowCaptureArmed();

// Called from the frontend soft-edge flatten (tr_frontend_addmodels.cpp) while armed: retains a CPU copy of
// this light's flattened edge array (with header records) before it is uploaded and dropped.
void  R_CaptureLightEdges( const viewLight_t* vLight, const softShadowEdge_t* flat, int records );

// Called from the frontend once the main viewDef is built: walks the view's soft lights + caster meshes and
// snapshots everything except the framebuffer. Disarms the frontend half of the capture.
void  R_CaptureFrontendView( const viewDef_t* viewDef );

// Called from the backend after the interaction pass (same frame): grabs the screenshot + depth and writes
// the complete .cap + .png. Fully disarms the capture.
void  R_CaptureBackendFinish();

#endif // CAP_NO_ENGINE_API
#endif // __cplusplus

#endif // __RENDERCAPTURE_H__
