# Extent / classification "cache" — feasibility study

HEAD 7af7a057, branch analytical-penumbra. Read-only. Tags: **[MEASURED]** = a number
already in tree/memory, **[DERIVED]** = arithmetic from those, **[ESTIMATE]** = my guess.

## TL;DR verdict

**The extent structure the owner describes already exists as a per-frame pass and is NOT a
cache: `SoftShadowClassify` (`r_softShadowClassify`).** It builds a per-light world-cell
umbra/lit/penumbra grid every frame at flatten, rides the existing vertex-cache joint buffer,
and has **none** of the seed/serve/warm/black-frame machinery that broke the grid cache. The
honest answer to the crux (Q4) is: **do not persist it — the extent is a pure function of
light+caster geometry that the frontend already recomputes each frame, so caching it buys
nothing and re-imports exactly the staleness/churn/load-burst bugs that convicted the grid
cache.** The novel deltas worth building are (a) the UMBRA class (spec'd "M2", not yet coded)
and (b) a smooth-vs-sharp penumbra split that drives the **already-plumbed but placeholder-only
VRS rate image** (`r_softShadowVRS 3`, "Milestone 2" is literally a `TODO` uniform fill today).
Ranked rec below: **fold into per-frame classify + VRS; drop the "cache" framing.**

---

## 1. Storage structure + bytes vs the grid cache

### Grid value cache (what we're comparing against) [MEASURED, from SoftShadowSurfCache.cpp + cvars]
- Hash table: `r_softShadowSurfCacheCap` default **1,048,576** slots × **8 uints (32 B)** = **32 MB**.
- Fubini bit-grid buffer (`GetGridBuffer`): `TableCap × swChords × 2 words × 4 B`. At default
  `r_softShadowScanChords 16` → **128 B/slot** → 1M × 128 B = **128 MB**. [DERIVED]
- Residual pool: `r_softShadowSurfCachePoolCap` 4M uints = **16 MB**.
- **Total default ≈ 176 MB persistent VRAM; max cap (8M slots, 32 chords) → ~2 GB.** [DERIVED]
  This is the "128 B/slot × millions of texels → GBs" the owner named. Confirmed exact.

### Extent structure (already in tree: `softClassifyGrid_t` + dense byte grid) [MEASURED, SoftShadowClassify.{h,cpp}]
- Header per light: `softClassifyGrid_t` = aabbMin[3], cellSize, dims[3], valid, nLit, nPen
  ≈ **40 B**.
- Class grid: **1 byte/cell today** (packed 4 cells/uint, 16/float4), only 2 bits of information
  used (LIT/PEN, UMBRA spec'd). Cell size `r_softShadowClassifyCell` default **32u**.
- A typical light volume ~512³ world units at 32u → 16³ = **4096 cells ≈ 4 KB/light**. [DERIVED]
  Budget ceiling `SW_CLASSIFY_MAX_CELLS = 4M` → 4 MB worst case, and the build *refuses and falls
  back to the full walk* past that (no unbounded alloc). [MEASURED]
- Storage medium: **appended to the vertex-cache joint buffer** and read via the existing
  `t_SoftEdges` binding — **no new buffer, no hash table, no key bijection**. [MEASURED, softterm.cs.hlsl:482-497]

**Verify the owner's "much simpler structure" claim: CONFIRMED, ~4 orders of magnitude.**
4–32 KB/light *transient* (rebuilt each frame, rides an existing buffer) vs 176 MB–2 GB
*persistent* dedicated VRAM. And it needs no serve/seed key at all because the lookup is a
plain 3D cell index into a per-light AABB (softterm.cs.hlsl:484-489), not an open-addressing
hash — that alone kills bug (b) ("fragile seed/serve key bijection, a dozen fixes").

**Smooth-region "fit" storage:** if you use hardware VRS block-constant reconstruction (below),
the smooth fit costs **zero extra bytes** — the 2-bit class *is* the shading rate; the hardware
broadcasts one shaded value across the 2×2/4×4 block. A bilinear-corner fit (if you ever want
better than block-constant) is 4×R16F = **8 B per (light, screen-tile)**, transient, ~64 KB/light
at 1440p/16px tiles. [DERIVED] Still negligible.

---

## 2. What extent info is already FREE

- **LIT extent: FREE, shipping.** `SoftShadowClassify` already emits it per-frame; the term CS
  skips provably-lit cells bit-exactly. Captures **81–86 % of walk WORK at 32u, 88–91 % at 16u**
  [MEASURED, cvar help + classify header]. Same conservative per-triangle cone cull as the tile bin.
- **UMBRA extent: SPEC'd, NOT built.** `SW_CLASS_UMBRA` is defined but the CPU build only ever
  writes LIT/PEN (SoftShadowClassify.cpp:100 writes PEN, else LIT; header calls UMBRA "M2").
  This is the first genuinely-new piece and it's small (add a "fully-occluded for every disk
  sample" test to the same cell loop). [MEASURED — gap confirmed]
- **Penumbra WIDTH per cell: derivable, not currently stored.** The per-light penumbra radius is
  already threaded (`R_SoftPenumbraRadius(vLight->lightDef)` at the build site;
  `g_lightR.w` disk radius in the shader), and the width law
  `w = R · (d_recv − d_occ)/d_occ` is already coded in the PCSS locator
  (interactionSM.ps.hlsl:435, `(dRecv−dBlk)/(1−dRecv)` form). The classify cull already computes
  the cone radius `coneR = swR·(cd+triRad)/dist` per cell×tri (SoftShadowClassify.cpp:92) — that
  cone radius **is** the local penumbra half-width. So the smooth/sharp split needs **no new
  spatial query**: it's a threshold on a quantity the lit/pen cull already evaluates. [DERIVED]
- **Tile-bin umbra-sentinel + empty-list (lit) tile classes: FREE at 16px screen granularity**
  [MEASURED, SoftTileBinPass.h + memory]. Coarser than the world-cell grid and screen-space
  (view-dependent), so it's a cheaper-but-blinder second source; the world-cell classify is the
  better extent oracle.

**Bottom line Q2: umbra/lit extent is ~free (one already ships, one is a small add to the same
loop); only the smooth/sharp split threshold + its validation is genuinely new work — and even
its input (cone half-width per cell) is already computed.**

---

## 3. Smooth-region representation + provable error bound

**Physical basis (owner's intuition, matches [[soft-shadow-subsampling]]):** the coverage term is
band-limited by penumbra width — gradient ~ 1/w, second derivative ~ 1/w². [MEASURED: the
flood-fill study found width-driven sampling is unbiased and **16–43× fewer evals, cheapest in the
widest penumbra** — the exact inversion the owner predicts.]

**Fit + bound (reuse the standard finite-difference remainder the VRS forensic derived):**
For a tile of screen-extent `h`, midpoint reconstruction (linear/bilinear or block-constant)
error is bounded by the **second difference** `D2 = f(x−h) − 2f(x) + f(x+h) ≈ h²·f''`:

    |interp error| ≤ |D2| / 8

Substituting `f'' ~ 1/w²`: `|D2| ~ (h/w)²`, so **error ≤ (h/w)²/8**. [DERIVED]
- Classify **SMOOTH** iff `(h/w)²/8 < ε` ⇔ **`w > h / sqrt(8ε)`** — a wide penumbra relative to
  the tile. This is exactly "wide w ⇒ low bandwidth ⇒ coarse is exact-to-the-eye", now with a
  number attached.
- **Tie ε to GateGrain, not a guess.** The banding quantum is 1/`chords` = 1/16 = **0.0625**
  [MEASURED]; GATE_STEP is 0.25 (4× the quantum, structurally sub-threshold — [[banding-root-cause-atlas-overflow]]).
  So the perceptually-safe ε is the *banding* threshold GateGrain enforces, ~**0.03–0.06**
  [ESTIMATE — set it empirically against GateGrain, §5]. With ε = 0.0625 and h = 16px:
  SMOOTH needs `w > 16/sqrt(0.5) ≈ 23px`. [DERIVED]
- **Robustness caveat already on record:** the flood-fill study found bilinear *overshoots
  0.10–0.15 at umbra/lit KINKS* — so the smooth class must **exclude cells adjacent to a class
  boundary** (any UMBRA or LIT neighbour ⇒ force PEN-sharp). The cone cull gives this for free:
  a smooth cell is one whose whole neighborhood is PEN with `w > threshold`. [MEASURED caveat]

**Reconstruction choice:** hardware VRS gives **block-constant** (rate-0) for free — sufficient
because SMOOTH is by construction where the second difference is below the quantum. A bilinear
reconstruct pass would tighten it but needs a separate pass; not needed for the first cut.

---

## 4. THE CRUX — persist vs recompute-per-frame

**Recommendation: recompute per frame. Do not cache. This is the whole win.**

Evidence:
1. **The extent is already recomputed per frame, cheaply, with zero cache machinery.**
   `SoftShadowClassify` (M1) rebuilds the dense grid *every frame* at flatten and rides the joint
   buffer. It has no hash, no seed, no serve key, no warm queue, no load burst, no invalidation
   generation — none of bugs (a)/(b)/(c). It is the existence proof that "compute-and-adapt per
   frame" works for this exact quantity. [MEASURED]
2. **The quantity is a pure function of light + caster geometry**, which the frontend already has
   in hand each frame (it's building the soft tri stream anyway). There is no reuse across frames
   to exploit except when geometry is static — and static geometry is *already cheap* to
   reclassify (the cull is the same cone test the tile bin runs regardless).
3. **Persisting re-imports every bug that killed the grid cache.** [[penumbra-tuning-features]]:
   *"187 CLEARs in one session (light 204: 76×) — the hash CHURNS with CAMERA motion."* A
   persisted classification keyed to world cells has the identical churn surface, plus the
   load-warm burst that black-screens the game (bug a), plus the key bijection (bug b). Recompute
   has *none* of these by construction.
4. **What persisting would buy — the per-frame classify build cost — is small and bounded** (CPU
   cone cull, `O(cells×tris)`, budget-capped at 4M cells → full-walk fallback). If that build ever
   shows up hot, the fix is to move it to a compute prepass (M2 note already says "hash grid" for
   the 20-40× empty-air waste), still per-frame, still no cross-frame cache.

**So: the extent/classification "cache" should not be a cache. It is `SoftShadowClassify` + two
additions (UMBRA class, smooth/sharp split), all per-frame. That sidesteps the entire broken
seed/serve/warm/black-frame apparatus — which is precisely the machinery the owner is trying to
escape.**

Comparison table:

| | grid value cache | per-frame VRS (no cache) | **extent = per-frame classify + VRS** |
|---|---|---|---|
| VRAM | 176 MB–2 GB persistent [M] | ~0 | 4–32 KB/light transient [D] |
| hit rate / coverage | 100 % on bench [M] | n/a | LIT 82–91 % free [M] + UMBRA + smooth |
| load burst / black frame | YES (bug a) [M] | none | **none** |
| key bijection fragility | YES (bug b) [M] | none | none (plain 3D index) |
| staleness under motion | churns/clears [M] | can't (recomputed) | **can't (recomputed)** |
| dynamic caster contact edge | stale until reclear | correct next frame | **correct next frame** |

---

## 5. Failure modes + gate strategy

- **Dynamic caster walks into a wide-soft region (sharp contact edge appears).** With
  *recompute-per-frame* this self-heals: the monster is in this frame's tri stream, so its cell
  reclassifies to PEN-sharp (small `w` at contact) automatically. **This is a second argument for
  recompute over persist** — a persisted "smooth" tile would miss the new contact edge for as long
  as it stayed cached. [DERIVED from M1 being per-frame]
- **Thin caster / ants / tripod inside a smooth tile.** The known hard case
  ([[banding-root-cause-atlas-overflow]] #4, #4-blocky). A thin caster near the receiver has small
  `w` (high bandwidth). Mitigation: the tile's classification must take the **min-w / max-bandwidth
  over the tile** (conservative) — *any* narrow-cone survivor forces PEN-sharp. The cone cull
  already visits every tri per cell, so the min is free. Do **not** average `w`.
- **Class-boundary overshoot (0.10–0.15).** Handled in §3: smooth excludes boundary-adjacent cells.
- **Temporal stability of the classification.** The class is derived from geometry, not from the
  high-frequency per-fragment rotation hash that causes the crawl ([[penumbra-tuning-features]]), so
  the classification itself is temporally stable as long as geometry is. Coarse-rate shading of a
  smooth region also *removes* per-pixel quantum flicker there (fewer distinct sample sets), which
  helps the crawl rather than hurting.
- **Gate arbitration.** [[banding-root-cause-atlas-overflow]]: the per-light gate is structurally
  blind to banding (one light always fits the atlas; GATE_STEP 0.25 ≫ quantum). So the
  smooth-threshold ε **must be validated against GateGrain** (the dense-region banding detector,
  [[fubini-endpoint-grain-fix]]), not the coarse defect gate. Set ε so GateGrain stays 0 on the
  wide-penumbra caps at the chosen rate — that empirically pins ε to the perceptual floor instead
  of the analytic (h/w)²/8, which is only an upper bound.

---

## 6. Verdict vs current state + the one decisive experiment

**Ranked recommendation:**

1. **BUILD (small): fold into the two passes that already exist.**
   - Add `SW_CLASS_UMBRA` to `SoftShadowClassify` (skip the walk, term saturated) — the spec'd-but-
     unwritten M2 piece.
   - Add the smooth/sharp split as a threshold on the cone half-width the cull already computes, and
     use it to **fill the VRS rate image** (`r_softShadowVRS 3`, "Milestone 2" — today
     RenderBackend.cpp:7811 is a placeholder uniform-2×2 `clearTextureUInt`; the plumbing to the
     draw is done). Coarse rate on SMOOTH world-cells, full rate on PEN-sharp/contact/boundary.
   This beats **both** rivals on the metric that matters: it's physically exact (fine walk wherever
   D2 is high, block-constant only where provably below the quantum), it ships without a warm burst
   or black frame (per-frame, rides existing buffers), and it costs KB not GB. Its big win is
   exactly the owner's thesis: **the stored structure is small enough that it needs no persistence,
   which is what dodges the load-burst bug currently wedging the game.**

2. **DROP the "cache" framing entirely.** There is no cross-frame state worth keeping; persisting
   reintroduces churn/clear/seed/serve/black-frame. Recompute per frame.

3. The grid *value* cache stays shelved (100 %-but-broken); per-frame VRS alone (uniform rate) is
   the fallback if the classify build ever proves too hot.

**Single cheapest decisive experiment (zero build, one cvar):**
The core claim is "wide penumbra ⇒ coarse is exact-to-the-eye." Test it on **existing code**:
run the gate **with GateGrain** on the three OPEN wide-penumbra heavy caps (cap0004/0007/0011,
the +2–8 % penumbra regressors from [[contribution-order-shipped]]) with **`r_softShadowVRS 2`**
(uniform 4×4 constant rate on the interaction PS — the dominant on-screen path, already wired at
RenderBackend_NVRHI.cpp:596). Read GateGrain + ms.
- If uniform 4×4 already keeps **GateGrain 0** and cuts ms on these wide-penumbra caps, the
  smooth-region claim holds *unconditionally* — and a width-**classified** rate (coarse only where
  `w > threshold`, fine elsewhere) is then strictly safer than the uniform test that already
  passed, so the whole "extent cache" collapses to Milestone-2 of an existing pass. [decisive]
- If uniform 4×4 trips GateGrain (expected on the contact/thin-caster caps), that failure **is** the
  smooth/sharp boundary the classifier must protect — and it tells you ε directly.

Either outcome answers the feasibility question in one run with no new code. Do it before writing
a line of the UMBRA/smooth-split work.
