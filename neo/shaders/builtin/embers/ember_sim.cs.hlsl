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

// Ember integration. Runs once per rendered frame but sub-steps internally at a
// higher tick rate for stable, controllable motion. Each ember starts nearly
// still (the demon "coming apart"), drifts slowly along the killing-blow
// direction, then an afterblow accelerates it while buoyancy lifts the residue.
//
// Self-contained (no global_inc.hlsl).

#pragma pack_matrix( row_major )

struct Ember
{
	float3	pos;		float	age;		// age < 0 => inactive slot
	float3	vel;		float	emitDelay;
	float3	dir;		float	seed;
};

// *INDENT-OFF*
RWStructuredBuffer<Ember>	u_Embers	: register( u0 );

cbuffer c_Sim : register( b0 )
{
	float	g_dt;			// frame delta (seconds)
	uint	g_substeps;		// physics sub-steps this frame
	uint	g_count;		// ember slots in this block
	float	g_buoyancy;		// upward accel (world +Z)
	float	g_drag;			// velocity damping per second
	float	g_afterblowStart;	// localT at which the afterblow kicks in
	float	g_afterblowAccel;	// afterblow acceleration along dir
	float	g_initialSpeed;		// gentle creep accel during the slow start
	float	g_slowStartT;		// duration of the slow-start ramp
	float	g_lifetime;			// ember life after it detaches
	float	g_gravity;			// downward accel (opposes buoyancy)
	uint	g_slotBase;			// first ember slot of this block
};
// *INDENT-ON*

[numthreads( 64, 1, 1 )]
void main( uint3 dispatchID : SV_DispatchThreadID )
{
	if( dispatchID.x >= g_count )
	{
		return;
	}
	const uint i = g_slotBase + dispatchID.x;

	Ember e = u_Embers[i];
	if( e.age < 0.0f )
	{
		return;	// inactive slot
	}

	const uint steps = max( 1u, g_substeps );
	const float sdt = g_dt / ( float )steps;

	[loop]
	for( uint s = 0; s < steps; s++ )
	{
		e.age += sdt;
		const float localT = e.age - e.emitDelay;
		if( localT < 0.0f )
		{
			continue;	// still part of the body, not yet detached
		}

		// velocity was set at seed (the blast); here we just apply forces. A small
		// optional afterblow keeps pushing along the blow, buoyancy lifts the burning
		// residue, gravity + drag settle it.
		const float afterblow = saturate( ( localT - g_afterblowStart ) / 0.4f );

		float3 accel = e.dir * ( g_afterblowAccel * afterblow );
		accel.z += g_buoyancy - g_gravity;

		e.vel += accel * sdt;
		e.vel *= saturate( 1.0f - g_drag * sdt );
		e.pos += e.vel * sdt;

		if( localT > g_lifetime )
		{
			e.age = -1.0f;	// expired
			break;
		}
	}

	u_Embers[i] = e;
}
