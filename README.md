# IW8 sidecar - OSP Blueprints full sniper package

Expected build ID:

```text
OVERLAY_CYCLIC_EXPANDED_POOL_OSP_BLUEPRINT_GUNSMITH_ATTACH_ID0_HOOK_BUILD_ID 2026-06-24
```

Purpose:
- Keep the normal Arena classTable override path available.
- Add OSP / OSP Blueprints weapon-pool overlays.
- Add a compact `classTable_arena_blueprints.csv` with one of each sniper from the earlier table.
- Fill every sniper with 5 attachment category tokens chosen from `attachmentmap.csv`.
- Preserve available stock blueprint variant ID lists for MW weapons already in `classTable_arena_blueprints.csv`.
- Use `0 1` variant IDs for T9/CW snipers that are not already in the stock blueprint table.
- Include the attachmentmap-inclusive external string pool.
- Synthesize matching `mp/gunsmith/*_variants.csv` custom attachment cells from `classTable_arena_blueprints.csv`, so OSP Blueprints can load the same attachment tokens through the stock variant path.

Install:
```text
DLL -> C:\Games\COD

assets\mp\classTable_arena.csv
assets\mp\classTable_arena_blueprints.csv
assets\mp\arenaGGWeapons.csv
assets\mp\arenaGGWeapons_alt.csv
assets\mp\wz_gulag_extra_string_pool_for_sidecar.csv
-> <game>\.iw8-mod\assets\mp\
```

Recommended test:
```text
Loadouts: OSP Blueprints
Change Loadout: Every Round
Starting Weapon: Random Sniper
Item 1-8: None
OSP Attachments: Enabled first; if attachments conflict, test Disabled too.
```

Useful log lines:
```text
OVERLAY_CYCLIC_EXPANDED_POOL_OSP_BLUEPRINT_GUNSMITH_ATTACH_ID0_HOOK_BUILD_ID
Target string_table: mp/arenaGGWeapons.csv
Target string_table: mp/arenaGGWeapons_alt.csv
Target string_table pattern: mp/gunsmith/*_variants.csv
Overlayed loose string_table 'mp/arenaGGWeapons.csv'
Overlayed loose string_table 'mp/classTable_arena_blueprints.csv'
Overlayed gunsmith variant attachments for 'mp/gunsmith/sn_t9standard_variants.csv'
Generic OSP table overlay stats
Gunsmith variant attachment overlay stats
missingStringsKeptStock=0
```

Notes:
- `arenaGGWeapons.csv` controls the OSP Random Sniper pool.
- `classTable_arena_blueprints.csv` is the likely blueprint/variant source.
- For OSP Blueprints, the DLL now patches matching gunsmith variant rows whose column 0 variant IDs are listed in `loadoutPrimaryVariantID`, writing `loadoutPrimaryAttachment1-5` into columns 5-15 as `token|0`.
- The normal `classTable_arena.csv` override is also included and still supported by the DLL.
- If OSP Blueprints still spawns plain weapons, check the log for `Overlayed gunsmith variant attachments`; if that line is missing, the real path is requesting a different variant table or loading loot data before this hook sees it.
