# Disk vs Cylinder emitter model — engineering evaluation

Branch: `analytical-penumbra`. Read-only analysis, no code changes.
Question: would changing the analytic soft-shadow source model from a disk to a
cylinder (squat cylinder emitter) HELP or HINDER performance, and what happens
to accuracy? Owner invariants: physically accurate, early-outs inside the
integral math, no special-case simplified paths.

# Verdict

- **Variant (a) — cylinder axis aligned to the fragment→light line: NEUTRAL as
  a flat model, HINDER if made exact.** Viewed end-on, a cylinder's silhouette
  is exactly the disk the code already scans, so the flat version is a no-op;
  the only new physics is axial depth-parallax, and modelling that exactly
  destroys the one property the whole pipeline is built on (planar source ⇒
  exact 1D chord union). Structural cost, no visual gain. Don't do it.
- **Variant (b) — world-oriented cylinder rendered as its flat "stadium"
  silhouette: HELPS accuracy for tube fixtures, perf cost bounded but with one
  real register-pressure risk.** The FillTri/chord-union machinery survives
  almost verbatim; the disk enters the hot path in only four places, all
  generalizable. The dominant cost is not ALU — it is that the compile-time
  disk mask becomes a per-fragment runtime mask in a kernel with a documented
  history of VGPR-spill cliffs, plus the truth oracle and gate corpus are
  defined against a disk and must be re-derived.
- **Exact volumetric cylinder (either variant): HINDER, structurally.** The
  Fubini identity used here — area(source ∩ ⋃ projected tris) =
  ∫ 1D-interval-union dy — is exact only for a **planar** source with a single
  central projection. An extended-depth emitter has no single projection plane;
  exactness requires a depth dimension (slices), multiplying fill cost and grid
  registers by the slice count. This is the one place the owner's "no lossy
  shortcuts" and "no structural blow-up" pull in opposite directions, and it is
  inherent, not an implementation artifact.

# Supporting analysis

## 1. What the disk costs today, and where "disk" is actually baked in

The frame (`neo/shaders/builtin/lighting/softwedge_coverage.inc.hlsl:291-301`,
`SoftShadow_Frame`) always orients the source plane perpendicular to
fragment→light — i.e. the current model is a **spherical omni emitter**
approximated by its silhouette disk. Radius is one scalar: `g_lightR.w`
(`neo/renderer/Passes/SoftShadowTermPass.cpp:27, 558-561`), authored per-light
or derived as `min(lightRadius.xyz) * autoScale`, capped at 128
(`neo/renderer/tr_frontend_addmodels.cpp:72-99`). No emitter orientation exists
anywhere in the data path.

`SoftScan_FillTri` (`softwedge_coverage.inc.hlsl:1040-1202`) per triangle:
3 dot products + depth-slab test, central projection of 3 (rarely up to 5
clipped) verts at scale `distPL/dn` (`:318-322`), exact tri-vs-disk reject
(`:1066`), already-covered skip (`:1116-1165`), then a per-edge-hoisted chord
sweep (one FMA per edge per spanned chord, `:1169-1201`). Disk-shape
dependence is confined to exactly four spots:

1. isotropic normalization `invR = 1/swR` (`:1047`);
2. the exact cull `SoftScan_TriDisk`/`SegDisk` (unit circle, `:1018-1035`) and
   the bbox-vs-circle reject (`:1105-1110`);
3. the compile-time chord tables `SW_SCAN_HC` + `SW_SCAN_MASK` +
   `SW_SCAN_DISKBITS` (`:854-932`);
4. the mask argument to `SoftScan_ReduceCov` (`:990-1009`) — which is
   **already parameterized** on the mask array.

Everything else — projection, slab clip, chord rasterization, envelope, skip —
is shape-agnostic: it fills a projected polygon into a 16x32 bit-grid and never
asks what the source outline is until reduction.

## 2. Per variant

**(a) Fragment-axis cylinder.** Silhouette from the fragment = circle of
radius r = the current disk. Flat version changes nothing (pointless). Exact
version: source points at depths `distPL ± h` need per-depth projections
(`distPL/dn` becomes `(distPL±h)/dn`), i.e. a 3D occupancy grid — chords x
columns x depth slices. Grid registers scale by slice count on a kernel that
historically sat at 256 VGPRs with 32-77 KB scratch/thread (MEASURED, comment
`:887-896`); fill cost scales likewise. Structural, >2x.

**(b) World-axis cylinder as flat stadium** (rectangle + two half-disk caps,
the Minkowski sum segment⊕disk). What each piece becomes:

- **Frame**: `v` = projected cylinder axis instead of the arbitrary up-cross
  (`:297-299`). Same ALU. Degenerates continuously to the disk as the projected
  axis length → 0 (view down the tube), so **disk = the L=0 case of the same
  path** — this satisfies the "no special-case simplified path" rule: one
  generalized path, not a fork.
- **Projection/normalization**: anisotropic `invR` (`float2` instead of scalar
  at `:1047,1063-1065`) — ~1 extra multiply per projected vertex, 3-5 per
  triangle. ESTIMATE: noise relative to the clip+sweep body.
- **Exact cull**: tri-vs-stadium = "segment-to-segment distance <= r" per edge
  instead of segment-to-point (`:1018-1035`). ESTIMATE ~2x the cull ALU; the
  cull is a small fraction of a fill.
- **1D interval-union along chords: fully survives.** The exactness property
  needs only a planar convex source region scanned by chords — nothing about
  circularity. Umbra >=99% early-out (`softterm.cs.hlsl:775, 837`) is a
  monotone-OR property, survives with a runtime `maskBits` denominator.
  Already-covered skip operates on the raw pre-mask grid (`:1116-1122`),
  mask-independent. Envelope reduction already takes the mask as an argument
  (`:990`). The analytic box path `SoftScan_FillBox` (`:1204+`) is
  silhouette-polygon-based, survives. Conservative cone/sphere culls
  (`:326-339`, `softterm.cs.hlsl:766-767`) work with the stadium circumradius
  `sqrt(L^2+r^2)` — conservative, still lossless.
- **The real cost — the mask.** `SW_SCAN_MASK`/`SW_SCAN_DISKBITS` were
  deliberately moved to compile-time because per-thread mask storage was
  convicted by shaderstats (MEASURED: 256 VGPRs, thousands of spilled
  registers, comment `:887-896`). A per-fragment stadium mask (aspect and
  orientation vary per fragment) = SW_SCAN_CHORDS runtime words ~= +16 VGPRs
  plus ~16 `SoftScan_Run` calls at frame setup (amortized once per fragment
  against a walk of ~80 surviving tris — the setup ALU is negligible, the
  **register residency is not**). Whether +16 VGPRs re-tips the kernel into
  scratch is the single decisive perf unknown; it is measurable with the
  existing shaderstats workflow and cannot be reasoned away. Alternative: keep
  the compile-time-mask variant as the L=0 shader permutation (the codebase's
  accepted mechanism — SW_SCAN_CHORDS already permutes this way, `:794-800`),
  so pure point lights pay nothing.
- **Chord count**: chords = penumbra level count (`:794-797`, plateaus at 8
  because exactness lives along the chord, `:789`). A tube's penumbra along its
  axis is physically wider (∝ L), so the ~16 levels spread over more screen
  pixels — banding risk grows with aspect ratio exactly as it would for a big
  disk. Compensating is expensive: MEASURED ~1.2 ms/chord linear (chord-count
  perf curve), so 16→32 ~= +19 ms on the heavy corpus; the 64-column A/B was
  already tried and reverted (bought 3/176 defects for doubled grid registers,
  `softscan_word.inc.hlsl:9-13`). ESTIMATE: tubes up to ~3:1 aspect hold at 16
  chords; long fluorescent fixtures may not.

## 3. Perf verdict

Variant (b) per-fill delta is **bounded, well under 2x** (ESTIMATE from the ALU
deltas above; the fill body is unchanged). No early-out breaks. The two items
that could make it structural are (i) the runtime-mask register cost in a
spill-cliff kernel — must be measured, and (ii) chord growth if long tubes
band — MEASURED slope makes that the expensive lever. Variant (a)-exact and any
volumetric-exact treatment are structural by construction.

## 4. Accuracy verdict

- **More correct with a cylinder**: the game's tube fixtures
  (fluorescent/strip fixtures throughout the Mars base and RoE maps). The disk
  gets their penumbra anisotropy — a first-order, visible effect (soft along
  the tube, sharp across it) — 100% wrong, since the auto-radius path collapses
  everything to `min(lightRadius.xyz)` (`tr_frontend_addmodels.cpp:91`). The
  flat-stadium error (ignored axial depth parallax) is second-order in
  extent/distance, **but** for a long tube close to a wall L is comparable to
  distPL and that error becomes first-order — near-field tube contact shadows
  would be approximate in a new way. The disk-for-sphere model already accepts
  exactly this class of error, just with R <= 128.
- **Worse with a cylinder**: every round emitter (bulbs, flames, plasma glows)
  — the current fragment-facing disk is the correct sphere silhouette
  (`:291-301`). So cylinder must be an additional per-light shape (axis +
  halfLength authored, one more cbuffer float4 through
  `SoftShadowTermPass.cpp:27`, `SoftTileBinPass.cpp:301`,
  `SoftShadowClassify.cpp:39`, the `RenderBackend.cpp` swParm sites), never a
  replacement.
- **The verification stack is disk-defined.** The float64 truth oracle samples
  16 Hammersley points **on a disk** and explicitly pins "RT area-light radius
  == analytic light-disk radius" (`neo/renderer/RenderCapture.cpp:2223-2264,
  1384-1405`). A cylinder model changes the definition of ground truth: the
  oracle needs stadium/cylinder sampling and the entire gate corpus (11 caps /
  76 lights) re-baselines. This is the largest non-shader cost of the change.

## 5. Surf-cache interaction

- **Grid mode** (`neo/shaders/builtin/lighting/softsurf_build.cs.hlsl:236-272`):
  builds the same Fubini grid via `SoftScan_FillTri` at the texel-centre frame
  — storage size unchanged (SW_SCAN_CHORDS words/texel), build cost unchanged
  plus one mask generation. The frame becomes axis-dependent but stays a
  deterministic function of (P, L, axis), so the validity *mechanism* is
  untouched; the known displaced-view serve approximation gains no new failure
  mode (the stadium frame moves with P exactly as the disk frame does).
- **Scalar-fold / reduced-set modes**: the fold classifier computes solo
  coverage with `SoftDisk_CircleTriArea`
  (`neo/shaders/builtin/lighting/softsurf_classify.inc.hlsl:55-67`) — the
  second, independent disk-specific primitive family (Green's-theorem
  polygon∩circle, `softwedge_coverage.inc.hlsl:138-281`, also used by the
  legacy wedge path). A stadium∩polygon closed form exists (straight edges +
  arc sectors of the two caps) but is a genuinely new, case-heavy derivation —
  the largest new math in the whole change.
- **Warm contents**: any model switch invalidates every cached F/grid, but the
  cache is a runtime request-driven structure, so that is a warm-up cost, not a
  format migration. `lightHasMoved`-style invalidation is unaffected (axis is
  static per light).

# Bottom line

Cylinder-as-flat-stadium (variant b, per-light opt-in, disk = L=0 of the same
path) is the only version worth considering: accuracy win is real and specific
(tube fixtures, first-order anisotropy), fill-cost delta is bounded, every
early-out survives, and the chord-union exactness — the system's core asset —
is preserved. Its go/no-go gates are (1) shaderstats on the +~16-VGPR runtime
mask, (2) banding at 16 chords on the longest in-game tube, (3) willingness to
extend the float64 oracle and re-baseline the gate. Variant (a) and any
volumetrically exact cylinder hinder: the former is a no-op dressed as a
feature, the latter forfeits the planar-Fubini exactness the whole pipeline is
built on.

---

# Tube-as-segment: two-extent penumbra reconstruction

Owner's reframe (2026-08-26): don't integrate a stadium at all. Model the tube
as a **line segment** A→B (the two ends of the fixture); get the umbra as the
**intersection** of the two endpoint point-light hard shadows, the lit set as
outside their **union**, and reconstruct the penumbra "between the two extents"
with **no per-triangle walk** — "the triangles don't even enter the picture,
relieving memory pressure." Evaluated below as a physicist, not a cheerleader.
Every number tagged MEASURED / DERIVED / ESTIMATE.

## 0. The one-line answer

The reframe splits cleanly into two different claims with **opposite verdicts**:

- **(S) Segment = the N=1 chord case of the existing Fubini machinery** — TRUE,
  exact, and a real but *bounded* speedup (the chord-sweep inner loop collapses
  16→1, storage 16→1 word). It still **walks the caster triangles**. This is a
  specialization of the current path, not an escape from it.
- **(T) Two hard-extent tests + a scalar blend, triangles never visited** — this
  is the "large-class win / relieves memory pressure" claim. It is **exact only
  for a single convex occluder silhouette**, and for the real Doom3 caster (a
  non-convex, multi-triangle silhouette — a marine, a pipe run, a railing) it
  **over-darkens the umbra and can violate the very monotonicity it assumes**.
  As a lossy approximation it is the already-catalogued point-like/hard-stencil
  hybrid (light-class-feasibility.txt) generalized to two volumes — and it is
  gated OUT by this project's "lossless = physically accurate" hard rule, same
  as PCSS was.

And the population that would pay for either is **near-zero as authored** (§3):
`light_radius` is the light's **falloff/attenuation box**, not emitter geometry,
and the emitter radius is deliberately `min(lightRadius.xyz)` — the *short*
axis (`tr_frontend_addmodels.cpp:91`, MEASURED). There is no tube-emitter datum
anywhere; the Mars-base "fluorescent tubes" are emissive **geometry**, and the
light entities near them are round/cubic falloff volumes.

## 1. Is segment penumbra analytic, or still a 1D walk? — derivation

Parametrise the source S(t) = A + t·(B−A), t∈[0,1], length L. Visibility of a
fragment P from S(t) is binary v(t)∈{0,1}. The soft term is exactly

    cov(P) = ∫₀¹ v(t) dt  =  (visible length of the segment) / L.

**Blocked set of one convex occluder O (P ∉ O).** The rays P→S(t) that strike
O form a **convex cone** (apex P, base the convex silhouette of O). A convex
cone meets the line A→B in **one interval** (or ∅). So the blocked parameter
set is a single interval [t₀,t₁]∩[0,1], and

    cov = 1 − |[t₀,t₁]∩[0,1]|              (single convex occluder)

is a **closed form** — its only inputs are t₀,t₁, the two projections onto the
segment of the occluder's two extreme tangent rays from P. **No walk, no grid.**
This is exactly the owner's intuition, and it is correct — *for a convex
silhouette.* [DERIVED]

**Multiple occluders, or one non-convex silhouette.** Each convex piece i
contributes its own interval [t₀ⁱ,t₁ⁱ]; the blocked set is their **union**:

    cov = 1 − |⋃ᵢ [t₀ⁱ,t₁ⁱ] ∩ [0,1]| / 1.   (general case)

That is a **1-D interval union along the segment** — precisely one chord of the
current disk Fubini, whose whole identity is `area = ∫ (1-D interval union
along chord y) dy` over ~8–16 chords (existing doc §1; fubini-scanline-union
memory). **A segment source is the disk-Fubini integrand evaluated at a single
chord** — the projected segment A→B is that chord. [DERIVED]

**So the precise boundary:**

| regime | cost | exact? |
|---|---|---|
| single convex occluder | 2 tangent projections, O(1) | yes, no walk |
| non-convex silhouette / ≥2 occluders | 1-D interval union over the contributing edges | yes, walks edges once, **1 chord** |

**Honest reduction vs the disk (regime = the real one, multi-triangle):** the
per-triangle **projection + cull + already-covered-skip stays** (existing doc
§1: FillTri does a fixed 3-vert central projection + tri-vs-source cull before
the chord sweep). Only the **chord-spanning inner loop** collapses: the disk
sweeps a triangle's edge across every chord it spans (≤16), the segment sweeps
it across **one**. So the speedup is on the chord-sweep term only:
`~16→1` on that term (DERIVED from the "one FMA per edge per spanned chord"
structure, existing doc §1), **not** on the ~80-survivor walk itself
(softshadow-walk-attribution: ~80 MT survivors/frag). Net per-fill delta is
therefore **bounded well under 16×** and is bracketed by how much of a fill is
projection/cull (unchanged) vs chord-sweep (16→1). MEASURED anchor:
r_softShadowScanChords is ~1.2 ms/chord roughly linear (chord-count-perf-curve),
which says the chord dimension is a **real and dominant** fraction — so 16→1 on
it is a **large** fraction of the term, ESTIMATE 3–8× per-fill on the heavy
caps, contingent on measurement, not the ~16× the "no triangles" framing
implies.

**Verdict Q1: the walk is fully eliminated ONLY for a single convex occluder.**
For everything the engine actually casts, it reduces to **single-chord Fubini** —
exact, a real speedup, but still a triangle walk. The owner's "triangles don't
enter the picture" holds in the convex-silhouette limit and nowhere else.

## 2. The two-extent hard-shadow mechanism — can it dodge the walk?

The machinery exists. Under the shipped soft config the frontend already builds
a **stencil shadow volume per light** (`tr_frontend_addmodels.cpp:1956-2080`,
MEASURED via light-class-feasibility §C), and a shadow-map/PCSS atlas path
exists (`ShadowAtlasPass`). Two endpoint hard shadows = two volumes or two
atlas lookups per fragment — cheap, no triangle walk. This is literally the
"point-like → hard stencil" class (light-class-feasibility Class 2) instantiated
**twice** (one per endpoint) instead of once.

**What two endpoint memberships can and cannot tell you.** Let a =
[blocked-from-A], b = [blocked-from-B], each one bit from one lookup:

- a∧b → candidate **umbra**; ¬a∧¬b → **lit**; a⊕b → **penumbra**. Correct as a
  three-way *region* label. [DERIVED]
- **But the penumbra term is a continuous value in (0,1)** — the visible
  fraction — set by *where* the occluder's shadow boundary crosses [A,B]. The
  two endpoint bits carry **2 bits total**; the crossing parameter is a real
  number they do not encode. A "scalar blend" of the two bits + endpoint depths
  is therefore an **interpolation heuristic** (a 1-tap PCSS-style penumbra),
  **not** the exact coverage. To recover the true crossing you must project the
  occluder tangent onto the segment — i.e. touch the occluder again. [DERIVED]

**Two exactness failures of the pure two-extent form, both from non-convexity:**

1. **Umbra over-darken.** True segment umbra = ∩_{all t} shadow(t). Since
   {A,B} ⊂ segment, ∩_{all t} ⊆ shadow(A)∩shadow(B): the endpoint intersection
   is a **superset** of the true umbra. It equals the true umbra **iff** the
   occluder's shadow sweeps monotonically in t (convex case). A concave
   silhouette or a mid-segment occluder that lets light through the middle while
   both ends are blocked → labelled umbra, **is** penumbra → over-darkened.
   [DERIVED]
2. **Monotonicity break.** The owner's "coverage transitions monotonically
   A→B" holds for one convex occluder (visible interval grows monotonically as
   the receiver leaves the shadow). A second occluder — or a concave pose
   (marine's arm + torso) — re-blocks a *middle* sub-interval as the receiver
   moves, giving a **non-monotone** term with a local dip. Exactly the
   `>1 interval` condition of §1. [DERIVED]

**Verdict Q2: the two-extent+blend is the hard-stencil hybrid doubled — cheap
and walk-free, but lossy for non-convex casters (over-dark umbra + non-monotone
penumbra).** By this project's own discipline (stencil-volumes-no-physical-
meaning: "no volume-based umbra/penumbra classification"; lossless-definition:
"lossless = physically accurate, full stop") it is **inadmissible as the shipped
term** for general casters, for the same reason PCSS and box-proxy were shelved.
It is admissible only where the caster is provably convex, which is not the
Doom3 caster.

## 3. Population + payoff — the decisive bound

Parsed erebus1 (220 lights, `scratchpad/erebus1_lights.json`, MEASURED):

- 161 cast (noshadows≠1); 152 non-fog casters. [MEASURED]
- **Elongation of the falloff box** (long axis / second axis of `light_radius`):
  87 near-cubic (<1.2), 52 mild (1.2–1.8), **11 in 1.8–3, and exactly 1 above 3**
  (`light_53808`, 512/460/1024). [MEASURED]
- The 12 elongated ones are **not round tubes**: several have all three axes
  distinct (96/64/208, 512/460/1024, 264/96/136 — cross-section aspect up to
  1.5), i.e. **oblong rooms**, not fluorescent bars. [MEASURED]

**The killer:** `light_radius` is the **attenuation/falloff cube**, not emitter
size, and `R_SoftPenumbraRadius` takes `min(lightRadius.xyz)`
(`tr_frontend_addmodels.cpp:91`, MEASURED) — the *short* axis. The engine
already collapses a long falloff box to a small round source **on purpose**.
There is **no tube-emitter primitive in the data** (light-class-feasibility §A
signal iii: emissive-surface linkage does not exist; the "texture" key is the
projection material, MEASURED). The Mars-base fluorescent fixtures are emissive
**func_static geometry**; the light entities beside them are round/cubic falloff.

**So the "large class of tube lights" this replaces does not exist as authored.**
To realise it you must **author** axis+halfLength per light (the same cbuffer
plumbing the stadium variant needs, existing doc §4) on a curated list — and the
generous ceiling is **12/152 = 7.9%** of casters (ESTIMATE, and their
cross-sections aren't round so a segment is a *poor* fit for most). Cross-check
against the term-cost lever: the heavy cinematic caps are dominated by **large
round fills**, not tubes (light-class-feasibility §B: cinematic zones are
texture-less fills, radii 64–496u). ESTIMATE: the tube-shaped share of heavy-cap
TERM cost is **low single-digit %**, not the "large chunk" the reframe assumes —
and it overlaps the *same* curation pool as the already-cheaper Class-1
(noshadows) tag, which removes those lights from the walk for **zero engine
code** (light-class-feasibility recommended experiment).

## 4. Accuracy vs the disk + oracle/gate cost

**More correct than the disk for a genuine tube** — same first-order anisotropy
win as the stadium (existing doc §4), and cheaper. The disk collapses the long
axis (`min(lightRadius.xyz)`), so for a real bar the segment is strictly better
physics along the axis.

**But the oracle is disk-defined and must be re-based.** MEASURED
(`RenderCapture.cpp:2223-2264`): `GateTruthVisibility` traces **16 Hammersley
disk samples** (equal-area radius `diskR`) in float64; the RT reference pins
"RT area-light radius == analytic light-disk radius" (`:1405`). A segment term
needs: (i) the float64 oracle to sample **along the segment** (trivial: 16
Hammersley on a line) and (ii) the RT reference area-light to become a
**segment/capsule** primitive (`r_rtShadowSoftRadius` → a line light — a **new
RT area-light shape**, moderate work, not a one-liner). Then the entire corpus
(11 caps / 76 lights, MEASURED per memory) **re-baselines** — identical
non-shader cost to the stadium change, and the largest line item.

**Failure modes (all trace to the same non-convexity / near-field root):**
- Near-field, tube subtends a large angle: the segment is a **1-D** model — it
  ignores the tube's cross-sectional thickness entirely, so *across* the axis it
  is **less** accurate than the disk (the r→0 limit of the stadium). Contact
  shadows under a close, fat tube read too sharp across-axis. [DERIVED]
- Occluder between the endpoints / concave silhouette → the §2 umbra over-dark
  and monotonicity break. The gate's GateGrain/EXTENT detectors would flag these
  as defects against a correct segment oracle — which is the *right* outcome, and
  is exactly why the lossy two-extent form (T) cannot pass.

## 5. Memory — the real kernel of the reframe

This is where the owner is pointing at something true. In the **walk-free (T)**
regime a tube light stores:

- 2 shadow extents (the stencil volumes **already built** each frame, or 2
  atlas tiles) + 1 per-light segment (axis+halfLength, one float4).
- **No per-triangle caster stream, no Fubini bit-grid, no surf-cache table.**

Quantify vs the disk walk it replaces (extent-cache-feasibility, MEASURED):
- surf-cache grid value cache: **176 MB–2 GB persistent** (128 B/slot × up to
  8M slots) — the structure currently **wedging the game load** (load-burst /
  churn bugs).
- classify grid: 4–32 KB/light **transient**.
- The (T) path stores **~16 B/light** (segment) + reuses volumes that exist
  anyway → **~0 net new**. [DERIVED]

**But**: this memory win is **not unique to the segment model** — it is the
generic property of *any* light that leaves the soft-walk path. Class-1
(noshadows) and Class-2 (hard stencil) already deliver it for **zero-to-two-site**
code and are already gate-strategised (light-class-feasibility §D). The segment
model's *marginal* memory advantage over just tagging those lights Class-2 is
that it keeps a **penumbra** (Class-2 is hard-edged). And that penumbra is
exactly the part that is **lossy for non-convex casters** (§2). In the exact
(S) 1-chord regime you still need the caster **edge stream** to compute
intervals — memory = disk-walk **minus 15/16 of the grid words**, not zero.

## 6. Verdict

**Not a large-class win as framed; it separates into one modest-but-real
specialization and one lossy hybrid already on the shelf.**

- **(S) Segment = single-chord Fubini**: exact, a bounded 3–8× per-fill ESTIMATE
  and a 16→1-word storage cut, but it **walks triangles** and needs authored
  tube emitters that **do not exist in the data** (§3). It is a *specialization*
  of the current path (chords=1 with a line source), not an escape from it. If
  ever built, build it as the L→∞ / degenerate-chord limit of the **stadium**
  variant (existing doc), so disk / stadium / segment are one monotone path, not
  three forks (no-special-case-perf-paths rule).
- **(T) Two-extent + blend, no triangles**: genuinely walk-free and memory-free,
  but **exact only for a convex occluder**; for the real Doom3 caster it
  **over-darkens the umbra and breaks monotonicity** — the same physical-meaning
  and lossless violations that shelved PCSS and the box-proxy. Admissible only as
  a curated **Class-2-with-soft-edge** approximation on provably-convex casters,
  gated by full-frame A/B, never as the general term.
- **Population caps the whole thing at ≤8% of casters, realistically low
  single-digit % of heavy-cap TERM**, and that pool **overlaps the Class-1
  noshadows curation** that removes the same lights for zero engine code.

The reframe's true kernel — "relieve memory pressure by not walking/caching
these lights" — is **real but already owned by the light-class Class-1/Class-2
work**, which achieves the same VRAM relief without the segment model's oracle
re-base or its non-convex accuracy debt. The segment adds a *soft* penumbra on
top of Class-2; that increment is worth it only for authored, provably-convex,
genuinely-elongated fixtures — a set that is currently **empty** and must be
curated into existence.

## 7. The single cheapest validating experiment (zero engine code)

The entire verdict pivots on one empirical question: **for a real Doom3 caster,
is the segment's blocked set a single interval (convex → (T) exact, "no
triangles" holds) or a union of intervals (→ collapses to 1-chord Fubini and
(T) is lossy)?** Answer it offline, no shader, no build:

Take one captured `.softcap` for a heavy cap, pick its most tube-like
light+caster (or synthesise a segment A→B across the light's long falloff axis),
and in a ~40-line python pass over the captured caster mesh compute, for a grid
of receiver fragments: (1) the **exact** segment coverage = length of the 1-D
interval **union** of every triangle's shadow on A→B (ground truth); (2) the
**two-extent** estimate = endpoint-intersection umbra + a linear blend across
a⊕b. Report per fragment: **number of blocked intervals** (1 ⇒ convex regime
holds; >1 ⇒ walk required) and **max |cov_exact − cov_2extent|** (the lossiness
of (T)).

- If most fragments are single-interval and the error is below the GateGrain
  quantum (~0.0625 MEASURED, extent-cache-feasibility §3): (T) is real for that
  caster class — proceed to author + oracle work.
- If (near-certain for marine/pipe/railing casters) fragments are multi-interval
  with error ≫ quantum: **(T) is refuted, the reframe collapses to single-chord
  Fubini (S)**, and the memory win is better captured by the zero-code Class-1
  noshadows curation instead.

One script over an existing fixture decides it before a line of engine code is
written — and it reuses the same interval-union primitive the shipping Fubini
scanline already computes, so the "ground truth" is the engine's own math at
chords→∞.
