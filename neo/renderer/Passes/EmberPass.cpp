/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 2026 Aleksandr Petrosyan

This program is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version. See <http://www.gnu.org/licenses/>.

===========================================================================
*/

#include <precompiled.h>
#pragma hdrstop

#include <unordered_set>
#include <unordered_map>

#include "renderer/RenderCommon.h"

#include "EmberPass.h"
#include "DdgiAccelStructures.h"

// ---------------------------------------------------------------------------
// Tuning cvars. Live-editable so the motion / look can be dialled in at runtime.
// ---------------------------------------------------------------------------
idCVar r_emberDissolve( "r_emberDissolve", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "GPU mesh-seeded ember death dissolve (shelved: too subtle)" );
idCVar r_emberCount( "r_emberCount", "4000", CVAR_RENDERER | CVAR_INTEGER, "max embers seeded from a dying mesh" );
idCVar r_emberLifetime( "r_emberLifetime", "2.4", CVAR_RENDERER | CVAR_FLOAT, "ember life after it detaches (seconds)" );
idCVar r_emberEmitSpread( "r_emberEmitSpread", "0.55", CVAR_RENDERER | CVAR_FLOAT, "how long the dissolve sweeps across the body (seconds)" );
idCVar r_emberSize( "r_emberSize", "0.22", CVAR_RENDERER | CVAR_FLOAT, "ember sprite radius (world units)" );
idCVar r_emberGrowth( "r_emberGrowth", "0.4", CVAR_RENDERER | CVAR_FLOAT, "ember sprite growth over life" );
idCVar r_emberIntensity( "r_emberIntensity", "1.3", CVAR_RENDERER | CVAR_FLOAT, "ember emissive intensity (HDR)" );
idCVar r_emberBuoyancy( "r_emberBuoyancy", "70.0", CVAR_RENDERER | CVAR_FLOAT, "ember upward accel (world +Z)" );
idCVar r_emberGravity( "r_emberGravity", "26.0", CVAR_RENDERER | CVAR_FLOAT, "ember downward accel (opposes buoyancy)" );
idCVar r_emberDrag( "r_emberDrag", "0.7", CVAR_RENDERER | CVAR_FLOAT, "ember velocity damping per second" );
idCVar r_emberInitialSpeed( "r_emberInitialSpeed", "22.0", CVAR_RENDERER | CVAR_FLOAT, "gentle creep accel during the slow start" );
idCVar r_emberSlowStart( "r_emberSlowStart", "0.28", CVAR_RENDERER | CVAR_FLOAT, "slow-start ramp duration (seconds)" );
idCVar r_emberAfterblow( "r_emberAfterblow", "40.0", CVAR_RENDERER | CVAR_FLOAT, "continued push along the blast after the initial rip" );
idCVar r_emberAfterblowStart( "r_emberAfterblowStart", "0.32", CVAR_RENDERER | CVAR_FLOAT, "delay before the afterblow kicks in (seconds)" );
idCVar r_emberSubsteps( "r_emberSubsteps", "4", CVAR_RENDERER | CVAR_INTEGER, "physics sub-steps per frame (higher tick rate)" );
idCVar r_emberDirSpread( "r_emberDirSpread", "0.35", CVAR_RENDERER | CVAR_FLOAT, "afterblow direction cone half-spread" );
// impact-localized seeding: only mesh verts near a hit become embers
idCVar r_emberImpactRadius( "r_emberImpactRadius", "16.0", CVAR_RENDERER | CVAR_FLOAT, "model-space radius around an impact that seeds embers" );
idCVar r_emberBlowSpeed( "r_emberBlowSpeed", "150.0", CVAR_RENDERER | CVAR_FLOAT, "initial blast speed of embers off the impact" );
idCVar r_emberOutward( "r_emberOutward", "1.0", CVAR_RENDERER | CVAR_FLOAT, "how much embers blow outward from the impact point" );
idCVar r_emberDirBlow( "r_emberDirBlow", "0.6", CVAR_RENDERER | CVAR_FLOAT, "how much embers follow the killing-blow direction" );
idCVar r_emberDebug( "r_emberDebug", "0", CVAR_RENDERER | CVAR_BOOL, "log ember seeding" );

namespace
{

// GPU ember particle - must match the Ember struct in the ember shaders (48 bytes).
struct EmberGPU
{
	float	pos[3];		float	age;
	float	vel[3];		float	emitDelay;
	float	dir[3];		float	seed;
};

struct SeedCB
{
	float		model0[4], model1[4], model2[4], model3[4];	// model->world (id column-major)
	float		dir[3];		float	impactRadius;			// world blow dir + model-space gate radius
	uint32_t	posedVertBase, numSrcVerts, capacity, slotBase;
	uint32_t	numImpacts;	float	blowSpeed, outwardBlow, dirBlow;
	uint32_t	seedSalt;	float	emitJitter, pad0, pad1;
	float		impacts[8][4];	// model-space impact origins (xyz)
};

struct ClearCB
{
	uint32_t	slotBase, count, pad0, pad1;
};

struct SimCB
{
	float		dt;			uint32_t	substeps;	uint32_t	count;	float	buoyancy;
	float		drag, afterblowStart, afterblowAccel, initialSpeed;
	float		slowStartT, lifetime, gravity;	uint32_t	slotBase;
};

struct RenderCB
{
	float		vp0[4], vp1[4], vp2[4], vp3[4];		// world->clip (id row-major rows)
	float		right[3];		float	spriteSize;
	float		up[3];			float	lifetime;
	float		colorWarm[3];	float	intensity;
	float		colorCool[3];	float	growth;
	uint32_t	slotBase;		float	pad[3];
};

// Warm ember colours (multiplied by intensity on the GPU).
const float EMBER_WARM[3] = { 1.0f, 0.55f, 0.16f };
const float EMBER_COOL[3] = { 0.55f, 0.10f, 0.03f };

const uint32_t EMBER_POOL = 131072;	// total particle slots (shared ring)

// Side channel for per-hit impact points (kept out of renderEntity_t so that
// struct's size stays a multiple of 16). Keyed by render-entity handle.
struct EmberImpactSet
{
	int		num;
	idVec3	pts[ MAX_ENTITY_EMBER_IMPACTS ];
};
std::unordered_map<int, EmberImpactSet> g_emberImpacts;

} // anonymous namespace

/*
========================
R_RegisterEmberImpacts

Called from idAI::Killed with the recent MODEL-space wound points.
========================
*/
void R_RegisterEmberImpacts( int renderEntityHandle, const idVec3* impacts, int numImpacts )
{
	if( renderEntityHandle < 0 || impacts == NULL || numImpacts <= 0 )
	{
		return;
	}
	EmberImpactSet set;
	set.num = Min( numImpacts, MAX_ENTITY_EMBER_IMPACTS );
	for( int i = 0; i < set.num; i++ )
	{
		set.pts[i] = impacts[i];
	}
	g_emberImpacts[ renderEntityHandle ] = set;
}

/*
========================
EmberPass::EmberPass
========================
*/
EmberPass::EmberPass( nvrhi::IDevice* device )
	: m_Device( device )
	, m_Cursor( 0 )
	, m_SeedBoundPosed( nullptr )
	, m_PipelinesTried( false )
	, m_PipelinesOK( false )
	, m_LastTimeMs( 0 )
{
}

EmberPass::~EmberPass()
{
}

/*
========================
EmberPass::EnsureComputePipelines
========================
*/
void EmberPass::EnsureComputePipelines()
{
	if( m_PipelinesTried )
	{
		return;
	}
	m_PipelinesTried = true;

	idList<shaderMacro_t> macros;
	m_ClearShader = renderProgManager.GetShader( renderProgManager.FindShader( "builtin/embers/ember_clear", SHADER_STAGE_COMPUTE, "", macros, true, LAYOUT_DRAW_VERT ) );
	m_SeedShader = renderProgManager.GetShader( renderProgManager.FindShader( "builtin/embers/ember_seed", SHADER_STAGE_COMPUTE, "", macros, true, LAYOUT_DRAW_VERT ) );
	m_SimShader = renderProgManager.GetShader( renderProgManager.FindShader( "builtin/embers/ember_sim", SHADER_STAGE_COMPUTE, "", macros, true, LAYOUT_DRAW_VERT ) );
	m_RenderVS = renderProgManager.GetShader( renderProgManager.FindShader( "builtin/embers/ember_render", SHADER_STAGE_VERTEX, "", macros, true, LAYOUT_DRAW_VERT ) );
	m_RenderPS = renderProgManager.GetShader( renderProgManager.FindShader( "builtin/embers/ember_render", SHADER_STAGE_FRAGMENT, "", macros, true, LAYOUT_DRAW_VERT ) );

	if( m_ClearShader == nullptr || m_SeedShader == nullptr || m_SimShader == nullptr || m_RenderVS == nullptr || m_RenderPS == nullptr )
	{
		common->Warning( "EmberPass: ember shaders failed to load - death dissolve disabled." );
		return;
	}

	// persistent particle pool
	nvrhi::BufferDesc pd;
	pd.byteSize = ( uint64_t )EMBER_POOL * sizeof( EmberGPU );
	pd.structStride = sizeof( EmberGPU );
	pd.canHaveUAVs = true;
	pd.initialState = nvrhi::ResourceStates::UnorderedAccess;
	pd.keepInitialState = true;
	pd.debugName = "Embers/Pool";
	m_EmberBuffer = m_Device->createBuffer( pd );

	// append cursor (one uint, reset per seed)
	nvrhi::BufferDesc cd;
	cd.byteSize = sizeof( uint32_t ) * 4;
	cd.structStride = sizeof( uint32_t );
	cd.canHaveUAVs = true;
	cd.initialState = nvrhi::ResourceStates::UnorderedAccess;
	cd.keepInitialState = true;
	cd.debugName = "Embers/Counter";
	m_CounterBuffer = m_Device->createBuffer( cd );

	// --- clear pipeline ---
	{
		nvrhi::BindingLayoutDesc ld;
		ld.visibility = nvrhi::ShaderType::Compute;
		ld.bindings =
		{
			nvrhi::BindingLayoutItem::VolatileConstantBuffer( 0 ),	// b0 : clear range
			nvrhi::BindingLayoutItem::StructuredBuffer_UAV( 0 ),	// u0 : ember pool
		};
		m_ClearLayout = m_Device->createBindingLayout( ld );

		nvrhi::ComputePipelineDesc pd2;
		pd2.bindingLayouts = { m_ClearLayout };
		pd2.CS = m_ClearShader;
		m_ClearPipeline = m_Device->createComputePipeline( pd2 );

		nvrhi::BufferDesc cb;
		cb.byteSize = sizeof( ClearCB );
		cb.isConstantBuffer = true;
		cb.isVolatile = true;
		cb.maxVersions = 256;
		cb.debugName = "Embers/ClearCB";
		m_ClearCB = m_Device->createBuffer( cb );

		nvrhi::BindingSetDesc sd;
		sd.bindings =
		{
			nvrhi::BindingSetItem::ConstantBuffer( 0, m_ClearCB ),
			nvrhi::BindingSetItem::StructuredBuffer_UAV( 0, m_EmberBuffer ),
		};
		m_ClearSet = m_Device->createBindingSet( sd, m_ClearLayout );
	}

	// --- seed pipeline ---
	{
		nvrhi::BindingLayoutDesc ld;
		ld.visibility = nvrhi::ShaderType::Compute;
		ld.bindings =
		{
			nvrhi::BindingLayoutItem::RawBuffer_SRV( 0 ),			// t0 : posed positions
			nvrhi::BindingLayoutItem::VolatileConstantBuffer( 0 ),	// b0 : seed constants
			nvrhi::BindingLayoutItem::StructuredBuffer_UAV( 0 ),	// u0 : ember pool
			nvrhi::BindingLayoutItem::StructuredBuffer_UAV( 1 ),	// u1 : append counter
		};
		m_SeedLayout = m_Device->createBindingLayout( ld );

		nvrhi::ComputePipelineDesc pd2;
		pd2.bindingLayouts = { m_SeedLayout };
		pd2.CS = m_SeedShader;
		m_SeedPipeline = m_Device->createComputePipeline( pd2 );

		nvrhi::BufferDesc cb;
		cb.byteSize = sizeof( SeedCB );
		cb.isConstantBuffer = true;
		cb.isVolatile = true;
		cb.maxVersions = 256;
		cb.debugName = "Embers/SeedCB";
		m_SeedCB = m_Device->createBuffer( cb );
	}

	// --- sim pipeline ---
	{
		nvrhi::BindingLayoutDesc ld;
		ld.visibility = nvrhi::ShaderType::Compute;
		ld.bindings =
		{
			nvrhi::BindingLayoutItem::VolatileConstantBuffer( 0 ),	// b0 : sim constants
			nvrhi::BindingLayoutItem::StructuredBuffer_UAV( 0 ),	// u0 : ember pool
		};
		m_SimLayout = m_Device->createBindingLayout( ld );

		nvrhi::ComputePipelineDesc pd2;
		pd2.bindingLayouts = { m_SimLayout };
		pd2.CS = m_SimShader;
		m_SimPipeline = m_Device->createComputePipeline( pd2 );

		nvrhi::BufferDesc cb;
		cb.byteSize = sizeof( SimCB );
		cb.isConstantBuffer = true;
		cb.isVolatile = true;
		cb.maxVersions = 256;
		cb.debugName = "Embers/SimCB";
		m_SimCB = m_Device->createBuffer( cb );

		nvrhi::BindingSetDesc sd;
		sd.bindings =
		{
			nvrhi::BindingSetItem::ConstantBuffer( 0, m_SimCB ),
			nvrhi::BindingSetItem::StructuredBuffer_UAV( 0, m_EmberBuffer ),
		};
		m_SimSet = m_Device->createBindingSet( sd, m_SimLayout );
	}

	// --- render binding layout + constant buffer (pipeline is framebuffer-dependent) ---
	{
		nvrhi::BindingLayoutDesc ld;
		ld.visibility = nvrhi::ShaderType::All;
		ld.bindings =
		{
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV( 0 ),	// t0 : ember pool
			nvrhi::BindingLayoutItem::VolatileConstantBuffer( 0 ),	// b0 : render constants
		};
		m_RenderLayout = m_Device->createBindingLayout( ld );

		nvrhi::BufferDesc cb;
		cb.byteSize = sizeof( RenderCB );
		cb.isConstantBuffer = true;
		cb.isVolatile = true;
		cb.maxVersions = 256;
		cb.debugName = "Embers/RenderCB";
		m_RenderCB = m_Device->createBuffer( cb );

		nvrhi::BindingSetDesc sd;
		sd.bindings =
		{
			nvrhi::BindingSetItem::StructuredBuffer_SRV( 0, m_EmberBuffer ),
			nvrhi::BindingSetItem::ConstantBuffer( 0, m_RenderCB ),
		};
		m_RenderSet = m_Device->createBindingSet( sd, m_RenderLayout );
	}

	m_PipelinesOK = true;
}

/*
========================
EmberPass::EnsureRenderPipeline

The graphics pipeline depends on the target framebuffer's formats, so it is
created lazily on the first draw (and rebuilt if the framebuffer changes).
========================
*/
void EmberPass::EnsureRenderPipeline( nvrhi::IFramebuffer* fb )
{
	if( m_RenderPipeline != nullptr )
	{
		return;
	}

	nvrhi::GraphicsPipelineDesc psoDesc;
	psoDesc.VS = m_RenderVS;
	psoDesc.PS = m_RenderPS;
	psoDesc.primType = nvrhi::PrimitiveType::TriangleList;
	psoDesc.inputLayout = nullptr;
	psoDesc.bindingLayouts = { m_RenderLayout };

	psoDesc.renderState.rasterState.setCullNone();

	auto& ds = psoDesc.renderState.depthStencilState;
	ds.depthTestEnable = true;
	ds.depthWriteEnable = false;
	ds.depthFunc = nvrhi::ComparisonFunc::LessOrEqual;	// engine depth is standard (near = small)
	ds.stencilEnable = false;

	nvrhi::BlendState::RenderTarget bt;
	bt.blendEnable = true;
	bt.srcBlend = nvrhi::BlendFactor::One;
	bt.destBlend = nvrhi::BlendFactor::One;			// additive
	bt.blendOp = nvrhi::BlendOp::Add;
	bt.srcBlendAlpha = nvrhi::BlendFactor::One;
	bt.destBlendAlpha = nvrhi::BlendFactor::One;
	bt.blendOpAlpha = nvrhi::BlendOp::Add;
	psoDesc.renderState.blendState.setRenderTarget( 0, bt );

	m_RenderPipeline = m_Device->createGraphicsPipeline( psoDesc, fb );
}

/*
========================
EmberPass::AlreadySeeded
========================
*/
bool EmberPass::AlreadySeeded( int entityIndex, int startTimeMs ) const
{
	for( size_t i = 0; i < m_Blocks.size(); i++ )
	{
		if( m_Blocks[i].entityIndex == entityIndex && m_Blocks[i].startTimeMs == startTimeMs )
		{
			return true;
		}
	}
	return false;
}

/*
========================
EmberPass::Render
========================
*/
void EmberPass::Render( nvrhi::ICommandList* commandList, const viewDef_t* viewDef, DdgiAccelStructures* accel, nvrhi::IFramebuffer* hdrFramebuffer )
{
	if( !r_emberDissolve.GetBool() )
	{
		m_Blocks.clear();
		return;
	}
	if( viewDef == NULL || viewDef->isSubview )
	{
		return;		// simulate + draw once, on the main view
	}

	EnsureComputePipelines();
	if( !m_PipelinesOK )
	{
		return;
	}

	const int nowMs = viewDef->renderView.time[0];
	float dt = ( m_LastTimeMs == 0 ) ? 0.0f : ( nowMs - m_LastTimeMs ) * 0.001f;
	m_LastTimeMs = nowMs;
	dt = idMath::ClampFloat( 0.0f, 0.1f, dt );

	const float lifetime = Max( 0.1f, r_emberLifetime.GetFloat() );
	const float emitSpread = Max( 0.0f, r_emberEmitSpread.GetFloat() );
	const int windowMs = ( int )( ( lifetime + emitSpread + 0.5f ) * 1000.0f );

	// 1. expire finished blocks
	for( int i = ( int )m_Blocks.size() - 1; i >= 0; i-- )
	{
		if( nowMs - m_Blocks[i].startTimeMs > windowMs )
		{
			m_Blocks.erase( m_Blocks.begin() + i );
		}
	}

	// 2. seed freshly-killed entities from their posed mesh vertices
	if( accel != NULL )
	{
		const std::vector<DdgiAccelStructures::PosedRange>& ranges = accel->GetPosedRanges();
		nvrhi::IBuffer* posed = accel->GetPosedBuffer();

		if( posed != NULL && !ranges.empty() )
		{
			std::unordered_set<int> triggered;
			for( int s = 0; s < viewDef->numDrawSurfs; s++ )
			{
				const drawSurf_t* surf = viewDef->drawSurfs[s];
				if( surf == NULL || surf->space == NULL || surf->space->entityDef == NULL )
				{
					continue;
				}
				const idRenderEntityLocal* ed = surf->space->entityDef;
				const int st = ed->parms.emberStartTime;
				if( st <= 0 || st > nowMs || ( nowMs - st ) > windowMs )
				{
					continue;
				}
				const int idx = ed->index;
				if( triggered.count( idx ) || AlreadySeeded( idx, st ) )
				{
					continue;
				}
				triggered.insert( idx );

				// gather this entity's posed ranges
				std::vector<const DdgiAccelStructures::PosedRange*> mine;
				for( size_t r = 0; r < ranges.size(); r++ )
				{
					if( ranges[r].entityIndex == idx )
					{
						mine.push_back( &ranges[r] );
					}
				}
				if( mine.empty() )
				{
					continue;
				}

				// impact points (model space, registered at Killed) drive where the
				// embers come off; no wound => the body just burns, no embers
				std::unordered_map<int, EmberImpactSet>::iterator itImp = g_emberImpacts.find( idx );
				if( itImp == g_emberImpacts.end() || itImp->second.num <= 0 )
				{
					continue;
				}
				const EmberImpactSet& impSet = itImp->second;
				const int numImpacts = impSet.num;

				idVec3 dir = ed->parms.emberDir;
				if( dir.LengthSqr() < 1e-4f )
				{
					dir.Set( 0, 0, 1 );
				}

				const uint32_t capacity = ( uint32_t )idMath::ClampInt( 64, ( int )EMBER_POOL, r_emberCount.GetInteger() );

				if( m_Cursor + capacity > EMBER_POOL )
				{
					m_Cursor = 0;
				}
				const uint32_t base = m_Cursor;
				m_Cursor += capacity;

				// evict any active block the new allocation overlaps
				for( int b = ( int )m_Blocks.size() - 1; b >= 0; b-- )
				{
					const uint32_t bs = m_Blocks[b].baseSlot;
					const uint32_t be = bs + m_Blocks[b].count;
					if( bs < base + capacity && base < be )
					{
						m_Blocks.erase( m_Blocks.begin() + b );
					}
				}

				// mark the block inactive, then reset the append cursor
				{
					ClearCB cc;
					cc.slotBase = base;
					cc.count = capacity;
					cc.pad0 = cc.pad1 = 0;
					commandList->writeBuffer( m_ClearCB, &cc, sizeof( cc ) );

					nvrhi::ComputeState cs;
					cs.pipeline = m_ClearPipeline;
					cs.bindings = { m_ClearSet };
					commandList->setComputeState( cs );
					commandList->dispatch( ( capacity + 63 ) / 64, 1, 1 );

					const uint32_t zero[4] = { 0, 0, 0, 0 };
					commandList->writeBuffer( m_CounterBuffer, zero, sizeof( zero ) );
				}

				if( m_SeedSet == nullptr || m_SeedBoundPosed != posed )
				{
					nvrhi::BindingSetDesc sd;
					sd.bindings =
					{
						nvrhi::BindingSetItem::RawBuffer_SRV( 0, posed ),
						nvrhi::BindingSetItem::ConstantBuffer( 0, m_SeedCB ),
						nvrhi::BindingSetItem::StructuredBuffer_UAV( 0, m_EmberBuffer ),
						nvrhi::BindingSetItem::StructuredBuffer_UAV( 1, m_CounterBuffer ),
					};
					m_SeedSet = m_Device->createBindingSet( sd, m_SeedLayout );
					m_SeedBoundPosed = posed;
				}

				// append embers from every posed vertex near an impact
				for( size_t m = 0; m < mine.size(); m++ )
				{
					const DdgiAccelStructures::PosedRange* pr = mine[m];

					SeedCB c;
					memset( &c, 0, sizeof( c ) );
					memcpy( c.model0, pr->modelMatrix, 16 * sizeof( float ) );
					c.dir[0] = dir.x;
					c.dir[1] = dir.y;
					c.dir[2] = dir.z;
					c.impactRadius = r_emberImpactRadius.GetFloat();
					c.posedVertBase = pr->outVertBase;
					c.numSrcVerts = pr->numVerts;
					c.capacity = capacity;
					c.slotBase = base;
					c.numImpacts = ( uint32_t )numImpacts;
					c.blowSpeed = r_emberBlowSpeed.GetFloat();
					c.outwardBlow = r_emberOutward.GetFloat();
					c.dirBlow = r_emberDirBlow.GetFloat();
					c.seedSalt = ( uint32_t )st + ( uint32_t )m * 9173u;
					c.emitJitter = 0.08f;
					for( int k = 0; k < numImpacts; k++ )
					{
						c.impacts[k][0] = impSet.pts[k].x;
						c.impacts[k][1] = impSet.pts[k].y;
						c.impacts[k][2] = impSet.pts[k].z;
						c.impacts[k][3] = 0.0f;
					}

					commandList->writeBuffer( m_SeedCB, &c, sizeof( c ) );
					nvrhi::ComputeState cs;
					cs.pipeline = m_SeedPipeline;
					cs.bindings = { m_SeedSet };
					commandList->setComputeState( cs );
					commandList->dispatch( ( pr->numVerts + 63 ) / 64, 1, 1 );
				}

				Block blk;
				blk.entityIndex = idx;
				blk.startTimeMs = st;
				blk.baseSlot = base;
				blk.count = capacity;
				m_Blocks.push_back( blk );

				if( r_emberDebug.GetBool() )
				{
					common->Printf( "EmberPass: SEEDED entity %i, %i impact(s), cap %u, %zu surf\n",
									idx, numImpacts, capacity, mine.size() );
				}
			}
		}
	}
	else if( r_emberDebug.GetBool() )
	{
		common->Printf( "EmberPass: no accel/posed pool this frame (RT reflections or DDGI must be on to seed)\n" );
	}

	if( m_Blocks.empty() )
	{
		return;
	}

	// 3. integrate every active block
	{
		SimCB sc;
		sc.dt = dt;
		sc.substeps = ( uint32_t )Max( 1, r_emberSubsteps.GetInteger() );
		sc.buoyancy = r_emberBuoyancy.GetFloat();
		sc.drag = r_emberDrag.GetFloat();
		sc.afterblowStart = r_emberAfterblowStart.GetFloat();
		sc.afterblowAccel = r_emberAfterblow.GetFloat();
		sc.initialSpeed = r_emberInitialSpeed.GetFloat();
		sc.slowStartT = r_emberSlowStart.GetFloat();
		sc.lifetime = lifetime;
		sc.gravity = r_emberGravity.GetFloat();

		for( size_t b = 0; b < m_Blocks.size(); b++ )
		{
			sc.count = m_Blocks[b].count;
			sc.slotBase = m_Blocks[b].baseSlot;
			commandList->writeBuffer( m_SimCB, &sc, sizeof( sc ) );

			nvrhi::ComputeState cs;
			cs.pipeline = m_SimPipeline;
			cs.bindings = { m_SimSet };
			commandList->setComputeState( cs );
			commandList->dispatch( ( m_Blocks[b].count + 63 ) / 64, 1, 1 );
		}
	}

	// 4. draw embers additively into the HDR scene target
	if( hdrFramebuffer == NULL )
	{
		return;
	}
	EnsureRenderPipeline( hdrFramebuffer );
	if( m_RenderPipeline == nullptr )
	{
		return;
	}

	RenderCB rc;
	memcpy( rc.vp0, viewDef->worldSpace.mvp[0], 16 * sizeof( float ) );
	const idMat3& va = viewDef->renderView.viewaxis;
	rc.right[0] = va[1].x;
	rc.right[1] = va[1].y;
	rc.right[2] = va[1].z;
	rc.up[0] = va[2].x;
	rc.up[1] = va[2].y;
	rc.up[2] = va[2].z;
	rc.spriteSize = r_emberSize.GetFloat();
	rc.lifetime = lifetime;
	rc.growth = r_emberGrowth.GetFloat();
	rc.intensity = r_emberIntensity.GetFloat();
	rc.colorWarm[0] = EMBER_WARM[0];
	rc.colorWarm[1] = EMBER_WARM[1];
	rc.colorWarm[2] = EMBER_WARM[2];
	rc.colorCool[0] = EMBER_COOL[0];
	rc.colorCool[1] = EMBER_COOL[1];
	rc.colorCool[2] = EMBER_COOL[2];

	const nvrhi::FramebufferInfoEx& fbinfo = hdrFramebuffer->getFramebufferInfo();
	nvrhi::ViewportState vpState;
	vpState.addViewportAndScissorRect( nvrhi::Viewport( ( float )fbinfo.width, ( float )fbinfo.height ) );

	for( size_t b = 0; b < m_Blocks.size(); b++ )
	{
		rc.slotBase = m_Blocks[b].baseSlot;
		commandList->writeBuffer( m_RenderCB, &rc, sizeof( rc ) );

		nvrhi::GraphicsState gs;
		gs.pipeline = m_RenderPipeline;
		gs.framebuffer = hdrFramebuffer;
		gs.bindings = { m_RenderSet };
		gs.viewport = vpState;
		commandList->setGraphicsState( gs );

		nvrhi::DrawArguments args;
		args.vertexCount = m_Blocks[b].count * 6;
		commandList->draw( args );
	}
}
