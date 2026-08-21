#!/usr/bin/env bash
# Run RBDoom3BFG inside a HEADLESS gamescope micro-compositor - GPU-accelerated, presents to a virtual
# 1080p output, never opens a window on the real display. Usage: headless.sh <+cmd ...>
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="${RBDOOM_BIN:-$HERE/../../build/RBDoom3BFG}"
# s_noSound 1: headless runs are silent (no audio to the host) - these are automated benchmarks
exec gamescope -W 1920 -H 1080 --backend headless -- \
    "$BIN" +set com_skipIntroVideos 1 +set s_noSound 1 +set r_fullscreen 0 +set r_windowWidth 1920 +set r_windowHeight 1080 "$@"
