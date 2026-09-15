/*
 * mod-solo-boss-loot
 *
 * Boss loot: when a dungeon or raid boss (or a boss chest) is looted, adds the boss's own loot that a real player in the
 * group can use and doesn't have yet.
 * World drops: when any other creature is looted, may add one of its rare items (gear, bags, recipes, mounts, pets)
 * that a real player in the group wants, at a boosted chance.
 * Bots are ignored. The normal loot roll is untouched; this only adds items.
 *
 * At startup (and on .reload config) the module works out which items belong to each boss loot table:
 *   - only boss loot tables drop the item (no trash mob, world-drop list, container, ...)
 *   - items shared by many bosses are only kept if they can be kept (gear, bags, mounts, pets, recipes)
 *   - quest-required items are left to the normal roll, and hard-mode (LootMode) rules are kept
 * and, for every other creature loot table, how likely each rare item is to drop and how far to raise it.
 * The `solo_boss_loot_overrides` world table adds boss chests and extra bosses, and forces items in or out.
 */

#include "Config.h"
#include "Containers.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "GameObject.h"
#include "Group.h"
#include "Log.h"
#include "LootMgr.h"
#include "Map.h"
#include "MiscScript.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "Timer.h"
#include "World.h"
#include "WorldScript.h"
#include "WorldSession.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    // Loggers are set in worldserver.conf. Unset ones fall back to their parent: world_drop -> solo_boss_loot -> module.
    constexpr char const* LOG_NAME       = "module.solo_boss_loot";            // startup, settings and loot caches
    constexpr char const* LOG_BOSS       = "module.solo_boss_loot.boss";       // boss kills and boss chests
    constexpr char const* LOG_WORLD_DROP = "module.solo_boss_loot.world_drop"; // other creature kills

    constexpr char const* OVERRIDES_TABLE = "solo_boss_loot_overrides";
    constexpr uint8 MAX_REFERENCE_DEPTH = 8;

    // ---------------------------------------------------------------------------
    // Configuration
    // ---------------------------------------------------------------------------
    constexpr std::array<float, 3> DEFAULT_QUALITY_WEIGHTS{ 1.0f, 3.0f, 6.0f };

    struct ModuleConfig
    {
        bool bossLootEnabled = true;
        bool skipOwnedItems = true;
        uint32 sharedPoolThreshold = 3;

        bool worldDropEnabled = true;
        uint32 worldDropMinQuality = ITEM_QUALITY_UNCOMMON;
        uint32 worldDropMaxItemLevel = 0;
        float worldDropMaxItemChance = 5.0f;
        float worldDropMaxCombinedChance = 15.0f;
        float worldDropCompressionRatio = 0.8f;
        std::array<float, 3> worldDropQualityWeights = DEFAULT_QUALITY_WEIGHTS; // uncommon, rare, epic and above
        float worldDropRecipeWeight = 0.75f;
    };

    ModuleConfig config;

    // Reads a number setting and clamps it, with a warning when it was out of range
    float GetClampedOption(char const* name, float defaultValue, float min, float max)
    {
        float const value = sConfigMgr->GetOption<float>(name, defaultValue);
        float const clamped = std::clamp(value, min, max);
        if (clamped != value)
            LOG_WARN(LOG_NAME, "SoloBossLoot: {} = {} is out of range ({} to {}), using {}", name, value, min, max, clamped);

        return clamped;
    }

    void LoadConfig()
    {
        config.bossLootEnabled     = sConfigMgr->GetOption<bool>("SoloBossLoot.BossLoot.Enable", true);
        config.skipOwnedItems      = sConfigMgr->GetOption<bool>("SoloBossLoot.SkipOwnedItems", true);
        config.sharedPoolThreshold = sConfigMgr->GetOption<uint32>("SoloBossLoot.SharedPoolThreshold", 3);

        config.worldDropEnabled           = sConfigMgr->GetOption<bool>("SoloBossLoot.WorldDrop.Enable", true);
        config.worldDropMinQuality        = sConfigMgr->GetOption<uint32>("SoloBossLoot.WorldDrop.MinQuality", ITEM_QUALITY_UNCOMMON);
        config.worldDropMaxItemLevel      = sConfigMgr->GetOption<uint32>("SoloBossLoot.WorldDrop.MaxItemLevel", 0);
        config.worldDropMaxItemChance     = GetClampedOption("SoloBossLoot.WorldDrop.MaxItemChance", 5.0f, 0.0f, 99.0f);
        config.worldDropMaxCombinedChance = GetClampedOption("SoloBossLoot.WorldDrop.MaxCombinedChance", 15.0f, 0.0f, 99.0f);
        config.worldDropCompressionRatio  = GetClampedOption("SoloBossLoot.WorldDrop.CompressionRatio", 0.8f, 0.0f, 1.0f);
        config.worldDropRecipeWeight      = sConfigMgr->GetOption<float>("SoloBossLoot.WorldDrop.RecipeWeight", 0.75f);

        if (config.worldDropRecipeWeight < 0.0f)
        {
            LOG_WARN(LOG_NAME, "SoloBossLoot: SoloBossLoot.WorldDrop.RecipeWeight = {} is below 0, using 0", config.worldDropRecipeWeight);
            config.worldDropRecipeWeight = 0.0f;
        }

        std::string const weights = sConfigMgr->GetOption<std::string>("SoloBossLoot.WorldDrop.QualityWeights", "1 3 6");
        std::istringstream stream(weights);
        std::array<float, 3> parsed{};
        bool valid = true;
        for (float& weight : parsed)
            valid = valid && (stream >> weight) && weight >= 0.0f;

        if (valid)
            config.worldDropQualityWeights = parsed;
        else
        {
            config.worldDropQualityWeights = DEFAULT_QUALITY_WEIGHTS;
            LOG_ERROR(LOG_NAME, "SoloBossLoot: SoloBossLoot.WorldDrop.QualityWeights \"{}\" needs three numbers of 0 or more, "
                "using \"1 3 6\"", weights);
        }

        LOG_DEBUG(LOG_NAME, "SoloBossLoot: Settings: BossLoot.Enable {}, SkipOwnedItems {}, SharedPoolThreshold {}, WorldDrop.Enable {}, "
            "WorldDrop.MinQuality {}, WorldDrop.MaxItemLevel {}, WorldDrop.MaxItemChance {}, WorldDrop.MaxCombinedChance {}, "
            "WorldDrop.CompressionRatio {}, WorldDrop.QualityWeights \"{} {} {}\", WorldDrop.RecipeWeight {}",
            config.bossLootEnabled, config.skipOwnedItems, config.sharedPoolThreshold, config.worldDropEnabled,
            config.worldDropMinQuality, config.worldDropMaxItemLevel, config.worldDropMaxItemChance, config.worldDropMaxCombinedChance,
            config.worldDropCompressionRatio, config.worldDropQualityWeights[0], config.worldDropQualityWeights[1],
            config.worldDropQualityWeights[2], config.worldDropRecipeWeight);
    }

    // ---------------------------------------------------------------------------
    // Loot cache
    // ---------------------------------------------------------------------------
    enum OverrideSourceType : uint8
    {
        OVERRIDE_CREATURE   = 0,
        OVERRIDE_GAMEOBJECT = 1,
        OVERRIDE_ITEM       = 2
    };

    // LootMode masks that must all share a bit with the kill's loot mode (one per table/reference on the way to the item)
    using ModePath = std::vector<uint16>;

    struct BossItem
    {
        uint32 itemId;
        uint8 minCount;
        uint8 maxCount;
        std::vector<ModePath> modePaths; // the item can drop if any path matches
    };

    using BossItemList = std::vector<BossItem>;

    struct WorldDropItem
    {
        uint32 itemId;
        uint8 minCount;
        uint8 maxCount;
        float chance; // chance (0..1) of this item's extra roll

        bool operator==(WorldDropItem const&) const = default;
    };

    struct WorldDropTable
    {
        std::vector<WorldDropItem> items;
        float chance = 0.0f; // chance (0..1) that at least one item's extra roll hits
    };

    struct LootCache
    {
        std::unordered_map<uint32, BossItemList> creatureLoot; // creature loot id -> items to add
        std::unordered_map<uint32, BossItemList> chestLoot;    // gameobject loot id -> items to add
        std::unordered_map<uint32, bool> creatureOverrides;    // creature entry -> is a boss
        std::unordered_set<uint32> bossChests;                 // gameobject entries
        std::unordered_map<uint32, std::shared_ptr<WorldDropTable const>> worldDropLoot; // creature loot id -> world drops (identical tables shared)
    };

    // Loot is generated on map threads, the cache is rebuilt on the world thread: swap it under a lock
    std::shared_ptr<LootCache const> lootCache;
    std::shared_mutex lootCacheLock;

    std::shared_ptr<LootCache const> GetLootCache()
    {
        std::shared_lock lock(lootCacheLock);
        return lootCache;
    }

    void SetLootCache(std::shared_ptr<LootCache const> cache)
    {
        std::unique_lock lock(lootCacheLock);
        lootCache = std::move(cache);
    }

    struct LootRow
    {
        uint32 item;
        uint32 reference;
        float chance;
        uint16 lootMode;
        uint8 groupId;
        uint8 minCount;
        uint8 maxCount;
        bool questRequired;
    };

    using LootRows = std::unordered_map<uint32, std::vector<LootRow>>; // loot entry -> rows

    LootRows LoadLootRows(char const* table)
    {
        LootRows rows;

        QueryResult result = WorldDatabase.Query(
            "SELECT `Entry`, `Item`, `Reference`, `QuestRequired`, `LootMode`, `MinCount`, `MaxCount`, `Chance`, `GroupId` FROM `{}`", table);
        if (!result)
            return rows;

        do
        {
            Field* fields = result->Fetch();

            LootRow row;
            row.item          = fields[1].Get<uint32>();
            row.reference     = static_cast<uint32>(std::abs(fields[2].Get<int32>()));
            row.questRequired = fields[3].Get<bool>();
            row.lootMode      = fields[4].Get<uint16>();
            row.minCount      = fields[5].Get<uint8>();
            row.maxCount      = fields[6].Get<uint8>();
            row.chance        = fields[7].Get<float>();
            row.groupId       = fields[8].Get<uint8>();

            rows[fields[0].Get<uint32>()].push_back(row);
        } while (result->NextRow());

        return rows;
    }

    // Marks a reference, everything below it and all its items as dropped by something that isn't a boss
    void MarkReferenceNonBoss(uint32 reference, LootRows const& referenceRows,
        std::unordered_set<uint32>& nonBossReferences, std::unordered_set<uint32>& nonBossItems)
    {
        std::vector<uint32> pending{ reference };
        while (!pending.empty())
        {
            uint32 const current = pending.back();
            pending.pop_back();

            if (!nonBossReferences.insert(current).second)
                continue;

            auto itr = referenceRows.find(current);
            if (itr == referenceRows.end())
                continue;

            for (LootRow const& row : itr->second)
            {
                if (row.reference)
                    pending.push_back(row.reference);
                else
                    nonBossItems.insert(row.item);
            }
        }
    }

    // Adds a mask to a path, dropping masks that became redundant
    ModePath WithMask(ModePath path, uint16 mask)
    {
        for (uint16 existing : path)
            if ((existing & ~mask) == 0) // whenever `existing` matches, `mask` matches too
                return path;

        path.erase(std::remove_if(path.begin(), path.end(),
            [mask](uint16 existing) { return (mask & ~existing) == 0; }), path.end());
        path.push_back(mask);
        return path;
    }

    // Every item a loot table can drop, following references. Quest-required items are left to the normal roll.
    std::unordered_map<uint32, BossItem> CollectLootTableItems(std::vector<LootRow> const& rows, LootRows const& referenceRows)
    {
        struct Pending
        {
            LootRow const* row;
            ModePath path;
            uint8 depth;
        };

        std::unordered_map<uint32, BossItem> items;
        std::vector<Pending> pending;
        for (LootRow const& row : rows)
            pending.push_back({ &row, {}, 0 });

        while (!pending.empty())
        {
            Pending current = std::move(pending.back());
            pending.pop_back();

            LootRow const& row = *current.row;
            if (!row.lootMode)
                continue;

            ModePath path = WithMask(std::move(current.path), row.lootMode);

            if (row.reference)
            {
                if (current.depth >= MAX_REFERENCE_DEPTH)
                    continue;

                if (auto itr = referenceRows.find(row.reference); itr != referenceRows.end())
                    for (LootRow const& child : itr->second)
                        pending.push_back({ &child, path, uint8(current.depth + 1) });

                continue;
            }

            if (row.questRequired)
                continue;

            BossItem& item = items.try_emplace(row.item, BossItem{ row.item, row.minCount, row.maxCount, {} }).first->second;
            std::vector<ModePath>& paths = item.modePaths;
            if (std::find(paths.begin(), paths.end(), path) == paths.end())
                paths.push_back(std::move(path));
        }

        return items;
    }

    bool IsGear(ItemTemplate const* proto)
    {
        return proto->Class == ITEM_CLASS_WEAPON || proto->Class == ITEM_CLASS_ARMOR;
    }

    // Items that stay in the player's bags once looted, so "skip owned" and "already learned" stop them repeating
    bool IsKeepable(ItemTemplate const* proto)
    {
        switch (proto->Class)
        {
            case ITEM_CLASS_WEAPON:
            case ITEM_CLASS_ARMOR:
            case ITEM_CLASS_CONTAINER:
            case ITEM_CLASS_QUIVER:
            case ITEM_CLASS_RECIPE:
                return true;
            case ITEM_CLASS_MISC:
                return proto->SubClass == ITEM_SUBCLASS_JUNK_MOUNT || proto->SubClass == ITEM_SUBCLASS_JUNK_PET;
            default:
                return false;
        }
    }

    // ---------------------------------------------------------------------------
    // World drop cache
    // ---------------------------------------------------------------------------
    struct DropRates
    {
        std::array<float, ITEM_QUALITY_HEIRLOOM> quality; // heirlooms ignore the rates
        float referenced;
        float referencedAmount;
        uint32 groupAmount;
    };

    DropRates GetDropRates()
    {
        DropRates rates;
        rates.quality[ITEM_QUALITY_POOR]      = sWorld->getRate(RATE_DROP_ITEM_POOR);
        rates.quality[ITEM_QUALITY_NORMAL]    = sWorld->getRate(RATE_DROP_ITEM_NORMAL);
        rates.quality[ITEM_QUALITY_UNCOMMON]  = sWorld->getRate(RATE_DROP_ITEM_UNCOMMON);
        rates.quality[ITEM_QUALITY_RARE]      = sWorld->getRate(RATE_DROP_ITEM_RARE);
        rates.quality[ITEM_QUALITY_EPIC]      = sWorld->getRate(RATE_DROP_ITEM_EPIC);
        rates.quality[ITEM_QUALITY_LEGENDARY] = sWorld->getRate(RATE_DROP_ITEM_LEGENDARY);
        rates.quality[ITEM_QUALITY_ARTIFACT]  = sWorld->getRate(RATE_DROP_ITEM_ARTIFACT);
        rates.referenced       = sWorld->getRate(RATE_DROP_ITEM_REFERENCED);
        rates.referencedAmount = sWorld->getRate(RATE_DROP_ITEM_REFERENCED_AMOUNT);
        rates.groupAmount      = std::max<uint32>(1, uint32(sWorld->getRate(RATE_DROP_ITEM_GROUP_AMOUNT)));
        return rates;
    }

    struct DropChance
    {
        uint8 minCount;
        uint8 maxCount;
        double chance; // expected drops per kill; rare items almost never drop twice, so it's used as the chance (0..1)
    };

    using DropChances = std::unordered_map<uint32, DropChance>; // item id -> chance

    // How likely each item is to drop, the way LootTemplate::Process and LootGroup::Roll roll the default loot mode
    // with the server's drop rates. `multiplier` is the chance of reaching these rows.
    void AddDropChances(std::vector<LootRow> const& rows, LootRows const& referenceRows, DropRates const& rates,
        double multiplier, bool isTopLevel, uint8 depth, DropChances& chances)
    {
        auto addReference = [&](LootRow const& row, double chance)
        {
            // Rate.Drop.Item.ReferencedAmount repeats the reference
            uint32 const repeats = uint32(float(row.maxCount) * rates.referencedAmount);
            if (chance <= 0.0 || !repeats || depth >= MAX_REFERENCE_DEPTH)
                return;

            if (auto itr = referenceRows.find(row.reference); itr != referenceRows.end())
                AddDropChances(itr->second, referenceRows, rates, multiplier * chance * repeats, false, depth + 1, chances);
        };

        auto addItem = [&](LootRow const& row, double chance)
        {
            if (chance <= 0.0 || row.questRequired)
                return;

            DropChance& drop = chances.try_emplace(row.item, DropChance{ row.minCount, row.maxCount, 0.0 }).first->second;
            drop.chance += multiplier * chance;
        };

        std::map<uint8, std::vector<LootRow const*>> groups;
        for (LootRow const& row : rows)
        {
            if (!(row.lootMode & LOOT_MODE_DEFAULT))
                continue;

            if (row.groupId)
            {
                groups[row.groupId].push_back(&row);
                continue;
            }

            // LootStoreItem::Roll: 100% rows always drop, other rows are scaled by the drop rates
            if (row.reference)
                addReference(row, row.chance >= 100.0f ? 1.0 : std::min(1.0, row.chance * rates.referenced / 100.0));
            else
            {
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(row.item);
                float const rate = proto && proto->Quality < ITEM_QUALITY_HEIRLOOM ? rates.quality[proto->Quality] : 1.0f;
                addItem(row, row.chance >= 100.0f ? 1.0 : std::min(1.0, row.chance * rate / 100.0));
            }
        }

        // LootGroup::Roll: one roll walks the explicitly chanced rows in order, and a miss picks one equal-chanced row.
        // Group chances ignore the drop rates. Rate.Drop.Item.GroupAmount repeats top-level groups for items.
        for (auto const& [groupId, groupRows] : groups)
        {
            auto addGroupRow = [&](LootRow const& row, double chance)
            {
                if (row.reference)
                    addReference(row, chance);
                else
                    addItem(row, chance * (isTopLevel ? rates.groupAmount : 1));
            };

            double taken = 0.0;
            uint32 equalCount = 0;
            for (LootRow const* row : groupRows)
            {
                if (row->chance == 0.0f)
                {
                    ++equalCount;
                    continue;
                }

                double const chance = std::min(1.0, row->chance / 100.0);
                addGroupRow(*row, std::max(0.0, std::min(chance, 1.0 - taken)));
                taken += chance;
            }

            if (!equalCount || taken >= 1.0)
                continue;

            for (LootRow const* row : groupRows)
                if (row->chance == 0.0f)
                    addGroupRow(*row, (1.0 - taken) / equalCount);
        }
    }

    bool IsWorldDropCandidate(ItemTemplate const* proto, std::unordered_set<uint32> const& questItems)
    {
        return IsKeepable(proto)
            && proto->Quality >= config.worldDropMinQuality
            && (!config.worldDropMaxItemLevel || proto->ItemLevel < config.worldDropMaxItemLevel)
            && proto->Bonding != BIND_QUEST_ITEM && proto->Bonding != BIND_QUEST_ITEM1
            && !proto->StartQuest
            && !questItems.count(proto->ItemId);
    }

    double GetWorldDropWeight(ItemTemplate const* proto)
    {
        uint32 const quality = std::clamp<uint32>(proto->Quality, ITEM_QUALITY_UNCOMMON, ITEM_QUALITY_EPIC);
        double weight = config.worldDropQualityWeights[quality - ITEM_QUALITY_UNCOMMON];

        if (proto->Class == ITEM_CLASS_RECIPE)
            weight *= config.worldDropRecipeWeight;

        return weight;
    }

    // Chance (0..1) that at least one of these independent chances hits
    double CombinedChance(std::vector<double> const& chances)
    {
        double missAll = 1.0;
        for (double chance : chances)
            missAll *= 1.0 - chance;

        return 1.0 - missAll;
    }

    struct RareItem
    {
        ItemTemplate const* proto;
        DropChance drop; // chance capped to 0..1
    };

    // Raises a creature's rare items so that at least one of them drops with MaxCombinedChance: compress their chances
    // toward the geometric mean, weight them by quality and recipe, then scale them to the target.
    // No item drops less often than before or more often than MaxItemChance.
    std::shared_ptr<WorldDropTable> BuildWorldDropTable(std::vector<RareItem> const& rareItems)
    {
        double const maxItemChance = config.worldDropMaxItemChance / 100.0;
        double const target = config.worldDropMaxCombinedChance / 100.0;
        double const compression = config.worldDropCompressionRatio;

        std::vector<double> original;
        original.reserve(rareItems.size());
        double logSum = 0.0;
        for (RareItem const& item : rareItems)
        {
            original.push_back(item.drop.chance);
            logSum += std::log(item.drop.chance);
        }

        if (CombinedChance(original) >= target)
            return nullptr; // its rare items already drop often enough

        double const geometricMean = std::exp(logSum / original.size());
        std::vector<double> shaped;
        shaped.reserve(original.size());
        double lowestShaped = 0.0;
        for (size_t i = 0; i < rareItems.size(); ++i)
        {
            double const value = std::pow(original[i], 1.0 - compression) * std::pow(geometricMean, compression)
                * GetWorldDropWeight(rareItems[i].proto);

            shaped.push_back(value);
            if (value > 0.0 && (lowestShaped == 0.0 || value < lowestShaped))
                lowestShaped = value;
        }

        if (lowestShaped == 0.0)
            return nullptr; // every weight is 0

        std::vector<double> boosted(original.size());
        auto applyScale = [&](double scale)
        {
            for (size_t i = 0; i < original.size(); ++i)
                boosted[i] = std::min(maxItemChance, std::max(original[i], scale * shaped[i]));

            return CombinedChance(boosted);
        };

        // `high` puts every weighted item at MaxItemChance. If that overshoots, search for the scale that hits the target.
        double high = maxItemChance / lowestShaped;
        double scale = high;
        if (applyScale(high) > target)
        {
            double low = 0.0;
            for (uint8 i = 0; i < 40; ++i)
            {
                double const middle = (low + high) / 2.0;
                if (applyScale(middle) > target)
                    high = middle;
                else
                    low = middle;
            }

            scale = low;
        }

        applyScale(scale);

        auto table = std::make_shared<WorldDropTable>();
        std::vector<double> extraChances;
        for (size_t i = 0; i < rareItems.size(); ++i)
        {
            // The normal roll already gives the original chance; the extra roll makes up the rest
            double const chance = (boosted[i] - original[i]) / (1.0 - original[i]);
            if (chance <= 0.0)
                continue;

            RareItem const& item = rareItems[i];
            table->items.push_back({ item.proto->ItemId, item.drop.minCount, item.drop.maxCount, float(chance) });
            extraChances.push_back(chance);
        }

        if (table->items.empty())
            return nullptr;

        table->chance = float(CombinedChance(extraChances));
        return table;
    }

    struct WorldDropTableHash
    {
        size_t operator()(std::shared_ptr<WorldDropTable const> const& table) const
        {
            size_t hash = table->items.size();
            for (WorldDropItem const& item : table->items)
                hash = (hash * 31 + item.itemId) * 31 + std::hash<float>()(item.chance);

            return hash;
        }
    };

    struct WorldDropTableEqual
    {
        bool operator()(std::shared_ptr<WorldDropTable const> const& left, std::shared_ptr<WorldDropTable const> const& right) const
        {
            return left->items == right->items;
        }
    };

    void BuildWorldDropLoot(LootCache& cache, std::unordered_set<uint32> const& lootIds, LootRows const& creatureRows,
        LootRows const& referenceRows)
    {
        uint32 const startTime = getMSTime();
        DropRates const rates = GetDropRates();
        double const maxItemChance = config.worldDropMaxItemChance / 100.0;

        std::unordered_set<uint32> questItems;
        for (auto const& [questId, quest] : sObjectMgr->GetQuestTemplates())
            for (uint32 itemId : quest->RequiredItemId)
                if (itemId)
                    questItems.insert(itemId);

        // Many creatures share the same reference tables, so many tables come out identical
        std::unordered_set<std::shared_ptr<WorldDropTable const>, WorldDropTableHash, WorldDropTableEqual> uniqueTables;
        size_t itemCount = 0;

        for (uint32 lootId : lootIds)
        {
            auto rowsItr = creatureRows.find(lootId);
            if (rowsItr == creatureRows.end())
                continue;

            DropChances chances;
            AddDropChances(rowsItr->second, referenceRows, rates, 1.0, true, 0, chances);

            std::vector<RareItem> rareItems;
            for (auto const& [itemId, drop] : chances)
            {
                double const chance = std::min(1.0, drop.chance);
                if (chance <= 0.0 || chance >= maxItemChance)
                    continue; // never drops, or already drops often enough

                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
                if (!proto || !IsWorldDropCandidate(proto, questItems))
                    continue;

                rareItems.push_back({ proto, { drop.minCount, drop.maxCount, chance } });
            }

            if (rareItems.empty())
                continue;

            // Same order for the same items, so identical tables can be shared
            std::sort(rareItems.begin(), rareItems.end(), [](RareItem const& left, RareItem const& right)
            {
                return left.proto->ItemId < right.proto->ItemId;
            });

            std::shared_ptr<WorldDropTable const> table = BuildWorldDropTable(rareItems);
            if (!table)
                continue;

            auto [itr, inserted] = uniqueTables.insert(std::move(table));
            if (inserted)
                itemCount += (*itr)->items.size();

            cache.worldDropLoot[lootId] = *itr;
        }

        LOG_INFO(LOG_NAME, "SoloBossLoot: World drop cache built: {} creature loot tables ({} unique, {} items) in {} ms",
            cache.worldDropLoot.size(), uniqueTables.size(), itemCount, GetMSTimeDiffToNow(startTime));
    }

    // Works out which items belong to each boss loot table (boss creatures and boss chests)
    void BuildBossLoot(LootCache& cache, std::unordered_set<uint32> const& bossCreatureLootIds,
        std::unordered_set<uint32> const& forcedItems, std::unordered_set<uint32> const& excludedItems,
        LootRows const& creatureRows, LootRows const& referenceRows)
    {
        uint32 const startTime = getMSTime();

        std::unordered_map<uint32, uint32> bossChestLootIds; // loot id -> chest entry
        for (uint32 chestEntry : cache.bossChests)
        {
            GameObjectTemplate const* chestTemplate = sObjectMgr->GetGameObjectTemplate(chestEntry);
            if (uint32 const lootId = chestTemplate->GetLootId())
                bossChestLootIds.try_emplace(lootId, chestEntry);
            else
                LOG_WARN(LOG_NAME, "SoloBossLoot: Boss chest {} ({}) has no loot id, so it gets no extra loot", chestTemplate->name, chestEntry);
        }

        // Anything reachable from a non-boss loot table is not boss loot
        std::unordered_set<uint32> nonBossReferences;
        std::unordered_set<uint32> nonBossItems;

        auto markNonBoss = [&](std::vector<LootRow> const& rows)
        {
            for (LootRow const& row : rows)
            {
                if (row.reference)
                    MarkReferenceNonBoss(row.reference, referenceRows, nonBossReferences, nonBossItems);
                else
                    nonBossItems.insert(row.item);
            }
        };

        struct BossTable
        {
            bool isChest;
            uint32 lootId;
            std::vector<LootRow> const* rows;
        };

        std::vector<BossTable> bossTables;

        for (auto const& [lootId, rows] : creatureRows)
        {
            if (bossCreatureLootIds.count(lootId))
                bossTables.push_back({ false, lootId, &rows });
            else
                markNonBoss(rows);
        }

        LootRows const gameobjectRows = LoadLootRows("gameobject_loot_template");
        for (auto const& [lootId, rows] : gameobjectRows)
        {
            if (bossChestLootIds.count(lootId))
                bossTables.push_back({ true, lootId, &rows });
            else
                markNonBoss(rows);
        }

        for (auto const& [lootId, chestEntry] : bossChestLootIds)
            if (!gameobjectRows.count(lootId))
                LOG_WARN(LOG_NAME, "SoloBossLoot: Boss chest {} has loot id {}, which has no gameobject_loot_template rows",
                    chestEntry, lootId);

        for (char const* table : { "fishing_loot_template", "item_loot_template", "pickpocketing_loot_template",
            "skinning_loot_template", "mail_loot_template", "spell_loot_template", "milling_loot_template",
            "prospecting_loot_template", "disenchant_loot_template", "player_loot_template" })
        {
            for (auto const& [entry, rows] : LoadLootRows(table))
                markNonBoss(rows);
        }

        // Items per boss table, and how many boss tables drop each item
        std::vector<std::unordered_map<uint32, BossItem>> tableItems;
        tableItems.reserve(bossTables.size());
        std::unordered_map<uint32, uint32> bossTableCount;

        for (BossTable const& table : bossTables)
        {
            tableItems.push_back(CollectLootTableItems(*table.rows, referenceRows));
            for (auto const& [itemId, item] : tableItems.back())
                ++bossTableCount[itemId];
        }

        // Why items were left out: per boss table (trace) and in total (debug)
        struct SkipCounts
        {
            uint32 notBossOnly = 0;
            uint32 lowQuality = 0;
            uint32 sharedPool = 0;
            uint32 excluded = 0;
        };

        SkipCounts totalSkipped;
        uint32 itemCount = 0;
        for (size_t i = 0; i < bossTables.size(); ++i)
        {
            BossItemList items;
            SkipCounts skipped;

            for (auto& [itemId, item] : tableItems[i])
            {
                if (excludedItems.count(itemId))
                {
                    ++skipped.excluded;
                    continue;
                }

                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
                if (!proto)
                    continue;

                if (!forcedItems.count(itemId))
                {
                    if (nonBossItems.count(itemId))
                    {
                        ++skipped.notBossOnly;
                        continue; // also drops from trash, world-drop lists, containers, ...
                    }

                    if (proto->Quality == ITEM_QUALITY_POOR || (IsGear(proto) && proto->Quality < ITEM_QUALITY_UNCOMMON))
                    {
                        ++skipped.lowQuality;
                        continue;
                    }

                    if (bossTableCount[itemId] > config.sharedPoolThreshold && !IsKeepable(proto))
                    {
                        ++skipped.sharedPool;
                        continue; // shared by many bosses and gets used up: would repeat at every boss
                    }
                }

                items.push_back(std::move(item));
            }

            LOG_TRACE(LOG_NAME, "SoloBossLoot: Boss {} loot table {}: {} items kept, skipped {} not boss-only, {} grey or white gear, "
                "{} shared pool, {} excluded by override", bossTables[i].isChest ? "chest" : "creature", bossTables[i].lootId,
                items.size(), skipped.notBossOnly, skipped.lowQuality, skipped.sharedPool, skipped.excluded);

            totalSkipped.notBossOnly += skipped.notBossOnly;
            totalSkipped.lowQuality += skipped.lowQuality;
            totalSkipped.sharedPool += skipped.sharedPool;
            totalSkipped.excluded += skipped.excluded;

            if (items.empty())
                continue;

            itemCount += items.size();
            auto& target = bossTables[i].isChest ? cache.chestLoot : cache.creatureLoot;
            target[bossTables[i].lootId] = std::move(items);
        }

        LOG_INFO(LOG_NAME, "SoloBossLoot: Boss loot cache built: {} creature and {} chest loot tables, {} items in {} ms",
            cache.creatureLoot.size(), cache.chestLoot.size(), itemCount, GetMSTimeDiffToNow(startTime));

        LOG_DEBUG(LOG_NAME, "SoloBossLoot: Boss loot items skipped (counted once per loot table): {} not boss-only, "
            "{} grey or white gear, {} shared pool, {} excluded by override",
            totalSkipped.notBossOnly, totalSkipped.lowQuality, totalSkipped.sharedPool, totalSkipped.excluded);
    }

    std::shared_ptr<LootCache const> BuildLootCache()
    {
        auto cache = std::make_shared<LootCache>();

        // Overrides
        std::unordered_set<uint32> forcedItems;
        std::unordered_set<uint32> excludedItems;
        uint32 overrideCount = 0;

        if (QueryResult result = WorldDatabase.Query("SELECT `SourceType`, `Entry`, `Mode` FROM `{}`", OVERRIDES_TABLE))
        {
            do
            {
                Field* fields = result->Fetch();
                uint8 const sourceType = fields[0].Get<uint8>();
                uint32 const entry     = fields[1].Get<uint32>();
                bool const include     = fields[2].Get<uint8>() != 0;

                switch (sourceType)
                {
                    case OVERRIDE_CREATURE:
                        if (!sObjectMgr->GetCreatureTemplate(entry))
                            LOG_WARN(LOG_NAME, "SoloBossLoot: `{}` has creature {}, which isn't in creature_template", OVERRIDES_TABLE, entry);
                        cache->creatureOverrides[entry] = include;
                        break;
                    case OVERRIDE_GAMEOBJECT:
                        if (!sObjectMgr->GetGameObjectTemplate(entry))
                            LOG_WARN(LOG_NAME, "SoloBossLoot: `{}` has gameobject {}, which isn't in gameobject_template", OVERRIDES_TABLE, entry);
                        else if (include)
                            cache->bossChests.insert(entry);
                        break;
                    case OVERRIDE_ITEM:
                        if (!sObjectMgr->GetItemTemplate(entry))
                            LOG_WARN(LOG_NAME, "SoloBossLoot: `{}` has item {}, which isn't in item_template", OVERRIDES_TABLE, entry);
                        (include ? forcedItems : excludedItems).insert(entry);
                        break;
                    default:
                        LOG_ERROR(LOG_NAME, "SoloBossLoot: `{}` has unknown SourceType {} (Entry {}), skipped", OVERRIDES_TABLE, sourceType, entry);
                        break;
                }

                ++overrideCount;
            } while (result->NextRow());
        }

        if (!overrideCount && config.bossLootEnabled)
            LOG_WARN(LOG_NAME, "SoloBossLoot: `{}` is empty or missing, so boss chests get no extra loot. "
                "Has the module's SQL been applied to the world database?", OVERRIDES_TABLE);

        // Boss creature loot ids: encounter bosses and boss-flagged creatures, plus their difficulty versions
        std::unordered_set<uint32> bossCreatureLootIds;
        std::unordered_set<uint32> bossDifficultyEntries;
        std::vector<std::pair<uint32, uint32>> otherCreatures; // entry, loot id
        for (auto const& [entry, creatureTemplate] : *sObjectMgr->GetCreatureTemplates())
        {
            bool isBoss = creatureTemplate.HasFlagsExtra(CREATURE_FLAG_EXTRA_DUNGEON_BOSS)
                || (creatureTemplate.type_flags & CREATURE_TYPE_FLAG_BOSS_MOB);

            if (auto itr = cache->creatureOverrides.find(entry); itr != cache->creatureOverrides.end())
                isBoss = itr->second;

            if (!isBoss)
            {
                if (creatureTemplate.lootid)
                    otherCreatures.emplace_back(entry, creatureTemplate.lootid);
                continue;
            }

            if (creatureTemplate.lootid)
                bossCreatureLootIds.insert(creatureTemplate.lootid);

            for (uint32 difficultyEntry : creatureTemplate.DifficultyEntry)
            {
                if (!difficultyEntry)
                    continue;

                bossDifficultyEntries.insert(difficultyEntry);
                if (CreatureTemplate const* difficultyTemplate = sObjectMgr->GetCreatureTemplate(difficultyEntry))
                    if (difficultyTemplate->lootid)
                        bossCreatureLootIds.insert(difficultyTemplate->lootid);
            }
        }

        // Every other creature's loot table gets world drops (a boss's heroic version isn't another creature)
        std::unordered_set<uint32> worldDropLootIds;
        for (auto const& [entry, lootId] : otherCreatures)
            if (!bossDifficultyEntries.count(entry))
                worldDropLootIds.insert(lootId);

        LootRows const referenceRows = LoadLootRows("reference_loot_template");
        LootRows const creatureRows = LoadLootRows("creature_loot_template");

        if (config.bossLootEnabled)
            BuildBossLoot(*cache, bossCreatureLootIds, forcedItems, excludedItems, creatureRows, referenceRows);
        else
            LOG_INFO(LOG_NAME, "SoloBossLoot: Boss loot disabled");

        if (config.worldDropEnabled)
            BuildWorldDropLoot(*cache, worldDropLootIds, creatureRows, referenceRows);
        else
            LOG_INFO(LOG_NAME, "SoloBossLoot: World drops disabled");

        return cache;
    }

    void RebuildLootCache()
    {
        if (!config.bossLootEnabled && !config.worldDropEnabled)
        {
            SetLootCache(nullptr);
            LOG_INFO(LOG_NAME, "SoloBossLoot: Boss loot and world drops disabled");
            return;
        }

        SetLootCache(BuildLootCache());
    }

    // ---------------------------------------------------------------------------
    // Who wants what
    // ---------------------------------------------------------------------------
    // Real players in the loot owner's group who are in the same instance (and, when `rewardSource` is given, close
    // enough to it to share its rewards). Bots never count.
    std::vector<Player*> GetRealPlayers(Player* lootOwner, WorldObject const* rewardSource = nullptr)
    {
        std::vector<Player*> players;

        auto addIfReal = [&](Player* player)
        {
            if (!player || !player->IsInMap(lootOwner) || !player->GetSession() || player->GetSession()->IsBot())
                return;

            if (rewardSource && !player->IsAtGroupRewardDistance(rewardSource))
                return;

            players.push_back(player);
        };

        if (Group* group = lootOwner->GetGroup())
        {
            for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
                addIfReal(itr->GetSource());
        }
        else
            addIfReal(lootOwner);

        return players;
    }

    // Highest armor type the player has learned: plate for a level 40+ warrior, mail below 40
    uint32 GetMainArmorSubClass(Player const* player)
    {
        if (player->HasSkill(SKILL_PLATE_MAIL))
            return ITEM_SUBCLASS_ARMOR_PLATE;
        if (player->HasSkill(SKILL_MAIL))
            return ITEM_SUBCLASS_ARMOR_MAIL;
        if (player->HasSkill(SKILL_LEATHER))
            return ITEM_SUBCLASS_ARMOR_LEATHER;
        return ITEM_SUBCLASS_ARMOR_CLOTH;
    }

    // Weapon types each class can learn to use
    uint32 GetWeaponSubClassMask(uint8 playerClass)
    {
        auto const bit = [](uint32 subClass) { return uint32(1) << subClass; };

        switch (playerClass)
        {
            case CLASS_WARRIOR:
                return bit(ITEM_SUBCLASS_WEAPON_AXE) | bit(ITEM_SUBCLASS_WEAPON_AXE2) | bit(ITEM_SUBCLASS_WEAPON_MACE)
                    | bit(ITEM_SUBCLASS_WEAPON_MACE2) | bit(ITEM_SUBCLASS_WEAPON_SWORD) | bit(ITEM_SUBCLASS_WEAPON_SWORD2)
                    | bit(ITEM_SUBCLASS_WEAPON_POLEARM) | bit(ITEM_SUBCLASS_WEAPON_STAFF) | bit(ITEM_SUBCLASS_WEAPON_FIST)
                    | bit(ITEM_SUBCLASS_WEAPON_DAGGER) | bit(ITEM_SUBCLASS_WEAPON_BOW) | bit(ITEM_SUBCLASS_WEAPON_GUN)
                    | bit(ITEM_SUBCLASS_WEAPON_CROSSBOW) | bit(ITEM_SUBCLASS_WEAPON_THROWN);
            case CLASS_PALADIN:
            case CLASS_DEATH_KNIGHT:
                return bit(ITEM_SUBCLASS_WEAPON_AXE) | bit(ITEM_SUBCLASS_WEAPON_AXE2) | bit(ITEM_SUBCLASS_WEAPON_MACE)
                    | bit(ITEM_SUBCLASS_WEAPON_MACE2) | bit(ITEM_SUBCLASS_WEAPON_SWORD) | bit(ITEM_SUBCLASS_WEAPON_SWORD2)
                    | bit(ITEM_SUBCLASS_WEAPON_POLEARM);
            case CLASS_HUNTER:
                return bit(ITEM_SUBCLASS_WEAPON_AXE) | bit(ITEM_SUBCLASS_WEAPON_AXE2) | bit(ITEM_SUBCLASS_WEAPON_SWORD)
                    | bit(ITEM_SUBCLASS_WEAPON_SWORD2) | bit(ITEM_SUBCLASS_WEAPON_POLEARM) | bit(ITEM_SUBCLASS_WEAPON_STAFF)
                    | bit(ITEM_SUBCLASS_WEAPON_FIST) | bit(ITEM_SUBCLASS_WEAPON_DAGGER) | bit(ITEM_SUBCLASS_WEAPON_BOW)
                    | bit(ITEM_SUBCLASS_WEAPON_GUN) | bit(ITEM_SUBCLASS_WEAPON_CROSSBOW) | bit(ITEM_SUBCLASS_WEAPON_THROWN);
            case CLASS_ROGUE:
                return bit(ITEM_SUBCLASS_WEAPON_AXE) | bit(ITEM_SUBCLASS_WEAPON_MACE) | bit(ITEM_SUBCLASS_WEAPON_SWORD)
                    | bit(ITEM_SUBCLASS_WEAPON_FIST) | bit(ITEM_SUBCLASS_WEAPON_DAGGER) | bit(ITEM_SUBCLASS_WEAPON_BOW)
                    | bit(ITEM_SUBCLASS_WEAPON_GUN) | bit(ITEM_SUBCLASS_WEAPON_CROSSBOW) | bit(ITEM_SUBCLASS_WEAPON_THROWN);
            case CLASS_PRIEST:
                return bit(ITEM_SUBCLASS_WEAPON_MACE) | bit(ITEM_SUBCLASS_WEAPON_STAFF) | bit(ITEM_SUBCLASS_WEAPON_DAGGER)
                    | bit(ITEM_SUBCLASS_WEAPON_WAND);
            case CLASS_SHAMAN:
                return bit(ITEM_SUBCLASS_WEAPON_AXE) | bit(ITEM_SUBCLASS_WEAPON_AXE2) | bit(ITEM_SUBCLASS_WEAPON_MACE)
                    | bit(ITEM_SUBCLASS_WEAPON_MACE2) | bit(ITEM_SUBCLASS_WEAPON_STAFF) | bit(ITEM_SUBCLASS_WEAPON_FIST)
                    | bit(ITEM_SUBCLASS_WEAPON_DAGGER);
            case CLASS_MAGE:
            case CLASS_WARLOCK:
                return bit(ITEM_SUBCLASS_WEAPON_SWORD) | bit(ITEM_SUBCLASS_WEAPON_STAFF) | bit(ITEM_SUBCLASS_WEAPON_DAGGER)
                    | bit(ITEM_SUBCLASS_WEAPON_WAND);
            case CLASS_DRUID:
                return bit(ITEM_SUBCLASS_WEAPON_MACE) | bit(ITEM_SUBCLASS_WEAPON_MACE2) | bit(ITEM_SUBCLASS_WEAPON_POLEARM)
                    | bit(ITEM_SUBCLASS_WEAPON_STAFF) | bit(ITEM_SUBCLASS_WEAPON_FIST) | bit(ITEM_SUBCLASS_WEAPON_DAGGER);
            default:
                return 0;
        }
    }

    bool IsGearUsableBy(Player const* player, ItemTemplate const* proto)
    {
        uint8 const playerClass = player->getClass();

        if (proto->Class == ITEM_CLASS_WEAPON)
            return proto->SubClass < 32 && (GetWeaponSubClassMask(playerClass) & (uint32(1) << proto->SubClass)) != 0;

        if (proto->InventoryType == INVTYPE_CLOAK)
            return true;

        switch (proto->SubClass)
        {
            case ITEM_SUBCLASS_ARMOR_MISC: // rings, necks, trinkets, off-hand items
                return true;
            case ITEM_SUBCLASS_ARMOR_CLOTH:
            case ITEM_SUBCLASS_ARMOR_LEATHER:
            case ITEM_SUBCLASS_ARMOR_MAIL:
            case ITEM_SUBCLASS_ARMOR_PLATE:
                return proto->SubClass == GetMainArmorSubClass(player);
            case ITEM_SUBCLASS_ARMOR_BUCKLER:
            case ITEM_SUBCLASS_ARMOR_SHIELD:
                return playerClass == CLASS_WARRIOR || playerClass == CLASS_PALADIN || playerClass == CLASS_SHAMAN;
            case ITEM_SUBCLASS_ARMOR_LIBRAM:
                return playerClass == CLASS_PALADIN;
            case ITEM_SUBCLASS_ARMOR_IDOL:
                return playerClass == CLASS_DRUID;
            case ITEM_SUBCLASS_ARMOR_TOTEM:
                return playerClass == CLASS_SHAMAN;
            case ITEM_SUBCLASS_ARMOR_SIGIL:
                return playerClass == CLASS_DEATH_KNIGHT;
            default:
                return false;
        }
    }

    // Recipes, mounts and pets teach a spell and disappear, so check the spell instead of the bags
    bool IsAlreadyLearned(Player const* player, ItemTemplate const* proto)
    {
        _Spell const& learnSpell = proto->Spells[1];
        return learnSpell.SpellTrigger == ITEM_SPELLTRIGGER_LEARN_SPELL_ID && learnSpell.SpellId > 0
            && player->HasSpell(static_cast<uint32>(learnSpell.SpellId));
    }

    // Why a player doesn't want an item, or nullptr if they do: class, gear type, learned, profession and owned checks.
    // Loot conditions (LootItem::AllowedForPlayer) are checked by the caller.
    char const* GetUnwantedReason(Player const* player, ItemTemplate const* proto, bool needsProfession)
    {
        if (proto->AllowableClass && !(proto->AllowableClass & player->getClassMask()))
            return "other class"; // tier tokens, class books, ...

        if (proto->AllowableRace && !(proto->AllowableRace & player->getRaceMask()))
            return "other race";

        if (IsGear(proto) && !IsGearUsableBy(player, proto))
            return "gear type";

        if (IsAlreadyLearned(player, proto))
            return "already learned";

        if (needsProfession && proto->Class == ITEM_CLASS_RECIPE && proto->RequiredSkill && !player->HasSkill(proto->RequiredSkill))
            return "no profession"; // recipe for a profession they don't have

        if (config.skipOwnedItems && player->HasItemCount(proto->ItemId, 1, true))
            return "owned";

        return nullptr;
    }

    bool PlayerWantsItem(Player const* player, ItemTemplate const* proto, bool needsProfession)
    {
        return !GetUnwantedReason(player, proto, needsProfession);
    }

    // ---------------------------------------------------------------------------
    // Log text. Only call these inside LOG_* macros, which skip their arguments when the level is off.
    // ---------------------------------------------------------------------------
    std::string DescribeSource(WorldObject const* source)
    {
        return fmt::format("{} ({} {})", source->GetName(),
            source->GetTypeId() == TYPEID_GAMEOBJECT ? "gameobject" : "creature", source->GetEntry());
    }

    std::string DescribeItem(uint32 itemId)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        return fmt::format("{} ({})", proto ? proto->Name1 : std::string("unknown item"), itemId);
    }

    std::string DescribeItems(std::vector<LootStoreItem> const& items)
    {
        if (items.empty())
            return "none";

        std::string text;
        for (LootStoreItem const& item : items)
            text += (text.empty() ? "" : ", ") + DescribeItem(item.itemid);

        return text;
    }

    std::string DescribePlayers(std::vector<Player*> const& players)
    {
        std::string text;
        for (Player const* player : players)
            text += (text.empty() ? "" : ", ") + player->GetName();

        return text;
    }

    // Each real player's reason for not wanting a boss item, e.g. "Alice: owned, Bob: gear type"
    std::string DescribeUnwanted(std::vector<Player*> const& players, ItemTemplate const* proto, LootItem const& lootItem,
        ObjectGuid source)
    {
        std::string text;
        for (Player const* player : players)
        {
            char const* reason = GetUnwantedReason(player, proto, false);
            if (!reason && !lootItem.AllowedForPlayer(player, source))
                reason = "loot conditions";

            text += fmt::format("{}{}: {}", text.empty() ? "" : ", ", player->GetName(), reason ? reason : "wants it");
        }

        return text;
    }

    bool IsBoss(LootCache const& cache, Creature const* creature)
    {
        if (auto itr = cache.creatureOverrides.find(creature->GetEntry()); itr != cache.creatureOverrides.end())
            return itr->second;

        return creature->IsDungeonBoss() || creature->isWorldBoss();
    }

    // ---------------------------------------------------------------------------
    // Boss loot
    // ---------------------------------------------------------------------------
    bool DropsInLootMode(BossItem const& item, uint16 lootMode)
    {
        return std::any_of(item.modePaths.begin(), item.modePaths.end(), [lootMode](ModePath const& path)
        {
            return std::all_of(path.begin(), path.end(), [lootMode](uint16 mask) { return (mask & lootMode) != 0; });
        });
    }

    BossItemList const* FindCreatureBossItems(LootCache const& cache, Map* map, ObjectGuid source,
        LootStore const& store, LootTemplate const* tab, WorldObject const*& sourceObject)
    {
        if (!source.IsCreature())
            return nullptr;

        Creature* creature = map->GetCreature(source);
        if (!creature || !IsBoss(cache, creature))
            return nullptr;

        uint32 const lootId = creature->GetCreatureTemplate()->lootid;
        if (!lootId || store.GetLootFor(lootId) != tab)
        {
            LOG_DEBUG(LOG_BOSS, "SoloBossLoot: [{}] Loot came from another loot table (a script?), nothing added", DescribeSource(creature));
            return nullptr;
        }

        auto itr = cache.creatureLoot.find(lootId);
        if (itr == cache.creatureLoot.end())
        {
            LOG_DEBUG(LOG_BOSS, "SoloBossLoot: [{}] Loot table {} has no boss-only items, nothing added", DescribeSource(creature), lootId);
            return nullptr;
        }

        sourceObject = creature;
        return &itr->second;
    }

    BossItemList const* FindChestBossItems(LootCache const& cache, Map* map, ObjectGuid source,
        LootStore const& store, LootTemplate const* tab, WorldObject const*& sourceObject)
    {
        if (!source.IsGameObject())
            return nullptr;

        GameObject* chest = map->GetGameObject(source);
        if (!chest || !cache.bossChests.count(chest->GetEntry()))
            return nullptr;

        uint32 const lootId = chest->GetGOInfo()->GetLootId();
        if (!lootId || store.GetLootFor(lootId) != tab)
        {
            LOG_DEBUG(LOG_BOSS, "SoloBossLoot: [{}] Loot came from another loot table (a script?), nothing added", DescribeSource(chest));
            return nullptr;
        }

        auto itr = cache.chestLoot.find(lootId);
        if (itr == cache.chestLoot.end())
        {
            LOG_DEBUG(LOG_BOSS, "SoloBossLoot: [{}] Loot table {} has no boss-only items, nothing added", DescribeSource(chest), lootId);
            return nullptr;
        }

        sourceObject = chest;
        return &itr->second;
    }

    void AddBossLoot(LootCache const& cache, Loot* loot, LootTemplate const* tab, LootStore const& store, Player* lootOwner,
        uint16 lootMode, Map* map, bool isCreatureLoot)
    {
        ObjectGuid const source = loot->sourceWorldObjectGUID;
        WorldObject const* sourceObject = nullptr;
        BossItemList const* bossItems = isCreatureLoot
            ? FindCreatureBossItems(cache, map, source, store, tab, sourceObject)
            : FindChestBossItems(cache, map, source, store, tab, sourceObject);

        if (!bossItems)
            return;

        std::vector<Player*> const players = GetRealPlayers(lootOwner);
        if (players.empty())
        {
            LOG_DEBUG(LOG_BOSS, "SoloBossLoot: [{}] No real players in the instance, nothing added", DescribeSource(sourceObject));
            return;
        }

        std::unordered_set<uint32> present;
        for (LootItem const& item : loot->items)
            present.insert(item.itemid);
        for (LootItem const& item : loot->quest_items)
            present.insert(item.itemid);

        // Checked once here instead of for every item
        bool const trace = sLog->ShouldLog(LOG_BOSS, LOG_LEVEL_TRACE);

        std::vector<LootStoreItem> picks;
        for (BossItem const& bossItem : *bossItems)
        {
            if (present.count(bossItem.itemId))
            {
                if (trace)
                    LOG_TRACE(LOG_BOSS, "SoloBossLoot: [{}] {} skipped: already in the loot window",
                        DescribeSource(sourceObject), DescribeItem(bossItem.itemId));
                continue;
            }

            if (!DropsInLootMode(bossItem, lootMode))
            {
                if (trace)
                    LOG_TRACE(LOG_BOSS, "SoloBossLoot: [{}] {} skipped: doesn't drop in loot mode {}",
                        DescribeSource(sourceObject), DescribeItem(bossItem.itemId), lootMode);
                continue;
            }

            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(bossItem.itemId);
            if (!proto)
                continue;

            LootStoreItem storeItem(bossItem.itemId, 0, 100.0f, false, LOOT_MODE_DEFAULT, 0, bossItem.minCount, bossItem.maxCount);
            LootItem lootItem(storeItem);
            tab->CopyConditions(&lootItem);
            storeItem.conditions = lootItem.conditions;

            bool const wanted = std::any_of(players.begin(), players.end(), [&](Player const* player)
            {
                // The same check the loot window uses: conditions, faction, hidden recipes, finished quest starters, ...
                return PlayerWantsItem(player, proto, false) && lootItem.AllowedForPlayer(player, source);
            });

            if (wanted)
                picks.push_back(std::move(storeItem));
            else if (trace)
                LOG_TRACE(LOG_BOSS, "SoloBossLoot: [{}] {} skipped: not wanted ({})",
                    DescribeSource(sourceObject), DescribeItem(bossItem.itemId), DescribeUnwanted(players, proto, lootItem, source));
        }

        size_t const freeSlots = loot->items.size() < MAX_NR_LOOT_ITEMS ? MAX_NR_LOOT_ITEMS - loot->items.size() : 0;
        size_t const wantedCount = picks.size();

        // Everything fits: add it all. Otherwise every wanted item gets the same odds of making the cut.
        if (picks.size() > freeSlots)
        {
            Acore::Containers::RandomShuffle(picks);

            if (trace)
                for (auto itr = picks.begin() + freeSlots; itr != picks.end(); ++itr)
                    LOG_TRACE(LOG_BOSS, "SoloBossLoot: [{}] {} skipped: not picked ({} wanted items for {} free slots)",
                        DescribeSource(sourceObject), DescribeItem(itr->itemid), wantedCount, freeSlots);

            picks.erase(picks.begin() + freeSlots, picks.end());
        }

        for (LootStoreItem const& pick : picks)
            loot->AddItem(pick);

        LOG_DEBUG(LOG_BOSS, "SoloBossLoot: [{}] {} boss items, {} wanted by real players ({}), {} free slots, {} added: {}",
            DescribeSource(sourceObject), bossItems->size(), wantedCount, DescribePlayers(players), freeSlots, picks.size(),
            DescribeItems(picks));
    }

    // ---------------------------------------------------------------------------
    // World drops
    // ---------------------------------------------------------------------------
    void AddWorldDrop(LootCache const& cache, Loot* loot, LootTemplate const* tab, LootStore const& store, Player* lootOwner,
        uint16 lootMode, Map* map)
    {
        ObjectGuid const source = loot->sourceWorldObjectGUID;
        if (!(lootMode & LOOT_MODE_DEFAULT) || !source.IsCreature() || map->IsBattlegroundOrArena())
            return;

        Creature* creature = map->GetCreature(source);
        if (!creature || IsBoss(cache, creature))
            return; // bosses get boss loot instead

        uint32 const lootId = creature->GetCreatureTemplate()->lootid;
        if (!lootId || store.GetLootFor(lootId) != tab)
            return; // loot filled from some other table, e.g. by a script

        auto itr = cache.worldDropLoot.find(lootId);
        if (itr == cache.worldDropLoot.end())
            return;

        WorldDropTable const& table = *itr->second;

        // One roll for the whole table first, so the checks below only run when something could drop
        if (!roll_chance_f(table.chance * 100.0f))
        {
            LOG_TRACE(LOG_WORLD_DROP, "SoloBossLoot: [{}] No world drop: table roll ({:.2f}%) missed",
                DescribeSource(creature), table.chance * 100.0f);
            return;
        }

        if (loot->items.size() >= MAX_NR_LOOT_ITEMS)
        {
            LOG_TRACE(LOG_WORLD_DROP, "SoloBossLoot: [{}] No world drop: the loot window is full", DescribeSource(creature));
            return;
        }

        std::vector<Player*> const players = GetRealPlayers(lootOwner, creature);
        if (players.empty())
        {
            LOG_TRACE(LOG_WORLD_DROP, "SoloBossLoot: [{}] No world drop: no real player close enough", DescribeSource(creature));
            return;
        }

        std::unordered_set<uint32> present;
        for (LootItem const& item : loot->items)
            present.insert(item.itemid);
        for (LootItem const& item : loot->quest_items)
            present.insert(item.itemid);

        std::vector<WorldDropItem const*> wanted;
        std::vector<double> weights;
        double missAllWanted = 1.0;
        for (WorldDropItem const& item : table.items)
        {
            if (present.count(item.itemId))
                continue;

            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item.itemId);
            if (!proto)
                continue;

            bool const isWanted = std::any_of(players.begin(), players.end(), [&](Player const* player)
            {
                return PlayerWantsItem(player, proto, true);
            });

            if (!isWanted)
                continue;

            wanted.push_back(&item);
            weights.push_back(item.chance);
            missAllWanted *= 1.0 - item.chance;
        }

        if (wanted.empty())
        {
            LOG_TRACE(LOG_WORLD_DROP, "SoloBossLoot: [{}] No world drop: none of its {} rare items are wanted by real players ({})",
                DescribeSource(creature), table.items.size(), DescribePlayers(players));
            return;
        }

        // Items nobody wants don't drop, and aren't replaced by wanted ones, so each wanted item keeps its own chance
        float const wantedChance = float((1.0 - missAllWanted) / table.chance * 100.0);
        if (!roll_chance_f(wantedChance))
        {
            LOG_TRACE(LOG_WORLD_DROP, "SoloBossLoot: [{}] No world drop: {} of {} rare items wanted, wanted roll ({:.2f}%) missed",
                DescribeSource(creature), wanted.size(), table.items.size(), wantedChance);
            return;
        }

        size_t const wantedCount = wanted.size();

        while (!wanted.empty())
        {
            auto const pickItr = Acore::Containers::SelectRandomWeightedContainerElement(wanted, weights);
            size_t const index = std::distance(wanted.cbegin(), pickItr);
            WorldDropItem const& pick = **pickItr;

            LootStoreItem storeItem(pick.itemId, 0, 100.0f, false, LOOT_MODE_DEFAULT, 0, pick.minCount, pick.maxCount);
            LootItem lootItem(storeItem);
            tab->CopyConditions(&lootItem);
            storeItem.conditions = lootItem.conditions;

            // Conditions are only copied for the picked item: doing it for hundreds of items would be slow
            bool const allowed = std::any_of(players.begin(), players.end(), [&](Player const* player)
            {
                return lootItem.AllowedForPlayer(player, source);
            });

            if (allowed)
            {
                loot->AddItem(storeItem);
                LOG_DEBUG(LOG_WORLD_DROP, "SoloBossLoot: [{}] World drop: added {} ({:.3f}% extra roll, {} of {} rare items wanted by {})",
                    DescribeSource(creature), DescribeItem(pick.itemId), pick.chance * 100.0f, wantedCount, table.items.size(),
                    DescribePlayers(players));
                return;
            }

            LOG_TRACE(LOG_WORLD_DROP, "SoloBossLoot: [{}] {} not picked: its loot conditions don't allow any real player",
                DescribeSource(creature), DescribeItem(pick.itemId));

            wanted.erase(wanted.begin() + index);
            weights.erase(weights.begin() + index);
        }

        LOG_TRACE(LOG_WORLD_DROP, "SoloBossLoot: [{}] No world drop: loot conditions ruled out all {} wanted items",
            DescribeSource(creature), wantedCount);
    }

    // ---------------------------------------------------------------------------
    // Scripts
    // ---------------------------------------------------------------------------
    class SoloBossLoot_WorldScript : public WorldScript
    {
    public:
        SoloBossLoot_WorldScript()
            : WorldScript("SoloBossLoot_WorldScript", { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP }) { }

        void OnAfterConfigLoad(bool reload) override
        {
            LoadConfig();

            // On the first load the world data isn't loaded yet; OnStartup builds the cache then
            if (reload)
                RebuildLootCache();
        }

        void OnStartup() override
        {
            RebuildLootCache();
        }
    };

    class SoloBossLoot_MiscScript : public MiscScript
    {
    public:
        SoloBossLoot_MiscScript()
            : MiscScript("SoloBossLoot_MiscScript", { MISCHOOK_ON_AFTER_LOOT_TEMPLATE_PROCESS }) { }

        void OnAfterLootTemplateProcess(Loot* loot, LootTemplate const* tab, LootStore const& store, Player* lootOwner,
            bool /*personal*/, bool /*noEmptyError*/, uint16 lootMode) override
        {
            if (!loot || !tab || !lootOwner)
                return;

            bool const isCreatureLoot = &store == &LootTemplates_Creature;
            if (!isCreatureLoot && &store != &LootTemplates_Gameobject)
                return;

            Map* map = lootOwner->GetMap();
            if (!map)
                return;

            std::shared_ptr<LootCache const> cache = GetLootCache();
            if (!cache)
                return;

            if (config.bossLootEnabled && map->IsDungeon())
                AddBossLoot(*cache, loot, tab, store, lootOwner, lootMode, map, isCreatureLoot);

            if (isCreatureLoot && config.worldDropEnabled)
                AddWorldDrop(*cache, loot, tab, store, lootOwner, lootMode, map);
        }
    };
}

void AddSoloBossLootScripts()
{
    new SoloBossLoot_WorldScript();
    new SoloBossLoot_MiscScript();
}
