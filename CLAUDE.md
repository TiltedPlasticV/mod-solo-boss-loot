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
- **Docs (README.md, CLAUDE.md, the conf):** when something changes, rewrite the existing text so it describes how things
  work now. Don't add new sections or notes about the change, and no deprecation or migration notes (the developer is
  the only user, so settings can simply be renamed). README.md stays very simple and concise, never uses em-dashes,
  and leaves out anything the config file already makes clear.

## Who this is for
A player going solo through the game with playerbots, who should get the fun loot they'd never see on a normal playthrough.

Goals:
- One Molten Core run (for example) should get the player most of their MC gear.
- Very rare drops (e.g. Baron Rivendare's Deathcharger's Reins, 1%) should drop at the same chance as that boss's other unique items.
- 100% of every boss's unique loot isn't expected (some bosses have more than the 18-slot loot window). Aim for a good middle ground.
- Class filtering is fine, but it must never cause quest items or misc items to be lost.
- Super rare world drops (rare greens at low level, blues, epics) should turn up now and then, without filling bags
  and without boosting common drops at all.

Files:
- `src/mod_solo_boss_loot.cpp`: all the code
- `conf/mod_solo_boss_loot.conf.dist`: settings `SoloBossLoot.SkipOwnedItems`, `.SharedPoolThreshold`, `.BossLoot.Enable`, `.WorldDrop.*`.
  Boss loot and world drops are enabled separately; boss detection, overrides and the creature/reference loot rows
  are loaded for either, the boss-only item analysis (`BuildBossLoot`) only for boss loot
- `data/sql/db-world/base/solo_boss_loot_overrides.sql`: overrides table and the boss chest list
- `apps/world_drop_sim.py`: offline world drop simulator for testers (Python, reads the SQL dumps and config files).
  It mirrors the world drop C++ (`AddDropChances`, `IsWorldDropCandidate`, `BuildWorldDropTable`, `PlayerWantsItem`),
  so **update it whenever that algorithm or its settings change**. Running it is fine (it isn't a build).

## How boss loot works
History: this module started as a fork of hallgaeuer/mod-dynamic-loot-rates. Its dungeon/raid loot rate settings
(`Dungeon.Rate.*`, `Raid.Rate.*`, via `OnAfterCalculateLootGroupAmount` / `OnAfterRefCount`) and a first
"guaranteed boss loot" attempt (`Boss.GuaranteedLoot` / `SharedThreshold`) were removed and replaced by the design below.
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
    unless core hides it from them. (World drops differ: they need the profession.)
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

## How world drops work
History: replaces the developer's hand-run SQL scripts, which multiplied `creature_loot_template.Chance` for rare
weapon/armor rows (x150 below 0.02%, x35 otherwise, capped at 5%) and then compressed and throttled each creature
to a 15% combined chance. Doing it in the module needs no DB edits and follows DB updates.
If a world DB still has those edits, they must be reverted first or they get boosted twice.

Decisions (made with the developer):
- **Per item, not per row.** The boost works on the chance of one specific item dropping from a kill, followed through
  references and groups. Boosting rows would boost whole reference packs (greys and whites included), and
  misses vanilla level-band packs completely (see data below).
- **Keepable items only** (same list as boss loot), quality >= `MinQuality` (default green, rare greens are fun).
- **Wants check reused from boss loot**, plus: recipes need a real player with the profession (`RequiredSkill`).
- **At most one extra item per kill.**
- **No multiplier tiers.** The SQL's tiers barely mattered: 93.5% of tables were throttled anyway. Dropped for a simpler config.
- **Quality weights** (default 1/3/6) so the budget isn't mostly greens, and a **recipe weight** (default 0.75).
- **No item level limit** by default (the SQL used < 100, which left Northrend with almost no gear).

### At startup and on `.reload config`: build a world drop table per creature loot table
- **Which tables:** loot ids of every creature template that isn't a boss (same flags and overrides as boss loot),
  skipping boss difficulty versions.
- **Drop chance per item** (`AddDropChances`), mirroring core at `LOOT_MODE_DEFAULT` with the server rates:
  - ungrouped rows: `Chance x Rate.Drop.Item.<quality>`, references `Chance x Rate.Drop.Item.Referenced`, 100% rows always
  - groups (`LootGroup::Roll`): explicit chances take the roll in row order, the rest is shared by equal-chanced rows;
    no rates. Top-level groups repeat items `Rate.Drop.Item.GroupAmount` times.
  - references repeat `MaxCount x Rate.Drop.Item.ReferencedAmount` times, max depth 8
  - chances along a path multiply, several paths add up (expected drops, fine for rare items)
- **Rare items:** chance above 0 and below `MaxItemChance` (5%), keepable, quality >= `MinQuality`, below `MaxItemLevel`
  (0 = no limit), not quest-bound, not a quest starter, not a quest objective (`Quest::RequiredItemId`).
  Quest-required rows are skipped.
- **Boost** (`BuildWorldDropTable`):
  - if the rare items' combined chance is already >= `MaxCombinedChance`, the table gets nothing
  - compress toward the geometric mean: `chance^(1-r) x mean^r` (`CompressionRatio` r, default 0.8)
  - multiply by the quality weight (green, blue, epic+) and `RecipeWeight` for recipes
  - binary search a scale so the combined chance hits `MaxCombinedChance`; each item is floored at its original
    chance and capped at `MaxItemChance`
  - extra roll per item `(boosted - original) / (1 - original)`, table chance = combined extra chance
- Identical tables are shared (many creatures use the same reference packs).

### At each non-boss creature kill
- Creature loot only, default loot mode, not in a battleground or arena, creature isn't a boss (`IsBoss`: same check
  as boss loot, including overrides; this also covers open-world world bosses). The `lootid` must match the template.
- Roll the table chance first; everything else only runs on a hit (about 7-14% of kills).
- Needs a real player in the group, in the same map and at group reward distance (`IsAtGroupRewardDistance`).
- Wanted items: not already in the window, and `PlayerWantsItem` passes for any real player (with the profession check).
- A second roll accepts with `P(any wanted) / P(table)`, so each wanted item keeps its own chance and unwanted items
  just don't drop (they are not replaced).
- Pick one wanted item weighted by its extra chance. Only then copy conditions and check `AllowedForPlayer`
  (copying for hundreds of items would be slow); if that fails, drop it from the list and pick again.
- Added with `Loot::AddItem`, skipped if the window is full.

### Data behind the decisions (base world DB, all server rates 1.0, reproduce with `apps/world_drop_sim.py`)
- Rare drops are stored three ways: direct rows (Scarlet Monk's 0.02% blues), old-style packs (1-2% reference row to
  10 blues or ~100 greens, per item ~0.01-0.05%), and **vanilla level-band packs**: a `Chance 0` grouped reference
  (e.g. Defias Pillager: group 5 picks 1000114 or 1000115) to 150-400 mixed-quality items with their own groups.
  The SQL scripts left those at 0.
- 7,172 non-boss loot tables, 6,706 with rare items, 5,494 distinct rare items (no item level limit).
  With `MaxItemLevel = 100` it was 4,046 items, and level 71-80 tables had a median of 25 rare items instead of 231.
- With the defaults: 5,869 tables hit 15%, 491 are already above it, 346 (1-3 rare items) end with every item at 5%.
  6,215 tables get world drops, 2,045 of them unique.
- Per item median (gear): green 0.010% to 0.036%, blue 0.005% to 0.091%, epic 0.004% to 0.136%.
  Recipes: green 0.006% to 0.026%, blue 0.004% to 0.048%, epic 0.001% to 0.083%.
- Extra item chance per kill (median): 14% at level 1-10 down to 7% at 71-80. After the wants check for a warrior,
  hunter or priest with two professions: about 2-7%.
- Recipe share of bonus drops for a player with two professions: 12-16% at weight 1, 9-13% at 0.75, 7-9% at 0.5.

### Known gaps / ideas
- `.reload` of loot tables alone doesn't rebuild the cache; run `.reload config` afterwards.
- Loot modes other than default (rare for non-bosses) get no world drops.

## Logging
- **Loggers:** `module.solo_boss_loot` (`LOG_NAME`: startup, settings, cache builds), `.boss` (`LOG_BOSS`: boss kills
  and chests), `.world_drop` (`LOG_WORLD_DROP`: other creature kills). Unset loggers fall back to their parent,
  then to `Logger.module`. They must be set in worldserver.conf (see core facts).
- **Levels:**
  - Error: bad settings, unknown override `SourceType`
  - Warn: settings clamped, override rows for missing templates, boss chests without a loot id or loot rows,
    empty overrides table
  - Info: cache summaries, boss loot or world drops disabled
  - Debug: settings after load, build totals, each boss kill's summary with the items added, each world drop added
  - Trace: per item skip reasons at boss kills (with each real player's reason from `GetUnwantedReason`),
    world drop rolls that added nothing, one line per boss loot table at build time
- **Cost:** the `LOG_*` macros only evaluate their arguments when the level is on, so describe helpers
  (`DescribeItem`, `DescribePlayers`, ...) go inside the macro call. Item loops check `ShouldLog` once.
  No per-table trace for world drop tables at build time (thousands of them): `apps/world_drop_sim.py` covers that.

## Core facts worth knowing (paths relative to the AzerothCore source root)
- **Loot flow:** `Loot::FillLoot` (`src/server/game/Loot/LootMgr.cpp`) runs `LootTemplate::Process`, then the
  `OnAfterLootTemplateProcess` hook, and only then assigns group loot rights.
  `Loot::clear()` does not reset `sourceWorldObjectGUID`.
- **Roll rules:** `LootStoreItem::Roll` applies `Rate.Drop.Item.<quality>` (items) or `.Referenced` (references) to
  ungrouped rows; `LootGroup::Roll` ignores rates. `LootGroupInvalidSelector` only filters loot mode and duplicates.
- **`OnItemRoll` hook** gets the row and a modifiable chance, but not which loot table it belongs to, and nested
  reference rows look the same as top-level ones. Returning false in a group roll cancels the whole group.
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
  (`src/server/game/World/World.cpp`). Rates (`WorldConfig::Initialize`) are loaded before `OnAfterConfigLoad`.
  `OnStartup` runs from worldserver `Main.cpp` after `SetInitialWorldSettings`.
- **Logging config:** worldserver `Main.cpp` calls `sLog->Initialize` (reads `Appender.*` / `Logger.*`) before
  `LoadModulesConfigs`, so `Logger` lines in a module .conf are ignored at startup; they belong in worldserver.conf.
  `.reload config` re-reads loggers (`World::LoadConfigSettings`). `Log::GetLoggerByType` falls back to the parent by
  cutting at the last `.`, then `root`. Appenders also filter by their own level.
- **Module loader:** CMake generates a call to `Add<folder name with - replaced by _>Scripts()` (`modules/CMakeLists.txt`),
  so the module folder must be named `mod-solo-boss-loot` to match `Addmod_solo_boss_lootScripts()` in the loader file.
- **Module SQL:** the DB updater applies any `data/sql/<dir>` whose name contains the DB name (`world`), recursively
  (`src/server/database/Updater/UpdateFetcher.cpp`).
- **Offline analysis:** base world DB SQL is in `data/sql/base/db_world/*.sql`.
  A live DB may differ because other modules change loot.
