/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 2026 Aleksandr Petrosyan

This program is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version. See <http://www.gnu.org/licenses/>.

===========================================================================
*/

// CONTRIBUTOR-CACHE PROTOCOL SIM. A CPU replica of the softterm.cs.hlsl contrib slot protocol
// (claim / eval ticket / append / flip / poison / refinement / serve-defer / per-frame claim
// budget) run under deterministic adversarial interleavings, at micro-op granularity for exactly
// the operations whose ordering the GPU does not sequence: the claim CAS, the ticket add, the
// triCount reserve vs the entry store, and the BUILT flip vs same-dispatch stragglers.
//
// This is NOT a coverage test - there is no geometry. Each fragment carries a synthetic
// "observed contributor set" (a subset of its cell's ground-truth union); the protocol's job is
// to build the union from bounded evaluations and never serve wrong/torn state. The properties
// asserted here are the spec for the TICKET-GATED protocol (the eval ticket taken BEFORE the
// record walk):
//   1. record walks per cell  <=  K' + scheduler concurrency (bounded transient),
//   2. no serve on the flip frame; no serve of a POISONed cell; no torn (zero) entry served,
//   3. the per-frame budget admits min(budget, demand) unique claims - no clog, no overshoot
//      beyond in-flight concurrency,
//   4. refinement: an eval that appends a new contributor revokes serving (POISON) until a
//      converged eval restores it; overflow ( > K entries ) poisons permanently,
//   5. quiescence: once no eval appends anything new, every fragment either serves the full
//      union of what refinement has observed, or its cell is poisoned (exact walk) - never an
//      under-covering serve that refinement cannot repair.
// A negative control runs the SAME machine with the ticket taken AFTER the walk (the v1 shader
// order) and asserts the transient is unbounded there - proving the instrument detects the
// defect this spec exists to kill.

#include "idUnitTest.h"

#include <vector>
#include <set>
#include <cstdint>

namespace swcontrib
{

// ---- tunables mirroring the shader defines (small K so overflow paths are reachable) ----------
static const int K_ENTRIES  = 16;		// SW_CONTRIB_K analogue (entry capacity)
static const uint32_t BUILT  = 0x80000000u;
static const uint32_t POISON = 0x40000000u;

struct Slot
{
	uint32_t keyLo = 0;
	uint32_t ev = 0;				// evalCount | BUILT | POISON | flipFrame<<16
	uint32_t triCount = 0;
	uint32_t entries[K_ENTRIES] = {};	// 1-based tri ids, 0 = reserved-not-stored
};

struct Table
{
	std::vector<Slot> slots;
	uint32_t pool = 0;				// header[0]: per-frame successful-claim counter
	int recordWalks = 0;			// instrument: full record walks executed (the overhead)
	int appends = 0;				// instrument: entry stores (quiescence = appends stop, walks may refine forever)
	int serves = 0;
	Table( int n ) : slots( n ) {}
};

// deterministic LCG - the scheduler's only randomness source (seeded per test run)
struct Rng
{
	uint64_t s;
	explicit Rng( uint64_t seed ) : s( seed * 2654435761u + 1 ) {}
	uint32_t Next()
	{
		s = s * 6364136223846793005ull + 1442695040888963407ull;
		return ( uint32_t )( s >> 33 );
	}
};

// A fragment's interaction with the protocol, decomposed into resumable micro-ops. step() returns
// false when the fragment is done. The scheduler interleaves step() calls across in-flight
// fragments - each step is one "atomic" unit; everything the GPU can reorder is split across steps.
struct Fragment
{
	// inputs
	int cell = 0;						// slot index (direct-mapped: the hash probe is not under test)
	uint32_t key = 0;
	std::vector<int> observed;			// this fragment's true contributor set (1-based ids)
	bool refinementHash = false;		// the 1-in-64 pixel hash analogue
	int frame = 0;

	// protocol config
	bool ticketGate = true;				// spec order (ticket BEFORE walk); false = v1 defect order
	int evalK = 8;						// K' threshold
	uint32_t budget = 0;				// per-frame claim budget

	// state machine
	enum Phase { P_PROBE, P_WALK, P_APPEND_RESERVE, P_APPEND_STORE, P_FINALIZE, P_DONE };
	Phase phase = P_PROBE;
	bool recording = false;
	bool appendedNew = false;
	uint32_t myTicket = 0xFFFFFFFFu;
	size_t appendIdx = 0;				// cursor over `observed` during append
	int pendingStore = -1;				// reserved entry index awaiting store (the torn window)
	int pendingVal = 0;

	// outputs
	bool served = false;
	std::set<int> servedUnion;			// what a serve read (must never contain 0/torn garbage)
	bool plainWalk = false;

	bool Step( Table& t )
	{
		Slot& s = t.slots[cell];
		switch( phase )
		{
			case P_PROBE:
			{
				if( s.keyLo == 0 )
				{
					// claim CAS: racy pool pre-check then increment on success (mirrors the shader)
					if( t.pool < budget )
					{
						s.keyLo = key;		// single-threaded scheduler = the CAS always wins here
						t.pool++;
						recording = true;
						phase = P_WALK;
					}
					else
					{
						plainWalk = true;	// budget exhausted
						phase = P_DONE;
					}
					return phase != P_DONE;
				}
				const uint32_t ev = s.ev;
				const uint32_t flipFrame = ( ev >> 16 ) & 0x7FFFu;
				if( ( ev & POISON ) != 0 && ( ev & BUILT ) == 0 )
				{
					plainWalk = true;		// pre-flip overflow: never record, never serve
					phase = P_DONE;
					return false;
				}
				if( ( ev & BUILT ) != 0 && ( ev & POISON ) == 0 && flipFrame != ( uint32_t )( frame & 0x7FFF ) )
				{
					if( refinementHash )
					{
						recording = true;	// refinement: exact walk + append check
						phase = P_WALK;
						return true;
					}
					// SERVE: snapshot the entries, skipping torn zeros
					served = true;
					t.serves++;
					const uint32_t have = s.triCount < ( uint32_t )K_ENTRIES ? s.triCount : ( uint32_t )K_ENTRIES;
					for( uint32_t i = 0; i < have; i++ )
					{
						if( s.entries[i] != 0 )
						{
							servedUnion.insert( ( int )s.entries[i] );
						}
					}
					phase = P_DONE;
					return false;
				}
				if( ( ev & BUILT ) != 0 && ( ev & POISON ) != 0 )
				{
					// BUILT+POISON (refinement revoked): spec = only the refinement hash records
					if( refinementHash )
					{
						recording = true;
						phase = P_WALK;
						return true;
					}
					plainWalk = true;
					phase = P_DONE;
					return false;
				}
				if( ( ev & BUILT ) != 0 )
				{
					// BUILT but not serveable THIS frame (flip-frame defer / poison handled above).
					// SPEC: plain walk - no ticket, no recording. v1 let these stragglers record
					// (the unbounded "record-through-flip"), which the negative control preserves.
					if( ticketGate )
					{
						plainWalk = true;
						phase = P_DONE;
						return false;
					}
					recording = true;
				}
				else if( ticketGate )
				{
					// SPEC ORDER: take the eval ticket BEFORE the walk; losers plain-walk
					myTicket = s.ev & 0xFFFFu;
					if( ( int )myTicket >= evalK )
					{
						plainWalk = true;	// tickets exhausted, waiting for flip
						phase = P_DONE;
						return false;
					}
					s.ev++;					// the InterlockedAdd
					recording = true;
				}
				else
				{
					recording = true;		// v1 order: everyone records, ticket taken at the end
				}
				phase = P_WALK;
				return true;
			}
			case P_WALK:
			{
				t.recordWalks++;			// THE overhead being bounded
				appendIdx = 0;
				appendedNew = false;
				phase = P_APPEND_RESERVE;
				return true;
			}
			case P_APPEND_RESERVE:
			{
				// dedup scan + reserve, one observed contributor per step
				while( appendIdx < observed.size() )
				{
					const int id = observed[appendIdx];
					const uint32_t have = s.triCount < ( uint32_t )K_ENTRIES ? s.triCount : ( uint32_t )K_ENTRIES;
					bool dup = false;
					for( uint32_t i = 0; i < have && !dup; i++ )
					{
						dup = ( s.entries[i] == ( uint32_t )id );
					}
					appendIdx++;
					if( dup )
					{
						continue;
					}
					appendedNew = true;
					const uint32_t at = s.triCount++;	// InterlockedAdd reserve
					if( at < ( uint32_t )K_ENTRIES )
					{
						pendingStore = ( int )at;		// store happens NEXT step: the torn window
						pendingVal = id;
						phase = P_APPEND_STORE;
					}
					else
					{
						s.ev |= POISON;					// overflow
					}
					return true;
				}
				phase = P_FINALIZE;
				return true;
			}
			case P_APPEND_STORE:
			{
				t.appends++;
				s.entries[pendingStore] = ( uint32_t )pendingVal;
				pendingStore = -1;
				phase = P_APPEND_RESERVE;
				return true;
			}
			case P_FINALIZE:
			{
				const uint32_t evNow = s.ev;
				// refinement outcome resolves ONLY for ticketless fragments: a ticketed straggler
				// finishing after the flip legitimately extended the union and must not poison
				const bool ticketed = ticketGate && myTicket != 0xFFFFFFFFu;
				if( ( evNow & BUILT ) != 0 && !ticketed )
				{
					// refinement outcome (or v1 straggler on a flipped cell)
					if( appendedNew )
					{
						s.ev |= POISON;					// revoke serving
					}
					else if( ( evNow & POISON ) != 0 && s.triCount <= ( uint32_t )K_ENTRIES )
					{
						s.ev &= ~POISON;				// converged: restore service
					}
					phase = P_DONE;
					return false;
				}
				if( !ticketGate )
				{
					myTicket = s.ev & 0xFFFFu;			// v1: ticket AFTER the walk
					s.ev++;
				}
				if( ( int )myTicket + 1 == evalK && s.triCount <= ( uint32_t )K_ENTRIES )
				{
					// the K'-th evaluator flips BUILT with the flip-frame stamp
					s.ev = ( s.ev & 0xFFFFu ) | BUILT | ( ( uint32_t )( frame & 0x7FFF ) << 16 );
				}
				phase = P_DONE;
				return false;
			}
			default:
				return false;
		}
	}
};

// scheduler: run `frags` to completion with `width` in flight, interleaved by rng
static void RunInterleaved( Table& t, std::vector<Fragment>& frags, int width, Rng& rng )
{
	std::vector<size_t> inflight;
	size_t next = 0;
	while( next < frags.size() || !inflight.empty() )
	{
		while( ( int )inflight.size() < width && next < frags.size() )
		{
			inflight.push_back( next++ );
		}
		if( inflight.empty() )
		{
			break;
		}
		const size_t pick = rng.Next() % inflight.size();
		if( !frags[inflight[pick]].Step( t ) )
		{
			inflight.erase( inflight.begin() + ( long )pick );
		}
	}
}

// build one frame's fragment batch for a cell: nFrags fragments, each observing a deterministic
// subset of the cell's ground-truth union (all of it collectively within the first evalK frags)
static std::vector<Fragment> MakeBatch( int cell, int frame, int nFrags, const std::vector<int>& truth,
										bool ticketGate, int evalK, uint32_t budget, Rng& rng )
{
	std::vector<Fragment> v;
	v.reserve( nFrags );
	for( int i = 0; i < nFrags; i++ )
	{
		Fragment f;
		f.cell = cell;
		f.key = ( uint32_t )( cell + 1 );
		f.frame = frame;
		f.ticketGate = ticketGate;
		f.evalK = evalK;
		f.budget = budget;
		f.refinementHash = ( ( i * 7 + frame * 13 ) & 63 ) == 0;
		// observed subset: 3 contributors starting at a rotating offset -> the union of any evalK
		// consecutive fragments covers `truth` when evalK >= truth.size()
		for( int k = 0; k < 3; k++ )
		{
			f.observed.push_back( truth[( i + k * ( int )( rng.Next() % 3 + 1 ) ) % truth.size()] );
		}
		v.push_back( f );
	}
	return v;
}

}	// namespace swcontrib

using namespace swcontrib;

// 1 + 3: bounded record transient under the ticket gate, budget admits demand, across seeds
TEST( SoftContribProtocol, TicketGateBoundsRecordWalks )
{
	for( uint64_t seed = 1; seed <= 24; seed++ )
	{
		Rng rng( seed );
		Table t( 4 );
		const int WIDTH = 16;			// in-flight concurrency (the overshoot bound)
		const int EVALK = 8;
		const std::vector<int> truth = { 1, 2, 3, 4, 5, 6 };
		int walksFrame0;
		{
			std::vector<Fragment> b = MakeBatch( 0, /*frame*/ 0, /*nFrags*/ 256, truth, true, EVALK, /*budget*/ 8, rng );
			RunInterleaved( t, b, WIDTH, rng );
			walksFrame0 = t.recordWalks;
			// SPEC 1: the claim-frame transient is bounded by K' + concurrency, NOT by fragment count
			CHECK( walksFrame0 <= EVALK + WIDTH );
			// SPEC 3: exactly one unique claim consumed (one cell)
			CHECK( t.pool == 1 );
			// no serve on the flip frame (SPEC 2): nothing served in frame 0
			for( const Fragment& f : b )
			{
				CHECK_FALSE( f.served );
			}
		}
		{
			t.pool = 0;					// per-frame CPU reset
			std::vector<Fragment> b = MakeBatch( 0, /*frame*/ 1, 256, truth, true, EVALK, 8, rng );
			RunInterleaved( t, b, WIDTH, rng );
			// steady state: recording is refinement-only (SPEC 1 steady form)
			int refiners = 0;
			for( const Fragment& f : b )
			{
				refiners += f.refinementHash ? 1 : 0;
			}
			CHECK( t.recordWalks - walksFrame0 <= refiners );
			// serves happened and never contain a torn zero or an id outside truth (SPEC 2)
			int served = 0;
			for( const Fragment& f : b )
			{
				if( f.served )
				{
					served++;
					for( int id : f.servedUnion )
					{
						CHECK( id >= 1 && id <= ( int )truth.size() );
					}
				}
			}
			CHECK( served > 0 );
		}
	}
}

// negative control: the v1 order (ticket AFTER the walk) is unbounded - every fragment of the
// claim dispatch records. Proves this harness detects the defect the spec kills.
TEST( SoftContribProtocol, V1OrderTransientIsUnbounded )
{
	Rng rng( 7 );
	Table t( 4 );
	const std::vector<int> truth = { 1, 2, 3, 4, 5, 6 };
	std::vector<Fragment> b = MakeBatch( 0, 0, 256, truth, /*ticketGate*/ false, 8, 8, rng );
	RunInterleaved( t, b, 16, rng );
	CHECK( t.recordWalks > 8 + 16 );	// far beyond K' + concurrency: the live 49.5ms-vs-30.9 defect
}

// 3: budget clamps unique claims per frame to min(budget, demand); next frame admits the rest
TEST( SoftContribProtocol, BudgetAdmitsDemandWithoutClog )
{
	Rng rng( 11 );
	Table t( 64 );
	const std::vector<int> truth = { 1, 2, 3 };
	// 32 distinct cells demand claims, budget 8: exactly 8 unique claims this frame
	std::vector<Fragment> all;
	for( int c = 0; c < 32; c++ )
	{
		std::vector<Fragment> b = MakeBatch( c, 0, 2, truth, true, 8, /*budget*/ 8, rng );
		all.insert( all.end(), b.begin(), b.end() );
	}
	RunInterleaved( t, all, 16, rng );
	CHECK( t.pool == 8 );
	int claimed = 0;
	for( const Slot& s : t.slots )
	{
		claimed += ( s.keyLo != 0 ) ? 1 : 0;
	}
	CHECK( claimed == 8 );
	// frame 1: pool resets, the NEXT 8 cells claim - no clog (the held-ticket model's failure)
	t.pool = 0;
	std::vector<Fragment> again;
	for( int c = 0; c < 32; c++ )
	{
		std::vector<Fragment> b = MakeBatch( c, 1, 2, truth, true, 8, 8, rng );
		again.insert( again.end(), b.begin(), b.end() );
	}
	RunInterleaved( t, again, 16, rng );
	claimed = 0;
	for( const Slot& s : t.slots )
	{
		claimed += ( s.keyLo != 0 ) ? 1 : 0;
	}
	CHECK( claimed == 16 );
}

// 4: refinement revoke/restore + overflow poison
TEST( SoftContribProtocol, RefinementRevokesAndOverflowPoisons )
{
	// (a) refinement discovers a missing contributor -> POISON blocks serving until convergence
	{
		Rng rng( 3 );
		Table t( 1 );
		const std::vector<int> truthSmall = { 1, 2, 3 };
		std::vector<Fragment> warm = MakeBatch( 0, 0, 64, truthSmall, true, 4, 8, rng );
		RunInterleaved( t, warm, 8, rng );
		CHECK( ( t.slots[0].ev & BUILT ) != 0 );
		// frame 1: a refinement fragment observes a NEW contributor (7) -> must revoke
		Fragment ref;
		ref.cell = 0;
		ref.key = 1;
		ref.frame = 1;
		ref.refinementHash = true;
		ref.observed = { 7 };
		std::vector<Fragment> b = { ref };
		RunInterleaved( t, b, 1, rng );
		CHECK( ( t.slots[0].ev & POISON ) != 0 );
		// frame 2: non-refinement fragments must NOT serve while poisoned
		std::vector<Fragment> b2 = MakeBatch( 0, 2, 8, truthSmall, true, 4, 8, rng );
		for( Fragment& f : b2 )
		{
			f.refinementHash = false;
		}
		RunInterleaved( t, b2, 8, rng );
		for( const Fragment& f : b2 )
		{
			CHECK_FALSE( f.served );
			CHECK( f.plainWalk );
		}
		// frame 3: a converged refinement eval (nothing new) restores service
		Fragment conv;
		conv.cell = 0;
		conv.key = 1;
		conv.frame = 3;
		conv.refinementHash = true;
		conv.observed = { 1, 7 };		// both already recorded
		std::vector<Fragment> b3 = { conv };
		RunInterleaved( t, b3, 1, rng );
		CHECK( ( t.slots[0].ev & POISON ) == 0 );
	}
	// (b) union larger than K_ENTRIES -> overflow poison, cell never serves
	{
		Rng rng( 5 );
		Table t( 1 );
		std::vector<int> big;
		for( int i = 1; i <= K_ENTRIES + 8; i++ )
		{
			big.push_back( i );
		}
		// every fragment observes a distinct triple -> the union overflows during recording
		std::vector<Fragment> b;
		for( int i = 0; i < 64; i++ )
		{
			Fragment f;
			f.cell = 0;
			f.key = 1;
			f.frame = 0;
			f.evalK = 32;
			f.budget = 8;
			f.observed = { big[( i * 3 ) % ( int )big.size()], big[( i * 3 + 1 ) % ( int )big.size()], big[( i * 3 + 2 ) % ( int )big.size()] };
			b.push_back( f );
		}
		RunInterleaved( t, b, 8, rng );
		CHECK( ( t.slots[0].ev & POISON ) != 0 );
		std::vector<Fragment> b2 = MakeBatch( 0, 1, 8, big, true, 32, 8, rng );
		RunInterleaved( t, b2, 8, rng );
		for( const Fragment& f : b2 )
		{
			CHECK_FALSE( f.served );
		}
	}
}

// 5: quiescence - after appends stop, serving fragments see the full recorded union (subset of
// truth, and covering once refinement has observed everything)
TEST( SoftContribProtocol, QuiescentServesAreComplete )
{
	Rng rng( 9 );
	Table t( 1 );
	const std::vector<int> truth = { 1, 2, 3, 4, 5 };
	int lastAppends = -1;
	// run frames until APPENDS stop growing (refinement walks continue forever by design)
	for( int frame = 0; frame < 32; frame++ )
	{
		t.pool = 0;
		std::vector<Fragment> b = MakeBatch( 0, frame, 128, truth, true, 8, 8, rng );
		RunInterleaved( t, b, 16, rng );
		if( t.appends == lastAppends && frame > 2 )
		{
			// quiescent: every serve this frame returned the complete recorded union
			const Slot& s = t.slots[0];
			std::set<int> recorded;
			for( uint32_t i = 0; i < s.triCount && i < ( uint32_t )K_ENTRIES; i++ )
			{
				if( s.entries[i] != 0 )
				{
					recorded.insert( ( int )s.entries[i] );
				}
			}
			int served = 0;
			for( const Fragment& f : b )
			{
				if( f.served )
				{
					served++;
					CHECK( f.servedUnion == recorded );
				}
			}
			CHECK( served > 0 );
			return;
		}
		lastAppends = t.appends;
	}
	CHECK( false );		// never quiesced
}
