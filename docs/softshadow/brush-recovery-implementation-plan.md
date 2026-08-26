# Brush-recovery soft shadows — implementation plan

Emit one **convex hull per shadow-casting brush at dmap-time**, store it in the `.proc`,
load it, and cast worldspawn soft shadows from the hulls (one `SoftScan_FillPoly` per hull)
instead of the world triangles. Lossless by construction (a brush is convex, hull == brush;
measured over-shadow 0.000000), ~12–22× fewer primitives on the worldspawn ~half of the soft
cost, zero runtime cost (pure preprocess). Study: `level-convex-subset-study.md`.

## Pipeline
```
dmap: brush → hull (winding verts), clip per BSP area, write lump
  → .proc `shadowHulls` block (new, additive) → load into per-area hull table
  → worldspawn soft-caster emits hull records (not area tris)  [tr_frontend_addmodels.cpp:2092]
  → term walk fills each hull via SoftScan_FillPoly            [softterm.cs / softwedge inc]
  → gate lossless (Replay-0, worldspawn included) + corpus bench
```

## Why it parallelizes
The stages couple through exactly **two interfaces**. Fix both first (Phase 0), then the
producer, consumer, shader, and tests are built independently against the contract, editing
**disjoint files**, and merged at the end.

---

## Phase 0 — CONTRACT (blocking; done first, committed)
Owner: main (not delegated — it is the interface every agent depends on).

**Interface 1 — `.proc` `shadowHulls` block (dmap writes ↔ loader reads).**
`.proc` is text (`output.cpp`). Add an ADDITIVE top-level block after the area models,
tolerated-missing so old `.proc` still loads (hull-less → triangle fallback):
```
shadowHulls { /* numAreas = */ N
  /* area */ 0 { /* numHulls = */ M
    /* hull */ { /* numVerts = */ K  ( x y z ) ( x y z ) ... }
    ...
  }
  ...
}
```
Bump the `.proc` version constant OR gate on block presence (prefer presence — no recompile
forced on unrelated maps).

**Interface 2 — caster-record encoding (CPU emit ↔ shader fill). ALREADY 90% BUILT by the
coplanar work (`2e59c5fe`).** `softterm.cs.hlsl:919-931`: an analytic caster is high-bit tagged
in its tile-list entry; the first record slot's `.w` discriminates **box (`.w>0` → 8 corners →
FillBox)** from **convex poly (`.w=-N` → N verts → FillPoly)**. A brush hull REUSES the `.w=-N`
record verbatim (N convex-hull verts in the stream). The ONE new piece: a brush is a 3-D convex
POLYHEDRON, not a flat face, so its fill is a silhouette, not a planar polygon. Define **`FillHull(verts,N)`
= project the N verts, take their 2-D convex hull (the silhouette), fill it** — which SUBSUMES
`FillPoly` (a flat face's projected silhouette is itself) and `FillBox` (a box is an 8-vert hull).
So `.w=-N` means "N-vertex convex hull → FillHull"; box (`.w>0`) stays for back-compat. Contract:
`FillHull` MUST reduce bit-exactly to `FillPoly` for coplanar input (the existing gate must not move).

**Deliverable:** a small committed header (`SoftShadowHull.h` or additions to an existing
soft-shadow header) defining the record layout constants + the in-memory `areaShadowHull_t`
struct, plus the `.proc` block grammar documented here. Everything below `#include`s it.

### `shadowHulls` block — AS IMPLEMENTED by package A (authoritative for the loader)
Written by `WriteShadowHulls` in `dmap/output.cpp`, appended once after the worldspawn's
`model`/`interAreaPortals`/`nodes` blocks (a top-level block, additive, presence-gated —
old `.proc` simply lacks it → triangle fallback). `/* … */` comments are decorative and MUST
be skipped by the parser (same convention as every other `.proc` block). Numbers are written
with dmap's `WriteFloat` (integers print with no decimal point). Exact grammar:
```
shadowHulls { /* numAreas = */ <N>

/* area */ <areaIndex> { /* numHulls = */ <M>
	{ /* numVerts = */ <K> ( x y z ) ( x y z ) ... }
	...          // M hull lines, K in [4 .. SW_POLY_MAX_VERTS(8)]
}                // one such area block per area, areaIndex = 0 .. N-1

}
```
- `N` = worldspawn `numAreas` (matches the `interAreaPortals`/`nodes` area numbering). An area
  with no qualifying hulls still emits its header line with `numHulls = 0`.
- Vertices are **world space**, worldspawn `originOffset` subtracted (= 0 for worldspawn); the
  full brush corner set (dedup of its face windings), i.e. hull == brush, lossless.
- **Caps enforced by the producer** (so the loader never sees a bad hull): a hull is emitted
  only if `4 <= K <= SW_POLY_MAX_VERTS`; a brush spanning more than 16 areas is dropped
  entirely. Dropped brushes fall to the triangle path.
- **COMPLETENESS RULE — the loader/B may rely on this.** Because B's per-area swap is
  *all-or-nothing* (an area with any hull uses hulls and drops its triangle stream), the
  producer emits hulls for an area **only if EVERY shadow-casting brush touching that area is
  hull-emittable**. If any brush in an area is dropped/capped, that whole area emits
  `numHulls = 0`, so B keeps the full triangle stream there and nothing is lost. Consequence:
  an area's hull list is either complete (all its shadow casters) or empty — a non-empty
  `shadowHulls` area is a guarantee that those hulls are the area's *entire* caster set. This
  trades coverage on mixed areas for guaranteed losslessness (non-negotiable for this feature).
- **Shadow-caster filter** (matches dmap's own `TriListForSide`): brush is `opaque`, not
  `CONTENTS_AREAPORTAL`, and has ≥1 side whose material `SurfaceCastsShadow()`.

---

## A — dmap producer  (parallel)
Files: `neo/tools/compilers/dmap/{map,ubrush,portals,output}.cpp`
- For each shadow-casting brush (skip `noshadows`/nodraw/portal/trigger surfaces), take its
  winding vertices as the hull (brush is already convex — the planes' intersection). Clip per
  BSP area (dmap already splits brushes across portals; emit one hull per (brush ∩ area)).
- Write the Phase-0 `shadowHulls` block in `output.cpp` alongside `WriteOutputSurfaces`.
- Cap: if a brush fragments into > T areas or a hull exceeds `SW_POLY_MAX_VERTS`, drop it to
  the triangle path (never emit a degenerate/oversized hull).
**Reuse:** brushes (`map.cpp`), area partition (`portals.cpp`), windings, `WriteFloat`.
**Test (self-contained):** `rbdmap` a tiny fixture `.map` (a few box + wedge brushes) → parse
the emitted `shadowHulls` → assert one hull per shadow brush, vert positions match the brush.
**Depends on:** Phase 0 only. Does NOT need the renderer.

## B — renderer consumer  (parallel)
Files: `neo/renderer/RenderWorld_load.cpp`, `neo/renderer/tr_frontend_addmodels.cpp:2092`
- Parse the `shadowHulls` block at `InitFromMap` into a per-area `idList<areaShadowHull_t>`.
- At the worldspawn soft-caster site (`:2092–2116`, where area tris go to
  `R_CollectPenumbraFaces`), when `r_softShadowBrushHulls` is on AND the area has hulls, emit
  hull records (Phase-0 encoding) instead of the area's triangles; else fall back to tris.
**Reuse:** the box emit in `Interaction.cpp` is the record-encoding template; the worldspawn
caster path is the exact swap point.
**Test (no dmap dep):** feed a **hand-written `shadowHulls` fixture** through the loader +
emit; assert the record stream matches the expected hull records.
**Depends on:** Phase 0 (both interfaces).

## C — shader fill: FillHull primitive  (parallel; the one genuinely new primitive)
Files: `neo/shaders/builtin/lighting/softwedge_coverage.inc.hlsl` (primitive), `softterm.cs.hlsl` (dispatch)
- Implement `SoftScan_FillHull(verts, N)` = project the N hull verts to the light plane, take
  their 2-D convex hull (the silhouette), fill it — reuse the `FillPoly` chord machinery on the
  silhouette loop. Route the `.w=-N` record to `FillHull` (at `:926-931`, replacing the flat
  `FillPoly` call). Keep the box `.w>0` path or migrate it to `FillHull` (a box is an 8-vert hull).
**Reuse:** `SoftScan_FillPoly` chord fill (committed `2e59c5fe`); the `.w=-N` decode already exists.
**Test:** extend `SoftShadowFillPoly_test` — (a) `FillHull` on a 3-D convex point set == the
triangle-union coverage of its hull, bit-exact; (b) REGRESSION: `FillHull` on coplanar input ==
`FillPoly` (the existing coplanar gate must not move).
**Depends on:** Phase 0 (record encoding). **This is the critical-path package** — A/B are plumbing;
C is the new math and the accuracy guarantee.

## D — verification  (parallel)
Files: `neo/tests/*`, `neo/tools/softshadow/*` (recipe/script)
- Unit test: brush-hull coverage == brush-triangles coverage, bit-exact (formalises the
  study's 0.000000) using the shipped interval-union math.
- **Re-gate recipe** (load-bearing): the current bench EXCLUDES worldspawn
  (`com_softShadowGateBenchReplay 0` includes it) — write the corpus gate + bench invocation
  that (a) recompiles a map via `rbdmap`, (b) gates with worldspawn included, (c) benches
  ON/OFF and reports per-cap ms.
**Depends on:** Phase 0 only (synthetic hulls) — fully independent of A/B/C.

---

## Dependency graph
```
Phase 0 (contract header, committed)
   ├─ A dmap producer      ┐
   ├─ B renderer consumer  │  parallel, disjoint files, worktree-isolated
   ├─ C shader fill        │
   └─ D verification       ┘
            ↓
Integration (main): merge A+B+C+D → rbdmap a real map → re-gate lossless (Replay-0) → corpus bench
```

## Cvar
`r_softShadowBrushHulls` (default 0). Default-off must be byte-identical (loader tolerates the
new block; caster ignores hulls). Feature-on: gate 0/76 (lossless) with worldspawn included.

## Risks (ranked)
1. **Gate/re-baseline (sequential tail).** dmap changes the `.proc`; the corpus references the
   old world and the bench excludes worldspawn — D's Replay-0 recipe + a re-capture of at least
   one map is required to see AND verify the win. Without it the lever is invisible to the gate.
2. **Area fragmentation / record budget.** A brush spanning many BSP areas fragments; cap
   fragments + hull verts (A drops oversized to tris) so the record count stays bounded.
3. **`.proc` compatibility.** Additive-block + presence-gate so unrelated maps need no recompile
   and old `.proc` still loads (hull-less → triangle path).
4. **Worldspawn caster identity.** Confirm the worldspawn faces at `:2092` correspond 1:1 to the
   dmap areas the hulls are keyed by (area index must match between dmap emit and load).
```
