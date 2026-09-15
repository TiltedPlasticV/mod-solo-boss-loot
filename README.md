# mod-solo-boss-loot

An AzerothCore module for solo players. When you loot a dungeon or raid boss, or a boss chest, the module adds that boss's own loot for your class, so you can gear up without farming the same boss over and over. Rare drops from other creatures also show up more often.

Built for the mod-playerbots fork of AzerothCore. Bots are ignored: only real players count.

## What gets added

- Items that only bosses drop (no trash or world drops)
- Gear your class can use: your weapon types, your main armor type, cloaks, rings, necks, trinkets, shields and relics
- Mounts, pets, bags and recipes you haven't learned yet
- Hard-mode loot, only when you did the hard mode

The normal loot roll still happens; the module only adds items. Anything you already own is skipped. If there is more loot than the 18-slot loot window can hold, a random selection is added.

## World drops

Other creatures (not bosses, not in battlegrounds) sometimes add one extra rare item:

- Rare means it normally drops less than 5% of the time, is green or better, and is something you keep: gear, bags, recipes, mounts or pets
- Very rare items get the biggest boost, and blues and epics are favored over greens
- The item must be one a real player in your group can use and doesn't have yet
- Recipes only drop for professions someone in your group has
- Quest items are never boosted

The boost is worked out from your world database at startup. If you raised drop chances there by hand, revert those edits first.

To try world drop settings without starting the server, run `apps/world_drop_sim.py` (Python 3.8 or newer). It reads the world database SQL that ships with AzerothCore and your config, and shows how often each creature drops rare items before and after the boost. Run it with `--help` for the options.

## Installation

1. Clone this repository into your AzerothCore `modules` folder. The folder must be named `mod-solo-boss-loot`.
2. Re-run CMake and rebuild.
3. Make sure `data/sql/db-world/base/solo_boss_loot_overrides.sql` is applied to your world database. The database updater does this automatically if it's enabled.
4. Copy `mod_solo_boss_loot.conf.dist` to `mod_solo_boss_loot.conf` in your configs folder.

## Configuration

| Setting | Default | Description |
|---|---|---|
| `SoloBossLoot.Enable` | 1 | Turn the module on or off |
| `SoloBossLoot.SkipOwnedItems` | 1 | Skip items you already have (equipped, in bags or in the bank) |
| `SoloBossLoot.SharedPoolThreshold` | 3 | Items dropped by more than this many bosses are only added if you keep them (gear, bags, mounts, pets, recipes) |
| `SoloBossLoot.WorldDrop.Enable` | 1 | Turn world drops on or off |
| `SoloBossLoot.WorldDrop.MinQuality` | 2 | Lowest quality that counts as rare (2 green, 3 blue, 4 epic) |
| `SoloBossLoot.WorldDrop.MaxItemLevel` | 0 | Only boost items below this item level (0 for no limit) |
| `SoloBossLoot.WorldDrop.MaxItemChance` | 5 | Items dropping at least this often (%) aren't boosted, and boosted items stay below it |
| `SoloBossLoot.WorldDrop.MaxCombinedChance` | 15 | Chance (%) that one of a creature's rare items drops |
| `SoloBossLoot.WorldDrop.CompressionRatio` | 0.8 | How much rare items' chances are evened out (0 to 1) |
| `SoloBossLoot.WorldDrop.QualityWeights` | "1 3 6" | Weights for green, blue and epic items |
| `SoloBossLoot.WorldDrop.RecipeWeight` | 0.75 | Extra weight for recipes |

## Logging

Add `Logger` lines to `worldserver.conf` (they don't work in the module config):

- `Logger.module.solo_boss_loot`: startup and settings
- `Logger.module.solo_boss_loot.boss`: boss kills and boss chests
- `Logger.module.solo_boss_loot.world_drop`: other creature kills

Level 5 (debug) shows what each kill added. Level 6 (trace) also shows why items were skipped. For example:

```
Logger.module.solo_boss_loot=5,Console Server
```

## Overrides

The `solo_boss_loot_overrides` world table lists the boss chests, and lets you mark extra bosses or force items in or out. The SQL file explains the columns.

Changes to the settings or the table apply on server restart or `.reload config`.
