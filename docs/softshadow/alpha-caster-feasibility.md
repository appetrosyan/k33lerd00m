# Alpha-aware soft-shadow casters — feasibility study

Read-only study, branch `analytical-penumbra`, 2026-08-26. No code changed.
Every quantitative claim is tagged **MEASURED** (read from code/data in this repo) or **ESTIMATE**.

---

## 1. Current-behavior verdict: what a burning corpse casts today

### The dissolve mechanism (ground truth for the whole study)

The burn is **per-texel alpha-TEST erosion, not a surface fade**. Canonical stage
(`base/materials/monster_vulgar.mtr:28-47`, same pattern in monster_bruiser / hunter / characters.mtr):

```
{   // burning corpse effect
    if parm7                            // only when dead
    blend  gl_zero, gl_one              // coverage mask, draws nothing
    noclamp map models/monsters/spectre/global_dis.tga
    alphaTest 0.05 + 0.3 * (time - parm7)
}
```

- Threshold rises linearly; texels whose dissolve-texture alpha falls below it become holes.
  Nominal full burn ≈ 3.2 s (threshold reaches 1.0 at `time − parm7 = 3.167`).
- `alphaTest` anywhere in a material forces `coverage = MC_PERFORATED`
  (`neo/renderer/Material.cpp:2111-2113`), independent of `parm7`.
- `SurfaceCastsShadow()` ignores coverage entirely — it is only `MF_FORCESHADOWS / !MF_NOSHADOWS`
  (`neo/renderer/Material.h:577-580`). **A perforated surface casts as a fully solid mesh in every
  shadow path**, including the soft walk (raw triangles streamed by `R_CollectPenumbraFaces`,
  `neo/renderer/Interaction.cpp:747`; consumed opaque by the walk).
- `MC_TRANSLUCENT` auto-sets `MF_NOSHADOWS` (`Material.cpp:3137-3139`) — **glass never enters any
  caster stream today**. "Glass shadows a little less" is currently "glass shadows not at all".

### The death timeline (the fork, with `g_deathFX` on)

1. **Kill instant** — `idAI::Killed` (`neo/d3xp/ai/AI.cpp:4054-4063`): burn skin applied,
   `SHADERPARM_TIME_OF_DEATH` set **immediately**; `idAI::Think` then *rewinds* the clock each frame
   to slow the dissolve by `g_deathFXDissolveScale` (`AI.cpp:1218-1228`, scale clamps 0.05–1.0 → burn
   stretches up to ~63 s). `renderEntity.noShadow` is **not** touched here.
2. **Death anim window** — `state_Killed` waits for the "dead" anim action
   (`base/script/ai_monster_base.script:662-670`), typically 1–3 s (**ESTIMATE**). During this window
   the body is visibly dissolving (per-texel holes) **but casts its full, solid shadow** — the soft
   collect gate is only `SurfaceCastsShadow()` + `entityDef->parms.noShadow`
   (`neo/renderer/tr_frontend_addmodels.cpp:1885, 1937`, soft collect at `:2102-2145`).
   **Over-shadow bug window — the "holes still cast" accuracy bug is real, here.**
3. **`state_Dead` entry** — `preBurn()` runs *before* the burn wait
   (`ai_monster_base.script:677-685`) and does exactly one thing:
   `renderEntity.noShadow = true` — comment: *"for now this just turns shadows off"*
   (`neo/d3xp/ai/AI_events.cpp:2221-2228`). The whole corpse **stops casting in one frame**, honored
   by all paths (stencil `addmodels:1937`, warm collector `Interaction.cpp:1189`, fingerprint
   `Interaction.cpp:1407`). With the slowed fork dissolve the body stays largely solid and visible
   for seconds more while casting **nothing**. **Under-shadow pop — the inverse bug.**
4. Gibs never cast at all (`noShadow = true` at spawn, `neo/d3xp/AFEntity.cpp:1363-1365`).
5. **Debris is the pure form of the hypothesized bug**: `idExplodingBarrel` debris gets
   `SHADERPARM_TIME_OF_DEATH` with **no** `noShadow` (`neo/d3xp/Moveable.cpp:1339`) — dissolving and
   fully-dissolved debris chunks cast full opaque shadows until `EV_Remove`.

**Verdict**: the owner's premise is confirmed but two-sided. Today's model is a hard binary pop at
`state_Dead`: *before* it, dissolving geometry over-shadows (full solid caster, windows 1–2 and all
debris); *after* it, still-solid geometry under-shadows (zero caster, window 3). `noShadow` is the
legacy crutch standing in for exactly the alpha-driven behavior the proposal describes. Both defects
are invisible to the gate corpus (cinematic caps contain no monster deaths — see §5).

---

## 2. Class 1 — full cull before the shader

### Predicate (exact, no texture data needed)

A surface contributes **zero coverage** when its coverage-mask stage is active and its alphaTest
threshold ≥ 1.0 (max texel alpha is 1.0; every texel fails). CPU evaluation is already a solved
pattern: `R_SetupDrawSurfShader` on a scratch surf (existing usage `tr_frontend_addmodels.cpp:1824-1828`)
→ read `regs[stage->conditionRegister]` and `regs[stage->alphaTestRegister]`
(`Material.h:276, 280-281, 904-921`). Cost: one `EvaluateRegisters` per caster surface per frame,
O(tens of ops) — negligible (**ESTIMATE**). The predicate is expression-agnostic: it works for any
animated alphaTest, not just the burn table.

### Integration points

| Site | File:line | Needed? |
|---|---|---|
| Per-view soft collect (dynamic casters — monsters/debris live here) | `tr_frontend_addmodels.cpp:2102` (beside the `:1885` gate) | **Yes — the only live site for burn content** |
| Warm static collector | `Interaction.cpp:1242` | Share predicate for hygiene; static world never burns (collector skips non-world entities at `:1175-1178`) |
| Static fingerprint (must mirror warm) | `Interaction.cpp:1437` | Same — no-op in practice, keeps stream alignment |
| Tile bin / term CS | — | Nothing: culled records never reach the flatten (`:2976-3093`), so bins/records shrink automatically |

### Invalidation story

None needed. Dynamic casters are re-collected **every frame**; the caches fold static content only
(warm collector rejects non-world/non-soup entities `Interaction.cpp:1175-1178`; contributor cache
records *static* triangles only, `softterm.cs.hlsl:115`; surf-fold static prefix
`addmodels:2966-2994`). Alive→dead→gone and `Event_ClearBurn` (`AI_events.cpp:2247-2252`) resolve
naturally frame-to-frame. This is the payoff of the existing static/dynamic split.

### Payoff

Vs. today: **≈ 0 perf** for monsters (the `preBurn` pop already culls earlier and harder than the
predicate would) — class 1 alone is an *accuracy enabler*, not a win. Real perf effect: debris
chunks stop casting after dissolve-end instead of until `EV_Remove` (small; **ESTIMATE** a few
hundred tris per barrel event for ~seconds). Class 1's true role is to be the terminal state of
class 2 once `preBurn`'s `noShadow` crutch is retired.

---

## 3. Class 2 — intermediate alpha in the exact union integral

### The exact formula the machinery can compute

Physics: transmittance at source-disk point p is `T(p) = Π_layers (1−α_i)`;
`occ = 1 − (1/A)∫ T dA`. The bit-grid computes **unions**, not layer counts, so the general N-group
product is out of reach (overlap regions need `Π` per subset — inclusion–exclusion, 2^N grids).
What **is** exactly computable with unions, for opaque set `O` plus one alpha group `A` at uniform α:

```
occ = cov(O) + α · cov(A ∖ O)  =  cov(O) + α · ( cov(O ∪ A) − cov(O) )
```

Exact under the walk's existing single-count union convention (opaque overlap correctly saturates:
T=0 wherever O covers, regardless of alpha layers).

### The cheap implementation: snapshot, not a second grid

The prompt's two-grid AND-NOT form (`popcount(gridA & ~gridO)`) works but doubles the live grid
state — `swGrid[16] + swEnv[16]` is already ~48 registers across the whole walk
(`softwedge_coverage.inc.hlsl:1605-1608`), and the kernel has a documented spill cliff
(`:887-890`: "thousands of spilled registers, 32–77 KB scratch/thread"). SW_SCAN_BITS 64 was
refuted for exactly this cost shape (`softscan_word.inc.hlsl:9-13`, **MEASURED** history).

The identical quantity needs **no second grid**: the caster loop (`SoftShadow_FaceCoverage`,
`softwedge_coverage.inc.hlsl:1617`) walks records in flatten order. Order records
**opaque-first, alpha-last**; when the loop index crosses the partition boundary, snapshot
`covO = SoftScan_ReduceCov(grid, env, mask)` (`:990` — a pure function of current state, callable
mid-walk); finish the walk; final fold gives `cov(O∪A)`; return `covO + α·(covFinal − covO)`.

- Extra state: **2 scalars** (covO, α). No spill risk.
- Extra ALU: one `SoftScan_ReduceCov` (16 chords × ~15 ops ≈ 250 ALU, **ESTIMATE**) per fragment
  per light *only when an alpha batch exists*; the boundary index is wave-uniform (records are
  per-tile shared) so the no-alpha case is a single uniform compare — byte-identical output when
  the alpha batch is empty, which is the gate-friendly off-state this codebase requires.
- The legacy sampled path (`swMask` bit-union, `:1597`) snapshots a popcount the same way — the
  generalization covers both, no forked walker.
- Early-outs stay inside the integral: a full-disk early-out during the opaque phase yields occ=1
  exactly; skip-tests (`SwGridHas`) during the alpha phase are still correct because only the
  *marginal* union past covO matters.
- All four walk sites (term CS tile-list `:1872`, cluster-list `:2140`, unbinned `:2677`, plus the
  PS walker) share the `SW_FUNC` body and the C++ mirror build — one change generalizes all,
  including the CPU test oracle.

### Ordering and record plumbing

- Flatten already emits a **static prefix, dynamic suffix** (`addmodels:2966-2994`); alpha casters
  are dynamic, so the needed order is "dynamic-opaque before dynamic-alpha" — a second sort key at
  the existing `r_softShadowDepthOrder` reorder site (`addmodels:2508-2515`; make it
  (alphaClass, angular size)). Tile bins and cluster lists inherit flatten order.
- The caster record has no free field (`c1 = (firstTri, ±numTris, firstClu, numClu)`,
  `addmodels:2986-2989`), but the bounded design doesn't need one: **one alpha batch per light**
  = a per-light uniform pair (alphaStartRecord, α). K distinct alphas → a tiny constant array with
  cascade snapshots at each boundary, sorted α-descending:
  `occ = Σ_k α_k (cov_k − cov_{k−1})`, α₀ = 1 (opaque). Exact when groups don't overlap on the
  disk; in overlaps it attributes coverage to the larger α, which under-shadows relative to the
  true `1−Π(1−α)` but never below the darkest single layer — bounded, monotone, and the overlap
  case (two corpses aligned on one source disk from one fragment) is rare (**ESTIMATE**).
- Caches unaffected: contributor cache and surf-fold cache are static-only (see §2); the cached
  linear contribution stays opaque-pure, alpha rides the exact dynamic residual walk — consistent
  with the "cache the linear part, walk the non-linear residual" architecture.

### What α is, for burn content (the honest limitation)

Mid-burn the truth is per-texel binary, not uniform α. The correct scalar in expectation is the
**surviving-texel fraction**: `α(t) = 1 − CDF_mask(threshold(t))`, from a one-time histogram of the
dissolve texture's alpha channel at load (global_dis.tga & co; the only real new plumbing —
BFG's binarized image path needs a CPU readback hook for alphaTest-stage textures). Spatial error
remains (the shadow loses its holes' *positions*, keeps their *area*); exact at both endpoints,
which is where today's model is binarily wrong. Optional refinement for closed skins: a ray
crosses ≥2 faces, so `α_eff = 1−(1−α)²` — a calibration knob, not core.

- **Glass**: needs opt-in (translucent ⇒ `MF_NOSHADOWS` today, §1) — a material key
  (e.g. `softShadowAlpha <expr>`) that admits a translucent surface into the *soft* caster stream
  only. Uniform-α is *exactly* right for glass (no texel structure). Content + parser work; phase 2.
- **Grates/fences (perforated, mostly holes)**: a scalar is a poor model (structured holes at
  shadow scale). Keep them opaque-or-class-1 exactly as today — do **not** class-2 them.
  (Their over-shadowing today is a separate pre-existing approximation, unchanged by this work.)

---

## 4. Harness / oracle impact

- **Capture loses alpha class — confirmed limitation.** `capCaster_t` is bare world-space
  triangles, no material identity (`neo/renderer/RenderCapture.h:99-107`); bench replay dresses
  every caster in `_default` (`RenderCapture.cpp:1973`). Any class-2 gate/bench needs a
  `capCaster_t` alpha field (format version bump) written at capture time from the same CPU
  evaluation, and the replay spawner must carry it into collection (per-entity shaderParm or
  tagged material).
- **Class-1 oracle: trivial.** The cull runs at collection, before capture ever records the
  stream — oracle and shader see the same set by construction (shared predicate, nothing to do
  in `GateTruthVisibility`).
- **Class-2 oracle: doable, modest.** `GateTruthVisibility` (`RenderCapture.cpp:2226`) is
  16 Hammersley samples × binary any-hit. Change per sample: accumulate
  `t_s = Π_casters-hit (1−α_c)` with any-hit *per caster* (matching the walk's single-count
  convention); `vis = Σ t_s / 16`. ~20 lines once alpha is in the capture.
- Gate discipline holds: with no alpha batch present the walk is byte-identical (uniform-branch
  off-state), so the existing corpus stays green untouched — and it *will* be untouched, because:

## 5. Population & payoff

- **The bench corpus contains ~0 alpha-class casters** (**ESTIMATE**, high confidence): erebus1
  RoE intro cinematics — no monster deaths, no debris events; casters are world brushes + props +
  cinematic actors. The proposal is **gameplay-visible, benchmark-invisible**. Measuring it needs
  a combat capture (softShadowRecapture on a devmap fight), or it will be "measured" as zero.
- Combat population (**ESTIMATE**): one burning corpse ≈ 0.5–1.5 k light-facing walk-stream tris
  across its burn window (nominal 3.2 s; up to ~63 s at min dissolve scale); typical fight 1–5
  concurrent. Against ~80 MT survivors/frag typical walk load, per-corpse cost is real but
  bounded and transient. Note the sign: vs. today's early pop, class 2 *adds back* caster work
  during the burn — that is the price of the correct picture, terminated by class 1.
- Other beneficiaries: debris (bug fix, §1), glass/lenses (phase 2 opt-in), forcefields/energy
  surfaces if content opts them in. Not grates (§3).

## 6. Recommended first experiment

1. **Count before building** (one afternoon, no shader work): add a one-shot readback counter
   (existing `fe_softEdgesCollected` pattern, `addmodels:2127-2128`; no per-frame console output)
   splitting collected soft tris by "entity has `parm7 > 0`" at the per-view collect. Run a
   devmap combat bout + the corpus. Confirms §5's population estimate and gives the denominator
   for every later claim.
2. **Class-2 snapshot walk, minimal bounded form**: one alpha batch per light, per-light uniform
   (alphaStart, α), α from the register evaluation with the crude proxy `α = 1 − saturate(threshold)`
   (CDF histogram deferred — the proxy is exact at both endpoints, monotone between). Retire
   `Event_PreBurn`'s `noShadow` **only under the soft path's feature flag** so stencil behavior is
   untouched. Verify: gate byte-identical with the flag off and on-with-no-deaths; then a combat
   recapture with the versioned alpha-carrying format + the transmittance oracle (§4) as the new
   red-until-fixed property test.
3. Class-1 predicate lands as part of (2) — it is just the α→0 endpoint of the same evaluation.

Rationale for order: the snapshot design makes class 2 nearly as cheap to *build* as class 1, the
two share every integration point, and only class 2 answers the accuracy question the owner is
actually asking; the counter run keeps us honest about payoff before any of it ships.
