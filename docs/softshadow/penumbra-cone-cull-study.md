# Penumbra cone-cull study — cap0007 (offline CPU)

**Verdict: NO-GO.** A per-cell penumbra-cone cull does **not** replace the ~155-triangle
tile-bin list with a `~|CONTRIB|` set losslessly. The cone-culled set on cap0007 is
**~190–250 triangles regardless of cell size** — parity with the current tile list, not an
order of magnitude below it — and it over-keeps the set that actually casts coverage by ~3.2×.
The genuine small set (≈5 occluders reach 99 % of coverage) is only reachable by evaluating
coverage (contribution-ordered early-out, already shipped), not by a geometric cull.

The cull *is* admissible (provably lossless: 0 dropped contributors, all cell sizes). Its one
real benefit is bounding the pathological depth-discontinuity tiles (tile-list tail 7000+ →
cone ≤500), i.e. it targets the *spill/overflow* tiles, not the median frame cost.

---

## Method (all numbers MEASURED from cap0007, or DERIVED as noted)

- **Harness:** standalone C++ (`scratchpad/study.cpp`), no engine build, no launch. It compiles
  the **shipped shader math as C++**: `softwedge_coverage.inc.hlsl`
  (`SoftShadow_Frame`/`ProjectVert`/`ClipSlab`/`CircleTriArea`) and
  `softsurf_classify.inc.hlsl` (`SurfBuild_SoloCovExact`, the shipped contribution-order key),
  loaded via `neo/tests/SoftShadowMesh.h` (`LoadCap`). No coverage math is hand-rolled.
- **Scene:** `/home/app/.local/share/rbdoom3bfg/base/cap/cap0007.cap` — 7 soft lights,
  6.6k–11.7k caster triangles each (61 971 total), penumbra radius **r = 9.0** world units per
  light (from the capture's `capLight_t.penumbraSize`; sidecar confirms 9.0). Screen 2560×1440.
- **Cell size G = 8** world units (`r_softShadowSurfCacheTexel` default). Receiver fragments are
  sampled on the captured per-light receiver surfaces (`capReceiver_t` → `recvVerts`/`recvIdx`,
  world space), quantised into G-cells. Only **on-screen penumbra cells** are reported (a cell
  with ≥1 fragment whose true coverage is strictly in (0,1)).
- **Sample size (G=8 run):** 420 penumbra cells, 1426 penumbra fragments across the 7 lights.
  Disk ground-truth sampled at 32×32 (≈800 rays inside the disk) per fragment.

### The three sets, per cell

| set | definition | how computed |
|---|---|---|
| **FULL** | what the tile bin presents | shipped tile-bin cull (`softtile_bin.cs.hlsl` L248–256) applied to each 16×16 **screen** tile the cell touches, tile world-AABB built from the light's on-screen receiver fragments. |
| **CONE** | the proposed per-cell cull | the **same shipped predicate**, applied once to the tight per-cell world-AABB (centre `Pc`, half-diagonal `tR`). |
| **REACH** | casts nonzero coverage | `SurfBuild_SoloCovExact > 1e-5` for ≥1 fragment (shipped exact solo coverage). The lossless walk-reduction *target*. |
| **CONTRIB** | changes exact coverage | ground truth: a triangle that is the **sole blocker** of ≥1 disk ray from ≥1 fragment (removing it changes the union coverage). Ray-cast against the per-fragment cone-culled survivors (a provably conservative superset of all ray-blockers). |

**Conservative predicate (documented, mirrors the shipped tile bin exactly):** a triangle with
centroid `tcen`, tight centroid radius `r1w` is kept for a receiver AABB `(Pc, tR)` iff, with
`nrm,distPL` from `Pc→L`, `triRad = r1w + tR`:
`cd = (tcen−Pc)·nrm`; keep unless `cd+triRad < eps`, or `cd−triRad > distPL+tR`, or
`|perp|² > (coneR+triRad)²` where `perp = (tcen−Pc) − cd·nrm`, `coneR = r·(cd+triRad)/(distPL−tR)`.
This is the classic conservative cone–sphere test; the tile bin uses it per tile, CONE uses it
per cell. Losslessness of CONE follows by construction (a cell-AABB cone with `tR` inflation
contains every point-cone for `P` inside the cell) and is confirmed empirically below.

---

## Results — distributions per cell (G=8)

| set | median | p90 | max | mean |
|---|---|---|---|---|
| **\|FULL\|** | 362 | 7335 | 10017 | 1793 |
| **\|CONE\|** | **236** | 350 | 498 | 224 |
| **\|REACH\|** | 64 | 136 | 221 | 70 |
| **\|CONTRIB\|** | **2** | 8 | 24 | 3.4 |

- **Reduction |FULL|/|CONE|:** median **1.18**, p90 **29.8**, max 182.
- **Tightness |CONE|/|REACH|:** median **3.19**, p90 5.7, max 16.3.
- **LOSSLESSNESS:** sole-blockers dropped by CONE = **0**; nonzero-coverage triangles dropped
  by CONE = **0**. The cull is admissible (`CONTRIB ⊆ REACH ⊆ CONE`, no misses).
- **Early-out depth** (occluders in shipped contribution order — biggest solo-coverage first —
  to reach 99 % / 99.9 % of a fragment's final coverage): median **3**, p90 **17 / 22**,
  max 341.

> `|FULL|` note: the `.cap` depth block is degenerate (all 1.0), so FULL's tile world-AABB is
> reconstructed from this light's on-screen receiver fragments rather than the shared depth
> buffer. This makes FULL a *per-light* tile bound; its heavy tail (p90 7335, max 10017) is the
> real depth-discontinuity inflation (two world-separated surfaces in one 16-px tile → giant
> AABB → cone balloons), the same case the engine spills. The median FULL (362) therefore
> sits above the nominal "~155" the walk sees on well-behaved tiles; the NO-GO does not rest on
> FULL's precision — it rests on **CONE ≈ 200–250 being parity with ~155 and 3× looser than
> REACH**.

## Cell-size sensitivity (the decisive control)

| G (world units) | \|CONE\| median | \|REACH\| median | \|CONTRIB\| median | CONE/REACH |
|---|---|---|---|---|
| 2  | 187 | 48 | 1 | 3.51 |
| 4  | 205 | 56 | 2 | 3.36 |
| 8  | 236 | 64 | 2 | 3.19 |
| 16 | 253 | 68 | 2 | 3.07 |
| 32 | 218 | 60 | 2 | 3.43 |

Shrinking the cell all the way to **2 world units** still leaves CONE ≈ **187** — the cull floor
barely moves. The cone from a cell to the r=9 disk is *long and thin*; its lateral thinness is
already near the disk's angular-size floor, so tightening the cell **footprint** does nothing:
CONE is dominated by the **depth extent** of the cone (all the intervening wall/prop geometry the
frustum passes through on its way to the light), which the cell size cannot shrink. This is why
CONE cannot approach REACH geometrically.

---

## Why NO-GO (the reasoning)

1. **CONE is not an order of magnitude below the tile list.** ~200–250 vs a nominal ~155 is
   parity. Feeding the per-fragment walk a 236-triangle CONE list instead of a ~155 tile list
   makes the walk cone-test *more* triangles, not fewer — and it still re-rejects `CONE − REACH`
   ≈ 170 of them per fragment as zero-coverage. The 94 %-reject the hypothesis observed is
   intrinsic to the cone geometry at r=9; a per-cell cone rejects the *same* fraction because
   its survivors are still 3× the real coverage set.
2. **CONE is 3.2× looser than REACH, and REACH is the lossless floor a cull could reach.** The
   ~170 extra triangles are inside the bounding-sphere cone but project *off* the disk (or are
   edge-on / occluded), casting ≈0 coverage. Distinguishing them requires
   `SurfBuild_SoloCovExact` per triangle — i.e. **the walk's own work**. There is no cheap
   geometric proxy that gets from CONE (200) to REACH (60) losslessly.
3. **CONTRIB (2) is real but not a usable walk target.** The strict "changes coverage" set is
   tiny because dense mesh surfaces overlap on the disk — no single triangle is the sole
   blocker, yet the union needs all of them. You cannot walk 2 triangles and reproduce the union
   of ~60 overlapping ones. `|CONE|/|CONTRIB|` (median ~100×) is therefore a mirage.
4. **The genuine lever is contribution-ORDER, already shipped.** ~3–5 occluders (biggest solo
   coverage first) reach 99 % of final coverage; p90 17–22. That is the shipped `depthOrder`
   early-out, not a geometric input cull.

## The one partial win (if pursued)

The per-cell cone **bounds the pathological tiles**: FULL's tail (p90 7335, max 10017 — the
depth-straddling 16-px tiles that spill to the O(all-casters) cluster walk, historically ~12 ms)
collapses to **CONE ≤ 500**. A world-space (or depth-aware) cell binning would specifically
cap those spill tiles. That is a **worst-case/spill** mitigation, not the >2× median-frame lever
the hypothesis proposed.

**Cheapest seam, if the spill-tile win is wanted:** the tile bin is built in
`neo/renderer/Passes/SoftTileBinPass.cpp` (shader `softtile_bin.cs.hlsl`), which already computes
a per-tile receiver AABB and runs this exact cone predicate. Replacing the 16×16 **screen** tile
AABB with a depth-clustered / world-cell AABB (splitting a tile that straddles a depth
discontinuity before the cull) would bound `tR` and kill the spill tail — no new predicate, same
cull math, localized to the AABB construction. Do **not** expect it to shrink the median list.

---

## Reproduce

```
g++ -O2 -std=c++17 -I neo/tests -I neo/shaders/builtin/lighting scratchpad/study.cpp -o study
./study /home/app/.local/share/rbdoom3bfg/base/cap/cap0007.cap [G]   # G defaults to 8
```

Harness + logs live in the session scratchpad (throwaway, not committed). Every distribution
above is over the sampled penumbra cells stated; losslessness is an exhaustive check over every
sampled fragment's ray-cast contributor set against that cell's CONE membership.
