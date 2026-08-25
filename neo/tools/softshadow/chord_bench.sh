#!/usr/bin/env bash
# Perf sweep: Fubini chord count (r_softShadowScanChords 4/8/16/32) vs term/walk ms.
# Isolates the chord cost - caches OFF, scanline ON, one heavy cap, bench-only (no defect probes).
# Headless (gamescope). Prints the [softgate] BENCH TERM/walk line per chord count.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SAVE="${RBDOOM_SAVEPATH:-$HOME/.local/share/rbdoom3bfg/base}"
CAP="${CAP:-cap0009}"                       # heaviest cap by default
SCRATCH="${SCRATCH:-/tmp/claude-1000/-home-app-Games-gog-doom-3-bfg-edition/23e3d413-fae3-4e41-bf39-04e051ab67d9/scratchpad}"
CDIR="$SCRATCH/chordcap"                     # single-cap corpus dir
rm -rf "$CDIR"; mkdir -p "$CDIR"; ln -sf "$SAVE/cap/$CAP.cap" "$CDIR/$CAP.cap"
FRAMES="${FRAMES:-24}"
for N in 4 8 16 32; do
	LOG="$SCRATCH/chord_${N}.log"
	RBDOOM_W=1920 RBDOOM_H=1080 "$HERE/headless.sh" \
		+set com_softShadowGate "$CDIR" \
		+set com_softShadowGateBenchOnly 1 +set com_softShadowGateBench "$FRAMES" \
		+set com_softShadowGateBenchReplay 1 \
		+set r_softShadowFaceCoverage 1 +set r_softShadowScanline 1 \
		+set r_softShadowContribCache 0 +set r_softShadowSurfCache 0 \
		+set r_softShadowNearRadius 0 \
		+set r_softShadowScanChords "$N" \
		> "$LOG" 2>&1
	echo "=== chords=$N ==="
	grep -E 'BENCH .*TERM/walk|BENCH .*ms/frame' "$LOG" | tail -3
done
