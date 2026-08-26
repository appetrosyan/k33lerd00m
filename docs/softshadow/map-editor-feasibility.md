# Full map editor for soft-shadow proxy authoring — feasibility

Branch `analytical-penumbra`. READ-ONLY study; no engine/shader/CMake edits, no
build, no launch. Question: what is the realistic path to a **full map editor**
— a spatial level-editing environment with the level geometry visible around the
light — for authoring per-light soft-shadow proxy geometry (a segment/tube
emitter with world endpoints A,B; an analytic box proxy transform)?

The prior study (`tube-authoring-ui-feasibility.md`) already settled that the
in-game ImGui `LightEditor` *panel* is the wrong scope. This study evaluates the
full-editor paths and does **not** re-recommend that panel.

**Verdict: use the external editor that is already wired into this fork —
TrenchBroom.** The `.map` grammar it emits already round-trips through this
tree's parser, a full FGD entity set already ships in `base/_tb/`, the campaign
geometry it needs is extractable from the shipped `.resources`, and the
proxy data it must persist is key/value spawnargs that ride the existing
`_extra_ents.map` override channel and the existing authored-spawnarg render
seam. Almost nothing new links into the engine.

Claims are tagged **VERIFIED** (read, file:line) or **ESTIMATE**. Reuse
candidates carry a confidence: **CONFIRMED-PRESENT** / **GATED-OFF** /
**STRIPPED** / **ABSENT** / **EXTERNAL**.

---

## Path 1 — In-engine Radiant survivors — verdict: **ABSENT / STRIPPED**

There is no full map editor in this source tree. This is RBDOOM-3-BFG, which
removed the id Win32/MFC editor suite and replaced it with a handful of ImGui
panels plus external-tool support. The evidence:

- **VERIFIED** the entire `neo/tools/` tree is: `compilers/` (dmap, aas),
  `imgui/` (`lighteditor/`, `afeditor/`, `util/`), `softshadow/`, and a single
  header `edit_public.h`. There is **no** `radiant/`, `guied/`, `sound/`,
  `decl/`, `particle/`, `script/`, `af/` editor-window directory. (`ls
  neo/tools`, `find neo/tools -type d`.)
- **VERIFIED** a full-tree search for `radiant`/`Radiant`/`idEditor`/
  `RadiantRun`/`EditorInit`/`ID_ALLOW_TOOLS` finds **zero** editor
  implementation. The only `Radiant` hits are comments (`RenderWorld_defs.cpp:969`
  "Radiant messes it up", `Material.cpp:296` "like D3Radiant does"). **ABSENT**.
- **VERIFIED** no MFC/Win32 GUI source survives anywhere:
  `grep -rln 'afxwin|#include <afx|CWnd|CDialog|Radiant.h' neo/` returns nothing.
  So there is nothing even Win32-bound to revive. **STRIPPED**.
- **VERIFIED** the `com_editors` bitmask (`Common.h:123-135`) enumerates
  `EDITOR_GUI/DEBUGGER/SCRIPT/LIGHT/SOUND/DECL/AF/PARTICLE/PDA/AAS/MATERIAL/
  EXPORTDEFS`, but the dispatcher that actually *opens* a tool,
  `idCommonLocal::InitTool` (`Common.cpp:445-457`), wires only **two** bits:
  `EDITOR_LIGHT → ImGuiTools::LightEditorInit` and
  `EDITOR_AF → ImGuiTools::AfEditorInit`. Every other bit is a vestigial flag
  with no editor behind it. **GATED-OFF (flag only, no implementation).**
- **VERIFIED** the only editor console command registered is `editLights`
  (`SysCmds.cpp:2911`), which sets `EDITOR_LIGHT`. There is no `editor`,
  `radiant`, or `editEntities` command that opens a spatial editor. **ABSENT.**

Conclusion: "revive the in-engine full editor" is not a real option — the source
does not exist to revive. Building a full spatial editor **inside** the engine
(extending `g_editEntityMode` crosshair-select into a multi-entity Radiant-like
mode) is a from-scratch project, not a revival, and it is a strict superset of
the panel the prior study already rejected. Ruled out on cost.

---

## Path 2 — External Tech4 `.map` editor (TrenchBroom) — verdict: **CONFIRMED-PRESENT, RECOMMENDED**

This fork is already provisioned for TrenchBroom. Every prerequisite for a
lossless round-trip is present.

### 2.1 The on-disk grammar round-trips — CONFIRMED-PRESENT

- **VERIFIED** `CURRENT_MAP_VERSION == 3` (`MapFile.h:51`). Shipped source maps
  are `Version 3` with `brushDef3` primitives (`base/maps/testshadow.map` header:
  `Version 3` then `brushDef3 { ( plane ) ( texmat ) "material" ... }`).
- **VERIFIED** the entity parser accepts **both** native and TrenchBroom
  dialects. `idMapEntity::Parse` (`MapFile.cpp:1114-1160`) dispatches
  `brush/brushDef/brushDef2/brushDef3`, `patchDef2/patchDef3`, `mesh`, and — the
  fall-through — `idMapBrush::ParseValve220` with the comment *"assume it's a
  brush in Valve 220 style from TrenchBroom"* (`MapFile.cpp:1149`).
- **VERIFIED** the writer emits either dialect: `WriteDiff` stamps
  `// Game: Doom 3 BFG` / `// Format: Doom3 (Valve)` when `valve220Format`, else
  `Version 3` (`MapFile.cpp:2020-2028`); `idMapBrush::WriteValve220`
  (`MapFile.cpp:899`) and `ConvertToValve220Format` (`MapFile.cpp:141`, *"heavily
  inspired by Valve220_from_BP from Netradiant-custom"*) exist.

So a map authored/edited in TrenchBroom's Valve-220 Doom-3 mode parses, and the
engine's own diff writer speaks the same format back. Round-trip is real.

### 2.2 The game-definition file already exists — CONFIRMED-PRESENT

- **VERIFIED** `base/_tb/fgd/` ships six FGDs: `DOOM-3-all.fgd`,
  `DOOM-3-all-and-models.fgd`, `DOOM-3-slim.fgd`, `DOOM-3-slim-and-models.fgd`,
  `DOOM-3-models.fgd`, `DOOM-3-multiplayer.fgd`, plus a `_tb/models/*.obj`
  display-proxy tree and `base/def/_tb_helpers.def`, `_tb_models.def`.
- **VERIFIED** the `light` entity is a fully-specified `@PointClass` in the FGD
  (`DOOM-3-all.fgd:7221` `... = light : "Light source..."`) exposing `light`
  radius, `_color`, `style`, `noshadows` (`light_radius`/`light_noshadows` also
  appear at `:7426`/`:7430`). This is the entity the author already selects and
  moves in TrenchBroom.

Extending this for proxies is a **data-only** edit: add two keys to the existing
`light` class (`soft_tube_a`, `soft_tube_b` as `string`/vector) and, if a box
proxy is wanted, either more keys on `light` (`soft_box_mins`/`soft_box_maxs`/
`soft_box_axis`) or a new small `@PointClass soft_proxy_box`. No engine change to
teach the editor about them — the FGD is a plain text data file. **ESTIMATE:
~10-20 FGD lines.**

### 2.3 Campaign spatial context is recoverable — CONFIRMED-PRESENT (the decisive check)

The obvious objection to an external editor is "the shipped BFG campaign maps
have no editable `.map` source, so TrenchBroom would show floating lights with no
walls." That objection is **false for this tree**:

- **VERIFIED** `base/maps/` holds 47 `.map` files, of which 45 are
  `*_extra_ents.map` overrides and only 2 (`testshadow.map`, `softbox.map`, the
  author's own dmap-able test maps) are full brush sources. The campaign maps
  ship as `erebus1.resources` (135 MB) + `erebus1.crc`, not loose `.map`.
- **VERIFIED** the full `brushDef3` source is **embedded** in the resource
  bundle: `strings base/maps/erebus1.resources` yields `maps/game/erebus1.map`
  followed by `"classname" "worldspawn"` and many `brushDef3` blocks — the real
  world geometry, not a stub.
- **VERIFIED** the engine ships a console command to extract it:
  `extractResourceFile <resource file> <outpath> <copysound> <all>`
  (`FileSystem.cpp:3343`, impl `:2806-2835` → `idResourceContainer::
  ExtractResourceFile`). This writes the embedded `erebus1.map` (and assets) to
  disk. The engine already reads full `.map` source transparently through the
  bundle (e.g. `exportMapToOBJ` does `map.Parse("maps/<name>.map", ...)`,
  `Common_mapconvert.cpp:448-470`).

So the workflow "extract `erebus1.map` → open in TrenchBroom → see the full level
around the light → drag proxy handles" is achievable with tools already in the
binary. TrenchBroom is a **full** spatial editor here, on the actual benchmark
map (RoE intro / erebus1), not a stripped one.

### 2.4 How authored proxies reach the render path — the integration seam

The parked tube coverage primitive consumes per-light endpoints A,B; there is no
emitter-geometry datum in map data today (`light_radius` is falloff, collapsed to
`min(lightRadius.xyz)`, **VERIFIED** the auto-penumbra reads exactly that,
`tr_frontend_addmodels.cpp:88`). So proxy geometry must be **authored as
spawnargs on the `light` entity**, for one hard structural reason:

- **VERIFIED — decisive constraint.** The persistence channel is **key/value
  only; it cannot carry brushes.** `idMapFile::Parse`'s `_extra_ents.map` merge
  copies `epairs` onto matched entities and, for a *new* entity, does
  `mapEnt->epairs.Copy(...)` under an explicit `// don't grab brushes or polys`
  (`MapFile.cpp:1889-1894`). The writer `WriteDiff` likewise emits only an epairs
  diff (`MapFile.cpp:2029-2075`), never primitives. A brush-based `soft_proxy_box`
  added through the override channel would silently lose its geometry.
  **⇒ the box proxy must be encoded as spawnarg vectors, not a brush**, unless
  you author it into the full extracted `.map` and re-dmap (heavier path). The
  tube is spawnargs by nature (two points), so this costs it nothing.

The seam from spawnarg to frontend already exists and is proven by an identical
feature:

1. **Spawn read — `neo/d3xp/Light.cpp:125`.** `idLight` reads `light_radius`
   (and neighbours) from `spawnArgs` into the `renderLight_t` parms at spawn.
   New keys `soft_tube_a`/`soft_tube_b` (and box keys) are parsed alongside here.
   **ESTIMATE ~6-15 lines** (`spawnArgs.GetVector` calls + assignment).
2. **Carrier — `neo/renderer/RenderWorld.h:209`.** `renderLight_t` (public parms:
   `lightRadius`, `lightCenter`, …) gains the A,B endpoint fields (and box
   transform fields). **ESTIMATE ~3-6 lines** of struct members + init.
3. **Frontend consume — `neo/renderer/tr_frontend_addmodels.cpp:72`,
   `R_SoftPenumbraRadius`.** This function **already** demonstrates the whole
   pattern: it prefers an authored `penumbraSize` spawnarg, else derives from
   `lightDef->parms.lightRadius`, else the global cvar (`:72-99`). The authored
   A,B endpoints follow the exact same map-data → `renderLight` parms → frontend
   route, then feed the parked tube-coverage primitive here (or at the caller
   that hands the penumbra size into `R_CollectPenumbraFaces`, `:59`).
   **ESTIMATE ~10-30 lines** to route A,B into the tube primitive call.

**Total new engine code for the recommended path: ~20-50 lines across 3 sites,**
all wiring, no new subsystem. Everything else (editor, format, override writer,
FGD) is reused.

---

## Path 3 — Hybrid / alternatives — considered, not better

- **In-engine full editor (new).** Rejected under Path 1: no source to revive,
  strict superset of the rejected panel, largest possible diff.
- **Headless `.map` transform tool.** A standalone script that injects
  `soft_tube_a/b` into a `.map` avoids TrenchBroom, but throws away the one thing
  the owner asked for — **spatial context**. You'd be typing coordinates blind.
  Worse on the stated requirement, not better.
- **TrenchBroom for placement + in-engine ImGuizmo for fine-tune.** Plausible as
  a *later* nicety (the ImGui `LightEditor` + ImGuizmo already live-apply
  spawnargs, per the prior study), but it is additive polish, not the spine.
  Ship Path 2 first; the gizmo tune-up is a free add-on because both edit the
  same spawnargs.

No alternative beats "reuse the editor and channels already in the tree."

---

## RECOMMENDATION

**Adopt TrenchBroom as the full map editor; extend the shipped FGD with the
soft-shadow proxy keys on the `light` class; persist proxies as spawnargs via the
existing `_extra_ents.map` override; consume them at the existing authored-spawnarg
render seam.**

Rationale, tied to reuse — every load-bearing asset already exists and is proven:

| Asset | Status | Evidence |
|---|---|---|
| External spatial editor | **EXTERNAL, provisioned** | `base/_tb/fgd/*.fgd` (6 files) |
| `.map` parser (native + Valve220) | **CONFIRMED-PRESENT** | `MapFile.cpp:1149`, `:459`, `:705` |
| `light` entity FGD to extend | **CONFIRMED-PRESENT** | `DOOM-3-all.fgd:7221` |
| Campaign geometry for context | **CONFIRMED-PRESENT (extractable)** | embedded `erebus1.map` in `.resources`; `extractResourceFile` `FileSystem.cpp:3343` |
| Override persistence (KV-only) | **CONFIRMED-PRESENT** | merge `MapFile.cpp:1837-1894`; writer `WriteDiff` `:1994`; `MapSave` `GameEdit.cpp:1199` |
| Authored-spawnarg render seam | **CONFIRMED-PRESENT** | `R_SoftPenumbraRadius` `tr_frontend_addmodels.cpp:72` already routes an authored key |
| Tube coverage primitive | **CONFIRMED-PRESENT (parked)** | worktree `worktree-agent-ae78e73409f00be4d`, consumes A,B |
| In-engine full editor | **ABSENT / STRIPPED** | no `radiant/`, no MFC; `InitTool` wires 2 bits |

The one design fact that constrains the whole design: **the override channel
carries key/values only, never brushes** (`MapFile.cpp:1889-1894`;
`WriteDiff` emits epairs). So the box proxy is spawnarg-encoded (mins/maxs/axis
vectors), not a brush — matching how the tube (A,B) is already shaped.

### Cheapest proof-of-life (single, no engine code)

Prove the whole spine end-to-end with **only data files**, before writing the
~20-50 engine lines:

1. `extractResourceFile maps/erebus1.resources <out> all` → get `erebus1.map`.
2. Open it in TrenchBroom with the shipped `DOOM-3-all.fgd`; confirm the level
   geometry and the `light` entities render in full spatial context.
3. On one light, hand-add `soft_tube_a "x y z"` / `soft_tube_b "x y z"` (either
   as FGD-declared keys or raw spawnargs), save.
4. Diff against the original into `maps/game/erebus1_extra_ents.map` (or let the
   engine's `MapSave`/`WriteDiff` produce it) and confirm the two keys land on
   the named light and survive a reload via the `_extra_ents.map` merge path.

If the two vectors round-trip onto the named light through the override file,
the editor, format, FGD, and persistence channel are all validated — and the
only remaining work is the three known engine sites in §2.4.

### Risks, ranked

1. **TrenchBroom can't natively drag the *second* endpoint of a point entity.**
   A `light` shows one origin handle; A,B needs two draggable points. Mitigations
   (pick at build time): (a) two linked helper point-entities the author drags,
   referenced by the light; (b) edit A,B numerically in the entity-property panel
   (loses live drag but keeps spatial context); (c) fine-tune A,B later with the
   existing in-engine ImGuizmo, which already live-applies spawnargs. **This is
   the real design decision**, and it is a UX choice, not a blocker.
2. **Coordinate-space / scale mismatch.** TrenchBroom Valve-220 vs the engine's
   `brushDef3` axis and unit conventions must agree for A,B to land where the
   author sees them. Verify with the proof-of-life (step 3-4): a known endpoint
   must render where placed. Low once checked, silent if not.
3. **Extraction/round-trip drift on the full campaign `.map`.** If the author
   ever saves the *whole* extracted map (not just the KV diff), brush/texcoord
   fidelity through the TrenchBroom→`brushDef3` path matters. Avoided entirely by
   keeping proxies in the **KV-only** `_extra_ents.map` channel and never
   round-tripping world brushes — which the recommended design already does.
4. **FGD drift.** The shipped FGDs are generated artifacts; hand-added proxy keys
   could be clobbered by a future regeneration. Low; document the added keys and
   the regenerator source.
