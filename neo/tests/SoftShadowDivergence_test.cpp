/*
===========================================================================
Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2026 Aleksandr Petrosyan

This file is part of the Doom 3 BFG Edition GPL Source Code. Free software under GPLv3+.
===========================================================================
*/

// DIVERGENCE gate for the analytic soft shadow. It heavily penalises the two error modes that are large and
// obvious in game and that the loose accuracy tests let through:
//   (A) FALSE-LIT   - the truth says penumbra/shadow, the analytic says lit (the sharp cutoff where a
//                     penumbra steps straight into lit).
//   (B) FALSE-SHADOW - the truth says lit, the analytic says shadow (a "turd" on a lit surface).
// Ground truth is TruthShadow (disk-sampled ray occlusion, SoftShadowBox.h); the analytic is the shipped
// SoftShadow_WedgeOcclusion with the centre-lit flag set from the actual centre-ray occlusion (what a correct
// classifier would feed it). Corpus = the error-prone situations: wide penumbra, elevated caster, off-axis.
// This is a RED-until-fixed instrument: any large false-lit / false-shadow fails, with the worst offender
// printed for diagnosis.

#include "hlsl_compat.h"
#include "softwedge_coverage.inc.hlsl"
#include "SoftShadowBox.h"
#include "idUnitTest.h"

#include <vector>
#include <cstdio>
#include <cmath>

using namespace swtest;

namespace
{
struct Cfg
{
	const char* name;
	float3      boxC;
	float3      boxH;
	float       R;			// light-disk radius = penumbra size
};

// 2D crossing-number point-in-polygon
inline bool PtInPoly( const std::vector<float2>& p, float x, float y )
{
	bool in = false;
	int n = ( int )p.size();
	for( int a = 0, b = n - 1; a < n; b = a++ )
	{
		if( ( ( p[a].y > y ) != ( p[b].y > y ) ) &&
				( x < ( p[b].x - p[a].x ) * ( y - p[a].y ) / ( p[b].y - p[a].y ) + p[a].x ) )
		{
			in = !in;
		}
	}
	return in;
}

// The GPU SHELL band region on the floor: each silhouette vertex is inflated RADIALLY about the caster
// centre by r' = R*margin (softband.vs.hlsl), then projected from the light centre onto z=0. This is the
// region where the analytic is allowed to run; outside it the classifier forces LIT. Replicated here to
// test whether it actually contains the outer penumbra.
inline std::vector<float2> ShellFloorPoly( const std::vector<float3>& loop, float3 centre, float3 L, float rPrime )
{
	std::vector<float2> poly;
	for( size_t k = 0; k < loop.size(); k++ )
	{
		float3 V = loop[k];
		float3 d = V - centre;
		float  len = std::sqrt( d.x * d.x + d.y * d.y + d.z * d.z );
		float3 Vp = ( len > 1e-4f ) ? float3( V.x + rPrime * d.x / len, V.y + rPrime * d.y / len, V.z + rPrime * d.z / len ) : V;
		float t = L.z / ( L.z - Vp.z );			// project L->Vp onto z=0
		poly.push_back( float2( L.x + t * ( Vp.x - L.x ), L.y + t * ( Vp.y - L.y ) ) );
	}
	return poly;
}
}

TEST( SoftShadowDivergence, penalise_false_lit_and_false_shadow_vs_truth )
{
	const float3 L( 0.0f, 0.0f, 14.0f );
	const Cfg cfgs[] =
	{
		{ "contact_R2",    float3( 0.0f, 0.0f, 1.2f ), float3( 1.2f, 1.2f, 1.0f ),  2.0f },
		{ "wide_R8",       float3( 0.0f, 0.0f, 3.0f ), float3( 1.2f, 1.2f, 1.5f ),  8.0f },
		{ "elevated_R8",   float3( 0.5f, 0.3f, 6.0f ), float3( 1.5f, 1.0f, 1.2f ),  8.0f },
		{ "bigR16",        float3( 0.0f, 0.0f, 4.0f ), float3( 1.5f, 1.5f, 1.5f ), 16.0f },
	};
	const int   N  = 96;			// floor grid N*N over [-G,G]^2 at z=0
	const float G  = 10.0f;
	const float EL = 0.02f;			// "lit"   threshold on occlusion
	const float EH = 0.15f;			// "shadow/penumbra" threshold on occlusion

	double worstFL = 0.0, worstFS = 0.0, worstShell = 0.0;
	float3 worstFLp( 0, 0, 0 ), worstFSp( 0, 0, 0 );

	for( const Cfg& cf : cfgs )
	{
		Box b = MakeBox( cf.boxC, cf.boxH, 0.4f, 0.1f );
		std::vector<float3> loop = Silhouette( b, L );
		std::vector<std::vector<float3>> loops;
		if( loop.size() >= 3 )
		{
			loops.push_back( loop );
		}
		std::vector<float4> rec = BuildCaster( loops );
		int numRec = ( int )( rec.size() / 2 );
		SoftEdgeBuffer buf{ rec.data(), ( int )rec.size() };
		std::vector<float2> shell = ShellFloorPoly( loop, cf.boxC, L, cf.R * 1.1f );	// the actual GPU shell region

		long fl = 0, fs = 0, pen = 0, lit = 0;
		long outerPen = 0, shellMiss = 0;		// penumbra beyond point-light shadow; penumbra the SHELL fails to cover
		double wfl = 0.0, wfs = 0.0, worstOuter = 0.0, worstShellMiss = 0.0;
		for( int j = 0; j < N; j++ )
		{
			for( int i = 0; i < N; i++ )
			{
				float3 P( -G + ( i + 0.5f ) * 2.0f * G / N, -G + ( j + 0.5f ) * 2.0f * G / N, 0.0f );
				float truthOcc = 1.0f - TruthShadow( P, L, cf.R, b, 64 );			// 0 = lit, 1 = shadow
				bool centreBlocked = RayHitsBox( P, L - P, b );						// in the POINT-LIGHT shadow?
				float centreLit = centreBlocked ? 0.0f : 1.0f;
				float anaOcc = SoftShadow_WedgeOcclusion( P, L, cf.R, 0, numRec, centreLit, buf );

				// CLASSIFIER BUG: a band == the point-light shadow (centre-ray) forces LIT wherever the centre
				// ray is visible. Every such point that is really penumbra is then a hard FALSE-LIT - the sharp
				// cutoff. This is the outer penumbra the wedges are supposed to inflate the band to cover.
				if( truthOcc > EH )
				{
					pen++;
					if( !centreBlocked )
					{
						outerPen++;										// a point-light-shadow band would FALSE-LIT this
						if( truthOcc > worstOuter ) { worstOuter = truthOcc; }
					}
					// does the ACTUAL shell region cover this real-penumbra point? if not, the classifier
					// forces it lit regardless of the (correct) wedge -> the sharp cutoff.
					if( !PtInPoly( shell, P.x, P.y ) )
					{
						shellMiss++;
						if( truthOcc > worstShellMiss ) { worstShellMiss = truthOcc; }
					}
				}

				if( truthOcc > EH )				// wedge-math check: truth penumbra but analytic (correct class) lit
				{
					if( anaOcc < EL )
					{
						fl++;
						double m = truthOcc - anaOcc;
						if( m > wfl ) { wfl = m; }
						if( m > worstFL ) { worstFL = m; worstFLp = P; }
					}
				}
				if( truthOcc < EL )				// truth is lit here
				{
					lit++;
					if( anaOcc > EH )			// ... but analytic says shadow = FALSE-SHADOW
					{
						fs++;
						double m = anaOcc - truthOcc;
						if( m > wfs ) { wfs = m; }
						if( m > worstFS ) { worstFS = m; worstFSp = P; }
					}
				}
			}
		}
		std::printf( "    [%-11s R=%2.0f] penumbra=%4ld | WEDGE(correct class) false-lit=%ld(%.2f) false-shadow=%ld(%.2f)"
					 " | OUTER=%ld(%.0f%%) | SHELL-MISS=%4ld (%5.1f%% of penumbra NOT covered by the shell, worst occ %.2f)\n",
					 cf.name, cf.R, pen, fl, wfl, fs, wfs,
					 outerPen, pen ? 100.0 * outerPen / pen : 0.0,
					 shellMiss, pen ? 100.0 * shellMiss / pen : 0.0, worstShellMiss );
		( void )lit;
		if( worstShellMiss > worstShell ) { worstShell = worstShellMiss; }
	}
	std::printf( "    [worst] false-lit %.3f at (%.1f,%.1f)   false-shadow %.3f at (%.1f,%.1f)\n",
				 worstFL, worstFLp.x, worstFLp.y, worstFS, worstFSp.x, worstFSp.y );

	// A correct classifier must never call a shadowed point fully lit, nor a lit point shadowed, by a large
	// margin, and its conservative band (the shell) must CONTAIN the whole penumbra. Strict on purpose -
	// this instrument is meant to go RED where the analytic (or its classifier geometry) diverges.
	CHECK( worstFL <= 0.10 );		// wedge must not miss shadow
	CHECK( worstFS <= 0.10 );		// wedge must not add shadow
	CHECK( worstShell <= 0.05 );	// the shell band must cover the penumbra (elevated casters currently miss ~0.20)
}
