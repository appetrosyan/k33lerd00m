# Monster convex-hull study — real Doom 3 BFG monster meshes (offline CPU)

**Verdict: GO only for the near-rigid blob class (LOST SOUL); NO-GO as a lossless swap for the
IMP, CACODEMON, and MANCUBUS.** Whole convex hull is bit-exact for the lost soul (100 % gate-clean at
every light softness). For the other three the hull is exact in the **umbra** but **over-shadows the
penumbra soft-edge and inter-limb light-leaks by 0.5–0.9 coverage** — 8–14× the 0.0625 gate quantum.
Decomposition (spatial K-means *or* rig per-bone) lowers the *bulk* error but the per-fragment penumbra
error **plateaus far above the quantum**: the imp goes 31 %→~42 % gate-clean from K=1 to K=32 and then
**stops** (per-bone with 60 parts is *worse* than K=16). The residual is not inter-limb gaps that more
parts would close — it is the **convex fattening of each part's own silhouette**, which no partition
removes. The walk-cost collapse the owner wants is real and large (a shadowed fragment pays for
**100–300+ of the monster's triangles per light**, collapsing to **1** convex-hull primitive), but for
the imp it buys that speed with a penumbra over-shadow that turns the imp's recognizable spread-limb
shadow into a solid blob.

The live seam the owner names — rig per-bone hulls at caster build, filled by `SoftScan_FillBox`
generalized to an arbitrary convex hull — is sound *mechanically* (the fill is proven bit-identical to
a hull's triangle union, `SoftShadowFillBox_test`), but the measurement says it is **not lossless** for
anything but the blob, and the rig does **not** give a "free" accuracy win over spatial clustering.

---

## Geometry source + pose (real meshes, real skeleton)

The four monster meshes are **not** loose `.md5mesh` on this install — Doom 3 BFG ships them as
**compiled binary render models** (`.bmd5mesh`) inside per-map `.resources` containers. I extracted
them directly (no engine, no launch) by parsing the resource container format
(`neo/framework/File_Resource.cpp`: magic `0xD000000D`, mixed-endian table) and the binary MD5 model
format (`idRenderModelMD5::LoadBinaryModel`, `neo/renderer/Model_md5.cpp:764`):

| monster | source container | `.bmd5mesh` | verts | tris | joints | pose |
|---|---|---|---|---|---|---|
| **cacodemon** | `base/maps/caverns1.resources` | `.../monsters/cacodemon/cacodemon.bmd5mesh` | 1117 | 1574 | 53 | bind pose |
| **imp** | `base/maps/admin.resources` | `.../monsters/imp/imp.bmd5mesh` | 946 | 1346 | 71 | bind pose |
| **lost soul** | `base/maps/delta1.resources` | `.../monsters/lostsoul/lostsoul.bmd5mesh` | 268 | 408 | 6 | bind pose |
| **mancubus** | `base/maps/erebus5.resources` | `.../monsters/mancubus/james/mancubus.bmd5mesh` | 1382 | 2054 | 60 | bind pose |

**Pose = the md5 bind pose** baked into each mesh's `deformInfo.verts` (idDrawVert `xyz`, big-endian
floats). The binary bakes the bind-pose vertex positions and **discards the per-vertex joint weights**
— so the "free per-bone decomposition by dominant joint" is not literally available from the shipped
model. The skeleton *does* survive (`joints` + `invertedDefaultPose`), so I recovered joint world
positions from the inverted joint matrices (`origin = −Rᵀt`, in vert space by construction, verified
against the vertex bounds) and assign each vertex to its **nearest joint** as a dominant-joint proxy.
Spatial K-means (K=2,4,8,16,32) is the partition sweep for the "smallest decomposition" question.

Bind-pose extents (world units) show the complexity spread the owner asked for:

| monster | extent X×Y×Z | hull-vertex fraction | shape |
|---|---|---|---|
| lost soul | 19×15×21 | **0.28** | small near-rigid flying skull-blob (6 joints) |
| cacodemon | 63×61×83 | 0.09 | rounded body, but open maw + spread feet/tendrils |
| mancubus | 65×**245**×129 | 0.07 | bulky, arm-cannons spread wide (T-pose) |
| imp | 23×**104**×95 | **0.05** | thin depth, arms+legs+horns spread — genuinely non-convex |

`hull-vertex fraction` = share of mesh verts that lie on the convex hull; the lost soul (0.28) is by
far the most convex, the imp (0.05) the least.

## Method (ground-truth coverage = the shipped kernel)

- **Coverage = the live interval-union math**, not a reimplementation. `scratchpad/cov.cpp` compiles
  `neo/shaders/builtin/lighting/softwedge_coverage.inc.hlsl` as C++ via `neo/tests/hlsl_compat.h`
  (`SW_SCANLINE 1`, `SW_SCAN_BITS 32`, `SW_SCAN_CHORDS 16`) and fills every representation's triangles
  with the shipped `SoftScan_FillTri` / `SoftScan_Run`, exactly mirroring
  `neo/tests/SoftShadowFillBox_test.cpp`. Occluded fraction = `popcount(grid & diskMask)/diskBits`.
- **Scene (per monster):** disc light of radius `swR` at `C + d·6Rm`, receiver plane at `C − d·2.5Rm`,
  light direction `d` = the monster's **thin axis** (worst case: the silhouette exposes the spread
  limbs). ~2500 receiver fragments sampled on a 50×50 grid across the shadow footprint; a fragment is
  *shadowed* if exact occ > 0.02, *penumbra* if 0.02 < occ < 0.98, *umbra* if occ ≥ 0.98.
- **Representations:** `exact` (all monster triangles) · `whole-hull` · `kmeans-{2,4,8,16,32}`
  (per-cluster hull) · `per-bone` (per-nearest-joint hull). Each convex hull counts as **one** walk
  primitive because `SoftScan_FillBox` fills a convex silhouette in one call, bit-identical to walking
  its triangle union (the shipped analytic-box contract).
- **Over-shadow error** = |Δcoverage| of a representation vs the exact triangle stream, per shadowed
  fragment. All numbers MEASURED. `swR = 9` is the primary (matching prior studies); `swR ∈ {3,9,18}`
  is the sensitivity sweep.

---

## Result 1 — whole hull (swR = 9)

| monster | tris → hull (V/T) | primitive collapse | max \|Δ\| | p90 \|Δ\| | mean \|Δ\| | **gate-clean %** | penumbra p90 | **penumbra-clean %** |
|---|---|---|---|---|---|---|---|---|
| **lost soul** | 408 → 76/148 | 408 → **1** | **0.031** | 0.010 | 0.003 | **100.0 %** | 0.012 | **100.0 %** |
| **mancubus** | 2054 → 100/196 | 2054 → **1** | 0.979 | 0.617 | 0.131 | 75.4 % | 0.900 | 15.5 % |
| **cacodemon** | 1574 → 99/194 | 1574 → **1** | 0.979 | 0.730 | 0.204 | 61.2 % | 0.873 | 18.7 % |
| **imp** | 1346 → 52/100 | 1346 → **1** | 0.976 | 0.862 | 0.351 | **31.1 %** | 0.895 | **6.7 %** |

The error is **entirely a penumbra / soft-edge phenomenon.** Bucketed by exact occ (whole-hull, per
monster, mean |Δ|):

| exact occ bucket | lost soul | cacodemon | mancubus | imp |
|---|---|---|---|---|
| 0.02–0.20 (soft rim / leaks) | 0.004 | 0.63 | 0.79 | **0.81** |
| 0.20–0.50 | 0.004 | 0.53 | 0.57 | 0.63 |
| 0.50–0.80 | 0.004 | 0.27 | 0.33 | 0.34 |
| 0.80–0.98 | 0.006 | 0.08 | 0.08 | 0.11 |
| 0.98–1.00 (umbra) | 0.000 | **0.000** | **0.000** | **0.001** |

For the three complex monsters the **umbra is bit-exact** (mean |Δ| ≈ 0) — the hull and the mesh both
fully block. The over-shadow lives where the mesh only *partially* blocks: the fattened hull silhouette
and the inter-limb light-leaks the hull fills in. The lost soul is exact in *every* bucket — it is a
convex blob, so hull = mesh everywhere.

## Result 2 — decomposition sweep (gate-clean %, swR = 9)

Overall gate-clean % (and penumbra-clean %) vs number of hull primitives:

| primitives | lost soul | cacodemon | mancubus | imp (pen-clean) |
|---|---|---|---|---|
| 1 (whole) | **100 %** | 61 % | 75 % | 31 % (6.7 %) |
| K=2 | 89 % | 65 % | 72 % | 30 % (9.7 %) |
| K=4 | 81 % | 66 % | 74 % | 38 % (17.8 %) |
| K=8 | 59 % | 65 % | 77 % | 38 % (23.1 %) |
| K=16 | 20 % | 63 % | 75 % | 44 % (35.6 %) |
| K=32 | 6 % | 62 % | 67 % | 42 % (40.9 %) |
| per-bone | 84 % (5) | 65 % (48) | 65 % (49) | 36 % (33.5 %) (60) |

Two things the decomposition sweep proves:

1. **Decomposition never makes a non-blob gate-clean.** The imp's penumbra-clean share climbs
   6.7 %→40.9 % from K=1 to K=32 and then **plateaus** — K=32 is no better than K=16, and per-bone
   (60 parts) is *worse*. The floor is the **per-part convex fattening**: each limb's own hull is
   fatter than the limb, so each part over-shadows its own soft edge no matter how finely you split.
   Closing inter-limb gaps (what more parts buys you) is a second-order term next to that floor.
2. **Decomposition HURTS a convex object.** Splitting the lost soul's convex blob drops it from 100 %
   to 20 % (K=16) / 6 % (K=32) gate-clean — the per-cluster hulls *under*-shadow between clusters. So
   there is no monotone "more parts = better"; the right number of parts is object-dependent, and for
   the truly convex case it is **one**.

The rig per-bone partition tracks the spatial K sweep and does not beat it (imp per-bone ≈ K=8; mancubus
per-bone < K=8). The skeleton buys a *natural* partition, not a *more accurate* one.

## Result 3 — light-softness sensitivity (whole hull)

The umbra-exact / penumbra-lossy split is **swR-robust**. Per-fragment penumbra error stays ~0.85–0.90
at every softness; what changes is how much of the shadow *is* penumbra:

| monster | swR=3 clean % | swR=9 clean % | swR=18 clean % | penumbra p90 (all swR) |
|---|---|---|---|---|
| lost soul | 100 % | 100 % | 100 % | ~0.01–0.02 |
| mancubus | 89 % | 75 % | 57 % | ~0.80–0.92 |
| cacodemon | 81 % | 61 % | 51 % | ~0.82–0.87 |
| imp | 69 % | 31 % | **8 %** | ~0.86–0.90 |

Harder light → mostly umbra → hull looks fine; softer light → mostly penumbra → the hull's error
dominates (imp at swR=18 is gate-clean on only 8 % of its shadow). The per-fragment penumbra error is
geometric (silhouette fattening), not a soft-light artifact.

## Result 4 — walk cost (before → after)

A fragment squarely behind a monster pays, in the cone list, for a large share of that monster's
triangles **per light** (conservative cone admit test, sampled shadowed fragments):

| monster | monster tris | cone-list tris / fragment (median / p90 / max) | after merge |
|---|---|---|---|
| cacodemon | 1574 | 44 / 168 / 252 | **1** (whole) … K (decomp) |
| imp | 1346 | 2 / 75 / **276** | **1** … K |
| lost soul | 408 | 189 / 283 / 324 | **1** |
| mancubus | 2054 | 2 / 33 / 106 | **1** … K |

The collapse is exactly the owner's "hundreds of primitives → single digits": 100–300+ cone-list
triangles at a deep-shadow fragment become **one** convex-hull primitive (or K for a decomposition),
each filled by one `SoftScan_FillBox`-class call — bit-identical to the hull's triangle union, so no
new fill primitive and no new cull predicate is needed. The cull-tax (the 94 %-reject cost) falls with
the primitive count. **The frame-drop lever works; accuracy is the only blocker.**

---

## Complexity ranking (blob → imp)

By intrinsic convexity (hull-vertex fraction) and whole-hull penumbra fidelity:

1. **Lost soul** — hull-V 0.28, penumbra p90 0.012, **100 % gate-clean**. A convex blob.
2. **Cacodemon** — hull-V 0.09, penumbra p90 0.87. Rounded body but open maw + spread tendrils make
   it non-convex; umbra-exact, penumbra badly over-shadowed.
3. **Mancubus** — hull-V 0.07, penumbra p90 0.90. Bulky solid torso (large umbra, so high *overall*
   clean %) but the wide arm-cannons over-shadow the fringe as badly as the imp.
4. **Imp** — hull-V 0.05, penumbra p90 0.90, **31 % gate-clean (6.7 % of penumbra)**. The spread
   limbs/horns make almost the whole shadow soft-edge or leak; the worst case, exactly as predicted.

## GO / NO-GO

- **Lost soul (and the small near-rigid flying-blob class): GO.** Whole hull is bit-exact — 100 %
  gate-clean at every light softness, penumbra p90 ≈ 0.01. Replace the triangle stream with the single
  hull outright; do **not** decompose it (splitting a convex blob *adds* error).
- **Imp / cacodemon / mancubus: NO-GO as a lossless swap.** Umbra-exact, but the penumbra soft-edge
  and inter-limb leaks over-shadow by 0.5–0.9 coverage (8–14× the quantum), and **no decomposition
  brings the imp/mancubus penumbra under the quantum** — spatial K and rig per-bone both plateau at the
  convex-fattening floor (imp ~40 % penumbra-clean at best, and adding parts past K≈16 stops helping /
  starts hurting). There is no "smallest K" that clears the gate for these; the answer is *none up to
  K=32 / 60 bones*.
- **"Does convex-hull merge remove the monster frame-drop?"** The *cost* answer is yes — hundreds of
  cone-list triangles collapse to one convex primitive. But for the priority case (imp) it trades the
  frame-drop for a penumbra over-shadow that reads as a solid-blob shadow instead of the imp's
  spread-limb silhouette, and per-limb/per-bone decomposition does not fix it. So **NO-GO as a blanket
  optimization; GO only for near-convex blobs.**

**Productive direction the data supports (not yet measured):** the hull is *exact in the umbra*, so use
it as an **umbra-only accelerator** — fill the coarse convex hull for the fully-occluded core (where it
is bit-exact and cheap) and keep the exact triangle stream only for the silhouette/penumbra band. That
preserves the walk collapse over the umbra (the bulk of a deep shadow) while leaving the soft edge
correct, and it needs the same generalized `SoftScan_FillBox` primitive this study assumed.

## Reproduce

```
# scratchpad harness (throwaway, not committed):
#   extract.py     - pull .bmd5mesh out of a .resources container
#   md5load.py     - parse binary MD5 (bind-pose verts, tris, joints via inverted pose)
#   cov.cpp        - SHIPPED coverage kernel compiled as C++ (the ground truth)
#   study.py       - hulls + K-means/per-bone decomposition + fragment sampling + Δ stats
g++ -O2 -std=c++17 -I neo/tests -I neo/shaders/builtin/lighting cov.cpp -o cov
KS=2,4,8,16,32 python3 study.py ex/*.bmd5mesh     # SWR=<r> for a different light radius
```
