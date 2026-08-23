#!/usr/bin/env bash
# GPU soft-shadow artifact gate. Renders the whole capture corpus TWICE - once with the shipped
# analytic path, once with the RT oracle - at 1920x1080, then diffs each pair for turds / ants /
# hard black bands / halos / missing shadow. This is the ONLY gate that means anything: it scores
# the actual GPU-rendered frame, not a CPU coverage prototype.
#
# Speed: process init + map load are paid ONCE per config; every capture after that is one frozen
# softShadowGoto + one dumpHDR (a single frame, a fraction of a second). ZERO +wait frames -
# softShadowGoto is a deterministic still, there is no physics to settle. The map assets are the
# only thing that needs loading.
#
# RUN THIS YOURSELF (it launches the game; that is intentional here and must NOT be run on a live
# desktop). Two launches total for the whole corpus.
#   ./gpu_gate.sh                 # default corpus, erebus1 map
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="${RBDOOM_BIN:-$HERE/../../build/RBDoom3BFG}"
BINDIR="$(cd "$(dirname "$BIN")" && pwd)"; cd "$BINDIR"
SAVE="${RBDOOM_SAVEPATH:-$HOME/.local/share/rbdoom3bfg/base}"
CAPDIR="$SAVE/cap"
MAP="${MAP:-game/erebus1}"
W="${W:-1920}"; H="${H:-1080}"
CAPS="${CAPS:-cap0000 cap0001 cap0002 cap0003 cap0004 cap0005}"   # override with e.g. CAPS="cap0001 ..."
OUT="$SAVE"                                            # dumpHDR writes <name>.png here

launch() { # $1 = tag (analytic|rt), $2.. = config cvars: pay init+map-load ONCE, then goto+dump each
           # capture with ZERO waits (a frozen still per capture = a fraction of a second each)
	local tag="$1"; shift
	local tail=""
	for c in $CAPS; do tail="$tail +softShadowGoto $CAPDIR/$c.cap +dumpHDR gate_${tag}_$c"; done
	timeout 400 "$BIN" +set com_skipIntroVideos 1 +set com_skipSignInManager 1 +set com_showFPS 0 \
		+set r_fullscreen 0 +set r_windowWidth "$W" +set r_windowHeight "$H" +set r_swapInterval 0 \
		"$@" +devmap "$MAP" +loadGame quick +noclip $tail +quit
}

echo "[gate] analytic (shipped) corpus render..."
launch analytic +set r_useSoftShadowVolumes 1 +set r_softShadowFaceCoverage 1 \
	+set r_shadowMapPCSS 1 +set r_shadowMapPCSSAnalyticContact 1 +set r_useShadowAtlas 1 +set r_softShadowVRS 0
echo "[gate] RT oracle corpus render..."
launch rt +set r_useRTShadows 1 +set r_rtShadowRays 512 +set r_rtShadowDenoise 0 \
	+set r_useSoftShadowVolumes 0 +set r_shadowMapPCSS 0 +set r_useShadowAtlas 0

echo "[gate] scoring..."
fail=0
for c in $CAPS; do
	python3 "$HERE/softshadow_artifacts.py" "$OUT/gate_analytic_$c.png" "$OUT/gate_rt_$c.png" "$c" || fail=1
done
exit $fail
