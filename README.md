## In-development, an injectable DLL which adds custom additions to the Gunfight OSP Blueprints mode of Modern Warfare 2019.
## Tested on version 1.41 with IW8-Mod odin/v3.0.0-milestone2_public-beta

# What this currently does:
 - Adds T9 (Cold War) snipers to the "Random Snipers" custom games option within OSP Gunfight modes
 - Player will now spawn with Fists instead of no secondary in Gunfight OSP Blueprints
 - Blueprint pool for OSP Blueprints is slightly expanded (Will expand more in the future)

<img width="1920" height="1080" alt="Capture" src="https://github.com/user-attachments/assets/17893b08-038d-43ac-a204-566801b9a00d" />


# How to use:
- Open MW2019/IW8-Mod, navigate to custom games, set up your gunfight match and game settings, etc.
-  Take your DLL and inject it. The injector I use myself for 1.41 is https://github.com/raminkarimkhani1996/DLLInjector/tree/master
-  Start your match, and enjoy

# Plans for the future:
-  Get attachments from Gunfight OSP Classtable for use ingame
-  Fill OSP Classtable with all current blueprints available
-  Add remaining T9/Misc weapons to their categories under Gunfight OSP

#









Made with ChatGPT's Codex Software.
AI's Notes below:





# IW8 sidecar - OSP Blueprints full sniper package

Expected build ID:

```text
OVERLAY_CYCLIC_EXPANDED_POOL_OSP_BLUEPRINT_GUNSMITH_ATTACH_ID0_HOOK_BUILD_ID 2026-06-24
```

Purpose:
- Keep the normal Arena classtable override path available.
- Add OSP / OSP Blueprints weapon-pool overlays.
- Add `classtable_arena_blueprints.csv` with one primary class per weapon root, including former secondaries in the primary slot.
- Keep secondaries, equipment, perks, and overkill neutralized in the blueprint class table.
- Keep `rand_weapon` only on the sniper/DMR rows in `arenaggweapons.csv`.
- Include the attachmentmap-inclusive external string pool as `stringpool.csv`.
- Synthesize matching `mp/gunsmith/*_variants.csv` custom attachment cells from `classtable_arena_blueprints.csv`, so OSP Blueprints can load the same attachment tokens through the stock variant path.
- Redirect `mp/attachmentmap.csv` scope cells from the first CSV-selected optic per weapon root when the source row includes an optic token.

Install:
```text
DLL -> C:\Games\COD

assets\mp\classtable_arena.csv
assets\mp\classtable_arena_blueprints.csv
assets\mp\arenaggweapons.csv
assets\mp\stringpool.csv
-> <game>\.iw8-mod\assets\mp\
```

Recommended test:
```text
Loadouts: OSP Blueprints
Change Loadout: Every Round
Starting Weapon: Random Sniper or Random Weapon
Item 1-8: None
OSP Attachments: leave game-rule optic overrides off when testing CSV-selected optics.
```

Useful log lines:
```text
OVERLAY_CYCLIC_EXPANDED_POOL_OSP_BLUEPRINT_GUNSMITH_ATTACH_ID0_HOOK_BUILD_ID
Target string_table: mp/arenaggweapons.csv
Target string_table: mp/attachmentmap.csv (scope cells redirected from CSV-selected optic per weapon root)
Target string_table pattern: mp/gunsmith/*_variants.csv
Overlayed loose string_table 'mp/arenaggweapons.csv'
Overlayed loose string_table 'mp/classtable_arena_blueprints.csv'
Overlayed attachmentmap CSV scope redirect for 'mp/attachmentmap.csv'
Overlayed gunsmith variant attachments for 'mp/gunsmith/sn_t9standard_variants.csv'
Generic OSP table overlay stats
Attachmentmap CSV scope overlay stats
Gunsmith variant attachment overlay stats
missingStringsKeptStock=0
```

Notes:
- `arenaggweapons.csv` controls the OSP Random Sniper pool.
- `classtable_arena_blueprints.csv` is the blueprint/variant and custom attachment source.
- For OSP Blueprints, the DLL now patches matching gunsmith variant rows whose column 0 variant IDs are listed in `loadoutPrimaryVariantID`, writing `loadoutPrimaryAttachment1-5` into columns 5-15 as `token|0`.
- The normal `classtable_arena.csv` override is also included and still supported by the DLL.
- If OSP Blueprints still spawns plain weapons, check the log for `Overlayed gunsmith variant attachments`; if that line is missing, the real path is requesting a different variant table or loading loot data before this hook sees it.
