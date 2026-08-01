/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 2026 Robert Beckebans (RBDOOM-3-BFG contributors)

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG
Edition Source Code").

Doom 3 BFG Edition Source Code is free software: you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation, either version 3 of the License,
or (at your option) any later version.

Doom 3 BFG Edition Source Code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
Public License for more details.

You should have received a copy of the GNU General Public License along with
Doom 3 BFG Edition Source Code.  If not, see <http://www.gnu.org/licenses/>.

===========================================================================
*/
#ifndef RENDERER_PASSES_DDGIACCELSTRUCTURES_H_
#define RENDERER_PASSES_DDGIACCELSTRUCTURES_H_

#include <unordered_map>
#include <unordered_set>
#include <vector>

/*
================================================================================

	DdgiAccelStructures - ray tracing acceleration structures for DDGI.

	M1 scope: build one BLAS per unique *static* world surface (cached by its
	static vertex-cache handle, so each surface is only built once) and rebuild
	a TLAS from the visible static surfaces every frame. Both reference the
	shared static vertex/index cache directly - those buffers are allocated with
	isAccelStructBuildInput when r_useDDGI is set (see BufferObject_NVRHI.cpp).

	Not yet handled (later milestones): dynamic/skinned geometry that lives in
	the per-frame cache and needs a BLAS refit each frame, and BLAS compaction.

	RUNTIME-UNVERIFIED: compiles against the NVRHI RT API but has not been
	validated on a GPU. The instance transform conversion (GL column-major ->
	nvrhi row-major 3x4) and the vertex/index offsets are the first things to
	check in a RenderDoc capture.

================================================================================
*/

struct viewDef_t;
struct drawSurf_t;
class idMaterial;

// Per-TLAS-instance shading data, indexed by the ray hit's InstanceID in the
// trace shader. Layout must match DdgiInstanceData in probe_trace.cs.hlsl.
// Offsets are byte offsets into the shared static vertex/index cache buffers.
struct DdgiInstanceData
{
	uint32_t	vertexByteOffset;	// start of this surface's idDrawVert run
	uint32_t	indexByteOffset;	// start of this surface's index run (R16)
	uint32_t	pad0;
	uint32_t	pad1;
	float		albedo[4];			// average diffuse rgb (+ pad)
};

class DdgiAccelStructures
{
public:
	DdgiAccelStructures();

	void			Init( nvrhi::IDevice* device );
	void			Shutdown();

	// RT shadows only: restrict the TLAS to real shadow casters - drop noShadows-flagged
	// surfaces and translucent geometry (glass, blended). Without this the shadow TLAS
	// includes the same detail the stencil path excludes (grates, railings, decals, window
	// glass), which casts spurious aliased shadows and turns lit areas dark. DDGI and
	// reflections leave this off so their rays still see that geometry.
	void			SetShadowCastersOnly( bool b )
	{
		m_ShadowCastersOnly = b;
	}

	// RT shadows only: skip the 94-area frontend flood-occluder gather (AppendFrontendOccluders,
	// the bulk of the TLAS - thousands of instances every frame). The per-light shadow-caster
	// chains (AppendLightShadowCasters, vLight->globalShadows/localShadows) already carry every
	// occluder that shadows a visible light, including off-frustum ones, so for SHADOWS the flood
	// gather is redundant; dropping it shrinks the shadow TLAS from thousands to hundreds (faster
	// build AND faster rays). Reflections/DDGI leave this off - they genuinely need the wide world.
	void			SetSkipFrontendOccluders( bool b )
	{
		m_SkipFrontendOccluders = b;
	}

	// (Re)build the TLAS for this view, building any missing static BLAS as a
	// side effect. Returns true when a non-empty TLAS is ready to trace against.
	bool			RebuildFromView( nvrhi::ICommandList* commandList, const viewDef_t* viewDef );

	nvrhi::rt::IAccelStruct*	GetTLAS() const
	{
		return m_Tlas;
	}
	int				NumInstances() const
	{
		return m_NumInstances;
	}

	// Parallel to the TLAS instances (indexed by InstanceID): per-hit geometry
	// offsets + average albedo. Bound as a StructuredBuffer SRV to the trace shader.
	nvrhi::IBuffer*	GetInstanceDataBuffer() const
	{
		return m_InstanceDataBuffer;
	}
	// The shared static vertex/index cache buffers the BLAS reference, bound raw
	// (ByteAddressBuffer) so the trace shader can fetch hit-triangle positions.
	nvrhi::IBuffer*	GetStaticVertexBuffer() const;
	nvrhi::IBuffer*	GetStaticIndexBuffer() const;

	// Ember dissolve: expose the posed-position pool + the per-surface ranges skinned
	// this frame, so the ember pass can seed particles from the posed mesh vertices.
	// Valid for the current frame only (rebuilt every RebuildFromView).
	struct PosedRange
	{
		int			entityIndex;	// idRenderEntityLocal::index (== entity handle)
		uint32_t	outVertBase;	// first posed vertex in the pool (float3 units)
		uint32_t	numVerts;
		float		modelMatrix[16];	// model->world (id column-major)
	};
	nvrhi::IBuffer*	GetPosedBuffer() const
	{
		return m_PosedBuffer;
	}
	const std::vector<PosedRange>& GetPosedRanges() const
	{
		return m_PosedRanges;
	}

private:
	// Build (or fetch cached) a BLAS for a static surface from its vertex/index cache
	// handles. Keyed by ambientCache so each unique surface is built once.
	nvrhi::rt::IAccelStruct*	GetOrBuildBottomLevel( nvrhi::ICommandList* commandList,
			vertCacheHandle_t vbHandle, vertCacheHandle_t ibHandle, int numVerts, int numIndexes );

	// Push one TLAS instance + its parallel shading record for a static surface.
	void			AppendInstance( std::vector<nvrhi::rt::InstanceDesc>& instances,
									std::vector<DdgiInstanceData>& instanceData,
									nvrhi::rt::IAccelStruct* blas, const float* modelMatrix,
									vertCacheHandle_t ambientCache, vertCacheHandle_t indexCache,
									const idMaterial* material );

	// Widen the occluder set beyond the view frustum by adding each visible light's
	// shadow-caster surfaces (vLight->globalShadows/localShadows) - a frozen viewDef
	// snapshot the frontend already builds, force-resident and per-light, so off-view
	// casters stay in the TLAS under camera rotation. Deduped against the view surfaces.
	void			AppendLightShadowCasters( nvrhi::ICommandList* commandList, const viewDef_t* viewDef,
			std::vector<nvrhi::rt::InstanceDesc>& instances,
			std::vector<DdgiInstanceData>& instanceData,
			std::unordered_set<vertCacheHandle_t>& seen );

	// Add the frontend-gathered static world occluders (viewDef->rtOccluders): the
	// camera's connected-area geometry, frustum-independent, for shadows AND reflections.
	// A frozen frame-allocated snapshot (see R_GatherRTOccluders), safe to read here.
	void			AppendFrontendOccluders( nvrhi::ICommandList* commandList, const viewDef_t* viewDef,
			std::vector<nvrhi::rt::InstanceDesc>& instances,
			std::vector<DdgiInstanceData>& instanceData,
			std::unordered_set<vertCacheHandle_t>& seen );

	// Skinned/animated actors: GPU-skin the posed positions into a pooled buffer and
	// build a per-surface BLAS from them each frame, so animated actors reflect in
	// their current pose instead of the bind pose. Appends instances for every
	// GPU-skinned surface in the view; no-op until the skinning pipeline is built.
	void			BuildSkinnedInstances( nvrhi::ICommandList* commandList, const viewDef_t* viewDef,
										   std::vector<nvrhi::rt::InstanceDesc>& instances,
										   std::vector<DdgiInstanceData>& instanceData );
	void			EnsureSkinPipeline();

	// RT shadows only: build the TLAS from real shadow casters (see SetShadowCastersOnly).
	bool							m_ShadowCastersOnly;

	// RT shadows only: skip the wide 94-area flood gather (see SetSkipFrontendOccluders).
	bool							m_SkipFrontendOccluders;

	nvrhi::DeviceHandle				m_Device;
	nvrhi::rt::AccelStructHandle	m_Tlas;
	size_t							m_TlasCapacity;
	int								m_NumInstances;
	int								m_NumShadowBrushInstances = 0;	// diagnostics: textures/common/shadow casters in the TLAS
	int								m_LastShadowBrushInstances = -1;

	nvrhi::BufferHandle				m_InstanceDataBuffer;
	size_t							m_InstanceDataCapacity;	// in instances

	// BLAS cache keyed by the static ambientCache handle (unique per surface).
	std::unordered_map<vertCacheHandle_t, nvrhi::rt::AccelStructHandle> m_BlasCache;

	// --- skinned actors ---
	nvrhi::ShaderHandle				m_SkinShader;
	nvrhi::BindingLayoutHandle		m_SkinBindingLayout;
	nvrhi::BindingSetHandle			m_SkinBindingSet;
	nvrhi::ComputePipelineHandle	m_SkinPipeline;
	nvrhi::BufferHandle				m_SkinConstantBuffer;
	nvrhi::IBuffer*					m_SkinBoundJoints;		// joint buffer the set was built against
	nvrhi::IBuffer*					m_SkinBoundVertex;		// vertex buffer the set was built against
	bool							m_SkinPipelineTried;

	// posed-position pool (RWByteAddressBuffer, RGB32 per vertex) + per-frame BLAS pool
	nvrhi::BufferHandle				m_PosedBuffer;
	size_t							m_PosedCapacityVerts;
	std::vector<nvrhi::rt::AccelStructHandle> m_SkinnedBlas;

	// per-surface posed ranges skinned this frame (for ember seeding)
	std::vector<PosedRange>			m_PosedRanges;
};

#endif
