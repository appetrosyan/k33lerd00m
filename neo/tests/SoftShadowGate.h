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

// Soft-shadow GPU-frame DEFECT ANALYZER. Pure image math over shadow-TERM images (1 = lit, 0 = umbra)
// read back from the real renderer: the analytic frame vs the ray-traced reference, a repeat frame
// (temporal), and view-displaced frames (continuity). Every defect is COUNTED INDIVIDUALLY - the gate
// is green iff the total is zero; the count, not a pass ratio, is the output.
//
// Dependency-free (std only) so it is shared verbatim by the engine gate (com_softShadowGate in
// RenderCapture.cpp) and by rbdoom3bfg_tests, where synthetic images with KNOWN defect counts prove
// the counter itself can count - an analyzer whose zero was never shown able to be nonzero is a false
// instrument, which is exactly the failure this suite replaces.

#ifndef __SOFTSHADOWGATE_H__
#define __SOFTSHADOWGATE_H__

#include <vector>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <functional>

namespace swgate
{

struct GateImg
{
	int W = 0, H = 0;
	std::vector<float> t;
	GateImg() {}
	GateImg( int w, int h, float v = 0.0f ) : W( w ), H( h ), t( ( size_t )w * h, v ) {}
	float At( int x, int y ) const
	{
		return t[( size_t )y * W + x];
	}
	float& At( int x, int y )
	{
		return t[( size_t )y * W + x];
	}
	bool Valid() const
	{
		return W > 0 && H > 0 && ( int )t.size() == W * H;
	}
};

enum gateKind_t
{
	GATE_TURD = 0,		// dark where RT says lit, large connected blob
	GATE_ANT,			// dark where RT says lit, small speckle
	GATE_LIT_IN_UMBRA,	// lit where RT says umbra
	GATE_STEP,			// discontinuity (cliff) where the RT penumbra is a smooth ramp
	GATE_EXTENT,		// penumbra extent off by more than the tolerance (incl. no penumbra at all)
	GATE_TEMPORAL,		// two identical-state frames differ (jitter/ants)
	GATE_CONTINUITY,	// lit<->umbra flip of a co-visible world point under a small view displacement
	GATE_SEAM,			// surf-cache: spatial step the CACHED term has that the EXACT term does not
	GATE_SETUP,			// the scene could not be reconstructed faithfully - loud, never a silent skip
	GATE_KIND_COUNT
};

inline const char* GateKindName( int k )
{
	static const char* names[GATE_KIND_COUNT] = { "TURD", "ANT", "LIT_IN_UMBRA", "STEP", "EXTENT", "TEMPORAL", "CONTINUITY", "SEAM", "SETUP" };
	return ( k >= 0 && k < GATE_KIND_COUNT ) ? names[k] : "?";
}

struct GateDefect
{
	int kind = 0;
	int area = 0;
	int x0 = 0, y0 = 0, x1 = 0, y1 = 0;		// bbox, inclusive
	// EXTENT diagnosis (0 elsewhere): float64-truth vs analytic penumbra-band sample counts over
	// the region - the DIRECTION of the deviation (ana < truth = under-shadow / band too thin;
	// ana > truth = over-shadow) is the first fact any fix needs.
	int anaPen = 0, truthPen = 0;
};

struct GateCfg
{
	float litT         = 0.98f;		// term above this = lit
	float umbraT       = 0.02f;		// term below this = umbra
	float darkInLit    = 0.90f;		// analytic below this where RT is lit -> defect pixel (the >10% numerical allowance)
	float litInUmbra   = 0.10f;		// analytic above this where RT is umbra -> defect pixel
	float stepAna      = 0.25f;		// analytic neighbour step above this ...
	float stepRt       = 0.05f;		// ... where the RT step is below this = a cliff on a smooth ramp
	float extentTol    = 0.10f;		// penumbra-area ratio may deviate by this much
	float extBandLo    = 0.06f;		// penumbra BODY band for extent AREA counts: excludes the shallow tails,
	float extBandHi    = 0.90f;		// where the analytic's 1/16 sample quantum vs the reference's 1/512 makes
	//								   band MEMBERSHIP a quantization artifact (values 0.94-1.0 flapping across
	//								   0.98) - numerical tail disagreement, which is explicitly not a defect
	int   turdArea1080 = 64;		// blob >= this many px (at 1920x1080, scaled by area) = TURD, smaller = ANT
	int   extentMin1080 = 100;		// RT penumbra regions smaller than this are too small to judge extent
	int   guard        = 2;			// px dilation guard on RT masks (misregistration tolerance)
	float temporalTol  = 1e-6f;		// identical state => identical floats; any real delta is jitter
	float contDepthTol = 2e-3f;		// ndc-depth tolerance for co-visibility under displacement
	float seamTol      = 0.09f;		// cached-vs-exact EXCESS neighbour step above this = cache seam
	//								   (1.5x the 1/16 coverage quantum: the exact term's own quantized
	//								   steps and the bilerp's smooth gradient both stay under it)
	float grainAnaT    = 0.030f;	// GRAIN: analytic term local high-pass (|px - local mean|) above this = high-frequency energy
	float grainRtT     = 0.015f;	// ... AND the DENOISED-RT high-pass below this (true field flat here) = grain, not a real edge
	int   grainRtMedPasses = 2;		// edge-PRESERVING median passes that denoise the noisy 512-ray gate RT (kills per-pixel ray noise, KEEPS real edges - a box blur would erase edges and mint false grain there)
	int   grainHpR     = 1;			// high-pass neighbourhood radius (both ana and denoised-RT)
	int   grainMinArea1080 = 24;	// grain cluster smaller than this (after a 1px dilation) is texture/isolated - not counted
	int   ResScale( int at1080, int W, int H ) const
	{
		double s = ( double )W * H / ( 1920.0 * 1080.0 );
		int v = ( int )( at1080 * s + 0.5 );
		return v < 1 ? 1 : v;
	}
};

// ------------------------------------------------------------------ mask primitives

// square dilation by radius r (separable, two passes)
inline std::vector<uint8_t> GateDilate( const std::vector<uint8_t>& mask, int W, int H, int r )
{
	std::vector<uint8_t> a( mask.size(), 0 ), b( mask.size(), 0 );
	for( int y = 0; y < H; y++ )
	{
		for( int x = 0; x < W; x++ )
		{
			uint8_t v = 0;
			for( int d = -r; d <= r && !v; d++ )
			{
				int xx = x + d;
				if( xx >= 0 && xx < W && mask[( size_t )y * W + xx] )
				{
					v = 1;
				}
			}
			a[( size_t )y * W + x] = v;
		}
	}
	for( int y = 0; y < H; y++ )
	{
		for( int x = 0; x < W; x++ )
		{
			uint8_t v = 0;
			for( int d = -r; d <= r && !v; d++ )
			{
				int yy = y + d;
				if( yy >= 0 && yy < H && a[( size_t )yy * W + x] )
				{
					v = 1;
				}
			}
			b[( size_t )y * W + x] = v;
		}
	}
	return b;
}

struct GateComp
{
	int area = 0;
	int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

// connected components over a byte mask; BFS with an explicit stack (same shape as the SoftShadowDefects
// labeler). eightWay=true connects diagonals - use it for thin defect CHAINS (step lines) so a diagonal
// staircase counts as ONE defect, not one per pixel. Optionally returns the per-pixel label (-1 = none).
inline std::vector<GateComp> GateLabel( const std::vector<uint8_t>& mask, int W, int H,
										bool eightWay = false, std::vector<int>* outLabels = nullptr )
{
	std::vector<GateComp> comps;
	std::vector<int> label( ( size_t )W * H, -1 );
	std::vector<int> stack;
	for( int sy = 0; sy < H; sy++ )
	{
		for( int sx = 0; sx < W; sx++ )
		{
			size_t si = ( size_t )sy * W + sx;
			if( !mask[si] || label[si] >= 0 )
			{
				continue;
			}
			GateComp c;
			c.x0 = c.x1 = sx;
			c.y0 = c.y1 = sy;
			int id = ( int )comps.size();
			label[si] = id;
			stack.clear();
			stack.push_back( ( int )si );
			while( !stack.empty() )
			{
				int i = stack.back();
				stack.pop_back();
				int x = i % W, y = i / W;
				c.area++;
				if( x < c.x0 ) c.x0 = x;
				if( x > c.x1 ) c.x1 = x;
				if( y < c.y0 ) c.y0 = y;
				if( y > c.y1 ) c.y1 = y;
				for( int dy = -1; dy <= 1; dy++ )
				{
					for( int dx = -1; dx <= 1; dx++ )
					{
						if( ( dx == 0 && dy == 0 ) || ( !eightWay && dx != 0 && dy != 0 ) )
						{
							continue;
						}
						int nx = x + dx, ny = y + dy;
						if( nx < 0 || nx >= W || ny < 0 || ny >= H )
						{
							continue;
						}
						size_t ni = ( size_t )ny * W + nx;
						if( mask[ni] && label[ni] < 0 )
						{
							label[ni] = id;
							stack.push_back( ( int )ni );
						}
					}
				}
			}
			comps.push_back( c );
		}
	}
	if( outLabels != nullptr )
	{
		*outLabels = label;
	}
	return comps;
}

inline void GateEmit( const std::vector<GateComp>& comps, int kind, std::vector<GateDefect>& out )
{
	for( const GateComp& c : comps )
	{
		GateDefect d;
		d.kind = kind;
		d.area = c.area;
		d.x0 = c.x0;
		d.y0 = c.y0;
		d.x1 = c.x1;
		d.y1 = c.y1;
		out.push_back( d );
	}
}

// 3x3 median (edge-clamped). Grain (per-pixel-independent noise) does NOT survive a median, so
// |value - median| is LARGE for grain and ~0 for a COHERENT edge/step (which the median preserves) -
// the classic noise-vs-edge discriminator. This is what separates the scanline speckle from the
// discrete term's banding steps (both are analytic high-frequency; only the speckle is "ants").
inline std::vector<float> GateMedian3( const std::vector<float>& s, int W, int H )
{
	std::vector<float> out( s.size(), 0.0f );
	for( int y = 0; y < H; y++ )
		for( int x = 0; x < W; x++ )
		{
			float v[9]; int n = 0;
			for( int dy = -1; dy <= 1; dy++ )
				for( int dx = -1; dx <= 1; dx++ )
				{
					int xx = x + dx, yy = y + dy;
					if( xx >= 0 && xx < W && yy >= 0 && yy < H ) { v[n++] = s[( size_t )yy * W + xx]; }
				}
			for( int a = 1; a < n; a++ ) { float k = v[a]; int b = a - 1; while( b >= 0 && v[b] > k ) { v[b + 1] = v[b]; b--; } v[b + 1] = k; }
			out[( size_t )y * W + x] = v[n / 2];
		}
	return out;
}

// separable box mean (edge-clamped): averages an [r] neighbourhood. Used to denoise the stochastic RT
// and to build the local means for the high-pass grain test.
inline std::vector<float> GateBoxMean( const std::vector<float>& s, int W, int H, int r )
{
	std::vector<float> a( s.size(), 0.0f ), b( s.size(), 0.0f );
	for( int y = 0; y < H; y++ )
		for( int x = 0; x < W; x++ )
		{
			float sum = 0.0f; int n = 0;
			for( int d = -r; d <= r; d++ ) { int xx = x + d; if( xx >= 0 && xx < W ) { sum += s[( size_t )y * W + xx]; n++; } }
			a[( size_t )y * W + x] = n ? sum / n : 0.0f;
		}
	for( int y = 0; y < H; y++ )
		for( int x = 0; x < W; x++ )
		{
			float sum = 0.0f; int n = 0;
			for( int d = -r; d <= r; d++ ) { int yy = y + d; if( yy >= 0 && yy < H ) { sum += a[( size_t )yy * W + x]; n++; } }
			b[( size_t )y * W + x] = n ? sum / n : 0.0f;
		}
	return b;
}

// ------------------------------------------------------------------ probe: high-frequency term deviation
// The Fubini scanline term carries high-frequency structure that the true (converged) field does NOT:
// a SPATIALLY STABLE per-pixel speckle ("ants", the discrete grid's rotation-decorrelated discretisation
// error) AND coherent sub-cliff banding STEPS between discrete coverage levels. Both are REAL defects -
// the analytic term has detail the truth lacks - and both are invisible to the value-based agreement
// judge (zero-mean / sub-threshold per pixel) and to an analytic-vs-RT VALUE diff (the gate's RT is a
// 512-ray non-denoised oracle whose OWN grain is stochastic, so a value diff there drowns in ray noise).
// So compare FREQUENCY: flag pixels where the analytic term has local high-pass energy AND the DENOISED
// RT is locally FLAT (box-blurring the noisy RT is a cheap high-ray-count stand-in; a real sharp feature
// survives the blur and is NOT flagged). We do NOT discount the coherent ones - a 3x3 MEDIAN only
// CLASSIFIES kind: a per-pixel speckle does not survive the median (deviation stays high) -> ANT; a
// coherent edge/step DOES survive it (deviation collapses) -> STEP. Both are emitted. Depth creases
// (real geometry edges, legitimately high-frequency) are the only exclusion.
inline void GateGrain( const GateImg& ana, const GateImg& rt, const std::vector<uint8_t>& valid,
					   const std::vector<uint8_t>* crease, const GateCfg& cfg, std::vector<GateDefect>& out )
{
	const int W = ana.W, H = ana.H;
	if( W <= 0 || H <= 0 || rt.W != W || rt.H != H || ( int )valid.size() != W * H ) { return; }
	const std::vector<float> anaMed  = GateMedian3( ana.t, W, H );					// median: survives edges, kills speckle
	const std::vector<float> anaMean = GateBoxMean( ana.t, W, H, cfg.grainHpR );		// total analytic HF energy
	// edge-PRESERVING denoise of the noisy RT: repeated median removes per-pixel ray noise but KEEPS real
	// edges, so hpR reflects genuine structure. A box blur would smear a true edge into a ramp -> hpR ~ 0
	// there -> a matching analytic edge falsely flagged as grain (a real feature present in BOTH is NOT a
	// defect). This is what makes the RT the frequency reference the user asked for (high-ray = smooth
	// noise, real structure intact) without tracing more rays.
	std::vector<float> rtS = rt.t;
	for( int p = 0; p < cfg.grainRtMedPasses; p++ ) { rtS = GateMedian3( rtS, W, H ); }
	const std::vector<float> rtSMean = GateBoxMean( rtS, W, H, cfg.grainHpR );
	std::vector<uint8_t> grainMask( ( size_t )W * H, 0 ), stepMask( ( size_t )W * H, 0 );
	for( size_t i = 0; i < grainMask.size(); i++ )
	{
		if( !valid[i] ) { continue; }
		if( crease != nullptr && ( *crease )[i] ) { continue; }		// real geometry edge: HF legitimate
		const float hpMean = std::fabs( ana.t[i] - anaMean[i] );	// any analytic high-frequency energy
		const float hpR    = std::fabs( rtS[i]   - rtSMean[i] );	// true (denoised) high-frequency energy
		if( hpMean < cfg.grainAnaT || hpR > cfg.grainRtT ) { continue; }	// no analytic HF, or truth also structured -> not a defect
		const float hpMed = std::fabs( ana.t[i] - anaMed[i] );		// per-pixel component (survives-not-the-median)
		if( hpMed >= cfg.grainAnaT ) { grainMask[i] = 1; }			// speckle -> ANT
		else { stepMask[i] = 1; }									// coherent HF the truth lacks -> STEP (NOT discounted)
	}
	// dilate to bridge gaps, label, emit one defect per cluster above the area floor. Grain is a field
	// (4-connected blobs); steps are contour lines (8-connected chains -> one line = one defect).
	const int minA = cfg.ResScale( cfg.grainMinArea1080, W, H );
	std::vector<GateComp> keep;
	for( const GateComp& c : GateLabel( GateDilate( grainMask, W, H, 1 ), W, H ) ) { if( c.area >= minA ) { keep.push_back( c ); } }
	GateEmit( keep, GATE_ANT, out );
	keep.clear();
	for( const GateComp& c : GateLabel( GateDilate( stepMask, W, H, 1 ), W, H, true ) ) { if( c.area >= minA ) { keep.push_back( c ); } }
	GateEmit( keep, GATE_STEP, out );
}

// ------------------------------------------------------------------ probe: cross-texel seams (surf-cache)
// The surface-fold cache reconstructs the term per world texel; adjacent texels fold DIFFERENT occluder
// subsets, so their reconstructions can disagree along the shared border - a spatial step the exact walk
// does not have. This probe renders the SAME still cached and exact and flags every 4-neighbour pixel
// pair whose CACHED step exceeds the EXACT step by more than seamTol. Distinct from GATE_TEMPORAL
// (same-pixel change across frames) and GATE_STEP (cliff vs the RT reference ramp): SEAM is a
// self-consistency test of the cache against its own exact field, reference-free. Depth creases are
// excluded (geometry edges legitimately step); chains labeled 8-way so one border line = one defect.
inline void GateSeam( const GateImg& cached, const GateImg& exact, const std::vector<uint8_t>& valid,
					  const std::vector<uint8_t>* crease, const GateCfg& cfg, std::vector<GateDefect>& out )
{
	const int W = cached.W, H = cached.H;
	std::vector<uint8_t> seam( ( size_t )W * H, 0 );
	for( int y = 0; y < H; y++ )
	{
		for( int x = 0; x < W; x++ )
		{
			const size_t i = ( size_t )y * W + x;
			if( !valid[i] || ( crease != nullptr && ( *crease )[i] ) )
			{
				continue;
			}
			for( int n = 0; n < 2; n++ )		// forward neighbours only: each pair tested once
			{
				const int nx = x + ( n == 0 ? 1 : 0 );
				const int ny = y + ( n == 0 ? 0 : 1 );
				if( nx >= W || ny >= H )
				{
					continue;
				}
				const size_t j = ( size_t )ny * W + nx;
				if( !valid[j] || ( crease != nullptr && ( *crease )[j] ) )
				{
					continue;
				}
				const float dc = std::fabs( cached.t[i] - cached.t[j] );
				const float de = std::fabs( exact.t[i] - exact.t[j] );
				if( dc - de > cfg.seamTol )
				{
					seam[i] = 1;
				}
			}
		}
	}
	GateEmit( GateLabel( seam, W, H, true ), GATE_SEAM, out );
}

// ------------------------------------------------------------------ probe 1: temporal stability
// Nothing in the scene moved between the two renders, so the two term images must be IDENTICAL.
// Each connected cluster of differing pixels is one defect (jitter; ants surface here too).
inline void GateTemporal( const GateImg& a, const GateImg& b, const std::vector<uint8_t>& valid,
						  const GateCfg& cfg, std::vector<GateDefect>& out )
{
	std::vector<uint8_t> jitter( ( size_t )a.W * a.H, 0 );
	for( size_t i = 0; i < jitter.size(); i++ )
	{
		if( valid[i] && std::fabs( a.t[i] - b.t[i] ) > cfg.temporalTol )
		{
			jitter[i] = 1;
		}
	}
	GateEmit( GateLabel( jitter, a.W, a.H, true ), GATE_TEMPORAL, out );
}

// depth-CREASE mask (dilated): geometry seams/cracks where surfaces meet. The ray-traced reference
// traces from a depth-reconstructed origin with a world-units ray bias, so within that distance of a
// crease it is STRUCTURALLY blind to true contact shadows (and prone to its own acne) - agreement
// classes must not mint defects there. Creases are found as second-difference spikes of the LINEARIZED
// depth (1/(1-ndc) is linear across a plane in screen space, so planar interiors are exactly flat).
inline std::vector<uint8_t> GateCreaseMask( const GateImg& depth, int guard )
{
	const int W = depth.W, H = depth.H;
	std::vector<uint8_t> crease( ( size_t )W * H, 0 );
	auto lin = [&]( size_t i ) -> float
	{
		float d = depth.t[i];
		return 1.0f / ( 1.0f - ( d > 0.999999f ? 0.999999f : d ) );
	};
	for( int y = 1; y < H - 1; y++ )
	{
		for( int x = 1; x < W - 1; x++ )
		{
			size_t i = ( size_t )y * W + x;
			float c = lin( i );
			float ddx2 = std::fabs( lin( i - 1 ) + lin( i + 1 ) - 2.0f * c );
			float ddy2 = std::fabs( lin( i - W ) + lin( i + W ) - 2.0f * c );
			float g = std::fabs( lin( i + 1 ) - lin( i - 1 ) ) + std::fabs( lin( i + W ) - lin( i - W ) ) + 1e-4f;
			if( ddx2 + ddy2 > 0.25f * g )
			{
				crease[i] = 1;
			}
		}
	}
	return GateDilate( crease, W, H, guard );
}

// ------------------------------------------------------------------ probe 3: agreement with the RT reference
// truthAt: optional float64 ground-truth ARBITER, term (1=lit,0=umbra) at a pixel, or <0 for unknown.
// A dark-where-lit / lit-where-umbra component is only a defect if the arbiter AGREES with the RT
// reference there - where the exact trace sides with the analytic image (contact shadows inside the
// reference's ray bias, e.g. crack/seam shadows), the reference is the wrong party and no defect is
// minted. This keeps the count immune to reference blindness without excusing real speckle.
inline void GateAgreement( const GateImg& ana, const GateImg& rt, const std::vector<uint8_t>& valid,
						   const GateCfg& cfg, std::vector<GateDefect>& out,
						   std::vector<uint8_t>* outDefectPx = nullptr,
						   const std::vector<uint8_t>* crease = nullptr,
						   const std::function<float( int, int )>& truthAt = nullptr )
{
	const int W = ana.W, H = ana.H;
	const size_t n = ( size_t )W * H;

	// RT classification with a guard: a pixel only counts as safely-lit (safely-umbra) if its whole
	// guard neighbourhood is - dilating the COMPLEMENT and negating erodes the mask, so misregistration
	// at region boundaries can never mint defects.
	std::vector<uint8_t> rtNotLit( n, 0 ), rtNotUmbra( n, 0 ), rtPen( n, 0 );
	for( size_t i = 0; i < n; i++ )
	{
		rtNotLit[i]   = ( rt.t[i] <= cfg.litT ) ? 1 : 0;
		rtNotUmbra[i] = ( rt.t[i] >= cfg.umbraT ) ? 1 : 0;
		rtPen[i]      = ( rt.t[i] > cfg.umbraT && rt.t[i] < cfg.litT ) ? 1 : 0;
	}
	std::vector<uint8_t> notLitGrown   = GateDilate( rtNotLit, W, H, cfg.guard );
	std::vector<uint8_t> notUmbraGrown = GateDilate( rtNotUmbra, W, H, cfg.guard );
	std::vector<uint8_t> penGrown      = GateDilate( rtPen, W, H, cfg.guard );

	// dark-where-lit: turds and ants. Every connected blob is its own defect - the absolute delta of a
	// blob does not matter, its existence does. Depth creases are excluded when provided: the reference
	// cannot adjudicate contact shadows within its ray bias of a seam.
	std::vector<uint8_t> dark( n, 0 ), lit( n, 0 );
	for( size_t i = 0; i < n; i++ )
	{
		if( !valid[i] || ( crease != nullptr && ( *crease )[i] ) )
		{
			continue;
		}
		if( !notLitGrown[i] && ana.t[i] < cfg.darkInLit )
		{
			dark[i] = 1;
		}
		if( !notUmbraGrown[i] && ana.t[i] > cfg.litInUmbra )
		{
			lit[i] = 1;
		}
	}
	// arbitration sampler: up to 5 pixels per component, median truth decides
	auto arbitrate = [&]( const std::vector<uint8_t>& mask, std::vector<int>& labels, int numComps,
						  bool darkClass ) -> std::vector<bool>
	{
		std::vector<bool> keep( ( size_t )numComps, true );
		if( !truthAt )
		{
			return keep;
		}
		std::vector<int> sampled( ( size_t )numComps, 0 );
		std::vector<float> acc( ( size_t )numComps, 0.0f );
		std::vector<int> got( ( size_t )numComps, 0 );
		for( int y = 0; y < H; y++ )
		{
			for( int x = 0; x < W; x++ )
			{
				size_t i = ( size_t )y * W + x;
				if( !mask[i] || labels[i] < 0 || sampled[labels[i]] >= 5 )
				{
					continue;
				}
				sampled[labels[i]]++;
				float t = truthAt( x, y );
				if( t >= 0.0f )
				{
					acc[labels[i]] += t;
					got[labels[i]]++;
				}
			}
		}
		for( int cI = 0; cI < numComps; cI++ )
		{
			if( got[cI] == 0 )
			{
				continue;    // no verdict, keep the defect
			}
			float t = acc[cI] / got[cI];
			if( darkClass ? ( t <= cfg.litT ) : ( t >= cfg.umbraT ) )
			{
				keep[cI] = false;    // exact truth sides with the analytic image; the reference misjudged
			}
		}
		return keep;
	};

	const int turdArea = cfg.ResScale( cfg.turdArea1080, W, H );
	{
		std::vector<int> darkLabels;
		std::vector<GateComp> darkComps = GateLabel( dark, W, H, false, &darkLabels );
		std::vector<bool> keep = arbitrate( dark, darkLabels, ( int )darkComps.size(), true );
		for( size_t cI = 0; cI < darkComps.size(); cI++ )
		{
			if( !keep[cI] )
			{
				continue;
			}
			const GateComp& c = darkComps[cI];
			GateDefect d;
			d.kind = ( c.area >= turdArea ) ? GATE_TURD : GATE_ANT;
			d.area = c.area;
			d.x0 = c.x0;
			d.y0 = c.y0;
			d.x1 = c.x1;
			d.y1 = c.y1;
			out.push_back( d );
		}
	}
	{
		std::vector<int> litLabels;
		std::vector<GateComp> litComps = GateLabel( lit, W, H, false, &litLabels );
		std::vector<bool> keep = arbitrate( lit, litLabels, ( int )litComps.size(), false );
		for( size_t cI = 0; cI < litComps.size(); cI++ )
		{
			if( keep[cI] )
			{
				GateEmit( { litComps[cI] }, GATE_LIT_IN_UMBRA, out );
			}
		}
	}

	// penumbra discontinuity: inside (or hugging) the RT penumbra, an analytic neighbour STEP where the
	// reference ramps smoothly. Each 8-connected chain of cliff pixels = one defect (a staircase line
	// counts once, not per pixel).
	std::vector<uint8_t> step( n, 0 );
	for( int y = 0; y < H; y++ )
	{
		for( int x = 0; x < W; x++ )
		{
			size_t i = ( size_t )y * W + x;
			if( !valid[i] || !penGrown[i] || ( crease != nullptr && ( *crease )[i] ) )
			{
				continue;
			}
			for( int k = 0; k < 2; k++ )
			{
				int nx = x + ( k == 0 ? 1 : 0 ), ny = y + ( k == 0 ? 0 : 1 );
				if( nx >= W || ny >= H )
				{
					continue;
				}
				size_t j = ( size_t )ny * W + nx;
				if( !valid[j] )
				{
					continue;
				}
				float da = std::fabs( ana.t[i] - ana.t[j] );
				float dr = std::fabs( rt.t[i] - rt.t[j] );
				if( da > cfg.stepAna && dr < cfg.stepRt )
				{
					step[i] = 1;
				}
			}
		}
	}
	GateEmit( GateLabel( step, W, H, true ), GATE_STEP, out );

	// penumbra extent: per RT penumbra REGION (big enough to judge), the analytic penumbra area inside
	// the region's grown bbox must agree within extentTol. Ratio ~0 = no penumbra at all.
	std::vector<int> penLabels;
	std::vector<GateComp> penComps = GateLabel( rtPen, W, H, false, &penLabels );
	const int extentMin = cfg.ResScale( cfg.extentMin1080, W, H );
	for( const GateComp& c : penComps )
	{
		if( c.area < extentMin )
		{
			continue;
		}
		int bx0 = c.x0 - cfg.guard, by0 = c.y0 - cfg.guard, bx1 = c.x1 + cfg.guard, by1 = c.y1 + cfg.guard;
		if( bx0 < 0 ) bx0 = 0;
		if( by0 < 0 ) by0 = 0;
		if( bx1 >= W ) bx1 = W - 1;
		if( by1 >= H ) by1 = H - 1;
		int rtA = 0, anaA = 0, creaseA = 0;
		for( int y = by0; y <= by1; y++ )
		{
			for( int x = bx0; x <= bx1; x++ )
			{
				size_t i = ( size_t )y * W + x;
				if( !valid[i] )
				{
					continue;
				}
				if( rt.t[i] > cfg.extBandLo && rt.t[i] < cfg.extBandHi )
				{
					rtA++;
					if( crease != nullptr && ( *crease )[i] )
					{
						creaseA++;
					}
				}
				if( ana.t[i] > cfg.extBandLo && ana.t[i] < cfg.extBandHi )
				{
					anaA++;
				}
			}
		}
		if( rtA < extentMin )
		{
			continue;    // too few VALID reference-penumbra pixels to judge a 10% area ratio
		}
		if( creaseA * 2 > rtA )
		{
			continue;    // silhouette sliver: the reference's depth-reconstructed origin is unreliable
			//              at creases, so its penumbra extent there cannot indict the renderer
		}
		double ratio = ( double )anaA / rtA;
		if( ratio < 1.0 - cfg.extentTol || ratio > 1.0 + cfg.extentTol )
		{
			// ARBITRATION: the stochastic reference both manufactures phantom penumbra (ray-origin
			// reconstruction acne at grazing - measured hitT~0) and inflates real bands with sample
			// dither, so it only NOMINATES suspect regions; the float64 exact trace JUDGES them.
			// Sample the region uniformly, measure the analytic's penumbra area against the TRUTH's
			// over the same samples, and mint a defect only if the analytic disagrees with the truth
			// beyond the tolerance (widened slightly for binomial sampling noise).
			int anaPenOut = 0, truthPenOut = 0;		// exported for the defect's direction diagnosis
			if( truthAt )
			{
				const int target = 128;
				long boxPx = ( long )( bx1 - bx0 + 1 ) * ( by1 - by0 + 1 );
				const int stride = ( boxPx > target ) ? ( int )( boxPx / target ) : 1;
				long seen = 0;
				int sampled = 0, truthPen = 0, anaPen = 0;
				for( int y = by0; y <= by1; y++ )
				{
					for( int x = bx0; x <= bx1; x++ )
					{
						size_t i = ( size_t )y * W + x;
						if( !valid[i] )
						{
							continue;
						}
						if( ( seen++ % stride ) != 0 )
						{
							continue;
						}
						float t = truthAt( x, y );
						if( t < 0.0f )
						{
							continue;
						}
						sampled++;
						if( t > cfg.extBandLo && t < cfg.extBandHi )
						{
							truthPen++;
						}
						if( ana.t[i] > cfg.extBandLo && ana.t[i] < cfg.extBandHi )
						{
							anaPen++;
						}
					}
				}
				anaPenOut = anaPen;
				truthPenOut = truthPen;
				if( sampled >= 16 )
				{
					if( truthPen == 0 && anaPen == 0 )
					{
						continue;    // neither exact truth nor analytic sees penumbra: reference phantom
					}
					double tr2 = ( double )anaPen / ( double )( truthPen > 0 ? truthPen : 1 );
					if( truthPen > 0 && tr2 > 1.0 - ( cfg.extentTol + 0.05 ) && tr2 < 1.0 + ( cfg.extentTol + 0.05 ) )
					{
						continue;    // analytic agrees with the exact truth: the reference misjudged
					}
				}
			}
			GateDefect d;
			d.kind = GATE_EXTENT;
			d.area = std::abs( anaA - rtA );
			d.x0 = c.x0;
			d.y0 = c.y0;
			d.x1 = c.x1;
			d.y1 = c.y1;
			d.anaPen = anaPenOut;
			d.truthPen = truthPenOut;
			out.push_back( d );
		}
	}

	if( outDefectPx != nullptr )
	{
		outDefectPx->assign( n, 0 );
		for( size_t i = 0; i < n; i++ )
		{
			if( dark[i] )
			{
				( *outDefectPx )[i] = 1;    // dark-where-lit (turd/ant)
			}
			else if( lit[i] )
			{
				( *outDefectPx )[i] = 2;    // lit-where-umbra
			}
			else if( step[i] )
			{
				( *outDefectPx )[i] = 3;    // penumbra step
			}
		}
	}
}

// ------------------------------------------------------------------ probe 2: view continuity
// The shadow lives in world space: sliding the camera a few units must not make a shadowed region
// appear or disappear. Reproject each valid base pixel into the displaced view; where the SAME world
// point is visible in both (depth agrees), a full lit<->umbra flip is a defect pixel; each cluster of
// flips is one defect. Matrix conventions follow the .cap capture (row-major, clip = M * (P,1)).
struct GateView
{
	float mvp[16];		// world -> clip for this view
};

inline bool GateProject( const float* mvp, const float wp[3], int W, int H, float& sx, float& sy, float& ndcZ )
{
	float cx = mvp[0] * wp[0] + mvp[1] * wp[1] + mvp[2] * wp[2] + mvp[3];
	float cy = mvp[4] * wp[0] + mvp[5] * wp[1] + mvp[6] * wp[2] + mvp[7];
	float cz = mvp[8] * wp[0] + mvp[9] * wp[1] + mvp[10] * wp[2] + mvp[11];
	float cw = mvp[12] * wp[0] + mvp[13] * wp[1] + mvp[14] * wp[2] + mvp[15];
	if( cw <= 1e-6f )
	{
		return false;
	}
	sx = ( cx / cw * 0.5f + 0.5f ) * W;
	sy = ( 1.0f - ( cy / cw * 0.5f + 0.5f ) ) * H;
	ndcZ = cz / cw;
	return true;
}

// unproject pixel+ndcDepth to world via the INVERSE of the projecting MVP (same row-major clip=M*(P,1)
// convention). Homogeneous: world_h = inv * (ndc,1), world = world_h.xyz / world_h.w - projectively the
// clip-space w cancels, so no projection Z row is needed. Pass a true inverse of the SAME matrix used to
// project; a matrix from another source/convention (the capture's stored unprojectionToWorldMatrix is
// TRANSPOSED relative to worldMVP) silently yields garbage.
inline void GateUnproject( const float* invMvp, int x, int y, int W, int H, float depth, float out[3] )
{
	float uvx = ( x + 0.5f ) / W, uvy = ( y + 0.5f ) / H;
	float clip[4] = { uvx * 2.0f - 1.0f, 1.0f - uvy * 2.0f, depth, 1.0f };
	float wx = invMvp[0] * clip[0] + invMvp[1] * clip[1] + invMvp[2] * clip[2] + invMvp[3] * clip[3];
	float wy = invMvp[4] * clip[0] + invMvp[5] * clip[1] + invMvp[6] * clip[2] + invMvp[7] * clip[3];
	float wz = invMvp[8] * clip[0] + invMvp[9] * clip[1] + invMvp[10] * clip[2] + invMvp[11] * clip[3];
	float ww = invMvp[12] * clip[0] + invMvp[13] * clip[1] + invMvp[14] * clip[2] + invMvp[15] * clip[3];
	float inv = ( std::fabs( ww ) > 1e-12f ) ? 1.0f / ww : 0.0f;
	out[0] = wx * inv;
	out[1] = wy * inv;
	out[2] = wz * inv;
}

inline void GateContinuity( const GateImg& baseAna, const GateImg& baseDepth, const std::vector<uint8_t>& valid,
							const float* baseInvMvp,
							const GateImg& dispAna, const GateImg& dispDepth, const GateView& dispView,
							const GateCfg& cfg, std::vector<GateDefect>& out,
							const std::vector<uint8_t>* crease = nullptr,
							std::vector<uint8_t>* outDefectPx = nullptr,
							const std::vector<uint8_t>* dispValid = nullptr )
{
	const int W = baseAna.W, H = baseAna.H;
	std::vector<uint8_t> flip( ( size_t )W * H, 0 );
	for( int y = 0; y < H; y++ )
	{
		for( int x = 0; x < W; x++ )
		{
			size_t i = ( size_t )y * W + x;
			// creases excluded: nearest-pixel reprojection lands on the WRONG surface at seams and
			// silhouettes, minting classification "flips" that are lookup error, not shadow popping
			if( !valid[i] || ( crease != nullptr && ( *crease )[i] ) )
			{
				continue;
			}
			float a = baseAna.t[i];
			bool baseLit = a > cfg.litT, baseUmbra = a < cfg.umbraT;
			if( !baseLit && !baseUmbra )
			{
				continue;    // penumbra values may move; only full flips count
			}
			float wp[3];
			GateUnproject( baseInvMvp, x, y, W, H, baseDepth.t[i], wp );
			float sx, sy, ndcZ;
			if( !GateProject( dispView.mvp, wp, W, H, sx, sy, ndcZ ) )
			{
				continue;
			}
			int dx = ( int )sx, dy = ( int )sy;
			if( dx < 0 || dx >= W || dy < 0 || dy >= H )
			{
				continue;
			}
			size_t j = ( size_t )dy * W + dx;
			if( std::fabs( dispDepth.t[j] - ndcZ ) > cfg.contDepthTol )
			{
				continue;    // disocclusion - not the same world point
			}
			if( dispValid != nullptr && !( *dispValid )[j] )
			{
				continue;    // outside the light's interaction coverage in the displaced view (the
				//              scissor edge moved with the camera) - no term exists there to compare
			}
			float b = dispAna.t[j];
			if( ( baseLit && b < cfg.umbraT ) || ( baseUmbra && b > cfg.litT ) )
			{
				flip[i] = 1;
			}
		}
	}
	GateEmit( GateLabel( flip, W, H, true ), GATE_CONTINUITY, out );
	if( outDefectPx != nullptr && ( int )outDefectPx->size() == W * H )
	{
		for( size_t i = 0; i < flip.size(); i++ )
		{
			if( flip[i] )
			{
				( *outDefectPx )[i] = 4;    // continuity flip (cyan in the PPM)
			}
		}
	}
}

// ------------------------------------------------------------------ diagnostics
// annotated P6 dump: analytic term in grey, defect pixels coloured by class (1 red = dark-where-lit,
// 2 yellow = lit-where-umbra, 3 magenta = step), invalid pixels dimmed blue-black.
inline bool GateWritePPM( const char* path, const GateImg& ana, const std::vector<uint8_t>& valid,
						  const std::vector<uint8_t>& defectPx )
{
	FILE* f = std::fopen( path, "wb" );
	if( f == nullptr )
	{
		return false;
	}
	std::fprintf( f, "P6\n%d %d\n255\n", ana.W, ana.H );
	std::vector<uint8_t> row( ( size_t )ana.W * 3 );
	for( int y = 0; y < ana.H; y++ )
	{
		for( int x = 0; x < ana.W; x++ )
		{
			size_t i = ( size_t )y * ana.W + x;
			float v = ana.t[i];
			if( v < 0 ) v = 0;
			if( v > 1 ) v = 1;
			uint8_t g = ( uint8_t )( v * 255.0f );
			uint8_t r = g, gg = g, b = g;
			if( !valid[i] )
			{
				r = gg = 0;
				b = g / 4 + 16;
			}
			else if( !defectPx.empty() )
			{
				switch( defectPx[i] )
				{
					case 1: r = 255; gg = 32; b = 32; break;
					case 2: r = 255; gg = 224; b = 32; break;
					case 3: r = 255; gg = 32; b = 255; break;
					case 4: r = 32; gg = 224; b = 255; break;
				}
			}
			row[( size_t )x * 3 + 0] = r;
			row[( size_t )x * 3 + 1] = gg;
			row[( size_t )x * 3 + 2] = b;
		}
		std::fwrite( row.data(), 1, row.size(), f );
	}
	std::fclose( f );
	return true;
}

// per-kind tally for the report line
inline void GateTally( const std::vector<GateDefect>& defects, int counts[GATE_KIND_COUNT] )
{
	std::memset( counts, 0, sizeof( int ) * GATE_KIND_COUNT );
	for( const GateDefect& d : defects )
	{
		counts[d.kind]++;
	}
}

} // namespace swgate

#endif // __SOFTSHADOWGATE_H__
