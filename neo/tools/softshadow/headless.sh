#!/usr/bin/env bash
# Run RBDoom3BFG inside a HEADLESS gamescope micro-compositor - GPU-accelerated, presents to a virtual
# output, never opens a window on the real display. Usage: headless.sh <+cmd ...>
# Resolution defaults to 1920x1080; override with RBDOOM_W / RBDOOM_H. This MATTERS for the .cap repro/gate:
# MOC rasterises occluders at half the render resolution, so resolution-sensitive culls flip with size - a
# capture must be reproduced at the resolution it was taken (the repro shot asserts this and fails loudly).
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="${RBDOOM_BIN:-$HERE/../../build/RBDoom3BFG}"
W="${RBDOOM_W:-1920}"
H="${RBDOOM_H:-1080}"
# s_noSound 1: headless runs are silent (no audio to the host) - these are automated benchmarks
exec gamescope -W "$W" -H "$H" --backend headless -- \
    "$BIN" +set com_skipIntroVideos 1 +set s_noSound 1 +set r_fullscreen 0 +set r_windowWidth "$W" +set r_windowHeight "$H" "$@"
