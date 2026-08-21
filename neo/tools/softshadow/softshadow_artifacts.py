#!/usr/bin/env python3
# GPU soft-shadow ARTIFACT gate. Scores a rendered analytic soft-shadow frame against the RT oracle
# of the SAME frozen view, for the defects that read as "broken" in game: hard black bands (an
# over-sharp umbra step where truth is a gradient), halos (a bright/dark ring hugging the shadow),
# turds/ants (false shadow on lit ground), and missing shadow. This gates the ACTUAL rendered frame,
# not a CPU coverage prototype - the only thing that certifies the shipped image.
#
# usage: softshadow_artifacts.py analytic.png rt.png [tag]
# exit 0 = clean, 1 = artifact(s) over threshold. Prints a per-defect line.
import sys
import numpy as np
from PIL import Image

try:
    from scipy.ndimage import median_filter, label, binary_dilation, binary_erosion
    HAVE_SCIPY = True
except ImportError:
    HAVE_SCIPY = False

# thresholds (fractions of the shadow-region pixel count unless noted). Calibrate on real 1920x1080
# analytic-vs-RT frames; these are conservative first guesses, deliberately strict.
HARD_BAND_FRAC = 0.02     # boundary that steps straight to near-black instead of ramping
HALO_FRAC      = 0.02     # ring pixels significantly off the floor tone next to the shadow
TURD_AREA      = 64       # px: a false-shadow blob >= this is a TURD, smaller is an ANT
FALSE_FRAC     = 0.01     # total false-shadow area as a fraction of the true shadow
MISS_FRAC      = 0.02     # true shadow the analytic frame fails to darken


def load(p):
    return np.asarray(Image.open(p).convert("RGB")).astype(np.float32).mean(2)


def local(m, size=31):
    if HAVE_SCIPY:
        return median_filter(m, size=size)
    return np.full_like(m, np.median(m))


def shadow_mask(m):
    # markedly darker than the local unshadowed surround, so shared texture darkness never reads as
    # shadow. loc>25 keeps genuinely-black void (no receiver) out of it.
    loc = local(m)
    return (m < 0.6 * loc) & (loc > 25)


def cc(mask):
    if HAVE_SCIPY:
        lab, n = label(mask)
        if n == 0:
            return []
        return [int((lab == i).sum()) for i in range(1, n + 1)]
    return [int(mask.sum())] if mask.any() else []


def grad(m):
    gy, gx = np.gradient(m)
    return np.hypot(gx, gy)


def main():
    if len(sys.argv) < 3:
        print("usage: softshadow_artifacts.py analytic.png rt.png [tag]")
        sys.exit(2)
    tag = sys.argv[3] if len(sys.argv) > 3 else "?"
    a = load(sys.argv[1])
    r = load(sys.argv[2])
    if a.shape != r.shape:
        print(f"[artifacts {tag}] FAIL size mismatch {a.shape} vs {r.shape}")
        sys.exit(1)
    if min(a.shape) < 1080:
        print(f"[artifacts {tag}] FAIL resolution {a.shape[1]}x{a.shape[0]} < 1920x1080 (defects invisible below that)")
        sys.exit(1)

    sa, sr = shadow_mask(a), shadow_mask(r)
    nTrue = max(int(sr.sum()), 1)

    # (1) HARD BLACK BAND: shadow-boundary pixels that drop straight to near-black (a step), which a
    # soft penumbra never does. boundary = shadow edge; hard = high gradient INTO near-black.
    if HAVE_SCIPY:
        boundary = sa ^ binary_erosion(sa)
    else:
        boundary = sa
    g = grad(a)
    hard = boundary & (a < 24) & (g > 40)
    hardBand = int(hard.sum())

    # (2) HALO: a thin ring just OUTSIDE the shadow whose tone departs from the floor (bright or dark
    # rim). ring = dilate(shadow) - shadow; compare to the floor's local tone.
    if HAVE_SCIPY:
        ring = binary_dilation(sa, iterations=3) & ~sa
    else:
        ring = np.zeros_like(sa)
    loc = local(a)
    halo = ring & (np.abs(a - loc) > 0.35 * np.maximum(loc, 1))
    haloPx = int(halo.sum())

    # (3) FALSE SHADOW (turds/ants): analytic dark where RT is lit. (4) MISSING: RT dark, analytic lit.
    false_shadow = sa & ~binary_dilation(sr, iterations=2) if HAVE_SCIPY else (sa & ~sr)
    missing = sr & ~binary_dilation(sa, iterations=2) if HAVE_SCIPY else (sr & ~sa)
    blobs = cc(false_shadow)
    turds = sum(1 for b in blobs if b >= TURD_AREA)
    ants = sum(1 for b in blobs if b < TURD_AREA)
    falseArea = int(false_shadow.sum())
    missArea = int(missing.sum())

    ok = True
    def chk(name, val, limit, unit=""):
        nonlocal ok
        bad = val > limit
        ok = ok and not bad
        print(f"    [{tag}] {name:14s} {val:8d}{unit}  limit {limit:8.0f}  {'FAIL' if bad else 'ok'}")

    print(f"[artifacts {tag}] {a.shape[1]}x{a.shape[0]}  true-shadow px={nTrue}")
    chk("hard-band px", hardBand, HARD_BAND_FRAC * nTrue)
    chk("halo px", haloPx, HALO_FRAC * nTrue)
    chk("turds", turds, 0)
    chk("ants", ants, 0)
    chk("false-shadow px", falseArea, FALSE_FRAC * nTrue)
    chk("missing px", missArea, MISS_FRAC * nTrue)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
