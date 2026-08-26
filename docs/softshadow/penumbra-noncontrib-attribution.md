# Penumbra non-contributor attribution — cap0007 (offline CPU)

**Verdict: NO-GO.** No cheap lossy per-triangle pre-cull shrinks the term walk to a double-digit
*gate-clean* buffer. The walk's ~155–200 tested triangles per fragment reject at the quantum not
because they are individually cull-able noise, but because the umbra/deep-penumbra is the **UNION of
a dense fog of individually sub-quantum triangles** — each one's leave-one-out marginal is < quantum
(99.9 % of the walk), yet their union *is* the shadow. Drop them and coverage collapses: even a
*perfectly sound* "keep only solo ≥ quantum" oracle (median 3 kept) produces `max|Δcoverage| = 0.89`
and busts the gate on 28 % of fragments. The property that decides droppability ("does the union
still cover the disk?") is **not a per-triangle property**, so no per-triangle predicate can recover it.

The two named candidates both fail on first principles *and* in measurement:

- **Projected-solid-angle upper bound: not sound**, twice over. (1) The flat-patch closed form is not
  even an upper bound on the shipped solo coverage — it *under*-bounds on 0.8 % of triangles with
  `maxUnder = 1.00` (contact-hardening triangles straddling the receiver plane: degenerate centroid
  depth, but large clipped coverage the shipped per-edge slab-clip captures and a centroid bound
  cannot). (2) Even where sound, thresholding it drops the sub-quantum fog and collapses the union.
- **Silhouette extrusion: does not identify the non-contributors** for this walk. The shipped term
  walk is **union sampling** (`softwedge_coverage.inc.hlsl`: *"any triangle covering a sample occludes
  it"*), which it adopted precisely *because* Doom casters are single-sided / non-manifold and the
  signed-area winding drained the umbra to holes. In a union of ray hits the **interior** triangles
  block the central samples — they are not redundant. Interior-to-silhouette explains only **0.3 %**
  of non-contributors; keeping only silhouette triangles under-shadows (`maxΔ = 0.90`, 15 % of
  fragments bust the gate). Silhouette extrusion is lossless *only* against a winding-over-edges
  representation, which this walk deliberately abandoned.

---

## Method (numbers MEASURED from cap0007)

- **Harness:** `scratchpad/study2.cpp` (extends the prior `study.cpp`), no engine build, no launch.
  Per-triangle **solo** coverage is the SHIPPED `SurfBuild_SoloCovExact` (`softsurf_classify.inc.hlsl`,
  dual-compiled via `neo/tests/hlsl_compat.h`). The **union** ground truth is dense disk ray-casting —
  which *is* the shipped coverage model (the term walk is union sampling, not the retired signed-area
  integral), just at 812 samples (diskN 32) / 1804 samples (diskN 48) instead of the runtime's 16.
- **Scene:** `cap0007.cap`, 7 soft lights, penumbra radius r = 9.0, screen 2560×1440. Mesh loaded via
  `neo/tests/SoftShadowMesh.h`; the degenerate all-1.0 depth block is bypassed (receiver fragments
  reconstructed from `capReceiver_t`, as in the prior study).
- **Walk set (`sfrag`):** the shipped per-fragment cone cull (`softtile_bin.cs.hlsl` predicate, tR = 0)
  — the triangles the term actually tests. Median **196**, p90 311, max 347 (the task's "~155").
- **Sample:** 1400 on-screen **penumbra** fragments (coverage strictly in (0,1)) across the 7 lights,
  295 447 walk-triangle tests. Re-run at diskN 48 / 800 fragments: identical picture (below).

### Two lenses on "non-contribution" (they disagree, and the gap is the whole story)

| lens | definition | result |
|---|---|---|
| **SOLO < q** (the task's "reject") | shipped `SurfBuild_SoloCovExact` < quantum | **98.2 %** of walk triangles |
| **MARGINAL < q** (leave-one-out) | removing the triangle alone changes union coverage by < q | **99.9 %** of walk triangles |

The marginal lens is the honest one for culling (it asks "can I drop this?") and it is *stricter*:
only **0.11 %** of tested triangles are a sole blocker of ≥ q of the disk. That 99.9 % are individually
droppable is exactly why a naive cull looks tempting — and exactly the trap, because *droppable
individually ≠ droppable collectively*.

---

## Step 1 — Attribution histogram (why the ~99.9 % don't individually matter)

Primary-cause partition of the 295 126 non-contributors (marginal < q = 0.0625), diskN 32:

| primary cause | count | share |
|---|---:|---:|
| **SMALL** (projected solid angle small even face-on) | 207 904 | **70.4 %** |
| **OUT-OF-CONE** (centroid projects off the disk) | 63 875 | 21.6 % |
| **EDGE-ON** (`|N·V| < 0.05`, solo < q) | 18 545 | 6.3 % |
| **OCCLUDED-BY-NEARER** (solo ≥ q, silhouette, redundant) | 3 869 | 1.3 % |
| **INTERIOR-TO-SILHOUETTE** (solo ≥ q, interior) | 933 | **0.3 %** |

Overlap matrix (a triangle may satisfy several; % of non-contributors): OUT-OF-CONE 21.6 %,
EDGE-ON 10.1 %, SMALL(size) 9.4 %, **REDUNDANT(solo ≥ q) 1.7 %** (of which interior 0.3 %,
silhouette 1.4 %).

**Reading:** 98.3 % of the reject is the *geometric* family (SMALL/OUT-OF-CONE/EDGE-ON — i.e. the
triangle casts little on its own), dominated by **SMALL**: the shadow is a fog of tiny projections.
Only 1.6 % are "casts real coverage but redundant." So redundancy (the silhouette / already-covered
story) is a rounding error here; the mass is *small solo coverage that nonetheless tiles the umbra in
union*.

---

## Step 2 — Cheap lossy pre-culls (kept buffer + measured error)

Kept buffer size and `max|Δcoverage|` of `union(kept)` vs `union(all)`, per fragment, diskN 32.
Gate-clean = **no** fragment exceeds the quantum.

| predicate | kept median / p90 / max | maxΔ | p99 Δ | q=0.0625 busts | gate-clean | q=0.03125 busts | gate-clean |
|---|---|---:|---:|---:|:--:|---:|:--:|
| **P1** solid-angle UB ≥ q | 95 / 230 / 281 | 0.999 | 0.918 | 31.4 % | **no** | 34.1 % | **no** |
| **P2** `|N·V| ≥ eps` (0.05) | 132 / 238 / 283 | 0.999 | 0.956 | 15.0 % | **no** | 15.9 % | **no** |
| **P3** silhouette-only | 142 / 250 / 280 | 0.895 | 0.605 | 14.6 % | **no** | 18.3 % | **no** |
| **STACK** UB ∧ silhouette | 74 / 191 / 223 | 0.999 | 0.927 | 42.4 % | **no** | 45.4 % | **no** |
| **REF** solo ≥ q (shipped, expensive) | **3 / 8 / 20** | 0.890 | 0.336 | 28.2 % | **no** | 38.6 % | **no** |

`REF` is the reference "what if the cull were the shipped solo math itself" — the only predicate that
gets to a double-digit buffer (median 3). It is still **not gate-clean** (28 % bust). So the buffer
size and the gate-cleanliness are in direct opposition: everything small enough to help is lossy past
the quantum, and everything gate-clean is parity with the walk.

### Error by coverage band (maxΔ / % of fragments over q = 0.0625)

| predicate | [.03,.25) | [.25,.50) | [.50,.75) | [.75,.97) | [.97,1.0) |
|---|---|---|---|---|---|
| P1 solid-angle UB | 0.23 / 10 % | 0.49 / 50 % | 0.73 / 24 % | 0.97 / 31 % | 1.00 / 21 % |
| P2 `|N·V|` | 0.23 / 9 % | 0.49 / 8 % | 0.73 / 21 % | 0.97 / 31 % | 1.00 / 29 % |
| P3 silhouette | 0.22 / 5 % | 0.49 / 8 % | 0.73 / 28 % | 0.90 / 30 % | 0.68 / 19 % |
| REF solo ≥ q | 0.19 / 11 % | 0.42 / 53 % | 0.73 / 18 % | 0.89 / 11 % | 0.31 / 10 % |

The error grows monotonically into the umbra (the accumulation is worst where coverage → 1), but it
is **already over quantum in the shallowest band**: even at coverage [.03,.25) every predicate busts
the gate on 5–12 % of fragments with maxΔ ≈ 0.2. There is no coverage regime where a cheap cull is clean.

### UB soundness (the explicit question)

Across 295 447 walk triangles, the projected-solid-angle upper bound `UB < solo` (i.e. it *fails* to
bound the shipped solo coverage) on **2 343 (0.79 %)**, with `maxUnder = 1.00`. The failures are
contact-hardening triangles that straddle the receiver plane: the shipped math clips each edge to the
depth slab `[eps, distPL]` and can return solo ≈ 1 from the surviving front portion, while the
centroid-based `(area, dn, offset)` bound sees a degenerate depth and returns ≈ 0. A per-triangle
centroid/bounding predicate is therefore unsound *exactly* at the visually critical near-contact
region. Under the marginal lens this drops **178 real sole-blockers** outright.

---

## Answers to the deliverable's explicit questions

- **Does silhouette extrusion identify the non-contributors?** No. For the shipped **union-sampling**
  walk, interior triangles block the central disk samples and are genuine contributors; interior-to-
  silhouette is only 0.3 % of non-contributors, and silhouette-only culling under-shadows (`maxΔ 0.90`,
  15 % gate busts). It would be lossless only against a winding-over-silhouette-edges coverage, which
  this walk deliberately abandoned for non-manifold Doom geometry.
- **Is a per-triangle projected-solid-angle upper bound a sound lossy cull to the quantum?** No, on
  two independent grounds: (1) the closed form is not a sound upper bound on solo coverage
  (under-bounds contact triangles, `maxUnder 1.00`), and (2) even a sound solo threshold busts the
  gate by up to 0.89 because the umbra is an accumulation of sub-quantum contributors.
- **Does anything cheap reach a ~10–20 gate-clean buffer?** No. The only predicate reaching a
  double-digit buffer (REF, median 3) is the expensive shipped solo eval itself, and it is still not
  gate-clean. Stated plainly with the numbers: **the smallest gate-clean per-triangle cull is
  parity with the walk, not double digits.**

## What this leaves (already shipped, not a per-triangle input cull)

The lever is on the **union**, not the input set: contribution-**ORDER** early-out (biggest solo first
reaches ≥ 99 % coverage in ~3–5 occluders — the shipped `depthOrder`), and the **contributor cache**
(evaluate-once union, record the actual contributing set per cell/light and replay it). Both *evaluate*
the union rather than predicting it from a per-triangle geometric proxy — which is unavoidable, because
this attribution shows the deciding quantity is not carried by any single triangle.

---

## Reproduce

```
g++ -O2 -std=c++17 -I neo/tests -I neo/shaders/builtin/lighting scratchpad/study2.cpp -o study2
./study2 /home/app/.local/share/rbdoom3bfg/base/cap/cap0007.cap [diskN=32] [maxFrag=1400]
```

Harness + logs live in the session scratchpad (throwaway, not committed). Every Δ is `union(kept)`
vs `union(all)` over the stated disk samples per fragment; solo/UB soundness is an exhaustive check
over every sampled walk triangle. Confirmed stable at diskN 48 (1804 samples/800 fragments): solo<q
98.2 %, marginal<q 99.9 %, UB-under 0.79 %, P1 maxΔ 0.99 / REF maxΔ 0.89 — unchanged.
