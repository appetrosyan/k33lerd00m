# In-editor authoring UI for soft-shadow proxy geometry — feasibility

Branch `analytical-penumbra`. READ-ONLY study; no engine/shader/CMake edits, no
build, no launch. Question: can an author drag handles in 3D to place per-light
soft-shadow proxy geometry — a segment/tube (endpoints A,B) and the analytic box
proxy — see it live, and persist it into map data, built mostly from parts that
already exist in this tree?

**Verdict: GO.** The entire authoring spine already exists, compiles, and is
linked into the shipping executable: a working ImGui light editor, the ImGuizmo
3D manipulator it drives, entity-under-crosshair selection, live debug-draw, and
— decisively — a writer that already emits the `<map>_extra_ents.map` override
file. The new work is small and concentrated in one TU. The "curated is the safe
path" answer already has a home.

Every claim below is tagged **VERIFIED** (I read it, file:line cited) or
**ESTIMATE**. Reuse candidates carry a confidence:
CONFIRMED-PRESENT-AND-LINKED / PRESENT-BUT-GATED-OFF / VENDORED-UNUSED / ABSENT.

---

## 1. What already exists in this tree

### 1.1 The ImGui light editor — the spine — CONFIRMED-PRESENT-AND-LINKED

`neo/tools/imgui/lighteditor/LightEditor.cpp` (+ `.h`) is a complete,
gizmo-driven, spawnarg-editing, map-saving light editor. It is not vendored
dead code — it is globbed into the build target:

- **VERIFIED** `neo/CMakeLists.txt:775` globs `tools/imgui/lighteditor/*.cpp`
  into `IMGUI_EDITOR_LIGHT_SOURCES`, added to `RBDOOM3_SOURCES` at
  `neo/CMakeLists.txt:1044`, which feeds `add_executable(${APP_NAME} …)`
  (`:1220`). It builds into the main binary.

Reachability at runtime (the open path, all VERIFIED):

- `neo/d3xp/Light.cpp:1207-1209` — when `g_editEntityMode == 1` and a light is
  the selected entity, it calls `common->InitTool( EDITOR_LIGHT, &spawnArgs, this )`.
- `neo/framework/Common.cpp:447-449` — `EDITOR_LIGHT` routes to
  `ImGuiTools::LightEditorInit( dict, entity )`.
- `neo/tools/imgui/ImGuiTools.cpp:72-98` — `LightEditorInit` calls
  `LightEditor::Instance().ShowIt(true)` + `ReInit(dict, ent)`; the draw pump
  `ImGuiTools::…Draw()` calls `LightEditor::Instance().Draw()` when shown.

What it already does, per light (all VERIFIED in `LightEditor.cpp`):

- `LightInfo::FromDict` (`:113`) reads spawnargs into an editable struct
  (`origin`, `_color`, `light_radius`, `light_center`, `light_target/up/right`,
  `noshadows`, `texture`, …).
- `LightInfo::ToDict` (`:241`) serializes the struct back to an `idDict`, using
  `""` values to signal key deletion.
- `TempApplyChanges` (`:621`) live-applies edits to the running light via
  `gameEdit->EntityChangeSpawnArgs` + `EntityUpdateChangeableSpawnArgs` — you
  see the change immediately.
- `SaveChanges(bool saveMap)` (`:633`) pushes the dict into the in-memory map
  (`gameEdit->MapCopyDictToEntity`, `:639`) and, if asked, calls
  `gameEdit->MapSave()` (`:654`). Bound to Ctrl+S / File→Save Map (`:1072`).

This struct + FromDict/ToDict + apply + save is exactly the shape a proxy
authoring panel needs. Adding tube/box fields is an extension of an existing
pattern, not a new subsystem.

### 1.2 ImGuizmo 3D manipulator — CONFIRMED-PRESENT-AND-LINKED

- **VERIFIED** `neo/libs/imgui/ImGuizmo.cpp` is globbed by
  `neo/CMakeLists.txt:532` (`libs/imgui/*.cpp` → `IMGUI_SOURCES`) into
  `RBDOOM3_SOURCES` (`:1028`). Compiled and linked — **not** vendored-unused.
- **VERIFIED** It is already driven by the light editor: `LightEditor.cpp:1164`
  `ImGuizmo::Manipulate( cameraView, cameraProjection, mCurrentGizmoOperation,
  mCurrentGizmoMode, manipMatrix, … )`, with translate/rotate/scale radio
  buttons (`:922-934`), snap, and a live cube preview `DrawCubes` (`:1138`).

What a translate gizmo needs, and where it already comes from (VERIFIED):

- **View + projection matrices**: `LightEditor.cpp:1034` gets the player render
  view via `gameEdit->PlayerGetRenderView( viewDef.renderView )`, then
  `R_SetupViewMatrix(&viewDef)` / `R_SetupProjectionMatrix(&viewDef,false)`
  (`:1125-1126`) and passes `viewDef.worldSpace.modelViewMatrix` +
  `viewDef.unjitteredProjectionMatrix` (`:1128-1129`) straight to ImGuizmo.
- **A world transform in/out**: it builds `idMat4` from `cur.origin`/`cur.angles`
  (`:1137,1144`), calls `Manipulate`, and reads the result back
  (`cur.origin = gizmoMatrix.GetTranslation()`, `:1171`; angles at `:1178`).

So a second/third translate gizmo for endpoints A and B is a copy of an existing
call with a distinct `ImGuizmo::SetID(n)` and its own point-as-matrix. No new
matrix sourcing, no new input routing.

### 1.3 The override-file writer — CONFIRMED-PRESENT-AND-LINKED (the big one)

The task asked whether anything writes the `_extra_ents.map` override, and to
size it as new code if nothing does. **It already exists.**

- **VERIFIED** `neo/d3xp/GameEdit.cpp:1199` `idGameEdit::MapSave`: for GLTF maps
  (`mapFile->IsGLTF()`, `:1204`) it re-parses the original and calls
  `origFile->WriteDiff( mapFile, "<mapname>_extra_ents", ".map" )` (`:1207-1213`)
  — it writes precisely the patch file the curation manifest documents. The
  erebus1 maps are GLTF, so this is the path that runs.
- **VERIFIED** `MapCopyDictToEntity` (`:1246`) sets/deletes epairs on the named
  map entity in memory before the diff is written (empty value = delete, `:1262`).
- **VERIFIED** consumption side already understood: `idMapFile::Parse` merges
  `<mapname>_extra_ents.map` by `"name"` (`neo/idlib/MapFile.cpp:1837-1894`, per
  `docs/softshadow/light-curation-manifest.md:9-13`); savepath copy shadows the
  shipped file (`FileSystem.cpp:3788` last-added-first).

Net: **no new persistence code.** The UI writes proxy keys into the light's
dict via the existing `ToDict`; `SaveChanges(true)` already round-trips them into
`erebus1_extra_ents.map`. (One caveat — see Risks — the non-GLTF branch at
`GameEdit.cpp:1218` overwrites the whole `.map`; irrelevant for erebus1/GLTF.)

### 1.4 Entity-under-crosshair selection — CONFIRMED-PRESENT-AND-LINKED

- **VERIFIED** `idEditEntities::SelectEntity` (`neo/d3xp/GameEdit.cpp:474-499`)
  ray-casts from the view (`FindTraceEntity`, `:495`), gated on
  `g_editEntityMode` (`:479`). This is the "click a light" pick; it drives the
  `InitTool(EDITOR_LIGHT)` path in §1.1. No new pick code needed to select the
  light to author.

### 1.5 Live proxy preview via debug draw — CONFIRMED-PRESENT-AND-LINKED

- **VERIFIED** the API exists and is virtual-dispatched in
  `neo/renderer/RenderWorld.h:481-488`: `DebugLine`, `DebugArrow`, `DebugSphere`,
  `DebugBounds`, `DebugBox`, `DebugCone`.
- **VERIFIED** it already renders in-game from the edit path: the entity-editor
  draws boxes/arrows every frame — `gameRenderWorld->DebugBox(…)`
  (`GameEdit.cpp:325,757`), `DebugArrow(…)` (`:743,762-774`),
  `DebugBounds(…)` (`:757`). A tube preview is `DebugLine(A,B)` +
  `DebugSphere` at each endpoint; a box proxy is one `DebugBox`. Dropped into the
  existing `idEditEntities` display loop, it renders with zero new plumbing.

### 1.6 The proxy math consumers already exist

- **VERIFIED** the segment primitive takes per-light endpoints A,B as input:
  `git show worktree-agent-ae78e73409f00be4d:neo/shaders/builtin/lighting/softwedge_coverage.inc.hlsl`
  — `float2 SoftSeg_TriInterval( float3 v0, float3 v1, float3 v2, float3 P,
  float3 A, float3 B )` (`:854`) and `float SoftSeg_UnionLength( float2 iv[…],
  int n )` (`:909`). Parked on worktree branch, commit 73f41897; unit-tested
  77/77 per the task. It is starved of exactly one datum: A,B.
- **VERIFIED** the box proxy `SoftScan_FillBox` exists and was shelved for
  auto-boxing, not for the primitive — a manual curated UI is its intended
  rescue (`docs/softshadow/proxy-shadows-status`).
- **VERIFIED** the reason A,B don't exist today: the engine collapses light
  extent to `min(lightRadius.xyz)` at `neo/renderer/tr_frontend_addmodels.cpp:91`
  (`ext = Min( lightRadius.x, Min( y, z ) )`). Endpoints must be **authored** —
  which is this UI's whole job.

---

## 2. Minimal design (reuse-first)

Everything lands in the light editor TU (`LightEditor.cpp`/`.h`) plus a couple
of preview lines in `GameEdit.cpp`. No new panel, no new window, no new save
path, no new selection code.

### 2.1 Data model — extend `LightInfo`

Add fields (`.h`): `bool hasTube; idVec3 tubeA, tubeB; bool hasProxyBox;`
(the box's transform reuses the light's existing `origin`/`angles` + a
`idVec3 proxyBoxHalf`). ESTIMATE ~6 lines.

**Serialization keys** (author-facing, absolute world coords, matching how the
gizmo yields world translations), in `ToDict`/`FromDict` alongside the existing
`light_*` handling:

| key | type | meaning |
|-----|------|---------|
| `soft_tube_a` | vec3 | tube endpoint A, world space |
| `soft_tube_b` | vec3 | tube endpoint B, world space |
| `soft_proxy_box` | vec3 | OBB half-extents (orientation = light `angles`, center = light `origin`) |

`FromDict`: `e->GetVector("soft_tube_a","",tubeA)` sets `hasTube` on success
(mirror of the `light_center` read at `LightEditor.cpp:190`). `ToDict`: write the
vectors when present, `""` to delete when the author clears the proxy (mirror of
the `DELETE_VAL` idiom at `:254-291`). ESTIMATE ~25 lines total.

### 2.2 Panel — one collapsing header in the existing `Draw()`

A `Soft Shadow Proxy` section: a "Tube (segment)" checkbox toggling `hasTube`,
numeric `InputFloat3` for A and B (so it is editable without the gizmo), a "Box
proxy" checkbox + half-extent drag. Same ImGui idioms already used for radius and
center in this file. ESTIMATE ~40 lines.

### 2.3 Gizmo — two more translate handles

In the existing gizmo block (`LightEditor.cpp:1105-1182`), when `hasTube`, emit
two more `ImGuizmo::Manipulate` calls with `SetID(1)`/`SetID(2)`, feeding
point-as-`idMat4` for `tubeA`/`tubeB`, translate-only, reading world translation
back on `IsUsing()`. Identical structure to the existing origin gizmo; reuse the
same `cameraView`/`cameraProjection`. Box proxy reuses the existing
translate/rotate/scale gizmo already wired to `origin`/`angles` + a scale→half-
extent readout (the code already decomposes scale at `:1184-1188`).
ESTIMATE ~60-90 lines.

### 2.4 Live preview

In `idEditEntities` display (next to `GameEdit.cpp:757`), when the selected light
has a tube/box proxy: `gameRenderWorld->DebugLine( colorCyan, A, B )` +
`DebugSphere` endpoints, or `DebugBox( colorCyan, idBox(…) )`. ESTIMATE ~15 lines.

### 2.5 How the parked primitive consumes the authored A,B (separate from the UI)

This is the renderer-side follow-on, **not** part of the authoring UI, and it is
the plumbing already enumerated in `docs/softshadow/cylinder-light-evaluation.md`
§4: carry two `float4` (A.xyz, B.xyz) as one more cbuffer entry through
`SoftShadowTermPass.cpp:27`, `SoftTileBinPass.cpp:301`,
`SoftShadowClassify.cpp:39`, and the `RenderBackend.cpp` `swParm` sites
(`:1797`, `:1995`), sourced from new `renderLight_t`/`idRenderLightLocal` parms
populated at light spawn from the `soft_tube_a/b` spawnargs (parallel to how
`R_SoftPenumbraRadius` derives extent today). ESTIMATE ~60-120 lines across those
sites; independent of the UI and gated behind the primitive landing off its
worktree. **Do not edit `SoftShadowTermPass.cpp` / `softterm.cs.hlsl` now — a
merge agent owns them.**

---

## 3. Go / no-go, risks, size, proof-of-life

**Verdict: GO — build on the existing `LightEditor` + ImGuizmo spine.** Not
"ImGuizmo raw" (that would re-source matrices and input that the light editor
already solved) and not the legacy Radiant editor. Both the gizmo and the light
editor are compiled and linked here; the override writer exists. This is
extension, not construction.

### Decisive risks

1. **"Builds" ≠ "runs" (the one risk to retire first).** Everything above is
   VERIFIED as compiled/linked, but I did not launch the game (constraint). The
   `g_editEntityMode → InitTool(EDITOR_LIGHT) → ImGui` path could have bit-rotted
   in this fork (input capture, ImGui pump ordering, gizmo picking under this
   renderer). This is retired by the proof-of-life below, not by more reading.
2. **Non-GLTF save clobber.** `MapSave` only writes the `_extra_ents` diff for
   `IsGLTF()` maps; the `else` branch (`GameEdit.cpp:1218`) overwrites the whole
   `.map`. Erebus1 is GLTF so this is safe here, but a proxy-authoring session on
   a non-GLTF map would rewrite the source map. ESTIMATE: guard/opt-in if the UI
   is ever used off GLTF maps.
3. **Coordinate convention.** `soft_tube_a/b` as absolute world coords is
   simplest for the gizmo (world translation out) but does not follow a moving
   light. Lights here are static (`lightHasMoved` sticky), so acceptable;
   light-relative offsets are the upgrade if movers ever need tubes.
4. **Two-value invariant.** A tube needs both endpoints; a half-authored light
   (only `soft_tube_a`) must fall back cleanly. One guard in `FromDict`
   (`hasTube = gotA && gotB`).

### Size estimate (UI only; the §2.5 plumbing is separate)

- `LightInfo` fields + FromDict/ToDict keys: ~30 lines (`LightEditor.cpp`/`.h`).
- Panel section: ~40 lines (`LightEditor.cpp`).
- A/B translate gizmos + box gizmo readout: ~60-90 lines (`LightEditor.cpp`).
- Live DebugLine/DebugBox preview: ~15 lines (`GameEdit.cpp`).
- **Total UI: ~145-175 lines, essentially one TU.**
- Renderer consumption plumbing (§2.5, later, off-worktree): ~60-120 lines.

### Cheapest proof-of-life (retires risk #1, zero code)

Launch erebus1, `g_editEntityMode 1`, click a light, confirm the ImGui light
editor opens **with a working translate gizmo**, nudge the light, Ctrl+S, and
confirm `~/.local/share/rbdoom3bfg/base/maps/game/erebus1_extra_ents.map` gains
the override stanza. That single manual test proves the entire spine (selection
→ editor → gizmo → `_extra_ents` writer) is live. If a code hello-world is
wanted instead: add one `gameRenderWorld->DebugLine` between two fixed offsets of
the selected light's origin in the edit-display loop and confirm it draws —
proving live proxy preview end-to-end.
