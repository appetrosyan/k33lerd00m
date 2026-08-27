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

#ifndef __SOFT_SHADOW_COVERAGE_BENCH_H__
#define __SOFT_SHADOW_COVERAGE_BENCH_H__

// MINIMAL-INIT soft-shadow COVERAGE MICROBENCH (com_softShadowCoverageBench). Mirrors the
// R_SoftShadowGate lifecycle: runs on the already-booted render stack, dispatches the three
// coverage methods (0 max-combine, 1 radial-exact, 2 scanline-exact) over a synthetic scene at
// several caster counts, times each on the GPU (CPU wall-clock, no TimerQuery in this vendored
// nvrhi), reads back per-fragment coverage, cross-checks losslessness/under-shadow, prints a
// table, and returns the correctness-violation count (becomes the process exit code).
//
// The arg string is optional: if it parses as an integer > 1 it overrides the per-dispatch
// work-inflation loop count (loopN) so a dispatch lands in the few-ms timing sweet spot.
int R_SoftShadowCoverageBench( const char* arg );

#endif // __SOFT_SHADOW_COVERAGE_BENCH_H__
