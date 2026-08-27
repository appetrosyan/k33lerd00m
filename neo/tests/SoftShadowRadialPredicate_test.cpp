/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 2026 Aleksandr Petrosyan

This file is part of the Doom 3 BFG Edition GPL Source Code.

===========================================================================
*/

// Radial-max ELIGIBILITY PREDICATE proof. A radial-max soft-shadow union (one outer radius per angle,
// per-caster max) reproduces a caster's disk coverage EXACTLY only when the projected silhouette is a
// single CONVEX simple loop that CONTAINS the disk centre O = (0,0). The predicate (SwRadPred_*) is
// accumulated during the wedge's per-caster edge walk from the same disk-projected verts and crossing
// count the walk already forms. This test drives it over a LABELLED silhouette set and proves:
//   (1) CORRECT CLASSIFICATION: eligible == TRUE for exactly the convex+containsO silhouettes.
//   (2) ZERO FALSE POSITIVES (safety-critical): eligible is NEVER true for a silhouette that is really
//       concave, self-intersecting (star/pentagram), holed/multi-part, or has O outside - including the
//       near-degenerate adversaries (nearly-straight turns, a hair-reflex vertex, colinear/duplicate
//       verts, single-triangle silhouettes, O near an edge, O in a pentagram point).
//   (3) MULTI-CHAIN: a caster is several closed chains (a hole wound opposite, a multi-part entity). Any
//       caster with >1 chain is classified NON-eligible - a hole or a disjoint part is not a single
//       convex disk-containing region and radial-max would be lossy for it.
//
// The predicate operates in the wedge's DISK-PROJECTED plane, so this test builds silhouettes directly as
// float2 loops in that plane (the projection SoftShadow_ProjectVert is exercised to death by the FillBox /
// FillHull parity tests - here we isolate the classifier that consumes its output).

#include "hlsl_compat.h"

// Compile the live coverage source as C++, isolated in a namespace so it does not ODR-collide with the
// bodies other test TUs compile. Mirror SoftShadowFillBox.
#define SW_SCANLINE 1
#define SW_SCAN_BITS 32
#ifndef SW_SCAN_CHORDS
	#define SW_SCAN_CHORDS 16
#endif
#define inout
namespace swrad
{
#include "softwedge_coverage.inc.hlsl"
swAttrib_t g_swAttrib = {};
bool       g_swAttribOn = false;
}
using namespace swrad;

#include "idUnitTest.h"

#include <vector>
#include <cmath>

#ifndef M_PI
	#define M_PI 3.14159265358979323846
#endif

using std::vector;

namespace
{
// A caster silhouette in the disk-projected plane: one or more CLOSED vertex loops (chains). A hole or a
// multi-part surface is an extra chain. Vertices are in walk order; the closing edge (last->first of each
// chain) is implicit, exactly as the real edge walk closes an open chain.
typedef vector<float2>          Chain;
typedef vector<Chain>           Silhouette;

// Drive the predicate over a silhouette EXACTLY as SoftShadow_WedgeOcclusion's edge walk would: fold each
// chain's edges (dir = q1 - q0) in order, accumulate swCross from the same SoftDisk_Crossing on the actual
// verts, close each chain, then verdict. Returns eligibility plus the raw predicate state for white-box
// assertions.
struct Verdict { bool eligible; int turnSign; int chains; float dirCross; float swCross; };

Verdict Classify( const Silhouette& s )
{
	swRadPred_t p = SwRadPred_Begin();
	float swCross = 0.0f;
	for( size_t ci = 0; ci < s.size(); ci++ )
	{
		const Chain& ch = s[ci];
		int n = ( int )ch.size();
		for( int i = 0; i < n; i++ )
		{
			float2 a = ch[i];
			float2 b = ch[( i + 1 ) % n];					// closing edge wraps to the first vertex
			p = SwRadPred_Edge( p, b - a );					// edge DIRECTION into the convexity accumulator
			swCross += SoftDisk_Crossing( a, b );			// same winding the walk forms about O
		}
		p = SwRadPred_CloseChain( p );						// chain boundary / caster end
	}
	Verdict v;
	v.eligible = SwRadPred_Eligible( p, swCross );
	v.turnSign = p.turnSign;
	v.chains   = p.chains;
	v.dirCross = p.dirCross;
	v.swCross  = swCross;
	return v;
}

float2 P( float x, float y ) { return float2( x, y ); }

// translate a whole silhouette (to move O inside/outside without changing its shape).
Silhouette Shift( Silhouette s, float dx, float dy )
{
	for( auto& ch : s ) for( auto& v : ch ) { v = v + P( dx, dy ); }
	return s;
}

// --- shape library (all authored CCW, centred near the origin unless shifted) ---------------------------

Chain Square( float r )		// convex, 4 verts
{
	return { P( -r, -r ), P( r, -r ), P( r, r ), P( -r, r ) };
}

Chain Hexagon( float r )	// convex, 6 verts
{
	Chain c;
	for( int i = 0; i < 6; i++ ) { float a = ( float )( 2.0 * M_PI * i / 6.0 ); c.push_back( P( r * std::cos( a ), r * std::sin( a ) ) ); }
	return c;
}

Chain Triangle( float r )	// convex, 3 verts (single-triangle silhouette)
{
	return { P( -r, -r ), P( r, -r ), P( 0, r ) };
}

Chain LShape( float r )		// concave (one reflex vertex), 6 verts, contains origin
{
	return { P( -r, -r ), P( r, -r ), P( r, 0 ), P( 0, 0 ), P( 0, r ), P( -r, r ) };
}

Chain StarConcave( float rOuter, float rInner )	// 10-vert reflex star (NON self-intersecting): concave, contains O
{
	Chain c;
	for( int i = 0; i < 10; i++ )
	{
		float a = ( float )( 2.0 * M_PI * i / 10.0 ) + 1.5707963f;
		float rr = ( i & 1 ) ? rInner : rOuter;
		c.push_back( P( rr * std::cos( a ), rr * std::sin( a ) ) );
	}
	return c;
}

Chain Pentagram( float r )	// 5-vert SELF-INTERSECTING star: consistent turn signs, turning number 2
{
	Chain c;
	for( int i = 0; i < 5; i++ )
	{
		float a = ( float )( 2.0 * M_PI * ( 2 * i ) / 5.0 ) + 1.5707963f;	// step by 2 => pentagram
		c.push_back( P( r * std::cos( a ), r * std::sin( a ) ) );
	}
	return c;
}

// a convex polygon whose verts are ALMOST colinear (a very shallow wedge) - stress the near-straight branch.
Chain NearStraightConvex( float w, float bulge )
{
	return { P( -w, 0 ), P( 0, -0.001f ), P( w, 0 ), P( 0, bulge ) };	// bulge>0 keeps it convex
}

// same silhouette but the bottom vertex bulges the WRONG way by a hair => one genuine reflex => concave.
Chain NearStraightReflex( float w, float dent )
{
	return { P( -w, 0 ), P( 0, dent ), P( w, 0 ), P( 0, w ) };			// dent>0 pokes inward at the bottom edge => reflex
}

// convex square with an extra COLINEAR midpoint vertex on each edge, plus a duplicate vertex.
Chain SquareColinearDup( float r )
{
	return { P( -r, -r ), P( 0, -r ), P( r, -r ), P( r, -r ), P( r, 0 ), P( r, r ), P( 0, r ), P( -r, r ), P( -r, 0 ) };
}
}

// ------------------------------------------------------------------------------------------------------
// (1) CORRECT CLASSIFICATION + (2) ZERO FALSE POSITIVES over the labelled set.
// ------------------------------------------------------------------------------------------------------
TEST( SoftShadowRadialPredicate, classification )
{
	struct Case
	{
		const char* label;
		Silhouette  s;
		bool        trulyConvex;		// ground-truth: single convex simple loop
		bool        trulyContainsO;		// ground-truth: O strictly inside
	};

	const float r = 10.0f;
	vector<Case> cases =
	{
		// --- the ONLY eligible family: convex + contains O ---
		{ "square_convex_containsO",   { Square( r ) },                 true,  true  },
		{ "hexagon_convex_containsO",  { Hexagon( r ) },                true,  true  },
		{ "triangle_convex_containsO", { Triangle( r ) },               true,  true  },
		{ "nearStraight_convex_O",     { NearStraightConvex( r, 4.0f ) }, true, true  },
		{ "squareColinearDup_O",       { SquareColinearDup( r ) },      true,  true  },

		// --- convex but O OUTSIDE (shift the shape off the origin) ---
		{ "square_convex_O_outside",   { Square( r ) },                 true,  false }, // shifted below
		{ "triangle_convex_O_outside", { Triangle( r ) },               true,  false },

		// --- CONCAVE (reflex vertices), O inside ---
		{ "L_concave_containsO",       { LShape( r ) },                 false, true  },
		{ "star_concave_containsO",    { StarConcave( r, 4.0f ) },      false, true  },

		// --- CONCAVE, O outside ---
		{ "L_concave_O_outside",       { LShape( r ) },                 false, false },
		{ "star_concave_O_outside",    { StarConcave( r, 4.0f ) },      false, false },

		// --- SELF-INTERSECTING star (turns all one sign - a bare turn-sign test's blind spot) ---
		{ "pentagram_centre",          { Pentagram( r ) },              false, true  }, // O at centre (winding 2)

		// --- near-degenerate reflex: convex to the eye, but one hair-reflex vertex => concave ---
		{ "nearStraight_reflex_O",     { NearStraightReflex( r, 0.5f ) }, false, true },
	};

	// place O outside for the "outside" cases by shifting the shape well clear of the origin.
	for( auto& c : cases )
	{
		if( !c.trulyContainsO ) { c.s = Shift( c.s, 0.0f, -3.0f * r ); }	// move shape down so O is above it
	}

	// pentagram with O inside a POINT (winding 1, not the centre): the case where |swCross|>=1 AND all turns
	// one sign both fire, so ONLY the turning-number test can still reject it. Added separately with an
	// offset O achieved by shifting the shape a little.
	cases.push_back( { "pentagram_point", { Shift( { Pentagram( r ) }, 0.0f, -6.5f )[0] }, false, true } );

	int eligibleCount = 0;
	std::printf( "\n  %-28s | convex | O-in | dirCross | swCross | chains | turnSign | eligible | expect\n", "label" );
	std::printf( "  -----------------------------+--------+------+----------+---------+--------+----------+----------+-------\n" );
	for( const Case& c : cases )
	{
		Verdict v = Classify( c.s );
		bool expect = c.trulyConvex && c.trulyContainsO;
		std::printf( "  %-28s |   %d    |  %d   |  %+5.1f   |  %+5.1f  |   %d    |    %d     |    %d     |   %d\n",
			c.label, ( int )c.trulyConvex, ( int )c.trulyContainsO, v.dirCross, v.swCross, v.chains, v.turnSign,
			( int )v.eligible, ( int )expect );

		// (1) exact classification: eligible iff truly convex AND truly contains O.
		CHECK( v.eligible == expect );

		// (2) ZERO FALSE POSITIVES: eligible may be true ONLY when the geometry really is convex+containsO.
		CHECK( !v.eligible || ( c.trulyConvex && c.trulyContainsO ) );

		if( v.eligible ) { eligibleCount++; }
	}

	// exactly the 5 convex+containsO silhouettes came back eligible - nothing else slipped through.
	CHECK( eligibleCount == 5 );
}

// ------------------------------------------------------------------------------------------------------
// (3) MULTI-CHAIN handling: a hole (chain wound opposite) and a disjoint part are each >1 chain => NON-
// eligible, even though each individual chain is convex and one of them contains O.
// ------------------------------------------------------------------------------------------------------
TEST( SoftShadowRadialPredicate, multiChain )
{
	// convex outer square containing O, with a smaller square HOLE inside it (wound the other way).
	Chain outer = Square( 10.0f );
	Chain hole  = { P( -3, -3 ), P( -3, 3 ), P( 3, 3 ), P( 3, -3 ) };	// CW = opposite winding
	Verdict holed = Classify( { outer, hole } );
	CHECK( holed.chains == 2 );
	CHECK_FALSE( holed.eligible );			// a holed silhouette is not a convex disk-containing region

	// two DISJOINT convex squares (a multi-part entity): one contains O, the other does not.
	Chain a = Square( 6.0f );										// contains O
	Chain b = Shift( { Square( 4.0f ) }, 40.0f, 0.0f )[0];			// far away, does not
	Verdict multi = Classify( { a, b } );
	CHECK( multi.chains == 2 );
	CHECK_FALSE( multi.eligible );			// >1 chain => never eligible

	// sanity: the SAME outer square ALONE (one chain) is eligible - proves the rejection is the extra chain,
	// not the outer shape.
	Verdict solo = Classify( { outer } );
	CHECK( solo.chains == 1 );
	CHECK( solo.eligible );
}

// ------------------------------------------------------------------------------------------------------
// White-box guards on the three sub-tests, so a regression names WHICH property broke.
// ------------------------------------------------------------------------------------------------------
TEST( SoftShadowRadialPredicate, subtests )
{
	// convex simple loop: turning number is exactly +/-1.
	Verdict sq = Classify( { Square( 10.0f ) } );
	CHECK( std::fabs( std::fabs( sq.dirCross ) - 1.0f ) < 1e-3f );
	CHECK( sq.turnSign != 2 );
	CHECK( std::fabs( sq.swCross ) >= 0.5f );

	// pentagram: turns all one sign (turnSign != 2) yet turning number 2 - the turn-sign test alone would
	// MIS-classify it convex; the |dirCross|==1 test is what rejects it.
	Verdict pg = Classify( { Pentagram( 10.0f ) } );
	CHECK( pg.turnSign != 2 );							// bare convexity test is fooled here
	CHECK( std::fabs( pg.dirCross ) > 1.5f );			// turning number 2 => oneRev false => rejected
	CHECK_FALSE( pg.eligible );

	// L-shape: one reflex vertex => turnSign latched to 2 (inconsistent).
	Verdict l = Classify( { LShape( 10.0f ) } );
	CHECK( l.turnSign == 2 );
	CHECK_FALSE( l.eligible );

	// convex square with O outside: convex passes but winding 0 => containsO false.
	Verdict out = Classify( { Shift( { Square( 5.0f ) }, 0.0f, -40.0f )[0] } );
	CHECK( out.turnSign != 2 );
	CHECK( std::fabs( out.swCross ) < 0.5f );			// O outside => winding 0
	CHECK_FALSE( out.eligible );
}
