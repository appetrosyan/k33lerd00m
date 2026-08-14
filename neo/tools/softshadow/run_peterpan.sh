#!/usr/bin/env bash
# Autonomous headless peter-panning harness.
#
# Renders one captured viewpoint TWICE - once with ray-traced shadows (the
# peter-pan-IMMUNE oracle: shadow at the true contact point), once with PCSS -
# then measures how far the PCSS shadow drifted from the RT shadow.
#
# Two separate launches by design: r_useRTShadows builds its acceleration
# structure at MAP LOAD, so it only engages when set on the command line, not
# when toggled mid-session from the console. Each config is therefore its own
# launch, both driven to the exact same frozen viewpoint by softShadowGoto.
#
# Usage: run_peterpan.sh <capture.softcap> [map]
#   default map is game/erebus1 (all erebusN captures load it).
set -euo pipefail

CAP="${1:?usage: run_peterpan.sh <capture.softcap> [map]}"
MAP="${2:-game/erebus1}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="${RBDOOM_BIN:-$HERE/../../build/RBDoom3BFG}"
BINDIR="$(cd "$(dirname "$BIN")" && pwd)"
# +screenshot writes to <cwd>/base, so run from the binary's dir and read from there.
cd "$BINDIR"
SAVE="${RBDOOM_SAVEPATH:-$BINDIR/base}"

launch() { # $1=shotname  $2..=extra cvars
	local shot="$1"; shift
	timeout 340 env RBDOOM_HIDDEN_WINDOW=1 WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}" \
		"$BIN" +set com_skipIntroVideos 1 +set com_showFPS 0 "$@" \
		+devmap "$MAP" +wait 240 +loadGame quick +wait 400 \
		+noclip +softShadowGoto "$CAP" +wait 400 \
		+com_showFPS 0 +wait 5 +screenshot "$shot" +wait 30 +quit >/dev/null 2>&1
}

echo "[peterpan] RT oracle launch..."
launch pp_rt.png   +set r_useRTShadows 1 +set r_shadowMapPCSS 0 +set r_useShadowAtlas 0 \
                   +set r_rtShadowDenoise 0 +set r_rtShadowRays 512
echo "[peterpan] PCSS launch..."
launch pp_pcss.png +set r_useRTShadows 0 +set r_shadowMapPCSS 1 +set r_useShadowAtlas 1

echo "[peterpan] measuring..."
python3 "$HERE/peterpan_metric.py" "$SAVE/pp_rt.png" "$SAVE/pp_pcss.png"
