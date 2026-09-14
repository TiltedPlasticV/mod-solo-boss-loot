/*
 * mod-solo-boss-loot
 *
 * When a dungeon or raid boss (or a boss chest) is looted, adds the boss's own loot that a real player in the
 * group can use and doesn't have yet. Bots are ignored. The normal loot roll is untouched; this only adds items.
 *
 * At startup (and on .reload config) the module works out which items belong to each boss loot table:
 *   - only boss loot tables drop the item (no trash mob, world-drop list, container, ...)
 *   - items shared by many bosses are only kept if they can be kept (gear, bags, mounts, pets, recipes)
 *   - quest-required items are left to the normal roll, and hard-mode (LootMode) rules are kept
 * The `solo_boss_loot_overrides` world table adds boss chests and extra bosses, and forces items in or out.
 */

#include "Config.h"
#include "Containers.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "GameObject.h"
#include "Group.h"
#include "LootMgr.h"
#include "Map.h"
#include "MiscScript.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Timer.h"
#include "WorldScript.h"
#include "WorldSession.h"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    constexpr char const* LOG_NAME = "module.solo_boss_loot";
    constexpr char const* OVERRIDES_TABLE = "solo_boss_loot_overrides";

    // ---------------------------------------------------------------------------
    // Configuration
    // ---------------------------------------------------------------------------
    struct ModuleConfig
    {
        bool enabled = true;
        bool skipOwnedItems = true;
        uint32 sharedPoolThreshold = 3;
    };

    ModuleConfig config;

    void LoadConfig()
    {
        config.enabled             = sConfigMgr->GetOption<bool>("SoloBossLoot.Enable", true);
        config.skipOwnedItems      = sConfigMgr->GetOption<bool>("SoloBossLoot.SkipOwnedItems", true);
        config.sharedPoolThreshold = sConfigMgr->GetOption<uint32>("SoloBossLoot.SharedPoolThreshold", 3);
    }

    // ---------------------------------------------------------------------------
    // Boss loot cache
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

    struct BossLootCache
    {
        std::unordered_map<uint32, BossItemList> creatureLoot; // creature loot id -> items to add
        std::unordered_map<uint32, BossItemList> chestLoot;    // gameobject loot id -> items to add
        std::unordered_map<uint32, bool> creatureOverrides;    // creature entry -> is a boss
        std::unordered_set<uint32> bossChests;                 // gameobject entries
    };

    // Loot is generated on map threads, the cache is rebuilt on the world thread: swap it under a lock
    std::shared_ptr<BossLootCache const> bossLootCache;
    std::shared_mutex bossLootCacheLock;

    std::shared_ptr<BossLootCache const> GetBossLootCache()
    {
        std::shared_lock lock(bossLootCacheLock);
        return bossLootCache;
    }

    void SetBossLootCache(std::shared_ptr<BossLootCache const> cache)
    {
        std::unique_lock lock(bossLootCacheLock);
        bossLootCache = std::move(cache);
    }

    struct LootRow
    {
        uint32 item;
        uint32 reference;
        uint16 lootMode;
        uint8 minCount;
        uint8 maxCount;
        bool questRequired;
    };

    using LootRows = std::unordered_map<uint32, std::vector<LootRow>>; // loot entry -> rows

    LootRows LoadLootRows(char const* table)
    {
        LootRows rows;

        QueryResult result = WorldDatabase.Query(
            "SELECT `Entry`, `Item`, `Reference`, `QuestRequired`, `LootMode`, `MinCount`, `MaxCount` FROM `{}`", table);
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
        constexpr uint8 MaxReferenceDepth = 8;

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
                if (current.depth >= MaxReferenceDepth)
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

    std::shared_ptr<BossLootCache const> BuildBossLootCache()
    {
        uint32 const startTime = getMSTime();
        auto cache = std::make_shared<BossLootCache>();

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
                        cache->creatureOverrides[entry] = include;
                        break;
                    case OVERRIDE_GAMEOBJECT:
                        if (include)
                            cache->bossChests.insert(entry);
                        break;
                    case OVERRIDE_ITEM:
                        (include ? forcedItems : excludedItems).insert(entry);
                        break;
                    default:
                        LOG_ERROR(LOG_NAME, "SoloBossLoot: `{}` has unknown SourceType {} (Entry {}), skipped", OVERRIDES_TABLE, sourceType, entry);
                        break;
                }

                ++overrideCount;
            } while (result->NextRow());
        }

        if (!overrideCount)
            LOG_WARN(LOG_NAME, "SoloBossLoot: `{}` is empty or missing, so boss chests get no extra loot. "
                "Has the module's SQL been applied to the world database?", OVERRIDES_TABLE);

        // Boss creature loot ids: encounter bosses and boss-flagged creatures, plus their difficulty versions
        std::unordered_set<uint32> bossCreatureLootIds;
        for (auto const& [entry, creatureTemplate] : *sObjectMgr->GetCreatureTemplates())
        {
            bool isBoss = creatureTemplate.HasFlagsExtra(CREATURE_FLAG_EXTRA_DUNGEON_BOSS)
                || (creatureTemplate.type_flags & CREATURE_TYPE_FLAG_BOSS_MOB);

            if (auto itr = cache->creatureOverrides.find(entry); itr != cache->creatureOverrides.end())
                isBoss = itr->second;

            if (!isBoss)
                continue;

            if (creatureTemplate.lootid)
                bossCreatureLootIds.insert(creatureTemplate.lootid);

            for (uint32 difficultyEntry : creatureTemplate.DifficultyEntry)
                if (difficultyEntry)
                    if (CreatureTemplate const* difficultyTemplate = sObjectMgr->GetCreatureTemplate(difficultyEntry))
                        if (difficultyTemplate->lootid)
                            bossCreatureLootIds.insert(difficultyTemplate->lootid);
        }

        std::unordered_set<uint32> bossChestLootIds;
        for (uint32 chestEntry : cache->bossChests)
            if (GameObjectTemplate const* chestTemplate = sObjectMgr->GetGameObjectTemplate(chestEntry))
                if (uint32 const lootId = chestTemplate->GetLootId())
                    bossChestLootIds.insert(lootId);

        // Anything reachable from a non-boss loot table is not boss loot
        LootRows const referenceRows = LoadLootRows("reference_loot_template");
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

        LootRows const creatureRows = LoadLootRows("creature_loot_template");
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

        uint32 itemCount = 0;
        for (size_t i = 0; i < bossTables.size(); ++i)
        {
            BossItemList items;

            for (auto& [itemId, item] : tableItems[i])
            {
                if (excludedItems.count(itemId))
                    continue;

                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
                if (!proto)
                    continue;

                if (!forcedItems.count(itemId))
                {
                    if (nonBossItems.count(itemId))
                        continue; // also drops from trash, world-drop lists, containers, ...

                    if (proto->Quality == ITEM_QUALITY_POOR || (IsGear(proto) && proto->Quality < ITEM_QUALITY_UNCOMMON))
                        continue;

                    if (bossTableCount[itemId] > config.sharedPoolThreshold && !IsKeepable(proto))
                        continue; // shared by many bosses and gets used up: would repeat at every boss
                }

                items.push_back(std::move(item));
            }

            if (items.empty())
                continue;

            itemCount += items.size();
            auto& target = bossTables[i].isChest ? cache->chestLoot : cache->creatureLoot;
            target[bossTables[i].lootId] = std::move(items);
        }

        LOG_INFO(LOG_NAME, "SoloBossLoot: Boss loot cache built: {} creature and {} chest loot tables, {} items, {} overrides in {} ms",
            cache->creatureLoot.size(), cache->chestLoot.size(), itemCount, overrideCount, GetMSTimeDiffToNow(startTime));

        return cache;
    }

    void RebuildBossLootCache()
    {
        if (!config.enabled)
        {
            SetBossLootCache(nullptr);
            LOG_INFO(LOG_NAME, "SoloBossLoot: Module disabled");
            return;
        }

        SetBossLootCache(BuildBossLootCache());
    }

    // ---------------------------------------------------------------------------
    // Filling the loot window
    // ---------------------------------------------------------------------------
    bool DropsInLootMode(BossItem const& item, uint16 lootMode)
    {
        return std::any_of(item.modePaths.begin(), item.modePaths.end(), [lootMode](ModePath const& path)
        {
            return std::all_of(path.begin(), path.end(), [lootMode](uint16 mask) { return (mask & lootMode) != 0; });
        });
    }

    // Real players in the loot owner's group who are in the same instance. Bots never count.
    std::vector<Player*> GetRealPlayers(Player* lootOwner)
    {
        std::vector<Player*> players;

        auto addIfReal = [&](Player* player)
        {
            if (player && player->IsInMap(lootOwner) && player->GetSession() && !player->GetSession()->IsBot())
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

    bool PlayerWantsItem(Player const* player, ItemTemplate const* proto, LootItem const& lootItem, ObjectGuid source)
    {
        if (proto->AllowableClass && !(proto->AllowableClass & player->getClassMask()))
            return false; // tier tokens, class books, ... for another class

        if (proto->AllowableRace && !(proto->AllowableRace & player->getRaceMask()))
            return false;

        if (IsGear(proto) && !IsGearUsableBy(player, proto))
            return false;

        if (IsAlreadyLearned(player, proto))
            return false;

        if (config.skipOwnedItems && player->HasItemCount(lootItem.itemid, 1, true))
            return false;

        // The same check the loot window uses: conditions, faction, hidden recipes, finished quest starters, ...
        return lootItem.AllowedForPlayer(player, source);
    }

    BossItemList const* FindCreatureBossItems(BossLootCache const& cache, Map* map, ObjectGuid source,
        LootStore const& store, LootTemplate const* tab, std::string& sourceName)
    {
        if (!source.IsCreature())
            return nullptr;

        Creature* creature = map->GetCreature(source);
        if (!creature)
            return nullptr;

        bool isBoss = creature->IsDungeonBoss() || creature->isWorldBoss();
        if (auto itr = cache.creatureOverrides.find(creature->GetEntry()); itr != cache.creatureOverrides.end())
            isBoss = itr->second;

        if (!isBoss)
            return nullptr;

        uint32 const lootId = creature->GetCreatureTemplate()->lootid;
        if (!lootId || store.GetLootFor(lootId) != tab)
            return nullptr; // loot filled from some other table, e.g. by a script

        auto itr = cache.creatureLoot.find(lootId);
        if (itr == cache.creatureLoot.end())
            return nullptr;

        sourceName = fmt::format("{} (creature {})", creature->GetName(), creature->GetEntry());
        return &itr->second;
    }

    BossItemList const* FindChestBossItems(BossLootCache const& cache, Map* map, ObjectGuid source,
        LootStore const& store, LootTemplate const* tab, std::string& sourceName)
    {
        if (!source.IsGameObject())
            return nullptr;

        GameObject* chest = map->GetGameObject(source);
        if (!chest || !cache.bossChests.count(chest->GetEntry()))
            return nullptr;

        uint32 const lootId = chest->GetGOInfo()->GetLootId();
        if (!lootId || store.GetLootFor(lootId) != tab)
            return nullptr;

        auto itr = cache.chestLoot.find(lootId);
        if (itr == cache.chestLoot.end())
            return nullptr;

        sourceName = fmt::format("{} (gameobject {})", chest->GetName(), chest->GetEntry());
        return &itr->second;
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
                RebuildBossLootCache();
        }

        void OnStartup() override
        {
            RebuildBossLootCache();
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
            if (!config.enabled || !loot || !tab || !lootOwner)
                return;

            bool const isCreatureLoot = &store == &LootTemplates_Creature;
            if (!isCreatureLoot && &store != &LootTemplates_Gameobject)
                return;

            Map* map = lootOwner->GetMap();
            if (!map || !map->IsDungeon())
                return;

            std::shared_ptr<BossLootCache const> cache = GetBossLootCache();
            if (!cache)
                return;

            std::string sourceName;
            BossItemList const* bossItems = isCreatureLoot
                ? FindCreatureBossItems(*cache, map, loot->sourceWorldObjectGUID, store, tab, sourceName)
                : FindChestBossItems(*cache, map, loot->sourceWorldObjectGUID, store, tab, sourceName);

            if (!bossItems)
                return;

            std::vector<Player*> const players = GetRealPlayers(lootOwner);
            if (players.empty())
            {
                LOG_DEBUG(LOG_NAME, "SoloBossLoot: [{}] No real players in the instance, nothing added", sourceName);
                return;
            }

            std::unordered_set<uint32> present;
            for (LootItem const& item : loot->items)
                present.insert(item.itemid);
            for (LootItem const& item : loot->quest_items)
                present.insert(item.itemid);

            std::vector<LootStoreItem> picks;
            for (BossItem const& bossItem : *bossItems)
            {
                if (present.count(bossItem.itemId) || !DropsInLootMode(bossItem, lootMode))
                    continue;

                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(bossItem.itemId);
                if (!proto)
                    continue;

                LootStoreItem storeItem(bossItem.itemId, 0, 100.0f, false, LOOT_MODE_DEFAULT, 0, bossItem.minCount, bossItem.maxCount);
                LootItem lootItem(storeItem);
                tab->CopyConditions(&lootItem);
                storeItem.conditions = lootItem.conditions;

                bool const wanted = std::any_of(players.begin(), players.end(), [&](Player const* player)
                {
                    return PlayerWantsItem(player, proto, lootItem, loot->sourceWorldObjectGUID);
                });

                if (wanted)
                    picks.push_back(std::move(storeItem));
            }

            size_t const freeSlots = loot->items.size() < MAX_NR_LOOT_ITEMS ? MAX_NR_LOOT_ITEMS - loot->items.size() : 0;
            size_t const wantedCount = picks.size();

            // Everything fits: add it all. Otherwise every wanted item gets the same odds of making the cut.
            if (picks.size() > freeSlots)
            {
                Acore::Containers::RandomShuffle(picks);
                picks.erase(picks.begin() + freeSlots, picks.end());
            }

            for (LootStoreItem const& pick : picks)
                loot->AddItem(pick);

            LOG_DEBUG(LOG_NAME, "SoloBossLoot: [{}] {} boss items, {} wanted by {} real player(s), {} free slots, {} added",
                sourceName, bossItems->size(), wantedCount, players.size(), freeSlots, picks.size());
        }
    };
}

void AddSoloBossLootScripts()
{
    new SoloBossLoot_WorldScript();
    new SoloBossLoot_MiscScript();
}
