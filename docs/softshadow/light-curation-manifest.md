# Class-1 light curation — erebus1 (RoE intro bench), first conservative pass

Override file: `/home/app/.local/share/rbdoom3bfg/base/maps/game/erebus1_extra_ents.map`
(fs_savepath copy = shipped 66 env_probe entities byte-identical + 6 `noshadows 1`
override stanzas, entities 66-71). Data-only; no engine code, no rebuild.

## Why that path wins (merge-order evidence)

- `idMapFile::Parse` merges `<mapname>_extra_ents.map` onto named entities:
  `neo/idlib/MapFile.cpp:1837-1894`. Match is by `"name"`; every non-name key
  overrides (empty value deletes); an UNMATCHED non-worldspawn entity is APPENDED
  as a new entity — so a typo'd name silently spawns a broken entity instead of
  overriding. All 6 names verified against `erebus1_lights.json`.
- The extras file is opened through the normal filesystem search
  (one file wins outright — no cross-searchpath merging):
  - `neo/framework/FileSystem.cpp:3266-3274` — `SetupGameDirectories` adds
    fs_basepath first, fs_savepath second;
  - `neo/framework/FileSystem.cpp:3788` — `OpenFileReadFlags` walks
    `searchPaths` LAST-added-first ⇒ **fs_savepath (`~/.local/share/rbdoom3bfg`)
    beats fs_basepath (game dir)**;
  - `neo/framework/FileSystem.cpp:343` — `fs_resourceLoadPriority` defaults `0`
    ⇒ loose dirs are searched before .resources anyway; and
    `base/maps/erebus1.resources` contains **no** `extra_ents` entry at all
    (grep over the whole 135 MB pack, .map is embedded plain text — exit 1).
- Consequence: the savepath file **entirely shadows** the shipped
  `base/maps/game/erebus1_extra_ents.map` (66 env_probes). The savepath copy
  therefore carries those 66 entities verbatim (md5-verified before appending).

## Curated (6 lights → noshadows 1)

| name | origin | light_radius | texture | justification |
|---|---|---|---|---|
| light_53828 | -1328 -1823 1364 | 16 16 24 | lights/squareishlight | screen-glow: 8u in front of cpu_top.gui screen (y=-1831); no emissive-geometry link in data; radius so small any shadow is all-penumbra |
| light_53838 | -1392 -1823 1364 | 16 16 24 | lights/squareishlight | screen-glow, same bank, middle screen |
| light_53839 | -1456 -1823 1364 | 16 16 24 | lights/squareishlight | screen-glow, same bank, third screen |
| light_53807 | 1284 2020 1656 | 64 96 112 | (none = default point) | intro-zone texture-less fill; entity scan found ZERO model-bearing entities inside the radius box |
| light_52618 | 640 2912 1712 | 88 96 152 | (none) | intro-zone texture-less fill; only one static (d3xp_temple_object at beam edge) in radius box |
| light_53776 | 624 2352 1440 | 128 96 112 | (none) | intro-zone texture-less fill; zero model-bearing entities in radius box; lightest probe on cap0012 (2069 edges / 8 casters) |

Note: the study's "8 texture-less fills in the intro zone" includes 3 that are
already `noshadows 1` in the map (light_53773, light_53816, light_53820) —
overriding them is a no-op, so they are not in the file.

## Considered and EXCLUDED

| name | origin | light_radius | texture | why left alone |
|---|---|---|---|---|
| light_52609 | 896 3200 1856 | 480 496 432 | (none) | near-hall-scale fill over the hellhole set piece: pillars, skgenerator, d3xp_orbholder, temple objects AND six `moveable_base_brick` movers inside the beam — plausibly drives visible dramatic shadows |
| light_53772 | 864 2096 1576 | 384 352 352 | (none) | large fill with d3xp_brokenwall + staircave statics in beam; large-radius + large occluders |
| light_53824 / 52622 / 53771 / 53810* / 53811 / 53818 | intro zone | 8-160 | lights/spot01 | projected texture = authored directional look; not class-1 fills (*53810 already noshadows) |
| light_52536 / 53765 / 53817 | intro zone | 96-400 | lights/biground1 | projected texture; 53765 is a probed term light on cap0012 (index 2, 13587 edges) — leave its shadow alone |
| implight | 624 1992 1912 | 90 90 90 | lights/impflyflash | scripted effect light |

## Cap-corpus intersection (from `~/.local/share/rbdoom3bfg/base/cap/*.cap.json` sidecars, v6, per-light origins)

- **cap0012** (vieworg 1003 2152 1652 — the intro-zone cap): probe light
  **index 5 = light_53776** (origin match 624 2352 1440). cap0012 has 7 probe
  lights → drops to 6 after recapture. **No SETUP tripwire**: no cap loses all
  its soft lights.
- All other active caps (0001-0011, 0015) and the attic: zero intersections
  with the curated set (exact origin match, tol 2u). The two excluded large
  fills are not probed by any cap either.
- The screen-glow trio is not in any cap probe set — its win, if any, shows
  only on the live cinematic / a future CPU-room cap.

## A/B instructions (no cvar exists — file rename IS the toggle)

1. OFF (baseline): `mv ~/.local/share/rbdoom3bfg/base/maps/game/erebus1_extra_ents.map{,.off}`
   — the shipped fs_basepath copy (env_probes only) takes over; map is stock.
   ON: rename back. Never delete: the file also carries the 66 shipped env_probes.
2. The override lands at map parse ⇒ **full map (re)load between A and B**
   (fresh `devmap erebus1` per the bench recipe); a running map won't pick it up.
3. Recapture cap0012 first (`softShadowRecapture`) — its fixture pins
   light_53776's records; stale fixture = phantom-defect territory. Other caps
   need no recapture (no intersection), but the stale-capture rule says verify.
4. Gate on recaptured cap0012 must stay 0 defects (6 remaining lights).
5. Bench: RoE intro cinematic, fresh `devmap erebus1`, `com_fixedTic 1` (or the
   gate bench), BOTH 1080p and 1440p, report med/p99/max.
6. Eyes-on pass (risk E2): watch the tagged lights' receivers live for missing
   mid-size-occluder shadows — cinematic characters are script-moved and the
   frozen caps can't see them.

## Risks

- E2 (occluder-size): all 6 curated lights have small radii (16-152 on the
  fills' short axes) and clean beam boxes; the two lights where the argument
  fails were excluded.
- Ground-truth redefinition (E5): for these 6 lights the per-class truth is now
  `term == 1`; the gate enforces it by absence-of-probe after recapture.
- Name-append hazard: a wrong `"name"` in an override stanza spawns a NEW
  light entity instead of overriding (MapFile.cpp:1883-1891). Names were
  cross-checked against the parsed table.
