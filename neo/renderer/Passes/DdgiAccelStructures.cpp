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
#include "renderer/RenderWorld_local.h"

#include "DdgiAccelStructures.h"

// Widen the RT occluder set beyond the view frustum so shadows / reflections stop
// popping as portal-area visibility flips. Shared by every RT pass (DDGI, reflections,
// shadows) since they all build through RebuildFromView.
// DEFAULT OFF: the gather walks the LIVE render world (portalAreas / entityRefs /
// entity models) from the backend RebuildFromView, which races the frontend and
// segfaults sporadically (crash was in AppendStaticAreaOccluders). Every other RT
// pass reads only the frozen viewDef snapshot for this reason. Re-enable once the
// gather is moved into the frontend (safe world access) - see rt-shadows notes.
idCVar r_rtWorldOccluders( "r_rtWorldOccluders", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "RT accel struct: also gather static world geometry from the camera's BSP area + portal-connected neighbours, not just the visible frustum (fixes shadow/reflection area pop). UNSTABLE: backend walks the live world - crashes; needs a frontend redesign" );
idCVar r_rtOccluderAreaHops( "r_rtOccluderAreaHops", "8", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "RT accel struct: how many portal hops out from the camera area to gather static occluders (r_rtWorldOccluders)", 0, 64 );

DdgiAccelStructures::DdgiAccelStructures()
	: m_TlasCapacity( 0 )
	, m_NumInstances( 0 )
	, m_InstanceDataCapacity( 0 )
	, m_SkinBoundJoints( nullptr )
	, m_SkinBoundVertex( nullptr )
	, m_SkinPipelineTried( false )
	, m_PosedCapacityVerts( 0 )
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

Uses the material's fast-path diffuse image average colour (decoded from its 1x1
mip at load time), falling back to mid-grey when there is no diffuse image.
========================
*/
static idVec3 DDGI_MaterialAverageAlbedo( const idMaterial* material )
{
	if( material != NULL )
	{
		idImage* diffuse = material->GetFastPathDiffuseImage();
		if( diffuse != NULL )
		{
			const idVec4& c = diffuse->GetAverageColor();
			return idVec3( c.x, c.y, c.z );
		}
	}
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

	m_SkinnedBlas.clear();
	m_PosedBuffer = nullptr;
	m_PosedCapacityVerts = 0;
	m_SkinBindingSet = nullptr;
	m_SkinBoundJoints = nullptr;
	m_SkinBoundVertex = nullptr;
}

// Layout must match SkinConstants in builtin/reflections/skin_positions.cs.hlsl.
struct DdgiSkinConstants
{
	uint32_t	vertexByteOffset;
	uint32_t	jointFloat4Base;
	uint32_t	numVerts;
	uint32_t	outVertBase;
};

/*
========================
DdgiAccelStructures::EnsureSkinPipeline

Lazily builds the GPU-skinning compute pipeline used to pose animated actors into
m_PosedBuffer for ray tracing. Tried once; disabled silently if the shader is
missing so static reflections keep working.
========================
*/
void DdgiAccelStructures::EnsureSkinPipeline()
{
	if( m_SkinPipelineTried )
	{
		return;
	}
	m_SkinPipelineTried = true;

	idList<shaderMacro_t> macros;
	const int shaderIdx = renderProgManager.FindShader( "builtin/reflections/skin_positions", SHADER_STAGE_COMPUTE, "", macros, true, LAYOUT_DRAW_VERT );
	m_SkinShader = renderProgManager.GetShader( shaderIdx );
	if( m_SkinShader == nullptr )
	{
		common->Warning( "DdgiAccelStructures: skin_positions compute shader failed to load - animated actors will not reflect." );
		return;
	}

	nvrhi::BindingLayoutDesc layoutDesc;
	layoutDesc.visibility = nvrhi::ShaderType::Compute;
	layoutDesc.bindings =
	{
		nvrhi::BindingLayoutItem::RawBuffer_SRV( 0 ),			// t0 : bind-pose vertex cache
		nvrhi::BindingLayoutItem::StructuredBuffer_SRV( 1 ),	// t1 : joint matrices
		nvrhi::BindingLayoutItem::VolatileConstantBuffer( 0 ),	// b0 : per-surface skin constants
		nvrhi::BindingLayoutItem::RawBuffer_UAV( 0 ),			// u0 : posed positions out
	};
	m_SkinBindingLayout = m_Device->createBindingLayout( layoutDesc );

	nvrhi::ComputePipelineDesc pipelineDesc;
	pipelineDesc.bindingLayouts = { m_SkinBindingLayout };
	pipelineDesc.CS = m_SkinShader;
	m_SkinPipeline = m_Device->createComputePipeline( pipelineDesc );

	nvrhi::BufferDesc cbDesc;
	cbDesc.byteSize = sizeof( DdgiSkinConstants );
	cbDesc.debugName = "DDGI/SkinConstants";
	cbDesc.isConstantBuffer = true;
	cbDesc.isVolatile = true;
	cbDesc.maxVersions = 1024;	// one version per skinned surface per frame
	m_SkinConstantBuffer = m_Device->createBuffer( cbDesc );
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
nvrhi::rt::IAccelStruct* DdgiAccelStructures::GetOrBuildBottomLevel( nvrhi::ICommandList* commandList,
		vertCacheHandle_t vbHandle, vertCacheHandle_t ibHandle, int numVerts, int numIndexes )
{
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
	.setVertexCount( numVerts )
	.setIndexBuffer( indexBuffer->GetAPIObject() )
	.setIndexFormat( nvrhi::Format::R16_UINT )
	.setIndexOffset( indexOffset )
	.setIndexCount( numIndexes );

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
DdgiAccelStructures::BuildSkinnedInstances

GPU-skins every animated (jointCache != 0) surface in the view into m_PosedBuffer,
builds a BLAS per surface from the posed positions, and appends a TLAS instance
(with the entity's model->world transform) so animated actors reflect in their
current pose. v1 scope: the bind-pose vertices and indices must be in the static
cache (the common GPU-skinning case) - only the joint matrices are per-frame.
========================
*/
void DdgiAccelStructures::BuildSkinnedInstances( nvrhi::ICommandList* commandList, const viewDef_t* viewDef,
		std::vector<nvrhi::rt::InstanceDesc>& instances,
		std::vector<DdgiInstanceData>& instanceData )
{
	m_PosedRanges.clear();

	EnsureSkinPipeline();
	if( m_SkinPipeline == nullptr )
	{
		return;
	}

	struct SkinnedSurf
	{
		const drawSurf_t* surf;
		uint32_t vertByteOffset;
		uint32_t jointFloat4Base;
		uint32_t numVerts;
		uint32_t outVertBase;
	};
	std::vector<SkinnedSurf> skinned;
	uint32_t totalVerts = 0;
	const uint64 expectFrame = ( vertexCache.currentFrame - 1 ) & VERTCACHE_FRAME_MASK;

	for( int i = 0; i < viewDef->numDrawSurfs; i++ )
	{
		const drawSurf_t* surf = viewDef->drawSurfs[i];
		if( surf == NULL || surf->jointCache == 0 || surf->numIndexes <= 0 || surf->frontEndGeo == NULL || surf->space == NULL )
		{
			continue;
		}
		// v1: bind pose + topology in the static cache; only the joints animate
		if( !vertexCache.CacheIsStatic( surf->ambientCache ) || !vertexCache.CacheIsStatic( surf->indexCache ) )
		{
			continue;
		}
		if( vertexCache.CacheIsStatic( surf->jointCache ) )
		{
			continue;	// static joints: nothing to animate, handled by the static path
		}
		const uint64 jframe = ( surf->jointCache >> VERTCACHE_FRAME_SHIFT ) & VERTCACHE_FRAME_MASK;
		if( jframe != expectFrame )
		{
			continue;	// joints from a stale frame
		}

		SkinnedSurf s;
		s.surf = surf;
		s.vertByteOffset = static_cast<uint32_t>( surf->ambientCache >> VERTCACHE_OFFSET_SHIFT ) & VERTCACHE_OFFSET_MASK;
		s.jointFloat4Base = ( static_cast<uint32_t>( surf->jointCache >> VERTCACHE_OFFSET_SHIFT ) & VERTCACHE_OFFSET_MASK ) / 16u;
		s.numVerts = static_cast<uint32_t>( surf->frontEndGeo->numVerts );
		s.outVertBase = totalVerts;
		totalVerts += s.numVerts;
		skinned.push_back( s );

		// record the posed range so the ember pass can seed from this surface
		PosedRange pr;
		pr.entityIndex = ( surf->space->entityDef != NULL ) ? surf->space->entityDef->index : -1;
		pr.outVertBase = s.outVertBase;
		pr.numVerts = s.numVerts;
		memcpy( pr.modelMatrix, surf->space->modelMatrix, sizeof( pr.modelMatrix ) );
		m_PosedRanges.push_back( pr );
	}

	if( skinned.empty() )
	{
		return;
	}

	static bool loggedSkin = false;
	if( !loggedSkin )
	{
		common->Printf( "DdgiAccelStructures: skinning %i animated surface(s), %i verts for RT reflections.\n", ( int )skinned.size(), ( int )totalVerts );
		loggedSkin = true;
	}

	// grow the posed-position pool (float3 per vertex) with headroom
	if( !m_PosedBuffer || totalVerts > m_PosedCapacityVerts )
	{
		m_PosedCapacityVerts = ( size_t )totalVerts + totalVerts / 2 + 4096;

		nvrhi::BufferDesc pd;
		pd.byteSize = m_PosedCapacityVerts * 12;	// float3
		pd.canHaveUAVs = true;
		pd.canHaveRawViews = true;
		pd.isAccelStructBuildInput = true;
		pd.initialState = nvrhi::ResourceStates::UnorderedAccess;
		pd.keepInitialState = true;
		pd.debugName = "DDGI/PosedPositions";
		m_PosedBuffer = m_Device->createBuffer( pd );
		m_SkinBindingSet = nullptr;
	}

	nvrhi::IBuffer* jointBuf = vertexCache.frameData[vertexCache.drawListNum].jointBuffer.GetAPIObject();
	nvrhi::IBuffer* vertBuf = vertexCache.staticData.vertexBuffer.GetAPIObject();
	if( jointBuf == NULL || vertBuf == NULL )
	{
		return;
	}

	// the frame joint buffer rotates, so rebuild the binding set when it changes
	if( m_SkinBindingSet == nullptr || m_SkinBoundJoints != jointBuf || m_SkinBoundVertex != vertBuf )
	{
		nvrhi::BindingSetDesc sd;
		sd.bindings =
		{
			nvrhi::BindingSetItem::RawBuffer_SRV( 0, vertBuf ),
			nvrhi::BindingSetItem::StructuredBuffer_SRV( 1, jointBuf ),
			nvrhi::BindingSetItem::ConstantBuffer( 0, m_SkinConstantBuffer ),
			nvrhi::BindingSetItem::RawBuffer_UAV( 0, m_PosedBuffer ),
		};
		m_SkinBindingSet = m_Device->createBindingSet( sd, m_SkinBindingLayout );
		m_SkinBoundJoints = jointBuf;
		m_SkinBoundVertex = vertBuf;
	}

	// pass 1: skin every surface into the posed buffer
	for( size_t k = 0; k < skinned.size(); k++ )
	{
		DdgiSkinConstants c;
		c.vertexByteOffset = skinned[k].vertByteOffset;
		c.jointFloat4Base = skinned[k].jointFloat4Base;
		c.numVerts = skinned[k].numVerts;
		c.outVertBase = skinned[k].outVertBase;
		commandList->writeBuffer( m_SkinConstantBuffer, &c, sizeof( c ) );

		nvrhi::ComputeState st;
		st.pipeline = m_SkinPipeline;
		st.bindings = { m_SkinBindingSet };
		commandList->setComputeState( st );
		commandList->dispatch( ( skinned[k].numVerts + 63 ) / 64, 1, 1 );
	}

	// pass 2: build a BLAS per surface from the posed positions + static indices, and
	// add the TLAS instance. BLAS handles are recreated each frame (positions change);
	// nvrhi transitions the posed buffer UAV -> accel-struct-build-input for the builds.
	nvrhi::IBuffer* indexBuf = vertexCache.staticData.indexBuffer.GetAPIObject();
	m_SkinnedBlas.clear();
	m_SkinnedBlas.reserve( skinned.size() );

	for( size_t k = 0; k < skinned.size(); k++ )
	{
		const drawSurf_t* surf = skinned[k].surf;
		const uint32_t indexOffset = static_cast<uint32_t>( surf->indexCache >> VERTCACHE_OFFSET_SHIFT ) & VERTCACHE_OFFSET_MASK;

		nvrhi::rt::GeometryTriangles tris;
		tris.setVertexBuffer( m_PosedBuffer )
		.setVertexFormat( nvrhi::Format::RGB32_FLOAT )
		.setVertexOffset( ( uint64_t )skinned[k].outVertBase * 12 )
		.setVertexStride( 12 )
		.setVertexCount( skinned[k].numVerts )
		.setIndexBuffer( indexBuf )
		.setIndexFormat( nvrhi::Format::R16_UINT )
		.setIndexOffset( indexOffset )
		.setIndexCount( surf->numIndexes );

		nvrhi::rt::GeometryDesc geom;
		geom.setTriangles( tris ).setFlags( nvrhi::rt::GeometryFlags::Opaque );

		nvrhi::rt::AccelStructDesc bd;
		bd.isTopLevel = false;
		bd.addBottomLevelGeometry( geom );
		bd.buildFlags = nvrhi::rt::AccelStructBuildFlags::PreferFastBuild;
		bd.debugName = "DDGI/SkinnedBLAS";
		nvrhi::rt::AccelStructHandle blas = m_Device->createAccelStruct( bd );
		commandList->buildBottomLevelAccelStruct( blas, &geom, 1 );
		m_SkinnedBlas.push_back( blas );

		nvrhi::rt::InstanceDesc inst;
		inst.setBLAS( blas );
		inst.setInstanceMask( 0xFF );
		inst.setInstanceID( static_cast<uint32_t>( instances.size() ) );

		nvrhi::rt::AffineTransform xform;
		R_ModelMatrixToAffine( surf->space->modelMatrix, xform );
		inst.setTransform( xform );
		instances.push_back( inst );

		// per-instance data: the reflection trace only needs the hit position, not the
		// posed geometry, so the fetch offsets are unused here (kept for DDGI parity).
		DdgiInstanceData data;
		data.vertexByteOffset = 0;
		data.indexByteOffset = 0;
		data.pad0 = 0;
		data.pad1 = 0;
		const idVec3 albedo = DDGI_MaterialAverageAlbedo( surf->material );
		data.albedo[0] = albedo.x;
		data.albedo[1] = albedo.y;
		data.albedo[2] = albedo.z;
		data.albedo[3] = 0.0f;
		instanceData.push_back( data );
	}
}

/*
========================
DdgiAccelStructures::RebuildFromView
========================
*/
/*
========================
DdgiAccelStructures::AppendInstance

Push one TLAS instance + its parallel shading record for a static surface.
========================
*/
void DdgiAccelStructures::AppendInstance( std::vector<nvrhi::rt::InstanceDesc>& instances,
		std::vector<DdgiInstanceData>& instanceData, nvrhi::rt::IAccelStruct* blas,
		const float* modelMatrix, vertCacheHandle_t ambientCache, vertCacheHandle_t indexCache,
		const idMaterial* material )
{
	nvrhi::rt::InstanceDesc instance;
	instance.setBLAS( blas );
	instance.setInstanceMask( 0xFF );
	instance.setInstanceID( static_cast<uint32_t>( instances.size() ) );

	nvrhi::rt::AffineTransform xform;
	R_ModelMatrixToAffine( modelMatrix, xform );
	instance.setTransform( xform );

	instances.push_back( instance );

	// Parallel per-instance shading data (InstanceID == index). Offsets are byte
	// offsets into the shared static cache, matching GetOrBuildBottomLevel.
	DdgiInstanceData data;
	data.vertexByteOffset = static_cast<uint32_t>( ambientCache >> VERTCACHE_OFFSET_SHIFT ) & VERTCACHE_OFFSET_MASK;
	data.indexByteOffset = static_cast<uint32_t>( indexCache >> VERTCACHE_OFFSET_SHIFT ) & VERTCACHE_OFFSET_MASK;
	data.pad0 = 0;
	data.pad1 = 0;
	const idVec3 albedo = DDGI_MaterialAverageAlbedo( material );
	data.albedo[0] = albedo.x;
	data.albedo[1] = albedo.y;
	data.albedo[2] = albedo.z;
	data.albedo[3] = 0.0f;
	instanceData.push_back( data );
}

/*
========================
DdgiAccelStructures::AppendStaticAreaOccluders

Gather static world geometry from the camera's BSP area plus its portal-connected
neighbours (hop-capped), independent of the view frustum, so occluders the camera
cannot currently see still cast shadows / appear in reflections instead of popping
as portal-area visibility flips. Deduped against surfaces already added from the view.

Each portal area owns a static "_area%i" world model at entityDefs[i] (i <
numPortalAreas) with an identity transform; we walk those models' surfaces directly.
Surfaces whose static cache is not yet resident (area never rendered) are skipped -
they get picked up the first frame the area is actually drawn, which is exactly when
the pop would otherwise start.
========================
*/
void DdgiAccelStructures::AppendStaticAreaOccluders( nvrhi::ICommandList* commandList, const viewDef_t* viewDef,
		std::vector<nvrhi::rt::InstanceDesc>& instances, std::vector<DdgiInstanceData>& instanceData,
		std::unordered_set<vertCacheHandle_t>& seen )
{
	if( !r_rtWorldOccluders.GetBool() )
	{
		return;
	}

	idRenderWorldLocal* world = viewDef->renderWorld;
	if( world == NULL || world->numPortalAreas <= 0 )
	{
		return;
	}

	// target areas: camera area + portal-connected neighbours up to N hops. Outside the
	// world (areaNum < 0) -> gather everything, mirroring the portal-disabled view path.
	idList<int> areas;
	const int camArea = world->PointInArea( viewDef->renderView.vieworg );
	if( camArea < 0 )
	{
		for( int i = 0; i < world->numPortalAreas; i++ )
		{
			areas.Append( i );
		}
	}
	else
	{
		const int maxHops = r_rtOccluderAreaHops.GetInteger();
		idList<bool> visited;
		visited.AssureSize( world->numPortalAreas, false );
		idList<int> frontier;
		visited[camArea] = true;
		areas.Append( camArea );
		frontier.Append( camArea );

		for( int hop = 0; hop < maxHops && frontier.Num() > 0; hop++ )
		{
			idList<int> next;
			for( int f = 0; f < frontier.Num(); f++ )
			{
				const portalArea_t& pa = world->portalAreas[frontier[f]];
				for( portal_t* p = pa.portals; p != NULL; p = p->next )
				{
					if( ( p->doublePortal->blockingBits & PS_BLOCK_VIEW ) != 0 )
					{
						continue;	// sealed by a closed door - do not gather behind it
					}
					const int na = p->intoArea;
					if( na >= 0 && na < world->numPortalAreas && !visited[na] )
					{
						visited[na] = true;
						areas.Append( na );
						next.Append( na );
					}
				}
			}
			frontier = next;
		}
	}

	for( int a = 0; a < areas.Num(); a++ )
	{
		const portalArea_t& pa = world->portalAreas[areas[a]];
		for( areaReference_t* ref = pa.entityRefs.areaNext; ref != &pa.entityRefs; ref = ref->areaNext )
		{
			idRenderEntityLocal* ent = ref->entity;
			// only the static per-area world models (entityDefs[0 .. numPortalAreas));
			// movable entities are dynamic and handled by the view / skinned paths.
			if( ent == NULL || ent->index >= world->numPortalAreas )
			{
				continue;
			}
			const idRenderModel* model = ent->parms.hModel;
			if( model == NULL )
			{
				continue;
			}

			const int numSurfaces = model->NumSurfaces();
			for( int s = 0; s < numSurfaces; s++ )
			{
				const modelSurface_t* msurf = model->Surface( s );
				if( msurf == NULL || msurf->geometry == NULL )
				{
					continue;
				}
				const srfTriangles_t* tri = msurf->geometry;
				if( tri->numIndexes <= 0 )
				{
					continue;
				}
				if( !vertexCache.CacheIsStatic( tri->ambientCache ) || !vertexCache.CacheIsStatic( tri->indexCache ) )
				{
					continue;
				}
				if( seen.find( tri->ambientCache ) != seen.end() )
				{
					continue;
				}
				seen.insert( tri->ambientCache );

				nvrhi::rt::IAccelStruct* blas = GetOrBuildBottomLevel( commandList, tri->ambientCache, tri->indexCache, tri->numVerts, tri->numIndexes );
				if( blas == NULL )
				{
					continue;
				}
				AppendInstance( instances, instanceData, blas, ent->modelMatrix, tri->ambientCache, tri->indexCache, msurf->shader );
			}
		}
	}
}

bool DdgiAccelStructures::RebuildFromView( nvrhi::ICommandList* commandList, const viewDef_t* viewDef )
{
	std::vector<nvrhi::rt::InstanceDesc> instances;
	std::vector<DdgiInstanceData> instanceData;
	instances.reserve( viewDef->numDrawSurfs );
	instanceData.reserve( viewDef->numDrawSurfs );

	// static surfaces already added (keyed by ambientCache) so the wider area gather
	// below does not double-add the geometry the view already contributed.
	std::unordered_set<vertCacheHandle_t> seen;

	for( int i = 0; i < viewDef->numDrawSurfs; i++ )
	{
		const drawSurf_t* surf = viewDef->drawSurfs[i];
		if( surf == NULL || surf->numIndexes <= 0 || surf->frontEndGeo == NULL || surf->space == NULL )
		{
			continue;
		}

		// Animated (GPU-skinned) surfaces are handled by BuildSkinnedInstances, which
		// poses their vertices each frame; skip them here so they are not added in
		// their bind pose.
		if( surf->jointCache != 0 )
		{
			continue;
		}

		// static world geometry only in this loop. Non-skinned dynamic-cache surfaces
		// (deforms, particles) are left for a later milestone.
		if( !vertexCache.CacheIsStatic( surf->ambientCache ) || !vertexCache.CacheIsStatic( surf->indexCache ) )
		{
			continue;
		}

		nvrhi::rt::IAccelStruct* blas = GetOrBuildBottomLevel( commandList, surf->ambientCache, surf->indexCache, surf->frontEndGeo->numVerts, surf->numIndexes );
		if( blas == NULL )
		{
			continue;
		}

		AppendInstance( instances, instanceData, blas, surf->space->modelMatrix, surf->ambientCache, surf->indexCache, surf->material );
		seen.insert( surf->ambientCache );
	}

	// Widen beyond the frustum: gather static world occluders from the camera area +
	// portal-connected neighbours so off-screen geometry stops popping in shadows /
	// reflections. Deduped against the view surfaces above.
	AppendStaticAreaOccluders( commandList, viewDef, instances, instanceData, seen );

	// animated actors: skin + build BLAS per skinned surface and append instances
	BuildSkinnedInstances( commandList, viewDef, instances, instanceData );

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
