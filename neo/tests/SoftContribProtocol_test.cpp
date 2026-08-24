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

// CONTRIBUTOR-CACHE PROTOCOL SIM, v3 (BUILT-at-claim). A CPU replica of the softterm.cs.hlsl
// contrib slot protocol run under deterministic adversarial interleavings at micro-op granularity
// for the operations the GPU does not sequence (claim CAS, triCount reserve vs entry store,
// pend-frame stamp vs pend write, generation reclaim).
//
// v3 spec (the machine this file is the executable spec for):
//   - CLAIM plants the key + generation; there is no eval counter, no tickets, no BUILT flip.
//   - The ONLY warm path is the TILE-MISS record: a fragment whose tile's LIVE bit is unset runs
//     the full record walk (its whole tile does, that dispatch), then stamps PENDFRAME and pends
//     its tile bit. Pends promote to LIVE only in a later frame with no same-frame finalize.
//   - SERVE requires: key+generation match, not permanently poisoned, tile LIVE bit set.
//   - REFINEMENT: 1-in-N served fragments run the exact record walk instead; a NEW append revokes
//     (POISON), a converged eval (nothing new, triCount <= K) restores. Permanently overflowed
//     cells (POISON && triCount > K) take the plain walk - no refinement, no recording.
//   - GENERATION RECLAIM: a probe finding its cell's slot with a stale generation CAS-reclaims it
//     in place (clear LIVE/PEND/triCount/state, publish the new generation); losers see LIVE=0
//     and become tile-miss recorders.
// NOTE on the deferral negative control: within this sim each frame's batch runs to completion
// before the next frame, so cross-frame incomplete-union serves are unrepresentable by
// construction - the pend/live deferral property is asserted positively (no serve of a tile
// before a later-frame promotion), not by a broken-variant control.

#include "idUnitTest.h"

#include <vector>
#include <set>
#include <cstdint>

namespace swcontrib3
{

static const int K_ENTRIES  = 16;		// SW_CONTRIB_K analogue (entry capacity)
static const uint32_t POISON = 0x40000000u;

struct Slot
{
	uint32_t keyLo = 0;
	uint32_t state = 0;					// POISON bit only (v3: no BUILT, no counters)
	uint32_t triCount = 0;
	uint32_t entries[K_ENTRIES] = {};	// 1-based tri ids, 0 = reserved-not-stored
	uint32_t live = 0;					// promoted tile-coverage mask
	uint32_t pend = 0;					// this-frame pended tile bits
	uint32_t pendFrame = 0xFFFFFFFFu;	// frame of the last pend write
	uint32_t gen = 0;					// slot generation (reclaim on mismatch)
};

struct Table
{
	std::vector<Slot> slots;
	uint32_t pool = 0;					// header[0]: per-frame successful-claim counter
	int recordWalks = 0;
	int appends = 0;
	int serves = 0;
	Table( int n ) : slots( n ) {}
};

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

// A fragment's protocol interaction as resumable micro-ops; the scheduler interleaves Step()
// calls across in-flight fragments. Everything the GPU can reorder is split across steps.
struct Fragment
{
	// inputs
	int cell = 0;
	uint32_t key = 0;
	int tile = 0;						// tile index -> bit (1u << tile), tile < 32
	std::vector<int> observed;			// true contributor set at this fragment (1-based ids)
	bool refinementHash = false;
	int frame = 0;
	uint32_t gen = 0;					// current generation for this cell's light
	uint32_t budget = 0;

	enum Phase { P_PROBE, P_WALK, P_APPEND_RESERVE, P_APPEND_STORE, P_FINALIZE, P_DONE };
	Phase phase = P_PROBE;
	bool tileMissRec = false;
	bool refineRec = false;
	bool appendedNew = false;
	bool appendGaveUp = false;			// CAS contention cap hit: tile must not certify
	int appendTries = 0;				// per-id CAS retry counter
	size_t appendIdx = 0;
	int pendingStore = -1;
	int pendingVal = 0;

	// outputs
	bool served = false;
	std::set<int> servedUnion;
	bool plainWalk = false;
	bool recorded = false;

	bool Step( Table& t )
	{
		Slot& s = t.slots[cell];
		const uint32_t tileBit = 1u << ( uint32_t )tile;
		switch( phase )
		{
			case P_PROBE:
			{
				if( s.keyLo == 0 )
				{
					if( t.pool < budget )
					{
						s.keyLo = key;					// claim CAS (single-threaded scheduler wins)
						s.gen = gen;
						t.pool++;
						tileMissRec = true;				// claimer's tile is uncertified by construction
						phase = P_WALK;
						return true;
					}
					plainWalk = true;
					phase = P_DONE;
					return false;
				}
				// GENERATION RECLAIM = FREE the slot: the winner CASes keyLo to a freeing sentinel
				// (everyone else plain-walks this frame - the sentinel matches neither key nor
				// vacancy), clears state/counts/masks AND the entries (claim-by-value scans to the
				// first zero, so stale non-zero entries would both survive a count reset and break
				// the scan), then publishes keyLo = 0 LAST. The cell re-enters through the normal
				// budget-gated claim next frame - no reader can ever observe a half-cleared slot
				// under a live key.
				if( s.gen != gen )
				{
					s.keyLo = 0xFFFFFFFFu;				// the freeing CAS (single-threaded step = atomic)
					s.state = 0;
					s.triCount = 0;
					s.live = 0;
					s.pend = 0;
					for( int ei = 0; ei < K_ENTRIES; ei++ )
					{
						s.entries[ei] = 0;
					}
					s.gen = 0;
					s.keyLo = 0;						// publish the vacancy LAST
					plainWalk = true;
					phase = P_DONE;
					return false;
				}
				// PROMOTE pends -> live, only when no record finalized this frame
				if( s.pendFrame != ( uint32_t )frame && s.pend != 0 )
				{
					s.live |= s.pend;
				}
				if( ( s.state & POISON ) != 0 && s.triCount >= ( uint32_t )K_ENTRIES )
				{
					plainWalk = true;					// permanent overflow: no serve, no refinement
					phase = P_DONE;
					return false;
				}
				if( ( s.live & tileBit ) == 0 )
				{
					tileMissRec = true;					// tile uncertified: full-tile record
					phase = P_WALK;
					return true;
				}
				if( refinementHash && s.triCount < ( uint32_t )K_ENTRIES )
				{
					refineRec = true;
					phase = P_WALK;
					return true;
				}
				if( ( s.state & POISON ) != 0 )
				{
					plainWalk = true;					// revoked: only refinement records
					phase = P_DONE;
					return false;
				}
				served = true;							// SERVE: snapshot entries (skip torn zeros)
				t.serves++;
				{
					const uint32_t have = s.triCount < ( uint32_t )K_ENTRIES ? s.triCount : ( uint32_t )K_ENTRIES;
					for( uint32_t i = 0; i < have; i++ )
					{
						if( s.entries[i] != 0 )
						{
							servedUnion.insert( ( int )s.entries[i] );
						}
					}
				}
				phase = P_DONE;
				return false;
			}
			case P_WALK:
			{
				t.recordWalks++;
				recorded = true;
				appendIdx = 0;
				appendedNew = false;
				phase = P_APPEND_RESERVE;
				return true;
			}
			case P_APPEND_RESERVE:
			{
				// CLAIM-BY-VALUE append: scan to the first ZERO entry (entries are published by the
				// id CAS itself, so no reserved-unstored gaps exist and the list stays contiguous),
				// then CAS the id into that slot in the NEXT step (the adversarial window). Count-
				// reserve schemes let torn-window duplicates inflate triCount - a full-tile dispatch
				// racing the same 3 contributors blew 6 distinct ids to 16 reservations and
				// spuriously overflow-POISONed the cell (caught by this sim; the v2 shader has the
				// same flaw). triCount becomes an advisory InterlockedMax.
				while( appendIdx < observed.size() )
				{
					const int id = observed[appendIdx];
					if( appendTries >= K_ENTRIES + 16 )
					{
						appendGaveUp = true;			// contention cap: do not certify this tile
						appendTries = 0;
						appendIdx++;
						continue;
					}
					int idx = -1;
					bool dup = false;
					for( int i = 0; i < K_ENTRIES; i++ )
					{
						if( s.entries[i] == ( uint32_t )id )
						{
							dup = true;
							break;
						}
						if( s.entries[i] == 0 )
						{
							idx = i;
							break;
						}
					}
					if( dup )
					{
						appendTries = 0;
						appendIdx++;
						continue;
					}
					if( idx < 0 )
					{
						s.state |= POISON;				// K DISTINCT contributors exceeded: genuine overflow
						s.triCount = ( uint32_t )K_ENTRIES;
						appendTries = 0;
						appendIdx++;
						continue;
					}
					pendingStore = idx;					// CAS attempt happens next step (the race window)
					pendingVal = id;
					phase = P_APPEND_STORE;
					return true;
				}
				phase = P_FINALIZE;
				return true;
			}
			case P_APPEND_STORE:
			{
				// the id CAS: publish-by-value; a loser rescans (the winner's entry may be our id)
				if( s.entries[pendingStore] == 0 )
				{
					s.entries[pendingStore] = ( uint32_t )pendingVal;
					if( s.triCount < ( uint32_t )( pendingStore + 1 ) )
					{
						s.triCount = ( uint32_t )( pendingStore + 1 );	// InterlockedMax analogue
					}
					t.appends++;
					appendedNew = true;
					appendTries = 0;
					appendIdx++;
				}
				else
				{
					appendTries++;						// slot taken since the scan: retry this id
				}
				pendingStore = -1;
				phase = P_APPEND_RESERVE;
				return true;
			}
			case P_FINALIZE:
			{
				if( tileMissRec && !appendGaveUp )
				{
					// stamp FIRST (blocks same-frame promotion), then pend the bit
					s.pendFrame = ( uint32_t )frame;
					s.pend |= tileBit;
				}
				else if( refineRec )
				{
					if( appendedNew )
					{
						s.state |= POISON;				// union was incomplete: revoke serving
					}
					else if( ( s.state & POISON ) != 0 && s.triCount < ( uint32_t )K_ENTRIES )
					{
						s.state &= ~POISON;				// converged: restore
					}
				}
				phase = P_DONE;
				return false;
			}
			default:
				return false;
		}
	}
};

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

// one frame's fragments for a cell: nPerTile fragments in each of nTiles tiles; a tile-T fragment
// observes the per-tile contributor triple truth[T]
static std::vector<Fragment> MakeBatch( int cell, int frame, int nTiles, int nPerTile,
										const std::vector<std::vector<int>>& truth,
										uint32_t gen, uint32_t budget, Rng& rng )
{
	std::vector<Fragment> v;
	v.reserve( ( size_t )nTiles * nPerTile );
	for( int tl = 0; tl < nTiles; tl++ )
	{
		for( int i = 0; i < nPerTile; i++ )
		{
			Fragment f;
			f.cell = cell;
			f.key = ( uint32_t )( cell + 1 );
			f.tile = tl;
			f.frame = frame;
			f.gen = gen;
			f.budget = budget;
			f.refinementHash = ( ( i * 7 + tl * 3 + frame * 13 ) & 63 ) == 0;
			f.observed = truth[tl];
			( void )rng;
			v.push_back( f );
		}
	}
	return v;
}

}	// namespace swcontrib3

using namespace swcontrib3;

// warm shape + steady bound: frame 0 = one full-record dispatch per tile (claim path), frame 1 =
// pends promote and EVERY certified-tile fragment serves the complete union; steady recording is
// refinement-only. Across seeds.
TEST( SoftContribProtocolV3, TileMissRecordsOnceThenServes )
{
	const std::vector<std::vector<int>> truth = { { 1, 2, 3 }, { 4, 5 }, { 6 } };
	for( uint64_t seed = 1; seed <= 24; seed++ )
	{
		Rng rng( seed );
		Table t( 4 );
		const int PER_TILE = 20;
		std::vector<Fragment> f0 = MakeBatch( 0, 0, 3, PER_TILE, truth, /*gen*/ 1, /*budget*/ 8, rng );
		RunInterleaved( t, f0, 16, rng );
		// frame 0: every fragment records (all tiles uncertified) - bounded by the dispatch itself
		CHECK( t.recordWalks == 3 * PER_TILE );
		CHECK( t.pool == 1 );
		for( const Fragment& f : f0 )
		{
			CHECK_FALSE( f.served );		// no serve before promotion (deferral property)
		}
		// frame 1: pends promoted, everyone serves except the refinement hash
		t.pool = 0;
		std::vector<Fragment> f1 = MakeBatch( 0, 1, 3, PER_TILE, truth, 1, 8, rng );
		RunInterleaved( t, f1, 16, rng );
		int served = 0, refined = 0;
		std::set<int> want;
		for( const auto& tt : truth )
		{
			want.insert( tt.begin(), tt.end() );
		}
		for( const Fragment& f : f1 )
		{
			if( f.served )
			{
				served++;
				CHECK( f.servedUnion == want );	// complete union - every tile's contributors present
			}
			if( f.recorded )
			{
				refined++;
			}
		}
		CHECK( served + refined == 3 * PER_TILE );
		CHECK( refined <= 3 );					// steady recording = refinement-only
	}
}

// budget clamps unique claims per frame to min(budget, demand); the rest claim next frame
TEST( SoftContribProtocolV3, BudgetAdmitsDemandWithoutClog )
{
	const std::vector<std::vector<int>> truth = { { 1, 2 } };
	Rng rng( 11 );
	Table t( 64 );
	std::vector<Fragment> all;
	for( int c = 0; c < 32; c++ )
	{
		std::vector<Fragment> b = MakeBatch( c, 0, 1, 2, truth, 1, /*budget*/ 8, rng );
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
	t.pool = 0;
	std::vector<Fragment> again;
	for( int c = 0; c < 32; c++ )
	{
		std::vector<Fragment> b = MakeBatch( c, 1, 1, 2, truth, 1, 8, rng );
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

// refinement revoke/restore + permanent-overflow poison (no serve, no refinement there)
TEST( SoftContribProtocolV3, RefinementRevokesAndOverflowPoisons )
{
	// (a) refinement discovers a missing contributor -> POISON until a converged eval restores
	{
		const std::vector<std::vector<int>> truth = { { 1, 2, 3 } };
		Rng rng( 3 );
		Table t( 1 );
		std::vector<Fragment> warm = MakeBatch( 0, 0, 1, 32, truth, 1, 8, rng );
		RunInterleaved( t, warm, 8, rng );
		Fragment ref;
		ref.cell = 0;
		ref.key = 1;
		ref.tile = 0;
		ref.frame = 1;
		ref.gen = 1;
		ref.refinementHash = true;
		ref.observed = { 7 };					// NEW contributor
		std::vector<Fragment> b = { ref };
		RunInterleaved( t, b, 1, rng );
		CHECK( ( t.slots[0].state & POISON ) != 0 );
		// poisoned: non-refinement fragments plain-walk, never serve
		std::vector<Fragment> b2 = MakeBatch( 0, 2, 1, 8, truth, 1, 8, rng );
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
		// converged refinement (nothing new) restores service
		Fragment conv = ref;
		conv.frame = 3;
		conv.observed = { 1, 7 };
		std::vector<Fragment> b3 = { conv };
		RunInterleaved( t, b3, 1, rng );
		CHECK( ( t.slots[0].state & POISON ) == 0 );
	}
	// (b) union larger than K -> permanent poison: no serves AND no further record walks
	{
		Rng rng( 5 );
		Table t( 1 );
		std::vector<std::vector<int>> big( 1 );
		for( int i = 1; i <= K_ENTRIES + 8; i++ )
		{
			big[0].push_back( i );
		}
		std::vector<Fragment> b = MakeBatch( 0, 0, 1, 8, big, 1, 8, rng );
		RunInterleaved( t, b, 8, rng );
		CHECK( ( t.slots[0].state & POISON ) != 0 );
		CHECK( t.slots[0].triCount >= ( uint32_t )K_ENTRIES );	// CAS-append parks AT capacity on overflow
		const int walksBefore = t.recordWalks;
		std::vector<Fragment> b2 = MakeBatch( 0, 1, 1, 8, big, 1, 8, rng );
		RunInterleaved( t, b2, 8, rng );
		for( const Fragment& f : b2 )
		{
			CHECK_FALSE( f.served );
			CHECK( f.plainWalk );
		}
		CHECK( t.recordWalks == walksBefore );	// permanently poisoned cells stop recording entirely
	}
}

// generation reclaim: a bumped generation clears service in place; old entries are never served
// again, the tiles re-certify, and the new-generation union serves complete
TEST( SoftContribProtocolV3, GenerationReclaimFreesSlot )
{
	Rng rng( 9 );
	Table t( 1 );
	const std::vector<std::vector<int>> truthA = { { 1, 2 } };
	const std::vector<std::vector<int>> truthB = { { 8, 9 } };	// content changed with the bump
	std::vector<Fragment> f0 = MakeBatch( 0, 0, 1, 16, truthA, /*gen*/ 1, 8, rng );
	RunInterleaved( t, f0, 8, rng );
	std::vector<Fragment> f1 = MakeBatch( 0, 1, 1, 16, truthA, 1, 8, rng );
	RunInterleaved( t, f1, 8, rng );
	bool servedOld = false;
	for( const Fragment& f : f1 )
	{
		servedOld |= f.served;
	}
	CHECK( servedOld );
	// generation bump (content changed): frame 2 must NOT serve old entries; tiles re-record
	std::vector<Fragment> f2 = MakeBatch( 0, 2, 1, 16, truthB, /*gen*/ 2, 8, rng );
	RunInterleaved( t, f2, 8, rng );
	for( const Fragment& f : f2 )
	{
		CHECK_FALSE( f.served );				// reclaim cleared LIVE: recording frame, no serves
	}
	CHECK( t.slots[0].gen == 2u );
	// frame 3: the new-generation union serves, containing ONLY new-content contributors
	std::vector<Fragment> f3 = MakeBatch( 0, 3, 1, 16, truthB, 2, 8, rng );
	RunInterleaved( t, f3, 8, rng );
	int served = 0;
	for( const Fragment& f : f3 )
	{
		if( f.served )
		{
			served++;
			for( int id : f.servedUnion )
			{
				CHECK( id == 8 || id == 9 );	// no stale generation-1 entries
			}
		}
	}
	CHECK( served > 0 );
}
