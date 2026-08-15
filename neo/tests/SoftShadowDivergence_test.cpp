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
#include <algorithm>

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

	double worstFL = 0.0, worstFS = 0.0;
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

		long fl = 0, fs = 0, pen = 0, lit = 0;
		long outerPen = 0, outerPenArea = 0;	// penumbra where the CENTRE ray is visible = beyond the point-light shadow
		double wfl = 0.0, wfs = 0.0, worstOuter = 0.0;
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
		( void )outerPenArea;
		std::printf( "    [%-11s R=%2.0f] penumbra=%4ld lit=%4ld | WEDGE(correct class): FALSE-LIT=%4ld(worst %.2f) FALSE-SHADOW=%4ld(worst %.2f)"
					 " | CLASSIFIER: OUTER-penumbra beyond point-light shadow=%4ld (%5.1f%% of penumbra, worst occ %.2f) <- these FALSE-LIT if the band is the stencil hard shadow\n",
					 cf.name, cf.R, pen, lit, fl, wfl, fs, wfs,
					 outerPen, pen ? 100.0 * outerPen / pen : 0.0, worstOuter );
	}
	std::printf( "    [worst] false-lit %.3f at (%.1f,%.1f)   false-shadow %.3f at (%.1f,%.1f)\n",
				 worstFL, worstFLp.x, worstFLp.y, worstFS, worstFSp.x, worstFSp.y );

	// A correct classifier must never call a shadowed point fully lit, nor a lit point shadowed, by a large
	// margin. These are strict on purpose - this instrument is meant to go RED where the analytic diverges.
	CHECK( worstFL <= 0.10 );
	CHECK( worstFS <= 0.10 );
}
