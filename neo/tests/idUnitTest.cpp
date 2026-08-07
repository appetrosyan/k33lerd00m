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

std::vector<idUnitTestCase*>& idUnitTestRegistry()
{
	static std::vector<idUnitTestCase*> registry;
	return registry;
}

idUnitTestCase::idUnitTestCase( const char* s, const char* n, idTestFn f )
	: suite( s ), name( n ), fn( f )
{
	idUnitTestRegistry().push_back( this );
}

int RunAllUnitTests( const char* filter )
{
	int totalTests = 0, failedTests = 0, totalChecks = 0, totalFailures = 0;
	for( idUnitTestCase* tc : idUnitTestRegistry() )
	{
		if( filter != NULL && std::strstr( tc->suite, filter ) == NULL )
		{
			continue;
		}
		idTestResult tr;
		tc->fn( tr );
		totalTests++;
		totalChecks += tr.checks;
		totalFailures += tr.failures;
		if( tr.failures > 0 )
		{
			failedTests++;
			std::printf( "[FAIL] %s.%s  (%d/%d checks failed)\n",
					tc->suite, tc->name, tr.failures, tr.checks );
		}
		else
		{
			std::printf( "[ ok ] %s.%s  (%d checks)\n", tc->suite, tc->name, tr.checks );
		}
	}
	std::printf( "\n%d tests, %d passed, %d failed  (%d checks, %d failures)\n",
			totalTests, totalTests - failedTests, failedTests, totalChecks, totalFailures );
	return totalFailures;
}

#ifdef ID_UNIT_TEST_STANDALONE
int main( int argc, char** argv )
{
	const char* filter = ( argc > 1 ) ? argv[1] : NULL;
	return RunAllUnitTests( filter );
}
#endif
