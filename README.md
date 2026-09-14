# mod-solo-boss-loot

An AzerothCore module for solo players. When you loot a dungeon or raid boss, or a boss chest, the module adds that boss's own loot for your class, so you can gear up without farming the same boss over and over.

Built for the mod-playerbots fork of AzerothCore. Bots are ignored: only real players count.

## What gets added

- Items that only bosses drop (no trash or world drops)
- Gear your class can use: your weapon types, your main armor type, cloaks, rings, necks, trinkets, shields and relics
- Mounts, pets, bags and recipes you haven't learned yet
- Hard-mode loot, only when you did the hard mode

The normal loot roll still happens; the module only adds items. Anything you already own is skipped. If there is more loot than the 18-slot loot window can hold, a random selection is added.

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

## Overrides

The `solo_boss_loot_overrides` world table lists the boss chests, and lets you mark extra bosses or force items in or out. The SQL file explains the columns.

Changes to the settings or the table apply on server restart or `.reload config`.
