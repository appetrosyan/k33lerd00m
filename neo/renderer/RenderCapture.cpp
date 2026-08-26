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

#include "precompiled.h"
#pragma hdrstop

#include "RenderCommon.h"
#include "RenderWorld_local.h"
#include "RenderCapture.h"
#include "../framework/Common_local.h"

#include <sys/DeviceManager.h>

#include <vector>
#include <map>
#include <set>
#include <cstdio>
#include <algorithm>

#include "../tests/SoftShadowGate.h"	// dependency-free defect analyzer, shared with rbdoom3bfg_tests

#if defined(__linux__) || defined(__APPLE__)
	#include <fcntl.h>		// open: the gate's single-instance lock
	#include <sys/file.h>	// flock
#endif

extern DeviceManager* deviceManager;
void Com_SoftShadowGateHeartbeat();		// softgate progress watchdog (Common.cpp)

// soft light's real atlas placement, published by the backend (RenderBackend.cpp) so the tile dump
// reads the tiles the locator actually samples instead of a stale hardcoded offset.
idVec2i g_softDbgAtlasOff[6] = { {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1} };
idVec2i g_softDbgAtlasSize = { -1, -1 };

// --- soft-shadow "goto" ---------------------------------------------------------------------------------------
// Positioning the view at a capture can't be done inside a command handler (re-entrant commonLocal.Frame() unloads
// the map), and a bare setviewpos is overridden by the map's OPENING CINEMATIC camera. So arm a countdown here and
// let the NORMAL frame loop (Common::Frame -> R_SoftShadowGotoTick) skip the cinematic and pin the viewpoint over
// several real frames before the A/B test renders. Usage: `softShadowGoto <cap>` ; `wait 90` ; `testSoftShadowLocator <cap>`.
static idVec3   s_gotoOrg;
static idAngles s_gotoAng;
static int      s_gotoFrames = 0;

// EXPLICIT soft-shadow test config.
//
// The self-test must NOT inherit archived cvars from D3BFGConfig.cfg:
// e.g. r_useRTShadows 1 silently makes the whole soft-wedge path
// inert, so the test/ measures RT instead of what it claims to.
// 
// Pin every shadow-relevant cvar to a documented value BEFORE the
// frontend runs (i.e. from softShadowGoto, which precedes the
// geometry-building frames), and LOUDLY log any that differed from
// the live/archived value so a stale config can never masquerade as a
// code result again.
struct SoftPin { const char* name; const char* value; };
static const SoftPin s_softTestConfig[] =
{
	{ "r_useRTShadows",          "0" },	// CRITICAL: RT off, else the soft-wedge path is inert and RT renders instead
	{ "r_useStencilShadows",     "0" },
	{ "r_useSoftShadowVolumes",  "1" },
	{ "r_useShadowMapping",      "0" },
	{ "r_useShadowAtlas",        "1" },
	{ "r_shadowMapPCSS",         "1" },
	{ "r_shadowMapPCSSScale",    "4" },
	{ "r_shadowMapPCSSAnalyticContact", "1" },	// the SHIPPED term: exact analytic over the penumbra, PCSS only as edge-less fallback (archived 0 = pure-PCSS acne)
	{ "r_softShadowAAM",         "0" },	// PURE PCSS: no AAM band. The PCSS locator alone gates lit/penumbra/umbra.
	{ "r_softShadowBandMask",    "0" },
	{ "r_softShadowStencilOnly", "0" },
	{ "r_softShadowEmergentUmbra", "0" },
	{ "r_softShadowContinuous",  "0" },
	{ "r_useTemporalAA",         "0" },
	{ "r_softShadowDebugShader", "0" },
	// per-light emitter derivation OFF: gate/repro lights are reconstructed from the .cap's stored
	// per-light penumbraSize via the r_shadowPenumbraSize global, which must pass through exactly
	{ "r_shadowPenumbraAuto",    "0" },
};

void R_SoftShadowPinTestConfig( bool verbose )
{
	int overridden = 0;
	if( verbose ) { common->Printf( "[softtest] ==== pinning soft-shadow test config (archived cvars are IGNORED) ====\n" ); }
	for( int i = 0; i < ( int )( sizeof( s_softTestConfig ) / sizeof( s_softTestConfig[0] ) ); i++ )
	{
		const char* was = cvarSystem->GetCVarString( s_softTestConfig[i].name );
		const bool diff = ( idStr::Cmp( was, s_softTestConfig[i].value ) != 0 );
		if( verbose )
		{
			if( diff )	{ common->Printf( "[softtest]   %-26s %s -> %s   <== ARCHIVED VALUE OVERRIDDEN\n", s_softTestConfig[i].name, was, s_softTestConfig[i].value ); }
			else		{ common->Printf( "[softtest]   %-26s = %s\n", s_softTestConfig[i].name, s_softTestConfig[i].value ); }
		}
		if( diff ) { overridden++; }
		cvarSystem->SetCVarString( s_softTestConfig[i].name, s_softTestConfig[i].value );
	}
	if( verbose ) { common->Printf( "[softtest] ==== %d archived cvar(s) overridden ====\n", overridden ); }
}

// ---- shadow-technique cvar CONFLICT WARNINGS -------------------------------------------------------
// Mutually-exclusive / prerequisite relationships between shadow cvars, encoded so a contradictory combo
// WARNS instead of silently no-opping (the bug class that hid the scanline-vs-surfcache no-op and the
// RT-vs-soft precedence). Each rule: when cvar `a`==`aVal` AND cvar `b`==`bVal`, `a`'s intent is defeated.
struct shadowConflict_t { const char* a; int aVal; const char* b; int bVal; const char* why; };
static const shadowConflict_t s_shadowConflicts[] =
{
	{ "r_softShadowScanline",    0, "r_softShadowSurfCache",    1, "the surf permutation is scanline-ALWAYS (task #102): scanline 0 leaves the PLAIN path sampled while served fragments run Fubini - two algorithms in one frame (the fragmentation class that suppressed the caches)" },
	{ "r_softShadowScanline",    1, "r_softShadowFaceCoverage", 0, "scanline lives in the face-coverage path; faceCoverage 0 makes it inert" },
	{ "r_softShadowScanline",    1, "r_softShadowCompute",      0, "the scanline term needs the compute soft-shadow path (softShadowCompute 1)" },
	{ "r_useSoftShadowVolumes",  1, "r_useRTShadows",           1, "RT and soft-shadow volumes are mutually exclusive - RT precedence silently disables the soft path" },
	{ "r_useSoftShadowVolumes",  1, "r_skipShadows",            1, "r_skipShadows disables ALL shadows, soft included" },
	{ "r_useSoftShadowVolumes",  1, "r_softShadowCompute",      0, "the soft-shadow term needs softShadowCompute 1" },
	{ "r_useRTShadows",          1, "r_skipShadows",            1, "r_skipShadows disables ALL shadows, RT included" },
	{ "r_useRTShadows",          1, "r_useShadowMapping",       0, "the RT shadow dispatch keys on shadow-map occluders - needs useShadowMapping 1" },
};
// DEPENDENCY / INERT rules: cvar `cvar` is IGNORED (its value has no effect) while gate `gate`==`gateVal`.
// Warn only when the inert cvar is at a NON-DEFAULT value - the user meaningfully set it but it does nothing.
struct shadowInert_t { const char* cvar; const char* gate; int gateVal; const char* why; };
static const shadowInert_t s_shadowInert[] =
{
	{ "r_softShadowSamples",          "r_softShadowScanline",   1, "the scanline term rasterises exact chords and IGNORES the disk sample count" },
	{ "r_softShadowSamples",          "r_useSoftShadowVolumes", 0, "the disk sample count only affects the analytic soft-shadow path" },
	{ "r_softShadowSamples",          "r_softShadowSurfCache",  1, "the surf permutation is scanline-always (task #102); the disk sample count only affects the legacy sampled A/B baseline" },
	{ "r_softShadowSurfCacheGrid",    "r_softShadowSurfCache",  0, "the surf-cache grid mode is inert while the surf cache is off" },
	{ "r_softShadowSurfCacheReduced", "r_softShadowSurfCache",  0, "reduced-set mode is inert while the surf cache is off" },
	{ "r_rtShadowRays",               "r_useRTShadows",         0, "the RT ray count is inert while RT shadows are off" },
	{ "r_rtShadowSoftRadius",         "r_useRTShadows",         0, "the RT soft radius is inert while RT shadows are off" },
	{ "r_softShadowTermBlur",         "r_softShadowCompute",    0, "the term blur is inert without the compute soft-shadow path" },
};
static const char* const s_shadowWatchCvars[] =
{
	"r_softShadowScanline", "r_softShadowSurfCache", "r_softShadowFaceCoverage", "r_softShadowCompute",
	"r_useSoftShadowVolumes", "r_useRTShadows", "r_skipShadows", "r_useShadowMapping",
	"r_softShadowSamples", "r_softShadowSurfCacheGrid", "r_softShadowSurfCacheReduced", "r_rtShadowRays",
	"r_rtShadowSoftRadius", "r_softShadowTermBlur",
};

// Evaluate every rule and WARN on each active conflict. Returns the conflict count. verbose: also log an
// all-clear line. Callable directly (harness/test config pin) or by the per-frame poll below.
int R_CheckShadowConflicts( bool verbose )
{
	int n = 0;
	for( const shadowConflict_t& c : s_shadowConflicts )
	{
		if( cvarSystem->GetCVarInteger( c.a ) == c.aVal && cvarSystem->GetCVarInteger( c.b ) == c.bVal )
		{
			common->Warning( "shadow config CONFLICT: %s %d + %s %d -> %s", c.a, c.aVal, c.b, c.bVal, c.why );
			n++;
		}
	}
	for( const shadowInert_t& r : s_shadowInert )
	{
		idCVar* cv = cvarSystem->Find( r.cvar );
		if( cv == NULL ) { continue; }
		const bool nonDefault = idStr::Icmp( cv->GetString(), cv->GetDefaultString() ) != 0;
		if( nonDefault && cvarSystem->GetCVarInteger( r.gate ) == r.gateVal )
		{
			common->Warning( "shadow config INERT: %s=%s has no effect while %s %d -> %s",
							 r.cvar, cv->GetString(), r.gate, r.gateVal, r.why );
			n++;
		}
	}
	if( verbose && n == 0 ) { common->Printf( "[shadowcfg] no shadow-technique cvar conflicts\n" ); }
	return n;
}

void R_CheckShadowConflicts_f( const idCmdArgs& args ) { ( void )args; R_CheckShadowConflicts( true ); }

// Per-frame poll (called from Common::Frame): value-cache the watched cvars; when any changes, re-validate
// and warn. Gives "warn the moment a conflicting value is set" without a cvar-change callback.
void R_ShadowConflictTick()
{
	static int  s_last[ sizeof( s_shadowWatchCvars ) / sizeof( s_shadowWatchCvars[0] ) ];
	static bool s_init = false;
	const int N = ( int )( sizeof( s_shadowWatchCvars ) / sizeof( s_shadowWatchCvars[0] ) );
	bool changed = false;
	for( int i = 0; i < N; i++ )
	{
		const int v = cvarSystem->GetCVarInteger( s_shadowWatchCvars[i] );
		if( !s_init || v != s_last[i] ) { changed = true; }
		s_last[i] = v;
	}
	if( changed && s_init ) { R_CheckShadowConflicts( false ); }	// skip the boot-time state; only warn on CHANGES
	s_init = true;
}

void R_SoftShadowGotoTick()
{
	if( s_gotoFrames <= 0 )
	{
		return;
	}
	// re-pin every frame the goto is active: the frontend that builds soft edges/occluders runs on THESE frames,
	// so the config must be correct now, not just at test time (a menu/console tick could otherwise re-archive one).
	R_SoftShadowPinTestConfig( false );
	if( common->Game() != NULL && common->Game()->CheckInCinematic() )
	{
		common->Game()->SkipCinematicScene();		// same path as pressing ESC during the intro
	}
	// EXEC_NOW (not APPEND): this runs right before the game think, so the teleport lands BEFORE the player rebuilds
	// its render view this frame - otherwise the pose is always one frame stale and the no-input usercmd reverts the
	// view angle, so every capture rendered the same (quicksave) view regardless of its yaw.
	cmdSystem->BufferCommandText( CMD_EXEC_NOW, va( "setviewpos %f %f %f %f %f\n",
							s_gotoOrg.x, s_gotoOrg.y, s_gotoOrg.z, s_gotoAng.yaw, s_gotoAng.pitch ) );
	s_gotoFrames--;
}

void R_SoftShadowGoto_f( const idCmdArgs& args )
{
	if( args.Argc() < 2 )
	{
		common->Warning( "usage: softShadowGoto <capture.cap> | <x y z yaw pitch>  (then `wait 90` before testSoftShadowLocator)" );
		return;
	}
	// FREE-CAM form: 5+ args -> raw "x y z yaw pitch", so the harness can render ANY spot in the live map, not just
	// a captured viewpoint (dynamic props / elevated casters that no .cap covers). testSoftShadowLocator takes
	// the same form. No file needed - the goto tick just teleports the player there.
	if( args.Argc() >= 6 )
	{
		s_gotoOrg.Set( atof( args.Argv( 1 ) ), atof( args.Argv( 2 ) ), atof( args.Argv( 3 ) ) );
		s_gotoAng.Set( atof( args.Argv( 5 ) ), atof( args.Argv( 4 ) ), 0.0f );	// (pitch, yaw, roll)
		s_gotoFrames = 120;
		R_SoftShadowPinTestConfig( true );
		common->Printf( "[softtest] goto armed (free-cam): (%.0f %.0f %.0f) yaw %.0f pitch %.0f - pinning for 120 frames\n",
						s_gotoOrg.x, s_gotoOrg.y, s_gotoOrg.z, s_gotoAng.yaw, s_gotoAng.pitch );
		return;
	}
	FILE* cf = fopen( args.Argv( 1 ), "rb" );
	capHeader_t hdr;
	if( cf == NULL || fread( &hdr, sizeof( hdr ), 1, cf ) != 1 || hdr.magic != CAP_MAGIC )
	{
		if( cf != NULL ) { fclose( cf ); }
		common->Warning( "softShadowGoto: cannot read capture %s", args.Argv( 1 ) );
		return;
	}
	fclose( cf );
	s_gotoOrg.Set( hdr.vieworg[0], hdr.vieworg[1], hdr.vieworg[2] );
	s_gotoAng = idVec3( hdr.viewaxis[0], hdr.viewaxis[1], hdr.viewaxis[2] ).ToAngles();
	s_gotoFrames = 120;
	R_SoftShadowPinTestConfig( true );	// pin+log the full config NOW, before the frontend builds geometry
	common->Printf( "[softtest] goto armed: (%.0f %.0f %.0f) yaw %.0f pitch %.0f - skipping cinematic + pinning for 120 frames\n",
					s_gotoOrg.x, s_gotoOrg.y, s_gotoOrg.z, s_gotoAng.yaw, s_gotoAng.pitch );
}

// ---------------------------------------------------------------------------------------------------
// HEADLESS CORPUS RENDER. `softShadowShots <cap1> [cap2 ...]` renders each capture and quits, entirely
// from CODE - no bash orchestration, no `+wait`, no `timeout`, no per-command races (which segfaulted).
// It is a state machine driven by the normal frame loop (R_SoftShadowBatchTick, called next to the goto
// tick): per capture, re-use `softShadowGoto` to pin+build the view for a settle window (real frontend
// work, not idle waiting), then a buffered `dumpHDR` writes shot_<name>.png of that settled frame, then
// advance; after the last, `quit`. The map/save load is paid ONCE up front.
namespace
{
idStrList s_batchCaps;
int  s_batchIdx = -1;
int  s_batchState = 0;			// 0 = settling, 1 = captured (advance next tick)
int  s_batchSettle = 0;
const int SOFT_BATCH_SETTLE = 45;	// frames for the teleport to land + the frontend to (re)build soft edges
idStr R_BatchName( const idStr& path )
{
	idStr n = path;
	n.StripPath();
	n.StripFileExtension();
	return n;
}
}

void R_SoftShadowBatchTick()
{
	if( s_batchIdx < 0 )
	{
		return;
	}
	if( s_batchState == 0 )				// settling: let the pinned view + frontend build, then capture
	{
		if( --s_batchSettle > 0 )
		{
			return;
		}
		cmdSystem->BufferCommandText( CMD_EXEC_APPEND, va( "dumpHDR shot_%s\n", R_BatchName( s_batchCaps[s_batchIdx] ).c_str() ) );
		s_batchState = 1;
		return;
	}
	s_batchIdx++;						// captured: advance to the next capture (or quit)
	if( s_batchIdx < s_batchCaps.Num() )
	{
		// reproduce this capture's DYNAMIC casters (the rock etc.) THEN pin the view - both from the command buffer.
		cmdSystem->BufferCommandText( CMD_EXEC_APPEND, va( "softShadowSpawnCasters %s\n", s_batchCaps[s_batchIdx].c_str() ) );
		cmdSystem->BufferCommandText( CMD_EXEC_APPEND, va( "softShadowGoto %s\n", s_batchCaps[s_batchIdx].c_str() ) );
		s_batchSettle = SOFT_BATCH_SETTLE;
		s_batchState = 0;
	}
	else
	{
		common->Printf( "[softbatch] all captures rendered - quitting\n" );
		cmdSystem->BufferCommandText( CMD_EXEC_APPEND, "quit\n" );
		s_batchIdx = -1;
	}
}

void R_SoftShadowShots_f( const idCmdArgs& args )
{
	if( args.Argc() < 2 )
	{
		common->Warning( "usage: softShadowShots <cap1.cap> [cap2 ...] - renders each headless -> shot_<name>.png, then quits" );
		return;
	}
	s_batchCaps.Clear();
	for( int i = 1; i < args.Argc(); i++ )
	{
		s_batchCaps.Append( idStr( args.Argv( i ) ) );
	}
	s_batchIdx = 0;
	s_batchState = 0;
	s_batchSettle = SOFT_BATCH_SETTLE;
	cmdSystem->BufferCommandText( CMD_EXEC_APPEND, va( "softShadowSpawnCasters %s\n", s_batchCaps[0].c_str() ) );
	cmdSystem->BufferCommandText( CMD_EXEC_APPEND, va( "softShadowGoto %s\n", s_batchCaps[0].c_str() ) );
	common->Printf( "[softbatch] armed %d captures, settle=%d frames each\n", s_batchCaps.Num(), SOFT_BATCH_SETTLE );
}

// forward decls: these are defined later in this TU (R_RenderOneFrame is static file-scope; ReadImageRGBA8
// lives in the anonymous namespace above), and the repro command functions below call them.
static void R_RenderOneFrame();
namespace { bool ReadImageRGBA8( idImage* img, std::vector<uint8_t>& out, int& w, int& h ); }

// ===================================================================================================
// REPRO HARNESS (softShadowRepro <cap...>). Reconstructs each capture's EXACT live state from the .cap's
// embedded savegame, positions the camera, FREEZES the sim, and A/Bs scanline-vs-sampled (+ vs a
// shadows-off baseline) at one identical frozen pose, diffing in-engine. One process, batch, clean exit.
// Differential CANARY: soft-vs-noshadow MUST differ (shadows present) and scanline-vs-sampled MUST differ
// when it does (else the toggle silently no-op'd = instrument error). Menu/fade asserts kill garbage frames.
namespace
{
idStrList s_reproCaps;
int  s_reproIdx  = -1;
int  s_reproState = 0;			// see RS_* below
int  s_reproWait = 0;
int  s_reproFails = 0;			// deviation + instrument-error count (process exit signal)
enum { RS_SETRES = 0, RS_LOAD, RS_WAITLOAD, RS_SETTLE, RS_SHOTWAIT, RS_NEXT };
const int REPRO_LOAD_TIMEOUT = 600;	// frames to reach INGAME before giving up on a capture
const int REPRO_SETTLE       = 45;	// frames for the teleport to land + the frontend to rebuild soft edges
const int REPRO_RESTART_WAIT = 30;	// frames for vid_restart to recreate the render targets before loading

// RESOLUTION MATRIX. Each capture is reproduced at every target resolution, not just the one it was taken at:
// MOC rasterises occluders at half the render res, so resolution-sensitive culls (the near-plane light cull)
// flip with size - a bug present at 1440p can be absent at 1080p and vice-versa, and the shipped renderer must
// be correct at every resolution a player runs. The camera reconstruction (nodal inversion) is resolution-
// independent; only the render size varies (all 16:9, so fov/aspect stay constant). 4K is opt-in (slow, and a
// headless box may lack the VRAM) via r_softShadowRepro4K.
struct ReproRes { int w, h; const char* tag; };
const ReproRes s_reproResList[] = { { 1920, 1080, "1080p" }, { 2560, 1440, "1440p" }, { 3840, 2160, "4k" } };
int s_reproResIdx   = 0;
int s_reproResCount = 2;		// 1080p + 1440p by default; 4K appended when r_softShadowRepro4K is set (arm time)

// RECAPTURE MODE (softShadowRecapture). Reuses the exact repro load->goto->settle path, but at the settle
// stage OVERWRITES each cap in place via `capture` instead of running the A/B shot - regenerating the .cap
// with the current writer (v7 live-light tail). One resolution (the corpus 1440p), no A/B, clean exit.
bool  s_reproRecapture = false;
// When set, `capture` overwrites this exact base instead of allocating the next free capNNNN. Empty = normal.
idStr s_forcedCaptureBase;

// central-band mean luminance of an RGBA8 frame (fade-in / all-dark guard)
double R_CentralLuma( const std::vector<uint8_t>& img, int w, int h )
{
	if( w <= 0 || h <= 0 || ( int )img.size() < w * h * 4 ) { return 0.0; }
	double s = 0.0; int n = 0;
	for( int y = h / 4; y < 3 * h / 4; y++ )
		for( int x = w / 4; x < 3 * w / 4; x++ )
		{
			const uint8_t* p = img.data() + ( ( size_t )y * w + x ) * 4;
			s += ( p[0] + p[1] + p[2] ) * ( 1.0 / 3.0 ); n++;
		}
	return n ? s / n : 0.0;
}

// per-pixel luminance-delta stats between two RGBA8 frames of the same size
struct DiffStats { double meanAbs; int maxAbs; double darkerPct; double brighterPct; int bx0, by0, bx1, by1; };
DiffStats R_LumDiff( const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int w, int h )
{
	DiffStats d = { 0, 0, 0, 0, w, h, -1, -1 };
	if( w <= 0 || h <= 0 || ( int )a.size() < w * h * 4 || ( int )b.size() < w * h * 4 ) { return d; }
	double sum = 0.0; long darker = 0, brighter = 0; const int tot = w * h;
	for( int y = 0; y < h; y++ )
		for( int x = 0; x < w; x++ )
		{
			const size_t o = ( ( size_t )y * w + x ) * 4;
			const int la = ( a[o] + a[o + 1] + a[o + 2] ), lb = ( b[o] + b[o + 1] + b[o + 2] );
			const int dv = ( la - lb ) / 3;
			const int ad = dv < 0 ? -dv : dv;
			sum += ad;
			if( ad > d.maxAbs ) { d.maxAbs = ad; }
			if( dv < -24 ) { darker++;  if( x < d.bx0 ) d.bx0 = x; if( y < d.by0 ) d.by0 = y; if( x > d.bx1 ) d.bx1 = x; if( y > d.by1 ) d.by1 = y; }
			else if( dv > 24 ) { brighter++; if( x < d.bx0 ) d.bx0 = x; if( y < d.by0 ) d.by0 = y; if( x > d.bx1 ) d.bx1 = x; if( y > d.by1 ) d.by1 = y; }
		}
	d.meanAbs = tot ? sum / tot : 0.0;
	d.darkerPct   = tot ? 100.0 * darker   / tot : 0.0;
	d.brighterPct = tot ? 100.0 * brighter / tot : 0.0;
	return d;
}

idStr R_ReproSlot( const idStr& capPath )		// "…/cap0000.cap" -> savegame slot "cap0000"
{
	idStr s = capPath; s.StripPath(); s.StripFileExtension();
	return s;
}
}

// SYNCHRONOUS shot: config already pinned + camera positioned by the tick. Freeze, render the A/B set at the
// identical frozen state, diff in-engine, run the differential canary, report, and dump to dumps/.
// Invert idPlayer::GetViewPos so a plain `setviewpos` lands the RENDER eye EXACTLY on the captured vieworg,
// WITHOUT touching any engine cvar (the harness must render the identical view model the shipped game does).
// The engine builds the eye as: playerOrigin + eyeHeight + viewBob + [ fwd*g_viewNodalX + up*g_viewNodalZ +
// gravity*g_viewNodalZ ] (the "nodal" eye-from-neck offset). setviewpos already inverts the eyeHeight (Z), and
// the freeze zeroes viewBob, so the only residual is the nodal offset. Pre-subtract it - read from the engine's
// OWN cvars so it always matches - and setviewpos + the unchanged GetViewPos reproduce the eye to < 0.25u.
// gravity = (0,0,-1); fwd = viewaxis row0, up = viewaxis row2. Assumes standing (eyeHeight == pm_normalviewheight,
// which setviewpos also assumes) and health > 0; both hold for the repro shot.
static void R_CaptureViewposArg( const capHeader_t& hdr, idVec3& outArg, float& outYaw, float& outPitch )
{
	const idVec3 fwd( hdr.viewaxis[0], hdr.viewaxis[1], hdr.viewaxis[2] );
	const idVec3 up( hdr.viewaxis[6], hdr.viewaxis[7], hdr.viewaxis[8] );
	const float  nx = cvarSystem->GetCVarFloat( "g_viewNodalX" );
	const float  nz = cvarSystem->GetCVarFloat( "g_viewNodalZ" );
	const idVec3 E( hdr.vieworg[0], hdr.vieworg[1], hdr.vieworg[2] );
	outArg.x = E.x - fwd.x * nx - up.x * nz;
	outArg.y = E.y - fwd.y * nx - up.y * nz;
	outArg.z = E.z - 0.25f + nz - up.z * nz;	// gravity*(-nz) removed; setviewpos's +0.25 eyeheight fudge cancelled
	const idAngles a = fwd.ToAngles();
	outYaw = a.yaw; outPitch = a.pitch;
}

void R_SoftShadowReproShot_f( const idCmdArgs& args )
{
	const idStr capPath = args.Argc() > 1 ? args.Argv( 1 ) : "";
	// tag the name with the RENDER resolution: the same capture is reproduced at several resolutions, so prints
	// and dump filenames (repro_<name>_*.png) must stay distinct per resolution instead of overwriting.
	const idStr name = R_ReproSlot( capPath ) + va( "_%dx%d", renderSystem->GetWidth(), renderSystem->GetHeight() );

	// (a) NOT-IN-MENU assert (a render world exists for the loaded map = in-game, not the menu)
	if( tr.primaryWorld == NULL || session == NULL || session->GetState() != idSession::INGAME )
	{
		common->Printf( "[repro] %s: FAIL not-in-game (no world / not INGAME) - skipped\n", name.c_str() );
		return;
	}

	// pin config + validate it is not silently defeated
	R_SoftShadowPinTestConfig( false );
	cvarSystem->SetCVarInteger( "r_useTemporalAA", 0 );
	cvarSystem->SetCVarInteger( "r_softShadowDebugShader", 0 );
	// the pin is the PCSS-LOCATOR baseline; scanline/sampled live in the COMPUTE face-coverage term, so enable
	// that path explicitly (else r_softShadowScanline is inert and the A/B is a no-op - the exact trap we guard).
	cmdSystem->BufferCommandText( CMD_EXEC_NOW, "r_softShadowCompute 1 ; r_softShadowFaceCoverage 1 ; r_softShadowTileBin 1 ; r_softShadowSamples 16\n" );

	// FREEZE first, THEN snap the frozen player to the EXACT captured eye pose. The live goto frames let
	// physics/think settle the player (~6u drift) and view-bob jitter the eye Z - fatal for a knife-edge,
	// view-dependent MOC light cull. g_stopTime kills bob + settle; the post-freeze setviewpos places the eye
	// bit-exactly (setviewpos Z-adjusts by the view height, then Teleport). We still render through
	// game->Draw (the REAL engine, full frontend incl. MOC culling) - only the camera is pinned exactly.
	cvarSystem->SetCVarInteger( "g_stopTime", 1 );
	{
		FILE* cf = fopen( capPath.c_str(), "rb" );
		capHeader_t hdr;
		if( cf != NULL && fread( &hdr, sizeof( hdr ), 1, cf ) == 1 && hdr.magic == CAP_MAGIC )
		{
			// Teleport the player so the UNCHANGED engine's GetViewPos lands the render eye on the captured
			// vieworg (invert the nodal offset - see R_CaptureViewposArg). No engine cvars touched, no noclip:
			// the harness renders the identical view model the shipped game does. Frozen sim => no bob/drift.
			idVec3 argEye; float yaw = 0.0f, pitch = 0.0f;
			R_CaptureViewposArg( hdr, argEye, yaw, pitch );
			cmdSystem->BufferCommandText( CMD_EXEC_NOW, va( "setviewpos %f %f %f %f %f\n",
									argEye.x, argEye.y, argEye.z, yaw, pitch ) );
			capLight_t l0;
			if( hdr.numLights > 0 && fread( &l0, sizeof( l0 ), 1, cf ) == 1 && l0.penumbraSize > 0.0f )
			{
				cvarSystem->SetCVarFloat( "r_shadowPenumbraSize", l0.penumbraSize );
				cvarSystem->SetCVarFloat( "r_rtShadowSoftRadius", l0.penumbraSize );
			}
		}
		if( cf != NULL ) { fclose( cf ); }
	}

	// settle the frozen, exactly-posed view, then FADE / ALL-DARK assert. R_RenderOneFrame runs
	// commonLocal.Draw -> game->Draw -> the REAL game render (full frontend incl. MOC light culling).
	R_RenderOneFrame();
	std::vector<uint8_t> settleImg; int sw = 0, sh = 0;
	ReadImageRGBA8( globalImages->currentRenderHDRImage, settleImg, sw, sh );
	const double luma = R_CentralLuma( settleImg, sw, sh );
	if( luma < 2.0 )
	{
		common->Printf( "[repro] %s: FAIL frame too dark (central luma %.2f) - fade-in / not settled - skipped\n", name.c_str(), luma );
		return;
	}

	std::vector<uint8_t> noShadow, sampled, scanline; int w = 0, h = 0;
	// no-shadow baseline: disable the soft path outright (skipShadows alone does not stop soft volumes)
	cmdSystem->BufferCommandText( CMD_EXEC_NOW, "r_softShadowSurfCache 0 ; r_softShadowScanline 0 ; r_useSoftShadowVolumes 0 ; r_skipShadows 1\n" );
	R_RenderOneFrame();  ReadImageRGBA8( globalImages->currentRenderHDRImage, noShadow, w, h );
	cmdSystem->BufferCommandText( CMD_EXEC_NOW, "r_skipShadows 0 ; r_useSoftShadowVolumes 1 ; r_softShadowScanline 0\n" );	// sampled soft
	R_CheckShadowConflicts( false );
	R_RenderOneFrame();  ReadImageRGBA8( globalImages->currentRenderHDRImage, sampled, w, h );
	cmdSystem->BufferCommandText( CMD_EXEC_NOW, "r_softShadowScanline 1\n" );	// scanline soft
	R_CheckShadowConflicts( false );		// warns here if scanline is inert (e.g. surf cache still on)
	R_RenderOneFrame();  ReadImageRGBA8( globalImages->currentRenderHDRImage, scanline, w, h );

	// diagnostic: per-config luma + soft-edge tally + active config (disambiguates "no soft term" vs stale renders)
	{
		common->Printf( "[repro] %s: luma noShadow %.1f sampled %.1f scanline %.1f | cfg soft=%d compute=%d face=%d tilebin=%d pcss=%d scanline=%d\n",
						name.c_str(), R_CentralLuma( noShadow, w, h ), R_CentralLuma( sampled, w, h ), R_CentralLuma( scanline, w, h ),
						cvarSystem->GetCVarInteger( "r_useSoftShadowVolumes" ), cvarSystem->GetCVarInteger( "r_softShadowCompute" ),
						cvarSystem->GetCVarInteger( "r_softShadowFaceCoverage" ), cvarSystem->GetCVarInteger( "r_softShadowTileBin" ),
						cvarSystem->GetCVarInteger( "r_shadowMapPCSS" ), cvarSystem->GetCVarInteger( "r_softShadowScanline" ) );
	}
	// ASSERTIONS - the diagnostics ARE the test. A meaningless result (config not taken, soft path inert) is a
	// LOUD FAILURE, never a silent pass.
	const DiffStats dSampled    = R_LumDiff( sampled,  noShadow, w, h );	// sampled soft vs no-shadow
	const DiffStats dScanShadow = R_LumDiff( scanline, noShadow, w, h );	// scanline soft vs no-shadow
	const DiffStats dScan       = R_LumDiff( scanline, sampled,  w, h );	// the technique difference (the test)

	const bool cfgOK = cvarSystem->GetCVarInteger( "r_useSoftShadowVolumes" ) == 1 && cvarSystem->GetCVarInteger( "r_softShadowCompute" ) == 1
					   && cvarSystem->GetCVarInteger( "r_softShadowFaceCoverage" ) == 1 && cvarSystem->GetCVarInteger( "r_softShadowScanline" ) == 1;
	const bool softEngaged = dSampled.meanAbs > 1.0 || dScanShadow.meanAbs > 1.0;	// at least one soft path drew shadows

	if( !cfgOK )
	{
		common->Warning( "[repro] %s: ASSERT FAIL - the soft/compute/faceCoverage/scanline config did not take (a cvar conflict is defeating it) - result meaningless", name.c_str() );
		s_reproFails++;
	}
	else if( !softEngaged )
	{
		common->Warning( "[repro] %s: ASSERT FAIL - the soft-shadow path is INERT (both sampled AND scanline render == no-shadow, softEdges=%d). The state/config did not engage soft shadows - cannot test.",
						 name.c_str(), 0 );
		s_reproFails++;
	}
	else
	{
		// scanline and sampled MUST agree closely when both are correct. A large delta - or scanline drawing
		// shadow where sampled draws none (dScanShadow big while dSampled ~0) - is the scanline DEVIATION.
		const bool scanlineOnly = dSampled.meanAbs < 1.0 && dScanShadow.meanAbs > 1.0;
		const bool bug = scanlineOnly || dScan.darkerPct > 2.0 || dScan.maxAbs > 200;
		common->Printf( "[repro] %s: scanline-vs-sampled mean %.2f max %d darker %.2f%% bbox(%d,%d)-(%d,%d) | sampled-shadows %.2f scanline-shadows %.2f -> %s\n",
						name.c_str(), dScan.meanAbs, dScan.maxAbs, dScan.darkerPct, dScan.bx0, dScan.by0, dScan.bx1, dScan.by1,
						dSampled.meanAbs, dScanShadow.meanAbs,
						bug ? ( scanlineOnly ? "DEVIATION (scanline draws shadow sampled does not)" : "DEVIATION (scanline over-dark)" ) : "ok (banding-level)" );
		if( bug ) { s_reproFails++; }
	}

	// dump the pair + heatmap to the dumps dir for eyeballing
	cmdSystem->BufferCommandText( CMD_EXEC_NOW, "r_softShadowScanline 0\n" ); R_RenderOneFrame();
	R_ReadPixelsRGB8( deviceManager->GetDevice(), &backEnd.GetCommonPasses(), globalImages->currentRenderHDRImage->GetTextureHandle(),
					  nvrhi::ResourceStates::ShaderResource, va( "dumps/repro_%s_sampled.png", name.c_str() ) );
	cmdSystem->BufferCommandText( CMD_EXEC_NOW, "r_softShadowScanline 1\n" ); R_RenderOneFrame();
	R_ReadPixelsRGB8( deviceManager->GetDevice(), &backEnd.GetCommonPasses(), globalImages->currentRenderHDRImage->GetTextureHandle(),
					  nvrhi::ResourceStates::ShaderResource, va( "dumps/repro_%s_scanline.png", name.c_str() ) );

	// restore
	cvarSystem->SetCVarInteger( "r_softShadowScanline", 0 );
}

void R_SoftShadowReproTick()
{
	if( s_reproIdx < 0 ) { return; }
	const idStr& cap = s_reproCaps[s_reproIdx];
	switch( s_reproState )
	{
		case RS_SETRES:
		{
			// enter a new target resolution: set the RENDER size (decoupled from the display - gamescope
			// downscales) and vid_restart only if it actually changed. Then run every capture at this size.
			const ReproRes& R = s_reproResList[s_reproResIdx];
			if( renderSystem->GetWidth() != R.w || renderSystem->GetHeight() != R.h )
			{
				cvarSystem->SetCVarInteger( "r_windowWidth", R.w );
				cvarSystem->SetCVarInteger( "r_windowHeight", R.h );
				cmdSystem->BufferCommandText( CMD_EXEC_APPEND, "vid_restart\n" );
				common->Printf( "[repro] === resolution %s (%dx%d) - vid_restart ===\n", R.tag, R.w, R.h );
				s_reproWait = REPRO_RESTART_WAIT;
			}
			else
			{
				common->Printf( "[repro] === resolution %s (%dx%d) ===\n", R.tag, R.w, R.h );
				s_reproWait = 0;
			}
			s_reproIdx = 0;			// restart the capture loop at this resolution
			s_reproState = RS_LOAD;
			break;
		}
		case RS_LOAD:
		{
			if( s_reproWait > 0 ) { s_reproWait--; break; }	// let vid_restart finish recreating the targets
			const idStr slot = R_ReproSlot( cap );
			cvarSystem->SetCVarString( "com_autoLoadGame", slot.c_str() );	// skip the shell enumeration pre-check
			cmdSystem->BufferCommandText( CMD_EXEC_APPEND, va( "loadGame %s\n", slot.c_str() ) );
			common->Printf( "[repro] %s: loading embedded save (slot '%s')...\n", R_ReproSlot( cap ).c_str(), slot.c_str() );
			s_reproWait = 0; s_reproState = RS_WAITLOAD;
			break;
		}
		case RS_WAITLOAD:
		{
			const bool inGame = ( session != NULL && session->GetState() == idSession::INGAME )
								&& tr.primaryWorld != NULL;
			if( inGame )
			{
				cvarSystem->SetCVarString( "com_autoLoadGame", "" );
				// position the REAL player view at the captured camera via setviewpos (instant teleport, applied
				// over the goto's live frames), then the shot freezes + renders through game->Draw. Running the
				// actual game render is the point - a renderer-only bypass can pass while the shipped path fails.
				// RECAPTURE-FROM-VIEW EXCEPTION: a cinematic .cap stores a cutscene render camera parked in a dark
				// fully-penumbral corner where the gate has no valid (fully-lit) pixels and the RT oracle goes
				// degenerate - ungateable. r_softShadowRecaptureFromSaveView regenerates it from the SAVE's natural
				// player viewpoint instead (a real gameplay eye), turning a dead cinematic cap into a testable one.
				extern idCVar r_softShadowRecaptureFromSaveView;
				if( !( s_reproRecapture && r_softShadowRecaptureFromSaveView.GetBool() ) )
				{
					cmdSystem->BufferCommandText( CMD_EXEC_APPEND, va( "softShadowGoto %s\n", cap.c_str() ) );
				}
				s_reproWait = REPRO_SETTLE; s_reproState = RS_SETTLE;
			}
			else if( ++s_reproWait > REPRO_LOAD_TIMEOUT )
			{
				common->Warning( "[repro] %s: load did not reach INGAME in %d frames - skipping", R_ReproSlot( cap ).c_str(), REPRO_LOAD_TIMEOUT );
				s_reproFails++; s_reproState = RS_NEXT;
			}
			break;
		}
		case RS_SETTLE:
			if( --s_reproWait <= 0 )
			{
				if( s_reproRecapture )
				{
					// Overwrite this cap in place with the current writer: the loaded save restored the EXACT
					// live light state (script-moved / retinted cutscene lights), so `capture` stamps the v7
					// live-light tail that a pre-v7 cap lacks. Unfreeze so CaptureGameSave sees a live player.
					s_forcedCaptureBase = idStr( "cap/" ) + R_ReproSlot( cap );
					cvarSystem->SetCVarInteger( "g_stopTime", 0 );
					cmdSystem->BufferCommandText( CMD_EXEC_APPEND, "capture\n" );
				}
				else
				{
					cmdSystem->BufferCommandText( CMD_EXEC_APPEND, va( "softShadowReproShot %s\n", cap.c_str() ) );
				}
				s_reproState = RS_SHOTWAIT;
			}
			break;
		case RS_SHOTWAIT:
			s_forcedCaptureBase.Clear();	// the shot/capture ran this frame; drop the in-place overwrite target
			s_reproState = RS_NEXT;			// advance next tick
			break;
		case RS_NEXT:
			s_reproIdx++;
			if( s_reproIdx < s_reproCaps.Num() )
			{
				s_reproState = RS_LOAD;		// next capture at the current resolution
			}
			else if( ++s_reproResIdx < s_reproResCount )
			{
				s_reproState = RS_SETRES;	// done with this resolution; advance to the next
			}
			else
			{
				common->Printf( "[repro] done - %d capture(s) x %d resolution(s), %d deviation/instrument failure(s)\n",
								s_reproCaps.Num(), s_reproResCount, s_reproFails );
				cmdSystem->BufferCommandText( CMD_EXEC_APPEND, "quit\n" );
				s_reproIdx = -1;
			}
			break;
	}
}

void R_SoftShadowRepro_f( const idCmdArgs& args )
{
	if( args.Argc() < 2 )
	{
		common->Warning( "usage: softShadowRepro <cap1.cap> [cap2 ...] - restore each embedded save, freeze, A/B scanline vs sampled, diff in-engine, then quit" );
		return;
	}
	s_reproCaps.Clear();
	for( int i = 1; i < args.Argc(); i++ ) { s_reproCaps.Append( idStr( args.Argv( i ) ) ); }
	extern idCVar r_softShadowRepro4K;
	s_reproResCount = r_softShadowRepro4K.GetBool() ? 3 : 2;		// 1080p+1440p, +4K opt-in
	s_reproRecapture = false;
	s_reproResIdx = 0; s_reproIdx = 0; s_reproState = RS_SETRES; s_reproFails = 0;
	common->Printf( "[repro] armed %d capture(s) x %d resolution(s)\n", s_reproCaps.Num(), s_reproResCount );
}

// RECAPTURE: regenerate each given .cap in place at the corpus resolution (1440p), reusing the repro
// load->goto->settle path but writing the .cap with the current writer instead of running the A/B shot.
// The point is to stamp the v7 live-light tail onto pre-v7 caps (whose script-moved cutscene lights the
// static-map reconstruction cannot recover) so the gate stops reporting them as SETUP defects. Recapture is
// a normal maintenance step - re-run it whenever the .cap format or the capture path changes.
void R_SoftShadowRecapture_f( const idCmdArgs& args )
{
	if( args.Argc() < 2 )
	{
		common->Warning( "usage: softShadowRecapture <cap1.cap> [cap2 ...] - restore each embedded save, reposition, overwrite the .cap in place with the current writer (v7 tail), then quit" );
		return;
	}
	s_reproCaps.Clear();
	for( int i = 1; i < args.Argc(); i++ ) { s_reproCaps.Append( idStr( args.Argv( i ) ) ); }
	s_reproRecapture = true;
	s_reproResCount = 2;			// index 1..<2 == 1440p only (the corpus resolution)
	s_reproResIdx = 1; s_reproIdx = 0; s_reproState = RS_SETRES; s_reproFails = 0;
	s_forcedCaptureBase.Clear();
	common->Printf( "[recap] armed %d capture(s) for in-place regeneration at 1440p\n", s_reproCaps.Num() );
}

// One-shot capture of a live soft-shadow view, reconstructable headless. See RenderCapture.h. The capture is
// two-phase within one frame: the FRONTEND half snapshots the view/lights/edges/caster-meshes (all CPU-side),
// the BACKEND half grabs the screenshot and writes the .cap + .json. `captureSoftShadow` arms it; the
// halves fire on the next main view and disarm.

idCVar r_softShadowRepro4K( "r_softShadowRepro4K", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "softShadowRepro: also reproduce every capture at 3840x2160 (in addition to 1080p + 1440p). Off by default - 4K is slow and a headless box may lack the VRAM." );
idCVar r_softShadowRecaptureFromSaveView( "r_softShadowRecaptureFromSaveView", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "softShadowRecapture: regenerate each cap from the embedded save's NATURAL player viewpoint instead of the stored (cinematic) render camera. Use to rescue ungateable cutscene captures parked in dark penumbral corners." );

extern idCVar r_shadowPenumbraSize;

namespace
{
// ---- armed state ----
bool  s_armed = false;			// set by the command, cleared once both halves have run
bool  s_frontendDone = false;	// the frontend half snapshotted this frame

// ---- accumulators (frame-scoped; cleared when a new capture is armed) ----
capHeader_t         s_hdr;
std::vector<capLight_t>  s_lights;
std::vector<capLightParms_t> s_lightParms;	// v7 tail: LIVE renderLight_t per captured light (script-true state)
std::vector<capEdge_t>   s_edges;
std::vector<capCaster_t> s_casters;
std::vector<float>           s_meshVerts;	// float3 packed (caster meshes)
std::vector<uint32_t>        s_meshIdx;
std::vector<float>           s_depth;		// per-pixel raw depth (R channel), screenW*screenH, top-left origin
std::vector<capReceiver_t> s_receivers;	// receiver interaction surfaces (the shaded surfaces)
std::vector<float>           s_recvVerts;	// float3 packed (receiver meshes)
std::vector<uint32_t>        s_recvIdx;
std::vector<float>           s_recvST;		// float2 packed per receiver vert (v5 TEXTURE TAIL)
std::vector<uint32_t>        s_recvMat;		// per receiver surface: index into s_matPtrs (v5)
std::vector<const idMaterial*> s_matPtrs;	// unique receiver materials, in first-seen order (v5)
idStr                        s_mapName;		// current map (self-identifying capture: reload + reconstruct, v4+)
std::vector<float>           s_shadowVerts;	// float3 packed, CAPPED shadow volumes (world space), v4+
std::vector<uint32_t>        s_shadowIdx;
std::vector<capShadowVol_t> s_shadowVols;
// the frontend records each shadow surf's GPU cache handles + transform; the backend reads them back once the
// GPU is idle (the shadow geometry lives only in shadowCache/shadowIndexCache - the drawSurf has no CPU tris).
struct ShadowSurfRec { vertCacheHandle_t vc, ic; int numIdx; float m[16]; float lgt[3]; uint32_t li; };
std::vector<ShadowSurfRec>   s_shadowSurfs;

// append one drawSurf's world-space triangle mesh (frontEndGeo x modelMatrix) to verts/idx; return the range.
bool AppendSurfMesh( const drawSurf_t* cs, std::vector<float>& verts, std::vector<uint32_t>& idx,
					 uint32_t& firstVert, uint32_t& numVerts, uint32_t& firstIndex, uint32_t& numIndex )
{
	const srfTriangles_t* tri = cs->frontEndGeo;
	if( tri == NULL || tri->verts == NULL || tri->numVerts <= 0 || tri->indexes == NULL || tri->numIndexes <= 0 )
	{
		return false;
	}
	firstVert  = ( uint32_t )( verts.size() / 3 );
	numVerts   = ( uint32_t )tri->numVerts;
	firstIndex = ( uint32_t )idx.size();
	numIndex   = ( uint32_t )tri->numIndexes;
	for( int v = 0; v < tri->numVerts; v++ )
	{
		idVec3 w;
		R_LocalPointToGlobal( cs->space->modelMatrix, tri->verts[v].xyz, w );
		verts.push_back( w.x ); verts.push_back( w.y ); verts.push_back( w.z );
	}
	// store GLOBAL indices (offset by firstVert) so the flat vert array is one addressable soup - a ray-cast
	// over many surfaces indexes correctly, not just the first (whose firstVert is 0).
	for( int i = 0; i < tri->numIndexes; i++ ) { idx.push_back( firstVert + ( uint32_t )tri->indexes[i] ); }
	return true;
}

// edges retained per viewLight at flatten time, matched to the light in the frontend walk by pointer.
std::map<const viewLight_t*, std::pair<uint32_t, uint32_t>> s_edgeRange;	// vLight -> (firstEdge, count)

void ResetAccumulators()
{
	memset( &s_hdr, 0, sizeof( s_hdr ) );
	s_lights.clear();
	s_lightParms.clear();
	s_edges.clear();
	s_casters.clear();
	s_meshVerts.clear();
	s_meshIdx.clear();
	s_depth.clear();
	s_receivers.clear();
	s_recvVerts.clear();
	s_recvIdx.clear();
	s_recvST.clear();
	s_recvMat.clear();
	s_matPtrs.clear();
	s_mapName.Clear();
	s_shadowVerts.clear();
	s_shadowIdx.clear();
	s_shadowVols.clear();
	s_shadowSurfs.clear();
	s_edgeRange.clear();
	s_frontendDone = false;
}

// sequential base name "capNNNN" in the same spot screenshots go (fs_savepath).
// Read a RESIDENT texture back from the GPU as RGBA8 (v5 texture tail). Blit-decodes any sampleable
// format (the shipped game has only BC-compressed .bimage data - there are no source TGAs to load, so
// disk loading yields nothing; the VRAM copy is the only real texel source). Mirrors R_ReadPixelsRGB8.
bool ReadImageRGBA8( idImage* img, std::vector<uint8_t>& out, int& w, int& h )
{
	if( img == NULL || img->GetTextureHandle() == NULL ) { return false; }
	nvrhi::IDevice* device = deviceManager->GetDevice();
	nvrhi::ITexture* texture = img->GetTextureHandle();
	nvrhi::TextureDesc desc = texture->getDesc();
	w = ( int )desc.width;
	h = ( int )desc.height;
	if( w <= 0 || h <= 0 ) { return false; }

	nvrhi::CommandListHandle commandList = device->createCommandList();
	commandList->open();
	commandList->beginTrackingTextureState( texture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource );

	desc.format = nvrhi::Format::SRGBA8_UNORM;
	desc.isRenderTarget = true;
	desc.initialState = nvrhi::ResourceStates::RenderTarget;
	desc.keepInitialState = true;
	desc.mipLevels = 1;
	nvrhi::TextureHandle tempTexture = device->createTexture( desc );
	nvrhi::FramebufferHandle tempFramebuffer = device->createFramebuffer( nvrhi::FramebufferDesc().addColorAttachment( tempTexture ) );
	backEnd.GetCommonPasses().BlitTexture( commandList, tempFramebuffer, texture );

	nvrhi::StagingTextureHandle stagingTexture = device->createStagingTexture( desc, nvrhi::CpuAccessMode::Read );
	commandList->copyTexture( stagingTexture, nvrhi::TextureSlice(), tempTexture, nvrhi::TextureSlice() );
	commandList->setTextureState( texture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource );
	commandList->commitBarriers();
	commandList->close();
	device->executeCommandList( commandList );

	size_t rowPitch = 0;
	void* pData = device->mapStagingTexture( stagingTexture, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch );
	if( pData == NULL ) { return false; }
	out.resize( ( size_t )w * h * 4 );
	for( int y = 0; y < h; y++ )
	{
		memcpy( out.data() + ( size_t )y * w * 4, ( const uint8_t* )pData + ( size_t )y * rowPitch, ( size_t )w * 4 );
	}
	device->unmapStagingTexture( stagingTexture );
	return true;
}

void NextCaptureBaseName( idStr& out )
{
	if( !s_forcedCaptureBase.IsEmpty() )
	{
		out = s_forcedCaptureBase;
		return;
	}
	for( int i = 0; i <= 9999; i++ )
	{
		idStr candidate = va( "cap/cap%04i.cap", i );
		if( fileSystem->ReadFile( candidate, NULL, NULL ) == -1 )
		{
			out = va( "cap/cap%04i", i );
			return;
		}
	}
	out = "cap/cap9999";
}
} // namespace

bool R_SoftShadowCaptureArmed()
{
	return s_armed;
}

// ------------------------------------------------------------------- frontend half: edges (at flatten time)
void R_CaptureLightEdges( const viewLight_t* vLight, const softShadowEdge_t* flat, int records )
{
	if( !s_armed || s_frontendDone || vLight == NULL || flat == NULL || records <= 0 )
	{
		return;
	}
	const uint32_t first = ( uint32_t )s_edges.size();
	for( int i = 0; i < records; i++ )
	{
		capEdge_t e;
		e.e0[0] = flat[i].e0.x; e.e0[1] = flat[i].e0.y; e.e0[2] = flat[i].e0.z; e.e0[3] = flat[i].e0.w;
		e.e1[0] = flat[i].e1.x; e.e1[1] = flat[i].e1.y; e.e1[2] = flat[i].e1.z; e.e1[3] = flat[i].e1.w;
		s_edges.push_back( e );
	}
	s_edgeRange[vLight] = std::make_pair( first, ( uint32_t )records );
}

// ------------------------------------------------------------------- frontend half: view + lights + meshes
void R_CaptureFrontendView( const viewDef_t* viewDef )
{
	if( !s_armed || s_frontendDone || viewDef == NULL )
	{
		return;
	}

	// camera / view
	const renderView_t& rv = viewDef->renderView;
	s_hdr.magic = CAP_MAGIC;
	s_hdr.version = CAP_VERSION;
	s_hdr.screenW = ( uint32_t )( viewDef->viewport.x2 - viewDef->viewport.x1 + 1 );
	s_hdr.screenH = ( uint32_t )( viewDef->viewport.y2 - viewDef->viewport.y1 + 1 );
	for( int i = 0; i < 3; i++ ) { s_hdr.vieworg[i] = rv.vieworg[i]; }
	for( int r = 0; r < 3; r++ )
		for( int c = 0; c < 3; c++ ) { s_hdr.viewaxis[r * 3 + c] = rv.viewaxis[r][c]; }
	s_hdr.fovx = rv.fov_x;
	s_hdr.fovy = rv.fov_y;
	s_hdr.reserved[0] = ( uint32_t )rv.time[0];					// game time (ms) - determinism for reconstruction
	s_mapName = commonLocal.GetCurrentMapName();				// map identity - reload + reconstruct from the engine
	for( int i = 0; i < 16; i++ )
	{
		s_hdr.projectionMatrix[i]           = viewDef->projectionMatrix[i];
		s_hdr.unjitteredProjectionMatrix[i] = viewDef->unjitteredProjectionMatrix[i];
		s_hdr.unprojectionToWorldMatrix[i]  = viewDef->unprojectionToWorldMatrix[i];
		s_hdr.worldMVP[i]                   = viewDef->worldSpace.mvp[i / 4][i % 4];
	}
	s_hdr.viewport[0] = viewDef->viewport.x1; s_hdr.viewport[1] = viewDef->viewport.y1;
	s_hdr.viewport[2] = viewDef->viewport.x2; s_hdr.viewport[3] = viewDef->viewport.y2;
	s_hdr.taaFrameCount = tr.frameCount;

	// per soft-shadow light: assemble the final record (edge range came from the flatten half) + caster meshes
	for( const viewLight_t* vLight = viewDef->viewLights; vLight != NULL; vLight = vLight->next )
	{
		if( vLight->softEdgeCount <= 0 )
		{
			continue;
		}
		auto it = s_edgeRange.find( vLight );
		if( it == s_edgeRange.end() )
		{
			continue;	// armed after this light's flatten; skip rather than emit a light with no edges
		}
		capLight_t L;
		memset( &L, 0, sizeof( L ) );
		L.origin[0] = vLight->globalLightOrigin.x;
		L.origin[1] = vLight->globalLightOrigin.y;
		L.origin[2] = vLight->globalLightOrigin.z;
		// per-light resolved emitter radius (task #105): captures taken live embed each light's actual
		// radius (authored key / auto heuristic / global); gate replay pins auto off + sets the global
		// to this stored value per light, so fixtures replay at exactly their captured size
		{
			extern float R_SoftPenumbraRadius( const idRenderLightLocal* lightDef );
			L.penumbraSize = R_SoftPenumbraRadius( vLight->lightDef );
		}
		L.scissor[0] = vLight->scissorRect.x1; L.scissor[1] = vLight->scissorRect.y1;
		L.scissor[2] = vLight->scissorRect.x2; L.scissor[3] = vLight->scissorRect.y2;
		L.firstEdge = it->second.first;
		L.edgeCount = it->second.second;
		L.firstCaster = ( uint32_t )s_casters.size();

		const uint32_t lightIndex = ( uint32_t )s_lights.size();

		// caster silhouette solids (for the ray-cast ground truth)
		for( const drawSurf_t* cs = vLight->softShadowWedges; cs != NULL; cs = cs->nextOnLight )
		{
			capCaster_t C;
			C.lightIndex = lightIndex;
			C.casterId   = 0.0f;	// (edge headers carry the real casterId; not needed for the ray-cast)
			if( !AppendSurfMesh( cs, s_meshVerts, s_meshIdx, C.firstVert, C.numVerts, C.firstIndex, C.numIndex ) )
			{
				continue;
			}
			s_casters.push_back( C );
		}
		L.casterCount = ( uint32_t )s_casters.size() - L.firstCaster;

		// record this light's CAPPED shadow-volume surfs; geometry lives in the GPU caches, read back in backend
		for( int spass = 0; spass < 2; spass++ )
		{
			const drawSurf_t* slist = ( spass == 0 ) ? vLight->globalShadows : vLight->localShadows;
			for( const drawSurf_t* s = slist; s != NULL; s = s->nextOnLight )
			{
				if( s->numIndexes == 0 || s->shadowCache == 0 || s->indexCache == 0 || s->space == NULL ) { continue; }
				ShadowSurfRec rec;
				rec.vc = s->shadowCache; rec.ic = s->indexCache; rec.numIdx = s->numIndexes; rec.li = lightIndex;
				for( int m = 0; m < 16; m++ ) { rec.m[m] = s->space->modelMatrix[m]; }
				rec.lgt[0] = vLight->globalLightOrigin.x; rec.lgt[1] = vLight->globalLightOrigin.y; rec.lgt[2] = vLight->globalLightOrigin.z;
				s_shadowSurfs.push_back( rec );
			}
		}

		// receiver interaction surfaces (what the coverage shader shades). These give receiver world positions
		// with no depth reconstruction - coverage-vs-truth is evaluated at these surface points.
		for( int pass = 0; pass < 2; pass++ )
		{
			const drawSurf_t* list = ( pass == 0 ) ? vLight->globalInteractions : vLight->localInteractions;
			for( const drawSurf_t* rs = list; rs != NULL; rs = rs->nextOnLight )
			{
				capReceiver_t R;
				R.lightIndex = lightIndex;
				if( !AppendSurfMesh( rs, s_recvVerts, s_recvIdx, R.firstVert, R.numVerts, R.firstIndex, R.numIndex ) )
				{
					continue;
				}
				s_receivers.push_back( R );
				// v5 TEXTURE TAIL: per-vertex UVs + this surface's material (unique-listed; the diffuse
				// image is baked into the capture at write time so offline verification renders REAL
				// textures, not a stand-in pattern).
				const srfTriangles_t* rtri = rs->frontEndGeo;
				for( int v = 0; v < rtri->numVerts; v++ )
				{
					const idVec2 st = rtri->verts[v].GetTexCoord();
					s_recvST.push_back( st.x );
					s_recvST.push_back( st.y );
				}
				uint32_t mi = 0;
				for( ; mi < ( uint32_t )s_matPtrs.size(); mi++ )
				{
					if( s_matPtrs[mi] == rs->material ) { break; }
				}
				if( mi == ( uint32_t )s_matPtrs.size() ) { s_matPtrs.push_back( rs->material ); }
				s_recvMat.push_back( mi );
			}
		}
		s_lights.push_back( L );

		// v7 LIGHT-PARMS: the LIVE light state (scripts move/retint cutscene lights; the map-parse
		// reconstruction diverged 187u on cap0010 and drew nothing). One record per capLight_t.
		{
			capLightParms_t P;
			memset( &P, 0, sizeof( P ) );
			if( vLight->lightDef != NULL )
			{
				const renderLight_t& lp = vLight->lightDef->parms;
				for( int k = 0; k < 3; k++ )
				{
					P.origin[k] = lp.origin[k];
					P.lightRadius[k] = lp.lightRadius[k];
					P.lightCenter[k] = lp.lightCenter[k];
					P.target[k] = lp.target[k];
					P.right[k] = lp.right[k];
					P.up[k] = lp.up[k];
					P.start[k] = lp.start[k];
					P.end[k] = lp.end[k];
				}
				for( int k = 0; k < 9; k++ )
				{
					P.axis[k] = lp.axis[k / 3][k % 3];
				}
				for( int k = 0; k < 12 && k < MAX_ENTITY_SHADER_PARMS; k++ )
				{
					P.shaderParms[k] = lp.shaderParms[k];
				}
				P.pointLight = lp.pointLight ? 1u : 0u;
				P.parallel   = lp.parallel ? 1u : 0u;
				P.noShadows  = lp.noShadows ? 1u : 0u;
				if( lp.shader != NULL )
				{
					idStr::Copynz( P.shaderName, lp.shader->GetName(), sizeof( P.shaderName ) );
				}
			}
			s_lightParms.push_back( P );
		}
	}

	s_frontendDone = true;
}

// ------------------------------------------------------------------- backend half: screenshot + write files
// Read a region of a GPU buffer back to the CPU (staging copy + map). Used for the shadow-volume caches,
// whose geometry exists only on the GPU (the shadow drawSurf has no CPU triangles).
static bool R_ReadbackGPUBuffer( nvrhi::IBuffer* src, uint64 srcOffset, uint64 size, void* dst )
{
	if( src == NULL || size == 0 || dst == NULL ) { return false; }
	nvrhi::IDevice* device = deviceManager->GetDevice();
	nvrhi::BufferDesc sd;
	sd.byteSize = size;
	sd.cpuAccess = nvrhi::CpuAccessMode::Read;
	sd.initialState = nvrhi::ResourceStates::CopyDest;	// let nvrhi auto-manage the state (source is keepInitialState)
	sd.keepInitialState = true;
	sd.debugName = "capReadback";
	nvrhi::BufferHandle staging = device->createBuffer( sd );
	if( staging == NULL ) { return false; }
	nvrhi::CommandListHandle cl = device->createCommandList();
	cl->open();
	cl->copyBuffer( staging, 0, src, srcOffset, size );
	cl->close();
	device->executeCommandList( cl );
	void* mapped = device->mapBuffer( staging, nvrhi::CpuAccessMode::Read );
	if( mapped == NULL ) { return false; }
	std::memcpy( dst, mapped, ( size_t )size );
	device->unmapBuffer( staging );
	return true;
}

void R_CaptureBackendFinish()
{
	if( !s_armed || !s_frontendDone )
	{
		return;
	}
	// read back the CAPPED shadow-volume geometry recorded in the frontend (GPU caches -> world-space tris)
	for( const ShadowSurfRec& rec : s_shadowSurfs )
	{
		idVertexBuffer vb; idIndexBuffer ib;
		if( !vertexCache.GetVertexBuffer( rec.vc, &vb ) || !vertexCache.GetIndexBuffer( rec.ic, &ib ) ) { continue; }
		int numVerts = vb.GetSize() / ( int )sizeof( idShadowVert );
		// Read the FULL index buffer, not rec.numIdx: the drawSurf's count is the engine's SELECTED variant
		// (numShadowIndexesNoCaps when the eye is outside = capless), but the buffer holds the full CAPPED set
		// (numShadowIndexes = sides + front/rear caps). Reading it captures a closed volume regardless of the
		// capture viewpoint, so the offline z-fail count is faithful (a capless volume has no robust inside/out).
		int numIdx = ib.GetSize() / ( int )sizeof( triIndex_t );
		if( numVerts <= 0 || numIdx <= 0 ) { continue; }
		std::vector<idShadowVert> verts( numVerts );
		std::vector<triIndex_t>   idxs( numIdx );
		if( !R_ReadbackGPUBuffer( vb.GetAPIObject(), ( uint64 )vb.GetOffset(), ( uint64 )numVerts * sizeof( idShadowVert ), verts.data() ) ) { continue; }
		if( !R_ReadbackGPUBuffer( ib.GetAPIObject(), ( uint64 )ib.GetOffset(), ( uint64 )numIdx * sizeof( triIndex_t ), idxs.data() ) ) { continue; }
		capShadowVol_t sv; sv.lightIndex = rec.li;
		sv.firstVert = ( uint32_t )( s_shadowVerts.size() / 3 );
		sv.firstIdx  = ( uint32_t )s_shadowIdx.size();
		idVec3 lLocal; R_GlobalPointToLocal( rec.m, idVec3( rec.lgt[0], rec.lgt[1], rec.lgt[2] ), lLocal );
		const float BIG = 16000.0f;
		for( int v = 0; v < numVerts; v++ )
		{
			const idVec4& p = verts[v].xyzw;
			idVec3 local( p.x, p.y, p.z );
			if( p.w < 0.5f ) { local = local + ( local - lLocal ) * BIG; }		// w==0: extrude to (far) infinity from the light
			idVec3 w; R_LocalPointToGlobal( rec.m, local, w );
			s_shadowVerts.push_back( w.x ); s_shadowVerts.push_back( w.y ); s_shadowVerts.push_back( w.z );
		}
		sv.numVert = ( uint32_t )numVerts;
		for( int i = 0; i < numIdx; i++ ) { s_shadowIdx.push_back( sv.firstVert + ( uint32_t )idxs[i] ); }
		sv.numIdx = ( uint32_t )numIdx;
		s_shadowVols.push_back( sv );
	}
	s_hdr.reserved[2] = ( uint32_t )s_shadowVols.size();
	s_hdr.reserved[3] = ( uint32_t )( s_shadowVerts.size() / 3 );
	s_hdr.reserved[4] = ( uint32_t )s_shadowIdx.size();

	idStr base;
	NextCaptureBaseName( base );

	// FRAME column (full shaded LDR) straight to PNG, reusing the engine path
	idStr png = base + ".png";
	R_ReadPixelsRGB8( deviceManager->GetDevice(), &backEnd.GetCommonPasses(),
					  globalImages->currentRenderHDRImage->GetTextureHandle(), nvrhi::ResourceStates::ShaderResource, png.c_str() );

	// depth (full float) for per-pixel receiver world-position reconstruction on replay / ground truth
	const int pix = ( int )( s_hdr.screenW * s_hdr.screenH );
	float* depthPic = NULL;
	if( pix > 0 && R_ReadPixelsR32F( deviceManager->GetDevice(), &backEnd.GetCommonPasses(),
									 globalImages->currentDepthImage->GetTextureHandle(),
									 nvrhi::ResourceStates::ShaderResource, &depthPic, ( int )s_hdr.screenW, ( int )s_hdr.screenH ) && depthPic != NULL )
	{
		s_depth.assign( depthPic, depthPic + pix );
		R_StaticFree( depthPic );
	}

	// finalize header counts
	s_hdr.numLights     = ( uint32_t )s_lights.size();
	s_hdr.numEdges      = ( uint32_t )s_edges.size();
	s_hdr.numCasters    = ( uint32_t )s_casters.size();
	s_hdr.numMeshVerts  = ( uint32_t )( s_meshVerts.size() / 3 );
	s_hdr.numMeshIdx    = ( uint32_t )s_meshIdx.size();
	s_hdr.hasDepth      = ( uint32_t )( ( int )s_depth.size() == pix && pix > 0 ? 1 : 0 );
	s_hdr.numReceivers  = ( uint32_t )s_receivers.size();
	s_hdr.numRecvVerts  = ( uint32_t )( s_recvVerts.size() / 3 );
	s_hdr.numRecvIdx    = ( uint32_t )s_recvIdx.size();
	s_hdr.reserved[1]   = ( uint32_t )s_mapName.Length();		// MAPNAME block byte length (trailing, v4+)

	// write the binary blob
	idFile* f = fileSystem->OpenFileWrite( base + ".cap", "fs_savepath" );
	if( f != NULL )
	{
		f->Write( &s_hdr, sizeof( s_hdr ) );
		if( !s_lights.empty() )    { f->Write( s_lights.data(),    ( int )( s_lights.size()    * sizeof( capLight_t ) ) ); }
		if( !s_edges.empty() )     { f->Write( s_edges.data(),     ( int )( s_edges.size()     * sizeof( capEdge_t ) ) ); }
		if( !s_casters.empty() )   { f->Write( s_casters.data(),   ( int )( s_casters.size()   * sizeof( capCaster_t ) ) ); }
		if( !s_meshVerts.empty() ) { f->Write( s_meshVerts.data(), ( int )( s_meshVerts.size() * sizeof( float ) ) ); }
		if( !s_meshIdx.empty() )   { f->Write( s_meshIdx.data(),   ( int )( s_meshIdx.size()   * sizeof( uint32_t ) ) ); }
		if( s_hdr.hasDepth )        { f->Write( s_depth.data(),     ( int )( s_depth.size()      * sizeof( float ) ) ); }
		if( !s_receivers.empty() )  { f->Write( s_receivers.data(), ( int )( s_receivers.size()  * sizeof( capReceiver_t ) ) ); }
		if( !s_recvVerts.empty() )  { f->Write( s_recvVerts.data(), ( int )( s_recvVerts.size()  * sizeof( float ) ) ); }
		if( !s_recvIdx.empty() )    { f->Write( s_recvIdx.data(),   ( int )( s_recvIdx.size()    * sizeof( uint32_t ) ) ); }
		if( s_hdr.reserved[1] > 0 ) { f->Write( s_mapName.c_str(),  ( int )s_hdr.reserved[1] ); }	// MAPNAME block
		if( s_hdr.reserved[2] > 0 )		// SHADOWVOL section (v4+): vols table, then verts, then indices
		{
			f->Write( s_shadowVols.data(), ( int )( s_shadowVols.size() * sizeof( capShadowVol_t ) ) );
			if( !s_shadowVerts.empty() ) { f->Write( s_shadowVerts.data(), ( int )( s_shadowVerts.size() * sizeof( float ) ) ); }
			if( !s_shadowIdx.empty() )   { f->Write( s_shadowIdx.data(),   ( int )( s_shadowIdx.size()   * sizeof( uint32_t ) ) ); }
		}
		// v5 TEXTURE TAIL: self-describing, appended last - old readers stop before it. For each unique
		// receiver material, load its diffuse image from disk (the GPU copy is not CPU-readable), box-
		// downsample to <= CAP_TEX_MAX per side, store RGB8. Failures store a 1x1 grey.
		if( !s_recvMat.empty() && s_recvST.size() == s_recvVerts.size() / 3 * 2 )
		{
			std::vector<capMaterial_t> mats;
			std::vector<uint8_t> texels;
			for( const idMaterial* m : s_matPtrs )
			{
				capMaterial_t rec = {};
				idStr::Copynz( rec.name, m ? m->GetName() : "<null>", sizeof( rec.name ) );
				rec.firstTexel = ( uint32_t )texels.size();
				idImage* img = m ? const_cast<idImage*>( m->GetFastPathDiffuseImage() ) : NULL;
				if( img == NULL && m != NULL )
				{
					for( int st = 0; st < m->GetNumStages() && img == NULL; st++ )
					{
						const shaderStage_t* stage = m->GetStage( st );
						if( stage != NULL && stage->lighting == SL_DIFFUSE ) { img = stage->texture.image; }
					}
				}
				if( img == NULL && m != NULL )		// no diffuse stage (unlit/utility): first stage image
				{
					for( int st = 0; st < m->GetNumStages() && img == NULL; st++ )
					{
						const shaderStage_t* stage = m->GetStage( st );
						if( stage != NULL ) { img = stage->texture.image; }
					}
				}
				std::vector<uint8_t> pic;
				int pw = 0, ph = 0;
				if( ReadImageRGBA8( img, pic, pw, ph ) && pw > 0 && ph > 0 )
				{
					const int step = Max( 1, Max( pw, ph ) / CAP_TEX_MAX );
					rec.texW = ( uint32_t )Max( 1, pw / step );
					rec.texH = ( uint32_t )Max( 1, ph / step );
					for( uint32_t y = 0; y < rec.texH; y++ )
						for( uint32_t x = 0; x < rec.texW; x++ )
						{
							int r = 0, g = 0, b = 0, n = 0;
							for( int sy = 0; sy < step; sy++ )
								for( int sx = 0; sx < step; sx++ )
								{
									int px = ( int )x * step + sx, py = ( int )y * step + sy;
									if( px >= pw || py >= ph ) { continue; }
									const uint8_t* t = pic.data() + ( ( size_t )py * pw + px ) * 4;
									r += t[0]; g += t[1]; b += t[2]; n++;
								}
							n = Max( n, 1 );
							texels.push_back( ( uint8_t )( r / n ) );
							texels.push_back( ( uint8_t )( g / n ) );
							texels.push_back( ( uint8_t )( b / n ) );
						}
				}
				else
				{
					rec.texW = rec.texH = 1;
					texels.push_back( 128 ); texels.push_back( 128 ); texels.push_back( 128 );
				}
				mats.push_back( rec );
			}
			const uint32_t tail[5] = { CAP_TAIL_MAGIC, ( uint32_t )mats.size(), ( uint32_t )texels.size(),
									   ( uint32_t )s_recvST.size(), ( uint32_t )s_recvMat.size() };
			f->Write( tail, sizeof( tail ) );
			f->Write( mats.data(),     ( int )( mats.size()     * sizeof( capMaterial_t ) ) );
			f->Write( texels.data(),   ( int )texels.size() );
			f->Write( s_recvST.data(), ( int )( s_recvST.size() * sizeof( float ) ) );
			f->Write( s_recvMat.data(), ( int )( s_recvMat.size() * sizeof( uint32_t ) ) );
			common->Printf( "soft-shadow capture: v5 texture tail - %zu materials, %zu KB texels\n",
							mats.size(), texels.size() / 1024 );
		}
		// v6 SAVEGAME TAIL: embed the engine's full save serialisation so the .cap reconstructs the EXACT
		// live game state (health/ammo/weapons/inventory/entities), not just the render state. CaptureGameSave
		// also writes a loadable disk slot the repro harness restores through the proven LoadGame path.
		{
			idStr slot = base;
			slot.StripPath();							// "capNNNN" - the loadable disk slot name
			idList<byte> saveBytes, stringBytes;
			if( commonLocal.CaptureGameSave( slot.c_str(), saveBytes, stringBytes ) )
			{
				const uint32_t sv[3] = { CAP_SAVE_MAGIC, ( uint32_t )saveBytes.Num(), ( uint32_t )stringBytes.Num() };
				f->Write( sv, sizeof( sv ) );
				if( saveBytes.Num() > 0 )   { f->Write( saveBytes.Ptr(),   saveBytes.Num() ); }
				if( stringBytes.Num() > 0 ) { f->Write( stringBytes.Ptr(), stringBytes.Num() ); }
				common->Printf( "soft-shadow capture: v6 save tail - slot '%s', %d + %d bytes\n",
								slot.c_str(), saveBytes.Num(), stringBytes.Num() );
			}
			else
			{
				common->Warning( "soft-shadow capture: CaptureGameSave failed - .cap has NO embedded save (repro cannot restore game state)" );
			}
		}
		// v7 LIGHT-PARMS TAIL (see RenderCapture.h): [magic][count][array][count][magic]. The
		// trailing FOOTER lets the gate reader find it by seeking from the file END, without
		// walking every variable-length tail (textures, savegame) in between. LIVE script-true
		// light state, so the gate reconstructs cutscene lights instead of the stale map parse.
		{
			const uint32_t lp[2] = { CAP_LPARM_MAGIC, ( uint32_t )s_lightParms.size() };
			f->Write( lp, sizeof( lp ) );
			if( !s_lightParms.empty() )
			{
				f->Write( s_lightParms.data(), s_lightParms.size() * sizeof( capLightParms_t ) );
			}
			const uint32_t lpf[2] = { ( uint32_t )s_lightParms.size(), CAP_LPARM_MAGIC };
			f->Write( lpf, sizeof( lpf ) );
		}
		fileSystem->CloseFile( f );
		common->Printf( "soft-shadow capture: %s.cap  (%u lights, %u edges, %u casters/%u tris, %u recv/%u tris, depth=%u)\n",
						base.c_str(), s_hdr.numLights, s_hdr.numEdges, s_hdr.numCasters, s_hdr.numMeshIdx / 3,
						s_hdr.numReceivers, s_hdr.numRecvIdx / 3, s_hdr.hasDepth );
	}
	else
	{
		common->Warning( "soft-shadow capture: could not open %s.cap for write", base.c_str() );
	}

	// human-readable / trimmable sidecar
	idFile* j = fileSystem->OpenFileWrite( base + ".cap.json", "fs_savepath" );
	if( j != NULL )
	{
		j->Printf( "{\n  \"version\": %u,\n  \"screen\": [%u, %u],\n", s_hdr.version, s_hdr.screenW, s_hdr.screenH );
		j->Printf( "  \"vieworg\": [%.3f, %.3f, %.3f],\n", s_hdr.vieworg[0], s_hdr.vieworg[1], s_hdr.vieworg[2] );
		j->Printf( "  \"lights\": [\n" );
		for( size_t i = 0; i < s_lights.size(); i++ )
		{
			const capLight_t& L = s_lights[i];
			j->Printf( "    { \"index\": %u, \"origin\": [%.3f, %.3f, %.3f], \"penumbra\": %.3f, \"edges\": %u, \"casters\": %u }%s\n",
					   ( uint32_t )i, L.origin[0], L.origin[1], L.origin[2], L.penumbraSize, L.edgeCount, L.casterCount,
					   ( i + 1 < s_lights.size() ) ? "," : "" );
		}
		j->Printf( "  ]\n}\n" );
		fileSystem->CloseFile( j );
	}

	s_armed = false;
	ResetAccumulators();
}

// ============================================================ com_softShadowGateSmoke: the REAL-GAME SMOKE gate
// The minimal-init defect gate (com_softShadowGate) never boots the game, so it is structurally blind to
// load-path stalls and black-frame-but-no-crash failures (e.g. r_softShadowSurfCacheGrid 1: the map warms at
// load but the game view never presents - viewLights stays 0, the frame is black). This stage rides the FULL
// shipped load+present path: the caller launches `+devmap <map> +set com_softShadowGateSmoke N` (the proven
// benchmark path - the map load is the engine's job, exactly as com_softShadowFrameProbe does it), and the
// per-frame tick self-arms off the cvar, counts real game-view frames (INGAME && viewLights>0), and once N have
// rendered reads back the lit HDR frame and asserts it is not black. A run that never reaches a game view (load
// stall / hang) FAILs on the wall-clock budget - never a silent pass. Verdict line + process exit code mirror
// the defect gate; then quit. The `softShadowGateSmoke <map>` command is interactive convenience (arm + devmap
// from an already-booted console); the tested path is the pure cvar arm so nothing races engine init.
static bool  s_smokeArmed       = false;
static idStr s_smokeMap;
static int   s_smokeFrames      = 0;	// target genuine game-view frames before the luminance verdict
static int   s_smokeStartMs     = 0;
static int   s_smokeLastTickMs  = 0;	// wall time of the previous tick, for the per-frame stall check
static int   s_smokeGameViews    = 0;
static int   s_smokeTotalFrames = 0;

static void R_SoftShadowSmokeVerdict( bool pass, const char* reason, double meanLum, double litFrac, int wallMs )
{
	extern void Sys_SetExitCode( int code );
	s_smokeArmed = false;
	const int grid = cvarSystem->GetCVarInteger( "r_softShadowSurfCacheGrid" );
	char verdict[256];
	if( pass )
	{
		idStr::Copynz( verdict, "PASS", sizeof( verdict ) );
	}
	else
	{
		idStr::snPrintf( verdict, sizeof( verdict ), "FAIL(%s)", reason );
	}
	common->Printf( "[softsmoke] %s grid=%d: mean-lum %.4f, lit-frac %.2f%%, frames %d, wallms %d -> %s\n",
					s_smokeMap.c_str(), grid, meanLum, litFrac * 100.0, s_smokeTotalFrames, wallMs, verdict );
	Sys_SetExitCode( pass ? 0 : 1 );
	// disarm (cvar 0 so the next frame's tick early-outs) then quit CLEANLY via the command buffer - NOT
	// Sys_Quit() directly. The defect gate can Sys_Quit mid-call because it runs at minimal-init with no game
	// thread; here the full SMP game thread is live and an abrupt synchronous Sys_Quit from inside Frame()
	// tears it down under itself and crashes. The deferred `quit` shuts down at the frame boundary; the exit
	// code set above survives to Posix_Exit. Mirrors com_autoCapture / com_softShadowFrameProbe.
	cvarSystem->SetCVarInteger( "com_softShadowGateSmoke", 0 );
	cmdSystem->BufferCommandText( CMD_EXEC_APPEND, "quit\n" );
}

void R_SoftShadowGateSmokeTick( int viewLights )
{
	const int cvarN = cvarSystem->GetCVarInteger( "com_softShadowGateSmoke" );
	if( cvarN <= 0 )
	{
		return;	// disabled
	}
	// first tick after the cvar is set: latch config, start the wall clock, arm the hard-wedge watchdog. Self
	// arming off the cvar (not a command) means `+set com_softShadowGateSmoke N` alongside `+devmap <map>` works
	// with zero ordering races against engine init - the failure mode of issuing devmap from a startup command.
	if( !s_smokeArmed )
	{
		s_smokeArmed = true;
		s_smokeFrames = cvarN;
		s_smokeGameViews = 0;
		s_smokeTotalFrames = 0;
		s_smokeStartMs = Sys_Milliseconds();
		s_smokeLastTickMs = s_smokeStartMs;
		s_smokeMap = cvarSystem->GetCVarString( "com_softShadowGateSmokeMap" );
		const int budget = cvarSystem->GetCVarInteger( "com_softShadowGateSmokeMaxSeconds" );
		common->Printf( "[softsmoke] armed: map=%s frames=%d budget=%ds grid=%d\n",
						s_smokeMap.c_str(), s_smokeFrames, budget, cvarSystem->GetCVarInteger( "r_softShadowSurfCacheGrid" ) );
	}
	s_smokeTotalFrames++;

	const int now = Sys_Milliseconds();
	const int wallMs = now - s_smokeStartMs;
	const int frameMs = now - s_smokeLastTickMs;
	s_smokeLastTickMs = now;
	const int budgetMs = cvarSystem->GetCVarInteger( "com_softShadowGateSmokeMaxSeconds" ) * 1000;
	const int frameStallMs = cvarSystem->GetCVarInteger( "com_softShadowGateSmokeFrameStallMs" );

	// a real game view = the frontend actually drew lights this frame (viewLights>0, passed in from the
	// COMPLETED frame's stats_frontend). This is exactly the signal the perf overlay shows as 0 during the
	// black-frame load stall this gate exists to catch. NOTE: do NOT also require session INGAME - erebus1
	// opens on a scripted cinematic whose session state is not INGAME, yet it IS a real lit 3D view; gating on
	// INGAME would loop forever on the cinematic benchmark.
	if( viewLights > 0 )
	{
		s_smokeGameViews++;
	}

	// enough settled game-view frames rendered -> luminance verdict on the lit HDR frame
	if( s_smokeGameViews >= s_smokeFrames )
	{
		std::vector<uint8_t> px;
		int w = 0, h = 0;
		if( !ReadImageRGBA8( globalImages->currentRenderHDRImage, px, w, h ) || w <= 0 || h <= 0 )
		{
			R_SoftShadowSmokeVerdict( false, "hdr-readback-failed", -1.0, -1.0, wallMs );
			return;
		}
		// dump the exact frame the verdict scores, for eyeball confirmation
		R_ReadPixelsRGB8( deviceManager->GetDevice(), &backEnd.GetCommonPasses(),
						  globalImages->currentRenderHDRImage->GetTextureHandle(),
						  nvrhi::ResourceStates::ShaderResource, "dumps/softsmoke.png" );
		double sum = 0.0;
		long lit = 0;
		const long n = ( long )w * h;
		const double litThresh = 0.15;	// normalized luma a genuinely lit erebus1 pixel clears
		for( long i = 0; i < n; i++ )
		{
			const uint8_t* p = &px[( size_t )i * 4];
			const double luma = ( 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2] ) / 255.0;
			sum += luma;
			if( luma > litThresh )
			{
				lit++;
			}
		}
		const double meanLum = ( n > 0 ) ? sum / n : 0.0;
		const double litFrac = ( n > 0 ) ? ( double )lit / n : 0.0;
		// a correctly-lit erebus1 view has a nonzero mean AND a real fraction of bright pixels; a black
		// load-stall frame is ~0 on both. Floors are 10x below the observed lit values, 10x above black.
		const bool pass = ( meanLum > 0.01 ) && ( litFrac > 0.01 );
		R_SoftShadowSmokeVerdict( pass, "blackout: lit HDR frame is black", meanLum, litFrac, wallMs );
		return;
	}

	// STALL, per-frame: a single frame that took longer than the threshold before any real view = a froze-at-
	// load hitch (the "stuck at Loading deferred images" case). Catches it in one frame instead of waiting out
	// the whole budget. Skipped for frame 1 (it carries the whole pre-arm init gap).
	if( frameStallMs > 0 && s_smokeTotalFrames > 1 && frameMs > frameStallMs )
	{
		R_SoftShadowSmokeVerdict( false, "stall: a frame exceeded the per-frame budget before any game view", -1.0, -1.0, wallMs );
		return;
	}

	// STALL, total: budget blown before we ever accumulated the target game-view frames -> loud fail, never a
	// silent pass (mirrors the defect gate's "0 lights probed = nothing tested = FAIL" anti-false-green rule).
	// This catches the bug class where the game renders black frames forever (viewLights stays 0).
	// ponytail: a TOTAL frame-loop wedge (init deadlock, no frames at all) won't reach here - the outer harness
	// timeout is the backstop for that rarer class. Add an independent watchdog thread only if it recurs.
	if( budgetMs > 0 && wallMs > budgetMs )
	{
		R_SoftShadowSmokeVerdict( false, "stall: no game view (viewLights>0) within budget", -1.0, -1.0, wallMs );
		return;
	}
}

void R_SoftShadowGateSmoke_f( const idCmdArgs& args )
{
	const char* map = ( args.Argc() > 1 ) ? args.Argv( 1 ) : "game/erebus1";
	int n = ( args.Argc() > 2 ) ? atoi( args.Argv( 2 ) ) : cvarSystem->GetCVarInteger( "com_softShadowGateSmoke" );
	if( n <= 0 )
	{
		n = 30;	// N>=1 is required: "0 frames rendered" must never count as a pass
	}
	// set the label + arm the tick via the cvar, then devmap. Interactive path only (console already up, so
	// devmap does not race init); automated runs prefer `+devmap <map> +set com_softShadowGateSmoke N` directly.
	cvarSystem->SetCVarString( "com_softShadowGateSmokeMap", map );
	cvarSystem->SetCVarInteger( "com_softShadowGateSmoke", n );
	cmdSystem->BufferCommandText( CMD_EXEC_APPEND, va( "devmap %s\n", map ) );
	common->Printf( "[softsmoke] command: devmap %s, %d frames\n", map, n );
}

// ------------------------------------------------------------------------------------------- console command
void R_CaptureSoftShadow_f( const idCmdArgs& args )
{
	ResetAccumulators();
	s_armed = true;

	// Render one clean frame and read it back with the GPU idle, mirroring idRenderSystemLocal::TakeScreenshot
	// (readback inside a live pass would nest command lists / read a half-written target). The FRONTEND half
	// (R_CaptureFrontendView) fires inside Draw() because we are armed; the backend half reads back afterwards.
	commonLocal.WaitGameThread();
	tr.SwapCommandBuffers( NULL, NULL, NULL, NULL, NULL, NULL );
	commonLocal.Draw();
	const emptyCommand_t* cmd = tr.SwapCommandBuffers( NULL, NULL, NULL, NULL, NULL, NULL );
	tr.RenderCommandBuffers( cmd );

	// Read back HERE, before the next SwapCommandBuffers: that swap presents and begins the next frame, whose
	// depth clear would wipe currentDepthImage (the LDR image survives it, but depth does not). Our readback
	// creates its own command list ordered after the frame and mapStagingTexture blocks, so the data is ready.
	if( s_frontendDone )
	{
		R_CaptureBackendFinish();		// screenshot + depth + write
	}
	else
	{
		common->Warning( "soft-shadow capture: no soft-shadow view was rendered (r_useSoftShadowVolumes off?)" );
	}

	tr.SwapCommandBuffers( NULL, NULL, NULL, NULL, NULL, NULL );
	s_armed = false;
	ResetAccumulators();

	// The .cap above is the live-config analytic dump. Now capture EVERYTHING the comparison needs -
	// RT-ref + analytic-bandoff + analytic-bandon frame/term columns + the cvar manifest - forcing each config
	// itself so the result never depends on how the game was launched (an RT-off launch must still yield a
	// valid RT reference, not a black mask).
	R_CaptureShadowRefs_f( args );
}

// Render one clean frame with the GPU idle (mirrors R_CaptureSoftShadow_f) so a following readback sees a
// fully-written, non-nested target. Leaves the frame un-presented; the caller presents after the last readback.
static void R_RenderOneFrame()
{
	commonLocal.WaitGameThread();
	tr.SwapCommandBuffers( NULL, NULL, NULL, NULL, NULL, NULL );
	commonLocal.Draw();
	const emptyCommand_t* cmd = tr.SwapCommandBuffers( NULL, NULL, NULL, NULL, NULL, NULL );
	tr.RenderCommandBuffers( cmd );
}

// ------------------------------------------------------------- self-contained shadow-reference capture
// One command does everything: freezes game logic, cycles every shadow config, dumps both columns per config
// (FRAME = full shaded LDR, TERM = isolated shadow visibility, 1 = lit), then restores every touched cvar.
// All configs render from the same frozen viewpoint so the columns are pixel-aligned for diffing. The debug
// oracle is rt_ref (RT converged: 1024 rays, no denoise, no PCSS) with its area-light radius pinned to the
// analytic light-disk radius (r_shadowPenumbraSize) so RT and analytic amplitudes agree. Extend to another
// visual effect by adding a row to cfgs[]. NOTE: term columns pass through the LDR sRGB/tonemap transfer
// (analytic) and the R8->sRGBA8 blit (RT mask); both are monotonic - relative over/under-occlusion is honest,
// exact numeric agreement needs linearising both offline.
void R_CaptureShadowRefs_f( const idCmdArgs& args )
{
	static const char* const touched[] =
	{
		"g_stopTime", "r_useStencilShadows", "r_useSoftShadowVolumes", "r_useRTShadows",
		"r_rtShadowAnalyticPenumbra", "r_rtShadowDenoise", "r_rtShadowRays", "r_rtShadowSoftRadius",
		"r_softShadowBandMask", "r_softShadowDebugShader",
	};
	const int nTouched = ( int )( sizeof( touched ) / sizeof( touched[0] ) );
	idStrList prev;
	for( int i = 0; i < nTouched; i++ )
	{
		prev.Append( cvarSystem->GetCVarString( touched[i] ) );
	}

	// freeze animated content (flicker lights, particles, AI) so the per-config sub-frames align exactly
	cvarSystem->SetCVarInteger( "g_stopTime", 1 );
	// amplitude agreement: RT area-light radius == analytic light-disk radius
	cvarSystem->SetCVarFloat( "r_rtShadowSoftRadius", cvarSystem->GetCVarFloat( "r_shadowPenumbraSize" ) );

	struct refCfg_t { const char* label; const char* setup; bool termFromMask; };
	static const refCfg_t cfgs[] =
	{
		{ "rt_ref",      "r_useStencilShadows 0 ; r_useSoftShadowVolumes 0 ; r_useRTShadows 1 ; r_rtShadowAnalyticPenumbra 0 ; r_rtShadowDenoise 0 ; r_rtShadowRays 1024", true  },
		{ "ana_bandoff", "r_useRTShadows 0 ; r_useSoftShadowVolumes 1 ; r_softShadowBandMask 0",                                                                          false },
		{ "ana_bandon",  "r_useRTShadows 0 ; r_useSoftShadowVolumes 1 ; r_softShadowBandMask 1",                                                                          false },
	};
	const int nCfg = ( int )( sizeof( cfgs ) / sizeof( cfgs[0] ) );

	for( int c = 0; c < nCfg; c++ )
	{
		idStr setup = cfgs[c].setup;
		setup += "\n";
		cmdSystem->BufferCommandText( CMD_EXEC_NOW, setup.c_str() );

		// FRAME column: full shaded LDR (real coverage, no debug override)
		cvarSystem->SetCVarInteger( "r_softShadowDebugShader", 0 );
		R_RenderOneFrame();
		idStr frame;
		frame.Format( "shadowref_%s_frame.png", cfgs[c].label );
		R_ReadPixelsRGB8( deviceManager->GetDevice(), &backEnd.GetCommonPasses(),
						  globalImages->currentRenderHDRImage->GetTextureHandle(), nvrhi::ResourceStates::ShaderResource, frame.c_str() );

		// TERM column: isolated shadow visibility (1 = lit)
		idStr term;
		term.Format( "shadowref_%s_term.png", cfgs[c].label );
		if( cfgs[c].termFromMask )
		{
			// RT resolves visibility into rtShadowMaskImage (R8; value in the red channel after the RGB8 blit),
			// already produced by the FRAME render above.
			R_ReadPixelsRGB8( deviceManager->GetDevice(), &backEnd.GetCommonPasses(),
							  globalImages->rtShadowMaskImage->GetTextureHandle(), nvrhi::ResourceStates::ShaderResource, term.c_str() );
		}
		else
		{
			// Analytic wedge computes coverage inline; debug mode 8 outputs the raw shadow factor to LDR.
			cvarSystem->SetCVarInteger( "r_softShadowDebugShader", 8 );
			R_RenderOneFrame();
			R_ReadPixelsRGB8( deviceManager->GetDevice(), &backEnd.GetCommonPasses(),
							  globalImages->currentRenderHDRImage->GetTextureHandle(), nvrhi::ResourceStates::ShaderResource, term.c_str() );
		}
		common->Printf( "shadowref: wrote shadowref_%s_{frame,term}.png\n", cfgs[c].label );
	}

	tr.SwapCommandBuffers( NULL, NULL, NULL, NULL, NULL, NULL );	// present the last rendered frame

	// CVAR MANIFEST: everything needed to reproduce/interpret the comparison, independent of how the game was
	// launched. Dumps every renderer cvar (r_shadowPenumbraSize, the RT knobs, band mode, ...) as `set n v`.
	idFile* cf = fileSystem->OpenFileWrite( "shadowref.cvars.cfg", "fs_savepath" );
	if( cf != NULL )
	{
		cvarSystem->WriteFlaggedVariables( CVAR_RENDERER, "set", cf );
		fileSystem->CloseFile( cf );
	}

	// restore every touched cvar
	for( int i = 0; i < nTouched; i++ )
	{
		cvarSystem->SetCVarString( touched[i], prev[i].c_str() );
	}
	common->Printf( "shadowref: done (%d configs) -> shadowref_{rt_ref,ana_bandoff,ana_bandon}_{frame,term}.png + shadowref.cvars.cfg; restored cvars\n", nCfg );
}

// ---------------------------------------------------------- in-engine AUTOMATED locator self-check
// Runs the REAL renderer twice from the same frozen viewpoint - once with the ray-traced oracle (1024 rays),
// once with the soft-shadow + PCSS-locator hybrid - and measures FALSE SHADOW: pixels the oracle leaves lit
// but the hybrid darkens. This exercises the actual frontend, shaders, shadow atlas and uniform plumbing (the
// exact surface the offline CPU test cannot reach), so a regression like "locator shadows everywhere" fails
// here automatically instead of needing a human to eyeball debug modes. Prints a PASS/FAIL verdict.
// Load a map first, then: `testSoftShadowLocator`. Needs ray-query hardware for the oracle.
static float SoftTestLum( const uint8_t* p )
{
	return ( 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2] ) / 255.0f;
}

// ---------------------------------------------------------- MINIMAL-INIT self-test (no game/sound/menu/player)
// The canonical game-free spawnargs->renderLight parser, copied VERBATIM from tools/compilers/dmap/map.cpp
// (its DMAP branch). Kept identical so the harness's lights match exactly what dmap and the editor build - the
// game's idGameEdit::ParseSpawnArgsToRenderLight is the same code, but lives in the game DLL we deliberately
// don't boot here.
static void SelfTestParseLight( const idDict* args, renderLight_t* renderLight )
{
	bool gotTarget, gotUp, gotRight;
	const char* texture;
	idVec3 color;

	memset( renderLight, 0, sizeof( *renderLight ) );

	if( !args->GetVector( "light_origin", "", renderLight->origin ) )
	{
		args->GetVector( "origin", "", renderLight->origin );
	}
	gotTarget = args->GetVector( "light_target", "", renderLight->target );
	gotUp = args->GetVector( "light_up", "", renderLight->up );
	gotRight = args->GetVector( "light_right", "", renderLight->right );
	args->GetVector( "light_start", "0 0 0", renderLight->start );
	if( !args->GetVector( "light_end", "", renderLight->end ) )
	{
		renderLight->end = renderLight->target;
	}
	if( ( gotTarget || gotUp || gotRight ) != ( gotTarget && gotUp && gotRight ) )
	{
		return;
	}
	if( !gotTarget )
	{
		renderLight->pointLight = true;
		args->GetVector( "light_center", "0 0 0", renderLight->lightCenter );
		if( !args->GetVector( "light_radius", "300 300 300", renderLight->lightRadius ) )
		{
			float radius;
			args->GetFloat( "light", "300", radius );
			renderLight->lightRadius[0] = renderLight->lightRadius[1] = renderLight->lightRadius[2] = radius;
		}
	}
	idAngles angles;
	idMat3 mat;
	if( !args->GetMatrix( "light_rotation", "1 0 0 0 1 0 0 0 1", mat ) )
	{
		if( !args->GetMatrix( "rotation", "1 0 0 0 1 0 0 0 1", mat ) )
		{
			if( args->GetAngles( "light_angles", "0 0 0", angles ) || args->GetAngles( "angles", "0 0 0", angles ) )
			{
				angles[0] = idMath::AngleNormalize360( angles[0] );
				angles[1] = idMath::AngleNormalize360( angles[1] );
				angles[2] = idMath::AngleNormalize360( angles[2] );
				mat = angles.ToMat3();
			}
			else
			{
				args->GetFloat( "angle", "0", angles[1] );
				angles[0] = 0;
				angles[1] = idMath::AngleNormalize360( angles[1] );
				angles[2] = 0;
				mat = angles.ToMat3();
			}
		}
	}
	mat[0].FixDegenerateNormal();
	mat[1].FixDegenerateNormal();
	mat[2].FixDegenerateNormal();
	renderLight->axis = mat;

	args->GetVector( "_color", "1 1 1", color );
	renderLight->shaderParms[SHADERPARM_RED]   = color[0];
	renderLight->shaderParms[SHADERPARM_GREEN] = color[1];
	renderLight->shaderParms[SHADERPARM_BLUE]  = color[2];
	args->GetFloat( "shaderParm3", "1", renderLight->shaderParms[SHADERPARM_TIMESCALE] );
	renderLight->shaderParms[SHADERPARM_TIMEOFFSET] = 0;
	args->GetFloat( "shaderParm5", "0", renderLight->shaderParms[5] );
	args->GetFloat( "shaderParm6", "0", renderLight->shaderParms[6] );
	args->GetFloat( "shaderParm7", "0", renderLight->shaderParms[SHADERPARM_MODE] );
	args->GetBool( "noshadows", "0", renderLight->noShadows );
	args->GetBool( "nospecular", "0", renderLight->noSpecular );
	args->GetBool( "parallel", "0", renderLight->parallel );
	args->GetString( "texture", "lights/squarelight1", &texture );
	renderLight->shader = declManager->FindMaterial( texture, false );
}

// RenderScene -> flush to GPU -> read back the LDR result (mirrors the envprobe bake path: pure tr.* calls, no
// commonLocal.Draw, so it works at minimal init with no game thread).
static bool SelfTestRenderReadback( idRenderWorld* rw, renderView_t* rv, std::vector<uint8_t>& out, int& w, int& h )
{
	rw->RenderScene( rv );
	const emptyCommand_t* cmd = tr.SwapCommandBuffers( NULL, NULL, NULL, NULL, NULL, NULL );
	tr.RenderCommandBuffers( cmd );
	tr.SwapCommandBuffers( NULL, NULL, NULL, NULL, NULL, NULL );
	return ReadImageRGBA8( globalImages->currentRenderHDRImage, out, w, h );
}

// Minimal-init soft-shadow self-test. Common.cpp diverts here right after renderSystem->Init(), before the
// game/sound/menu/player boot - so ONLY device+shaders+images+vertexCache+decls are up. Loads the map's
// renderWorld + its real .map lights + the paired capture's camera, renders the RT oracle vs the soft+PCSS
// hybrid through the REAL backend, and prints a false-shadow verdict. Returns false-shadow pixel count (0=pass,
// -1=setup error).
int R_SoftShadowSelfTest( const char* mapName )
{
	// paired capture (neo/tests/data/<basename>.cap) supplies the artifact CAMERA the user captured
	globalImages->LoadDeferredImages();		// splash/base images the render stack deferred at boot

	idRenderWorld* rw = renderSystem->AllocRenderWorld();
	if( !rw->InitFromMap( mapName ) )
	{
		common->Printf( "[selftest] FAIL: renderWorld InitFromMap(%s) failed\n", mapName );
		renderSystem->FreeRenderWorld( rw );
		return -1;
	}

	// Gather the map's REAL lights (game-free parse), and DERIVE the camera from them: the .cap camera is
	// in a different coordinate frame than the loaded .proc here, so we instead sit the camera among the lights
	// (guaranteed to be in the lit, geometry-filled part of the world) and look into the cluster.
	idMapFile map;
	if( !map.Parse( mapName ) )
	{
		common->Printf( "[selftest] FAIL: could not parse %s.map\n", mapName );
		renderSystem->FreeRenderWorld( rw );
		return -1;
	}
	std::vector<renderLight_t> lights;
	idVec3 centroid( 0, 0, 0 );
	for( int e = 0; e < map.GetNumEntities(); e++ )
	{
		idMapEntity* ent = map.GetEntity( e );
		if( ent == NULL || idStr::Icmp( ent->epairs.GetString( "classname" ), "light" ) != 0 ) { continue; }
		renderLight_t rl;
		SelfTestParseLight( &ent->epairs, &rl );
		if( rl.shader == NULL ) { continue; }
		lights.push_back( rl );
		centroid += rl.origin;
	}
	if( lights.empty() )
	{
		common->Printf( "[selftest] FAIL: %s has no lights\n", mapName );
		renderSystem->FreeRenderWorld( rw );
		return -1;
	}
	centroid /= ( float )lights.size();

	// anchor = the light FARTHEST from the centroid (a corner of the lit area); camera sits just behind it and
	// looks toward the centroid, so the lit cluster fills the frame. Add every light within radius of the camera.
	int anchor = 0;
	float bestD = -1.0f;
	for( int i = 0; i < ( int )lights.size(); i++ )
	{
		float d = ( lights[i].origin - centroid ).LengthSqr();
		if( d > bestD ) { bestD = d; anchor = i; }
	}
	idVec3 look = centroid - lights[anchor].origin;
	if( look.LengthSqr() < 1.0f ) { look = idVec3( 1, 0, 0 ); }
	look.Normalize();
	idVec3 camOrg = lights[anchor].origin - look * 48.0f;		// just behind the corner light, not on top of it

	// the NEAREST few lights only - a couple of shadow-casting lights expose the systematic false-shadow bug,
	// and the stencil oracle (single-threaded, whole-world shadow volumes per light) is far too slow with more.
	std::vector<int> order;
	for( int i = 0; i < ( int )lights.size(); i++ ) { order.push_back( i ); }
	std::sort( order.begin(), order.end(), [&]( int a, int b )
	{
		return ( lights[a].origin - camOrg ).LengthSqr() < ( lights[b].origin - camOrg ).LengthSqr();
	} );
	int nLights = 0;
	for( int k = 0; k < ( int )order.size() && nLights < 3; k++ )
	{
		rw->AddLightDef( &lights[order[k]] );
		nLights++;
	}
	renderView_t rv;
	memset( &rv, 0, sizeof( rv ) );
	rv.vieworg = camOrg;
	rv.viewaxis = look.ToMat3();		// idVec3::ToMat3 puts the vector on the forward (X) view axis
	rv.fov_x = 90.0f;
	rv.fov_y = 73.74f;

	cvarSystem->SetCVarInteger( "r_useTemporalAA", 0 );
	cvarSystem->SetCVarInteger( "r_softShadowDebugShader", 0 );
	cvarSystem->SetCVarFloat( "r_rtShadowSoftRadius", cvarSystem->GetCVarFloat( "r_shadowPenumbraSize" ) );

	// Never present: this is a hidden/headless render. GL_BlockingSwapBuffers would block forever on an
	// unmapped surface. The readback's own executeCommandList + mapStagingTexture provides GPU sync (the
	// envprobe bake path relies on exactly this).
	tr.InvalidateSwapBuffers();

	// ORACLE: exact HARD shadows via stencil z-fail (no RT accel structure needed - that is built by the game
	// map load, which minimal init skips). Stencil gives an exact lit/shadowed reference, which is all the
	// FALSE-SHADOW metric needs (hybrid darker than a fully-lit oracle pixel = false shadow). Then HYBRID
	// (soft-shadow volumes + PCSS locator).
	cmdSystem->BufferCommandText( CMD_EXEC_NOW,
								  "r_useRTShadows 0 ; r_useSoftShadowVolumes 0 ; r_shadowMapPCSS 0 ; r_useStencilShadows 1\n" );
	std::vector<uint8_t> ref;
	int rw2 = 0, rh2 = 0;
	SelfTestRenderReadback( rw, &rv, ref, rw2, rh2 );

	cmdSystem->BufferCommandText( CMD_EXEC_NOW, "r_useRTShadows 0 ; r_useSoftShadowVolumes 1 ; r_shadowMapPCSS 1\n" );
	std::vector<uint8_t> test;
	int tw2 = 0, th2 = 0;
	SelfTestRenderReadback( rw, &rv, test, tw2, th2 );

	renderSystem->FreeRenderWorld( rw );

	if( ref.empty() || test.empty() || rw2 != tw2 || rh2 != th2 || rw2 <= 0 )
	{
		common->Printf( "[selftest] FAIL: readback mismatch (%dx%d vs %dx%d)\n", rw2, rh2, tw2, th2 );
		return -1;
	}

	const float litThresh = 0.12f, darkenFrac = 0.5f;
	long litN = 0, falseN = 0;
	std::vector<uint8_t> diff( ( size_t )rw2 * rh2 * 3, 0 );
	for( int i = 0; i < rw2 * rh2; i++ )
	{
		float lr = SoftTestLum( &ref[( size_t )i * 4] ), ls = SoftTestLum( &test[( size_t )i * 4] );
		diff[( size_t )i * 3 + 0] = ( uint8_t )( lr * 255.0f );
		diff[( size_t )i * 3 + 1] = ( uint8_t )( ls * 255.0f );
		if( lr < litThresh ) { continue; }
		litN++;
		if( ls < darkenFrac * lr ) { falseN++; diff[( size_t )i * 3 + 2] = 255; }
	}
	double rate = litN ? ( double )falseN / litN : 0.0;
	const bool pass = ( litN > 1000 ) && ( rate < 0.02 );
	idFile* d = fileSystem->OpenFileWrite( "softtest_falseshadow.ppm", "fs_savepath" );
	if( d != NULL )
	{
		d->Printf( "P6\n%d %d\n255\n", rw2, rh2 );
		d->Write( diff.data(), ( int )diff.size() );
		fileSystem->CloseFile( d );
	}
	common->Printf( "[selftest] map=%s lights=%d  false-shadow %ld / %ld lit px = %.2f%%  ->  %s\n",
					mapName, nLights, falseN, litN, rate * 100.0, pass ? "PASS" : "FAIL" );
	return ( int )falseN;
}

// ---- captured-geometry replay ---------------------------------------------------------------------------------
// The harness renders the LIVE loadGame-quick world at the capture viewpoint, but a DYNAMIC caster present at
// capture time (a physics gib, a moved crate) is absent from that world - so its shadow simply cannot be
// reproduced (verified: cap0019's lights show casters glob=n loc=n at replay though the capture recorded 37).
// To reproduce the EXACT captured frame we rebuild the caster meshes the .cap embeds (MESHVERTS/MESHIDX, world
// space) into one static model and add it to the render world as a shadow-casting entity, so the normal atlas
// occluder path renders it. Cleared after the A/B renders.
static qhandle_t     s_capturedCasterEntity = -1;
static idRenderModel* s_capturedCasterModel = NULL;
static idRenderWorld* s_capturedCasterWorld = NULL;		// the world the entity was added to (primary OR a gate world)

static void R_SoftShadowClearCapturedCasters()
{
	if( s_capturedCasterEntity != -1 && s_capturedCasterWorld != NULL )
	{
		s_capturedCasterWorld->FreeEntityDef( s_capturedCasterEntity );
	}
	s_capturedCasterEntity = -1;
	s_capturedCasterWorld = NULL;
	if( s_capturedCasterModel != NULL )
	{
		renderModelManager->FreeModel( s_capturedCasterModel );
		s_capturedCasterModel = NULL;
	}
}

idCVar r_softShadowReplayCaster( "r_softShadowReplayCaster", "-1", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW,
								 "harness: replay ONLY this caster index from the .cap (isolates the dynamic crate/gib); -1 = all casters" );

static void R_SoftShadowSpawnCapturedCasters( const char* path, idRenderWorld* world = NULL )
{
	R_SoftShadowClearCapturedCasters();
	if( world == NULL )
	{
		world = tr.primaryWorld;
	}
	if( world == NULL )
	{
		common->Warning( "softShadowSpawnCasters: no render world" );
		return;
	}

	FILE* cf = fopen( path, "rb" );
	if( cf == NULL ) { return; }
	capHeader_t hdr;
	if( fread( &hdr, sizeof( hdr ), 1, cf ) != 1 || hdr.magic != CAP_MAGIC || hdr.numMeshVerts == 0 || hdr.numMeshIdx == 0 )
	{
		fclose( cf );
		common->Printf( "[softtest] no captured caster geometry to replay\n" );
		return;
	}
	// block order after the header: lights, edges, casters, then MESHVERTS (float3), MESHIDX (uint32).
	const long castersOff = ( long )sizeof( hdr )
							+ ( long )hdr.numLights * ( long )sizeof( capLight_t )
							+ ( long )hdr.numEdges  * ( long )sizeof( capEdge_t );
	const long meshVertsOff = castersOff + ( long )hdr.numCasters * ( long )sizeof( capCaster_t );
	std::vector<capCaster_t> casters( hdr.numCasters );
	std::vector<float>    mv( ( size_t )hdr.numMeshVerts * 3 );
	std::vector<uint32_t> mi( hdr.numMeshIdx );
	if( fseek( cf, castersOff, SEEK_SET ) != 0
		|| ( hdr.numCasters > 0 && fread( casters.data(), sizeof( capCaster_t ), casters.size(), cf ) != casters.size() )
		|| fseek( cf, meshVertsOff, SEEK_SET ) != 0
		|| fread( mv.data(), sizeof( float ), mv.size(), cf ) != mv.size()
		|| fread( mi.data(), sizeof( uint32_t ), mi.size(), cf ) != mi.size() )
	{
		fclose( cf );
		common->Warning( "testSoftShadowLocator: failed to read captured caster meshes" );
		return;
	}
	fclose( cf );

	// HARNESS ISOLATION: `r_softShadowReplayCaster` picks ONE caster mesh to replay (>= 0), instead of the whole
	// scene's caster soup (which floods the frame - the plate, walls and every gib are all in here). The dynamic
	// object of interest (crate/gib) is one caster; find its index by projecting each caster to the capture view
	// (see peterpan_geometric.py), then replay just it so its shadow is isolated and measurable. -1 = all.
	extern idCVar r_softShadowReplayCaster;
	const int only = r_softShadowReplayCaster.GetInteger();
	uint32_t firstV = 0, numV = hdr.numMeshVerts, firstI = 0, numI = hdr.numMeshIdx;
	if( only >= 0 && only < ( int )hdr.numCasters )
	{
		firstV = casters[only].firstVert;  numV = casters[only].numVerts;
		firstI = casters[only].firstIndex; numI = casters[only].numIndex;
		common->Printf( "[softtest] replaying ONLY caster %d: verts[%u..%u) idx[%u..%u)\n", only, firstV, firstV + numV, firstI, firstI + numI );
	}

	srfTriangles_t* tri = R_AllocStaticTriSurf();
	R_AllocStaticTriSurfVerts( tri, ( int )numV );
	R_AllocStaticTriSurfIndexes( tri, ( int )numI );
	tri->numVerts = ( int )numV;
	tri->numIndexes = ( int )numI;
	for( uint32_t i = 0; i < numV; i++ )
	{
		tri->verts[i].Clear();
		tri->verts[i].xyz.Set( mv[( firstV + i ) * 3 + 0], mv[( firstV + i ) * 3 + 1], mv[( firstV + i ) * 3 + 2] );
	}
	// MESHIDX stores GLOBAL vert indices; rebase to this caster's local vert window.
	for( uint32_t i = 0; i < numI; i++ )
	{
		tri->indexes[i] = ( triIndex_t )( mi[firstI + i] - firstV );
	}
	R_BoundTriSurf( tri );

	modelSurface_t surf;
	surf.id = 0;
	// VISIBLE + shadow-casting replay: a dynamic caster (crate/gib) is absent from `loadGame quick`, so to
	// reconstruct the captured scene headless we must both DRAW the object (to confirm placement and see it) and
	// have it cast (into the atlas for PCSS, and - once traced - for RT). shadow2 was invisible + atlas-only,
	// which hid whether the object was even placed. `_white` draws and casts shadows by default (no noshadows).
	// _white is UNLIT and NON-CASTING (castsShadow=0, receivesLighting=0 - measured), which silently
	// drops the whole replayed model from every light list (ModelHasShadowCastingSurfaces()==false).
	// _default is a normal LIT material that both receives and casts, so the replayed casters shadow
	// and are shadowed like the gameplay originals.
	surf.shader = declManager->FindMaterial( "_default" );
	surf.geometry = tri;

	s_capturedCasterModel = renderModelManager->AllocModel();
	s_capturedCasterModel->InitEmpty( "_softShadowCapturedCasters" );
	s_capturedCasterModel->AddSurface( surf );		// model takes ownership of tri
	s_capturedCasterModel->FinishSurfaces( false );

	// STATIC vertex/index buffers for the replayed mesh: the RT shadow TLAS only accepts
	// static-cache surfaces, so without this the captured caster (the rock) silently vanishes
	// from the ray-traced reference while the analytic path still shadows it.
	{
		nvrhi::CommandListHandle cl = deviceManager->GetDevice()->createCommandList();
		cl->open();
		R_CreateStaticBuffersForTri( *tri, cl );
		cl->close();
		deviceManager->GetDevice()->executeCommandList( cl );
	}

	renderEntity_t re;
	memset( &re, 0, sizeof( re ) );
	re.hModel = s_capturedCasterModel;
	re.axis = mat3_identity;				// MESHVERTS are already world space
	re.origin.Zero();
	re.shaderParms[0] = re.shaderParms[1] = re.shaderParms[2] = re.shaderParms[3] = 1.0f;
	re.noShadow = false;
	s_capturedCasterEntity = world->AddEntityDef( &re );
	s_capturedCasterWorld = world;
	common->Printf( "[softtest] REPLAY captured casters: %u verts / %u tris -> entity %d\n",
					hdr.numMeshVerts, hdr.numMeshIdx / 3, s_capturedCasterEntity );
}

// ---- bench scene reconstruction: DEDUPED per-object captured casters -------------------------------
// The cap stores each caster mesh once PER LIGHT it casts for (~3.5x duplication), so the single
// map-spanning blob R_SoftShadowSpawnCapturedCasters builds is both duplicated AND unculled (every
// light over-processes the whole soup -> the measured 4-min bench hang). This spawns each DISTINCT
// physical object (deduped by first-vertex position + vert count, world space) as its OWN entity with
// tight bounds, so the frontend culls it per light exactly like the live scene. This is the faithful
// bench caster set: world casters AND the dynamic objects (items/props/gibs) the static map parse
// cannot reproduce - the dominant shadow load in heavy rooms.
static std::vector<qhandle_t>     s_benchCasterEntities;
static std::vector<idRenderModel*> s_benchCasterModels;
static idRenderWorld*             s_benchCasterWorld = NULL;

static void R_SoftShadowClearBenchCasters()
{
	if( s_benchCasterWorld != NULL )
	{
		for( qhandle_t h : s_benchCasterEntities )
		{
			if( h != -1 )
			{
				s_benchCasterWorld->FreeEntityDef( h );
			}
		}
	}
	for( idRenderModel* m : s_benchCasterModels )
	{
		if( m != NULL )
		{
			renderModelManager->FreeModel( m );
		}
	}
	s_benchCasterEntities.clear();
	s_benchCasterModels.clear();
	s_benchCasterWorld = NULL;
}

static int R_SoftShadowSpawnBenchCasters( const char* path, idRenderWorld* world )
{
	R_SoftShadowClearBenchCasters();
	if( world == NULL )
	{
		return 0;
	}
	FILE* cf = fopen( path, "rb" );
	if( cf == NULL )
	{
		return 0;
	}
	capHeader_t hdr;
	if( fread( &hdr, sizeof( hdr ), 1, cf ) != 1 || hdr.magic != CAP_MAGIC || hdr.numMeshVerts == 0 || hdr.numMeshIdx == 0 )
	{
		fclose( cf );
		return 0;
	}
	const long castersOff = ( long )sizeof( hdr )
							+ ( long )hdr.numLights * ( long )sizeof( capLight_t )
							+ ( long )hdr.numEdges  * ( long )sizeof( capEdge_t );
	const long meshVertsOff = castersOff + ( long )hdr.numCasters * ( long )sizeof( capCaster_t );
	std::vector<capCaster_t> casters( hdr.numCasters );
	std::vector<float>    mv( ( size_t )hdr.numMeshVerts * 3 );
	std::vector<uint32_t> mi( hdr.numMeshIdx );
	if( fseek( cf, castersOff, SEEK_SET ) != 0
		|| ( hdr.numCasters > 0 && fread( casters.data(), sizeof( capCaster_t ), casters.size(), cf ) != casters.size() )
		|| fseek( cf, meshVertsOff, SEEK_SET ) != 0
		|| fread( mv.data(), sizeof( float ), mv.size(), cf ) != mv.size()
		|| fread( mi.data(), sizeof( uint32_t ), mi.size(), cf ) != mi.size() )
	{
		fclose( cf );
		return 0;
	}
	fclose( cf );

	s_benchCasterWorld = world;
	nvrhi::CommandListHandle cl = deviceManager->GetDevice()->createCommandList();
	cl->open();
	std::set<uint64_t> seen;
	for( uint32_t c = 0; c < hdr.numCasters; c++ )
	{
		const capCaster_t& C = casters[c];
		if( C.numVerts == 0 || C.numIndex == 0 )
		{
			continue;
		}
		// dedup key: first-vertex quantized position + vert count (world space -> one object hashes
		// identically across the lights it cast for). Matches the offline 815->230 measurement.
		const float* v0 = &mv[( size_t )C.firstVert * 3];
		auto q = []( float f ) -> uint64_t { return ( uint64_t )( int64_t )llroundf( f * 8.0f ); };
		const uint64_t key = ( q( v0[0] ) * 73856093ull ) ^ ( q( v0[1] ) * 19349663ull ) ^ ( q( v0[2] ) * 83492791ull ) ^ ( ( uint64_t )C.numVerts << 1 );
		if( !seen.insert( key ).second )
		{
			continue;			// this physical object already spawned (cast for an earlier light)
		}
		srfTriangles_t* tri = R_AllocStaticTriSurf();
		R_AllocStaticTriSurfVerts( tri, ( int )C.numVerts );
		R_AllocStaticTriSurfIndexes( tri, ( int )C.numIndex );
		tri->numVerts = ( int )C.numVerts;
		tri->numIndexes = ( int )C.numIndex;
		for( uint32_t i = 0; i < C.numVerts; i++ )
		{
			tri->verts[i].Clear();
			tri->verts[i].xyz.Set( mv[( C.firstVert + i ) * 3 + 0], mv[( C.firstVert + i ) * 3 + 1], mv[( C.firstVert + i ) * 3 + 2] );
		}
		for( uint32_t i = 0; i < C.numIndex; i++ )
		{
			tri->indexes[i] = ( triIndex_t )( mi[C.firstIndex + i] - C.firstVert );	// MESHIDX is global; rebase local
		}
		R_BoundTriSurf( tri );

		modelSurface_t surf;
		surf.id = 0;
		surf.shader = declManager->FindMaterial( "_default" );	// lit, casts + receives, like the gameplay original
		surf.geometry = tri;

		idRenderModel* model = renderModelManager->AllocModel();
		model->InitEmpty( va( "_softBenchCaster_%u", c ) );
		model->AddSurface( surf );			// takes ownership of tri
		model->FinishSurfaces( false );
		R_CreateStaticBuffersForTri( *tri, cl );

		renderEntity_t re;
		memset( &re, 0, sizeof( re ) );
		re.hModel = model;
		re.axis = mat3_identity;			// MESHVERTS are already world space
		re.origin.Zero();
		re.shaderParms[0] = re.shaderParms[1] = re.shaderParms[2] = re.shaderParms[3] = 1.0f;
		re.noShadow = false;
		s_benchCasterModels.push_back( model );
		s_benchCasterEntities.push_back( world->AddEntityDef( &re ) );
	}
	cl->close();
	deviceManager->GetDevice()->executeCommandList( cl );
	common->Printf( "[softgate] bench caster reconstruction: %u entries -> %d distinct objects spawned\n",
					hdr.numCasters, ( int )s_benchCasterEntities.size() );
	return ( int )s_benchCasterEntities.size();
}

// command wrapper so the headless batch (softShadowShots) can reproduce a capture's DYNAMIC casters (the
// scripted rock/crate/gibs that loadGame-quick does not spawn) from the command buffer - a safe point,
// unlike the mid-frame batch tick. Without this the batch rendered the view but an empty floor.
void R_SoftShadowSpawnCasters_f( const idCmdArgs& args )
{
	if( args.Argc() < 2 )
	{
		common->Warning( "usage: softShadowSpawnCasters <capture.cap>" );
		return;
	}
	R_SoftShadowSpawnCapturedCasters( args.Argv( 1 ) );
}

// =========================================================== com_softShadowGate: the GPU DEFECT GATE
// Minimal-init (no game/sound/menu) corpus gate: every .cap is reconstructed into a real render
// world (its map + its captured dynamic casters + its captured lights), rendered through the SHIPPED
// GPU soft-shadow path at the live resolution (>= 1920x1080 enforced), and the frames are analyzed
// in-process by the shared defect counter (tests/SoftShadowGate.h) against three references:
//   - itself, re-rendered (temporal stability: identical state must give identical frames),
//   - itself, from slightly displaced views (continuity: world-space shadows must not pop),
//   - the in-engine ray-traced oracle (agreement: turds/ants/lit-in-umbra/steps/extent).
// EVERY defect instance is counted individually; the gate is green iff the grand total is ZERO.
namespace
{

struct gateCap_t
{
	capHeader_t hdr;
	std::vector<capLight_t> lights;
	// TRUE captured caster meshes - the defect arbiter's PROXY-INDEPENDENT ground truth. The arbiter must
	// NOT trace the render's consumed edge stream (s_edges): a lossy geometry proxy (r_softShadowProxyBox)
	// makes the shader consume proxy triangles, so an s_edges arbiter would trace the proxy and rubber-stamp
	// its own over-shadowing. These meshes are the recorded real geometry, unchanged by any proxy.
	std::vector<capCaster_t> casters;
	std::vector<float>           meshVerts;	// float3 packed
	std::vector<uint32_t>        meshIdx;	// GLOBAL into meshVerts (v4+ caps are already global)
	// v7: the LIVE per-light renderLight_t state at capture time (index-parallel with `lights`).
	// Empty on pre-v7 caps -> the probe falls back to map-light matching.
	std::vector<capLightParms_t> lightParms;
	idStr mapName;
	idStr path, name;
};

// header + lights + trailing MAPNAME block only - the caster spawn re-reads its own blocks.
bool GateLoadCap( const char* path, gateCap_t& cap )
{
	FILE* f = fopen( path, "rb" );
	if( f == NULL )
	{
		return false;
	}
	if( fread( &cap.hdr, sizeof( cap.hdr ), 1, f ) != 1 || cap.hdr.magic != CAP_MAGIC || cap.hdr.version < 4 )
	{
		fclose( f );
		return false;
	}
	const capHeader_t& h = cap.hdr;
	cap.lights.resize( h.numLights );
	if( h.numLights > 0 && fread( cap.lights.data(), sizeof( capLight_t ), h.numLights, f ) != h.numLights )
	{
		fclose( f );
		return false;
	}

	// TRUE caster meshes for the arbiter (see gateCap_t). Blocks follow lights: [edges][casters][meshVerts]
	// [meshIdx]. On any read failure the arbiter simply has no ground truth and never masks (conservative).
	{
		const long castersOff = ( long )sizeof( h ) + ( long )h.numLights * ( long )sizeof( capLight_t )
								+ ( long )h.numEdges * ( long )sizeof( capEdge_t );
		const long meshVOff = castersOff + ( long )h.numCasters * ( long )sizeof( capCaster_t );
		const long meshIOff = meshVOff + ( long )h.numMeshVerts * 3L * ( long )sizeof( float );
		cap.casters.resize( h.numCasters );
		cap.meshVerts.resize( ( size_t )h.numMeshVerts * 3 );
		cap.meshIdx.resize( h.numMeshIdx );
		bool gok = true;
		if( h.numCasters   && ( fseek( f, castersOff, SEEK_SET ) != 0 || fread( cap.casters.data(),   sizeof( capCaster_t ), cap.casters.size(),   f ) != cap.casters.size() ) )   { gok = false; }
		if( gok && cap.meshVerts.size() && ( fseek( f, meshVOff, SEEK_SET ) != 0 || fread( cap.meshVerts.data(), sizeof( float ),    cap.meshVerts.size(), f ) != cap.meshVerts.size() ) ) { gok = false; }
		if( gok && cap.meshIdx.size()   && ( fseek( f, meshIOff, SEEK_SET ) != 0 || fread( cap.meshIdx.data(),   sizeof( uint32_t ), cap.meshIdx.size(),   f ) != cap.meshIdx.size() ) )   { gok = false; }
		if( !gok ) { cap.casters.clear(); cap.meshVerts.clear(); cap.meshIdx.clear(); }
	}
	const long mapOff = ( long )sizeof( h )
						+ ( long )h.numLights   * ( long )sizeof( capLight_t )
						+ ( long )h.numEdges    * ( long )sizeof( capEdge_t )
						+ ( long )h.numCasters  * ( long )sizeof( capCaster_t )
						+ ( long )h.numMeshVerts * 3L * ( long )sizeof( float )
						+ ( long )h.numMeshIdx  * ( long )sizeof( uint32_t )
						+ ( h.hasDepth ? ( long )h.screenW * h.screenH * ( long )sizeof( float ) : 0L )
						+ ( long )h.numReceivers * ( long )sizeof( capReceiver_t )
						+ ( long )h.numRecvVerts * 3L * ( long )sizeof( float )
						+ ( long )h.numRecvIdx  * ( long )sizeof( uint32_t );
	const uint32_t mapLen = h.reserved[1];
	if( mapLen > 0 && mapLen < 1024 && fseek( f, mapOff, SEEK_SET ) == 0 )
	{
		std::vector<char> buf( mapLen + 1, 0 );
		if( fread( buf.data(), 1, mapLen, f ) == mapLen )
		{
			cap.mapName = buf.data();
			// captures store the session name ("game/erebus1"); InitFromMap/idMapFile want "maps/game/erebus1"
			if( cap.mapName.Icmpn( "maps/", 5 ) != 0 )
			{
				cap.mapName = "maps/" + cap.mapName;
			}
		}
	}
	// v7 LIGHT-PARMS TAIL: locate via the trailing footer [count][magic] at the file END (the tail
	// follows variable-length texture/save blobs; the footer avoids walking them). Sanity-bounded;
	// any inconsistency leaves lightParms empty (map-match fallback).
	{
		uint32_t lpf[2] = { 0, 0 };
		if( fseek( f, -( long )sizeof( lpf ), SEEK_END ) == 0
				&& fread( lpf, sizeof( uint32_t ), 2, f ) == 2
				&& lpf[1] == CAP_LPARM_MAGIC
				&& lpf[0] == cap.lights.size() && lpf[0] <= 4096 )
		{
			const long arrOff = -( long )sizeof( lpf ) - ( long )lpf[0] * ( long )sizeof( capLightParms_t );
			cap.lightParms.resize( lpf[0] );
			if( lpf[0] == 0 || fseek( f, arrOff, SEEK_END ) != 0
					|| fread( cap.lightParms.data(), sizeof( capLightParms_t ), lpf[0], f ) != lpf[0] )
			{
				cap.lightParms.clear();
			}
		}
	}
	fclose( f );
	cap.path = path;
	cap.name = path;
	cap.name.StripPath();
	cap.name.StripFileExtension();
	return true;
}

void GateRenderFrame( idRenderWorld* rw, renderView_t* rv )
{
	// softgate progress watchdog heartbeat: every gate/bench/warm frame counts as progress;
	// a GPU hang or device loss stalls this and the watchdog aborts the run loudly
	::Com_SoftShadowGateHeartbeat();
	rw->RenderScene( rv );
	const emptyCommand_t* cmd = tr.SwapCommandBuffers( NULL, NULL, NULL, NULL, NULL, NULL );
	tr.RenderCommandBuffers( cmd );
	tr.SwapCommandBuffers( NULL, NULL, NULL, NULL, NULL, NULL );
}

// full-precision single-channel readback into the analyzer's image type (R = the shadow term)
bool GateReadR32F( idImage* img, swgate::GateImg& out )
{
	if( img == NULL || img->GetTextureHandle() == NULL )
	{
		return false;
	}
	const int w = img->GetUploadWidth(), h = img->GetUploadHeight();
	float* pic = NULL;
	if( !R_ReadPixelsR32F( deviceManager->GetDevice(), &backEnd.GetCommonPasses(), img->GetTextureHandle(),
						   nvrhi::ResourceStates::ShaderResource, &pic, w, h ) || pic == NULL )
	{
		return false;
	}
	out.W = w;
	out.H = h;
	out.t.assign( pic, pic + ( size_t )w * h );
	R_StaticFree( pic );
	return true;
}

// The per-light slice of idRenderWorldLocal::GenerateAllInteractions: create the STATIC interactions
// (light tris + static shadow-volume indexes) for a JUST-ADDED light against every entity in its areas.
// Runtime-added lights otherwise only get lazy dynamic interactions with numShadowIndexes==0, and the
// frontend's RT caster branch keys on numShadowIndexes>0 - so without this the RT reference sees no
// casters for the probe light (measured: mask never dispatched, all-dark).
void GateCreateStaticInteractionsForLight( idRenderWorld* world, qhandle_t lightHandle )
{
	idRenderWorldLocal* rwl = static_cast<idRenderWorldLocal*>( world );
	if( lightHandle < 0 || lightHandle >= rwl->lightDefs.Num() || rwl->lightDefs[lightHandle] == NULL )
	{
		return;
	}
	idRenderLightLocal* ldef = rwl->lightDefs[lightHandle];
	tr.viewDef = NULL;		// no view-specific optimizations, same as GenerateAllInteractions
	tr.commandList->open();
	int made = 0, seen = 0;
	bool sawSpawned = false;
	for( areaReference_t* lref = ldef->references; lref != NULL; lref = lref->ownerNext )
	{
		portalArea_t* area = lref->area;
		for( areaReference_t* eref = area->entityRefs.areaNext; eref != &area->entityRefs; eref = eref->areaNext )
		{
			idRenderEntityLocal* edef = eref->entity;
			seen++;
			if( edef->parms.hModel != NULL && ( idStr::Icmp( edef->parms.hModel->Name(), "_softShadowCapturedCasters" ) == 0
					|| idStr::Cmpn( edef->parms.hModel->Name(), "_softBenchCaster_", 17 ) == 0 ) )
			{
				// leave the replayed caster soup DYNAMIC: its non-manifold triangles have no silEdges and
				// _white makes no static light tris, so a forced static interaction resolves to EMPTY -
				// which the frontend treats as "statically proven no interaction" and drops the entity
				// (and its shadows) from every list. The lazy dynamic path handles it, as in-game.
				sawSpawned = true;
				continue;
			}
			idInteraction* inter;
			for( inter = edef->firstInteraction; inter != NULL; inter = inter->entityNext )
			{
				if( inter->lightDef == ldef )
				{
					break;
				}
			}
			if( inter != NULL )
			{
				continue;
			}
			inter = idInteraction::AllocAndLink( edef, ldef );
			inter->CreateStaticInteraction( tr.commandList );
			made++;
		}
	}
	tr.commandList->close();
	deviceManager->GetDevice()->executeCommandList( tr.commandList );
	if( cvarSystem->GetCVarBool( "r_rtAccelDebug" ) )
	{
		common->Printf( "[softgate] light interactions: %d entities in light areas, %d created, spawned-casters %s\n",
						seen, made, sawSpawned ? "PRESENT" : "ABSENT" );
	}
}

// float64 ground-truth visibility at a world point vs the light's RECORD triangles (the exact caster
// set the shader consumed, retained by the capture hook): 16 Hammersley disk samples, double-precision
// Moller-Trumbore. This is the ARBITER for reference-vs-analytic disagreements - the RT reference has a
// world-units ray bias that blinds it to contact shadows in seams/cracks, which the exact trace sees.
float GateTruthVisibility( const idVec3& P, const idVec3& L, float diskR, const gateCap_t& cap, int li )
{
	// disk basis
	idVec3 ld = L - P;
	double dist = ld.Length();
	if( dist < 1e-3 )
	{
		return -1.0f;
	}
	if( cap.meshVerts.empty() )
	{
		return -1.0f;			// no captured ground-truth mesh: abstain (keeps every candidate defect)
	}
	idVec3 lz = ld * ( float )( 1.0 / dist );
	idVec3 lx = ( idMath::Fabs( lz.x ) < 0.9f ) ? idVec3( 1, 0, 0 ).Cross( lz ) : idVec3( 0, 1, 0 ).Cross( lz );
	lx.Normalize();
	idVec3 ly = lz.Cross( lx );
	int blocked = 0;
	const int NS = 16;
	for( int s = 0; s < NS; s++ )
	{
		// Hammersley radical-inverse angle + equal-area radius (matches the test oracles)
		uint32_t bits = ( uint32_t )s;
		bits = ( bits << 16 ) | ( bits >> 16 );
		bits = ( ( bits & 0x55555555u ) << 1 ) | ( ( bits & 0xAAAAAAAAu ) >> 1 );
		bits = ( ( bits & 0x33333333u ) << 2 ) | ( ( bits & 0xCCCCCCCCu ) >> 2 );
		bits = ( ( bits & 0x0F0F0F0Fu ) << 4 ) | ( ( bits & 0xF0F0F0F0u ) >> 4 );
		bits = ( ( bits & 0x00FF00FFu ) << 8 ) | ( ( bits & 0xFF00FF00u ) >> 8 );
		double ri = ( double )bits * 2.3283064365386963e-10;
		double r = sqrt( ( s + 0.5 ) / NS ) * diskR;
		double th = ri * 6.283185307179586;
		idVec3 tgt = L + lx * ( float )( r * cos( th ) ) + ly * ( float )( r * sin( th ) );
		double Pd[3] = { P.x, P.y, P.z };
		double Dd[3] = { tgt.x - P.x, tgt.y - P.y, tgt.z - P.z };
		bool hit = false;
		// PROXY-INDEPENDENT ground truth: the light's TRUE captured caster meshes, NOT the consumed edge
		// stream (r_softShadowProxyBox replaces that with box tris; an arbiter tracing the proxy would
		// rubber-stamp its own over-shadow). Trace the full closed mesh: blocked if ANY triangle stops
		// the ray to the disk sample.
		for( const capCaster_t& C : cap.casters )
		{
			if( ( int )C.lightIndex != li )
			{
				continue;
			}
		for( uint32_t k = C.firstIndex; k + 2 < C.firstIndex + C.numIndex && !hit; k += 3 )
		{
			const uint32_t ia = cap.meshIdx[k], ib = cap.meshIdx[k + 1], ic = cap.meshIdx[k + 2];
			double a[3] = { cap.meshVerts[ia * 3], cap.meshVerts[ia * 3 + 1], cap.meshVerts[ia * 3 + 2] };
			double b[3] = { cap.meshVerts[ib * 3], cap.meshVerts[ib * 3 + 1], cap.meshVerts[ib * 3 + 2] };
			double c[3] = { cap.meshVerts[ic * 3], cap.meshVerts[ic * 3 + 1], cap.meshVerts[ic * 3 + 2] };
			double e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
			double e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
			double pv[3] = { Dd[1] * e2[2] - Dd[2] * e2[1], Dd[2] * e2[0] - Dd[0] * e2[2], Dd[0] * e2[1] - Dd[1] * e2[0] };
			double det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2];
			if( fabs( det ) < 1e-14 )
			{
				continue;
			}
			double inv = 1.0 / det;
			double tv[3] = { Pd[0] - a[0], Pd[1] - a[1], Pd[2] - a[2] };
			double u = ( tv[0] * pv[0] + tv[1] * pv[1] + tv[2] * pv[2] ) * inv;
			if( u < -1e-9 || u > 1.0 + 1e-9 )
			{
				continue;
			}
			double q[3] = { tv[1] * e1[2] - tv[2] * e1[1], tv[2] * e1[0] - tv[0] * e1[2], tv[0] * e1[1] - tv[1] * e1[0] };
			double v = ( Dd[0] * q[0] + Dd[1] * q[1] + Dd[2] * q[2] ) * inv;
			if( v < -1e-9 || u + v > 1.0 + 1e-9 )
			{
				continue;
			}
			double t = ( e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2] ) * inv;
			// near-clip MUST match the shipped shader's contact-shadow bias (softwedge_coverage.inc.hlsl
			// 'tt > 1e-4f && tt <= 1.0f'): the shader suppresses occluders within ~1e-4 of the ray length
			// of the receiver (anti-acne - a 1e-6 clip mints self-shadow acne on every near-contact
			// surface). An arbiter with a 100x tighter clip sees "umbra" the shader intentionally (and
			// correctly) does not cast, minting phantom LIT_IN_UMBRA at sub-0.05-unit near-contacts. Trace
			// the SAME contact model the shader ships so the truth judges the shader's real output.
			if( t > 1e-4 && t <= 1.0 )
			{
				hit = true;
			}
		}
		}
		if( hit )
		{
			blocked++;
		}
	}
	return 1.0f - ( float )blocked / NS;
}

void GateSetup( const gateCap_t& cap, const capLight_t& light )
{
	// pinned baseline first, then the per-probe overrides on top of it
	R_SoftShadowPinTestConfig( false );
	cvarSystem->SetCVarInteger( "r_skipAmbient", 1 );		// interaction term only: no emissive/ambient pollution
	// MASKED OCCLUSION CULLING OFF for every gate render - the ROOT CAUSE of the intermittent
	// "init deadlock" (2026-08-20): the gate's RECONSTRUCTED caster models feed MOC geometry its
	// AVX2 rasterizer walks out of bounds - a deterministic OOB whose symptom ASLR picks per run
	// (clean / glibc "double free or corruption (out)" wedging in the abort path / straight SIGSEGV
	// in RenderTriangles, all caught under gdb). The gate never wants MOC anyway: it is a
	// still-frame CORRECTNESS instrument that needs the full deterministic caster set - an entity
	// culled by a software occlusion raster would silently thin the casters. Normal gameplay never
	// sees the reconstructed models, so the game keeps MOC.
	cvarSystem->SetCVarInteger( "r_useMaskedOcclusionCulling", 0 );
	cvarSystem->SetCVarFloat( "r_shadowPenumbraSize", light.penumbraSize );
	cvarSystem->SetCVarFloat( "r_rtShadowSoftRadius", light.penumbraSize );
}

} // namespace

// Deterministic motion model for the surf-cache A/B bench (com_softShadowGateBenchMotion): perturb a
// base camera pose by frame index across 4 equal segments - shake / rotate / move / combo - so the
// bench exercises the screen-shake, panning and dolly the player produces, and their combination. No
// RNG (pure sinusoids of the frame index), so two runs render bit-identical paths.
static void R_SoftShadowBenchMotionPose( const idVec3& baseOrg, const idAngles& baseAng, int f, int total, renderView_t* rv )
{
	const float t = ( float )f;
	const int seg = ( total > 0 ) ? Min( 3, ( f * 4 ) / total ) : 3;	// 0 shake, 1 rot, 2 move, 3 combo
	const bool shake = ( seg == 0 || seg == 3 );
	const bool rot   = ( seg == 1 || seg == 3 );
	const bool move  = ( seg == 2 || seg == 3 );

	idAngles ang = baseAng;
	idVec3   org = baseOrg;
	if( shake )				// high-frequency sub-degree camera rattle (weapon/impact shake)
	{
		ang.yaw   += 0.35f * idMath::Sin( t * 13.1f );
		ang.pitch += 0.30f * idMath::Sin( t * 11.7f + 1.3f );
		ang.roll  += 0.20f * idMath::Sin( t * 17.3f );
	}
	if( rot )				// steady pan + slow look up/down
	{
		ang.yaw   += 0.9f * t;
		ang.pitch += 8.0f * idMath::Sin( t * 0.05f );
	}
	const idMat3 axis = ang.ToMat3();
	if( move )				// dolly along forward + strafe along the side axis
	{
		org += axis[0] * ( 64.0f * idMath::Sin( t * 0.045f ) );
		org += axis[1] * ( 40.0f * idMath::Sin( t * 0.031f ) );
	}
	rv->vieworg  = org;
	rv->viewaxis = axis;
}

// Iterates the corpus, prints one defect line per capture x light and the grand total; returns the
// total defect count (the process exit code, clamped by the caller).
// FIXME: This function is monstrous. Do not add more gates here. 
int R_SoftShadowGate( const char* arg )
{
	using namespace swgate;

	// SINGLE-INSTANCE GUARD: two concurrent gate runs contend for the GPU and clobber each other's
	// softgate_* outputs, silently corrupting BOTH verdicts. Hold an
	// exclusive advisory lock for the whole run; a second instance aborts LOUDLY as a SETUP failure.
	// The fd is deliberately leaked to process exit (the gate quits the process when done), which
	// releases the flock; a crashed/killed run releases it automatically too - no stale-lock state.
#if defined(__linux__) || defined(__APPLE__)
	{
		const int swLockFd = open( "softgate.lock", O_CREAT | O_RDWR, 0644 );
		if( swLockFd < 0 || flock( swLockFd, LOCK_EX | LOCK_NB ) != 0 )
		{
			common->Printf( "[softgate] FATAL: softgate.lock held - " );
			return 1;
		}
	}
#endif

	globalImages->LoadDeferredImages();
	tr.InvalidateSwapBuffers();		// headless: never present (blocking swap would hang on a hidden surface)

	// ---- corpus --------------------------------------------------------------------------------
	extern int Sys_ListFiles( const char* directory, const char* extension, idStrList& list );
	idStr dir = ( arg == NULL || arg[0] == '\0' || idStr::Icmp( arg, "corpus" ) == 0 ) ? "../tests/data" : arg;
	idStrList files;
	Sys_ListFiles( dir, ".cap", files );
	if( files.Num() > 1 )
	{
		std::sort( &files[0], &files[0] + files.Num(), []( const idStr& a, const idStr& b )
		{
			return idStr::Icmp( a, b ) < 0;
		} );
	}
	common->Printf( "[softgate] corpus: %s  (%d captures)\n", dir.c_str(), files.Num() );

	// every shadow-relevant cvar the gate touches is restored afterwards so an archived config is never polluted
	static const char* const touched[] =
	{
		"r_useRTShadows", "r_useStencilShadows", "r_useSoftShadowVolumes", "r_useShadowMapping", "r_useShadowAtlas",
		"r_shadowMapPCSS", "r_shadowMapPCSSScale", "r_softShadowAAM", "r_softShadowBandMask", "r_softShadowStencilOnly",
		"r_softShadowEmergentUmbra", "r_softShadowContinuous", "r_useTemporalAA", "r_softShadowDebugShader",
		"r_skipAmbient", "r_skipShadows", "r_shadowPenumbraSize", "r_rtShadowSoftRadius", "r_rtShadowRays",
		"r_rtShadowDenoise", "r_rtShadowAnalyticPenumbra", "r_rtShadowBias",
		"r_useMaskedOcclusionCulling", "r_useDDGI",
		"r_softShadowCompute", "r_softShadowFaceCoverage", "r_softShadowSurfCache",
	};
	const int nTouched = ( int )( sizeof( touched ) / sizeof( touched[0] ) );
	idStrList prev;
	for( int i = 0; i < nTouched; i++ )
	{
		prev.Append( cvarSystem->GetCVarString( touched[i] ) );
	}

	// MOC off for the WHOLE run (probes set it per light in GateSetup; this covers the bench frames
	// and any render before the first GateSetup) - see the root-cause note in GateSetup
	cvarSystem->SetCVarInteger( "r_useMaskedOcclusionCulling", 0 );
	// DDGI off for the WHOLE run: the gate renders with r_skipAmbient 1, so the probe irradiance is
	// never consumed - yet DdgiPass::Render rebuilt BLAS/TLAS from the reconstructed occluders EVERY
	// frame (measured wedge site: malloc inside buildBottomLevelAccelStruct on a corrupted heap;
	// ASan-clean on our code, so the corruption exposure sits in the uninstrumented accel path this
	// churn hammers). The RT shadow ORACLE owns its own DdgiAccelStructures instance and is
	// unaffected. Pure waste removal + corruption-surface removal; restored after the run.
	cvarSystem->SetCVarInteger( "r_useDDGI", 0 );

	// SURF-FOLD CACHE: force ON for the whole DEFECT gate so the cache is TESTED, not opt-in - a cache
	// that erodes umbra (or introduces any turd/ant/step/extent defect) is now a GATE defect vs the RT
	// oracle, not a silent green. The cache lives in the compute term path (r_softShadowCompute 1) on the
	// face-coverage walk (r_softShadowFaceCoverage 1); each probe light is warmed through the shipped
	// WarmLight driver just before its analytic still, so anaA IS the cached frame the detectors score.
	// The cache-OFF exact walk is still rendered per light (the R4 seam probe) and compared. Restored
	// after the run via the `touched` list.
	extern idCVar com_softShadowGateSurfCache;
	const int swGateCache = com_softShadowGateSurfCache.GetInteger();
	cvarSystem->SetCVarInteger( "r_softShadowCompute", 1 );
	cvarSystem->SetCVarInteger( "r_softShadowFaceCoverage", 1 );
	cvarSystem->SetCVarInteger( "r_softShadowSurfCache", swGateCache );

	GateCfg cfg;
	std::vector<GateDefect> all;
	int capsRun = 0, lightsRun = 0;

	// LAUNCH-time shadow config, restored for the BENCH renders: the probes pin their own baseline, but
	// the bench must honour +set overrides so shadow methods can be A/B-timed from the command line.
	const int launchSoft    = cvarSystem->GetCVarInteger( "r_useSoftShadowVolumes" );
	const int launchPCSS    = cvarSystem->GetCVarInteger( "r_shadowMapPCSS" );
	const int launchStencil = cvarSystem->GetCVarInteger( "r_useStencilShadows" );
	const int launchMapping = cvarSystem->GetCVarInteger( "r_useShadowMapping" );
	const int launchContact = cvarSystem->GetCVarInteger( "r_shadowMapPCSSAnalyticContact" );
	// The probes set r_shadowPenumbraSize to EACH capture light's stored radius (GateSetup) and never
	// restore it, so the bench inherited whichever light was probed LAST - a different, arbitrary disk
	// radius per capture. That taints every A/B: the coverage cost scales with the disk, and the
	// subdivision threshold is a multiple of it. Bench with the LAUNCH radius, like the shipped game.
	const float launchPenumbra = cvarSystem->GetCVarFloat( "r_shadowPenumbraSize" );

	// map world cache: consecutive captures share the map, load it once
	idStr loadedMap;
	idRenderWorld* rw = NULL;
	std::vector<renderLight_t> mapLights;
	std::vector<renderEntity_t> mapModels;		// static model entities (func_static etc.) - the live game's casters

	for( int fi = 0; fi < files.Num(); fi++ )
	{
		gateCap_t cap;
		idStr full = dir + "/" + files[fi];
		if( !GateLoadCap( full.c_str(), cap ) )
		{
			common->Printf( "[softgate] %s: UNREADABLE capture -> SETUP defect\n", files[fi].c_str() );
			GateDefect d;
			d.kind = GATE_SETUP;
			all.push_back( d );
			continue;
		}
		if( cap.mapName.IsEmpty() )
		{
			common->Printf( "[softgate] %s: capture names no map -> SETUP defect\n", cap.name.c_str() );
			GateDefect d;
			d.kind = GATE_SETUP;
			all.push_back( d );
			continue;
		}

		// ---- capture filter (com_softShadowGateCaps): substring whitelist, BEFORE any world/light
		// setup so a filtered sweep of the heavy caps costs seconds, not the full-corpus minutes.
		// Skipped caps never reach capsRun++, so the totals denominator counts only what actually ran.
		{
			const char* swFilt = cvarSystem->GetCVarString( "com_softShadowGateCaps" );
			if( swFilt != NULL && swFilt[0] != '\0' )
			{
				bool swMatch = false;
				for( const char* p = swFilt; !swMatch; )
				{
					const char* c = strchr( p, ',' );
					idStr tok = c != NULL ? idStr( p, 0, ( int )( c - p ) ) : idStr( p );
					tok.StripLeading( ' ' );
					tok.StripTrailingWhitespace();
					swMatch = !tok.IsEmpty() && cap.name.Find( tok.c_str(), false ) >= 0;
					if( c == NULL )
					{
						break;
					}
					p = c + 1;
				}
				if( !swMatch )
				{
					common->Printf( "[softgate] SKIP %s (filter)\n", cap.name.c_str() );
					continue;
				}
			}
		}

		// ---- world ----------------------------------------------------------------------------
		if( rw == NULL || loadedMap != cap.mapName )
		{
			R_SoftShadowClearCapturedCasters();
			if( rw != NULL )
			{
				renderSystem->FreeRenderWorld( rw );
				rw = NULL;
			}
			mapLights.clear();
			mapModels.clear();
			// complete the world the way a REAL map load does (Common_load.cpp order):
			//  - Begin/EndLevelLoad pins every loaded model into the STATIC vertex cache; the RT
			//    shadow TLAS only accepts static-cache surfaces, so without this the reference is
			//    an empty TLAS (all-dark mask) and the gate measures nothing;
			//  - GenerateAllInteractions allocates interactionTable, which the frontend's
			//    R_AddSingleLight reads UNGUARDED once any entity exists (the minimal-init
			//    self-test survived without it purely because it never added an entity).
			renderSystem->BeginLevelLoad();
			idRenderWorld* nw = renderSystem->AllocRenderWorld();
			const bool mapOk = nw->InitFromMap( cap.mapName.c_str() );
			renderSystem->EndLevelLoad();
			if( !mapOk )
			{
				renderSystem->FreeRenderWorld( nw );
				common->Printf( "[softgate] %s: InitFromMap(%s) FAILED -> SETUP defect\n", cap.name.c_str(), cap.mapName.c_str() );
				GateDefect d;
				d.kind = GATE_SETUP;
				all.push_back( d );
				continue;
			}
			rw = nw;
			loadedMap = cap.mapName;
			rw->GenerateAllInteractions();
			idMapFile mapFile;
			if( mapFile.Parse( cap.mapName.c_str() ) )
			{
				for( int e = 0; e < mapFile.GetNumEntities(); e++ )
				{
					idMapEntity* ent = mapFile.GetEntity( e );
					if( idStr::Icmp( ent->epairs.GetString( "classname" ), "light" ) == 0 )
					{
						renderLight_t rl;
						SelfTestParseLight( &ent->epairs, &rl );
						mapLights.push_back( rl );
						continue;
					}
					// STATIC MODEL ENTITIES (func_static, mover machinery, ...) are the live game's
					// casters and roughly DOUBLE the soft-record stream vs the bare worldspawn
					// (measured erebus1 cap0061: 114k records in-game vs 56k without them - the
					// gate bench under-read the shipped soft cost ~3x). Add every entity whose
					// spawnargs resolve to a loadable non-animated model; the bench places them so
					// its frame carries the live caster density. Animated md5 meshes are skipped
					// (their live pose is gameplay state the gate cannot know).
					const char* mdl = ent->epairs.GetString( "model" );
					if( mdl == NULL || mdl[0] == '\0' || idStr::Icmp( ent->epairs.GetString( "classname" ), "worldspawn" ) == 0 )
					{
						continue;
					}
					if( idStr( mdl ).Find( ".md5mesh", false ) >= 0 || ent->epairs.GetBool( "hide" ) || ent->epairs.GetBool( "noshadows" ) )
					{
						continue;
					}
					// minimal spawn-arg parse: gameEdit->ParseSpawnArgsToRenderEntity needs the
					// GAME-registered modelDef decl type, which the gate's minimal init never
					// registers (Sys_Error "bad type"). Static casters need only model + placement.
					renderEntity_t re;
					memset( &re, 0, sizeof( re ) );
					re.hModel = renderModelManager->FindModel( mdl );
					if( re.hModel == NULL || re.hModel->IsDefaultModel() )
					{
						continue;
					}
					re.bounds = re.hModel->Bounds( NULL );
					ent->epairs.GetVector( "origin", "0 0 0", re.origin );
					if( !ent->epairs.GetMatrix( "rotation", "1 0 0 0 1 0 0 0 1", re.axis ) )
					{
						float angle = ent->epairs.GetFloat( "angle" );
						if( angle != 0.0f )
						{
							re.axis = idAngles( 0.0f, angle, 0.0f ).ToMat3();
						}
						else
						{
							re.axis.Identity();
						}
					}
					re.shaderParms[0] = re.shaderParms[1] = re.shaderParms[2] = re.shaderParms[3] = 1.0f;
					mapModels.push_back( re );
				}
			}
			common->Printf( "[softgate] loaded %s (%d map lights, %d model entities)\n",
							cap.mapName.c_str(), ( int )mapLights.size(), ( int )mapModels.size() );
		}

		// NOTE: the captured DYNAMIC casters (the scripted rock etc.) are deliberately NOT replayed here.
		// Duplicating gameplay-state geometry over a fresh-loaded map makes the probe scene inconsistent
		// (the soup model paints unlit checkerboard over real receivers, and its RT-vs-analytic plumbing
		// asymmetries mint defects that are capture-fidelity problems, not renderer problems). The gate
		// tests the RENDERER on a self-consistent scene: the live map + its real lights. In-game replay
		// via `softShadowSpawnCasters` still exists for scene-reconstruction work.

		// ---- camera (straight from the capture; same map => same world frame) ------------------
		renderView_t rv;
		memset( &rv, 0, sizeof( rv ) );
		rv.vieworg.Set( cap.hdr.vieworg[0], cap.hdr.vieworg[1], cap.hdr.vieworg[2] );
		rv.viewaxis[0].Set( cap.hdr.viewaxis[0], cap.hdr.viewaxis[1], cap.hdr.viewaxis[2] );
		rv.viewaxis[1].Set( cap.hdr.viewaxis[3], cap.hdr.viewaxis[4], cap.hdr.viewaxis[5] );
		rv.viewaxis[2].Set( cap.hdr.viewaxis[6], cap.hdr.viewaxis[7], cap.hdr.viewaxis[8] );
		rv.fov_x = cap.hdr.fovx;
		rv.fov_y = cap.hdr.fovy;

		// CINEMATIC-CAMERA RESCUE: cutscene cameras can sit outside the playable BSP (sealed
		// cinematic rooms, skybox vantages) - point-in-solid resolves to area -1 and the view
		// renders NOTHING, so every light "draws 0 px" and the capture is silently untestable
		// (cap0010, the hunter-arena cutscene: the exact scene a playtest saw ants in). Nudge the
		// camera along its forward axis, then vertically, until it lands in a real area; the view
		// direction still frames the captured scene. Loud either way.
		{
			int swVArea = rw->PointInArea( rv.vieworg );
			if( swVArea < 0 )
			{
				const idVec3 swVFwd = rv.viewaxis[0];
				const idVec3 swVOrg0 = rv.vieworg;
				for( int st = 1; st <= 64 && swVArea < 0; st++ )
				{
					const idVec3 p = swVOrg0 + swVFwd * ( ( float )st * 4.0f );
					swVArea = rw->PointInArea( p );
					if( swVArea >= 0 )
					{
						rv.vieworg = p;
					}
				}
				for( int st = 1; st <= 32 && swVArea < 0; st++ )
				{
					const idVec3 p = swVOrg0 - idVec3( 0.0f, 0.0f, ( float )st * 8.0f );
					swVArea = rw->PointInArea( p );
					if( swVArea >= 0 )
					{
						rv.vieworg = p;
					}
				}
				if( swVArea >= 0 )
				{
					common->Printf( "[softgate] %s: capture camera IN SOLID (cinematic?) - nudged %.0f units to area %d\n",
									cap.name.c_str(), ( rv.vieworg - swVOrg0 ).Length(), swVArea );
				}
				else
				{
					common->Printf( "[softgate] %s: capture camera IN SOLID and no rescue within 256u - expect 0-px lights\n",
									cap.name.c_str() );
				}
			}
		}

		capsRun++;

		// ---- rank the captured lights by soft-edge count and probe EVERY one that draws. The
		// single-dominant-light shortcut was a measured blind spot: the erebus1_14 halo the user saw
		// in-game lived under a secondary light and the gate passed. Lights that draw <1000 px at
		// this camera are skipped silently (a matched light can sit behind a closed door); only if
		// NONE draws is it a SETUP defect.
		std::vector<int> probeLights;
		// bench-only mode: skip every per-light probe (they cost ~40-60 s per capture) and go straight
		// to the timed full frames - the fast loop for perf-config sweeps. NOT a correctness verdict:
		// defect counting needs the probes, so PASS from a bench-only run means nothing.
		extern idCVar com_softShadowGateBenchOnly;
		if( !com_softShadowGateBenchOnly.GetBool() )
		for( int li = 0; li < ( int )cap.lights.size(); li++ )
		{
			if( cap.lights[li].edgeCount > 0 )
			{
				probeLights.push_back( li );
			}
		}
		std::sort( probeLights.begin(), probeLights.end(), [&]( int a, int b )
		{
			return cap.lights[a].edgeCount > cap.lights[b].edgeCount;
		} );
		if( probeLights.empty() && !com_softShadowGateBenchOnly.GetBool() )
		{
			common->Printf( "[softgate] %s: no light with soft edges -> SETUP defect (degenerate capture)\n", cap.name.c_str() );
			GateDefect d;
			d.kind = GATE_SETUP;
			all.push_back( d );
		}
		bool capProbed = false;

		// ---- per light (every drawing light is probed) -----------------------------------------
		for( int pi = 0; pi < ( int )probeLights.size(); pi++ )
		{
			const int li = probeLights[pi];
			const capLight_t& cl = cap.lights[li];
			idVec3 clOrg( cl.origin[0], cl.origin[1], cl.origin[2] );

			renderLight_t rl;
			if( li < ( int )cap.lightParms.size() )
			{
				// v7: rebuild the light from its CAPTURED live state - scripts move/retint cutscene
				// lights, so the map-parse state can be wrong by hundreds of units (cap0010: the
				// matched pre-cutscene light drew 0 px at the cinematic camera)
				const capLightParms_t& LP = cap.lightParms[li];
				memset( &rl, 0, sizeof( rl ) );
				rl.origin.Set( LP.origin[0], LP.origin[1], LP.origin[2] );
				rl.axis[0].Set( LP.axis[0], LP.axis[1], LP.axis[2] );
				rl.axis[1].Set( LP.axis[3], LP.axis[4], LP.axis[5] );
				rl.axis[2].Set( LP.axis[6], LP.axis[7], LP.axis[8] );
				rl.lightRadius.Set( LP.lightRadius[0], LP.lightRadius[1], LP.lightRadius[2] );
				rl.lightCenter.Set( LP.lightCenter[0], LP.lightCenter[1], LP.lightCenter[2] );
				rl.target.Set( LP.target[0], LP.target[1], LP.target[2] );
				rl.right.Set( LP.right[0], LP.right[1], LP.right[2] );
				rl.up.Set( LP.up[0], LP.up[1], LP.up[2] );
				rl.start.Set( LP.start[0], LP.start[1], LP.start[2] );
				rl.end.Set( LP.end[0], LP.end[1], LP.end[2] );
				for( int k = 0; k < 12 && k < MAX_ENTITY_SHADER_PARMS; k++ )
				{
					rl.shaderParms[k] = LP.shaderParms[k];
				}
				rl.pointLight = LP.pointLight != 0;
				rl.parallel   = LP.parallel != 0;
				rl.noShadows  = LP.noShadows != 0;
				rl.shader = declManager->FindMaterial( LP.shaderName[0] != '\0' ? LP.shaderName : "lights/squarelight1", false );
				if( rl.shader == NULL )
				{
					rl.shader = declManager->FindMaterial( "lights/squarelight1", false );
				}
			}
			else
			{
			// match the real .map light whose global origin is the captured one (globalLightOrigin
			// includes light_center, so try both origin and origin+center)
			int best = -1;
			float bestD = 16.0f * 16.0f;
			for( int m = 0; m < ( int )mapLights.size(); m++ )
			{
				float d0 = ( mapLights[m].origin - clOrg ).LengthSqr();
				float d1 = ( mapLights[m].origin + mapLights[m].lightCenter - clOrg ).LengthSqr();
				float d = Min( d0, d1 );
				if( d < bestD )
				{
					bestD = d;
					best = m;
				}
			}
			if( best >= 0 )
			{
				rl = mapLights[best];
			}
			else
			{
				// purely dynamic light (no .map source): synthesize a point light at the captured
				// origin. The probes are self-consistent (analytic and RT see the same light), so
				// this still gates the renderer - just log the approximation loudly.
				memset( &rl, 0, sizeof( rl ) );
				rl.pointLight = true;
				rl.origin = clOrg;
				rl.axis = mat3_identity;
				rl.lightRadius.Set( 300, 300, 300 );
				rl.shaderParms[SHADERPARM_RED] = rl.shaderParms[SHADERPARM_GREEN] = rl.shaderParms[SHADERPARM_BLUE] = 1.0f;
				rl.shaderParms[SHADERPARM_TIMESCALE] = 1.0f;
				rl.shader = declManager->FindMaterial( "lights/squarelight1", false );
				common->Printf( "[softgate] %s L%d: no .map light at (%.0f %.0f %.0f) - SYNTHESIZED point light\n",
								cap.name.c_str(), li, clOrg.x, clOrg.y, clOrg.z );
			}
			}	// pre-v7 map-match fallback

			qhandle_t lh = rw->AddLightDef( &rl );
			GateCreateStaticInteractionsForLight( rw, lh );
			GateSetup( cap, cl );
			lightsRun++;

			std::vector<GateDefect> defects;
			swgate::GateImg mask1, rt, anaA, anaB, depthA;

			// R0 - interaction-coverage mask: shadows skipped, term==1 exactly where this light's
			// interaction draws. Everything outside is excluded from every probe. The mask is deliberately
			// TIGHT (only high-confidence fully-lit-and-unshadowed pixels): loosening it to the light's full
			// geometric reach folds the whole penumbra into the tested region and surfaces thousands of
			// penumbra-level analytic-vs-RT disagreements that this gate is not meant to adjudicate (measured
			// 180 -> 6029). A cinematic camera parked in a fully-penumbral dark corner therefore has NO valid
			// pixels here and cannot be gated - that is a bad capture, recreated via softShadowRecapture from
			// a gameplay viewpoint, not a reason to widen the mask.
			cvarSystem->SetCVarInteger( "r_skipShadows", 1 );
			cvarSystem->SetCVarInteger( "r_useRTShadows", 0 );
			cvarSystem->SetCVarInteger( "r_useSoftShadowVolumes", 1 );
			cvarSystem->SetCVarInteger( "r_softShadowDebugShader", 8 );
			GateRenderFrame( rw, &rv );
			GateReadR32F( globalImages->currentRenderHDRImage, mask1 );

			// R1 - the ray-traced reference (converged, no denoise); the mask image IS the term.
			// r_useShadowMapping 1 is LOAD-BEARING: with RT on, the frontend's soft/stencil branch is
			// skipped and only the shadow-map occluder path still fills vLight->globalShadows - which
			// the RT dispatch gate (R_LightUsesRTShadows) requires as its "light has casters" signal.
			// Without it the trace never dispatches and the mask reads all-dark (measured).
			cvarSystem->SetCVarInteger( "r_skipShadows", 0 );
			cvarSystem->SetCVarInteger( "r_useSoftShadowVolumes", 0 );
			cvarSystem->SetCVarInteger( "r_useShadowMapping", 1 );
			cvarSystem->SetCVarInteger( "r_softShadowDebugShader", 0 );
			cvarSystem->SetCVarInteger( "r_useRTShadows", 1 );
			cvarSystem->SetCVarInteger( "r_rtShadowRays", 512 );
			cvarSystem->SetCVarInteger( "r_rtShadowDenoise", 0 );
			cvarSystem->SetCVarInteger( "r_rtShadowAnalyticPenumbra", 0 );
			// reference ray bias stays at the mature default (1.5, slope-scaled): tightening it to see
			// sub-unit contact shadows just trades seam blindness for the reference's own reconstruction
			// acne (measured: LIT_IN_UMBRA 15 -> 206 at 0.25). The analyzer instead EXCLUDES agreement
			// defects at depth creases, where the reference is structurally untrustworthy either way.
			GateRenderFrame( rw, &rv );
			GateReadR32F( globalImages->rtShadowMaskImage, rt );

			// diagnostic mode (r_rtAccelDebug): dump what each reference render actually produced,
			// so a degenerate probe is inspectable as an image instead of argued about from counts
			extern idCVar r_rtAccelDebug;
			if( r_rtAccelDebug.GetBool() )
			{
				R_ReadPixelsRGB8( deviceManager->GetDevice(), &backEnd.GetCommonPasses(), globalImages->currentRenderHDRImage->GetTextureHandle(),
								  nvrhi::ResourceStates::ShaderResource, va( "softgate_dbg_%s_L%d_rtframe.png", cap.name.c_str(), li ) );
				R_ReadPixelsRGB8( deviceManager->GetDevice(), &backEnd.GetCommonPasses(), globalImages->rtShadowMaskImage->GetTextureHandle(),
								  nvrhi::ResourceStates::ShaderResource, va( "softgate_dbg_%s_L%d_rtmask.png", cap.name.c_str(), li ) );
			}

			// R2/R3 - the SHIPPED analytic term, twice back to back (temporal probe)
			cvarSystem->SetCVarInteger( "r_useRTShadows", 0 );
			cvarSystem->SetCVarInteger( "r_useShadowMapping", 0 );		// back to the pinned soft-path baseline
			cvarSystem->SetCVarInteger( "r_useSoftShadowVolumes", 1 );
			cvarSystem->SetCVarInteger( "r_softShadowDebugShader", 8 );

			// SURF-FOLD CACHE: warm this light so the analytic stills below render the CACHED term and the
			// defect probes score the cache (steady state, not the miss->built transition) against the RT
			// oracle. The cache is forced ON for the whole defect gate (see the run-level force above).
			{
				SoftShadowSurfCache* swSurf = backEnd.GetSoftShadowSurfCache();
				idRenderWorldLocal* rwlWarm = static_cast<idRenderWorldLocal*>( rw );
				if( cvarSystem->GetCVarInteger( "r_softShadowSurfCache" ) != 0 && swSurf != NULL
						&& lh >= 0 && lh < rwlWarm->lightDefs.Num() && rwlWarm->lightDefs[lh] != NULL )
				{
					// warm THIS light through the SHIPPED WarmLight driver: one command-list submit +
					// waitForIdle, watchdog-safe, exactly like WarmMapBurst does per light. The runtime is
					// read-only now (the old per-frame GateRenderFrame build loop is DEAD - the cache builds
					// ONLY here), so the warm must be explicit. A fresh gate light is a new fingerprint =>
					// full reseed + build-to-completion inside WarmLight.
					nvrhi::IDevice* wdev = deviceManager->GetDevice();
					nvrhi::CommandListHandle wcl = wdev->createCommandList();
					wcl->open();
					swSurf->WarmLight( wcl, rwlWarm->lightDefs[lh] );
					wcl->close();
					wdev->executeCommandList( wcl );
					wdev->waitForIdle();
				}
			}
			// CONTRIBUTOR-CACHE warm: like the surf cache above, the contrib cache must be WARM before
			// the probes - its per-cell record->flip transitions legitimately change the term between
			// consecutive frames (the state machine settling), which the temporal probe would read as
			// jitter. A few settle frames let visible cells claim + record + flip; the probes then
			// compare stable serve-vs-serve frames, which is the honest test of the cached term.
			if( cvarSystem->GetCVarInteger( "r_softShadowContribCache" ) != 0 )
			{
				for( int wf = 0; wf < 8; wf++ )
				{
					GateRenderFrame( rw, &rv );
				}
			}
			// ALWAYS retain this render's edge records (the exact caster triangles the shader consumed)
			// via the capture hook - the defect arbiter float64-traces against them. Diagnostic mode
			// additionally writes the full .cap for offline interrogation.
			ResetAccumulators();
			s_armed = true;
			GateRenderFrame( rw, &rv );
			if( r_rtAccelDebug.GetBool() )
			{
				R_CaptureBackendFinish();
			}
			s_armed = false;
			GateReadR32F( globalImages->currentRenderHDRImage, anaA );
			// bit-exactness instrument: an FNV hash of the raw analytic term, printed per light, lets
			// two gate runs under different perf configs (tile binning on/off, wave jump, ...) prove
			// "no pixel changed" across processes - the probes' tolerances can't see sub-threshold
			// differences, a hash can.
			{
				uint32_t swHashSum = 2166136261u;
				for( size_t hi = 0; hi < anaA.t.size(); hi++ )
				{
					uint32_t b;
					memcpy( &b, &anaA.t[hi], 4 );
					swHashSum = ( swHashSum ^ b ) * 16777619u;
				}
				common->Printf( "[softgate] %s L%d anaTerm hash %08x\n", cap.name.c_str(), li, swHashSum );
			}
			// diagnostic mode: ALWAYS dump the analytic term as a PPM (not just on defects) so the
			// LOOK of the term - banding, grain, plateaus - is inspectable offline. The probes only
			// count classified defects; "gate green but visually banded" is exactly the blind spot.
			{
				extern idCVar r_rtAccelDebug;
				if( r_rtAccelDebug.GetBool() )
				{
					std::vector<uint8_t> allValid( ( size_t )anaA.W * anaA.H, 1 );
					idStr ppm = va( "softgate_term_%s_L%d.ppm", cap.name.c_str(), li );
					GateWritePPM( ppm.c_str(), anaA, allValid, std::vector<uint8_t>() );
				}
			}
			GateReadR32F( globalImages->currentDepthImage, depthA );
			GateRenderFrame( rw, &rv );
			GateReadR32F( globalImages->currentRenderHDRImage, anaB );
			if( r_rtAccelDebug.GetBool() )
			{
				R_ReadPixelsRGB8( deviceManager->GetDevice(), &backEnd.GetCommonPasses(), globalImages->currentRenderHDRImage->GetTextureHandle(),
								  nvrhi::ResourceStates::ShaderResource, va( "softgate_dbg_%s_L%d_anaterm.png", cap.name.c_str(), li ) );
				cvarSystem->SetCVarInteger( "r_softShadowDebugShader", 0 );
				GateRenderFrame( rw, &rv );
				R_ReadPixelsRGB8( deviceManager->GetDevice(), &backEnd.GetCommonPasses(), globalImages->currentRenderHDRImage->GetTextureHandle(),
								  nvrhi::ResourceStates::ShaderResource, va( "softgate_dbg_%s_L%d_anaframe.png", cap.name.c_str(), li ) );
				cvarSystem->SetCVarInteger( "r_softShadowDebugShader", 8 );
			}

			// R4 - EXACT term for the cross-texel SEAM probe (surface-fold cache only): the SAME still
			// with the cache forced off. The exact walk is the same field the cache reconstructs, so any
			// neighbour step the cached render has beyond this one is a discontinuity the cache
			// introduced (adjacent texels folding different occluder subsets). The cache buffers persist
			// across the toggle (BeginView only clears on capacity/fingerprint changes), so this render
			// does not perturb the warm-up the later captures depend on.
			swgate::GateImg anaOff;
			const bool swSurfSeam = ( cvarSystem->GetCVarInteger( "r_softShadowSurfCache" ) != 0 );
			if( swSurfSeam )
			{
				cvarSystem->SetCVarInteger( "r_softShadowSurfCache", 0 );
				GateRenderFrame( rw, &rv );
				GateReadR32F( globalImages->currentRenderHDRImage, anaOff );
				cvarSystem->SetCVarInteger( "r_softShadowSurfCache", 1 );
			}

			if( !mask1.Valid() || !rt.Valid() || !anaA.Valid() || !anaB.Valid() || !depthA.Valid()
					|| rt.W != anaA.W || rt.H != anaA.H )
			{
				common->Printf( "[softgate] %s L%d: readback FAILED -> SETUP defect\n", cap.name.c_str(), li );
				GateDefect d;
				d.kind = GATE_SETUP;
				defects.push_back( d );
				all.insert( all.end(), defects.begin(), defects.end() );
				rw->FreeLightDef( lh );
				continue;
			}
			const int W = anaA.W, H = anaA.H;
			if( W < 1920 || H < 1080 )
			{
				common->Printf( "[softgate] FATAL: render %dx%d is below 1920x1080 - defects are invisible at this size. "
								"Launch with +set r_windowWidth 1920 +set r_windowHeight 1080.\n", W, H );
				GateDefect d;
				d.kind = GATE_SETUP;
				all.push_back( d );
				rw->FreeLightDef( lh );
				break;
			}

			std::vector<uint8_t> valid( ( size_t )W * H, 0 );
			long validN = 0;
			for( size_t i = 0; i < valid.size(); i++ )
			{
				if( std::fabs( mask1.t[i] - 1.0f ) <= 1e-3f )
				{
					valid[i] = 1;
					validN++;
				}
			}
			if( validN < 1000 )
			{
				// the matched map light doesn't reach this camera (closed door, tiny scissor) -
				// not a defect on its own; only if NO light draws does the capture flag SETUP below.
				// Print the light's parsed params: a script-driven cinematic light parses with its
				// EDITOR state (_color black / start_off) and draws nothing here despite being lit
				// in the captured cutscene frame.
				common->Printf( "[softgate] %s L%d: light drew only %ld px - skipped (matched org %.0f %.0f %.0f color %.2f %.2f %.2f radius %.0f shader %s)\n",
								cap.name.c_str(), li, validN,
								rl.origin.x, rl.origin.y, rl.origin.z,
								rl.shaderParms[SHADERPARM_RED], rl.shaderParms[SHADERPARM_GREEN], rl.shaderParms[SHADERPARM_BLUE],
								rl.lightRadius[0], rl.shader != NULL ? rl.shader->GetName() : "NULL" );
				rw->FreeLightDef( lh );
				if( pi + 1 >= ( int )probeLights.size() && !capProbed )
				{
					common->Printf( "[softgate] %s: NO candidate light draws at this camera -> SETUP defect\n", cap.name.c_str() );
					GateDefect d;
					d.kind = GATE_SETUP;
					all.push_back( d );
				}
				continue;
			}
			capProbed = true;
			{
				// a starved RT reference (empty TLAS -> fully-lit mask) must never silently pass the gate
				long anaSh = 0, rtSh = 0;
				for( size_t i = 0; i < valid.size(); i++ )
				{
					if( !valid[i] )
					{
						continue;
					}
					if( anaA.t[i] < 0.5f )
					{
						anaSh++;
					}
					if( rt.t[i] < 0.5f )
					{
						rtSh++;
					}
				}
				// a dead reference must never pass OR flood the gate: all-lit (empty TLAS fell back
				// to lit) or all-dark (mask never written, cleared to 0) while the analytic term
				// disagrees wholesale is an oracle failure, not 2000 renderer defects.
				const bool rtAllLit  = ( rtSh * 1000 < validN && anaSh * 20 > validN );
				const bool rtAllDark = ( ( validN - rtSh ) * 1000 < validN && anaSh * 2 < validN );
				if( rtAllLit || rtAllDark )
				{
					common->Printf( "[softgate] %s L%d: RT reference is degenerate (%s: rtShadow=%ld ana=%ld of %ld px) "
									"-> SETUP defect, agreement probes skipped\n", cap.name.c_str(), li,
									rtAllLit ? "all-lit" : "all-dark", rtSh, anaSh, validN );
					GateDefect d;
					d.kind = GATE_SETUP;
					defects.push_back( d );
				}

				// ---- probes -------------------------------------------------------------------
				GateTemporal( anaA, anaB, valid, cfg, defects );
				std::vector<uint8_t> defectPx( ( size_t )W * H, 0 );
				std::vector<uint8_t> crease = GateCreaseMask( depthA, cfg.guard );
				// cross-texel SEAM: cached still vs its own exact field (reference-free self-consistency)
				if( swSurfSeam && anaOff.Valid() && anaOff.W == W && anaOff.H == H )
				{
					GateSeam( anaA, anaOff, valid, &crease, cfg, defects );
				}
				if( !rtAllLit && !rtAllDark )
				{
					// float64 truth arbiter over the retained record triangles (see GateTruthVisibility)
					idMat4 mvpArb, invArb;
					memcpy( mvpArb.ToFloatPtr(), cap.hdr.worldMVP, sizeof( float ) * 16 );
					invArb = mvpArb.Inverse();
					const idVec3 gLightOrg = !s_lights.empty()
											 ? idVec3( s_lights[0].origin[0], s_lights[0].origin[1], s_lights[0].origin[2] )
											 : clOrg;
					const float diskR = cl.penumbraSize;
						// EXACT softpos receiver positions (RGBA32F: xyz world, w=1 valid) - the point the
						// shipped shader actually shaded. The arbiter judges truth HERE, not at a
						// depth-unprojected point: depth reconstruction is grazing-unstable (the very reason
						// softpos exists), so unprojecting reintroduces that error and mints false
						// LIT_IN_UMBRA at thin/grazing pixels where the reconstructed point lands in umbra
						// while the shaded surface point is lit. Falls back to unprojection where softpos is
						// unavailable (never rasterised, or the readback failed).
						std::vector<float> posBuf;
						{
							float* pp = NULL;
							if( globalImages->softShadowPosImage != NULL && globalImages->softShadowPosImage->GetTextureHandle() != NULL
									&& R_ReadPixelsRGBA32F( deviceManager->GetDevice(), &backEnd.GetCommonPasses(),
											globalImages->softShadowPosImage->GetTextureHandle(),
											nvrhi::ResourceStates::ShaderResource, &pp, W, H ) && pp != NULL )
							{
								posBuf.assign( pp, pp + ( size_t )W * H * 4 );
								R_StaticFree( pp );
							}
						}
					std::function<float( int, int )> truthAt = [&]( int x, int y ) -> float
					{
						size_t i = ( size_t )y * W + x;
						float dep = depthA.t[i];
						if( dep <= 1e-6f || dep >= 1.0f - 1e-6f || s_edges.empty() )
						{
							return -1.0f;
						}
						if( !posBuf.empty() && posBuf[i * 4 + 3] != 0.0f )
							{
								return GateTruthVisibility( idVec3( posBuf[i * 4 + 0], posBuf[i * 4 + 1], posBuf[i * 4 + 2] ), gLightOrg, diskR, cap, li );
							}
							float wp[3];
						GateUnproject( invArb.ToFloatPtr(), x, y, W, H, dep, wp );
						return GateTruthVisibility( idVec3( wp[0], wp[1], wp[2] ), gLightOrg, diskR, cap, li );
					};
					GateAgreement( anaA, rt, valid, cfg, defects, &defectPx, &crease, truthAt );
					// HIGH-FREQUENCY GRAIN ("ants"): a frequency test, not a value diff - flag analytic
					// high-pass energy where the denoised (high-ray-equivalent) RT is flat. Catches the
					// spatially-stable scanline speckle the value-based agreement judge is blind to.
					GateGrain( anaA, rt, valid, &crease, cfg, defects );
				}

				// continuity needs the capture matrices to hold for THIS render: unproject->reproject
				// must return to the same pixel (validated on a sample grid before trusting it).
				// The unprojection is the TRUE inverse of the capture worldMVP, computed here - the
				// capture's stored unprojectionToWorldMatrix is transposed relative to worldMVP and
				// reconstructs garbage (measured), so it is deliberately not used.
				idMat4 mvpM, invMvpM;
				memcpy( mvpM.ToFloatPtr(), cap.hdr.worldMVP, sizeof( float ) * 16 );
				invMvpM = mvpM.Inverse();
				// sample the VALID pixels themselves (a fixed grid misses small interaction regions
				// entirely and starved the check into a false SETUP defect)
				int reprojTested = 0, reprojOk = 0;
				const size_t reprojStride = ( validN > 64 ) ? ( size_t )( validN / 64 ) : 1;
				size_t validSeen = 0;
				for( size_t i = 0; i < valid.size() && reprojTested < 64; i++ )
				{
					if( !valid[i] )
					{
						continue;
					}
					if( ( validSeen++ % reprojStride ) != 0 )
					{
						continue;
					}
					{
						int x = ( int )( i % W ), y = ( int )( i / W );
						float dep = depthA.t[i];
						if( dep <= 1e-6f || dep >= 1.0f - 1e-6f )
						{
							continue;
						}
						float wp[3], sx, sy, nz;
						GateUnproject( invMvpM.ToFloatPtr(), x, y, W, H, dep, wp );
						if( !GateProject( cap.hdr.worldMVP, wp, W, H, sx, sy, nz ) )
						{
							continue;
						}
						reprojTested++;
						if( std::fabs( sx - x ) <= 2.0f && std::fabs( sy - y ) <= 2.0f )
						{
							reprojOk++;
						}
						else if( r_rtAccelDebug.GetBool() && reprojTested <= 5 )
						{
							common->Printf( "[softgate]   reproj (%d,%d) d=%.5f -> world (%.1f %.1f %.1f) -> (%.1f,%.1f)\n",
											x, y, depthA.t[i], wp[0], wp[1], wp[2], sx, sy );
						}
					}
				}
				const bool matricesHold = ( reprojTested >= 8 && reprojOk * 10 >= reprojTested * 7 );
				if( !matricesHold )
				{
					common->Printf( "[softgate] %s L%d: capture matrices do not reproject (%d/%d) -> SETUP defect, continuity skipped\n",
									cap.name.c_str(), li, reprojOk, reprojTested );
					GateDefect d;
					d.kind = GATE_SETUP;
					defects.push_back( d );
				}
				else
				{
					// displaced-view MVPs derived algebraically: V = P^-1 * MVP, then the translation
					// column shifts by -R*delta (delta in world). Convention: row-major, clip = M*(P,1).
					idMat4 P, MVP;
					memcpy( P.ToFloatPtr(), cap.hdr.projectionMatrix, sizeof( float ) * 16 );
					memcpy( MVP.ToFloatPtr(), cap.hdr.worldMVP, sizeof( float ) * 16 );
					idMat4 V = P.Inverse() * MVP;
					// ponytail: one horizontal + one vertical slide (not +-both) keeps the run fast and
					// still catches view-dependent pops; add the mirrored pair if flips ever slip through.
					const float DISP = 4.0f;
					const idVec3 deltas[2] = { rv.viewaxis[1] * DISP, rv.viewaxis[2] * DISP };
					for( int dv = 0; dv < 2; dv++ )
					{
						const idVec3& delta = deltas[dv];
						renderView_t drv = rv;
						drv.vieworg = rv.vieworg + delta;
						// coverage mask of the DISPLACED view first (shadows skipped, term==1 where this
						// light draws): the light's interaction region is screen-space and moves with the
						// camera, so without it the region's edge reads as a giant lit->umbra "flip"
						cvarSystem->SetCVarInteger( "r_skipShadows", 1 );
						GateRenderFrame( rw, &drv );
						swgate::GateImg dMask;
						GateReadR32F( globalImages->currentRenderHDRImage, dMask );
						cvarSystem->SetCVarInteger( "r_skipShadows", 0 );
						GateRenderFrame( rw, &drv );
						swgate::GateImg dTerm, dDepth;
						GateReadR32F( globalImages->currentRenderHDRImage, dTerm );
						GateReadR32F( globalImages->currentDepthImage, dDepth );
						if( !dTerm.Valid() || !dDepth.Valid() || !dMask.Valid() )
						{
							continue;
						}
						std::vector<uint8_t> dispValid( ( size_t )W * H, 0 );
						for( size_t vi = 0; vi < dispValid.size(); vi++ )
						{
							if( std::fabs( dMask.t[vi] - 1.0f ) <= 1e-3f )
							{
								dispValid[vi] = 1;
							}
						}
						float rd[3];		// R*delta: rotation part of V applied to the world displacement
						for( int r = 0; r < 3; r++ )
						{
							rd[r] = V[r][0] * delta.x + V[r][1] * delta.y + V[r][2] * delta.z;
						}
						GateView gv;
						memcpy( gv.mvp, cap.hdr.worldMVP, sizeof( gv.mvp ) );
						for( int r = 0; r < 4; r++ )
						{
							gv.mvp[r * 4 + 3] = cap.hdr.worldMVP[r * 4 + 3] - ( P[r][0] * rd[0] + P[r][1] * rd[1] + P[r][2] * rd[2] );
						}
						GateContinuity( anaA, depthA, valid, invMvpM.ToFloatPtr(), dTerm, dDepth, gv, cfg, defects, &crease, &defectPx, &dispValid );
					}
				}

				if( !defects.empty() )
				{
					idStr ppm = va( "softgate_%s_L%d.ppm", cap.name.c_str(), li );
					GateWritePPM( ppm.c_str(), anaA, valid, defectPx );
					// the RT reference next to it: profile ana vs rt through a defect offline
					idStr rtppm = va( "softgate_%s_L%d_rt.ppm", cap.name.c_str(), li );
					GateWritePPM( rtppm.c_str(), rt, valid, defectPx );
				}
			}

			// endgame diagnostics: once a light is down to a handful of defects, print each one
			if( !defects.empty() && defects.size() <= 24 )
			{
				for( const GateDefect& d : defects )
				{
					if( d.kind == GATE_EXTENT && ( d.anaPen + d.truthPen ) > 0 )
					{
						common->Printf( "[softgate]   %s area=%d bbox=(%d,%d)-(%d,%d) anaPen=%d truthPen=%d (%s)\n",
										GateKindName( d.kind ), d.area, d.x0, d.y0, d.x1, d.y1,
										d.anaPen, d.truthPen,
										d.anaPen < d.truthPen ? "UNDER-shadow: band too thin/missing" : "OVER-shadow: band too wide" );
					}
					else
					{
						common->Printf( "[softgate]   %s area=%d bbox=(%d,%d)-(%d,%d)\n",
										GateKindName( d.kind ), d.area, d.x0, d.y0, d.x1, d.y1 );
					}
				}
			}
			int counts[GATE_KIND_COUNT];
			GateTally( defects, counts );
			common->Printf( "[softgate] %-14s L%d (%dx%d, valid %ld px): TURD=%d ANT=%d LIT_IN_UMBRA=%d STEP=%d EXTENT=%d TEMPORAL=%d CONTINUITY=%d SEAM=%d SETUP=%d\n",
							cap.name.c_str(), li, W, H, validN,
							counts[GATE_TURD], counts[GATE_ANT], counts[GATE_LIT_IN_UMBRA], counts[GATE_STEP],
							counts[GATE_EXTENT], counts[GATE_TEMPORAL], counts[GATE_CONTINUITY], counts[GATE_SEAM], counts[GATE_SETUP] );
			all.insert( all.end(), defects.begin(), defects.end() );

			rw->FreeLightDef( lh );
		}

		// ---- BENCH (com_softShadowGateBench N): the FULL shipped frame at this scenario ----------
		// Every parsed map light goes into the world (the frontend culls to the view, exactly like
		// gameplay), full shading (no term debug, ambient on), and N pipelined frames are timed.
		// This is the 60-FPS instrument: same launch, one ms/FPS line per capture.
		const int benchFrames = cvarSystem->GetCVarInteger( "com_softShadowGateBench" );
		if( benchFrames > 0 )
		{
			std::vector<qhandle_t> benchLights;
			benchLights.reserve( mapLights.size() );
			for( const renderLight_t& ml : mapLights )
			{
				benchLights.push_back( rw->AddLightDef( &ml ) );
			}
			// CASTER SCENE. Default (BenchReplay on): reconstruct from the .cap's DEDUPED captured
			// casters - the exact live soft-caster set, DYNAMIC objects included - and exclude the loaded
			// worldspawn from soft-casting (its faces are already in the captured set) so nothing is
			// double-counted. The old path (BenchReplay off) adds the static func_static map guess, which
			// misses every dynamic caster - the dominant shadow load - and over-counts static geometry.
			extern idCVar com_softShadowGateBenchReplay;
			extern idCVar r_softShadowBenchExcludeWorld;
			const bool benchReplay = com_softShadowGateBenchReplay.GetBool();
			std::vector<qhandle_t> benchModels;
			if( benchReplay )
			{
				R_SoftShadowSpawnBenchCasters( full.c_str(), rw );
				cvarSystem->SetCVarInteger( "r_softShadowBenchExcludeWorld", 1 );	// world faces come from the captured set
			}
			else
			{
				cvarSystem->SetCVarInteger( "r_softShadowBenchExcludeWorld", 0 );
				benchModels.reserve( mapModels.size() );
				for( const renderEntity_t& me : mapModels )
				{
					benchModels.push_back( rw->AddEntityDef( &me ) );
				}
			}
			// static interactions for the added bench lights so the surf cache has a caster chain to warm
			// (WarmLight walks light->firstInteraction). GenerateAllInteractions cannot be re-run here -
			// the table was already grown by the added lights and a second full build segfaults - so use
			// the per-light static creator the gate already relies on. Reconstructed DYNAMIC casters stay
			// dynamic (see GateCreateStaticInteractionsForLight), so the cache warms STATIC/world casters
			// only, which is exactly what a static cache can hold. One-time, before the timed frames.
			for( qhandle_t bh : benchLights )
			{
				GateCreateStaticInteractionsForLight( rw, bh );
			}
			R_SoftShadowPinTestConfig( false );
			cvarSystem->SetCVarInteger( "r_skipAmbient", 0 );
			cvarSystem->SetCVarInteger( "r_skipShadows", 0 );
			cvarSystem->SetCVarInteger( "r_useRTShadows", 0 );
			cvarSystem->SetCVarInteger( "r_softShadowDebugShader", 0 );
			// honour the LAUNCH shadow method so configs can be A/B-timed via +set
			cvarSystem->SetCVarInteger( "r_useSoftShadowVolumes", launchSoft );
			cvarSystem->SetCVarInteger( "r_shadowMapPCSS", launchPCSS );
			cvarSystem->SetCVarInteger( "r_useStencilShadows", launchStencil );
			cvarSystem->SetCVarInteger( "r_useShadowMapping", launchMapping );
			cvarSystem->SetCVarInteger( "r_shadowMapPCSSAnalyticContact", launchContact );
			cvarSystem->SetCVarFloat( "r_shadowPenumbraSize", launchPenumbra );
			// RE-FIRE the surf-cache load burst for THIS cap's scene (task #87): TakeLoadWarm keys on
			// world pointer identity and fired (if at all) BEFORE the bench lights above existed, so
			// bench-only runs served ~0% (cap0012: 0 hits of 4M frags) - the burst had warmed a light
			// set that no longer exists. Forgetting the burst world makes the first warm-up frame
			// re-run WarmMapBurst over the bench lights + their just-built static chains; the timed
			// frames start after the warm-ups, so the one heavy burst frame never pollutes the numbers.
			{
				extern idCVar r_softShadowSurfCache;
				if( r_softShadowSurfCache.GetBool() && backEnd.GetSoftShadowSurfCache() != NULL )
				{
					backEnd.GetSoftShadowSurfCache()->ResetLoadWarm();
				}
			}
			for( int wu = 0; wu < 3; wu++ )		// warm-up: caches, atlas, pipelines
			{
				GateRenderFrame( rw, &rv );
			}
			// TABLE CENSUS after the warm-ups, before the timed loop (harness-audit finding #2/#3
			// adjudicator): built vs requested-unbuilt vs empty tells build-truncation apart from a
			// GC wipe apart from seed starvation in ONE line, instead of inferring from hit%%.
			{
				uint32_t tc[4] = {};
				if( backEnd.GetSoftShadowSurfCache() != NULL && backEnd.GetSoftShadowSurfCache()->IsActive()
						&& backEnd.GetSoftShadowSurfCache()->GetTableCensus( tc ) )
				{
					common->Printf( "[softgate] BENCH %-14s surf table-census post-warm: built %u | requested-unbuilt %u | empty %u | walk-always/other %u\n",
									cap.name.c_str(), tc[0], tc[1], tc[2], tc[3] );
				}
			}
			// contributor-cache settle + steady-state snapshot: on a STATIC bench scene every visible
			// cell must claim/record/flip within a few frames, after which recording evals must be ZERO
			// (the whole point of the cache). Snapshot the cumulative counters here; the delta over the
			// timed frames is the steady-state leak detector (recording delta > 0 = a record-path leak
			// paying the 2-3x walk every frame - the exact overhead class that made the live probe
			// net-negative).
			uint32_t csWarm[20] = {};
			uint64_t swCALWarm = 0, swCFRWarm = 0;
			bool csWarmOk = false;
			if( cvarSystem->GetCVarInteger( "r_softShadowContribCache" ) != 0 )
			{
				for( int wf = 0; wf < 12; wf++ )
				{
					GateRenderFrame( rw, &rv );
				}
				csWarmOk = backEnd.GetSoftShadowTermPass() != NULL && backEnd.GetSoftShadowTermPass()->GetContribStats( csWarm );
				if( csWarmOk )
				{
					swCALWarm = backEnd.GetSoftShadowTermPass()->m_ContribActiveLights;
					swCFRWarm = backEnd.GetSoftShadowTermPass()->m_ContribFragments;
				}
			}
			const int t0 = Sys_Microseconds();
			int benchRecords = 0, benchDropped = 0;
			int benchSoftLights = 0, benchTermLights = 0, benchBinnedLights = 0;
			// GRANULAR GPU attribution (accumulated over the timed frames): the soft path split into
			// its phases via the RLS_SOFT_* timer kinds, so the walk (term) is isolated from bin/pos/read.
			double gPos = 0, gBin = 0, gTerm = 0, gRead = 0, gGpu = 0;
			// REST attribution (plan Step 0a): the non-soft per-pass GPU timers, so caps where "rest"
			// dominates (cap0001/0002: soft 11-13 ms but rest ~100 ms) get a named breakdown instead of
			// one opaque number. All fields already exist in backEndCounters_t; this only accumulates.
			double gDepth = 0, gHiZ = 0, gGeom = 0, gSSAO = 0, gAmbient = 0, gAtlas = 0, gInter = 0,
				   gShaderPass = 0, gFog = 0, gPost = 0, gCpuBE = 0;
			int    gN = 0;
			// per-frame samples (index, gpu, wall, soft): the means hide outliers (one pipeline-compile
			// /stall frame of seconds pollutes a 24-frame mean by 100+ ms - cap0000 measured GPU 884 ms
			// mean at wall 18 ms/frame, impossible steady-state). Outliers (> 2x median) are reported
			// PER FRAME with a CLASS, because the fix differs: a PREDICTABLE engine stall (pipeline
			// compile, blocking readback, sync bug) spikes the GPU timers and recurs at the same frame
			// index across runs - ours to fix; an EXTERNAL stall (OS/compositor preemption) inflates
			// wall over a clean GPU timeline at a random index - environment, not a code defect.
			struct swBenchFrame_t
			{
				int    idx;
				double gpu, wall, soft;
			};
			std::vector<swBenchFrame_t> swFrames;
			// fail-fast (com_softShadowGateBenchAbortMs): the cache is fully pre-warmed above (load
			// burst + warm-up frames), so a cap still sustaining over-threshold WALL frames here will
			// not improve - abandon it after 5 consecutive slow frames instead of waiting out the loop.
			const int swAbortMs = cvarSystem->GetCVarInteger( "com_softShadowGateBenchAbortMs" );
			int swSlowStreak = 0;
			bool swAborted = false;
			for( int f = 0; f < benchFrames; f++ )
			{
				const int64 wf0 = Sys_Microseconds();
				// pipelined like the game loop: frontend builds frame f while the GPU draws f-1
				rw->RenderScene( &rv );
				// sample the stream counters BEFORE SwapCommandBuffers resets tr.pc for the next frame
				benchRecords = Max( benchRecords, tr.pc.c_softShadowEdges );
				benchDropped = Max( benchDropped, tr.pc.c_softShadowDroppedEdges );
				const emptyCommand_t* cmd = tr.SwapCommandBuffers( NULL, NULL, NULL, NULL, NULL, NULL );
				tr.RenderCommandBuffers( cmd );
				// valid AND sane: > 5 s "GPU frames" are stale cross-frame begin/end pairings (see the
				// timer-artifact stall class below) - excluding them keeps the means honest.
				if( backEnd.pc.gpuMicroSec > 0 && backEnd.pc.gpuMicroSec < 5000000 )
				{
					// granular soft phases carried on idle shadow timer kinds during the bench
					// (RenderBackend swDiag): shadowmap=softpos, rtmask=tile-bin, stencil=term/walk.
					gPos  += backEnd.pc.gpuShadowMapMicroSec;
					gBin  += backEnd.pc.gpuRTShadowMaskMicroSec;
					gTerm += backEnd.pc.gpuStencilShadowMicroSec;
					gRead += backEnd.pc.gpuSoftShadowMicroSec;	// interaction term-atlas READ
					gGpu  += backEnd.pc.gpuMicroSec;
					// non-soft passes (REST attribution)
					gDepth      += backEnd.pc.gpuDepthMicroSec;
					gHiZ        += backEnd.pc.gpuHiZMicroSec;
					gGeom       += backEnd.pc.gpuGeometryMicroSec;
					gSSAO       += backEnd.pc.gpuScreenSpaceAmbientOcclusionMicroSec;
					gAmbient    += backEnd.pc.gpuAmbientPassMicroSec;
					gAtlas      += backEnd.pc.gpuShadowAtlasPassMicroSec;
					gInter      += backEnd.pc.gpuInteractionsMicroSec;
					gShaderPass += backEnd.pc.gpuShaderPassMicroSec + backEnd.pc.gpuShaderPassPostMicroSec;
					gFog        += backEnd.pc.gpuFogAllLightsMicroSec;
					gPost       += backEnd.pc.gpuBloomMicroSec + backEnd.pc.gpuMotionVectorsMicroSec
								   + backEnd.pc.gpuTemporalAntiAliasingMicroSec + backEnd.pc.gpuToneMapPassMicroSec
								   + backEnd.pc.gpuPostProcessingMicroSec + backEnd.pc.gpuDrawGuiMicroSec
								   + backEnd.pc.gpuCrtPostProcessingMicroSec;
					gCpuBE      += backEnd.pc.cpuTotalMicroSec;
					gN++;
				}
				swBenchFrame_t bf;
				bf.idx  = f;
				bf.gpu  = backEnd.pc.gpuMicroSec / 1000.0;		// 0 when the timer query was invalid this frame
				bf.wall = ( Sys_Microseconds() - wf0 ) / 1000.0;
				bf.soft = ( backEnd.pc.gpuShadowMapMicroSec + backEnd.pc.gpuRTShadowMaskMicroSec
							+ backEnd.pc.gpuStencilShadowMicroSec + backEnd.pc.gpuSoftShadowMicroSec ) / 1000.0;
				swFrames.push_back( bf );
				// PATH PROVENANCE, sampled from the backend counters of the frame just rendered:
				// which evaluation path each soft light's term actually took. term < total or
				// binned < total is not an error (slot budget, ineligible lights) but it must be
				// VISIBLE - a silent bit-exact fallback is a perf leak the gate cannot see (the
				// tile-rect Y-flip bug hid exactly this way).
				benchSoftLights   = Max( benchSoftLights,   backEnd.pc.c_softLightsTotal );
				benchTermLights   = Max( benchTermLights,   backEnd.pc.c_softLightsTerm );
				benchBinnedLights = Max( benchBinnedLights, backEnd.pc.c_softLightsBinned );
				if( swAbortMs > 0 )
				{
					swSlowStreak = ( bf.wall > swAbortMs ) ? swSlowStreak + 1 : 0;
					if( swSlowStreak >= 5 )
					{
						swAborted = true;
						common->Printf( "[softgate] BENCH %s ABORT: %d frames > %dms sustained (cache pre-warmed - not improving)\n",
										cap.name.c_str(), swSlowStreak, swAbortMs );
						break;
					}
				}
			}
			tr.SwapCommandBuffers( NULL, NULL, NULL, NULL, NULL, NULL );	// drain the last frame
			// frames ACTUALLY run: the abort break shortens the loop; every mean below must divide by
			// this, never the requested benchFrames.
			const int framesRun = Max( 1, ( int )swFrames.size() );
			const double ms = ( Sys_Microseconds() - t0 ) / 1000.0 / framesRun;
			// stream accounting from the LAST bench frame: dropped>0 means the frame BUDGET silently
			// erased whole lights' soft shadows - such a bench time is a lie (faster because shadows
			// are missing), so the drop count must be printed next to the ms it taints.
			common->Printf( "[softgate] BENCH %-14s %6.2f ms/frame (%4.0f FPS) over %d frames%s, %d map lights, "
							"%d soft records (%d dropped), %d soft lights (%d term, %d binned)\n",
							cap.name.c_str(), ms, 1000.0 / ms, framesRun, swAborted ? " (ABORTED)" : "", ( int )mapLights.size(),
							benchRecords, benchDropped, benchSoftLights, benchTermLights, benchBinnedLights );
			// granular GPU phase attribution (us->ms), averaged over frames with a valid timer query.
			// soft = pos + bin + term + read; the rest of gpu is everything non-soft.
			if( gN > 0 )
			{
				const double iv = 1.0 / ( 1000.0 * gN );
				const double soft = ( gPos + gBin + gTerm + gRead ) * iv;
				common->Printf( "[softgate] BENCH %-14s GPU %.2f ms | soft %.2f (softpos %.2f + tilebin %.2f + TERM/walk %.2f + read %.2f) | rest %.2f ms\n",
								cap.name.c_str(), gGpu * iv, soft, gPos * iv, gBin * iv, gTerm * iv, gRead * iv, gGpu * iv - soft );
				// REST breakdown (plan Step 0a): name the non-soft GPU ms. interactions here EXCLUDES the
				// soft phases (they ride the split-out shadow timer kinds above). unattr = GPU total minus
				// everything named - a large unattr means a pass without a timer kind, itself a finding.
				const double named = ( gDepth + gHiZ + gGeom + gSSAO + gAmbient + gAtlas + gInter + gShaderPass + gFog + gPost ) * iv + soft;
				common->Printf( "[softgate] BENCH %-14s rest: depth %.2f + hiz %.2f + geom %.2f + ssao %.2f + ambient %.2f + shadowatlas %.2f + interactions %.2f + shaderpass %.2f + fog %.2f + post %.2f | unattr %.2f | cpuBE %.2f ms\n",
								cap.name.c_str(), gDepth * iv, gHiZ * iv, gGeom * iv, gSSAO * iv, gAmbient * iv, gAtlas * iv,
								gInter * iv, gShaderPass * iv, gFog * iv, gPost * iv, gGpu * iv - named, gCpuBE * iv );
				// per-frame steady-state + STALL DEFECTS. Medians are the honest per-frame cost; every
				// stalled frame (> 2x median, > +1 ms) is a DEFECT with its own class and attack, never
				// background noise to average away:
				//   gpu-soft   - GPU spike, mostly inside the soft phase timers -> our soft-path stall
				//   gpu-engine - GPU spike outside soft (pipeline compile between passes, sync bug) -> ours
				//   cpu/extern - wall spike over a CLEAN GPU timeline -> CPU-side or OS/compositor
				// PREDICTABLE stalls recur at the SAME idx across runs (re-run to confirm); external ones move.
				{
					std::vector<double> gs, ws;
					for( const swBenchFrame_t& bf : swFrames )
					{
						if( bf.gpu > 0 )
						{
							gs.push_back( bf.gpu );
						}
						ws.push_back( bf.wall );
					}
					std::sort( gs.begin(), gs.end() );
					std::sort( ws.begin(), ws.end() );
					const double gMed = gs.empty() ? 0.0 : gs[ gs.size() / 2 ];
					const double wMed = ws.empty() ? 0.0 : ws[ ws.size() / 2 ];
					common->Printf( "[softgate] BENCH %-14s steady-state: GPU median %.2f (mean %.2f) | WALL median %.2f (mean %.2f) ms\n",
									cap.name.c_str(), gMed, gGpu * iv, wMed, ms );
					for( const swBenchFrame_t& bf : swFrames )
					{
						const bool gpuOut  = ( bf.gpu > 0 ) && ( bf.gpu > 2.0 * gMed ) && ( bf.gpu > gMed + 1.0 );
						const bool wallOut = ( bf.wall > 2.0 * wMed ) && ( bf.wall > wMed + 1.0 );
						if( !gpuOut && !wallOut )
						{
							continue;
						}
						// physically impossible sample (GPU seconds at sub-100ms wall): the GPU_TIME
						// begin timestamp is STALE from before the bench (load/warm-up frames never
						// fetched, so the parity slot holds an old begin) paired with a fresh end. A
						// KNOWN instrument defect (root-fix pending: proper begin/end pairing), already
						// excluded from every statistic - NOT printed; the line only obscured real stalls.
						if( bf.gpu > 5000.0 && bf.wall < 100.0 )
						{
							continue;
						}
						const char* cls = !gpuOut ? "cpu/extern (GPU clean)"
										  : ( ( bf.soft > 0.5 * ( bf.gpu - gMed ) ) ? "gpu-soft (soft-phase stall)" : "gpu-engine (outside soft: compile/sync)" );
						common->Printf( "[softgate] BENCH %-14s STALL frame %2d: gpu %.1f wall %.1f soft %.1f ms (+%.1f over median) class=%s\n",
										cap.name.c_str(), bf.idx, bf.gpu, bf.wall, bf.soft,
										( gpuOut ? bf.gpu - gMed : bf.wall - wMed ), cls );
					}
				}
			}
			// contributor-cache steady-state (delta over the TIMED frames, post-settle): on a static
			// scene steady recording MUST be 0 - any recording here is a per-frame leak re-paying the
			// 2-3x record walk. PASS/FAIL is the unit-level invariant for the overhead-reduction loop.
			if( csWarmOk )
			{
				uint32_t csEnd[20] = {};
				if( backEnd.GetSoftShadowTermPass() != NULL && backEnd.GetSoftShadowTermPass()->GetContribStats( csEnd ) )
				{
					const uint32_t dServe  = csEnd[1] - csWarm[1];
					const uint32_t dRecord = csEnd[2] - csWarm[2];
					const uint32_t dClaim  = csEnd[3] - csWarm[3];
					const uint32_t dRefine = csEnd[10] - csWarm[10];
					// refinement records ARE steady-state by design (1/64 validator); the leak verdict is
					// on recording BEYOND them
					common->Printf( "[softgate] BENCH %-14s CONTRIB steady-state: serves %u/frame | recording %u/frame (refine %u/frame) | claims %u/frame -> %s\n",
									cap.name.c_str(), dServe / framesRun, dRecord / framesRun, dRefine / framesRun, dClaim / framesRun,
									dRecord <= dRefine + 64 * framesRun ? "PASS (refinement only)" : "FAIL (steady-state RECORD LEAK)" );
					common->Printf( "[softgate] BENCH %-14s CONTRIB cumulative (incl. warm): serves %u | recordings %u | cells claimed %u\n",
									cap.name.c_str(), csEnd[1], csEnd[2], csEnd[3] );
					const uint64_t swStT = backEnd.GetSoftShadowTermPass()->m_ContribStaticTris;
					const uint64_t swTtT = backEnd.GetSoftShadowTermPass()->m_ContribTotalTris;
					common->Printf( "[softgate] BENCH %-14s CONTRIB diag deltas: budget-blocked %u/frame | probe-exhausted %u/frame | built-unservable %u/frame | STATIC share %.1f%% | active lights %.1f/frame | frags %.0fk/frame\n",
									cap.name.c_str(), ( csEnd[7] - csWarm[7] ) / framesRun, ( csEnd[8] - csWarm[8] ) / framesRun,
									( csEnd[9] - csWarm[9] ) / framesRun,
									swTtT > 0 ? 100.0 * ( double )swStT / ( double )swTtT : 0.0,
									( double )( backEnd.GetSoftShadowTermPass()->m_ContribActiveLights - swCALWarm ) / framesRun,
									( double )( backEnd.GetSoftShadowTermPass()->m_ContribFragments - swCFRWarm ) / framesRun / 1000.0 );
					// serve-verify (r_softShadowContribCache 2): the decisive correctness number for the
					// fast loop - served-vs-plain term mismatches over the timed frames
					if( csEnd[4] - csWarm[4] > 0 )
					{
						float swVMaxD;
						memcpy( &swVMaxD, &csEnd[6], sizeof( float ) );
						const uint32_t dCmp = csEnd[4] - csWarm[4];
						const uint32_t dMis = csEnd[5] - csWarm[5];
						common->Printf( "[softgate] BENCH %-14s CONTRIB VERIFY: compares %u | mismatches %u (%.5f%%) | max|diff| %.6f\n",
										cap.name.c_str(), dCmp, dMis, 100.0 * dMis / dCmp, swVMaxD );
						common->Printf( "[softgate] BENCH %-14s CONTRIB VERIFY split: serve-DARKER %u | serve-LIGHTER %u | on-spill %u | on-tile %u\n",
										cap.name.c_str(), csEnd[16] - csWarm[16], csEnd[17] - csWarm[17],
										csEnd[18] - csWarm[18], csEnd[19] - csWarm[19] );
					}
					// serve-work attribution: what an average SERVE actually walks. Compare vs the plain
					// walk's ~55-107 MT survivors/fragment - if union+dyn FillTris approach that, the
					// serve is not saving work and the term ms will show it.
					if( dServe > 0 )
					{
						common->Printf( "[softgate] BENCH %-14s CONTRIB serve-work: union entries %.1f | union FillTri %.1f | dyn FillTri %.1f | umbra-out %.1f%% (per serve)\n",
										cap.name.c_str(),
										( double )( csEnd[12] - csWarm[12] ) / dServe,
										( double )( csEnd[13] - csWarm[13] ) / dServe,
										( double )( csEnd[14] - csWarm[14] ) / dServe,
										100.0 * ( double )( csEnd[15] - csWarm[15] ) / dServe );
					}
				}
			}
			// surface-fold cache path split from the LAST bench frame (r_softShadowSurfCache):
			// hit% is THE cache-health number - a low rate explains a high TERM ms instantly
			// (misses pay the full walk) instead of leaving it to conjecture.
			{
				uint32_t ss[4] = {};
				if( backEnd.GetSoftShadowSurfCache() != NULL && backEnd.GetSoftShadowSurfCache()->IsActive()
						&& backEnd.GetSoftShadowSurfCache()->GetStats( ss ) )
				{
					const double tot = ( double )ss[0] + ss[1] + ss[2] + ss[3];
					if( tot > 0.0 )
					{
						int swCur = 0, swCap = 0, swPend = 0;
						backEnd.GetSoftShadowSurfCache()->GetPrewarmState( swCur, swCap, swPend );
						common->Printf( "[softgate] BENCH %-14s surf: hit %u (%.1f%%) miss %u walkalways %u anchor-rej %u | prewarm cursor %d/%d pending %d\n",
										cap.name.c_str(), ss[0], 100.0 * ss[0] / tot, ss[1], ss[2], ss[3], swCur, swCap, swPend );
						// MISS SPLIT (task #87 built-vs-read attribution): empty/overflow = warm/key
						// defect (the built-but-not-read class), requested = build-budget lag, stale-gen
						// = invalidation churn. Separates "never seeded", "seeded not built", "built
						// then invalidated every frame", and "built under a key the read never derives".
						uint32_t mr[4] = {};
						if( ss[1] > 0 && backEnd.GetSoftShadowSurfCache()->GetMissReasons( mr ) )
						{
							common->Printf( "[softgate] BENCH %-14s surf miss-split: stale-gen %u | requested-unbuilt %u | empty-slot %u | probe-overflow %u\n",
											cap.name.c_str(), mr[0], mr[1], mr[2], mr[3] );
						}
						// PER-LIGHT hit/miss rings (lightKey&15): names WHICH lights own the miss bucket.
						// An all-or-nothing light (never seeded/streamed) reads hit 0 / miss large; a
						// healthy light reads a high hit share. Ring collisions possible past 16 lights -
						// read as attribution hints, not exact per-light truth.
						uint32_t pl[32] = {};
						if( ss[1] > 0 && backEnd.GetSoftShadowSurfCache()->GetPerLightStats( pl ) )
						{
							idStr swPlLine;
							for( int r = 0; r < 16; r++ )
							{
								const uint32_t h = pl[r * 2], m = pl[r * 2 + 1];
								if( h + m == 0 )
								{
									continue;
								}
								swPlLine += va( " %d:%u/%u(%.0f%%)", r, h, m, 100.0 * h / ( double )( h + m ) );
							}
							common->Printf( "[softgate] BENCH %-14s surf by-light ring:hit/miss(hit%%):%s\n",
											cap.name.c_str(), swPlLine.c_str() );
							// TERM-LIGHT IDENTITY: name the rings. index(ring, cache on/off at CB fill,
							// warm?) - a ring whose misses dwarf its hits while its named light reads
							// OFF/COLD is an unwarmed light hiding in a ring collision, not a keying gap.
							{
								extern int R_SoftSurfTermLightsTake( int* outIdx, bool* outOn, int cap );
								int tlIdx[64]; bool tlOn[64];
								const int tlN = R_SoftSurfTermLightsTake( tlIdx, tlOn, 64 );
								// PORTAL-AREA audit (r_softShadowAreaCull confirmation): each light's
								// reached-area list from its lightDef->references chain - the exact set
								// the term CB's area mask is built from. n=0 = empty reference list
								// (mask stays all-ones, cull inert for that light).
								idRenderWorldLocal* swTlWorld = ( idRenderWorldLocal* )tr.primaryWorld;
								idStr swTlLine;
								for( int tl = 0; tl < tlN; tl++ )
								{
									idStr swTlAreas;
									int swTlAreaN = 0;
									if( swTlWorld != NULL && tlIdx[tl] >= 0 && tlIdx[tl] < swTlWorld->lightDefs.Num()
											&& swTlWorld->lightDefs[tlIdx[tl]] != NULL )
									{
										for( const areaReference_t* ref = swTlWorld->lightDefs[tlIdx[tl]]->references;
												ref != NULL; ref = ref->ownerNext )
										{
											swTlAreas += va( "%s%d", swTlAreaN ? "," : "", ref->area ? ref->area->areaNum : -1 );
											swTlAreaN++;
										}
									}
									swTlLine += va( " %d(r%d,%s,%s,areas:%d=[%s])", tlIdx[tl], tlIdx[tl] & 15,
													tlOn[tl] ? "on" : "OFF",
													backEnd.GetSoftShadowSurfCache()->IsWarmLight( tlIdx[tl] ) ? "warm" : "COLD",
													swTlAreaN, swTlAreas.c_str() );
								}
								common->Printf( "[softgate] BENCH %-14s surf term lights idx(ring,cache,warm,areas):%s | viewArea %d of %d\n",
												cap.name.c_str(), swTlLine.c_str(),
												tr.viewDef ? tr.viewDef->areaNum : -999,
												swTlWorld ? swTlWorld->NumAreas() : -1 );
							}
							// VIZ FRAME DUMP: with a surf viz mode active, dump the last bench frame -
							// the term tint (viz 2 dims serves, viz 4 brightens hits / tints classes)
							// shows WHICH SURFACES serve: the spatial truth the counters cannot give.
							// (The repro path replays captured records without the cache, so its dumps
							// are structurally blind to serving - this is the bench-side counterpart.)
							extern idCVar r_softShadowSurfCacheViz;
							if( r_softShadowSurfCacheViz.GetInteger() != 0 )
							{
								// Dump the TERM ATLAS, not the lit HDR frame: the lit frame multiplies
								// the viz term by the (near-zero on dark caps) lighting and the heatmap
								// vanishes. The atlas is the raw per-light term, scissor-shelf packed -
								// every light's slot lands in the one image, which is what we want.
								// R16F single-channel -> the PNG carries the heatmap in the RED channel.
								nvrhi::ITexture* vizTex = ( backEnd.GetSoftShadowTermPass() != NULL
															&& backEnd.GetSoftShadowTermPass()->GetTermTexture() != nullptr )
														  ? backEnd.GetSoftShadowTermPass()->GetTermTexture()
														  : ( nvrhi::ITexture* )globalImages->currentRenderHDRImage->GetTextureHandle();
								R_ReadPixelsRGB8( deviceManager->GetDevice(), &backEnd.GetCommonPasses(),
												  vizTex,
												  nvrhi::ResourceStates::ShaderResource,
												  va( "dumps/benchviz_%s.png", cap.name.c_str() ) );
								common->Printf( "[softgate] BENCH %-14s viz frame dumped to dumps/benchviz_%s.png\n",
												cap.name.c_str(), cap.name.c_str() );
								if( r_softShadowSurfCacheViz.GetInteger() == 9 )
								{
									// legend for the walk-cost heatmap (mirrors SwVizCostTerm in softterm.cs.hlsl)
									common->Printf( "[softgate] BENCH %-14s viz9 legend: 0=never-written 0.04=early-out 0.08=umbra-tile 0.12=cache-hit 0.16=other-zero-fill/empty-walk, log2 cost 0.25..1.0, max 4096 fills\n",
													cap.name.c_str() );
								}
							}
						}
					}
				}
			}
			// spill accounting from the LAST bench frame: silent spill-region exhaustion sends the
			// starved tiles back to the O(all-casters) full walk - a perf leak the correctness gate
			// can never see, so the demand must be printed next to the region size it must fit in.
			uint32_t spillStats[4] = {};
			if( backEnd.GetSoftTileBinPass() != NULL && backEnd.GetSoftTileBinPass()->GetSpillStats( spillStats ) && spillStats[1] > 0 )
			{
				common->Printf( "[softgate] BENCH %-14s spill: demand %u of %d region (%s), %u overflow tiles, worst tile %u tris\n",
								cap.name.c_str(), spillStats[0], SoftTileBinPass::SPILL_ELEMENTS,
								spillStats[0] > ( uint32_t )SoftTileBinPass::SPILL_ELEMENTS ? "EXHAUSTED" : "fits",
								spillStats[1], spillStats[2] );
			}
			// ---- ONE-PASS ATTRIBUTION -------------------------------------------------------------
			// The walk counters used to print only when the user remembered r_softShadowWalkCounters,
			// costing a second multi-minute launch. Instead: AFTER the timed loop (ms already computed)
			// and AFTER the last-bench-frame pass-stat reads above (stream/surf/spill/contrib), force
			// the cvar on and render 2 extra UNTIMED frames through the COUNTING permutation, restore,
			// then print. The timed numbers are untouched; if the cvar was already on, the timed frames
			// were counting frames and we read them directly (today's behaviour, the override).
			{
				extern idCVar r_softShadowWalkCounters;
				if( !r_softShadowWalkCounters.GetBool() )
				{
					r_softShadowWalkCounters.SetBool( true );
					for( int cf = 0; cf < 2; cf++ )
					{
						GateRenderFrame( rw, &rv );
					}
					r_softShadowWalkCounters.SetBool( false );
					// GetWalkStats stays valid: m_WalkCntEnabled snapshots the cvar per VIEW, and the
					// last rendered view (counting frame 2) snapshotted it ON.
				}
			}
			// GPU per-fragment walk counters: the shipped (scanline) walk's real cull cascade per
			// walked fragment. The counting permutation is SW_SCANLINE=1 = the shipped algorithm; the
			// surf/contrib CACHE pipelines are structurally bypassed in counting mode, so every number
			// below attributes the PLAIN walk (tile-list, spill/cluster, unbinned-full paths - all
			// instrumented). Slots: [0/1] caster+cluster sphere tests/culls, [2/3] coarse v0, [4/5]
			// tight cone, [6] survivors reaching FillTri, [7] fragments walked.
			uint32_t walk[28] = {};
			if( backEnd.GetSoftShadowTermPass() != NULL && backEnd.GetSoftShadowTermPass()->GetWalkStats( walk ) && walk[7] > 0 )
			{
				// SCANLINE FillTri attribution (task #106, slots 20-26): where the chord-sweep walk spends
				// its per-triangle work, per walked fragment. fills = FillTri entered (post cone cull);
				// rejects = clip/disk/chord early-outs; skips = already-covered (with the skip-test + fold
				// loop iteration counts = the skip path's per-chord ALU); sweeps = tris paying the per-edge
				// setup + chord rows (rows = SoftScan_Run + envelope updates).
				{
					const double f = ( double )walk[7];
					common->Printf( "[softgate] BENCH %-14s walk-scan/frag: fills %.1f -> rejects %.1f | skips %.1f (test-iters %.1f, fold-iters %.1f) | sweep-tris %.1f, sweep-rows %.1f\n",
									cap.name.c_str(), walk[20] / f, walk[21] / f, walk[22] / f, walk[23] / f, walk[24] / f, walk[25] / f, walk[26] / f );
				}
				// LIT-EARLY-OUT execution proof (slot 27): fragments the intensity cut skipped at T > 0.
				// Nonzero = the r_softShadowLitEarlyOut path actually ran under this config (a 0-defect
				// sweep with this at 0 would be vacuous).
				if( walk[27] > 0 )
				{
					common->Printf( "[softgate] BENCH %-14s lit-early-out: %u frags skipped (T > 0 path EXECUTED)\n",
									cap.name.c_str(), walk[27] );
				}
				const double f = ( double )walk[7];			// fragments that ran the walk (this frame)
				// Cull cascade, all plain-walk paths (tile-list fills tight/mtTri; spill/cluster fills
				// caster+coarse+tight+mtTri; unbinned-full fills all). "FillTri survivors" replaces the
				// old "x16 sample-tests" phrasing - the counting permutation is scanline, there are no
				// per-sample ray tests to multiply by.
				common->Printf( "[softgate] BENCH %-14s walk/frag: cone-tests %.0f (cull %.0f%%) -> FillTri survivors %.1f | sphere-tests %.0f (cull %.0f%%), coarse-tests %.0f (cull %.0f%%) | %u frags\n",
								cap.name.c_str(), walk[4] / f,
								100.0 * walk[5] / ( double )( walk[4] ? walk[4] : 1 ),
								walk[6] / f,
								walk[0] / f, 100.0 * walk[1] / ( double )( walk[0] ? walk[0] : 1 ),
								walk[2] / f, 100.0 * walk[3] / ( double )( walk[2] ? walk[2] : 1 ),
								walk[7] );
				// Walk WORK split by FINAL COVERAGE (scanline: lit = coverage 0, umbra = >=0.99 - the
				// walkers' rounding band - else penumbra; umbra-sentinel tiles bucket as umbra with 0
				// survivors). The old mask-based flush filed every scanline fragment as lit (100/0/0).
				// survivors = triangles surviving the cone cull into FillTri (the per-fragment cost).
				const double litF = walk[8],  litS = walk[9];
				const double penF = walk[10], penS = walk[11];
				const double umbF = walk[12], umbS = walk[13];
				const double totS = litS + penS + umbS;
				const double tf   = litF + penF + umbF;
				common->Printf( "[softgate] BENCH %-14s walk-buckets(final-coverage): frags lit/pen/umb %.0f/%.0f/%.0f%% | survivor-WORK lit/pen/umb %.0f/%.0f/%.0f%% (%.0f/%.0f/%.0f surv per-frag) | bucketed %.0f of %u frags\n",
								cap.name.c_str(),
								tf > 0 ? 100.0 * litF / tf : 0.0, tf > 0 ? 100.0 * penF / tf : 0.0, tf > 0 ? 100.0 * umbF / tf : 0.0,
								totS > 0 ? 100.0 * litS / totS : 0.0, totS > 0 ? 100.0 * penS / totS : 0.0, totS > 0 ? 100.0 * umbS / totS : 0.0,
								litF > 0 ? litS / litF : 0.0, penF > 0 ? penS / penF : 0.0, umbF > 0 ? umbS / umbF : 0.0,
								tf, walk[7] );
				// Survivor outcome. Slots 14/15 (MT hit / block-nothing) belong to the SAMPLED walk only -
				// the scanline path never runs the per-sample MT loop, so when they are zero the truthful
				// split is FillTri's own outcome classes (reject = survived the cone cull but provably adds
				// no coverage; skip = its span is already covered; sweep = pays the chord walk).
				const double hitS = walk[14], missS = walk[15], mtotS = hitS + missS;
				if( mtotS > 0 )
				{
					common->Printf( "[softgate] BENCH %-14s survivors@MT: %.0f%% HIT (contribute) / %.0f%% block-NOTHING | %.0f hit + %.0f miss per frag (sampled-walk path)\n",
									cap.name.c_str(), 100.0 * hitS / mtotS, 100.0 * missS / mtotS,
									hitS / f, missS / f );
				}
				else if( walk[20] > 0 )
				{
					const double fl = ( double )walk[20];
					common->Printf( "[softgate] BENCH %-14s survivors@FillTri: %.0f%% sweep (pay chord walk) / %.0f%% reject (add nothing) / %.0f%% skip (already covered) of %.1f fills/frag\n",
									cap.name.c_str(), 100.0 * walk[25] / fl, 100.0 * walk[21] / fl, 100.0 * walk[22] / fl, fl / f );
				}
				// TILE-CLASS census (Lever B decision data): of the threads reaching the tile dispatch,
				// how many land in FLAT tiles (umbra sentinel / empty list = zero walk iterations - the only
				// ones a penumbra-tile compaction could delete) vs LISTED/SPILL tiles that actually walk.
				const double cUmb = walk[16], cEmpty = walk[17], cSpill = walk[18], cList = walk[19];
				const double cTot = cUmb + cEmpty + cSpill + cList;
				if( cTot > 0 )
				{
					// empty-list at 1 decimal: count-0 tiles are REAL (a scissor tile whose survivors all
					// culled at tile grain with no umbra proof - the bin writes count 0, the walker runs 0
					// iterations) but rare, and the integer print rounded a genuine sub-0.5% share to a
					// suspicious-looking flat 0%.
					common->Printf( "[softgate] BENCH %-14s tile-census: FLAT %.0f%% (umbra-tile %.0f%% + empty-list %.1f%%) | WALKING %.0f%% (listed %.0f%% + spill %.0f%%) of %.0f tile-dispatch threads\n",
									cap.name.c_str(), 100.0 * ( cUmb + cEmpty ) / cTot, 100.0 * cUmb / cTot, 100.0 * cEmpty / cTot,
									100.0 * ( cList + cSpill ) / cTot, 100.0 * cList / cTot, 100.0 * cSpill / cTot, cTot );
				}
			}
			// ---- FULL-FRAME QUALITY A/B (com_softShadowGateFullFrame N) --------------------------------
			// The harness for what the per-light frozen-view probes are STRUCTURALLY blind to: (a) light
			// CONTENTION - one probed light never exhausts term slots / bin budgets / spill regions, so
			// every fallback a real multi-light frame triggers is invisible; (b) MOVING-CAMERA cache
			// staleness - unions served along a motion path are perpetually a few frames behind the
			// view. Both were playtest-visible (ants + stairstepping) at gate 0-defects (2026-08-24).
			// Every pose renders the complete shipped frame twice - contributor cache OFF then ON - at
			// the SAME pose; the ON cache state carries across poses exactly as in play. The lit HDR
			// R-channel diff is classified (area levels, isolated-pixel ants, horizontal-run steps) and
			// the ON frame's light-path provenance is tracked; the worst pose is re-rendered and dumped
			// as an OFF/ON PNG pair for the eyeball.
			extern idCVar com_softShadowGateFullFrame;
			const int ffPoses = com_softShadowGateFullFrame.GetInteger();
			if( ffPoses > 0 )
			{
				const idVec3   ffBaseOrg = rv.vieworg;
				const idAngles ffBaseAng = rv.viewaxis.ToAngles();
				const int ffPrevContrib = cvarSystem->GetCVarInteger( "r_softShadowContribCache" );
				swgate::GateImg ffA, ffB;
				std::vector<unsigned char> ffMask;
				uint64_t ffLo = 0, ffHi = 0, ffAnts = 0, ffSteps = 0, ffCmp = 0;
				uint64_t ffWorstHi = 0;
				int      ffWorstPose = -1;
				double   ffMaxRel = 0.0;
				int      ffTermMin = INT_MAX, ffTotMax = 0, ffFallbackMax = 0;
				for( int ffp = 0; ffp < ffPoses; ffp++ )
				{
					R_SoftShadowBenchMotionPose( ffBaseOrg, ffBaseAng, ffp, ffPoses, &rv );
					cvarSystem->SetCVarInteger( "r_softShadowContribCache", 0 );
					GateRenderFrame( rw, &rv );
					if( !GateReadR32F( globalImages->currentRenderHDRImage, ffA ) )
					{
						continue;
					}
					cvarSystem->SetCVarInteger( "r_softShadowContribCache", 1 );
					// inline render (not GateRenderFrame): the light-path provenance counters must be
					// sampled BETWEEN RenderCommandBuffers and the draining swap, which resets them
					{
						rw->RenderScene( &rv );
						const emptyCommand_t* ffCmd = tr.SwapCommandBuffers( NULL, NULL, NULL, NULL, NULL, NULL );
						tr.RenderCommandBuffers( ffCmd );
						ffTermMin     = Min( ffTermMin, backEnd.pc.c_softLightsTerm );
						ffTotMax      = Max( ffTotMax, backEnd.pc.c_softLightsTotal );
						ffFallbackMax = Max( ffFallbackMax, backEnd.pc.c_softLightsTotal - backEnd.pc.c_softLightsTerm );
						tr.SwapCommandBuffers( NULL, NULL, NULL, NULL, NULL, NULL );
					}
					if( !GateReadR32F( globalImages->currentRenderHDRImage, ffB ) )
					{
						continue;
					}
					if( ffA.W != ffB.W || ffA.H != ffB.H || ffA.t.empty() )
					{
						continue;
					}
					const int ffW = ffA.W, ffH = ffA.H;
					ffMask.assign( ( size_t )ffW * ffH, 0 );
					uint64_t lo = 0, hi = 0;
					for( size_t i = 0; i < ffA.t.size(); i++ )
					{
						const float a = ffA.t[i], b = ffB.t[i];
						// RELATIVE diff in linear HDR (the +0.02 floor keeps black areas from
						// exploding the ratio); 5% = visible shading shift, 25% = gross error
						const float dr = fabsf( a - b ) / ( fabsf( a ) + 0.02f );
						ffMaxRel = Max( ffMaxRel, ( double )dr );
						if( dr > 0.25f )
						{
							ffMask[i] = 2;
							hi++;
							lo++;
						}
						else if( dr > 0.05f )
						{
							ffMask[i] = 1;
							lo++;
						}
					}
					// ants: gross-error pixels with at most one deviating 4-neighbour (isolated speckle)
					// steps: pixels inside horizontal deviation runs >= 8 px (the stairstep signature)
					for( int y = 0; y < ffH; y++ )
					{
						int run = 0;
						for( int x = 0; x < ffW; x++ )
						{
							const size_t i = ( size_t )y * ffW + x;
							if( ffMask[i] != 0 )
							{
								run++;
							}
							else
							{
								if( run >= 8 )
								{
									ffSteps += run;
								}
								run = 0;
							}
							if( ffMask[i] == 2 )
							{
								int nb = 0;
								nb += ( x > 0 && ffMask[i - 1] != 0 ) ? 1 : 0;
								nb += ( x + 1 < ffW && ffMask[i + 1] != 0 ) ? 1 : 0;
								nb += ( y > 0 && ffMask[i - ffW] != 0 ) ? 1 : 0;
								nb += ( y + 1 < ffH && ffMask[i + ffW] != 0 ) ? 1 : 0;
								if( nb <= 1 )
								{
									ffAnts++;
								}
							}
						}
						if( run >= 8 )
						{
							ffSteps += run;
						}
					}
					ffLo += lo;
					ffHi += hi;
					ffCmp += ffA.t.size();
					if( hi >= ffWorstHi )
					{
						ffWorstHi = hi;
						ffWorstPose = ffp;
					}
				}
				// worst pose: re-render both states and dump the PNG pair for the eyeball
				if( ffWorstPose >= 0 )
				{
					R_SoftShadowBenchMotionPose( ffBaseOrg, ffBaseAng, ffWorstPose, ffPoses, &rv );
					cvarSystem->SetCVarInteger( "r_softShadowContribCache", 0 );
					GateRenderFrame( rw, &rv );
					R_ReadPixelsRGB8( deviceManager->GetDevice(), &backEnd.GetCommonPasses(), globalImages->currentRenderHDRImage->GetTextureHandle(),
									  nvrhi::ResourceStates::ShaderResource, va( "dumps/fullframe_%s_off.png", cap.name.c_str() ) );
					cvarSystem->SetCVarInteger( "r_softShadowContribCache", 1 );
					GateRenderFrame( rw, &rv );
					R_ReadPixelsRGB8( deviceManager->GetDevice(), &backEnd.GetCommonPasses(), globalImages->currentRenderHDRImage->GetTextureHandle(),
									  nvrhi::ResourceStates::ShaderResource, va( "dumps/fullframe_%s_on.png", cap.name.c_str() ) );
				}
				common->Printf( "[softgate] FULLFRAME %-10s %d poses: diff>5%% %llu (%.4f%% of %llu px) | gross>25%% %llu | ANTS %llu | STEP-px %llu | maxRel %.3f | worst pose %d (PNG pair in dumps/)\n",
								cap.name.c_str(), ffPoses,
								( unsigned long long )ffLo, ffCmp ? 100.0 * ffLo / ffCmp : 0.0, ( unsigned long long )ffCmp,
								( unsigned long long )ffHi, ( unsigned long long )ffAnts, ( unsigned long long )ffSteps,
								ffMaxRel, ffWorstPose );
				common->Printf( "[softgate] FULLFRAME %-10s provenance (ON frames): lights term-path min %d of %d total | worst fallback count %d%s\n",
								cap.name.c_str(), ffTermMin == INT_MAX ? 0 : ffTermMin, ffTotMax, ffFallbackMax,
								ffFallbackMax > 0 ? "  <-- lights OFF the cached/binned path (contention: sampled-quality risk pre-scanline-port, uncached perf)" : "" );
				cvarSystem->SetCVarInteger( "r_softShadowContribCache", ffPrevContrib );
				// restore the base pose for any later block
				rv.vieworg  = ffBaseOrg;
				rv.viewaxis = ffBaseAng.ToMat3();
			}

			// ---- MOTION A/B (com_softShadowGateBenchMotion N): the surf-cache proof --------------------
			// Run N frames of camera motion (shake/rotate/move/combo segments) TWICE - cache OFF then ON -
			// recording each frame's GPU ms. The cache warms one light per frame during the ON pass, so
			// its FIRST frames spike (the warm builds) then flatten; OFF is a flat uncached baseline.
			// Reports steady-state gain + hitch counts so a motion-triggered rebuild (the failure we must
			// NOT see) shows up as ON hitches past the warm ramp. One-shot summary only - never per-frame.
			extern idCVar com_softShadowGateBenchMotion;
			const int motionFrames = com_softShadowGateBenchMotion.GetInteger();
			if( motionFrames > 0 )
			{
				const idVec3   baseOrg = rv.vieworg;
				const idAngles baseAng = rv.viewaxis.ToAngles();
				const int prevSurf = cvarSystem->GetCVarInteger( "r_softShadowSurfCache" );
				// CACHE SCENE: the surf cache holds STATIC (world) casters only, so the A/B needs the world
				// casting and the uncacheable dynamics gone. The default replay scene EXCLUDES the world
				// (the one thing the cache can hold) -> 0% hits; the all-536-models scene OOMs the frame
				// arena under motion. Reset to WORLD-ONLY static casters here: drop the replay/captured
				// soup, let the world cast, and (re)create the per-light static world interactions the
				// warm walk reads. This is the scene the cache is actually for.
				R_SoftShadowClearBenchCasters();
				R_SoftShadowClearCapturedCasters();
				cvarSystem->SetCVarInteger( "r_softShadowBenchExcludeWorld", 0 );
				for( qhandle_t bh : benchLights )
				{
					GateCreateStaticInteractionsForLight( rw, bh );
				}
				// pin the bench lights into the warm queue so the cache resolves them against THIS world
				// (the game feeds the queue from its interaction hooks; the gate has no game thread).
				if( backEnd.GetSoftShadowSurfCache() != NULL )
				{
					idRenderWorldLocal* rwl = static_cast<idRenderWorldLocal*>( rw );
					for( qhandle_t bh : benchLights )
					{
						if( bh >= 0 && bh < rwl->lightDefs.Num() && rwl->lightDefs[bh] != NULL )
						{
							backEnd.GetSoftShadowSurfCache()->EnqueueWarm( rwl->lightDefs[bh] );
						}
					}
				}
				// force the warm-at-load burst to re-fire against THIS (world-only) scene: the fixed-view
				// bench already load-warmed the earlier caster scene, so the ON pass frame 0 triggers the
				// real backend TakeLoadWarm -> WarmMapBurst path (one heavy frame, then flat + warm).
				if( backEnd.GetSoftShadowSurfCache() != NULL )
				{
					backEnd.GetSoftShadowSurfCache()->ResetLoadWarm();
				}
				double abSteady[2] = { 0, 0 }, abMax[2] = { 0, 0 };
				int    abHitch[2] = { 0, 0 };
				for( int pass = 0; pass < 2; pass++ )
				{
					const bool cacheOn = ( pass == 1 );
					cvarSystem->SetCVarInteger( "r_softShadowSurfCache", cacheOn ? 1 : 0 );
					std::vector<double> ms;
					ms.reserve( motionFrames );
					double segSum[4] = { 0, 0, 0, 0 };
					int    segN[4]   = { 0, 0, 0, 0 };
					for( int f = 0; f < motionFrames; f++ )
					{
						R_SoftShadowBenchMotionPose( baseOrg, baseAng, f, motionFrames, &rv );
						rw->RenderScene( &rv );
						const emptyCommand_t* cmd = tr.SwapCommandBuffers( NULL, NULL, NULL, NULL, NULL, NULL );
						tr.RenderCommandBuffers( cmd );
						if( backEnd.pc.gpuMicroSec > 0 )		// GPU timer valid this frame
						{
							const double fms = backEnd.pc.gpuMicroSec / 1000.0;
							ms.push_back( fms );
							const int seg = Min( 3, ( f * 4 ) / motionFrames );
							segSum[seg] += fms;
							segN[seg]++;
						}
					}
					tr.SwapCommandBuffers( NULL, NULL, NULL, NULL, NULL, NULL );	// drain last
					if( ms.empty() )
					{
						continue;
					}
					std::vector<double> srt = ms;
					std::sort( srt.begin(), srt.end() );
					const double med  = srt[srt.size() / 2];
					const double p99  = srt[Min( srt.size() - 1, srt.size() * 99 / 100 )];
					const double mx   = srt.back();
					double sum = 0;
					for( double v : ms ) { sum += v; }
					const double mean = sum / ms.size();
					int hitch = 0;
					for( double v : ms ) { if( v > 1.5 * med ) { hitch++; } }
					// steady state = last half of the run (past the one-light-per-frame warm ramp)
					double ssum = 0;
					int    sn = 0;
					for( size_t i = ms.size() / 2; i < ms.size(); i++ ) { ssum += ms[i]; sn++; }
					const double steady = sn ? ssum / sn : mean;
					abSteady[pass] = steady;
					abMax[pass]    = mx;
					abHitch[pass]  = hitch;
					uint32_t hitPct = 0;
					uint32_t ss[4] = {};
					if( cacheOn && backEnd.GetSoftShadowSurfCache() != NULL
							&& backEnd.GetSoftShadowSurfCache()->GetStats( ss ) )
					{
						const double tot = ( double )ss[0] + ss[1] + ss[2] + ss[3];
						hitPct = tot > 0.0 ? ( uint32_t )( 100.0 * ss[0] / tot ) : 0;
					}
					common->Printf( "[softgate] MOTION %-12s cache %s: mean %.2f steady %.2f p99 %.2f max %.2f ms | hitch>1.5x %d/%d | seg shake %.2f rot %.2f move %.2f combo %.2f | hit %u%% [h=%u m=%u wa=%u ar=%u]\n",
									cap.name.c_str(), cacheOn ? "ON " : "OFF",
									mean, steady, p99, mx, hitch, ( int )ms.size(),
									segN[0] ? segSum[0] / segN[0] : 0.0, segN[1] ? segSum[1] / segN[1] : 0.0,
									segN[2] ? segSum[2] / segN[2] : 0.0, segN[3] ? segSum[3] / segN[3] : 0.0, hitPct, ss[0], ss[1], ss[2], ss[3] );
				}
				common->Printf( "[softgate] MOTION %-12s A/B: steady OFF %.2f -> ON %.2f ms (%.0f%% %s) | max OFF %.2f ON %.2f | hitches OFF %d ON %d\n",
								cap.name.c_str(), abSteady[0], abSteady[1],
								abSteady[0] > 0.0 ? 100.0 * ( abSteady[0] - abSteady[1] ) / abSteady[0] : 0.0,
								abSteady[1] <= abSteady[0] ? "faster" : "SLOWER",
								abMax[0], abMax[1], abHitch[0], abHitch[1] );
				cvarSystem->SetCVarInteger( "r_softShadowSurfCache", prevSurf );
			}

			for( qhandle_t bh : benchLights )
			{
				rw->FreeLightDef( bh );
			}
			for( qhandle_t bh : benchModels )
			{
				rw->FreeEntityDef( bh );
			}
			R_SoftShadowClearBenchCasters();								// reconstructed captured casters
			cvarSystem->SetCVarInteger( "r_softShadowBenchExcludeWorld", 0 );
		}

		R_SoftShadowClearCapturedCasters();

		// PER-CAP ISOLATION (measurement-integrity fix): free the world after EVERY capture, even on
		// the same map. Reusing the world across caps poisoned every cap after the first: the freed +
		// re-added bench lights get handles past the interactionTable built at load (a second
		// GenerateAllInteractions segfaults, see above), so those lights fall to PER-FRAME dynamic
		// interaction creation - measured cap0001 in-corpus GPU 136 ms / cpuBE 193 ms vs 9.6 / 0.3 ms
		// solo, a fabricated 14x "rest" that sat unattributed. The reload costs seconds per cap and
		// buys numbers that cannot cross-contaminate; the gate is an instrument, correctness first.
		if( rw != NULL )
		{
			R_SoftShadowClearBenchCasters();
			renderSystem->FreeRenderWorld( rw );
			rw = NULL;
			loadedMap.Clear();
		}
	}

	if( rw != NULL )
	{
		R_SoftShadowClearBenchCasters();
		R_SoftShadowClearCapturedCasters();
		renderSystem->FreeRenderWorld( rw );
	}

	for( int i = 0; i < nTouched; i++ )
	{
		cvarSystem->SetCVarString( touched[i], prev[i].c_str() );
	}

	int counts[GATE_KIND_COUNT];
	GateTally( all, counts );
	common->Printf( "[softgate] ==============================================================\n" );
	common->Printf( "[softgate] TOTAL: TURD=%d ANT=%d LIT_IN_UMBRA=%d STEP=%d EXTENT=%d TEMPORAL=%d CONTINUITY=%d SEAM=%d SETUP=%d\n",
					counts[GATE_TURD], counts[GATE_ANT], counts[GATE_LIT_IN_UMBRA], counts[GATE_STEP],
					counts[GATE_EXTENT], counts[GATE_TEMPORAL], counts[GATE_CONTINUITY], counts[GATE_SEAM], counts[GATE_SETUP] );
	// NOTHING-TESTED IS A FAILURE, NOT A PASS. A run that probed 0 captures or 0 lights has verified
	// nothing - reporting "0 defects -> PASS" is a false green that hides a missing corpus, a bad path,
	// or an init abort (observed 2026-08-23: a stale gamescope stole the X socket, the gate loaded 0
	// captures and still exited 0). Fail loudly with a SETUP-class exit code so CI/scripts see it.
	if( capsRun == 0 || lightsRun == 0 )
	{
		common->Printf( "[softgate] TOTAL DEFECTS: %d across %d captures, %d lights -> FAIL (nothing tested - "
						"missing corpus or aborted init; a run that probes 0 lights verifies nothing)\n",
						( int )all.size(), capsRun, lightsRun );
		return 124;		// distinct SETUP-fail code, below the 125 defect clamp and the 126 watchdog abort
	}
	// contributor-cache health/verify report (r_softShadowContribCache): serve/record volumes, and in
	// serve-verify mode (=2) the decisive number - served-vs-plain term mismatches with the max |diff|
	{
		uint32_t cs[20] = {};
		if( backEnd.GetSoftShadowTermPass() != NULL && backEnd.GetSoftShadowTermPass()->GetContribStats( cs ) && ( cs[1] | cs[2] | cs[3] ) != 0 )
		{
			float maxDiff;
			memcpy( &maxDiff, &cs[6], sizeof( float ) );
			common->Printf( "[softgate] CONTRIB: serves %u | recording evals %u | cells claimed %u | pool now %u | VERIFY compares %u mismatches %u (%.4f%%) max|diff| %.6f\n",
							cs[1], cs[2], cs[3], cs[0], cs[4], cs[5],
							cs[4] ? 100.0 * cs[5] / cs[4] : 0.0, cs[4] ? maxDiff : 0.0f );
			common->Printf( "[softgate] CONTRIB diag: budget-blocked %u | probe-exhausted %u | built-unservable %u | refine records %u\n",
							cs[7], cs[8], cs[9], cs[10] );
		}
	}
	common->Printf( "[softgate] TOTAL DEFECTS: %d across %d captures, %d lights -> %s\n",
					( int )all.size(), capsRun, lightsRun, all.empty() ? "PASS" : "FAIL" );
	return ( int )all.size();
}

void R_TestSoftShadowLocator_f( const idCmdArgs& args )
{
	if( tr.primaryWorld == NULL )
	{
		common->Warning( "testSoftShadowLocator: no map loaded" );
		return;
	}
	if( args.Argc() < 2 )
	{
		common->Warning( "usage: testSoftShadowLocator <capture.cap> | <x y z yaw pitch>  (load its map first - all erebusN are game/erebus1)" );
		return;
	}

	// Camera = the capture's EXACT artifact viewpoint (or a free-cam "x y z yaw pitch", see softShadowGoto). We move
	// the (noclipping) player there via the goto tick and render via the real Draw() path: Draw() applies shadow-cvar
	// changes between configs, whereas a bare RenderScene reuses the interactions cached at map load and renders every
	// config identically. The file/args only supply the camera for the log line; the actual pose is set by the goto.
	idVec3 camOrg;
	idAngles ang;
	if( args.Argc() >= 6 )
	{
		camOrg.Set( atof( args.Argv( 1 ) ), atof( args.Argv( 2 ) ), atof( args.Argv( 3 ) ) );
		ang.Set( atof( args.Argv( 5 ) ), atof( args.Argv( 4 ) ), 0.0f );
		common->Printf( "[softtest] free-cam (%.0f %.0f %.0f) yaw %.0f pitch %.0f\n", camOrg.x, camOrg.y, camOrg.z, ang.yaw, ang.pitch );
	}
	else
	{
		FILE* cf = fopen( args.Argv( 1 ), "rb" );
		capHeader_t hdr;
		if( cf == NULL || fread( &hdr, sizeof( hdr ), 1, cf ) != 1 || hdr.magic != CAP_MAGIC )
		{
			if( cf != NULL ) { fclose( cf ); }
			common->Warning( "testSoftShadowLocator: cannot read capture %s", args.Argv( 1 ) );
			return;
		}
		fclose( cf );
		camOrg.Set( hdr.vieworg[0], hdr.vieworg[1], hdr.vieworg[2] );
		ang = idVec3( hdr.viewaxis[0], hdr.viewaxis[1], hdr.viewaxis[2] ).ToAngles();
		common->Printf( "[softtest] capture camera (%.0f %.0f %.0f) yaw %.0f pitch %.0f from %s\n",
						camOrg.x, camOrg.y, camOrg.z, ang.yaw, ang.pitch, args.Argv( 1 ) );
		// reproduce the captured DYNAMIC casters (absent from the live loadGame-quick world) so their shadows render
		R_SoftShadowSpawnCapturedCasters( args.Argv( 1 ) );
	}

	// pin the full config again right before the A/B renders (in case the test is run WITHOUT softShadowGoto, or a
	// frame in between re-archived something). Verbose so the exact config under test is in the log every time.
	R_SoftShadowPinTestConfig( true );

	static const char* const touched[] =
	{
		"g_stopTime", "r_useStencilShadows", "r_useSoftShadowVolumes", "r_useRTShadows",
		"r_shadowMapPCSS", "r_softShadowDebugShader", "r_useTemporalAA", "r_skipShadows", "r_useShadowAtlas",
	};
	const int nTouched = ( int )( sizeof( touched ) / sizeof( touched[0] ) );
	idStrList prev;
	for( int i = 0; i < nTouched; i++ ) { prev.Append( cvarSystem->GetCVarString( touched[i] ) ); }
	cvarSystem->SetCVarInteger( "r_useTemporalAA", 0 );
	cvarSystem->SetCVarInteger( "r_softShadowDebugShader", 0 );

	// move the player to the capture viewpoint (issue `noclip` yourself once before sweeping many captures, so
	// the camera can sit inside geometry). setviewpos only moves the PLAYER ENTITY - a bare Draw() then re-renders
	// the STALE view built by the last game tick (every capture came out identical: the spawn view). Run several
	// FULL game frames (sim unfrozen) so the player teleports and the game rebuilds the render view AT the capture
	// position; only THEN freeze the sim and do the A/B renders.
	// the VIEWPOINT + cinematic-skip must already be applied by `softShadowGoto <cap>` + a `wait` ahead of this
	// command (it runs through the normal frame loop; doing it here re-entrantly unloads the map). We only render
	// the current view. One Draw settles the latest view, then freeze the sim for the deterministic A/B pair.
	R_RenderOneFrame();

	// ORACLE: ray-traced shadows - peter-pan IMMUNE (traces the actual scene geometry, no shadow-map projection),
	// rendered through the real Draw()->game->Draw()->RenderScene frontend that rebuilds per cvar, exactly like
	// toggling r_useRTShadows in-game. RT casts the LIVE world's own casters at their TRUE contact point, so the
	// PCSS gap against it IS the peter-panning. Converged (many rays, denoise off) for a stable reference; soft
	// radius matched to the analytic light-disk (r_shadowPenumbraSize) so RT and PCSS penumbra amplitudes agree.
	// (Stencil is a dead no-op in this build - proven identical to PCSS - so RT is the only working immune oracle.)
	cmdSystem->BufferCommandText( CMD_EXEC_NOW, "r_skipShadows 0 ; r_useStencilShadows 0 ; r_useSoftShadowVolumes 0 ; r_useShadowAtlas 0 ; r_shadowMapPCSS 0 ; r_useRTShadows 1 ; r_rtShadowDenoise 0 ; r_rtShadowRays 512 ; r_rtShadowAnalyticPenumbra 0\n" );	// RT oracle (immune)
	cvarSystem->SetCVarFloat( "r_rtShadowSoftRadius", cvarSystem->GetCVarFloat( "r_shadowPenumbraSize" ) );
	R_RenderOneFrame();		// NOTE: RT will NOT engage from here (accel structure is map-load time); the deltaN==0 guard below catches it
	std::vector<uint8_t> ref;
	int rw = 0, rh = 0;
	ReadImageRGBA8( globalImages->currentRenderHDRImage, ref, rw, rh );
	R_ReadPixelsRGB8( deviceManager->GetDevice(), &backEnd.GetCommonPasses(),
					  globalImages->currentRenderHDRImage->GetTextureHandle(), nvrhi::ResourceStates::ShaderResource, "softtest_oracle.png" );

	// HYBRID: soft-shadow volumes + PCSS locator gating the analytic
	cmdSystem->BufferCommandText( CMD_EXEC_NOW, "r_skipShadows 0 ; r_useStencilShadows 0 ; r_useRTShadows 0 ; r_useShadowAtlas 1 ; r_useSoftShadowVolumes 1 ; r_shadowMapPCSS 1\n" );
	R_RenderOneFrame();
	std::vector<uint8_t> test;
	int tw = 0, th = 0;
	ReadImageRGBA8( globalImages->currentRenderHDRImage, test, tw, th );
	R_ReadPixelsRGB8( deviceManager->GetDevice(), &backEnd.GetCommonPasses(),
					  globalImages->currentRenderHDRImage->GetTextureHandle(), nvrhi::ResourceStates::ShaderResource, "softtest_hybrid.png" );

	cvarSystem->SetCVarInteger( "g_stopTime", 1 );	// freeze only now, for the (config-invariant) dbg dumps

	// CONFIG + why soft edges did/didn't generate (the frontend tallies caster acceptance/rejection)
	{
		extern int fe_stencilBuilt, fe_softEdgesCollected, fe_rejSilEdges, fe_rejSurfInter, fe_rejNumIdx, fe_rejIdxStale, fe_rejShadowCache;
		int softEdges = 0;
		idRenderWorldLocal* rwl2 = ( idRenderWorldLocal* )tr.primaryWorld;
		for( viewLight_t* vl = tr.viewDef ? tr.viewDef->viewLights : NULL; vl != NULL; vl = vl->next ) { softEdges += vl->softEdgeCount; }
		( void )rwl2;
		common->Printf( "[softtest] cfg soft=%d pcss=%d atlas=%d RT=%d stencil=%d shadowMapping=%d band=%d AAM=%d penumbra=%.1f\n",
						cvarSystem->GetCVarInteger( "r_useSoftShadowVolumes" ), cvarSystem->GetCVarInteger( "r_shadowMapPCSS" ),
						cvarSystem->GetCVarInteger( "r_useShadowAtlas" ), cvarSystem->GetCVarInteger( "r_useRTShadows" ),
						cvarSystem->GetCVarInteger( "r_useStencilShadows" ), cvarSystem->GetCVarInteger( "r_useShadowMapping" ),
						cvarSystem->GetCVarInteger( "r_softShadowBandMask" ), cvarSystem->GetCVarInteger( "r_softShadowAAM" ),
						cvarSystem->GetCVarFloat( "r_shadowPenumbraSize" ) );
		common->Printf( "[softtest] frontend softEdges(view)=%d  softEdgesCollected(cumulative)=%d  stencilBuilt=%d rejSil=%d rejNumIdx=%d\n",
						softEdges, fe_softEdgesCollected, fe_stencilBuilt, fe_rejSilEdges, fe_rejNumIdx );
	}

	// dump the locator's own inputs/outputs (soft+PCSS config): mode 10 = classification (red=umbra/false
	// shadow, green=lit), 11 = receiver depth, 12 = shadow-atlas depth the blocker search samples. These reveal
	// whether the atlas holds real occluders or volume/garbage, and whether the projection is sane.
	{
		struct DbgDump { int mode; const char* file; };
		static const DbgDump dumps[] = { { 10, "softtest_dbg10_locator.png" }, { 11, "softtest_dbg11_recvdepth.png" }, { 12, "softtest_dbg12_atlasdepth.png" }, { 13, "softtest_dbg13_uv.png" }, { 14, "softtest_dbg14_w.png" }, { 15, "softtest_dbg15_face.png" } };
		for( int di = 0; di < 6; di++ )
		{
			cvarSystem->SetCVarInteger( "r_softShadowDebugShader", dumps[di].mode );
			R_RenderOneFrame();
			R_ReadPixelsRGB8( deviceManager->GetDevice(), &backEnd.GetCommonPasses(),
							  globalImages->currentRenderHDRImage->GetTextureHandle(), nvrhi::ResourceStates::ShaderResource, dumps[di].file );
			// numeric probe: mean R (and for locator, red-fraction) over the lit central band, so recvZ (11) vs
			// atlas sample (12) can be compared directly - a big gap is the depth-encoding (plumbing) mismatch.
			std::vector<uint8_t> pb; int pw = 0, ph = 0;
			ReadImageRGBA8( globalImages->currentRenderHDRImage, pb, pw, ph );
			if( pw > 0 && ph > 0 )
			{
				double sumR = 0, sumG = 0; long n = 0;
				for( int y = ph / 4; y < 3 * ph / 4; y++ )
					for( int x = pw / 4; x < 3 * pw / 4; x++ )
					{
						sumR += pb[( ( size_t )y * pw + x ) * 4 + 0]; sumG += pb[( ( size_t )y * pw + x ) * 4 + 1]; n++;
					}
				common->Printf( "[softtest] dbg%d central meanR=%.3f meanG=%.3f (of 255)\n", dumps[di].mode, n ? sumR / n : 0.0, n ? sumG / n : 0.0 );
			}
		}
		cvarSystem->SetCVarInteger( "r_softShadowDebugShader", 0 );
	}

	// dump the soft light's ATLAS TILE (what the blocker search actually samples): near depth = dark. Real
	// occluders show as small dark silhouettes on a far/white field; volume geometry fills the tile dark.
	{
		extern int fe_occludersBuilt;
		common->Printf( "[softtest] occludersBuilt(cumulative)=%d  softDbgAtlasSize=(%d,%d) off0=(%d,%d)\n",
						fe_occludersBuilt, g_softDbgAtlasSize.x, g_softDbgAtlasSize.y, g_softDbgAtlasOff[0].x, g_softDbgAtlasOff[0].y );
		const int AS = cvarSystem->GetCVarInteger( "r_shadowMapAtlasSize" );
		float* atlas = NULL;
		const bool readOK = AS > 0 && R_ReadPixelsR32F( deviceManager->GetDevice(), &backEnd.GetCommonPasses(),
						globalImages->shadowAtlasImage->GetTextureHandle(), nvrhi::ResourceStates::ShaderResource, &atlas, AS, AS ) && atlas != NULL;
		if( !readOK )
		{
			common->Printf( "[softtest] atlas readback FAILED (AS=%d)\n", AS );
		}
		if( readOK )
		{
			extern idVec2i g_softDbgAtlasOff[6];
			extern idVec2i g_softDbgAtlasSize;
			const int sz = g_softDbgAtlasSize.x;
			for( int face = 0; face < 6; face++ )
			{
				const int ox = g_softDbgAtlasOff[face].x, oy = g_softDbgAtlasOff[face].y;
				if( ox < 0 || oy < 0 || sz <= 0 || ox + sz > AS || oy + sz > AS ) { continue; }
				idStr name = va( "softtest_atlastile_f%d.pgm", face );
				idFile* f = fileSystem->OpenFileWrite( name, "fs_savepath" );
				if( f != NULL )
				{
					f->Printf( "P5\n%d %d\n255\n", sz, sz );
					double sum = 0;
					int nnear = 0;
					for( int y = 0; y < sz; y++ )
						for( int x = 0; x < sz; x++ )
						{
							float d = atlas[( size_t )( oy + y ) * AS + ( ox + x )];
							sum += d;
							if( d < 0.99f ) { nnear++; }
							unsigned char c = ( unsigned char )( Max( 0.0f, Min( 1.0f, d ) ) * 255.0f );
							f->Write( &c, 1 );
						}
					fileSystem->CloseFile( f );
					common->Printf( "[softtest] atlas tile f%d (%d,%d,%d): meanDepth=%.4f  nearPx(<0.99)=%d/%d\n", face, ox, oy, sz, sum / ( sz * sz ), nnear, sz * sz );
				}
			}
			R_StaticFree( atlas );
		}
	}

	tr.SwapCommandBuffers( NULL, NULL, NULL, NULL, NULL, NULL );	// present the last frame
	for( int i = 0; i < nTouched; i++ ) { cvarSystem->SetCVarString( touched[i], prev[i].c_str() ); }

	if( ref.empty() || test.empty() || rw != tw || rh != th || rw <= 0 )
	{
		common->Warning( "testSoftShadowLocator: readback failed or size mismatch (ref %dx%d, test %dx%d)", rw, rh, tw, th );
		return;
	}

	// PETER-PANNING METRIC. Oracle = RT (immune: shadow at TRUE contact). Hybrid = PCSS. Peter-panning displaces
	// the PCSS shadow AWAY from the object, so it shows as a signed disagreement, measured two ways:
	//   MISSED  = oracle-shadowed but hybrid-lit  -> the contact region PCSS wrongly leaves lit (the detach gap)
	//   FALSE   = hybrid-shadowed but oracle-lit  -> where the displaced PCSS shadow landed instead
	//   SHIFT   = |centroid(hybrid shadow) - centroid(oracle shadow)| in px -> the raw peter-pan displacement,
	//             with its direction, independent of shadow area (the single number the user asked for).
	// A pixel is a shadow-relevant SURFACE if either config shows it lit (excludes background); within that set a
	// pixel is "shadowed" in a config when its luminance is < darkenFrac of the brighter-of-the-two (the local
	// unshadowed estimate), so plain texture darkness that both configs share is never counted.
	const float litThresh = 0.12f;
	const float darkenFrac = 0.65f;
	long litN = 0, missedN = 0, falseN = 0, bothN = 0, deltaN = 0;
	double oSx = 0, oSy = 0, hSx = 0, hSy = 0;
	long oShad = 0, hShad = 0;
	std::vector<uint8_t> diff( ( size_t )rw * rh * 3, 0 );
	for( int i = 0; i < rw * rh; i++ )
	{
		float lr = SoftTestLum( &ref[( size_t )i * 4] );
		float ls = SoftTestLum( &test[( size_t )i * 4] );
		diff[( size_t )i * 3 + 0] = ( uint8_t )( lr * 255.0f );
		diff[( size_t )i * 3 + 1] = ( uint8_t )( ls * 255.0f );
		if( idMath::Fabs( lr - ls ) > 0.1f ) { deltaN++; }
		float bright = Max( lr, ls );
		if( bright < litThresh ) { continue; }			// background / unlit surface
		litN++;
		bool shadO = lr < darkenFrac * bright;			// RT says shadowed here
		bool shadH = ls < darkenFrac * bright;			// PCSS says shadowed here
		const int x = i % rw, y = i / rw;
		if( shadO ) { oShad++; oSx += x; oSy += y; }
		if( shadH ) { hShad++; hSx += x; hSy += y; }
		if( shadO && !shadH ) { missedN++; diff[( size_t )i * 3 + 2] = 255; }			// blue = detach gap
		else if( shadH && !shadO ) { falseN++; diff[( size_t )i * 3 + 0] = 255; }		// red-boost = misplaced shadow
		else if( shadO && shadH ) { bothN++; }
	}
	const double shiftX = ( oShad && hShad ) ? ( hSx / hShad - oSx / oShad ) : 0.0;
	const double shiftY = ( oShad && hShad ) ? ( hSy / hShad - oSy / oShad ) : 0.0;
	const double shift = sqrt( shiftX * shiftX + shiftY * shiftY );
	const long unionShad = missedN + falseN + bothN;
	const double iou = unionShad ? ( double )bothN / unionShad : 1.0;		// shadow overlap; peter-pan drives it down
	const double missedRate = oShad ? ( double )missedN / oShad : 0.0;
	// peter-pan verdict: RT and PCSS shadows should sit on top of each other. A big centroid shift or a large
	// fraction of the RT shadow that PCSS misses at contact = peter-panning.
	const bool pass = ( litN > 1000 ) && ( shift < 2.0 ) && ( missedRate < 0.15 );

	// diff image: R=oracle lum (red-boosted where PCSS over-shadows), G=hybrid lum, B=detach-gap mask
	idFile* d = fileSystem->OpenFileWrite( "softtest_falseshadow.ppm", "fs_savepath" );
	if( d != NULL )
	{
		d->Printf( "P6\n%d %d\n255\n", rw, rh );
		d->Write( diff.data(), ( int )diff.size() );
		fileSystem->CloseFile( d );
	}

	if( litN <= 1000 )
	{
		common->Warning( "testSoftShadowLocator: only %ld lit px - no usable oracle (ray-query hardware? map loaded?)", litN );
	}
	if( deltaN == 0 )
	{
		// RT builds its acceleration structure at MAP LOAD, so enabling r_useRTShadows here (mid-session, from a
		// console command) is a no-op and the RT oracle renders identically to the PCSS hybrid - a false PASS.
		// The objective metric must be run as TWO launches with the config set on the command line:
		common->Warning( "testSoftShadowLocator: oracle == hybrid (RT did NOT engage mid-run). Use the autonomous "
						 "two-launch harness for a valid peter-pan measurement: neo/tools/softshadow/run_peterpan.sh <capture.cap>" );
	}
	common->Printf( "[softtest] %s : PETER-PAN shift=%.2f px (dx=%.2f dy=%.2f)  missed=%ld/%ld RTshadow (%.1f%%)  false=%ld  IoU=%.3f  shadowDelta=%ld  ->  %s\n",
					args.Argv( 1 ), shift, shiftX, shiftY, missedN, oShad, missedRate * 100.0, falseN, iou, deltaN, pass ? "PASS" : "FAIL" );

	R_SoftShadowClearCapturedCasters();		// remove the replayed captured casters from the render world
}
