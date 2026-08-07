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

// Soft-shadow SCENE CAPTURE (.softcap). Captures one main view's soft-shadow state - camera, the per-light
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
#define SOFTCAP_MAGIC   0x50434653u			// 'SFCP' little-endian
#define SOFTCAP_VERSION 2u					// v2: added receiver surface meshes (RECVVERTS/RECVIDX)

#pragma pack( push, 1 )

// One flattened soft-shadow edge record = the coverage shader's t_SoftEdges element (softShadowEdge_t).
// Header records carry e0w < 0 and the caster bounding sphere; edge records carry world endpoints.
struct softcapEdge_t
{
	float e0[4];		// xyz endpoint 0 / (centre, -1 marker) for a header; w = silhouette weight
	float e1[4];		// xyz endpoint 1 / (radius,0,0,casterId) for a header; w = header record index
};

// Per soft-shadow light: origin + the range of its edge records in the EDGES block.
struct softcapLight_t
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
struct softcapCaster_t
{
	uint32_t lightIndex;		// which softcapLight_t this caster belongs to
	float    casterId;			// matches the header record's e1.w for correlation
	uint32_t firstVert;			// index into MESHVERTS (float3 elements)
	uint32_t numVerts;
	uint32_t firstIndex;		// index into MESHIDX (uint32 elements); triangles = numIndex/3
	uint32_t numIndex;
};

// Per RECEIVER interaction surface (world space): the surfaces the coverage shader shades. The coverage uses
// the receiver's surface position, so evaluating coverage-vs-truth at these surface points reproduces the
// artifact WITHOUT depth reconstruction. Verts/indices live in the RECVVERTS / RECVIDX blocks.
struct softcapReceiver_t
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
struct softcapHeader_t
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
	uint32_t numEdges;						// total softcapEdge_t across all lights
	uint32_t numCasters;
	uint32_t numMeshVerts;					// float3 count (caster meshes)
	uint32_t numMeshIdx;					// uint32 count (caster meshes)
	uint32_t hasDepth;						// 1 if a depth block follows, else 0
	uint32_t numReceivers;					// softcapReceiver_t count
	uint32_t numRecvVerts;					// float3 count (receiver meshes)
	uint32_t numRecvIdx;					// uint32 count (receiver meshes)
	uint32_t reserved[5];
};

#pragma pack( pop )

#ifdef __cplusplus
#ifndef SOFTCAP_NO_ENGINE_API

// ---- engine-side capture API (defined in RenderCapture.cpp) --------------------------------------
struct viewDef_t;
struct viewLight_t;
struct softShadowEdge_t;
class  idCmdArgs;

// console command: `captureSoftShadow` arms a one-shot capture.
void  R_CaptureSoftShadow_f( const idCmdArgs& args );

// True when a one-shot capture has been armed by the `captureSoftShadow` console command.
bool  R_SoftShadowCaptureArmed();

// Called from the frontend soft-edge flatten (tr_frontend_addmodels.cpp) while armed: retains a CPU copy of
// this light's flattened edge array (with header records) before it is uploaded and dropped.
void  R_CaptureLightEdges( const viewLight_t* vLight, const softShadowEdge_t* flat, int records );

// Called from the frontend once the main viewDef is built: walks the view's soft lights + caster meshes and
// snapshots everything except the framebuffer. Disarms the frontend half of the capture.
void  R_CaptureFrontendView( const viewDef_t* viewDef );

// Called from the backend after the interaction pass (same frame): grabs the screenshot + depth and writes
// the complete .softcap + .png. Fully disarms the capture.
void  R_CaptureBackendFinish();

#endif // SOFTCAP_NO_ENGINE_API
#endif // __cplusplus

#endif // __RENDERCAPTURE_H__
