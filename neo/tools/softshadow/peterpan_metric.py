#!/usr/bin/env python3
# Objective peter-panning metric: compare a PCSS shadow against a peter-pan-IMMUNE
# ray-traced reference of the SAME scene. Peter-panning displaces the shadow away
# from the object, so it shows up as the two shadow masks failing to overlap.
#
# Inputs: two screenshots of one frozen viewpoint, RT and PCSS, same resolution.
# Output: shadow centroid shift (px), IoU, largest disagreement cluster, PASS/FAIL.
#
# No scipy dependency beyond ndimage (used only for the local floor + clustering);
# falls back to a global floor if scipy is absent.
import sys
import numpy as np
from PIL import Image

try:
    from scipy.ndimage import median_filter, label
    HAVE_SCIPY = True
except ImportError:
    HAVE_SCIPY = False


def load(p):
    return np.asarray(Image.open(p).convert("RGB")).astype(float).mean(2)


def shadow_mask(m):
    # a pixel is "shadowed" when it is markedly darker than its local unshadowed
    # surroundings (local median), so shared texture darkness never reads as shadow.
    if HAVE_SCIPY:
        loc = median_filter(m, size=31)
    else:
        loc = np.full_like(m, np.median(m))
    return (m < 0.6 * loc) & (loc > 25)


def centroid(mask):
    ys, xs = np.mgrid[0 : mask.shape[0], 0 : mask.shape[1]]
    n = mask.sum()
    if not n:
        return 0.0, 0.0, 0
    return float(xs[mask].mean()), float(ys[mask].mean()), int(n)


def main():
    if len(sys.argv) < 3:
        print("usage: peterpan_metric.py <rt.png> <pcss.png> [exclude_x0 exclude_y0]")
        sys.exit(2)
    rt, pc = load(sys.argv[1]), load(sys.argv[2])
    if rt.shape != pc.shape:
        print(f"FAIL: size mismatch {rt.shape} vs {pc.shape}")
        sys.exit(2)
    H, W = rt.shape

    sR, sP = shadow_mask(rt), shadow_mask(pc)
    # mask out the first-person weapon (bottom-right) and HUD strip (bottom rows),
    # which are identical in both configs and only add noise.
    keep = np.ones_like(sR, dtype=bool)
    keep[int(H * 0.91) :, :] = False
    keep[int(H * 0.6) :, int(W * 0.6) :] = False
    sR &= keep
    sP &= keep

    cxR, cyR, nR = centroid(sR)
    cxP, cyP, nP = centroid(sP)
    shift = ((cxP - cxR) ** 2 + (cyP - cyR) ** 2) ** 0.5 if (nR and nP) else 0.0
    inter = int((sR & sP).sum())
    union = int((sR | sP).sum())
    iou = inter / union if union else 1.0

    # largest connected region where the two disagree = candidate peter-pan gap
    xor = sR ^ sP
    if HAVE_SCIPY and xor.any():
        lbl, n = label(xor)
        biggest = max((int((lbl == i).sum()) for i in range(1, n + 1)), default=0)
    else:
        biggest = int(xor.sum())

    # Verdict: RT and PCSS shadows should sit on top of each other. A centroid shift
    # of more than ~2 px, a low overlap, or a large coherent disagreement cluster is
    # peter-panning (a scattered handful of edge pixels is just anti-aliasing).
    peterpan = (shift > 2.0) or (iou < 0.85) or (biggest > 40)
    verdict = "PETER-PANNING" if peterpan else "OK (no peter-panning)"
    print(
        f"RTshadow={nR} PCSSshadow={nP}  shift={shift:.2f}px "
        f"(dx={cxP-cxR:.2f} dy={cyP-cyR:.2f})  IoU={iou:.3f}  "
        f"largestDisagreement={biggest}px  ->  {verdict}"
    )
    sys.exit(1 if peterpan else 0)


if __name__ == "__main__":
    main()
