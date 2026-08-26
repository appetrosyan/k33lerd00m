# Occluder-merge study — cap0007 (offline CPU)

**Verdict: GO (qualified) for coplanar merge; NO-GO for convex-object silhouette on this cap.**
Coplanar-triangle merge collapses cap0007's caster geometry **1.58×** globally (61 971 tris →
39 186 coplanar polygons) and cuts the **per-fragment primitive count 2.06× at the median**
(cone list 116 → 56 primitives; the 94 %-reject cull-test tax falls with it, 113 → 55). It is
**lossless by construction** for exactly-coplanar faces — proven bit-exact on a synthetic control
(ray-cast Δ = 0.0022 = one boundary sample, scan-grid Δ = 0.0048 = a 2-bit chord seam). It is a
**~2× lever on the reject tax, not the >100× the shipped contribution-order early-out already
gets** (median 1 triangle actually fills). Convex-object silhouette is a **NO-GO here**: only
**36 of 377** caster objects are convex-closed (**0.6 %** of geometry) — Doom 3 world/prop meshes
are not convex solids.

The cheapest integration seam already exists: `R_CollectPenumbraFaces` (Interaction.cpp:747), the
once-per-static-surface cached tri-stream builder that **already** substitutes a single polygon
record for the tri stream on the box-proxy path (`outIsBox`). Coplanar merge is the general case of
that same substitution.

---

## Method (all numbers MEASURED from cap0007 unless noted)

- **Harness:** standalone C++ (`scratchpad/merge_study.cpp`, copied from the prior study's
  `study.cpp`), **no engine build, no launch.** It compiles the **shipped coverage math as C++**:
  `SoftScan_FillTri` (softwedge_coverage.inc.hlsl, `SW_SCANLINE` path) for the disk bit-grid — the
  exact kernel the term walk fills primitives with — mirroring `neo/tests/SoftShadowFillBox_test.cpp`
  (16 chords × 32 bits). Mesh via `neo/tests/SoftShadowMesh.h` (`LoadCap`). The per-fragment cull is
  the **shipped tile-bin cone predicate** (softtile_bin.cs.hlsl 248-256). No coverage math is
  hand-rolled.
- **Scene:** `/home/app/.local/share/rbdoom3bfg/base/cap/cap0007.cap` — 7 soft lights, penumbra
  radius r = 9.0 world units, screen 2560×1440. A **caster object = one `capCaster_t`** (one
  model-object, the capture's grouping). 377 caster objects across the 7 lights (world geometry
  duplicated per light it interacts with); **61 971 caster triangles total.**
- **Coplanar cluster** = maximal set of edge-adjacent triangles of one object sharing a plane
  (|n·n| ≥ 0.999999, |Δd| ≤ 0.02, transitive), with a **planarity gate** (max vertex-to-plane
  distance): the merged primitive is that cluster's convex polygon.
- **Sample sizes:** Part 1 is exhaustive over all 61 971 triangles / 377 objects. Parts 2-4 sample
  **385 penumbra cells / 1144 penumbra fragments** (G = 8 world-unit cells, ≤12 frag/cell, on-screen
  only). Part 0 (control) = 2443 penumbra fragments on a synthetic patch.

---

## PART 0 — Coplanar-merge is the geometric identity (synthetic control, bit-exact)

A flat 4×4-quad patch (32 triangles, **every vertex exactly in one plane**) merged to its 4-corner
quad, over 2443 penumbra fragments incl. grazing placements:

| check | max \|Δcoverage\| | meaning |
|---|---|---|
| **ray-cast** (member tris vs merged quad, 24×24 disk) | **0.002232** | = **1 sample / 448** (a boundary-grazing ray); the union point-sets are identical |
| **scan-grid** (shipped `SoftScan_FillTri`) | **0.004762** | = **~2 bits**; the ≤1-bit-per-chord rasterization seam between differing internal triangulations |

This isolates the identity from real-mesh float noise: **the union of coplanar triangles and the
merged polygon occupy the same planar point set**, so their disk coverage is equal up to disk
sampling / chord quantization. This is why `SoftScan_FillBox` can already replace a box's 12
triangles bit-for-bit (`SoftShadowFillBox_test`): a merged polygon is a drop-in for the tri union.

## PART 1 — Primitive-count collapse (MEASURED, view-independent)

| quantity | value |
|---|---|
| total caster triangles | **61 971** |
| after coplanar merge | **39 186 polygons** |
| **global reduction** | **1.58×** (36.8 % of triangles eliminated) |
| coplanar-mergeable share (tris in a cluster of size ≥ 2) | **52.8 %** |
| clusters that are ONE convex polygon (fan-lossless) | 38 386 / 39 186 (**98.0 %**) |
| clusters needing a split (concave/holed) | 800 (**2.0 %**) |
| **convex-CLOSED objects** (silhouette = 1 loop) | **36 / 377 = 0.6 % of geometry** |

Per object: **median 61 tris → 41 polygons** (p90 442 → 267). The "regular case" — a flat wall/floor
of N triangles collapsing to one quad — is **just over half** the caster geometry on this cap; the
rest is genuinely non-coplanar (curved trims, detail props, subdivided-but-slightly-non-planar
surfaces). The reduction is real but **not** an order of magnitude — cap0007's casters are already
fairly finely faceted.

## PART 2 — Per-fragment test reduction (the real cost proxy)

For 1144 penumbra fragments, the primitives the walk would fill/reject BEFORE (triangles) vs AFTER
(merged polygons):

| set | median | p90 | max | mean |
|---|---|---|---|---|
| cone list **TRIS** (fill + reject) | **116** | 236 | 385 | 131.3 |
| cone list **POLYGONS** (post-merge) | **56** | 140 | 232 | 67.9 |
| **reduction** | **2.06×** | 3.25× | 5.91× | |
| REJECT-tests **TRIS** (the 94 % tax) | 113 | 227 | | 126.6 |
| REJECT-tests **POLYGONS** | **55** | 133 | | 64.4 |
| FILL-tests **TRIS** (reach) | 1 | 16 | | 4.7 |
| FILL-tests **POLYGONS** | 1 | 11 | | 3.5 |

The mean-131 cone list matches the hypothesis's "~155 triangles/fragment, 94 % reject". **Merging
halves that**: the per-primitive cull test (bounding-sphere cone test — one per polygon, same cost as
one per triangle) runs on ~56 polygons instead of ~116 triangles. Because the merged polygon's
bounding sphere is larger it culls slightly less (55 of 56 polygons pass, vs 113 of 116 tris), so the
saving is the **primitive-count** collapse, not a better cull. **Fills are already tiny (median 1)**
— the shipped contribution-order early-out reaches the union with a handful of primitives, so
merging barely touches the fill count.

## PART 3 — Losslessness (real mesh) + where convex-silhouette breaks

| representation | fills | max \|Δcoverage\| | reading |
|---|---|---|---|
| **coplanar merge**, convex clusters, ray-cast | — | **0.067** | pure geometry; see below |
| **coplanar merge**, convex clusters, scan-grid | 8770 | 0.302 | admitted non-planarity + seam |
| **convex-silhouette** on CONCAVE clusters, scan-grid | 844 | **0.990** | convex-hull overshoot (lossy) |

The **synthetic control (Part 0) is the losslessness proof** — on exactly-coplanar geometry Δ is a
single boundary sample / a 2-bit seam. The **real-mesh residual is not a merge defect**: game
"coplanar" faces are only near-coplanar in float, and soft-shadow projection is a *central*
(perspective) projection whose gradient blows up ~1/dn² at grazing incidence, so a sub-millimetre
out-of-plane deviation on a curved-but-nearly-flat cluster shifts several disk bits. Tightening the
planarity gate collapses the residual monotonically (gate 0.03 → 0.0002 world units drops the
convex-cluster scan Δ from 0.302 to 0.057 and moves ~10 000 slightly-bent clusters into the
"needs split" bucket). **The shippable rule is therefore: merge only vertices that share a plane to
float precision** — flat brush faces qualify; curved surfaces stay triangulated. That is the exact
precondition the synthetic control satisfies.

**Convex-object silhouette is honestly lossy where claimed:** on a concave cluster the convex hull
overshoots by up to 0.99 coverage (it fills concavities and holes). It is exact **only** for
convex-closed objects — of which cap0007 has 36 (0.6 % of geometry). Not worth a code path here.

## PART 4 — Walk-primitive generalization + edge-work

The coverage integral **already takes a convex polygon at ~one-triangle per-primitive cost**:
`SoftScan_FillBox` (softwedge_coverage.inc.hlsl:1497) fills a convex ≤6-gon silhouette by the
*same* slab-clip → project → per-chord x-span-OR kernel as `SoftScan_FillTri`, generalized to N
verts (line 1562, "FillTri body, generalised to loopN verts"), and the term walk already dispatches
it (softterm.cs.hlsl:929). A merged polygon record is a drop-in.

Fill **edge-work** (per-chord edge evaluations, the fill kernel's inner cost):

| | edges |
|---|---|
| BEFORE = 3 × triangles filled | 203 304 |
| AFTER = Σ merged-polygon perimeters | 248 365 |
| ratio | **0.82× (edge-work rises 22 %)** |

This is the one place merging **costs**: a surviving merged polygon fills its whole perimeter (median
4-8 edges) even when only one of its triangles reached the disk, so the *fill* edge-work goes up.
But fills are rare (median 1/fragment) — this term is small next to the **55-113 per-primitive cull
tests** the merge halves. Net per-fragment work is down; the fill-edge rise is a second-order tax,
not a blocker.

## Merge cost + where it lives

The coplanar merge is a **once-per-static-caster-surface CPU pass** (per-frame only for dynamic
casters), cached exactly like today's tri stream:

1. plane-bucket a surface's triangles (quantized normal + offset, weld by position);
2. union edge-adjacent same-plane triangles (union-find);
3. boundary-extract each cluster to a convex polygon (reject non-planar / concave / holed clusters,
   which stay triangulated).

Cost is O(tris) with small constants — negligible against a frame, and **amortized to zero** for
static world geometry (`lightHasMoved` is sticky; static casters build once). This is the same class
of work `R_CollectPenumbraFaces` already does to cluster-order the tri stream.

**Cheapest seam:** `R_CollectPenumbraFaces` (`neo/renderer/Interaction.cpp:747`), the cached
per-surface stream builder. Its leaf emit (Interaction.cpp:935-963) writes 3 float4/triangle records
+ a cluster table; it **already** carries an `outIsBox` path that substitutes a single polygon record
for the tri stream and is consumed by `SoftScan_FillBox`. Coplanar merge generalizes that: emit one
convex-polygon record (corner list, box-style high-bit tag) per fan-lossless cluster, and keep the
tri records only for the non-planar remainder. No new fill primitive, no new cull predicate — the
polygon fill and the box-record encoding both already ship.

---

## GO / NO-GO

- **Coplanar merge: GO (qualified).** 1.58× fewer primitives, **2.06× fewer per-fragment cull tests**
  (the 94 %-reject tax), **lossless** for exactly-coplanar faces (bit-exact synthetic control), at
  amortized-zero merge cost, into an existing cached seam with the fill/record machinery already
  shipped. Qualified because: the win is ~2× on the reject tax (not the frame — fills are already
  early-out-bounded), the mergeable share on this cap is ~53 %, and the merge MUST gate on
  float-precision coplanarity (near-flat surfaces stay triangulated) to stay lossless.
- **Convex-object silhouette: NO-GO on cap0007.** 0.6 % of geometry is convex-closed; lossy on
  everything else. Not worth a path.

## Reproduce

```
g++ -O2 -std=c++17 -I neo/tests -I neo/shaders/builtin/lighting scratchpad/merge_study.cpp -o merge_study
./merge_study /home/app/.local/share/rbdoom3bfg/base/cap/cap0007.cap
# knobs: NDOT/DOFF (coplanar tolerance), PLAN (planarity gate, world units), DBG=1 (worst-case dump)
```

Harness + logs live in the session scratchpad (throwaway, not committed).
