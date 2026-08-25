/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2026 Aleksandr Petrosyan

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").

Doom 3 BFG Edition Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

===========================================================================
*/

// The defect COUNTER must itself be proven able to count before its zero is believed: synthetic term
// images with KNOWN injected defect counts, checked exactly. These are analyzer tests, not renderer
// tests - the renderer gate is the com_softShadowGate engine run over the cap corpus.

#include "idUnitTest.h"
#include "SoftShadowGate.h"

using namespace swgate;

namespace
{

const int W = 192, H = 108;

// the real gate runs at >=1920x1080 where the area-scaled thresholds are meaningful; at this synthetic
// 192x108 the default turd threshold would collapse to 1 px (every ant a turd). Scale the config so the
// MECHANISM (blob >= threshold = turd, else ant) is exercised with threshold 64 / extent-min 100 here.
GateCfg TestCfg()
{
	GateCfg c;
	c.turdArea1080 = 6400;
	c.extentMin1080 = 10000;
	return c;
}

std::vector<uint8_t> AllValid()
{
	return std::vector<uint8_t>( ( size_t )W * H, 1 );
}

// a lit field with a horizontal smooth penumbra ramp band: lit above y0, umbra below y1, linear between
GateImg RampScene()
{
	GateImg img( W, H, 1.0f );
	const int y0 = 40, y1 = 70;
	for( int y = 0; y < H; y++ )
	{
		for( int x = 0; x < W; x++ )
		{
			float t = 1.0f;
			if( y >= y1 )
			{
				t = 0.0f;
			}
			else if( y >= y0 )
			{
				t = 1.0f - ( float )( y - y0 ) / ( float )( y1 - y0 );
			}
			img.At( x, y ) = t;
		}
	}
	return img;
}

int Count( const std::vector<GateDefect>& d, int kind )
{
	int n = 0;
	for( const GateDefect& x : d )
	{
		if( x.kind == kind )
		{
			n += 1;
		}
	}
	return n;
}

} // namespace

// clean pair: analytic == RT, repeat frame identical -> ZERO defects of any kind
TEST( SoftShadowGate, clean_scene_has_zero_defects )
{
	GateCfg cfg = TestCfg();
	GateImg rt = RampScene();
	GateImg ana = rt;
	std::vector<uint8_t> valid = AllValid();
	std::vector<GateDefect> d;
	GateAgreement( ana, rt, valid, cfg, d );
	GateTemporal( ana, ana, valid, cfg, d );
	CHECK( d.empty() );
}

// one large dark blob + three isolated dark speckles in the RT-lit field -> exactly 1 TURD + 3 ANTs,
// each instance its own defect
TEST( SoftShadowGate, turds_and_ants_counted_individually )
{
	GateCfg cfg = TestCfg();
	GateImg rt = RampScene();
	GateImg ana = rt;
	const int turdArea = cfg.ResScale( cfg.turdArea1080, W, H );
	// blob: a square comfortably above the scaled turd threshold, deep in the lit region
	int side = 1;
	while( side * side < turdArea + 4 )
	{
		side++;
	}
	for( int y = 5; y < 5 + side; y++ )
	{
		for( int x = 10; x < 10 + side; x++ )
		{
			ana.At( x, y ) = 0.2f;
		}
	}
	// ants: single dark pixels, well separated, still in the lit region
	ana.At( 60, 10 ) = 0.3f;
	ana.At( 90, 20 ) = 0.0f;
	ana.At( 130, 8 ) = 0.5f;
	std::vector<GateDefect> d;
	GateAgreement( ana, rt, AllValid(), cfg, d );
	CHECK( Count( d, GATE_TURD ) == 1 );
	CHECK( Count( d, GATE_ANT ) == 3 );
	CHECK( Count( d, GATE_LIT_IN_UMBRA ) == 0 );
}

// two separated lit patches inside the RT umbra -> exactly 2 LIT_IN_UMBRA defects
TEST( SoftShadowGate, lit_in_umbra_counted_individually )
{
	GateCfg cfg = TestCfg();
	GateImg rt = RampScene();
	GateImg ana = rt;
	for( int x = 20; x < 26; x++ )
	{
		ana.At( x, 90 ) = 0.9f;
	}
	for( int x = 120; x < 124; x++ )
	{
		ana.At( x, 100 ) = 1.0f;
	}
	std::vector<GateDefect> d;
	GateAgreement( ana, rt, AllValid(), cfg, d );
	CHECK( Count( d, GATE_LIT_IN_UMBRA ) == 2 );
	CHECK( Count( d, GATE_TURD ) + Count( d, GATE_ANT ) == 0 );
}

// a hard cliff across half the penumbra where RT ramps smoothly -> exactly 1 STEP chain (one line,
// not one defect per pixel)
TEST( SoftShadowGate, penumbra_cliff_is_one_step_defect )
{
	GateCfg cfg = TestCfg();
	GateImg rt = RampScene();
	GateImg ana = rt;
	// left half: replace the smooth ramp with a binary step at mid-band (the "hard black band" shape)
	for( int y = 40; y < 70; y++ )
	{
		for( int x = 0; x < W / 2; x++ )
		{
			ana.At( x, y ) = ( y < 55 ) ? 0.97f : 0.03f;		// stays inside the penumbra band, so only STEP fires
		}
	}
	std::vector<GateDefect> d;
	GateAgreement( ana, rt, AllValid(), cfg, d );
	CHECK( Count( d, GATE_STEP ) == 1 );
}

// analytic renders NO penumbra at all (hard edge) -> the extent probe fires on the ramp region
TEST( SoftShadowGate, missing_penumbra_is_an_extent_defect )
{
	GateCfg cfg = TestCfg();
	GateImg rt = RampScene();
	GateImg ana( W, H, 1.0f );
	for( int y = 0; y < H; y++ )
	{
		for( int x = 0; x < W; x++ )
		{
			ana.At( x, y ) = ( y < 55 ) ? 1.0f : 0.0f;		// binary shadow, zero penumbra area
		}
	}
	std::vector<GateDefect> d;
	GateAgreement( ana, rt, AllValid(), cfg, d );
	CHECK( Count( d, GATE_EXTENT ) == 1 );
}

// penumbra values that disagree numerically but stay inside the band are NOT defects
TEST( SoftShadowGate, penumbra_value_disagreement_is_not_a_defect )
{
	GateCfg cfg = TestCfg();
	GateImg rt = RampScene();
	GateImg ana = rt;
	for( int y = 41; y < 69; y++ )
	{
		for( int x = 0; x < W; x++ )
		{
			float t = ana.At( x, y );
			if( t > cfg.umbraT + 0.1f && t < cfg.litT - 0.1f )
			{
				ana.At( x, y ) = t * 0.8f + 0.1f;		// squashed ramp: different values, same band, still smooth
			}
		}
	}
	std::vector<GateDefect> d;
	GateAgreement( ana, rt, AllValid(), cfg, d );
	CHECK( Count( d, GATE_TURD ) + Count( d, GATE_ANT ) + Count( d, GATE_LIT_IN_UMBRA ) + Count( d, GATE_STEP ) == 0 );
}

// two separated pixel clusters differ between two "identical" frames -> exactly 2 TEMPORAL defects;
// identical frames -> 0
TEST( SoftShadowGate, temporal_jitter_clusters_counted )
{
	GateCfg cfg = TestCfg();
	GateImg a = RampScene();
	GateImg b = a;
	b.At( 30, 30 ) += 0.01f;
	b.At( 31, 30 ) -= 0.02f;		// touches the first -> same cluster
	b.At( 150, 90 ) += 0.5f;
	std::vector<GateDefect> d;
	GateTemporal( a, b, AllValid(), cfg, d );
	CHECK( Count( d, GATE_TEMPORAL ) == 2 );
	d.clear();
	GateTemporal( a, a, AllValid(), cfg, d );
	CHECK( d.empty() );
}

// continuity: identity reprojection (same view twice). A region that flips lit->umbra between the two
// renders = 1 CONTINUITY defect; unchanged frames = 0. Uses a trivial orthographic-style MVP mapping
// world (x,y) directly to NDC so the probe's project/unproject round-trip is exercised end to end.
TEST( SoftShadowGate, continuity_flip_detected_through_reprojection )
{
	GateCfg cfg = TestCfg();
	const float sx = 2.0f / W, sy = 2.0f / H;
	// clip = M * (P,1): x_ndc = px*sx - 1 (+half-texel), y_ndc = -(py*sy - 1), z = pz, w = 1
	GateView view = {};
	view.mvp[0] = sx;
	view.mvp[3] = -1.0f + sx * 0.0f;
	view.mvp[5] = -sy;
	view.mvp[7] = 1.0f;
	view.mvp[10] = 1.0f;
	view.mvp[15] = 1.0f;
	// unprojection = inverse mapping pixel->world for that MVP with clipW == 1: world x = ndcx+1 scaled
	float unproj[16] = {};
	unproj[0] = 1.0f / sx;
	unproj[3] = 1.0f / sx;		// x = (ndcx + 1)/sx
	unproj[5] = -1.0f / sy;
	unproj[7] = 1.0f / sy;		// y = (1 - ndcy)/sy
	unproj[10] = 1.0f;
	unproj[15] = 1.0f;
	GateImg base = RampScene();
	GateImg depth( W, H, 0.0f );
	GateImg disp = base;
	// flip a lit patch to umbra in the displaced view - the same world points, radically different class
	for( int y = 10; y < 16; y++ )
	{
		for( int x = 40; x < 52; x++ )
		{
			disp.At( x, y ) = 0.0f;
		}
	}
	std::vector<GateDefect> d;
	GateContinuity( base, depth, AllValid(), unproj, disp, depth, view, cfg, d );
	CHECK( Count( d, GATE_CONTINUITY ) == 1 );
	d.clear();
	GateContinuity( base, depth, AllValid(), unproj, base, depth, view, cfg, d );
	CHECK( d.empty() );
}

// masked-out pixels can never mint defects (background/no-interaction regions are excluded)
TEST( SoftShadowGate, invalid_pixels_never_count )
{
	GateCfg cfg = TestCfg();
	GateImg rt = RampScene();
	GateImg ana = rt;
	std::vector<uint8_t> valid = AllValid();
	for( int y = 0; y < 20; y++ )
	{
		for( int x = 0; x < W; x++ )
		{
			ana.At( x, y ) = 0.0f;		// grossly dark where RT is lit...
			valid[( size_t )y * W + x] = 0;		// ...but outside the interaction mask
		}
	}
	std::vector<GateDefect> d;
	GateAgreement( ana, rt, valid, cfg, d );
	GateTemporal( ana, rt, valid, cfg, d );
	CHECK( d.empty() );
}

// GRAIN detector: a genuinely clean pair (analytic == smooth RT) mints no high-frequency defect.
// Proves the detector's zero is believable before its fires are trusted.
TEST( SoftShadowGate, grain_clean_pair_zero )
{
	GateCfg cfg = TestCfg();
	GateImg rt = RampScene();
	GateImg ana = rt;
	std::vector<GateDefect> d;
	GateGrain( ana, rt, AllValid(), nullptr, cfg, d );
	CHECK( d.empty() );
}

// GRAIN: per-pixel speckle in the analytic term over a FLAT true field = ANT (does not survive the
// median). The RT is smooth, so the high-frequency energy is spurious.
TEST( SoftShadowGate, grain_speckle_over_flat_truth_is_ant )
{
	GateCfg cfg = TestCfg();
	GateImg rt( W, H, 1.0f );		// flat lit truth
	GateImg ana = rt;
	// scatter isolated dark speckles (0.85) in a compact patch - each deviates from its 3x3 mean AND
	// median (neighbours are 1.0), the grain signature
	for( int y = 40; y < 60; y += 2 )
		for( int x = 40; x < 80; x += 2 )
		{
			ana.At( x, y ) = 0.85f;
		}
	std::vector<GateDefect> d;
	GateGrain( ana, rt, AllValid(), nullptr, cfg, d );
	CHECK( Count( d, GATE_ANT ) >= 1 );		// the speckle field is caught (the patch boundary may also mint a STEP - real detail, not discounted)
}

// GRAIN classifier: a COHERENT sub-cliff step the smooth truth lacks = STEP (survives the median), NOT
// discounted. A step edge is not per-pixel noise, but it is still analytic detail the truth does not have.
TEST( SoftShadowGate, grain_coherent_step_over_flat_truth_is_step )
{
	GateCfg cfg = TestCfg();
	GateImg rt( W, H, 1.0f );		// flat truth
	GateImg ana = rt;
	for( int y = 50; y < H; y++ )		// a hard 0.15 drop across the whole width
		for( int x = 0; x < W; x++ )
		{
			ana.At( x, y ) = 0.85f;
		}
	std::vector<GateDefect> d;
	GateGrain( ana, rt, AllValid(), nullptr, cfg, d );
	CHECK( Count( d, GATE_STEP ) >= 1 );
	CHECK( Count( d, GATE_ANT ) == 0 );
}

// GRAIN gate: analytic high-frequency that the TRUE field also has (both structured) is a real feature,
// not a defect - the denoised-RT-flat test excludes it.
TEST( SoftShadowGate, grain_real_feature_in_both_not_flagged )
{
	GateCfg cfg = TestCfg();
	GateImg rt( W, H, 1.0f );
	for( int y = 50; y < H; y++ )		// the step is in the TRUTH too
		for( int x = 0; x < W; x++ )
		{
			rt.At( x, y ) = 0.85f;
		}
	GateImg ana = rt;					// analytic matches -> no spurious HF
	std::vector<GateDefect> d;
	GateGrain( ana, rt, AllValid(), nullptr, cfg, d );
	CHECK( d.empty() );
}
