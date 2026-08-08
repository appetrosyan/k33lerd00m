# Soft-shadow analytic coverage — per-step failure map

## FINAL STATE (supersedes historical sections below where they conflict)

Shipped pipeline (r_softShadowAAM 1, default): z-fail stencil CORE = solid umbra; capped watertight
SHELL volumes (softband.vs.hlsl side quads + cap fans) mark the penumbra ring by z-fail INVERT
PARITY of SOFTBAND_SHELL_BIT — sign-agnostic, camera-independent; coverage runs ONLY in the ring
(EQUAL RING_REF), cheap lit pass at LIT_REF, all counting around the clamp-safe SOFTBAND_CORE_BASE
(see neo/renderer/SoftShadowBand.h, the contract header shared with the SoftContract unit tests).
In-ring coverage carries two guards justified by the ring's centre-visible guarantee: winding
subtraction (illusory-umbra loops) and the physical-bound debris drop (|area| > 1.2 pi r^2 = clip
fragments of many-chain casters).

Verification: per-primitive unit tests + oracles; camera-sweep pipeline tests; ants/turds/holes/flip
detectors; full-frame ray-traced reference over the entire nine-pose v5 corpus (real GPU-baked
textures, real captured frame as comparison pane). USER verdict on this state: artifact-free in game,
but the penumbra is not visibly soft — reads like plain stencil shadows. The open frontier is
penumbra WIDTH/SHAPE fidelity in the ring (reference-test red: 0.1–5.2% gross band error, worst
erebus13 with 438 extraneous + 106 missing px), plus F6/F9 below.

Method: every intermediate of both implementations (planar light-disk coverage
`softwedge_coverage.inc.hlsl`; direction-space `SoftShadowDir.h`) was decomposed into a named pure
function and asserted by its own unit test against an oracle sharing no code with the step under
test (`SoftShadowPrimitives_test.cpp`). The oracles themselves were verified first against closed
forms and independent samplers. A golden corpus pins both implementations bit-exactly across
refactors. Suite policy: a proven-broken step keeps a RED test until its fix lands.

## Verdicts — implementation A (planar, the live shader)

| Step | Verdict | Evidence |
|---|---|---|
| `SoftDisk_Tri` / `SoftDisk_Sector` | PROVEN OK | closed forms; sign conventions |
| `SoftDisk_SegCircleRoots` | PROVEN OK | closed-form roots; t1<=t2 fuzz |
| `SoftDisk_CircleTriArea`, all 7 branches | WAS BROKEN → FIXED | branch-verified vs winding-weighted MC (worst 0.0016 vs disk area); **F4**: on 411/163843 rim-grazing in→out edges the exit root rounds outside [0,1] and the code substituted the ENTRY root — a point off the segment (100% of firings). Fix: clamp the correct root. Post-fix continuity worst dev 3e-5 |
| loop shoelace sum | PROVEN OK | convex / star / enclosing / hole / 3e5-coordinate loops vs MC |
| `SoftShadow_Frame` | PROVEN OK | orthonormality, orientation, both up-branches |
| `SoftShadow_ProjectVert` | PROVEN OK | similar-triangles closed form; projection invariance along ray |
| `SoftShadow_ClipSlab` | PROVEN OK | 12-case interval table + 3000-config depth-range fuzz |
| `SoftShadow_CullCaster` | PROVEN OK (conservative) | 1093 culled casters, worst un-culled occlusion 2.4e-8. Cone math exact (tan≥sin widens) |
| chain state machine | PROVEN OK | split/order invariance; **F10** pinch-vertex chain fusion proven BENIGN (zero-length connector degenerates); max-combine + early-break semantics |
| header protocol | WAS BROKEN → FIXED | **F14**: records before the first header were computed then silently discarded (engine offset bug ⇒ invisible missing shadows). Fix: `haveCaster` starts true |
| near-plane connector | PROVEN SAFE as implemented | review folklore wrong: both crossings scale from the same centre, the chord cannot re-enter the disk. Huge-coordinate overflow degrades into the correct sector branch (F7 probe: finite, correct) |
| **far-plane (light-plane) closure** | **BROKEN — RED (F6)** | caster piercing the light plane: live 0.975 vs truth 0.811 (missing shadow). The straight connector chord replaces the caster's light-plane cross-section, which the edge stream cannot express. Same architecture gap as the near-plane cross-section memo — needs a design decision |

## Verdicts — implementation B (direction-space)

| Step | Verdict | Evidence |
|---|---|---|
| `SphTriSolidAngle` | PROVEN OK | octant π/2, sign, small-triangle limit |
| `CapArcBasis` | PROVEN OK | omega/e2 properties, endpoint reconstruction, degenerate flags |
| `CapArcCrossings` | PROVEN OK | symmetric-dip closed form; crossings on the boundary; no-dip case |
| `CapTri` (sum over loops) | PROVEN OK | inside / enclosing / straddling / off-cap loops vs cap-sampled MC winding oracle |
| whole `DirOcclusion` (front hemisphere) | PROVEN OK | 60-loop fuzz vs MC, worst 0.0002 |
| **caster cull** | WAS BROKEN → FIXED | **F1/F2**: `skip` was dead code — no cull at all. A caster wholly behind the receiver winds the azimuth around the axis' antipodal piercing: occ = 1.0 where truth = 0. Fix: bounding-sphere behind/beyond cull from the header it used to discard. Residual: a caster PARTIALLY behind that encircles the backward axis is still wrong — full fix needs hemisphere disambiguation (design decision) |
| sector azimuth tie (F3) | NOT REPRODUCIBLE | edge through the axis: occ continuous through δ=±1e-5 (0.5001/0.5000/0.4999). The wrapped ±π sweep equals the winding limit |
| **antipodal edge guard** | WAS BROKEN → FIXED | **F11**: edge grazing the receiver (directions near-antipodal) returned 0, dropping up to half the cap — asymmetric probe read 0.5 where truth is 1.0. Fix: `CapTriWorld` splits the edge at its world midpoint (an identity on the direction path) until well-conditioned |

## Verdicts — oracles

| Oracle | Verdict | Evidence |
|---|---|---|
| `RayHitsBox` | WAS BROKEN → FIXED | **F8**: segment starting inside the solid reported unblocked (`tmin>1e-5`) — every contact-shadow truth was lit-biased. Fix: overlap of [tmin,tmax] with (0,1). Ripple: ground-standing fuzz failures 352→298; deep-umbra characterization 1.000→0.638 (the old numbers were partly oracle bias) |
| `TruthShadow` / sampling | PROVEN OK | half-plane = 0.5 exactly; circular-segment closed form; grid vs independent random sampler 0.6333/0.6331 |
| `Silhouette` | PROVEN OK for light outside | 400 loops: closed walks of box edges, no repeats, uniform winding (+400/−0), hull contains all corners. **F9** (light inside caster → empty loop) remains RED: the edge-stream format cannot express "light swallowed ⇒ full shadow"; engine has the same gap |
| `BuildCaster` | WAS BROKEN → FIXED | **F13**: header radius was the FULL AABB diagonal (2× oversized) — cull-sensitivity failures untestable by construction. Fix: half-diagonal (also in `AppendReceiverSilhouetteRecords`). Empty-loop garbage ±1e30 header removed |
| C++/GPU numerics parity | WAS BROKEN → FIXED | **F15**: unsuffixed double literals made tests validate kinder math than the GPU runs; shim `saturate`/`min`/`max` propagated NaN where D3D flushes (a GPU NaN = silent lit hole, C++ NaN = loud). Fix: f-suffixed literals throughout the .inc; shim NaN semantics = HLSL |

## Engine feed (verified on 7 committed v4 captures; erebus0/1 are v1-legacy, unreadable)

- Chains 100% closed, walk-ordered; within-chain adjacency bit-exact where chains continue.
- Header spheres bound their edges: 0 violations (engine sphere from edge-endpoint AABB is sound).
- Winding ~98% uniform per capture; the minority sign (7–26 chains/capture) is consistent with holes.
- Captured records byte-identical to the shader buffer (same `flat[]`, captured at flatten time).

## Still red / needs a design decision (not fixable locally)

1. **F6 far-plane cross-section** — as the near-plane memo predicted for dn≈0, the light-plane clip
   also needs the caster's cross-section wrap that the silhouette edge stream cannot express.
2. **F9 light-inside-caster** — empty silhouette ⇒ lit. Engine-side detection (caster exists, zero
   front faces ⇒ force occ 1) would fix it; touches `R_CollectPenumbraEdges`.
3. **F2 residual** — direction-space partial-behind casters encircling the backward axis;
   needs hemisphere disambiguation of the sector measure.
4. Architecture items already proven by the pre-existing suite: light-apex silhouette fed where the
   receiver-apex contour is needed (dominant error: 260 vs 12 / 1407); max-combine seam leak
   (0.5 at joints — per-entity sum is the proven fix); near-plane cross-section (deep umbra 0.638
   vs 0 target after oracle fix).

## Capture-level accuracy after the fixes (erebus2 / erebus5 / erebus13, vs mesh ray-cast truth)

| Metric | planar (shipped) | direction-space | dir + hard gate | hard-base + wedge ring |
|---|---|---|---|---|
| EXTRANEOUS (cov<0.5, truth>0.85) | 34 / 84 / 100 | **401 / 450 / 1040** | 0 / 0 / 0 | 2 / 3 / — |
| MISSING (cov>truth+0.35) | 151 / 97 / 246 | 59 / 24 / 55 | 59 / 24 / 55 | 0 / 0 / — |
| mean \|cov−truth\| | 0.166 / 0.176 / 0.146 | — | — | **0.014 / 0.008 / —** |

Readings:
- The direction-space extraneous EXPLOSION on real captures is the F2 residual live: world geometry
  partially behind the receiver encircling the backward axis is common, not exotic. Direction-space
  is unusable without either the hard gate or hemisphere disambiguation.
- The hard-shadow-base + wedge-penumbra-ring composite is an order of magnitude more accurate than
  either pure analytic method on real content (mean error 0.008–0.014 vs 0.15–0.18), with both
  failure directions ~0. Engine mapping: z-fail stencil core = solid umbra, coverage only on the
  shell band (3.7–8.2% of samples), lit elsewhere.
- The real softband gate as built leaves 15/30/40 extraneous (vs 34/84/100 ungated) — the idealized
  gate reaches 0, so the gap is band geometry, not the idea.

## Cost of the fixes

Shader permutation (LIGHT_POINT, SOFT_WEDGE): 515 → 535 SPIR-V arithmetic ops (+3.9%), from the F4
root clamps and F14 init change. Codegen verified with dxc + spirv-dis; the decomposition itself was
op-neutral (515 = 515 before the fixes).
