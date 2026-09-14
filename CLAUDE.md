# mod-solo-boss-loot: notes for Claude

This file holds all the rules and context for this project. Keep it up to date when decisions change.
Don't use Claude's memory files; anything worth remembering goes here so everyone can see and edit it.

## Hard rules
- Only edit files inside this module (`modules/mod-solo-boss-loot` in the AzerothCore source tree).
- Read-only, never edit:
  - the AzerothCore core source (the mod-playerbots fork of AzerothCore)
  - any other module under `modules/`
- Reading core and other modules to understand hooks and data is fine.
- If something seems to need a core or other-module change, say so and find a way to do it inside this module
  (existing hooks, this module's own `data/sql`).
- **Never compile or build anything** (no cmake, MSBuild, make, etc.). The developer compiles and tests changes themselves.
  Check code by reading it carefully against the core headers instead.
- **README.md:** keep it very simple and concise. Never use em-dashes in it.

## Who this is for
A player going solo through the game with playerbots, who should get the fun loot they'd never see on a normal playthrough.

Goals:
- One Molten Core run (for example) should get the player most of their MC gear.
- Very rare drops (e.g. Baron Rivendare's Deathcharger's Reins, 1%) should drop at the same chance as that boss's other unique items.
- 100% of every boss's unique loot isn't expected (some bosses have more than the 18-slot loot window). Aim for a good middle ground.
- Class filtering is fine, but it must never cause quest items or misc items to be lost.

## How boss loot works
Files:
- `src/mod_solo_boss_loot.cpp`: all the code
- `conf/mod_solo_boss_loot.conf.dist`: settings `SoloBossLoot.Enable`, `.SkipOwnedItems`, `.SharedPoolThreshold`
- `data/sql/db-world/base/solo_boss_loot_overrides.sql`: overrides table and the boss chest list

History: this module started as a fork of hallgaeuer/mod-dynamic-loot-rates. Its dungeon/raid loot rate settings
(`Dungeon.Rate.*`, `Raid.Rate.*`, via `OnAfterCalculateLootGroupAmount` / `OnAfterRefCount`) and a first
"guaranteed boss loot" attempt (`Boss.GuaranteedLoot` / `SharedThreshold`) were removed and replaced by the design below.
Boosting non-boss loot is out of scope for now.
Rejected idea: extra hidden re-rolls of the boss table (very rare items still wouldn't show up).

### At startup and on `.reload config`: build a loot list per boss loot table
- **Boss creatures:** templates with the `CREATURE_FLAG_EXTRA_DUNGEON_BOSS` flag or the `CREATURE_TYPE_FLAG_BOSS_MOB` type flag,
  plus their difficulty versions, adjusted by override rows.
- **Boss chests:** only from the override table (a curated list, including chests that scripts summon).
  Auto-detecting chests would pick up herb/ore nodes and trash chests.
- **What belongs to a boss:** an item counts only if nothing but boss loot tables (boss creatures + boss chests)
  use it, directly or through references. No trash mob, world-drop list, container, skinning table, etc.
  - e.g. Roogug's "rare" BoEs are also dropped by 28 non-boss sources, so they're excluded.
  - e.g. MC's recipe reference is only used by MC bosses, so it counts.
- Each loot table is loaded with one query. Rebuilding on `.reload config` causes a short hitch.
- **LootMode:** each item keeps its LootMode paths. Every mask on the way through references must match the kill's
  loot mode, the same as core's roll does, so hard-mode loot only drops for hard modes.
- **Skipped at build time:**
  - quest-required rows (left to the normal roll)
  - items that aren't boss-only
  - grey items, and white armor/weapons
  - **shared pool items** (dropped by more than `SharedPoolThreshold` boss loot tables, default 3) that aren't
    "keepable". Keepable = armor, weapons, bags, quivers, recipes, mounts, pets. Used-up or turned-in shared items
    (materials, gems, consumables, quest items, class tokens) would repeat at every boss because the owned check
    can't see them after use. Items unique to one boss are kept whatever their type.
- **Item overrides:** Mode 1 skips the boss-only, quality and shared-pool checks. Mode 0 never adds the item.

### At each boss kill or boss chest opening
- Only on dungeon/raid maps. Creature loot: `IsDungeonBoss()` or `isWorldBoss()` (or a creature override).
  Chest loot: a listed chest.
- The normal roll happens first, unchanged. The module only **adds** items and never removes any.
- **Bots are ignored completely** (`WorldSession::IsBot()`). Only real players in the group who are in the same
  instance count. If there are none, nothing is added.
- An item is added if **any** real player wants it:
  - `AllowableClass` / `AllowableRace` allow them (tier tokens, class books for other classes are skipped)
  - **gear usable:** weapons their class can use (static table per class), their **main armor type only**
    (highest armor skill they have: plate for a 40+ warrior, mail below 40), cloaks, misc armor
    (rings, necks, trinkets, off-hands), shields (warrior/paladin/shaman), their class's relic type
  - **not already learned:** recipes, mounts and pets (`Spells[1]` with the learn-spell trigger)
  - **not owned** (equipped, bags, bank) when `SkipOwnedItems = 1`
  - `LootItem::AllowedForPlayer` passes (conditions, faction, recipes hidden without the profession,
    finished quest starters), so items they couldn't see don't waste slots
  - This means **any recipe not yet known** is added, even for professions the player doesn't have,
    unless core hides it from them.
- Items already in the loot window, or not matching the kill's LootMode, are skipped.
- **Specials get no preference.** Everything wanted is treated the same:
  if it all fits in the free slots it all drops (no shuffle); if not, a random pick is made with equal odds for each item.
- Items are added with core `Loot::AddItem(LootStoreItem)` (conditions copied via `LootTemplate::CopyConditions`),
  so stack splitting, the 18-slot cap, `unlootedCount` and group loot work like a normal drop.

### Data behind the decisions (analysis of the base AzerothCore world DB)
- Simulated MC run (normal roll takes 4 slots): a warrior gets 47 of 48 usable MC gear pieces, priest 37/37, hunter 43/43.
- Forcing used-up shared items doubled the boss tables over the slot limit for a warrior (25 to 56).
  "Anything equippable" armor instead of main armor type doubled it again (118).
- `isWorldBoss()` in instances: 198 creatures, only one non-boss (Garrosh Hellscream, rank 1).

### Known gaps / ideas
- `.reload` of loot tables alone doesn't rebuild the cache; run `.reload config` afterwards.
- Chest of The Seven (BRD) is listed, but its gear also drops from BRD trash, so it adds nothing.

## Core facts worth knowing (paths relative to the AzerothCore source root)
- **Loot flow:** `Loot::FillLoot` (`src/server/game/Loot/LootMgr.cpp`) runs `LootTemplate::Process`, then the
  `OnAfterLootTemplateProcess` hook, and only then assigns group loot rights.
  `Loot::clear()` does not reset `sourceWorldObjectGUID`.
- **Boss flag:** `CREATURE_FLAG_EXTRA_DUNGEON_BOSS` is set at runtime from `instance_encounters` kill-credit rows only
  (`ObjectMgr::LoadInstanceEncounters` in `src/server/game/Globals/ObjectMgr.cpp`). It is not stored in the DB.
- `Creature::isWorldBoss()` checks the boss type flag (`src/server/game/Entities/Creature/Creature.h`).
- `WorldSession::IsBot()` (`src/server/game/Server/WorldSession.h`) comes from the mod-playerbots fork of AzerothCore.
- **Loot API:** `LootTemplate` entries and groups are private. `LootStoreItem` has a public constructor.
  `LootTemplate::CopyConditions(LootItem*)` also searches references.
- `Player::HasItemCount(item, count, inBank)` checks equipped items, bags, keyring/currency and optionally the bank.
- **Learnable items:** `Spells[0]` is the learn spell (483 recipes, 55884 mounts/pets),
  and `Spells[1]` (trigger `ITEM_SPELLTRIGGER_LEARN_SPELL_ID`) is the spell taught.
- **Heroic creatures:** `GetCreatureTemplate()` returns the difficulty template (use its `lootid`). `GetEntry()` is the normal entry.
- **Config and startup:** `.reload config` calls `OnBeforeConfigLoad` / `OnAfterConfigLoad(reload=true)`
  (`src/server/game/World/World.cpp`). `OnStartup` runs from worldserver `Main.cpp` after `SetInitialWorldSettings`.
- **Module loader:** CMake generates a call to `Add<folder name with - replaced by _>Scripts()` (`modules/CMakeLists.txt`),
  so the module folder must be named `mod-solo-boss-loot` to match `Addmod_solo_boss_lootScripts()` in the loader file.
- **Module SQL:** the DB updater applies any `data/sql/<dir>` whose name contains the DB name (`world`), recursively
  (`src/server/database/Updater/UpdateFetcher.cpp`).
- **Offline analysis:** base world DB SQL is in `data/sql/base/db_world/*.sql`.
  A live DB may differ because other modules change loot.
