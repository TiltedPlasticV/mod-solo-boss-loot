#!/usr/bin/env python3
"""
World drop simulator for mod-solo-boss-loot.

Shows what the module's world drop settings do, without starting a server: how likely rare items are to drop
from non-boss creatures before and after the boost, and how often a kill gives an extra item for example players.

It mirrors src/mod_solo_boss_loot.cpp (AddDropChances, IsWorldDropCandidate, BuildWorldDropTable, PlayerWantsItem).
Keep the two in sync when the algorithm changes.

Data comes from mysqldump-style SQL files, one <table>.sql per table including its CREATE TABLE statement.
By default it reads the base world database that ships with AzerothCore (data/sql/base/db_world). To check a live
database instead, dump these tables into a folder and pass it with --db:
    creature_loot_template reference_loot_template item_template quest_template creature_template instance_encounters

Approximations:
  - Players are fresh characters: nothing owned or learned. Race limits and loot conditions are ignored.
  - Maps aren't known, so creatures in battlegrounds (e.g. Alterac Valley) are included. The module skips them.
  - Bosses are instance_encounters kill credits and boss-flagged creatures, plus creature rows in the module's
    overrides SQL file (not a live overrides table).

Needs Python 3.8 or newer, no extra packages. A full run takes about a minute.
"""

import argparse
import math
import os
import re
import statistics
import sys
from collections import Counter, defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
MODULE_DIR = os.path.dirname(HERE)
DEFAULT_DB = os.path.normpath(os.path.join(MODULE_DIR, "..", "..", "data", "sql", "base", "db_world"))
DEFAULT_CONF = os.path.join(MODULE_DIR, "conf", "mod_solo_boss_loot.conf.dist")
DEFAULT_OVERRIDES = os.path.join(MODULE_DIR, "data", "sql", "db-world", "base", "solo_boss_loot_overrides.sql")

LOOT_MODE_DEFAULT = 0x1
MAX_REFERENCE_DEPTH = 8
CREATURE_TYPE_FLAG_BOSS_MOB = 0x4
ENCOUNTER_CREDIT_KILL_CREATURE = 0
BIND_QUEST_ITEMS = (4, 5)

QUALITY_NAMES = ["grey", "white", "green", "blue", "epic", "legendary", "artifact", "heirloom"]
TYPE_NAMES = {1: "bag", 2: "weapon", 4: "armor", 9: "recipe", 11: "quiver", 15: "mount/pet"}

ITEM_CLASS_CONTAINER, ITEM_CLASS_WEAPON, ITEM_CLASS_ARMOR, ITEM_CLASS_RECIPE, ITEM_CLASS_QUIVER, ITEM_CLASS_MISC = 1, 2, 4, 9, 11, 15
ITEM_SUBCLASS_JUNK_PET, ITEM_SUBCLASS_JUNK_MOUNT = 2, 5
ARMOR_MISC, ARMOR_CLOTH, ARMOR_LEATHER, ARMOR_MAIL, ARMOR_PLATE, ARMOR_BUCKLER, ARMOR_SHIELD = 0, 1, 2, 3, 4, 5, 6
ARMOR_RELICS = {7: "paladin", 8: "druid", 9: "shaman", 10: "deathknight"}  # libram, idol, totem, sigil
INVTYPE_CLOAK = 16

# Weapon subclasses: axe 0, axe2 1, bow 2, gun 3, mace 4, mace2 5, polearm 6, sword 7, sword2 8, staff 10,
# fist 13, dagger 15, thrown 16, crossbow 18, wand 19 (same table as GetWeaponSubClassMask)
CLASS_WEAPONS = {
    "warrior": {0, 1, 4, 5, 7, 8, 6, 10, 13, 15, 2, 3, 18, 16},
    "paladin": {0, 1, 4, 5, 7, 8, 6},
    "deathknight": {0, 1, 4, 5, 7, 8, 6},
    "hunter": {0, 1, 7, 8, 6, 10, 13, 15, 2, 3, 18, 16},
    "rogue": {0, 4, 7, 13, 15, 2, 3, 18, 16},
    "priest": {4, 10, 15, 19},
    "shaman": {0, 1, 4, 5, 10, 13, 15},
    "mage": {7, 10, 15, 19},
    "warlock": {7, 10, 15, 19},
    "druid": {4, 5, 6, 10, 13, 15},
}
CLASS_MASKS = {"warrior": 1, "paladin": 2, "hunter": 4, "rogue": 8, "priest": 16, "deathknight": 32,
               "shaman": 64, "mage": 128, "warlock": 256, "druid": 1024}
SHIELD_CLASSES = {"warrior", "paladin", "shaman"}
PROFESSIONS = {"alchemy": 171, "blacksmithing": 164, "enchanting": 333, "engineering": 202, "herbalism": 182,
               "inscription": 773, "jewelcrafting": 755, "leatherworking": 165, "mining": 186, "skinning": 393,
               "tailoring": 197, "cooking": 185, "first-aid": 129, "fishing": 356}
DEFAULT_PLAYERS = ["warrior:blacksmithing,mining,cooking,first-aid,fishing",
                   "hunter:leatherworking,skinning,cooking,first-aid,fishing",
                   "priest:tailoring,enchanting,cooking,first-aid,fishing"]
DEFAULT_CREATURES = ["Defias Pillager", "Scarlet Monk"]
LEVEL_BANDS = [(1, 10), (11, 20), (21, 30), (31, 40), (41, 50), (51, 60), (61, 70), (71, 80), (81, 255)]


# ---------------------------------------------------------------------------
# Reading SQL dumps and config files
# ---------------------------------------------------------------------------
def progress(message):
    print(message, file=sys.stderr, flush=True)


def read_table(db_dir, table):
    """Returns (column names, rows as lists of strings) from <db_dir>/<table>.sql."""
    path = os.path.join(db_dir, table + ".sql")
    if not os.path.isfile(path):
        sys.exit(f"Missing {path}")

    progress(f"Reading {table}...")
    with open(path, encoding="utf8", errors="replace") as file:
        text = file.read()

    create = re.search(r"CREATE TABLE `%s` \((.*?)\n\)" % re.escape(table), text, re.S)
    if not create:
        sys.exit(f"{path} has no CREATE TABLE statement for `{table}`")
    columns = re.findall(r"^\s*`([^`]+)`", create.group(1), re.M)

    rows = []
    marker = f"INSERT INTO `{table}` VALUES"
    position = text.find(marker)
    while position >= 0:
        i, length = position + len(marker), len(text)
        row, field, in_quote = None, [], False
        while i < length:
            char = text[i]
            if in_quote:
                if char == "\\":
                    field.append(text[i + 1])
                    i += 2
                    continue
                if char == "'":
                    if text.startswith("''", i):
                        field.append("'")
                        i += 2
                        continue
                    in_quote = False
                else:
                    field.append(char)
            elif char == "'":
                in_quote = True
            elif char == "(" and row is None:
                row, field = [], []
            elif char == "," and row is not None:
                row.append("".join(field))
                field = []
            elif char == ")" and row is not None:
                row.append("".join(field))
                rows.append(row)
                row = None
            elif char == ";" and row is None:
                break
            elif row is not None and not char.isspace():
                field.append(char)
            i += 1
        position = text.find(marker, i)

    return columns, rows


def column_indexes(columns, table, names, optional=False):
    lower = {name.lower(): index for index, name in enumerate(columns)}
    missing = [name for name in names if name.lower() not in lower]
    if missing and not optional:
        sys.exit(f"`{table}` has no column(s): {', '.join(missing)}")
    return {name: lower[name.lower()] for name in names if name.lower() in lower}


def to_int(value):
    try:
        return int(value)
    except ValueError:
        try:
            return int(float(value))
        except ValueError:
            return 0


def to_float(value):
    try:
        return float(value)
    except ValueError:
        return 0.0


def read_conf(path):
    """Key = value pairs from a worldserver-style .conf file."""
    values = {}
    if not path:
        return values
    if not os.path.isfile(path):
        sys.exit(f"Missing {path}")

    with open(path, encoding="utf8", errors="replace") as file:
        for line in file:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, value = line.split("=", 1)
            values[key.strip()] = value.strip().strip('"')
    return values


# ---------------------------------------------------------------------------
# Settings (same defaults and limits as LoadConfig / GetDropRates)
# ---------------------------------------------------------------------------
class Settings:
    def __init__(self, conf, worldserver_conf):
        prefix = "SoloBossLoot.WorldDrop."

        def get(key, default, cast):
            try:
                return cast(conf.get(prefix + key, default))
            except ValueError:
                return default

        self.enabled = get("Enable", 1, int) != 0
        self.min_quality = get("MinQuality", 2, int)
        self.max_item_level = get("MaxItemLevel", 0, int)
        self.max_item_chance = min(max(get("MaxItemChance", 5.0, float), 0.0), 99.0) / 100
        self.max_combined_chance = min(max(get("MaxCombinedChance", 15.0, float), 0.0), 99.0) / 100
        self.compression = min(max(get("CompressionRatio", 0.8, float), 0.0), 1.0)
        self.recipe_weight = max(get("RecipeWeight", 0.75, float), 0.0)

        # Like the module: the first three numbers, all 0 or more, or the defaults
        try:
            weights = [float(token) for token in conf.get(prefix + "QualityWeights", "1 3 6").split()[:3]]
            valid = len(weights) == 3 and all(weight >= 0 for weight in weights)
        except ValueError:
            valid = False
        self.quality_weights = weights if valid else [1.0, 3.0, 6.0]

        def rate(name):
            try:
                return float(worldserver_conf.get("Rate.Drop.Item." + name, 1.0))
            except ValueError:
                return 1.0

        self.quality_rates = [rate(name) for name in ("Poor", "Normal", "Uncommon", "Rare", "Epic", "Legendary", "Artifact")]
        self.referenced_rate = rate("Referenced")
        self.referenced_amount = rate("ReferencedAmount")
        self.group_amount = max(1, int(rate("GroupAmount")))


# ---------------------------------------------------------------------------
# Game data
# ---------------------------------------------------------------------------
class LootRow:
    __slots__ = ("item", "reference", "chance", "quest_required", "loot_mode", "group_id", "max_count")


class Item:
    __slots__ = ("cls", "subclass", "name", "quality", "inventory_type", "allowable_class", "item_level",
                 "required_skill", "bonding", "start_quest")


class CreatureTemplate:
    __slots__ = ("difficulty_entries", "name", "max_level", "type_flags", "loot_id")


def load_loot(db_dir, table):
    columns, rows = read_table(db_dir, table)
    col = column_indexes(columns, table, ["Entry", "Item", "Reference", "Chance", "QuestRequired", "LootMode",
                                          "GroupId", "MaxCount"])
    loot = defaultdict(list)
    for values in rows:
        row = LootRow()
        row.item = to_int(values[col["Item"]])
        row.reference = abs(to_int(values[col["Reference"]]))
        row.chance = to_float(values[col["Chance"]])
        row.quest_required = to_int(values[col["QuestRequired"]]) != 0
        row.loot_mode = to_int(values[col["LootMode"]])
        row.group_id = to_int(values[col["GroupId"]])
        row.max_count = to_int(values[col["MaxCount"]])
        loot[to_int(values[col["Entry"]])].append(row)
    return loot


class GameData:
    def __init__(self, db_dir, overrides_path):
        self.creature_loot = load_loot(db_dir, "creature_loot_template")
        self.reference_loot = load_loot(db_dir, "reference_loot_template")

        columns, rows = read_table(db_dir, "item_template")
        col = column_indexes(columns, "item_template", ["entry", "class", "subclass", "name", "Quality", "InventoryType",
                                                        "AllowableClass", "ItemLevel", "RequiredSkill", "bonding",
                                                        "startquest"])
        self.items = {}
        for values in rows:
            item = Item()
            item.cls = to_int(values[col["class"]])
            item.subclass = to_int(values[col["subclass"]])
            item.name = values[col["name"]]
            item.quality = to_int(values[col["Quality"]])
            item.inventory_type = to_int(values[col["InventoryType"]])
            item.allowable_class = to_int(values[col["AllowableClass"]])
            item.item_level = to_int(values[col["ItemLevel"]])
            item.required_skill = to_int(values[col["RequiredSkill"]])
            item.bonding = to_int(values[col["bonding"]])
            item.start_quest = to_int(values[col["startquest"]])
            self.items[to_int(values[col["entry"]])] = item

        columns, rows = read_table(db_dir, "quest_template")
        names = [f"RequiredItemId{i}" for i in range(1, 7)]
        col = column_indexes(columns, "quest_template", names)
        self.quest_items = {to_int(values[col[name]]) for values in rows for name in names} - {0}

        columns, rows = read_table(db_dir, "creature_template")
        col = column_indexes(columns, "creature_template", ["entry", "name", "maxlevel", "type_flags", "lootid"])
        difficulty_col = column_indexes(columns, "creature_template",
                                        ["difficulty_entry_1", "difficulty_entry_2", "difficulty_entry_3"], optional=True)
        self.creatures = {}
        for values in rows:
            creature = CreatureTemplate()
            creature.difficulty_entries = [to_int(values[index]) for index in difficulty_col.values()]
            creature.name = values[col["name"]]
            creature.max_level = to_int(values[col["maxlevel"]])
            creature.type_flags = to_int(values[col["type_flags"]])
            creature.loot_id = to_int(values[col["lootid"]])
            self.creatures[to_int(values[col["entry"]])] = creature

        columns, rows = read_table(db_dir, "instance_encounters")
        col = column_indexes(columns, "instance_encounters", ["creditType", "creditEntry"])
        encounter_bosses = {to_int(values[col["creditEntry"]]) for values in rows
                            if to_int(values[col["creditType"]]) == ENCOUNTER_CREDIT_KILL_CREATURE}

        overrides = {}
        if overrides_path and os.path.isfile(overrides_path):
            with open(overrides_path, encoding="utf8", errors="replace") as file:
                for source_type, entry, mode in re.findall(r"\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,", file.read()):
                    if int(source_type) == 0:
                        overrides[int(entry)] = int(mode) != 0

        # Same split as BuildLootCache: bosses (and their difficulty versions) get boss loot, the rest world drops
        self.bosses = set()
        boss_difficulty_entries = set()
        for entry, creature in self.creatures.items():
            is_boss = entry in encounter_bosses or (creature.type_flags & CREATURE_TYPE_FLAG_BOSS_MOB) != 0
            is_boss = overrides.get(entry, is_boss)
            if is_boss:
                self.bosses.add(entry)
                boss_difficulty_entries.update(e for e in creature.difficulty_entries if e)

        self.table_level = {}  # loot id -> lowest level of the creatures using it
        self.table_creatures = defaultdict(list)
        for entry, creature in self.creatures.items():
            if entry in self.bosses or entry in boss_difficulty_entries or not creature.loot_id:
                continue
            self.table_level[creature.loot_id] = min(self.table_level.get(creature.loot_id, 255), creature.max_level)
            self.table_creatures[creature.loot_id].append(entry)


# ---------------------------------------------------------------------------
# The world drop algorithm (mirrors the C++)
# ---------------------------------------------------------------------------
def drop_chances(rows, data, settings, multiplier=1.0, top_level=True, depth=0, chances=None):
    """AddDropChances: expected drops per kill for each item, the way core rolls the default loot mode."""
    if chances is None:
        chances = defaultdict(float)

    def add_reference(row, chance):
        repeats = int(row.max_count * settings.referenced_amount)
        if chance <= 0 or repeats <= 0 or depth >= MAX_REFERENCE_DEPTH:
            return
        child_rows = data.reference_loot.get(row.reference)
        if child_rows:
            drop_chances(child_rows, data, settings, multiplier * chance * repeats, False, depth + 1, chances)

    def add_item(row, chance):
        if chance > 0 and not row.quest_required:
            chances[row.item] += multiplier * chance

    groups = defaultdict(list)
    for row in rows:
        if not row.loot_mode & LOOT_MODE_DEFAULT:
            continue
        if row.group_id:
            groups[row.group_id].append(row)
            continue

        # LootStoreItem::Roll: 100% rows always drop, other rows are scaled by the drop rates
        if row.reference:
            add_reference(row, 1.0 if row.chance >= 100 else min(1.0, row.chance * settings.referenced_rate / 100))
        else:
            item = data.items.get(row.item)
            rate = settings.quality_rates[item.quality] if item and item.quality < 7 else 1.0
            add_item(row, 1.0 if row.chance >= 100 else min(1.0, row.chance * rate / 100))

    # LootGroup::Roll: explicit chances take the roll in order, a miss picks one equal-chanced row. No rates.
    for group_rows in groups.values():
        def add_group_row(row, chance):
            if row.reference:
                add_reference(row, chance)
            else:
                add_item(row, chance * (settings.group_amount if top_level else 1))

        taken, equal_count = 0.0, 0
        for row in group_rows:
            if row.chance == 0:
                equal_count += 1
                continue
            chance = min(1.0, row.chance / 100)
            add_group_row(row, max(0.0, min(chance, 1.0 - taken)))
            taken += chance

        if equal_count and taken < 1.0:
            for row in group_rows:
                if row.chance == 0:
                    add_group_row(row, (1.0 - taken) / equal_count)

    return chances


def is_keepable(item):
    return (item.cls in (ITEM_CLASS_WEAPON, ITEM_CLASS_ARMOR, ITEM_CLASS_CONTAINER, ITEM_CLASS_QUIVER, ITEM_CLASS_RECIPE)
            or (item.cls == ITEM_CLASS_MISC and item.subclass in (ITEM_SUBCLASS_JUNK_PET, ITEM_SUBCLASS_JUNK_MOUNT)))


def is_rare_item(item_id, chance, data, settings):
    """The chance check in BuildWorldDropLoot plus IsWorldDropCandidate."""
    item = data.items.get(item_id)
    if item is None or not 0 < chance < settings.max_item_chance:
        return False
    return (is_keepable(item)
            and item.quality >= settings.min_quality
            and (not settings.max_item_level or item.item_level < settings.max_item_level)
            and item.bonding not in BIND_QUEST_ITEMS
            and not item.start_quest
            and item_id not in data.quest_items)


def item_weight(item, settings):
    weight = settings.quality_weights[min(max(item.quality, 2), 4) - 2]
    return weight * settings.recipe_weight if item.cls == ITEM_CLASS_RECIPE else weight


def combined(chances):
    miss_all = 1.0
    for chance in chances:
        miss_all *= 1.0 - chance
    return 1.0 - miss_all


class TableResult:
    __slots__ = ("original", "boosted", "extra", "status")


def boost_table(original, data, settings):
    """BuildWorldDropTable. original: {item id: chance 0..1}."""
    result = TableResult()
    result.original, result.boosted, result.extra = original, dict(original), {}

    if combined(original.values()) >= settings.max_combined_chance:
        result.status = "already at target"
        return result

    r = settings.compression
    mean = math.exp(sum(math.log(chance) for chance in original.values()) / len(original))
    shaped = {i: chance ** (1 - r) * mean ** r * item_weight(data.items[i], settings) for i, chance in original.items()}
    positive = [value for value in shaped.values() if value > 0]
    if not positive:
        result.status = "no boost"
        return result

    boosted = result.boosted

    def apply(scale):
        for i, chance in original.items():
            boosted[i] = min(settings.max_item_chance, max(chance, scale * shaped[i]))
        return combined(boosted.values())

    high = settings.max_item_chance / min(positive)
    scale, result.status = high, "every item at MaxItemChance"
    if apply(high) > settings.max_combined_chance:
        low = 0.0
        for _ in range(40):
            middle = (low + high) / 2
            if apply(middle) > settings.max_combined_chance:
                high = middle
            else:
                low = middle
        scale, result.status = low, "hits target"
    apply(scale)

    result.extra = {i: (boosted[i] - chance) / (1 - chance) for i, chance in original.items() if boosted[i] > chance}
    if not result.extra:
        result.status = "no boost"
    return result


def simulate(data, settings):
    progress("Simulating...")
    results = {}
    for loot_id in data.table_level:
        rows = data.creature_loot.get(loot_id)
        if not rows:
            continue
        chances = drop_chances(rows, data, settings)
        original = {}
        for item_id, chance in chances.items():
            chance = min(1.0, chance)
            if is_rare_item(item_id, chance, data, settings):
                original[item_id] = chance
        if original:
            results[loot_id] = boost_table(original, data, settings)
    return results


# ---------------------------------------------------------------------------
# Players (PlayerWantsItem for a fresh character)
# ---------------------------------------------------------------------------
class Player:
    def __init__(self, text):
        name, _, professions = text.partition(":")
        self.cls = name.strip().lower().replace(" ", "").replace("-", "").replace("_", "")
        if self.cls not in CLASS_MASKS:
            raise argparse.ArgumentTypeError(f"unknown class '{name}' (choose from {', '.join(CLASS_MASKS)})")

        self.skills = set()
        self.professions = []
        for profession in filter(None, (p.strip().lower().replace(" ", "-") for p in professions.split(","))):
            if profession not in PROFESSIONS:
                raise argparse.ArgumentTypeError(f"unknown profession '{profession}' (choose from {', '.join(PROFESSIONS)})")
            self.skills.add(PROFESSIONS[profession])
            self.professions.append(profession)

    def label(self):
        return f"{self.cls} ({', '.join(self.professions) or 'no professions'})"

    def main_armor(self, level):
        if self.cls in ("warrior", "paladin"):
            return ARMOR_PLATE if level >= 40 else ARMOR_MAIL
        if self.cls == "deathknight":
            return ARMOR_PLATE
        if self.cls in ("hunter", "shaman"):
            return ARMOR_MAIL if level >= 40 else ARMOR_LEATHER
        if self.cls in ("rogue", "druid"):
            return ARMOR_LEATHER
        return ARMOR_CLOTH

    def wants(self, item, level):
        if item.allowable_class and not item.allowable_class & CLASS_MASKS[self.cls]:
            return False
        if item.cls == ITEM_CLASS_WEAPON:
            return item.subclass in CLASS_WEAPONS[self.cls]
        if item.cls == ITEM_CLASS_ARMOR:
            if item.inventory_type == INVTYPE_CLOAK or item.subclass == ARMOR_MISC:
                return True
            if item.subclass in (ARMOR_CLOTH, ARMOR_LEATHER, ARMOR_MAIL, ARMOR_PLATE):
                return item.subclass == self.main_armor(level)
            if item.subclass in (ARMOR_BUCKLER, ARMOR_SHIELD):
                return self.cls in SHIELD_CLASSES
            return ARMOR_RELICS.get(item.subclass) == self.cls
        if item.cls == ITEM_CLASS_RECIPE and item.required_skill and item.required_skill not in self.skills:
            return False
        return True

    def extra_chance(self, result, data, level):
        """Chance a kill gives this player an extra item (the module's two rolls combined)."""
        return combined(chance for i, chance in result.extra.items() if self.wants(data.items[i], level))


# ---------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------
def percent(value, decimals=2):
    return f"{value * 100:.{decimals}f}%"


def median(values, default=0.0):
    values = list(values)
    return statistics.median(values) if values else default


def mix(shares):
    total = sum(shares.values())
    return ", ".join(f"{name} {value / total:.0%}" for name, value in sorted(shares.items(), key=lambda kv: -kv[1])
                     if total and value / total >= 0.005) or "none"


def report_summary(data, settings, results, args):
    print("World drop simulation")
    print(f"  Data:     {args.db}")
    print(f"  Settings: {args.conf}")
    print(f"            MinQuality {settings.min_quality}, MaxItemLevel {settings.max_item_level or 'none'}, "
          f"MaxItemChance {percent(settings.max_item_chance, 1)}, MaxCombinedChance {percent(settings.max_combined_chance, 1)}, "
          f"CompressionRatio {settings.compression:g}, QualityWeights {' '.join(f'{w:g}' for w in settings.quality_weights)}, "
          f"RecipeWeight {settings.recipe_weight:g}")
    rates = " ".join(f"{name} {rate:g}" for name, rate in zip(("Poor", "Normal", "Uncommon", "Rare", "Epic", "Legendary", "Artifact"),
                                                             settings.quality_rates))
    print(f"  Rates:    {args.worldserver_conf or 'all 1 (no --worldserver-conf)'}: {rates}, Referenced {settings.referenced_rate:g}, "
          f"ReferencedAmount {settings.referenced_amount:g}, GroupAmount {settings.group_amount}")
    if not settings.enabled:
        print("  Note:     SoloBossLoot.WorldDrop.Enable is 0 in this config, so the module would add nothing.")

    boosted = [result for result in results.values() if result.extra]
    unique = {tuple(sorted((i, round(chance, 9)) for i, chance in result.extra.items())) for result in boosted}
    statuses = Counter(result.status for result in results.values())
    print()
    print(f"Non-boss creature loot tables: {len(data.table_level)}")
    print(f"  with rare items:    {len(results)} ({len({i for r in results.values() for i in r.original})} distinct items)")
    print(f"  getting world drops: {len(boosted)} ({len(unique)} unique tables)")
    print("  " + ", ".join(f"{status}: {count}" for status, count in statuses.most_common()))


def report_levels(data, results, players):
    print()
    print("By creature level (medians over loot tables with rare items)")
    header = f"{'level':>7} | {'tables':>6} | {'rare items':>10} | {'rare drop before -> after':>25} | {'extra item':>10}"
    for player in players:
        header += f" | {player.cls:>11}"
    print(header)

    for low, high in LEVEL_BANDS:
        ids = [loot_id for loot_id in results if low <= data.table_level[loot_id] <= high]
        if not ids:
            continue
        line = (f"{low:>3}-{high:<3} | {len(ids):>6} | {median(len(results[i].original) for i in ids):>10g} | "
                f"{percent(median(combined(results[i].original.values()) for i in ids)):>11} -> "
                f"{percent(median(combined(results[i].boosted.values()) for i in ids)):<10} | "
                f"{percent(median(combined(results[i].extra.values()) for i in ids)):>10}")
        for player in players:
            line += f" | {percent(median(player.extra_chance(results[i], data, data.table_level[i]) for i in ids)):>11}"
        print(line)
    print("  extra item: chance a kill adds an item, before the wants check. Player columns: after it.")


def report_mix(data, results, players):
    print()
    print("What the extra items are (each loot table weighted equally)")

    def shares(player=None):
        by_type, by_quality = defaultdict(float), defaultdict(float)
        for loot_id, result in results.items():
            level = data.table_level[loot_id]
            chances = {i: c for i, c in result.extra.items() if player is None or player.wants(data.items[i], level)}
            total = sum(chances.values())
            for i, chance in chances.items():
                item = data.items[i]
                by_type[TYPE_NAMES.get(item.cls, "other")] += chance / total
                by_quality[QUALITY_NAMES[min(item.quality, 7)]] += chance / total
        return by_type, by_quality

    for label, player in [("all rare items", None)] + [(p.label(), p) for p in players]:
        by_type, by_quality = shares(player)
        print(f"  {label}:")
        print(f"      {mix(by_type)}")
        print(f"      {mix(by_quality)}")


def report_items(data, results):
    print()
    print("Per-item chance, median before -> after")
    groups = defaultdict(lambda: ([], []))
    for result in results.values():
        for i, chance in result.original.items():
            item = data.items[i]
            key = ("recipe " if item.cls == ITEM_CLASS_RECIPE else "") + QUALITY_NAMES[min(item.quality, 7)]
            groups[key][0].append(chance)
            groups[key][1].append(result.boosted[i])
    for key in sorted(groups, key=lambda k: (k.startswith("recipe"), QUALITY_NAMES.index(k.split()[-1]))):
        before, after = groups[key]
        print(f"  {key:>16}: {len(before):>8} rows, {percent(median(before), 4)} -> {percent(median(after), 4)}")


def report_creature(query, data, results, players, top):
    print()
    if query.isdigit():
        entries = [int(query)] if int(query) in data.creatures else []
    else:
        entries = [entry for entry, creature in data.creatures.items() if creature.name.lower() == query.lower()]
    if not entries:
        print(f"Creature '{query}': not found")
        return

    for entry in entries[:3]:
        creature = data.creatures[entry]
        title = f"{creature.name} (creature {entry}, level {creature.max_level}, loot id {creature.loot_id})"
        if entry in data.bosses:
            print(f"{title}: a boss, gets boss loot instead")
            continue
        result = results.get(creature.loot_id)
        if result is None:
            print(f"{title}: no rare items")
            continue

        print(f"{title}: {len(result.original)} rare items, {result.status}")
        print(f"  rare drop {percent(combined(result.original.values()))} -> {percent(combined(result.boosted.values()))}, "
              f"extra item {percent(combined(result.extra.values()))}")
        for player in players:
            wanted = sum(1 for i in result.extra if player.wants(data.items[i], creature.max_level))
            print(f"  {player.label()}: {wanted} items wanted, extra item "
                  f"{percent(player.extra_chance(result, data, creature.max_level))}")
        for i in sorted(result.original, key=lambda i: (-result.boosted[i], i))[:top]:
            item = data.items[i]
            print(f"    {QUALITY_NAMES[min(item.quality, 7)]:>6} {TYPE_NAMES.get(item.cls, 'other'):>9} ilvl {item.item_level:>3} "
                  f"{item.name[:40]:<40} {percent(result.original[i], 4):>9} -> {percent(result.boosted[i], 4)}")


def main():
    parser = argparse.ArgumentParser(
        description="Simulate mod-solo-boss-loot world drops from world database SQL files.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="examples:\n"
               "  python apps/world_drop_sim.py\n"
               "  python apps/world_drop_sim.py --conf path/to/mod_solo_boss_loot.conf --worldserver-conf path/to/worldserver.conf\n"
               "  python apps/world_drop_sim.py --player mage:tailoring,enchanting --creature \"Defias Pillager\" --creature 4540\n"
               "\nprofessions: " + ", ".join(PROFESSIONS))
    parser.add_argument("--db", default=DEFAULT_DB, help="folder with <table>.sql dumps (default: AzerothCore's base world DB)")
    parser.add_argument("--conf", default=DEFAULT_CONF, help="module config to read SoloBossLoot.WorldDrop.* from (default: the .conf.dist)")
    parser.add_argument("--worldserver-conf", help="worldserver.conf to read Rate.Drop.Item.* from (default: all rates 1)")
    parser.add_argument("--overrides", default=DEFAULT_OVERRIDES, help="SQL file with solo_boss_loot_overrides rows (default: the module's)")
    parser.add_argument("--player", action="append", type=Player, metavar="CLASS:PROFESSION,...",
                        help="example player for the wants check, repeatable (default: warrior, hunter and priest with two professions)")
    parser.add_argument("--creature", action="append", metavar="NAME_OR_ENTRY",
                        help="show one creature's rare items, repeatable (default: Defias Pillager and Scarlet Monk)")
    parser.add_argument("--top", type=int, default=10, help="rare items to list per creature (default: 10)")
    args = parser.parse_args()

    players = args.player or [Player(text) for text in DEFAULT_PLAYERS]
    settings = Settings(read_conf(args.conf), read_conf(args.worldserver_conf))
    data = GameData(args.db, args.overrides)
    results = simulate(data, settings)

    report_summary(data, settings, results, args)
    report_levels(data, results, players)
    report_mix(data, results, players)
    report_items(data, results)
    for query in args.creature or DEFAULT_CREATURES:
        report_creature(query, data, results, players, args.top)


if __name__ == "__main__":
    main()
