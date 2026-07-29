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

private:
	nvrhi::rt::IAccelStruct*	GetOrBuildBottomLevel( nvrhi::ICommandList* commandList, const drawSurf_t* surf );

	nvrhi::DeviceHandle				m_Device;
	nvrhi::rt::AccelStructHandle	m_Tlas;
	size_t							m_TlasCapacity;
	int								m_NumInstances;

	// BLAS cache keyed by the static ambientCache handle (unique per surface).
	std::unordered_map<vertCacheHandle_t, nvrhi::rt::AccelStructHandle> m_BlasCache;
};

#endif
