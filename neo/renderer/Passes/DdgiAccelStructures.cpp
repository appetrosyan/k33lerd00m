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
#include <precompiled.h>
#pragma hdrstop

#include "renderer/RenderCommon.h"

#include "DdgiAccelStructures.h"

DdgiAccelStructures::DdgiAccelStructures()
	: m_TlasCapacity( 0 )
	, m_NumInstances( 0 )
	, m_InstanceDataCapacity( 0 )
{
}

nvrhi::IBuffer* DdgiAccelStructures::GetStaticVertexBuffer() const
{
	return vertexCache.staticData.vertexBuffer.GetAPIObject();
}

nvrhi::IBuffer* DdgiAccelStructures::GetStaticIndexBuffer() const
{
	return vertexCache.staticData.indexBuffer.GetAPIObject();
}

/*
========================
DDGI_MaterialAverageAlbedo

Average diffuse colour used for coloured probe bounce. Diffuse GI is
low-frequency, so one representative colour per material is enough (design
decision: per-surface average, no bindless / UVs).

STAGE 1: returns flat grey so hit geometry + normals can be verified first.
Real per-material average diffuse extraction is a Stage 1 refinement.
========================
*/
static idVec3 DDGI_MaterialAverageAlbedo( const idMaterial* material )
{
	return idVec3( 0.5f, 0.5f, 0.5f );
}

void DdgiAccelStructures::Init( nvrhi::IDevice* device )
{
	m_Device = device;
}

void DdgiAccelStructures::Shutdown()
{
	m_BlasCache.clear();
	m_Tlas = nullptr;
	m_TlasCapacity = 0;
	m_NumInstances = 0;
	m_InstanceDataBuffer = nullptr;
	m_InstanceDataCapacity = 0;
}

/*
========================
R_ModelMatrixToAffine

Doom's modelMatrix is an OpenGL column-major 4x4 (translation at indices
12/13/14). nvrhi::rt::AffineTransform is a row-major 3x4 stored as float[12]
with the translation in the last column of each row.
========================
*/
static void R_ModelMatrixToAffine( const float m[16], nvrhi::rt::AffineTransform& out )
{
	out[0]  = m[0];		out[1]  = m[4];		out[2]  = m[8];		out[3]  = m[12];
	out[4]  = m[1];		out[5]  = m[5];		out[6]  = m[9];		out[7]  = m[13];
	out[8]  = m[2];		out[9]  = m[6];		out[10] = m[10];	out[11] = m[14];
}

/*
========================
DdgiAccelStructures::GetOrBuildBottomLevel

Returns the cached BLAS for this static surface, building it on first use from
the shared static vertex/index cache. Only the position channel of idDrawVert
is used for ray tracing.
========================
*/
nvrhi::rt::IAccelStruct* DdgiAccelStructures::GetOrBuildBottomLevel( nvrhi::ICommandList* commandList, const drawSurf_t* surf )
{
	const vertCacheHandle_t vbHandle = surf->ambientCache;
	const vertCacheHandle_t ibHandle = surf->indexCache;

	std::unordered_map<vertCacheHandle_t, nvrhi::rt::AccelStructHandle>::iterator it = m_BlasCache.find( vbHandle );
	if( it != m_BlasCache.end() )
	{
		return it->second;
	}

	// static geometry lives in the persistent static cache buffers
	idVertexBuffer* vertexBuffer = &vertexCache.staticData.vertexBuffer;
	idIndexBuffer* indexBuffer = &vertexCache.staticData.indexBuffer;

	if( vertexBuffer->GetAPIObject() == NULL || indexBuffer->GetAPIObject() == NULL )
	{
		return NULL;
	}

	const uint vertOffset = static_cast<uint>( vbHandle >> VERTCACHE_OFFSET_SHIFT ) & VERTCACHE_OFFSET_MASK;
	const uint indexOffset = static_cast<uint>( ibHandle >> VERTCACHE_OFFSET_SHIFT ) & VERTCACHE_OFFSET_MASK;

	nvrhi::rt::GeometryTriangles tris;
	tris.setVertexBuffer( vertexBuffer->GetAPIObject() )
	.setVertexFormat( nvrhi::Format::RGB32_FLOAT )
	.setVertexOffset( vertOffset + DRAWVERT_XYZ_OFFSET )
	.setVertexStride( sizeof( idDrawVert ) )
	.setVertexCount( surf->frontEndGeo->numVerts )
	.setIndexBuffer( indexBuffer->GetAPIObject() )
	.setIndexFormat( nvrhi::Format::R16_UINT )
	.setIndexOffset( indexOffset )
	.setIndexCount( surf->numIndexes );

	nvrhi::rt::GeometryDesc geom;
	geom.setTriangles( tris ).setFlags( nvrhi::rt::GeometryFlags::Opaque );

	nvrhi::rt::AccelStructDesc blasDesc;
	blasDesc.isTopLevel = false;
	blasDesc.addBottomLevelGeometry( geom );
	blasDesc.buildFlags = nvrhi::rt::AccelStructBuildFlags::PreferFastTrace;
	blasDesc.debugName = "DDGI/BLAS";

	nvrhi::rt::AccelStructHandle blas = m_Device->createAccelStruct( blasDesc );
	commandList->buildBottomLevelAccelStruct( blas, &geom, 1 );

	m_BlasCache[vbHandle] = blas;
	return blas;
}

/*
========================
DdgiAccelStructures::RebuildFromView
========================
*/
bool DdgiAccelStructures::RebuildFromView( nvrhi::ICommandList* commandList, const viewDef_t* viewDef )
{
	std::vector<nvrhi::rt::InstanceDesc> instances;
	std::vector<DdgiInstanceData> instanceData;
	instances.reserve( viewDef->numDrawSurfs );
	instanceData.reserve( viewDef->numDrawSurfs );

	for( int i = 0; i < viewDef->numDrawSurfs; i++ )
	{
		const drawSurf_t* surf = viewDef->drawSurfs[i];
		if( surf == NULL || surf->numIndexes <= 0 || surf->frontEndGeo == NULL || surf->space == NULL )
		{
			continue;
		}

		// M1: static world geometry only. Dynamic/skinned surfaces live in the
		// per-frame cache and change every frame - handled in a later milestone.
		if( !vertexCache.CacheIsStatic( surf->ambientCache ) || !vertexCache.CacheIsStatic( surf->indexCache ) )
		{
			continue;
		}

		nvrhi::rt::IAccelStruct* blas = GetOrBuildBottomLevel( commandList, surf );
		if( blas == NULL )
		{
			continue;
		}

		nvrhi::rt::InstanceDesc instance;
		instance.setBLAS( blas );
		instance.setInstanceMask( 0xFF );
		instance.setInstanceID( static_cast<uint32_t>( instances.size() ) );

		nvrhi::rt::AffineTransform xform;
		R_ModelMatrixToAffine( surf->space->modelMatrix, xform );
		instance.setTransform( xform );

		instances.push_back( instance );

		// Parallel per-instance shading data (InstanceID == index). Offsets are
		// byte offsets into the shared static cache, matching GetOrBuildBottomLevel.
		DdgiInstanceData data;
		data.vertexByteOffset = static_cast<uint32_t>( surf->ambientCache >> VERTCACHE_OFFSET_SHIFT ) & VERTCACHE_OFFSET_MASK;
		data.indexByteOffset = static_cast<uint32_t>( surf->indexCache >> VERTCACHE_OFFSET_SHIFT ) & VERTCACHE_OFFSET_MASK;
		data.pad0 = 0;
		data.pad1 = 0;
		const idVec3 albedo = DDGI_MaterialAverageAlbedo( surf->material );
		data.albedo[0] = albedo.x;
		data.albedo[1] = albedo.y;
		data.albedo[2] = albedo.z;
		data.albedo[3] = 0.0f;
		instanceData.push_back( data );
	}

	m_NumInstances = static_cast<int>( instances.size() );
	if( instances.empty() )
	{
		return false;
	}

	// (Re)create the per-instance data buffer when it needs to grow, then upload
	// this frame's records. Bound as a StructuredBuffer SRV to the trace shader.
	if( !m_InstanceDataBuffer || instanceData.size() > m_InstanceDataCapacity )
	{
		m_InstanceDataCapacity = instanceData.size();

		nvrhi::BufferDesc dataDesc;
		dataDesc.byteSize = m_InstanceDataCapacity * sizeof( DdgiInstanceData );
		dataDesc.structStride = sizeof( DdgiInstanceData );
		dataDesc.initialState = nvrhi::ResourceStates::ShaderResource;
		dataDesc.keepInitialState = true;	// seed state tracking; writeBuffer transitions to CopyDest and back
		dataDesc.debugName = "DDGI/InstanceData";
		m_InstanceDataBuffer = m_Device->createBuffer( dataDesc );
	}
	commandList->writeBuffer( m_InstanceDataBuffer, instanceData.data(), instanceData.size() * sizeof( DdgiInstanceData ) );

	// (Re)create the TLAS only when it needs to grow to fit the instance count.
	if( !m_Tlas || instances.size() > m_TlasCapacity )
	{
		m_TlasCapacity = instances.size();

		nvrhi::rt::AccelStructDesc tlasDesc;
		tlasDesc.isTopLevel = true;
		tlasDesc.topLevelMaxInstances = m_TlasCapacity;
		tlasDesc.buildFlags = nvrhi::rt::AccelStructBuildFlags::PreferFastTrace;
		tlasDesc.debugName = "DDGI/TLAS";
		m_Tlas = m_Device->createAccelStruct( tlasDesc );
	}

	commandList->buildTopLevelAccelStruct( m_Tlas, instances.data(), instances.size() );
	return true;
}
