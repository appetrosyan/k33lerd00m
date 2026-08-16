# Soft-shadow capture corpus (.softcap)

Naming convention: **`<map>_<NN>.softcap`** — the map the capture was taken on, plus a two-digit
index in chronological capture order (game time). One file per unique scene; no duplicates.

The whole directory is the corpus of the GPU defect gate
(`RBDOOM_HIDDEN_WINDOW=1 ./RBDoom3BFG +set com_softShadowGate corpus ...`): every `*.softcap`
here is reconstructed and gated, so a new capture becomes part of the suite just by being
dropped here under the convention. `attic/` is excluded (parked duplicates/degenerates).

Provenance of the rename (2026-08-16), for archaeology against old logs/branches:

| current      | was          | native res | note                                   |
|--------------|--------------|------------|----------------------------------------|
| erebus1_01   | erebus0      | 320x200    |                                        |
| erebus1_02   | erebus1      | 320x200    |                                        |
| erebus1_03   | erebus2      | 320x200    |                                        |
| erebus1_04   | erebus3      | 320x200    |                                        |
| erebus1_05   | erebus4      | 320x200    |                                        |
| erebus1_06   | erebus5      | 320x200    | penumbra-rich                          |
| erebus1_07   | erebus6      | 320x200    |                                        |
| erebus1_08   | erebus7      | 320x200    | many-chain debris casters              |
| erebus1_09   | erebus13     | 320x200    | penumbra-rich, 8 lights                |
| erebus1_10   | erebusART    | 2560x1440  | == softcap0014 (dupe parked in attic)  |
| erebus1_11   | erebusART2   | 2560x1440  | == softcap0015 (dupe parked in attic)  |
| erebus1_12   | softcap0012  | 2560x1440  | tripod thin legs                       |
| erebus1_13   | softcap0013  | 2560x1440  | tripod thin legs                       |
| (attic)      | erebusART3   | 2560x1440  | degenerate: zero lights/casters        |
| erebus1_14   | softcap0039  | 2560x1440  | play-test halo report (3 lights)       |

Native res is the resolution the capture was TAKEN at (affects only the stored depth block used
by the CPU `@study` instruments); the gate re-renders every capture live at >= 1920x1080.
