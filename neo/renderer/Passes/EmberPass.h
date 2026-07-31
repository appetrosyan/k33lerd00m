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
#ifndef RENDERER_PASSES_EMBERPASS_H_
#define RENDERER_PASSES_EMBERPASS_H_

#include <vector>

/*
================================================================================

	EmberPass - GPU mesh-seeded death dissolve.

	When a monster dies, idAI::Killed stamps renderEntity.emberStartTime / emberDir.
	This pass, once per frame:
	  1. seeds a block of ember particles from the dying entity's POSED mesh vertices
	     (reusing the RT skinning posed-position pool), so the demon comes apart from
	     its own surface instead of a texture melted onto its outside;
	  2. integrates the embers on the GPU (slow start -> afterblow along the killing
	     blow -> buoyant rise), sub-stepped for a higher tick rate;
	  3. draws them as additive emissive sprites into the HDR scene target.

	The active-ember registry lives here (backend side), so the sim keeps running
	after the corpse is removed from the scene.

================================================================================
*/

struct viewDef_t;
class DdgiAccelStructures;

class EmberPass
{
public:
	EmberPass( nvrhi::IDevice* device );
	~EmberPass();

	// Per-frame entry. accel may be NULL when no RT pass ran this frame (then no new
	// deaths can be seeded, but existing embers still simulate + render).
	void			Render( nvrhi::ICommandList* commandList, const viewDef_t* viewDef,
							DdgiAccelStructures* accel, nvrhi::IFramebuffer* hdrFramebuffer );

private:
	void			EnsureComputePipelines();
	void			EnsureRenderPipeline( nvrhi::IFramebuffer* fb );
	bool			AlreadySeeded( int entityIndex, int startTimeMs ) const;

	nvrhi::DeviceHandle				m_Device;

	// persistent particle pool (StructuredBuffer; UAV for seed/sim, SRV for render)
	nvrhi::BufferHandle				m_EmberBuffer;
	nvrhi::BufferHandle				m_CounterBuffer;	// append cursor for impact seeding
	uint32_t						m_Cursor;		// ring-allocation cursor into the pool

	// clear compute (mark a block inactive before an append seed)
	nvrhi::ShaderHandle				m_ClearShader;
	nvrhi::BindingLayoutHandle		m_ClearLayout;
	nvrhi::ComputePipelineHandle	m_ClearPipeline;
	nvrhi::BufferHandle				m_ClearCB;
	nvrhi::BindingSetHandle			m_ClearSet;

	// seed compute
	nvrhi::ShaderHandle				m_SeedShader;
	nvrhi::BindingLayoutHandle		m_SeedLayout;
	nvrhi::ComputePipelineHandle	m_SeedPipeline;
	nvrhi::BufferHandle				m_SeedCB;
	nvrhi::BindingSetHandle			m_SeedSet;
	nvrhi::IBuffer*					m_SeedBoundPosed;

	// sim compute
	nvrhi::ShaderHandle				m_SimShader;
	nvrhi::BindingLayoutHandle		m_SimLayout;
	nvrhi::ComputePipelineHandle	m_SimPipeline;
	nvrhi::BufferHandle				m_SimCB;
	nvrhi::BindingSetHandle			m_SimSet;

	// render graphics
	nvrhi::ShaderHandle				m_RenderVS;
	nvrhi::ShaderHandle				m_RenderPS;
	nvrhi::BindingLayoutHandle		m_RenderLayout;
	nvrhi::GraphicsPipelineHandle	m_RenderPipeline;
	nvrhi::BufferHandle				m_RenderCB;
	nvrhi::BindingSetHandle			m_RenderSet;

	bool							m_PipelinesTried;
	bool							m_PipelinesOK;
	int								m_LastTimeMs;

	struct Block
	{
		int			entityIndex;
		int			startTimeMs;
		uint32_t	baseSlot;
		uint32_t	count;
	};
	std::vector<Block>				m_Blocks;
};

#endif
