/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 2026 Aleksandr Petrosyan

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").

Doom 3 BFG Edition Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

===========================================================================
*/

#include "precompiled.h"
#pragma hdrstop

#include "renderer/RenderCommon.h"
#include <sys/DeviceManager.h>
#include "SoftShadowCoverageBench.h"
#include "SoftShadowCoverageBench_scene.h"		// Boxer: BenchCB / BenchScene / BuildBenchScene

extern DeviceManager* deviceManager;

// Wall-clock timing (no nvrhi TimerQuery in this vendored build - see header). One dispatch per
// submit, blocking waitForIdle, so each measured interval is one method's GPU cost end to end.
// Sys_Microseconds() is declared in sys/sys_public.h (pulled via precompiled.h).

namespace
{

// caster counts to sweep: M = tri/box occluders in the synthetic scene (per Boxer's BuildBenchScene)
const int	kSweepM[]	= { 2, 4, 8, 16 };
const int	kMethods	= 3;			// 0 = max-combine, 1 = radial-exact, 2 = scanline-exact
const int	kTimingReps	= 7;			// median-of-R (warm-up discarded)
const int	kDefaultLoopN = 256;		// per-dispatch work inflation; overridable via the cvar arg

// StructuredBuffer<float4> SRV upload buffer (mirrors SoftShadowSurfCache::EnsureWarmStream: float4
// stride, ShaderResource keepInitialState). Sized to at least one element so an empty list still
// yields a bindable buffer for a shader-declared slot.
nvrhi::BufferHandle MakeFloat4SRV( nvrhi::IDevice* device, const idList<idVec4>& src, const char* name )
{
	nvrhi::BufferDesc bd;
	const int n = src.Num() > 0 ? src.Num() : 1;
	bd.byteSize = ( uint64_t )n * sizeof( idVec4 );
	bd.structStride = sizeof( idVec4 );			// matches StructuredBuffer<float4> in the bench shader
	bd.canHaveUAVs = false;
	bd.initialState = nvrhi::ResourceStates::ShaderResource;
	bd.keepInitialState = true;
	bd.debugName = name;
	return device->createBuffer( bd );
}

double Median( double* v, int n )
{
	// tiny n: insertion sort, take the middle. Cheap, obvious, correct on the R=7 sample set.
	for( int i = 1; i < n; i++ )
	{
		double k = v[i];
		int j = i - 1;
		while( j >= 0 && v[j] > k )
		{
			v[j + 1] = v[j];
			j--;
		}
		v[j + 1] = k;
	}
	return v[n / 2];
}

// one blocking dispatch of the currently-selected method: write the volatile CB, set state, dispatch
// the fixed 32x32 grid (256x256 threads, group 8x8). Returns the wall-clock microseconds it took.
uint64 DispatchOnce( nvrhi::IDevice* device, nvrhi::IBindingSet* set, nvrhi::IComputePipeline* pipe,
					 nvrhi::IBuffer* cbBuffer, const BenchCB& cb )
{
	nvrhi::CommandListHandle cl = device->createCommandList();
	cl->open();
	cl->writeBuffer( cbBuffer, &cb, sizeof( cb ) );		// volatile CB: written in the same list that consumes it
	nvrhi::ComputeState cs;
	cs.pipeline = pipe;
	cs.bindings = { set };
	cl->setComputeState( cs );
	cl->dispatch( 32, 32, 1 );							// 32*8 = 256 threads per axis -> fragCount 65536
	cl->close();
	const uint64 t0 = Sys_Microseconds();
	device->executeCommandList( cl );
	device->waitForIdle();
	return Sys_Microseconds() - t0;
}

// read u_BenchOut (fragCount floats) back into out via a CPU-readable staging copy (mirrors
// SoftShadowTermPass::GetWalkStats).
bool ReadCoverage( nvrhi::IDevice* device, nvrhi::IBuffer* outBuffer, float* out, int fragCount )
{
	nvrhi::BufferDesc sbd;
	sbd.byteSize = ( uint64_t )fragCount * sizeof( float );
	sbd.cpuAccess = nvrhi::CpuAccessMode::Read;
	sbd.debugName = "SoftShadowCoverageBench/Readback";
	nvrhi::BufferHandle staging = device->createBuffer( sbd );
	nvrhi::CommandListHandle cl = device->createCommandList();
	cl->open();
	cl->copyBuffer( staging, 0, outBuffer, 0, ( uint64_t )fragCount * sizeof( float ) );
	cl->close();
	device->executeCommandList( cl );
	device->waitForIdle();
	void* p = device->mapBuffer( staging, nvrhi::CpuAccessMode::Read );
	if( p == nullptr )
	{
		return false;
	}
	memcpy( out, p, ( size_t )fragCount * sizeof( float ) );
	device->unmapBuffer( staging );
	return true;
}

} // anonymous namespace

int R_SoftShadowCoverageBench( const char* arg )
{
	nvrhi::IDevice* device = deviceManager ? deviceManager->GetDevice() : nullptr;
	if( device == nullptr )
	{
		common->Warning( "[covbench] no nvrhi device - render stack not up; nothing to bench." );
		return 0;
	}

	// loopN: cvar arg overrides the per-dispatch work-inflation count (a dispatch should land in the
	// few-ms range so the CPU-wall timer resolves the methods apart). Any int > 1 wins; else default.
	int loopN = kDefaultLoopN;
	if( arg != nullptr )
	{
		const int a = atoi( arg );
		if( a > 1 )
		{
			loopN = a;
		}
	}

	// --- load the three method permutations (BENCH_METHOD 0/1/2). FindShader dedups by name+stage+
	// suffix and IGNORES macros, so each method needs a DISTINCT nameOutSuffix ("m0"/"m1"/"m2") or it
	// returns the first-built entry (same rule the softterm permutations follow). ---
	nvrhi::ShaderHandle shader[kMethods] = {};
	for( int m = 0; m < kMethods; m++ )
	{
		const char* mstr = ( m == 0 ) ? "0" : ( ( m == 1 ) ? "1" : "2" );
		idList<shaderMacro_t> macros;
		macros.Append( shaderMacro_t( "BENCH_METHOD", mstr ) );
		shader[m] = renderProgManager.GetShader( renderProgManager.FindShader(
					"builtin/lighting/softShadowCoverageBench", SHADER_STAGE_COMPUTE,
					( idStr( "m" ) + mstr ).c_str(), macros, true, LAYOUT_DRAW_VERT ) );
		if( shader[m] == nullptr )
		{
			common->Warning( "[covbench] BENCH_METHOD=%s compute shader failed to load - aborting bench.", mstr );
			return 0;
		}
	}

	// --- one binding layout shared by all three pipelines (identical slots): b0 CB, t0/t1/t2 the three
	// float4 SRVs, u0 the coverage RWStructuredBuffer<float>. ---
	nvrhi::BindingLayoutDesc ld;
	ld.visibility = nvrhi::ShaderType::Compute;
	ld.bindings =
	{
		nvrhi::BindingLayoutItem::VolatileConstantBuffer( 0 ),	// b0 : BenchCB
		nvrhi::BindingLayoutItem::StructuredBuffer_SRV( 0 ),		// t0 : t_BenchEdges
		nvrhi::BindingLayoutItem::StructuredBuffer_SRV( 1 ),		// t1 : t_BenchBoxes
		nvrhi::BindingLayoutItem::StructuredBuffer_SRV( 2 ),		// t2 : t_BenchFrags
		// t3 : t_SoftTiles - the bench shader declares this dummy SRV (tile-list walkers it never
		// dispatches). If DXC strips it, a layout superset is harmless (Vulkan permits an unreferenced
		// layout binding); if DXC keeps it, a shader-declared binding MISSING from the layout is the
		// desync/crash landmine (see SoftShadowSurfCache). Declaring it covers both outcomes.
		nvrhi::BindingLayoutItem::StructuredBuffer_SRV( 3 ),
		nvrhi::BindingLayoutItem::StructuredBuffer_UAV( 0 ),		// u0 : u_BenchOut
	};
	nvrhi::BindingLayoutHandle layout = device->createBindingLayout( ld );

	nvrhi::ComputePipelineHandle pipeline[kMethods] = {};
	for( int m = 0; m < kMethods; m++ )
	{
		nvrhi::ComputePipelineDesc pd;
		pd.bindingLayouts = { layout };
		pd.CS = shader[m];
		pipeline[m] = device->createComputePipeline( pd );
	}

	// volatile CB, written fresh per dispatch (method/loopN vary). maxVersions generous - each dispatch
	// is its own blocking submit, so one live version at a time in practice.
	nvrhi::BufferDesc cbd;
	cbd.byteSize = sizeof( BenchCB );				// 48 bytes: float4 + int4 + int4
	cbd.isConstantBuffer = true;
	cbd.isVolatile = true;
	cbd.maxVersions = 64;
	cbd.debugName = "SoftShadowCoverageBench/CB";
	nvrhi::BufferHandle cbBuffer = device->createBuffer( cbd );

	common->Printf( "[covbench] loopN=%d, %d methods, %d timing reps (median), CPU wall-clock timing\n",
					loopN, kMethods, kTimingReps );

	int totalViol = 0;
	const int nSweep = ( int )( sizeof( kSweepM ) / sizeof( kSweepM[0] ) );
	for( int s = 0; s < nSweep; s++ )
	{
		const int M = kSweepM[s];
		BenchScene scene;
		BuildBenchScene( M, scene );

		const int fragCount = scene.cb.fragCount;
		if( fragCount <= 0 )
		{
			common->Warning( "[covbench] M=%d built fragCount %d - skipping.", M, fragCount );
			continue;
		}

		// per-M SRV buffers (edge/box/frag counts vary with M) + the coverage UAV (constant fragCount).
		nvrhi::BufferHandle edgeBuf = MakeFloat4SRV( device, scene.edges, "SoftShadowCoverageBench/Edges" );
		nvrhi::BufferHandle boxBuf	= MakeFloat4SRV( device, scene.boxes, "SoftShadowCoverageBench/Boxes" );
		nvrhi::BufferHandle fragBuf	= MakeFloat4SRV( device, scene.frags, "SoftShadowCoverageBench/Frags" );

		nvrhi::BufferDesc od;
		od.byteSize = ( uint64_t )fragCount * sizeof( float );
		od.structStride = sizeof( float );			// RWStructuredBuffer<float>
		od.canHaveUAVs = true;
		od.initialState = nvrhi::ResourceStates::UnorderedAccess;
		od.keepInitialState = true;
		od.debugName = "SoftShadowCoverageBench/Out";
		nvrhi::BufferHandle outBuf = device->createBuffer( od );

		// upload the SRV data once (non-volatile keepInitialState buffers - mirrors the surf-cache stream
		// writeBuffer). Empty lists get no write (the 1-element buffer just stays undefined but bindable).
		{
			nvrhi::CommandListHandle cl = device->createCommandList();
			cl->open();
			if( scene.edges.Num() > 0 )
			{
				cl->writeBuffer( edgeBuf, scene.edges.Ptr(), ( size_t )scene.edges.Num() * sizeof( idVec4 ), 0 );
			}
			if( scene.boxes.Num() > 0 )
			{
				cl->writeBuffer( boxBuf, scene.boxes.Ptr(), ( size_t )scene.boxes.Num() * sizeof( idVec4 ), 0 );
			}
			if( scene.frags.Num() > 0 )
			{
				cl->writeBuffer( fragBuf, scene.frags.Ptr(), ( size_t )scene.frags.Num() * sizeof( idVec4 ), 0 );
			}
			cl->close();
			device->executeCommandList( cl );
			device->waitForIdle();
		}

		nvrhi::BindingSetDesc bsd;
		bsd.bindings =
		{
			nvrhi::BindingSetItem::ConstantBuffer( 0, cbBuffer ),
			nvrhi::BindingSetItem::StructuredBuffer_SRV( 0, edgeBuf ),
			nvrhi::BindingSetItem::StructuredBuffer_SRV( 1, boxBuf ),
			nvrhi::BindingSetItem::StructuredBuffer_SRV( 2, fragBuf ),
			nvrhi::BindingSetItem::StructuredBuffer_SRV( 3, edgeBuf ),	// dummy t3 (t_SoftTiles, never read)
			nvrhi::BindingSetItem::StructuredBuffer_UAV( 0, outBuf ),
		};
		nvrhi::BindingSetHandle set = device->createBindingSet( bsd, layout );

		double methodMs[kMethods] = { 0, 0, 0 };
		float* cov[kMethods] = { nullptr, nullptr, nullptr };
		for( int m = 0; m < kMethods; m++ )
		{
			BenchCB cb = scene.cb;
			cb.method = m;
			cb.loopN = loopN;

			DispatchOnce( device, set, pipeline[m], cbBuffer, cb );	// warm-up (discarded)

			double samples[kTimingReps];
			for( int r = 0; r < kTimingReps; r++ )
			{
				samples[r] = ( double )DispatchOnce( device, set, pipeline[m], cbBuffer, cb ) / 1000.0;	// us -> ms
			}
			methodMs[m] = Median( samples, kTimingReps );

			cov[m] = ( float* )Mem_Alloc( fragCount * sizeof( float ), TAG_TEMP );
			if( !ReadCoverage( device, outBuf, cov[m], fragCount ) )
			{
				common->Warning( "[covbench] M=%d method %d readback failed.", M, m );
			}
		}

		// --- cross-check: classify each fragment by M2 (the reference exact scanline coverage). On
		// UMBRA fragments the two exact methods must agree (lossless); on PENUMBRA fragments both exact
		// methods must cover AT LEAST as much as max-combine (M0 under-shadows the overlaps). ---
		const float eps = 0.02f;
		int losslessViol = 0;		// |M1 - M2| >= eps on an umbra fragment
		int holeViol = 0;			// M1 < M0 - eps or M2 < M0 - eps on a penumbra fragment (under-shadow)
		if( cov[0] && cov[1] && cov[2] )
		{
			for( int f = 0; f < fragCount; f++ )
			{
				const float c2 = cov[2][f];
				if( c2 >= 0.99f )		// umbra (by the exact reference)
				{
					if( idMath::Fabs( cov[1][f] - cov[2][f] ) >= eps )
					{
						losslessViol++;
					}
				}
				else if( c2 > 0.01f )	// penumbra / partial overlap
				{
					if( cov[1][f] < cov[0][f] - eps || cov[2][f] < cov[0][f] - eps )
					{
						holeViol++;
					}
				}
			}
		}
		totalViol += losslessViol + holeViol;

		const double ratio = methodMs[2] > 0.0 ? ( methodMs[1] / methodMs[2] ) : 0.0;
		common->Printf( "[covbench] M=%d casters=%d loopN=%d | M0(max) %.3f ms | M1(radial) %.3f ms | M2(scan) %.3f ms | M1/M2 %.2fx | lossless %s (viol %d) | holes-fixed %s (viol %d)\n",
						M, scene.cb.casterCount, loopN,
						methodMs[0], methodMs[1], methodMs[2], ratio,
						losslessViol == 0 ? "yes" : "NO", losslessViol,
						holeViol == 0 ? "yes" : "NO", holeViol );

		for( int m = 0; m < kMethods; m++ )
		{
			if( cov[m] )
			{
				Mem_Free( cov[m] );
			}
		}
	}

	common->Printf( "[covbench] DONE: %d total correctness violation(s) -> %s\n",
					totalViol, totalViol == 0 ? "PASS" : "FAIL" );
	return totalViol;
}
