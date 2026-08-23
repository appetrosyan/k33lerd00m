#!/usr/bin/env python3
# Objective GEOMETRIC peter-pan metric. No shadow-method comparison (which is blind for
# dynamic gibs - all methods sample the same atlas). Instead: from the .cap, project
# the caster from each light onto the floor it rests on -> where the shadow MUST be; then
# compare to where the shadow actually renders. Displacement along the cast direction = peter-pan.
#
# usage: peterpan_geometric.py <render.png> <capture.cap> [caster_index]
#   render must be at the capture's native resolution (2560x1440 for cap0020).
#   caster_index defaults to auto-pick the caster nearest the shard screen position.
import sys, struct
import numpy as np
from PIL import Image


def parse(cap):
    f = open(cap, "rb").read()
    o = 16 + 12 + 36 + 8 + 64 + 64 + 64
    mvp = np.array(struct.unpack_from("<16f", f, o), np.float64).reshape(4, 4); o += 64
    o += 16 + 4
    nL, nE, nC, nMV, nMI = struct.unpack_from("<5I", f, o)[:5]; o += 36 + 20
    W, H = struct.unpack_from("<2I", f, 8)
    lights = [np.array(struct.unpack_from("<3f", f, o + i * 48)) for i in range(nL)]; o += nL * 48
    o += nE * 32
    casters = [struct.unpack_from("<IfIIII", f, o + i * 24) for i in range(nC)]; o += nC * 24
    mv = np.array(struct.unpack_from("<%df" % (nMV * 3), f, o), np.float64).reshape(nMV, 3)
    return mvp, lights, casters, mv, W, H


def project(mvp, pts, W, H):
    c = np.c_[pts, np.ones(len(pts))] @ mvp.T
    w = c[:, 3]
    ndc = c[:, :3] / w[:, None]
    return np.c_[(ndc[:, 0] * 0.5 + 0.5) * W, (1 - (ndc[:, 1] * 0.5 + 0.5)) * H]


def main():
    render, cap = sys.argv[1], sys.argv[2]
    mvp, lights, casters, mv, W, H = parse(cap)
    img = np.asarray(Image.open(render).convert("RGB")).astype(float).mean(2)
    rH, rW = img.shape
    sc = rW / W  # render may be scaled from native

    # pick shard caster: smallest on-screen caster nearest image center-right (or arg)
    if len(sys.argv) > 3:
        ci = int(sys.argv[3])
    else:
        best = None
        for i, (li, cid, fv, nv, fi, ni) in enumerate(casters):
            p = project(mvp, mv[fv:fv + nv], W, H)
            ins = (p[:, 0] >= 0) & (p[:, 0] < W) & (p[:, 1] >= 0) & (p[:, 1] < H)
            if ins.sum() < 5:
                continue
            zspan = mv[fv:fv + nv, 2].ptp()
            cen = p[ins].mean(0)
            score = zspan + 0.01 * np.linalg.norm(cen - [W * 0.5, H * 0.5])  # prefer thin + central
            if best is None or score < best[0]:
                best = (score, i)
        ci = best[1]
    li, cid, fv, nv, fi, ni = casters[ci]
    shard = mv[fv:fv + nv]
    zf = shard[:, 2].min()
    shard_scr = project(mvp, shard, W, H) * sc
    sxc, syc = shard_scr.mean(0)

    # expected shadow: shard verts projected from each light onto floor plane z=zf
    exp = []
    for L in lights:
        for V in shard:
            dz = V[2] - L[2]
            if abs(dz) < 1e-6:
                continue
            t = (zf - L[2]) / dz
            if t >= 0:
                exp.append(L + t * (V - L))
    exp = np.array(exp)
    exp_scr = project(mvp, exp, W, H) * sc
    exp_cen = exp_scr.mean(0)

    # actual shadow: dark pixels near shard, excluding the shard body
    yy, xx = np.mgrid[0:rH, 0:rW]
    reg = ((xx - sxc) ** 2 + (yy - syc) ** 2 < (420 * sc) ** 2) & (xx < sxc + 40 * sc) & (yy > syc - 60 * sc)
    dark = (img < 45) & reg & ~(((xx - sxc) ** 2 + (yy - syc) ** 2) < (90 * sc) ** 2)
    ys, xs = np.where(dark)
    if len(xs) < 20:
        print(f"caster {ci}: no shadow found (<20 dark px)"); return
    act_cen = np.array([xs.mean(), ys.mean()])
    disp = act_cen - exp_cen
    print(f"caster={ci} shard@({sxc:.0f},{syc:.0f}) expected@({exp_cen[0]:.0f},{exp_cen[1]:.0f}) "
          f"actual@({act_cen[0]:.0f},{act_cen[1]:.0f})  DISPLACEMENT={np.linalg.norm(disp):.0f}px "
          f"(dx={disp[0]:.0f} dy={disp[1]:.0f}) @ {rW}x{rH}")


if __name__ == "__main__":
    main()
