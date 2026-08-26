# Level convex-subset study — brushes & compiled world as a lossless soft-shadow primitive-reduction lever

**Offline CPU study. No engine build, no gate/game launch.** Branch `analytical-penumbra`. Every number below
was parsed straight out of `base/maps/*.resources` (the text `.map` source and the binary `generated/…/.bproc`
compiled world) and every over-shadow figure was MEASURED with the shipped interval-union coverage kernel
(`softwedge_coverage.inc.hlsl` compiled as C++, the same `cov` binary the model study used — see
`docs/softshadow/model-convex-inventory.md`). Bench maps: **erebus1** (the RoE-intro benchmark), plus
**mars_city1** and **hell1** for breadth. Scripts in the session scratchpad (`brush.py`, `proc.py`,
`measure_brush.py`, `measure_level.py`).

## Verdict (TL;DR)

**GO — but via the dmap BRUSH-RECOVERY path, not a load-time `.proc` decomposition.**

- A Doom 3 **brush is convex by construction** (intersection of half-spaces), so the `.map` brush list is a
  ready-made, exact convex decomposition of the world. Verified with the shipped kernel: **a brush's convex
  hull casts the bit-identical soft shadow to its own triangulated faces — worst |Δcoverage| = 0.000000 across
  every fragment of 250 sampled brushes** (quantum 0.0625). A true convex subset over-shadows by exactly zero.
- erebus1 has **2832 visible (shadow-casting) brushes** collapsing **12.6×** (median 12 tris/box-brush → 1 hull);
  against the CSG-split compiled world the worldspawn collapse is **22.5×** (54 677 area tris → 2431 hulls).
  Consistent across maps (brush-tri 12.0–12.6×, worldspawn-hull 12.9–22.5×). Lossless.
- **The compiled `.proc` has already destroyed the linkage AND the solids.** Area surfaces are grouped by
  material per BSP leaf, carry no brush/primitive id, and are open shells (interior faces CSG'd away). A
  load-time convex-decomposition therefore only has *coplanar* regions to recover, and BSP splitting left most
  of them non-convex (doorway holes, L-shaped floors): the naive per-plane merge leaks up to |Δ| 0.98, and the
  **lossless coplanar merge collapses only 1.16×.** Not worth a load-time pass.
- **Recommendation: emit one convex hull per brush at dmap time**, where the brushes still exist intact, keyed
  per BSP area, fed to the existing `SoftScan_FillBox/FillPoly`. Zero runtime cost, ~2431 hulls/map (<100 KB).

## 1 — Brushes (`.map`): the world's built-in convex decomposition

`erebus1.map` is `Version 2` (brushDef3 half-space brushes + Valve-220 texcoords). Parsed all entities and
built each brush polyhedron by intersecting plane triples and keeping points inside every half-space
(`n·p + d ≤ 0`, verified against box brushes). **All 3742 brushes built clean polyhedra (0 failures).**

| map | entities | brushes | visible (shadow-casting) | clip/nodraw/trigger | worldspawn visible | Bezier patches |
|---|---:|---:|---:|---:|---:|---:|
| **erebus1** | 1420 | 3742 | **2832** | 910 | 2431 | 2352 |
| mars_city1 | — | 4133 | 3546 | 587 | 3441 | 2678 |
| hell1 | — | 6281 | 4463 | 1818 | 3961 | 803 |

"Visible" = at least one face has a normal drawn/opaque material (clip, player_clip, nodraw, caulk, trigger,
areaportal, aassolid… cast no shadow and are excluded). Each visible brush is one convex solid → **1 hull**.

**Collapse (tris → 1 hull per brush):**

| map | visible-brush tris | hulls | tris/brush (min/median/mean/max) | collapse |
|---|---:|---:|---:|---:|
| **erebus1** | 35 608 | 2832 | 4 / 12 / 12.6 / 40 | **12.6×** |
| mars_city1 | 42 700 | 3546 | — / 12 / 12.0 / — | 12.0× |
| hell1 | 54 000 | 4463 | — / 12 / 12.1 / — | 12.1× |

The median brush is a box: 6 faces, 12 tris → 1 hull. Mean 6.1 faces/brush.

**Over-shadow of a brush hull, measured (`measure_brush.py`, 250 sampled visible brushes, shipped kernel, disc
light swR=9 on the thin axis, ~1000 fragments each):** rep0 = the brush's triangulated faces, rep1 = the
`ConvexHull` of its corners. **worst |Δcoverage| = 0.000000, mean gate-clean fraction = 1.0000.** The hull *is*
the brush; the substitution is bit-exact. This is the whole thesis, confirmed on real level geometry.

## 2 — Compiled world (`.proc`): what the engine actually casts, and whether the linkage survives

The engine casts world shadows from the **compiled** surfaces, not raw brushes. `erebus1.bproc` is a stock-BFG
binary proc (magic `PRO\1`, 559 entries; each model is a `WriteBinaryModel` v108 blob). Parsed the container
and every model (`proc.py`): **417 render models parsed end-to-end, stopping cleanly at the `interAreaPortals`
entry** (any byte drift would have corrupted the next entry — strong validation), **355 carry geometry**,
vertex bounds match the stored surface bounds. All 36 `_area0.._area35` worldspawn models are contiguous and
captured before the portals block, so the world caster set is complete.

**World caster triangles = 78 270**, decomposing by source:

| source | models | tris | share |
|---|---:|---:|---:|
| **`_area*` (worldspawn BSP)** | 36 | **54 677** | **69.9 %** |
| `func_static` (inline brush entities) | 240 | 19 434 | 24.8 % |
| movers / doors / lifts / bricks / misc | 79 | 4 159 | 5.3 % |

**(a) Brush→`.proc` linkage — does it survive? NO.** The `_area` models are grouped **by material, per BSP
leaf**; they carry no brush or primitive id, and dmap has CSG'd away every interior face, T-junction-split the
rest along BSP planes, and coplanar-merged what it could. `func_static` entities survive as one inline model
each (grouped by entity, not brush). There is no per-brush handle left in the compiled world — so a load-time
"group by brush id" is impossible.

**(b) Convex decomposition on `.proc` tris — the load-time fallback.** With no solids and no ids, the only
lossless convex subsets recoverable at load time are **coplanar** planar regions (merging non-coplanar surface
shells into a hull fills the interior and over-shadows — the exact model-study penumbra failure). Grouping the
54 677 area tris by plane and hulling each group (`measure_level.py`):

| coplanar merge on erebus1 area geometry | value |
|---|---:|
| distinct coplanar groups | 14 525 (naive 3.76× collapse) |
| groups already convex (union == hull, lossless) | 11 480 (79 %) |
| groups non-convex (hull fills a notch/hole → leak) | 3 045 (21 %) |
| measured gate-clean groups (≥98 % frags within quantum), n=400 | 68 % |
| worst |Δcoverage| on a non-convex planar region | **0.979** |
| **lossless collapse** (convex groups → 1 poly, non-convex → keep exact tris) | **1.16×** |

The lossless load-time collapse is a dismal **1.16×**: the 21 % non-convex groups are exactly the tri-heavy
floors and walls (the convex 79 % are mostly already single tris), so almost all tris survive. **BSP
compilation has already spent the convexity.**

**Brush-recovery collapse (the ideal, measurable now against the compiled tri count):** 54 677 worldspawn area
tris ÷ 2431 worldspawn visible brushes = **22.5× collapse to lossless convex solids** (14.6× mars_city1, 12.9×
hell1). This is only available where the brushes still exist — i.e. inside dmap, before CSG.

**Patches stay exact.** The 2352 Bezier patches are genuinely curved / non-convex (the level-geometry analogue
of the model study's open frames): they are KEEP-TRIS, not merge candidates.

## 3 — Heavy-cap relevance: does world merge help the captured corpus?

The binary `.cap`/`.softcap` corpus is gitignored and local-only; it is **not on this disk** (only the `.png`
columns remain in `base/cap/`), so the world/dynamic soft-record split is quoted from the engine source rather
than re-measured here.

From `neo/renderer/RenderCapture.cpp`:

- The shipped bench (`com_softShadowGateBenchReplay`, default on) reconstructs the caster scene from the
  `.cap`'s **deduped captured casters — the exact live soft-caster set, dynamic objects included** — and
  **excludes the reloaded worldspawn from soft-casting because its faces are already in the captured set**
  (line 3435-3448). So worldspawn *is* represented in the captured soft records; it is merely not double-added.
- The documented magnitude (line 2737-2743): **erebus1 cap0061 = 114 k soft-records in-game with static model
  entities vs 56 k with bare worldspawn** — i.e. worldspawn silhouette geometry is ≈ 49 % of that frame's
  soft-record stream, static model props ≈ 51 %, with dynamic gibs/movers adding more in live play.

**Honest read:** world/level geometry is roughly **half** the shipped soft-cost on a representative erebus1
capture, not a live-only concern — the captured corpus already carries it. A convex-solid merge of worldspawn
collapses not just triangles but **silhouette edges** (a convex hull presents one convex silhouette loop to any
light, versus the many coplanar-split edges of the BSP-shattered surface), which is what the soft walk actually
pays for. So the lever bites on the ~half of the soft stream that is world geometry, on the exact benchmark
frames — while the dynamic props/monsters (the model-hull lever's turf) own the other half.

## The heuristic (recommended: dmap brush-recovery)

**Where it runs:** **dmap-time**, in the map compiler (`neo/tools/compilers/dmap`), where each brush still
exists as a `uBrush_t`/`primitive_t` with its face windings *before* CSG and BSP-splitting. This is the only
stage that has the convex solids intact.

**What it does:**
1. For every worldspawn/func_static brush whose material set is shadow-casting, take the brush's corner points
   (already computed as face windings) → that *is* the convex hull, no hull algorithm needed.
2. Clip the hull to each BSP area it touches (intersection of a convex solid with area half-spaces stays
   convex) and tag it with the area number, so the runtime culls hulls per area exactly like area surfaces.
3. Bezier patches and any brush that fails the shadow-material test are left as exact tris (KEEP-TRIS).

**Data emitted:** a new per-area side-list in the `.proc`/`.bproc` — per convex subset: `{ areaNum, N hull
vertices }` (optionally the face planes for the fill). ~2431 hulls for erebus1, a few thousand corner points →
under ~100 KB/map. No per-frame state, no cache, nothing to warm (strictly better than the surf-cache that
never paid).

**How it feeds the fill:** at caster build each hull → the existing convex-polygon primitive
`SoftScan_FillBox` / `SoftScan_FillPoly`, proven bit-identical to walking the hull's triangle union
(`SoftShadowFillBox_test`). **No new fill primitive, no new cull predicate** — same integration point the model
study specified.

**Cost:** dmap already enumerates and windings every brush; emitting the corner list is negligible added
compile time. Zero runtime/load cost beyond reading a small chunk. Expected collapse: **~12–22× on worldspawn
caster triangles**, plus a silhouette-edge collapse on the ~half of the captured soft stream that is world
geometry.

**Load-time fallback (only if dmap must not be touched):** coplanar convex-region merge on `.proc` — but it is
lossless at just **1.16×** (§2b) because compilation already spent the convexity. Documented for completeness;
not recommended.

## GO / NO-GO

- **GO** to preprocess the level into convex subsets for hull-replacement — **as a dmap brush-recovery pass.**
  Brushes are convex by construction, the hull is the brush, and the shipped kernel measures the substitution
  as bit-exact (worst |Δ| = 0.000000 / 250 brushes). ~12–22× lossless caster-triangle collapse, zero runtime
  cost.
- **NO-GO** on a load-time `.proc` convex-decomposition. The compiled world has no brush linkage and no solids;
  the best lossless recovery is coplanar-only and collapses a mere 1.16×.
- **Bigger lever than model hulls on the heavy caps? Comparable, and complementary.** World geometry is ≈ half
  the captured soft-record stream (cap0061: 56 k of 114 k) and collapses 12–22×; dynamic props/monsters are the
  other half and are the model-hull study's target. The two merges partition the shipped soft cost between them
  — brush-recovery claims the world half at dmap-time for free, model hulls claim the dynamic half at
  caster-build. Ship both.

---
*Parse fidelity: `.bproc` container + BRM v108 model reader validated by (a) stored-vs-vertex bounds match on
355 models, (b) end-to-end consumption stopping exactly at `interAreaPortals`, (c) contiguous `_area0.._area35`.
Brush polyhedra: 3742/3742 built, box-brush half-space sign verified. Coverage: shipped
`softwedge_coverage.inc.hlsl`, SW_SCANLINE / SW_SCAN_CHORDS 16, unchanged from the model study.*
