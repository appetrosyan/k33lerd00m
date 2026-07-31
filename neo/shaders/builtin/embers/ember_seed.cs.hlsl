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

// Ember seeding, IMPACT-LOCALIZED. Only mesh vertices near a recorded impact
// point (a shotgun pellet on the head, say) become embers - the rest of the body
// just burns. Each surviving vertex is appended to the ember pool (atomic counter)
// and given a velocity blown outward from the impact along the killing blow, as if
// the shot ripped a burning chunk off. Seeds from the RT skinning posed pool so the
// embers come off the actual mesh surface.
//
// Self-contained (no global_inc.hlsl). All positions here are MODEL space; impacts
// are passed in model space too, so the gate is invariant to the corpse's motion.

#pragma pack_matrix( row_major )

struct Ember
{
	float3	pos;		float	age;		// age < 0 => inactive slot
	float3	vel;		float	emitDelay;
	float3	dir;		float	seed;
};

#define MAX_IMPACTS 8

// *INDENT-OFF*
ByteAddressBuffer			t_Posed		: register( t0 );	// posed model-space float3 pool
RWStructuredBuffer<Ember>	u_Embers	: register( u0 );
RWStructuredBuffer<uint>	u_Counter	: register( u1 );	// append cursor (element 0)

cbuffer c_Seed : register( b0 )
{
	float4	g_model0;		// model->world (id column-major m[0..3])
	float4	g_model1;		// m[4..7]
	float4	g_model2;		// m[8..11]
	float4	g_model3;		// m[12..15]
	float3	g_dir;			float	g_impactRadius;		// world blow dir + model-space gate radius
	uint	g_posedVertBase;	// first source vertex (float3 units) in the posed pool
	uint	g_numSrcVerts;		// source vertices in this range
	uint	g_capacity;			// max embers in this block
	uint	g_slotBase;			// first ember slot of this block
	uint	g_numImpacts;	float	g_blowSpeed;	float	g_outwardBlow;	float	g_dirBlow;
	uint	g_seedSalt;		float	g_emitJitter;	float	g_pad0;		float	g_pad1;
	float4	g_impacts[MAX_IMPACTS];	// model-space impact origins (xyz)
};
// *INDENT-ON*

uint WangHash( uint s )
{
	s = ( s ^ 61u ) ^ ( s >> 16 );
	s *= 9u;
	s = s ^ ( s >> 4 );
	s *= 0x27d4eb2du;
	s = s ^ ( s >> 15 );
	return s;
}
float Rand01( uint s )
{
	return ( WangHash( s ) & 0x00ffffffu ) * ( 1.0f / 16777216.0f );
}

float3 ModelToWorldPoint( float3 lp )
{
	float3 wp;
	wp.x = g_model0.x * lp.x + g_model1.x * lp.y + g_model2.x * lp.z + g_model3.x;
	wp.y = g_model0.y * lp.x + g_model1.y * lp.y + g_model2.y * lp.z + g_model3.y;
	wp.z = g_model0.z * lp.x + g_model1.z * lp.y + g_model2.z * lp.z + g_model3.z;
	return wp;
}

float3 ModelToWorldDir( float3 ld )
{
	float3 wd;
	wd.x = g_model0.x * ld.x + g_model1.x * ld.y + g_model2.x * ld.z;
	wd.y = g_model0.y * ld.x + g_model1.y * ld.y + g_model2.y * ld.z;
	wd.z = g_model0.z * ld.x + g_model1.z * ld.y + g_model2.z * ld.z;
	return wd;
}

[numthreads( 64, 1, 1 )]
void main( uint3 dispatchID : SV_DispatchThreadID )
{
	const uint v = dispatchID.x;
	if( v >= g_numSrcVerts )
	{
		return;
	}

	const uint srcVert = g_posedVertBase + v;
	const float3 lp = asfloat( t_Posed.Load3( srcVert * 12u ) );

	// gate: only vertices within impactRadius of an impact point seed an ember
	float best = 1e30f;
	float3 bestImpact = float3( 0, 0, 0 );
	for( uint k = 0; k < g_numImpacts; k++ )
	{
		const float3 ip = g_impacts[k].xyz;
		const float d = distance( lp, ip );
		if( d < best )
		{
			best = d;
			bestImpact = ip;
		}
	}
	if( best > g_impactRadius )
	{
		return;		// away from every impact -> body just burns, no ember
	}

	// append
	uint slot;
	InterlockedAdd( u_Counter[0], 1u, slot );
	slot += g_slotBase;
	if( slot >= g_slotBase + g_capacity )
	{
		return;
	}

	const float r0 = Rand01( slot * 3u + 0u + g_seedSalt );
	const float r1 = Rand01( slot * 3u + 1u + g_seedSalt );
	const float r2 = Rand01( slot * 3u + 2u + g_seedSalt );

	// blast direction: outward from the impact (in world), biased along the shot
	float3 outM = ( best > 1e-4f ) ? normalize( lp - bestImpact ) : float3( 0, 0, 1 );
	float3 outW = normalize( ModelToWorldDir( outM ) );
	float3 blow = normalize( outW * g_outwardBlow + g_dir * g_dirBlow
							 + ( float3( r0, r1, r2 ) * 2.0f - 1.0f ) * 0.25f );

	Ember e;
	e.pos = ModelToWorldPoint( lp );
	e.age = 0.0f;
	e.vel = blow * ( g_blowSpeed * ( 0.55f + 0.9f * r1 ) );	// varied initial blast
	e.emitDelay = r0 * g_emitJitter;
	e.dir = blow;
	e.seed = r2;
	u_Embers[slot] = e;
}
