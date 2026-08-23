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

#include "idUnitTest.h"

#include <cstring>
#include <cstdio>
#include <chrono>

std::vector<idUnitTestCase*>& idUnitTestRegistry()
{
	static std::vector<idUnitTestCase*> registry;
	return registry;
}

idUnitTestCase::idUnitTestCase( const char* s, const char* n, idTestFn f, bool pipe, bool stud )
	: suite( s ), name( n ), fn( f ), pipeline( pipe ), study( stud )
{
	idUnitTestRegistry().push_back( this );
}

int RunAllUnitTests( const char* filter )
{
	// Stream results live: unbuffered stdout so every result AND every in-test progress line flushes
	// immediately even when redirected to a file/pipe (block buffering otherwise hides all output until
	// exit, so a slow suite looks hung and a mid-run crash loses everything printed).
	std::setvbuf( stdout, NULL, _IONBF, 0 );

	int totalTests = 0, failedTests = 0, totalChecks = 0, totalFailures = 0;
	int pipeTests = 0, pipeFailed = 0, unitTests = 0, unitFailed = 0;
	double totalMs = 0.0, unitMs = 0.0;
	// filter: "@unit" runs only the fast isolated tier, "@pipe" only the pipeline gate, "@study" only the
	// heavy CPU frame-emulation studies (excluded from EVERY other run - they are instruments, not tests),
	// otherwise a suite substring. `./rbdoom3bfg_tests @unit` is the seconds-long dev loop.
	const bool onlyUnit  = ( filter != NULL && std::strcmp( filter, "@unit" ) == 0 );
	const bool onlyPipe  = ( filter != NULL && std::strcmp( filter, "@pipe" ) == 0 );
	const bool onlyStudy = ( filter != NULL && std::strncmp( filter, "@study", 6 ) == 0 );
	// "@study:<suite-substring>" runs a SINGLE study instrument with its output streaming live -
	// piping the whole @study tier through grep batches all feedback to the end (user preference:
	// never filter test output through pipes; select in the runner instead).
	const char* studyName = ( onlyStudy && filter[6] == ':' ) ? filter + 7 : NULL;
	int studySkipped = 0;
	for( idUnitTestCase* tc : idUnitTestRegistry() )
	{
		if( onlyStudy )
		{
			if( !tc->study ) { continue; }
			if( studyName != NULL && std::strstr( tc->suite, studyName ) == NULL ) { continue; }
		}
		else if( tc->study ) { studySkipped++; continue; }
		else if( onlyUnit ) { if( tc->pipeline ) { continue; } }
		else if( onlyPipe ) { if( !tc->pipeline ) { continue; } }
		else if( filter != NULL && std::strstr( tc->suite, filter ) == NULL )
		{
			continue;
		}
		// PIPE = drives the shipped rendering pipeline (a real correctness gate); unit = ISOLATED sanity;
		// STDY = CPU frame-emulation instrument (opt-in only).
		const char* tag = tc->study ? "STDY" : ( tc->pipeline ? "PIPE" : "unit" );
		std::printf( "[....] (%s) %s.%s\n", tag, tc->suite, tc->name );	// pre-run marker: a hanging/slow test is identifiable
		idTestResult tr;
		auto _t0 = std::chrono::steady_clock::now();
		tc->fn( tr );
		double ms = std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - _t0 ).count();
		totalMs += ms;
		if( !tc->pipeline ) { unitMs += ms; }
		// A UNIT test over ~10ms is doing heavy MC / full-frame work. Slow does NOT mean it is an integration
		// test (that is about COMPOSING the pipeline, an orthogonal axis) - a slow unit test must be MADE FAST
		// (cut sample/pixel counts) or, if it is an inherently-heavy full-frame measurement, gated out of the
		// fast unit loop. Flag it so the unit tier stays in the seconds budget.
		const char* slow = ( !tc->pipeline && ms > 10.0 ) ? "  <<< SLOW unit (>10ms): make it fast, or gate as a measurement" : "";
		totalTests++;
		totalChecks += tr.checks;
		totalFailures += tr.failures;
		if( tc->pipeline ) { pipeTests++; }
		else { unitTests++; }
		if( tr.failures > 0 )
		{
			failedTests++;
			if( tc->pipeline ) { pipeFailed++; }
			else { unitFailed++; }
			std::printf( "[FAIL] (%s) %s.%s  (%d/%d checks failed, %.1fms)%s\n",
					tag, tc->suite, tc->name, tr.failures, tr.checks, ms, slow );
		}
		else
		{
			std::printf( "[ ok ] (%s) %s.%s  (%d checks, %.1fms)%s\n", tag, tc->suite, tc->name, tr.checks, ms, slow );
		}
	}
	std::printf( "\n%d tests, %d passed, %d failed  (%d checks, %d failures)  [%.0fms total]\n",
			totalTests, totalTests - failedTests, failedTests, totalChecks, totalFailures, totalMs );
	std::printf( "  unit suite time: %.0fms  <-- must stay in the low seconds; MC/full-frame work belongs in PIPELINE tests\n", unitMs );
	// Split the score by class. NOTHING in this binary renders through the GPU pipeline, so no line here is
	// evidence the image is right - the CPU frame-emulation "pipeline" tier stayed green through a total
	// in-game breakage and was demoted to @study for exactly that reason. The renderer verdict lives in the
	// engine gate (printed below).
	std::printf( "  in-process checks: %d passed, %d failed\n", pipeTests - pipeFailed, pipeFailed );
	std::printf( "  unit/sanity (dev only)     : %d passed, %d failed  <-- passing proves NOTHING about the rendered image\n",
			unitTests - unitFailed, unitFailed );
	if( studySkipped > 0 )
	{
		std::printf( "  %d study instrument(s) skipped (CPU frame emulation, not tests) - run with `@study` if you want them\n", studySkipped );
	}
	// The renderer-correctness gate is NOT in this binary: it is the com_softShadowGate ENGINE run, which
	// renders every .cap through the real GPU pipeline vs the RT reference and counts defects.
	std::printf( "  image gate: RBDOOM_HIDDEN_WINDOW=1 ./RBDoom3BFG +set com_softShadowGate corpus  <-- the ONLY renderer verdict\n" );
	return totalFailures;
}

#ifdef ID_UNIT_TEST_STANDALONE
int main( int argc, char** argv )
{
	const char* filter = ( argc > 1 ) ? argv[1] : NULL;
	return RunAllUnitTests( filter );
}
#endif
