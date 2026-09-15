#!/usr/bin/env python3
"""
World drop visualiser for mod-solo-boss-loot.

Writes a self-contained HTML page (no internet needed) with sliders for the world drop settings. It shows, live:
  - one creature's rare items going through each step of the boost (compress, weight, scale, cap)
  - what every creature's world drops look like: extra item chance by level, which qualities and recipes drop,
    per-item chances, and how tables end up (hit the target, every item capped, already above it)
  - a sweep of one setting across its range

Drop chances per loot table are worked out here with world_drop_sim.py (reference tables, groups and the server's
Rate.Drop.Item.* rates are baked in). The page's JavaScript redoes BuildWorldDropTable and PlayerWantsItem, so keep
apps/world_drop_viz.template.html in sync with src/mod_solo_boss_loot.cpp as well.

Same data and approximations as world_drop_sim.py. Needs Python 3.8 or newer, no extra packages.
"""

import argparse
import array
import base64
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import world_drop_sim as sim  # noqa: E402

TEMPLATE = os.path.join(HERE, "world_drop_viz.template.html")
DEFAULT_OUT = os.path.join(HERE, "world_drop_viz.html")

# Items are exported up to the top of the page's MaxItemChance slider, from uncommon up and at any item level,
# so MaxItemChance, MinQuality and MaxItemLevel can change in the page too
EXPORT_MAX_ITEM_CHANCE = 25


def encode(typecode, values):
    data = array.array(typecode, values)
    if sys.byteorder != "little":
        data.byteswap()
    return base64.b64encode(data.tobytes()).decode("ascii")


def build(data, settings, export_settings, args):
    sim.progress("Working out drop chances...")
    table_keys = {}
    offsets, entry_items, entry_chances = [0], [], []
    loot_ids, loot_tables, loot_levels = [], [], []
    item_index = {}

    for loot_id in sorted(data.table_level):
        rows = data.creature_loot.get(loot_id)
        if not rows:
            continue
        rare = []
        for item_id, chance in sim.drop_chances(rows, data, export_settings).items():
            chance = min(1.0, chance)
            if sim.is_rare_item(item_id, chance, data, export_settings):
                rare.append((chance, item_id))
        if not rare:
            continue

        rare.sort()
        ids = [item_id for _, item_id in rare]
        chances = array.array("f", [chance for chance, _ in rare])
        key = (tuple(ids), chances.tobytes())
        table = table_keys.get(key)
        if table is None:
            table = table_keys[key] = len(offsets) - 1
            for item_id in ids:
                entry_items.append(item_index.setdefault(item_id, len(item_index)))
            entry_chances.extend(chances)
            offsets.append(len(entry_items))

        loot_ids.append(loot_id)
        loot_tables.append(table)
        loot_levels.append(data.table_level[loot_id])

    items = []
    for item_id in item_index:
        item = data.items[item_id]
        items.append([item_id, item.name, item.quality, item.cls, item.subclass, item.inventory_type,
                      item.allowable_class, item.item_level, item.required_skill])

    creatures = []
    for row, loot_id in enumerate(loot_ids):
        for entry in data.table_creatures[loot_id]:
            creature = data.creatures[entry]
            creatures.append([creature.name, entry, creature.max_level, row])
    creatures.sort(key=lambda creature: (creature[0].lower(), creature[1]))

    players = []
    for text in sim.DEFAULT_PLAYERS:
        player = sim.Player(text)
        players.append({"cls": player.cls, "professions": player.professions})

    rates = " ".join(f"{name} {rate:g}" for name, rate in zip(("Poor", "Normal", "Uncommon", "Rare", "Epic", "Legendary",
                                                                "Artifact"), settings.quality_rates))
    sim.progress(f"{len(loot_ids)} loot tables with rare items, {len(offsets) - 1} unique, {len(entry_items)} items rows, "
                 f"{len(items)} distinct items, {len(creatures)} creatures")

    return {
        "source": {
            "db": os.path.abspath(args.db),
            "conf": os.path.abspath(args.conf),
            "rates": f"{args.worldserver_conf or 'all rates 1 (no --worldserver-conf)'}: {rates}, "
                     f"Referenced {settings.referenced_rate:g}, ReferencedAmount {settings.referenced_amount:g}, "
                     f"GroupAmount {settings.group_amount}",
            "nonBossTables": len(data.table_level),
        },
        "settings": {
            "maxItemChance": settings.max_item_chance * 100,
            "maxCombinedChance": settings.max_combined_chance * 100,
            "compression": settings.compression,
            "qualityWeights": settings.quality_weights,
            "recipeWeight": settings.recipe_weight,
            "minQuality": settings.min_quality,
            "maxItemLevel": settings.max_item_level,
        },
        "exportMaxItemChance": EXPORT_MAX_ITEM_CHANCE,
        "rules": {
            "classMasks": sim.CLASS_MASKS,
            "classWeapons": {cls: sorted(weapons) for cls, weapons in sim.CLASS_WEAPONS.items()},
            "shieldClasses": sorted(sim.SHIELD_CLASSES),
            "armorRelics": {str(subclass): cls for subclass, cls in sim.ARMOR_RELICS.items()},
            "professions": sim.PROFESSIONS,
        },
        "players": players,
        "levelBands": sim.LEVEL_BANDS,
        "items": items,
        "tables": {
            "offsets": encode("I", offsets),
            "items": encode("H", entry_items),
            "chances": encode("f", entry_chances),
        },
        "loot": {"ids": loot_ids, "tables": loot_tables, "levels": loot_levels},
        "creatures": creatures,
    }


def main():
    parser = argparse.ArgumentParser(
        description="Build an interactive HTML page showing what the mod-solo-boss-loot world drop settings do.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="examples:\n"
               "  python apps/world_drop_viz.py\n"
               "  python apps/world_drop_viz.py --conf path/to/mod_solo_boss_loot.conf --worldserver-conf path/to/worldserver.conf")
    parser.add_argument("--db", default=sim.DEFAULT_DB, help="folder with <table>.sql dumps (default: AzerothCore's base world DB)")
    parser.add_argument("--conf", default=sim.DEFAULT_CONF, help="module config for the starting slider values (default: the .conf.dist)")
    parser.add_argument("--worldserver-conf", help="worldserver.conf to read Rate.Drop.Item.* from (default: all rates 1)")
    parser.add_argument("--overrides", default=sim.DEFAULT_OVERRIDES, help="SQL file with solo_boss_loot_overrides rows (default: the module's)")
    parser.add_argument("--out", default=DEFAULT_OUT, help="HTML file to write (default: apps/world_drop_viz.html)")
    args = parser.parse_args()

    conf = sim.read_conf(args.conf)
    worldserver_conf = sim.read_conf(args.worldserver_conf)
    settings = sim.Settings(conf, worldserver_conf)
    export_conf = dict(conf)
    export_conf.update({"SoloBossLoot.WorldDrop.MaxItemChance": str(EXPORT_MAX_ITEM_CHANCE),
                        "SoloBossLoot.WorldDrop.MinQuality": "2",
                        "SoloBossLoot.WorldDrop.MaxItemLevel": "0"})
    export_settings = sim.Settings(export_conf, worldserver_conf)

    data = sim.GameData(args.db, args.overrides)
    payload = json.dumps(build(data, settings, export_settings, args), separators=(",", ":")).replace("</", "<\\/")

    with open(TEMPLATE, encoding="utf8") as file:
        page = file.read()
    if "/*__DATA__*/null" not in page:
        sys.exit(f"{TEMPLATE} has no /*__DATA__*/null placeholder")
    with open(args.out, "w", encoding="utf8") as file:
        file.write(page.replace("/*__DATA__*/null", payload, 1))

    print(f"Wrote {os.path.abspath(args.out)} ({os.path.getsize(args.out) / 1e6:.1f} MB). Open it in a browser.")


if __name__ == "__main__":
    main()
