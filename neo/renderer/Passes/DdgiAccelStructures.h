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
	nvrhi::rt::IAccelStruct*	GetOrBuildBottomLevel( nvrhi::ICommandList* commandList, const drawSurf_t* surf );

	// Skinned/animated actors: GPU-skin the posed positions into a pooled buffer and
	// build a per-surface BLAS from them each frame, so animated actors reflect in
	// their current pose instead of the bind pose. Appends instances for every
	// GPU-skinned surface in the view; no-op until the skinning pipeline is built.
	void			BuildSkinnedInstances( nvrhi::ICommandList* commandList, const viewDef_t* viewDef,
										   std::vector<nvrhi::rt::InstanceDesc>& instances,
										   std::vector<DdgiInstanceData>& instanceData );
	void			EnsureSkinPipeline();

	nvrhi::DeviceHandle				m_Device;
	nvrhi::rt::AccelStructHandle	m_Tlas;
	size_t							m_TlasCapacity;
	int								m_NumInstances;

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
