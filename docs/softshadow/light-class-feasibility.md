================================================================================
FEASIBILITY: PER-LIGHT PHYSICAL EMITTER CLASSES (diffuse-no-shadow / hard-stencil
/ analytic-disk) -- read-only study, branch analytical-penumbra, 2026-08-26
================================================================================
Every number is tagged MEASURED (inspected on disk / in code) or ESTIMATE.
No code was changed.

--------------------------------------------------------------------------------
VERDICT SUMMARY
--------------------------------------------------------------------------------
Class 1 (diffuse/areal -> no shadow term, N.L kept): FEASIBLE WITH ZERO ENGINE
  CODE. It is exactly the engine's existing "noshadows" light spawnarg, and the
  fork already has an engine-native curated-override mechanism
  (maps/game/<map>_extra_ents.map overrides spawnargs of named map entities).
  59/220 erebus1 lights already use it (MEASURED). The whole soft pipeline
  (records, stencil volumes, tile bins, term slots, cache warm, gate probes)
  already drops such a light automatically. The open question is purely
  curatorial + the occluder-size guard (risk E2).

Class 2 (point-like -> hard stencil): MECHANICALLY PRESENT BUT NOT WIRED
  PER-LIGHT. All pieces exist and run today (volume build, StencilShadowPass,
  stencil-tested interaction state), but the hand-off is gated by GLOBAL cvars
  with an explicit !r_useSoftShadowVolumes exclusion. Making it per-light is a
  two-site change (frontend collection predicate + backend performStencilTest
  predicate). Correctness risk is real: the stencil path needs silEdges, which
  FAIL on non-2-manifold meshes -- the very defect class (erebus1_05 EXTENT)
  that forced face-coverage mode. Gate coverage for class-2 lights is lost
  unless a new probe is built. Population on the bench corpus looks small
  (17/152 casting lights are tiny fixtures, MEASURED map-wide; per-cap unknown).

Class 3 (everything else): status quo, no work.

Recommended first experiment (zero code): curated noshadows overrides via
erebus1_extra_ents.map + recapture + gate & bench A/B on one heavy cap. Detail
at the end.

--------------------------------------------------------------------------------
A. CLASSIFICATION SIGNALS AVAILABLE PER LIGHT
--------------------------------------------------------------------------------
Per-light emitter radius already exists end to end (task #105):
  - renderLight_t.penumbraSize: neo/renderer/RenderWorld.h:213-217, parsed from
    the "penumbraSize" entity key in neo/d3xp/Light.cpp:98.
  - R_SoftPenumbraRadius( lightDef ): neo/renderer/tr_frontend_addmodels.cpp:72-99.
    Priority: authored penumbraSize (capped 128) > auto-derived
    min-light-extent * r_shadowPenumbraAutoScale (default 0.04), clamped to
    [1, r_shadowPenumbraSize (default 8)] > the global. Cvars at
    neo/renderer/RenderSystem_init.cpp:315-317.
  - Fed per light to the interaction PS (RenderBackend.cpp:1987), tile bin
    (RenderBackend.cpp:4992), capture (RenderCapture.cpp:953).
So signal (ii) "derived radius" exists TODAY; a class-2 epsilon test
(R_SoftPenumbraRadius(light) <= eps) is one comparison at any of those sites.

Tagging mechanisms, best first:
  (i-a) _extra_ents.map override -- ENGINE-NATIVE CURATED LIST. idMapFile::Parse
        merges maps/game/<map>_extra_ents.map and OVERRIDES spawnargs of
        entities matched BY NAME (neo/idlib/MapFile.cpp:1837-1894; empty value
        deletes a key). base/maps/game/erebus1_extra_ents.map already exists
        (66 env_probe entities, MEASURED). Adding
          { "name" "light_53807"  "noshadows" "1" }            -> class 1
          { "name" "light_53843"  "penumbraSize" "0.5" }       -> class-2 radius
        is data-only, per-map, no engine code, survives repacks. This is the
        direct analogue of the curated proxy-model precedent
        (r_softShadowProxyModel, tr_frontend_addmodels.cpp:203/280).
  (i-b) Authored spawnarg on the light def -- same parse point Light.cpp:98; a
        new "softClass" key would ride identically if an explicit enum is ever
        wanted. Not needed for the first experiment: "noshadows" (class 1) and
        "penumbraSize" (class 2 radius) are both already parsed.
  (ii)  Derived: class 2 = derived radius <= eps OR distance/radius huge;
        class 1 = penumbra width at typical receiver >> occluder size. The
        width side needs an occluder bound -- available per (light, caster) at
        collection time (tri->bounds in R_AddSingleModel /
        R_CollectPenumbraFaces, Interaction.cpp:748), so a size-conditioned
        derived class 1 is possible per-CASTER (see E2), not just per-light.
  (iii) Emissive-surface linkage: DOES NOT EXIST in the data. MEASURED: erebus1
        light entities carry only origin/light_radius/texture(light material)/
        noshadows etc. The "texture" key is the PROJECTION material
        (lights/spot01, lights/squareishlight, ...), not a link to any emissive
        mesh. GUI screens are separate func_statics (guis/screens/*.gui);
        association is by spatial proximity only. E.g. in the CPU-screen room,
        light_53828/53838/53839 ("lights/squareishlight", radius 24, at
        y=-1823) sit 8u in front of the three cpu_top.gui screens at y=-1831
        (MEASURED) -- clearly the screen-glow lights, but only a human (or a
        proximity heuristic) can say so. Curation is the reliable path;
        precedent accepted in this project.

--------------------------------------------------------------------------------
B. WHAT EACH CLASS SAVES ON THE BENCH CORPUS
--------------------------------------------------------------------------------
Map data (MEASURED -- erebus1 entities parsed out of base/maps/erebus1.resources,
which embeds the .map as plain text; parser output in scratchpad
erebus1_lights.json / erebus1_ents.json):
  - 1420 entities, 220 light entities (matches the known corpus number).
  - 59 lights already "noshadows" "1"; 15 fog/glare materials (fogs/*, never
    interact: RenderBackend.cpp:5217 + Material.h LightCastsShadows());
    overlap 6 -> 152 shadow-casting candidates.
  - Of the 152: 107 have NO texture key (default point-light material --
    generic fills, the class-1 candidate pool), 17 have min light_radius
    extent <= 32u (tiny fixtures, class-2 candidate pool), 5 are hall-scale
    (min extent >= 512).
  - Intro-cinematic camera zone (cameras cluster at ~(1100-1700, 2000-2700,
    z~1600-1712), MEASURED): 18 lights, 4 already noshadows, 8 texture-less
    fills, radii 64-496u. CPU-screen room: 19 lights, 5 already noshadows,
    incl. the 3 squareishlight screen-glow lights above.

Per-cap term-light identification is NOT possible offline: the .softcap
fixtures are local/gitignored and absent from this checkout (MEASURED: zero
*.softcap under base/), so which of the 6-21 term lights on each heavy cap
(99-286 ms TERM walk, MEASURED previously per project memory) map to which
entities cannot be read here. Estimates:
  - Class 1: the cinematic zones are dominated by texture-less fills; if the
    curation moves even a third of a heavy cap's term lights to class 1, the
    saving is that third of its TERM ms -- term cost is roughly per-light
    additive (the walk is dispatched per light slot). ESTIMATE: 20-40% of TERM
    on the intro-cinematic caps; must be measured per cap after recapture.
    Note class 1 also deletes that light's FRONTEND cost: record collection,
    stencil-volume build, tile bin, term slot (frees slot budget for other
    lights -- the 12-slot spill cliff), and surf-cache warm traffic.
  - Class 2: 17/152 tiny-fixture lights map-wide (MEASURED count of
    candidates); how many are term lights on heavy caps is unknown -- the
    heavy cinematic caps skew to LARGE fills, so ESTIMATE: small (0-3 per
    cap), and class 2 is NOT the lever for the heavy caps. Its value is
    correctness/appearance (maledict-style zero-penumbra looks) plus removing
    small-light walk cost in gameplay scenes.

--------------------------------------------------------------------------------
C. CLASS-2 MECHANICS -- WHAT HARD-SHADOW MACHINERY STILL WORKS
--------------------------------------------------------------------------------
Working in this fork:
  - Stencil shadow VOLUMES: built in the frontend whenever
    (r_useStencilShadows || r_useSoftShadowVolumes) && !r_useRTShadows --
    tr_frontend_addmodels.cpp:1956-2080 -- for BOTH static casters (baked
    surfInter->shadowIndexCache) and dynamic/animated casters
    (R_CreateInteractionShadowVolume, incl. posed MD5 / settled ragdolls),
    with z-pass/z-fail selection (R_ViewPotentiallyInsideInfiniteShadowVolume).
    So under the shipped soft config the volumes ALREADY exist per light in
    vLight->globalShadows/localShadows (the soft band/AAM prepass consumes
    them).
  - StencilShadowPass (RenderBackend.cpp:4626) + the classic stencil-tested
    interaction state: RenderInteractions takes performStencilTest and sets
    GLS_STENCIL_FUNC_EQUAL ref 128 (RenderBackend.cpp:2166-2177). The live
    per-light stamp is at RenderBackend.cpp:5530-5555.
  - Shadow-map/PCSS atlas path exists (ShadowAtlasPass RenderBackend.cpp:4245,
    ShadowMapPassFast :4037) and PCSS is the current edge-less-caster fallback
    (r_shadowMapPCSSAnalyticContact, RenderSystem_init.cpp:261), but
    r_useShadowMapping has NO idCVar definition in active code (only string
    refs; the gate pins it "0", RenderCapture.cpp:75). Stencil, not the atlas,
    is the natural class-2 target: volumes are already built under the soft
    config, and the atlas brings back its own defect family (overflow ants).

The blocker (the only one): the hand-off is global, not per-light.
  RenderBackend.cpp:5530-5532:
      const bool performStencilTest = r_useStencilShadows.GetBool()
          && !r_useRTShadows.GetBool()
          && !r_useSoftShadowVolumes.GetBool()          // <-- global exclusion
          && ( vLight->globalShadows || vLight->localShadows );
Per-light "not soft" precedent to ride: everything downstream already keys on
per-vLight state --
  - interaction soft branch: isSoftWedge = r_useSoftShadowVolumes &&
    vLight->softEdgeCount > 0 (RenderBackend.cpp:1970, and again :5560);
  - tile-bin/term eligibility: vLight->softEdgeCount / softCasterCount > 0
    (RenderBackend.cpp:4971); slot-budget overflow already leaves a light on
    the in-shader integral per light (SoftShadowTermPass.h:104-112, task #111);
    cost-gate bypass r_softShadowSurfCacheMinCost (RenderSystem_init.cpp:383);
  - per-light RT: R_LightUsesRTShadows(vLight) (RenderBackend.cpp:5253).
So a class enum does not even need new plumbing to the shaders: make the
frontend skip SOFT COLLECTION for a class-2 light (one predicate in the
condition at tr_frontend_addmodels.cpp:2102, where lightDef and
R_SoftPenumbraRadius are in hand) while still building its stencil volumes
(the block just above it), and softEdgeCount==0 then auto-drops the light from
every soft consumer. The backend needs the 5530 predicate turned per-light
(class-2 light => stencil test on even though r_useSoftShadowVolumes is on).
Two small sites total; the interaction shader needs nothing (the stencil test
is fixed-function state, and with softEdgeCount==0 the plain variant binds).

Confirmed no-shadow fallback today: a casting light with zero soft records
renders its interactions UNSHADOWED under the shipped soft config (no RT mask,
no stencil test, atlas off) -- i.e. class-1 behaviour is already the engine's
silent fallback; class 1 just makes it deliberate.

--------------------------------------------------------------------------------
D. GATE / GROUND-TRUTH IMPLICATIONS (RenderCapture.cpp)
--------------------------------------------------------------------------------
Per-light exclusion is STRUCTURALLY FREE. The gate probes only lights with
soft records: probeLights collects cap.lights[li].edgeCount > 0
(RenderCapture.cpp:2697-2703). A class-1 (noshadows) or class-2 (no soft
collection) light produces no records at capture, so after RECAPTURE (the
fixtures pin everything; stale-capture phantom-defect rule applies) it simply
vanishes from the probe set. The v7 lightParms already carry per-light
noShadows (RenderCapture.cpp:2749), so the fixture format needs no change.
Costs/caveats:
  - Recapture the corpus (softShadowRecapture exists per project memory); the
    corpus light count (11 caps / 76 lights) will shrink -- if a cap loses ALL
    soft lights it trips the degenerate-capture SETUP defect
    (RenderCapture.cpp:2708-2714) and must be retired (precedent: the 5
    cinematic caps).
  - Class-1 oracle: nothing to change -- "term == 1" is enforced by the light
    never being probed; the LIGHTING (N.L) is untouched machinery the gate
    never scored anyway. If you want positive confirmation rather than
    absence-of-probe, a trivial probe "this light draws and its interactions
    bind the unshadowed variant" could be added, but it is optional.
  - Class-2 oracle: the RT oracle COULD score it -- the disk model at r=0 is
    exactly the hard point shadow, and the RT sheet already traces per-light
    with r_rtShadowSoftRadius set from the cap (RenderCapture.cpp:484-487,
    2333-2334) -- but the shipped stencil result has no anaTerm readback to
    compare against (stencil is fixed-function; GateAgreement reads the
    analytic term field). Building a stencil-vs-RT probe means reading back
    the stencil-lit frame per light (the machinery for per-light framebuffer
    probes exists -- the gate already renders per-light sheets -- so it is
    moderate work, not trivial). Until then class-2 lights are UN-GATED, which
    collides with this project's red-until-fixed discipline. Recommendation:
    gate class 2 initially by full-frame A/B (com_softShadowGateFullFrame
    instrument) + the RT sheet at radius->0 as a visual diff, and keep the
    class-2 population curated and small.

--------------------------------------------------------------------------------
E. RISKS
--------------------------------------------------------------------------------
E1. Class-boundary pops. A light flipping class at runtime cannot happen with
    authored/curated tags (static per map) -- flips would only come from a
    DERIVED classifier reacting to light scaling/scripted radius changes.
    Cinematic lights are script-moved (v7 lesson), so a derived classifier
    evaluated per frame could flip mid-cutscene. Mitigation: classify ONCE at
    light spawn/update from parms (sticky, like lightHasMoved), never from
    per-frame geometry.
E2. The surgical-lamp argument is occluder-size-conditional. Penumbra width at
    the receiver is w ~ R*(d_recv-d_occ)/d_occ. "Effectively no shadow" holds
    only when w >> occluder size. MEASURED example: the screen-glow lights are
    R~24-50u; a head-size (16u) occluder midway keeps w ~ R ~ 1.5-3x the
    occluder -- a faint but REAL shadow the analytic path currently renders; a
    wall-size mover under the same light must still shadow. So class 1 as a
    blanket per-light tag is only safe for lights whose reachable casters are
    all small relative to w (fills high above walkable space, glow lights
    inside fixtures). Two guards possible:
      a) curation rule: only tag lights whose RT sheet shows term ~ 1
         everywhere on the cap corpus (objective, uses the existing oracle);
      b) derived per-CASTER guard at collection time: drop a caster from a
         light's stream when its bounds max extent < k * w(light, caster
         distance) -- keeps the wall case, deletes the hand case, and degrades
         to class 1 when ALL casters drop. More code, but self-guarding and
         monotonic (no special-case path: it generalizes the existing
         per-caster cull in collection).
    The blanket noshadows experiment is still the right FIRST step because the
    cinematic caps have fixed cameras and no large movers mid-beam -- verify
    on the live cinematic, not just the frozen cap (gate blind-spot lesson).
E3. Class-2 stencil correctness debt: silEdges fail on non-2-manifold meshes
    (tr_frontend_addmodels.cpp:2086-2090 comment; the erebus1_05 EXTENT
    defects were exactly this) -- casters without silEdges silently cast
    nothing on the stencil path. Face-coverage mode exists BECAUSE of this.
    Curate class-2 lights only where the casters are known-manifold (MD5
    characters like the maledict are; grates/rails are not), or accept the
    hole and gate it with the full-frame probe.
E4. Banned-pattern check: the hard rule bans stencil-volume UMBRA/PENUMBRA
    REGION classification INSIDE the soft integral (early-outs must live in
    the integral math). Class 2 does not resurrect it: the whole light leaves
    the soft path and the stencil result is the final binary term -- there is
    no region classification feeding an integral, and the soft path for
    classes 1/3 is untouched. Distinct mechanism; compliant. (The legacy AAM
    band prepass is the banned pattern's home and stays dead in face mode --
    RenderBackend.cpp:5571-5577 -- unaffected by this proposal.)
E5. Perceptual regression review: class 1 deletes real (if faint) gradients.
    Per the accuracy-before-perf rule this is a GROUND-TRUTH REDEFINITION the
    owner is explicitly making ("it is NOT inaccurate...") -- record it as
    such: the per-class ground truth must be written down (class 1: term == 1;
    class 2: point-visibility; class 3: disk integral), else the gate's
    "lossless = physically accurate" definition silently changes meaning.
E6. Bench discipline: cinematic A/B needs com_fixedTic 1 or the gate bench
    (cross-launch variance ~15%), 1080p+1440p both (MOC half-res lesson), and
    med/p99/max reporting.

--------------------------------------------------------------------------------
RECOMMENDED MINIMAL FIRST EXPERIMENT (zero engine code)
--------------------------------------------------------------------------------
1. Curate: from the cap corpus pick ONE heavy intro-cinematic cap. Using the
   RT sheets, list its term lights whose RT reference is ~everywhere-lit
   (term ~ 1) or whose shadows are imperceptible in the ref frames; cross-match
   origins against the parsed light table (scratchpad erebus1_lights.json has
   name/origin/texture/radius for all 220). Candidate pool: the texture-less
   fills + screen-glow lights in the two cinematic zones (lists in section B).
2. Tag: add override stanzas to base/maps/game/erebus1_extra_ents.map --
   { "classname" "light" "name" "<light_name>" "noshadows" "1" } per curated
   light. Data-only, reversible, per-map.
3. Recapture that cap (softShadowRecapture), then:
   a. gate on the recaptured cap -> must stay 0 defects (remaining lights);
   b. gate bench A/B (same binary, fixture-swapped) -> med/p99/max TERM ms;
   c. live cinematic pass at 1080p and 1440p, eyes on the tagged lights'
      receivers for missing mid-size-occluder shadows (E2).
4. Report the measured TERM delta per tagged light count. If the win is real,
   extend curation to the rest of the corpus caps; only then decide whether
   class 2 (the two-site per-light stencil hand-off,
   tr_frontend_addmodels.cpp:2102 + RenderBackend.cpp:5530) is worth its
   gate-coverage debt.

--------------------------------------------------------------------------------
KEY FILE:LINE INDEX
--------------------------------------------------------------------------------
R_SoftPenumbraRadius (per-light radius)   neo/renderer/tr_frontend_addmodels.cpp:72-99
penumbraSize parse (spawnarg)             neo/d3xp/Light.cpp:98
renderLight_t.penumbraSize                neo/renderer/RenderWorld.h:213-217
radius cvars                              neo/renderer/RenderSystem_init.cpp:315-319
LightCastsShadows (noshadows plumbing)    neo/renderer/RenderCommon.h:199-203,
                                          neo/renderer/Material.h:708-716,
                                          tr_frontend_addmodels.cpp:1931,
                                          tr_frontend_addlights.cpp:245
soft collection predicate (class hook)    neo/renderer/tr_frontend_addmodels.cpp:2102
stencil volume build (static/dynamic)     neo/renderer/tr_frontend_addmodels.cpp:1956-2080
per-light stencil stamp (global gate)     neo/renderer/RenderBackend.cpp:5530-5555
stencil-tested interaction state          neo/renderer/RenderBackend.cpp:2166-2177
interaction soft branch (per-light)       neo/renderer/RenderBackend.cpp:1970-2032
term/bin per-light eligibility            neo/renderer/RenderBackend.cpp:4965-4998
term slot budget bypass (task #111)       neo/renderer/Passes/SoftShadowTermPass.h:104-112
gate probe-light selection                neo/renderer/RenderCapture.cpp:2697-2714
gate per-light rebuild (v7, noShadows)    neo/renderer/RenderCapture.cpp:2721-2749
gate RT-oracle radius pin                 neo/renderer/RenderCapture.cpp:484-487, 2333-2334
extra_ents override mechanism             neo/idlib/MapFile.cpp:1837-1894
erebus1 map source (plain text in pack)   base/maps/erebus1.resources
parsed light/entity tables                /tmp/claude-1000/-home-app-Games-gog-doom-3-bfg-edition/23e3d413-fae3-4e41-bf39-04e051ab67d9/scratchpad/erebus1_lights.json, erebus1_ents.json
================================================================================
