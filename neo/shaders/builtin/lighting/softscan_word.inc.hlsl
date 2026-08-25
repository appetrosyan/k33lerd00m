// Fubini scanline grid word width - the SINGLE switch for the X-axis column resolution, split out so it
// can be included BEFORE the buffer declarations that store the grid (t_SurfGrid / u_SurfGrid) as well as
// before softwedge_coverage.inc.hlsl (which defines the SoftScan_* helpers on top of this typedef).
// Revert to the historical 32-bit grid by setting SW_SCAN_BITS to 32 here - nothing else changes.
#ifndef SOFTSCAN_WORD_INC
#define SOFTSCAN_WORD_INC

#ifndef SW_SCAN_BITS
	#define SW_SCAN_BITS 32		// 32 = uint (one word), 64 = uint2. With the fractional-envelope reduction
	//							   the bitmask carries topology (holes) + the interior quantum; 64 was A/B'd
	//							   against the gate corpus (2026-08-25) and bought only 3 of 176 defects while
	//							   doubling the grid registers - the residual defects were detector artifacts
	//							   (region-boundary statistics), fixed in GateGrain, gate 0 at 32.
#endif

#if SW_SCAN_BITS > 32
typedef uint2 SwGridWord;
#else
typedef uint  SwGridWord;
#endif

#endif // SOFTSCAN_WORD_INC
