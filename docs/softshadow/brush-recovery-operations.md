# Brush-recovery soft shadows — operations & reversal

Human-readable record of what the brush-recovery feature changes, the `erebus1_hull` test
map, and **how to reverse every part of it**. Nothing here touches the `.resources` packs,
shipped map files, or `master`/`main` history — the feature is default-OFF and all test
artifacts are additive loose files.

## What the feature does
At dmap-time, each shadow-casting worldspawn brush (convex by construction) is emitted as one
convex hull into an additive `.proc` block `shadowHulls`. At load these hulls are read into a
per-area table; when `r_softShadowBrushHulls 1`, the worldspawn soft-shadow caster emits one
analytic hull record per hull (`.w=-N` encoding → `SoftScan_FillHull`) **instead of** that
area's triangle stream. Lossless (hull == brush projection), ~12–22× fewer primitives on the
worldspawn ~half of the soft-shadow cost, zero runtime cost.

## Code changes (all on branch `analytical-penumbra`, NOT master)
| commit | package |
|---|---|
| `e2165f15` | Phase-0 contract + study docs (`brush-recovery-implementation-plan.md`, `level-convex-subset-study.md`) |
| `36a57e0c` | A — dmap producer: emits the `shadowHulls` block (`dmap/output.cpp`) |
| `9d7dcb31` | B — renderer consumer: loads the block, casts worldspawn hulls, `r_softShadowBrushHulls` cvar (`RenderWorld_load.cpp`, `tr_frontend_addmodels.cpp`, `RenderSystem_init.cpp`, `SoftShadowHull.h`) |
| `6f7e9e49` | C — `SoftScan_FillHull` silhouette primitive (`softwedge_coverage.inc.hlsl`, `softterm.cs.hlsl`) |
| (uncommitted) | hard-light fix + census instrument (`tr_frontend_addmodels.cpp`) — bundled into a follow-up commit once measured |

Prerequisite already committed: `2e59c5fe` (`SoftScan_FillPoly` + coplanar foundation, which
`FillHull` subsumes).

## Runtime switch
- `r_softShadowBrushHulls` — default **0**. At 0 the entire feature is inert (the loader table
  stays empty for shipped maps that have no `shadowHulls` block, and the caster branch is dead).
  The shipped default path is byte-identical (verified: gate 0/76 with the feature off).

## The `erebus1_hull` test map (additive, loose)
Created for measurement only — the original `erebus1` is untouched. A new *name* is used so the
loader finds no packed `.bproc` and parses the fresh loose `.proc` directly (no cache to fight,
no engine load-path edit needed).

Artifacts written (all under the repo `base/`, all removable):
- `base/maps/game/erebus1_hull.map` — copy of the extracted `erebus1.map`
- `base/maps/game/erebus1_hull.proc` — dmap output, contains the `shadowHulls` block
- `base/generated/maps/game/erebus1_hull.bproc` — binary cache (only if `base/generated` is a
  writable dir; currently a broken self-symlink so this write fails harmlessly and the `.proc`
  is re-parsed each load)

## HOW TO REVERSE

**Disable the feature (no file changes):** it's already default-off. `r_softShadowBrushHulls 0`.
Shipped maps are unaffected regardless (no `shadowHulls` block in their `.proc`).

**Remove the test map (restore shipped-only state):**
```
rm -f base/maps/game/erebus1_hull.map base/maps/game/erebus1_hull.proc
rm -f base/generated/maps/game/erebus1_hull.bproc
```

**Revert the code** (branch-local; master never touched):
```
# revert the feature commits, newest first:
git revert 6f7e9e49 9d7dcb31 36a57e0c        # C, B, A
# (Phase-0 docs e2165f15 and the FillPoly foundation 2e59c5fe can stay — inert with the above gone)
# OR discard everything after the pre-feature point on the branch:
git reset --hard e000d151                     # ONLY if you intend to drop the whole branch's soft-shadow work
```
Prefer `git revert` (keeps history). The `reset --hard` is listed for completeness and is the
destructive option — do not use it unless you mean to drop the branch work.

**Untouched by all of the above:** the `.resources` packs, every shipped `.map`/`.proc`/`.bproc`,
the cap corpus, and `master`/`main`. The only writes outside git are the three additive loose
files above.
