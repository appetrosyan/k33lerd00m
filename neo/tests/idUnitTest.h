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

// Minimal, dependency-free unit-test framework for the engine tree. Tests self-register via the
// TEST() macro (function-local static registry, no static-init-order issues), and RunAllUnitTests()
// executes them - it can be driven standalone (ID_UNIT_TEST_STANDALONE provides main()) so pure-logic
// tests run BEFORE the full game build, or called from an in-engine console command once linked in.
//
// No idlib dependency in the core so it can host both pure-C++ tests and (when linked with idlib/the
// engine) tests that touch engine types. Assertions record a failure and keep going, so one test
// reports every broken check, not just the first.

#ifndef __IDUNITTEST_H__
#define __IDUNITTEST_H__

#include <vector>
#include <string>
#include <cstdio>
#include <cmath>

struct idTestResult
{
	int checks = 0;
	int failures = 0;
	std::string firstFailure;

	void Fail( const char* file, int line, const std::string& expr )
	{
		failures++;
		char buf[64];
		std::snprintf( buf, sizeof( buf ), "%s:%d: ", file, line );
		std::string msg = std::string( buf ) + expr;
		if( firstFailure.empty() )
		{
			firstFailure = msg;
		}
		std::printf( "    FAIL %s\n", msg.c_str() );
	}
};

typedef void ( *idTestFn )( idTestResult& );

struct idUnitTestCase
{
	const char* suite;
	const char* name;
	idTestFn    fn;
	idUnitTestCase( const char* s, const char* n, idTestFn f );
};

// registry is a function-local static so registration order never depends on TU init order.
std::vector<idUnitTestCase*>& idUnitTestRegistry();

// runs every registered test whose suite contains `filter` (NULL = all). Returns the failure count
// (0 = all passed), suitable as a process exit code.
int RunAllUnitTests( const char* filter );

#define TEST( suite, name )                                                              \
	static void suite##_##name##_fn( idTestResult& );                                    \
	static idUnitTestCase suite##_##name##_reg( #suite, #name, suite##_##name##_fn );     \
	static void suite##_##name##_fn( idTestResult& _tr )

// _tr is the hidden idTestResult& each TEST body receives.
#define CHECK( cond )                                                                    \
	do { _tr.checks++; if( !( cond ) ) { _tr.Fail( __FILE__, __LINE__, #cond ); } } while( 0 )

#define CHECK_FALSE( cond ) CHECK( !( cond ) )

#define CHECK_NEAR( a, b, tol )                                                          \
	do {                                                                                 \
		_tr.checks++;                                                                    \
		double _va = ( double )( a ), _vb = ( double )( b ), _vt = ( double )( tol );     \
		if( !( std::fabs( _va - _vb ) <= _vt ) )                                          \
		{                                                                                \
			char _b[256];                                                                \
			std::snprintf( _b, sizeof( _b ), "%s ~= %s  (%.6g vs %.6g, tol %.3g)",        \
					#a, #b, _va, _vb, _vt );                                             \
			_tr.Fail( __FILE__, __LINE__, _b );                                          \
		}                                                                                \
	} while( 0 )

#endif // __IDUNITTEST_H__
