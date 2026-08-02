# Shadowing techniques beyond shadow-maps / RT / stencil volumes
### Survey + ranked plan for RBDOOM (dark, no-TAA, hard-edge-preferring, scRGB-FP16 HDR, RX 7900 XT)

*Two axes: **A** = technique families fundamentally distinct from the three already in-tree; **B** = variants/optimizations of stencil shadow volumes (the family being restored). Cited, adversarially fact-checked. Claims that failed verification are flagged.*

---

## Bottom line (ranked)

**Base hard-shadow implementation — pick from Axis B:**
1. **z-fail (Carmack's Reverse) with the Lengyel hybrid** — z-pass when the view is outside the volume, z-fail only when inside; scissor each point light to its projected illumination-radius box; depth-bounds test for Hi-Z. This is essentially what Doom 3 BFG shipped and the right default: robust, crisp, per-frame stable.
2. **ZP+ (Hornus 2005)** as an optimization over #1 *if* fill proves the bottleneck: keeps z-pass speed while fixing the camera-in-shadow defect (z-fail measured **up to 80 % slower** than z-pass; ZP+ **<10 %** slower).
3. **Per-triangle / alias-free shadow volumes (Sintorn–Olsson–Assarsson 2011)** — a forward-looking option: pixel-accurate hard shadows, no silhouette extraction, robust on triangle soup, beats z-fail. Bigger rewrite (compute-shader primitive test, not the fixed-function stencil path); revisit after the classic path works.

**Supplement — add one Axis-A family for what stencil misses:**
- **Screen-space contact shadows** (rank 1 supplement): cheap depth-buffer march that adds the fine contact detail coarse shadow-volume casters miss. **No-TAA caveat:** must run enough samples (or a *static* blue-noise/IGN dither, not a per-frame-animated one) or it bands/noises — the usual implementations hide undersampling with temporal noise + TAA, which is forbidden here. On a 7900 XT you can afford the higher sample count that makes it stable in a single frame.
- **POM / horizon self-shadowing** (rank 2 supplement): per-pixel height-field self-shadow for bump/greeble micro-detail, fully deterministic (no temporal needed). Only self-shadows a surface's own height field, not between objects.

**Dead-ends for this pipeline:** SDF cone shadows, voxel cone tracing, and all transmittance methods (deep/opacity/Fourier) are inherently **soft** and — for SDF/VCT — lean on temporal filtering; they can't serve as the crisp hard primary. Planar/blob projective shadows are too limited (planar receivers only). Neural/ray-guided sampled shadows need accumulation → incompatible with no-TAA. Details and the one case where a transmittance method *would* earn its place (particle/smoke volumes) below.

---

## Axis B — Stencil shadow-volume variants & optimizations

| Variant | What it fixes | Cost / fill | Robustness | Edge | Verdict here |
|---|---|---|---|---|---|
| **z-pass** (Crow / Heidmann) | baseline; O(s) silhouette tris, **no caps** | cheapest geometry | **fails when near-plane enters a volume** (shadows invert) | hard | building block, not safe alone |
| **z-fail / Carmack's Reverse** (Everitt & Kilgard 2002; Bilodeau–Songy) | camera-in-shadow correct; far-plane→∞ so never clipped | O(f) tris, more fill; needs **front+back caps** | correct for **all** scenes | hard | **robust base** |
| **ZP+** (Hornus et al. 2005) | keeps z-pass speed, fixes near-cap defect | ~**<10 %** over z-pass vs z-fail's **up to 80 %** | correct | hard | **best fill/robustness trade** |
| **Lengyel hybrid** | z-pass outside shadow, z-fail inside; **scissor to light radius** | cuts point-light fill by a constant factor | correct | hard | **the practical recipe** |
| **Per-triangle / alias-free** (Sintorn–Olsson–Assarsson 2011) | no silhouette extraction; whole-triangle primitive; trivial-accept/cull | beats z-fail; surpasses z-pass at hi-res | no capping / camera-in-shadow issues at all | **pixel-accurate hard** | strongest long-term, biggest rewrite |
| **CC shadow volumes / culling & clamping** (McGuire "Fast, Practical & Robust Shadows"; Lengyel) | removes volumes that are themselves shadowed / non-contributing; clamps to receiver regions | **1.87–5.26× fewer processed pixels** | — | hard | **apply as fill optimization** |
| **Penumbra wedges** (Assarsson & Akenine-Möller) | soft penumbrae on top of volumes | **>2× fill** (umbra + penumbra passes) | — | soft | skip — we want hard |

**Key correction (a common claim that is FALSE):** "shadow-volume geometry cannot be culled" is **refuted** by primary sources and by Doom 3 itself — CC shadow volumes, Lengyel scissor rectangles, and the `NV_depth_bounds_test` all cull shadow-volume fill; van Waveren's DOOM-3-BFG Technical Note documents per-light scissor + depth-bounds + a precise view-inside-volume test used specifically to *minimize* the slower z-fail path. So the "overdraw is uncullable" worry does not apply — plan on scissor + depth-bounds from the start.

**id/BFG specifics worth copying (van Waveren 2013):** shadow volumes are index sets over a static mesh, GPU-skinned just before rasterization; construction is stateless → fully parallel; a precise line-vs-expanded-triangle inside test decides z-pass vs z-fail per volume (z-fail is significantly slower, especially at high res); z-fail volumes are clipped in homogeneous clip space to get tight depth bounds for Hi-Z.

---

## Axis A — Distinct families (each vs our constraints)

Legend for the two constraints that decide fit here: **[HARD]** crisp stencil-like edges vs soft; **[no-TAA]** stable in a single frame vs needs temporal accumulation.

### Screen-space contact shadows — *supplement, rank 1*
- **Principle:** per-pixel, per-light march through the **depth buffer** toward the light; occluded if the ray passes behind stored depth.
- **Good at:** fine *contact* detail (object-meets-floor) that coarse casters miss. **Poor at:** anything not near-screen / off-screen occluders (must fade at screen edges); not a general shadow solution.
- **Cost:** cheap, scales with step count. **[HARD]** yes (binary). **[no-TAA]** ⚠ the standard trick is 8 samples + *animated* interleaved-gradient noise resolved by TAA → bands/noise in a single frame. Fix: more samples or a static blue-noise dither; affordable on a 7900 XT.
- **Dark-HDR:** fine — binary mask, no bright-sample issues.

### POM / horizon self-shadowing (Tatarchuk 2006) — *supplement, rank 2*
- **Principle:** during the parallax ray-march, cast a secondary ray in light space and do horizon-visibility queries against the height field.
- **Good at:** a surface's own bump/groove **self-shadowing** at texel scale. **Poor at:** inter-object shadows (self-shadow only); only where N·L>0.
- **Cost:** ~free (one PS pass). **[HARD]** base mode is hard — but *aliased/jagged* (the "crisp" framing is optimistic; a soft filtered variant exists). **[no-TAA]** ✔ fully deterministic, no accumulation — its artifacts are spatial (more samples fix them), not temporal.

### Signed-distance-field cone shadows (Unreal MDF) — *dead-end as primary; niche supplement*
- **Principle:** trace a ray through per-mesh SDFs; track closest passing distance to approximate a cone → soft area shadow at ~ray cost.
- **Good at:** cheap large-scale **soft** occlusion, graceful in the distance, few self-intersection/leak problems. **Poor at:** hard edges.
- **[HARD]** ✘ inherently soft (contact-hardening). **[no-TAA]** ✘ Epic's own docs: half-res + "TAA does a good job of helping reduce the flickering"; MDFs are **offline-generated** (no runtime build). Verdict: wrong shape *and* leans on the one thing we forbid.

### Voxel cone tracing (Crassin 2011) — *dead-end here*
- Sparse-octree scene, cone-trace visibility/AO. Soft, resolution-limited, 25–70 fps-era cost, revoxelization overhead. **[HARD]** ✘ **[no-TAA]** shaky. Overkill for hard shadows; its real use is GI/AO.

### Transmittance shadows — deep shadow maps / opacity shadow maps / deep opacity / Fourier opacity — *only for volumes*
- **Principle:** store a per-light-ray **transmittance** function (layered alpha / Fourier coeffs) so semi-transparent occluders attenuate light gradually.
  - Opacity Shadow Maps (Kim & Neumann 2001): planar alpha slices → banding.
  - Deep Opacity Maps (Yuksel & Keyser 2008): layers shaped to a depth map → artifact-free at ~3 layers, real-time.
  - Fourier Opacity Mapping (Jansen & Bavoil 2010, shipped in *Batman: Arkham Asylum*): Fourier coeffs, smooth media; **rings** on sharp opacity steps.
  - Deep Shadow Maps (Lokovic & Veach): high quality but **offline / unbounded memory**.
- **Good at:** hair/fur/smoke/foliage self-shadowing — the transmittance class stencil volumes and depth maps **cannot** do. **Poor at:** opaque hard shadows. **[HARD]** ✘ soft by construction.
- **Verdict:** irrelevant to the opaque hard-shadow goal — *unless* you later add volumetric/particle smoke lit by these lights, in which case FOM/deep-opacity is the *only* family that shadows it correctly. Keep in back pocket.

### Projected planar shadows (Blinn 1988) / blob & projected-texture imposters — *dead-end*
- Flatten caster to a plane via a projection matrix (planar receivers only; anti-shadow when light sits between caster and plane; no self-shadow), or project a caster silhouette texture (blob). Cheap, hard on the plane, but far too limited for a general interior scene. Historical interest only.

### Horizon-map SH self-shadowing (Max 1988 / Sloan; Snyder & Nowrouzezahrai 2008) & PRT baked visibility — *dead-end here*
- Precomputed order-4 **spherical-harmonic** visibility → inherently **low-frequency soft**, static geometry only. Cannot make hard edges; incompatible with fully dynamic id-Tech lighting. Baked visibility (lightmap/PRT) is the classic "static shadows for free" family but is soft + static.

### Neural / learned / ray-guided visibility — *dead-end here*
- Sampled/stochastic → relies on accumulation. Blue noise helps per-frame but still "improves markedly with accumulation" → violates the no-TAA constraint unless sample counts are pushed to noise-free, at which point cheaper deterministic methods win.

---

## Why the ranking lands where it does (for *this* pipeline)
- **Hard-edge preference** eliminates every inherently-soft family (SDF, VCT, transmittance, SH/PRT, penumbra wedges) as the *primary* — they're supplements or wrong-tool.
- **No-TAA** additionally knocks out the soft families that *specifically* lean on temporal filtering (SDF per Epic; screen-space's animated-noise trick) unless reconfigured for single-frame stability.
- **Dark scRGB-FP16 HDR** is friendly to all binary-mask methods (no bright-sample resolve issues); it mainly argues against anything that would *blur* a sparse bright highlight's shadow.
- **RX 7900 XT headroom** is the enabler: it makes stencil-volume fill a non-issue (with scissor + depth-bounds) and lets contact shadows run enough samples to be single-frame-stable.

---

## Sources (primary unless noted)
- Everitt & Kilgard, *Practical and Robust Stenciled Shadow Volumes* (NVIDIA 2002) + CEDEC/GDC slide decks — z-fail, robust capping, far-plane→∞, two-sided stencil.
- Hornus, Hoberock, Lefebvre, Hart, *ZP+: Correct Z-pass Stencil Shadows* (I3D 2005).
- Sintorn, Olsson & Assarsson, *An Efficient Alias-Free Shadow Algorithm using Per-Triangle Shadow Volumes* (SIGGRAPH Asia 2011, ACM TOG 30(6)).
- McGuire, Hornus, Hughes, Everitt, Kilgard, *Fast, Practical and Robust Shadows* / CC Shadow Volumes (2003); Lengyel scissor + depth-bounds.
- van Waveren, *DOOM 3 BFG Technical Note* (id Software, 2013).
- Epic, *Contact Shadows* & *Distance Field Soft Shadows* (Unreal Engine docs); Karabelas, *Screen Space Shadows* (2020).
- Crassin et al., *Interactive Indirect Illumination Using Voxel Cone Tracing* (CGF/I3D 2011).
- Kim & Neumann, *Opacity Shadow Maps* (EGSR 2001); Yuksel & Keyser, *Deep Opacity Maps* (EG 2008); Jansen & Bavoil, *Fourier Opacity Mapping* (I3D 2010); Lokovic & Veach, *Deep Shadow Maps* (SIGGRAPH 2000).
- Tatarchuk, *Practical Parallax Occlusion Mapping* (SIGGRAPH 2006); Snyder & Nowrouzezahrai, *Fast Soft Self-Shadowing on Dynamic Height Fields* (EGSR 2008); Blinn, *Me and My (Fake) Shadow* (1988).

*Generated from an adversarially-verified deep-research sweep (102 source/verify agents). Two "shadow volumes can't be culled" claims and one "POM edges are crisp" claim were caught and corrected during verification; reflected above.*
