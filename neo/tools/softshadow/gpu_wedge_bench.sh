#!/usr/bin/env bash
# GPU microbenchmark for the analytic soft-shadow wedge: the CPU unit-bench
# (SoftShadowBench_test.cpp) measures op-count but is BLIND to the GPU cost of
# the transcendentals (its libm atan2 is cheap; the GPU's is the expensive
# macro). This exercises the SHIPPED shader on the GPU instead: it renders the
# frozen soft-shadow capture uncapped (r_swapInterval 0 -> GPU-bound) and reads
# com_speeds "all:" with the analytic band OFF vs ON. The delta is the wedge's
# real per-frame GPU cost; run it before/after a shader change for the speedup.
#
# Usage: gpu_wedge_bench.sh [cap.softcap] [width] [height]
# Requires a shipped-shader rebuild first (touch interactionSM.ps.hlsl; build RBDoom3BFG).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
BIN="$ROOT/neo/build/RBDoom3BFG"
CAP="${1:-$HOME/.local/share/rbdoom3bfg/base/softcap/softcap0020.softcap}"
W="${2:-2560}"; H="${3:-1440}"

run() { # $1 = r_shadowMapPCSSAnalyticContact (0 off / 1 on)
  timeout 300 env RBDOOM_HIDDEN_WINDOW=1 WAYLAND_DISPLAY=wayland-0 "$BIN" \
    +set com_skipIntroVideos 1 +set com_skipSignInManager 1 \
    +set r_swapInterval 0 +set r_softShadowMapLod 0 +set r_fullscreen 0 \
    +set r_windowWidth "$W" +set r_windowHeight "$H" \
    +set r_useShadowAtlas 1 +set r_shadowMapPCSS 1 +set r_useSoftShadowVolumes 1 +set r_softShadowAAM 0 \
    +set r_shadowMapPCSSAnalyticContact "$1" \
    +devmap game/erebus1 +wait 240 +loadGame quick +wait 400 \
    +noclip +softShadowGoto "$CAP" +wait 200 +set com_speeds 1 +wait 120 +quit 2>&1 \
  | grep -oE "all: *[0-9]+" | grep -oE "[0-9]+" | tail -120 \
  | awk 'NR==1||$1<m{m=$1} END{if(m)printf "%.2f",m}'   # min frame = GPU-bound floor, repeatable across launches
}

echo "cap=$CAP  ${W}x${H}"
OFF=$(run 0); ON=$(run 1)
awk -v off="$OFF" -v on="$ON" 'BEGIN{
  printf "  PCSS only   : %6.2f ms (%3.0f FPS)\n", off, 1000/off
  printf "  + analytic  : %6.2f ms (%3.0f FPS)\n", on, 1000/on
  printf "  wedge band  : %6.2f ms  <- optimise this\n", on-off
}'
