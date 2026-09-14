#include "ScriptMgr.h"
#include "Config.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "GlobalScript.h"
#include "Group.h"
#include "Item.h"
#include "ItemEnchantmentMgr.h"
#include "LootMgr.h"
#include "Map.h"
#include "MiscScript.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "WorldScript.h"

#include <algorithm>
#include <random>
#include <unordered_map>
#include <unordered_set>

#define DLR_LOG_TYPE "dlr"

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
struct DynamicLootRatesConfig
{
    bool     enabled                   = true;
    bool     bossGuaranteedLoot        = false;

    uint32   dungeonLootGroupRate      = 1;
    uint32   dungeonLootReferenceRate  = 1;
    uint32   raidLootGroupRate         = 1;
    uint32   raidLootReferenceRate     = 1;

    uint32   sharedThreshold           = 3;
};

static DynamicLootRatesConfig config;

// ---------------------------------------------------------------------------
// Shared-item/reference detection caches (built at startup)
// ---------------------------------------------------------------------------
static std::unordered_map<uint32, uint32> s_refUsageCount;
static std::unordered_map<uint32, uint32> s_itemUsageCount;

static void BuildUsageCaches()
{
    s_refUsageCount.clear();
    s_itemUsageCount.clear();

    const char* allTables[] = {
        "creature_loot_template",
        "gameobject_loot_template",
        "fishing_loot_template",
        "item_loot_template",
        "pickpocketing_loot_template",
        "skinning_loot_template",
        "mail_loot_template",
        "spell_loot_template",
        "milling_loot_template",
        "prospecting_loot_template",
        "disenchant_loot_template",
        "reference_loot_template",
        "player_loot_template",
        nullptr
    };

    std::unordered_map<uint32, std::unordered_set<uint64>> refSources;

    for (int t = 0; allTables[t] != nullptr; ++t)
    {
        QueryResult result = WorldDatabase.Query(
            "SELECT Entry, Reference FROM {} WHERE Reference != 0", allTables[t]);

        if (!result)
            continue;

        uint32 rowCount = 0;
        do
        {
            Field* fields = result->Fetch();
            uint32 entry     = fields[0].Get<uint32>();
            int32  reference = fields[1].Get<int32>();
            uint32 absRef    = static_cast<uint32>(std::abs(reference));

            uint64 sourceKey = (static_cast<uint64>(t) << 32) | entry;
            refSources[absRef].insert(sourceKey);
            ++rowCount;
        } while (result->NextRow());

        LOG_DEBUG(DLR_LOG_TYPE, "DLR: Scanned {} — {} reference rows", allTables[t], rowCount);
    }

    for (auto const& [refId, sources] : refSources)
        s_refUsageCount[refId] = static_cast<uint32>(sources.size());

    {
        QueryResult result = WorldDatabase.Query(
            "SELECT Item, COUNT(DISTINCT Entry) FROM creature_loot_template "
            "WHERE Reference = 0 GROUP BY Item");

        if (result)
        {
            do
            {
                Field* fields = result->Fetch();
                s_itemUsageCount[fields[0].Get<uint32>()] = fields[1].Get<uint32>();
            } while (result->NextRow());
        }
    }

    uint32 sharedRefCount = 0, bossRefCount = 0;
    for (auto const& [refId, count] : s_refUsageCount)
    {
        if (count > config.sharedThreshold) ++sharedRefCount;
        else ++bossRefCount;
    }

    uint32 sharedItemCount = 0, bossItemCount = 0;
    for (auto const& [itemId, count] : s_itemUsageCount)
    {
        if (count > config.sharedThreshold) ++sharedItemCount;
        else ++bossItemCount;
    }

    LOG_INFO(DLR_LOG_TYPE, "DLR: Usage caches built — references: {} total ({} shared, {} boss-specific), "
             "direct items: {} total ({} shared, {} boss-specific), threshold: {}",
             s_refUsageCount.size(), sharedRefCount, bossRefCount,
             s_itemUsageCount.size(), sharedItemCount, bossItemCount,
             config.sharedThreshold);
}

static bool IsSharedReference(uint32 referenceId)
{
    auto it = s_refUsageCount.find(referenceId);
    return (it != s_refUsageCount.end()) && (it->second > config.sharedThreshold);
}

static bool IsSharedItem(uint32 itemId)
{
    auto it = s_itemUsageCount.find(itemId);
    return (it != s_itemUsageCount.end()) && (it->second > config.sharedThreshold);
}

// ---------------------------------------------------------------------------
// Boss loot cache
// ---------------------------------------------------------------------------
struct BossLootEntry
{
    uint32 itemid;
    uint8  mincount;
    uint8  maxcount;
};

static std::unordered_map<uint32, std::vector<BossLootEntry>> s_bossLootCache;

static void ResolveReferenceItems(uint32 refEntry,
                                  std::vector<BossLootEntry>& outItems,
                                  std::unordered_set<uint32>& visitedRefs)
{
    if (!visitedRefs.insert(refEntry).second)
        return;

    QueryResult result = WorldDatabase.Query(
        "SELECT Item, Reference, MinCount, MaxCount, QuestRequired "
        "FROM reference_loot_template WHERE Entry = {}", refEntry);

    if (!result)
        return;

    do
    {
        Field* fields = result->Fetch();
        uint32 item      = fields[0].Get<uint32>();
        int32  reference = fields[1].Get<int32>();
        uint8  mincount  = fields[2].Get<uint8>();
        uint8  maxcount  = fields[3].Get<uint8>();
        bool   questReq  = fields[4].Get<bool>();

        if (questReq)
            continue;

        if (reference != 0)
        {
            uint32 absRef = static_cast<uint32>(std::abs(reference));
            if (!IsSharedReference(absRef))
                ResolveReferenceItems(absRef, outItems, visitedRefs);
        }
        else
        {
            if (!IsSharedItem(item) && sObjectMgr->GetItemTemplate(item))
                outItems.push_back({item, mincount, maxcount});
        }
    } while (result->NextRow());
}

static void BuildBossLootCache()
{
    s_bossLootCache.clear();

    QueryResult result = WorldDatabase.Query(
        "SELECT Entry, Item, Reference, MinCount, MaxCount, QuestRequired "
        "FROM creature_loot_template");

    if (!result)
        return;

    struct RawEntry { uint32 item; int32 reference; uint8 mincount; uint8 maxcount; bool questReq; };
    std::unordered_map<uint32, std::vector<RawEntry>> rawEntries;

    do
    {
        Field* fields = result->Fetch();
        uint32 entry     = fields[0].Get<uint32>();
        uint32 item      = fields[1].Get<uint32>();
        int32  reference = fields[2].Get<int32>();
        uint8  mincount  = fields[3].Get<uint8>();
        uint8  maxcount  = fields[4].Get<uint8>();
        bool   questReq  = fields[5].Get<bool>();

        rawEntries[entry].push_back({item, reference, mincount, maxcount, questReq});
    } while (result->NextRow());

    uint32 totalItems = 0;
    for (auto const& [entry, rows] : rawEntries)
    {
        std::vector<BossLootEntry> bossItems;

        for (auto const& row : rows)
        {
            if (row.questReq)
                continue;

            if (row.reference != 0)
            {
                uint32 absRef = static_cast<uint32>(std::abs(row.reference));
                if (!IsSharedReference(absRef))
                {
                    std::unordered_set<uint32> visited;
                    ResolveReferenceItems(absRef, bossItems, visited);
                }
            }
            else
            {
                if (!IsSharedItem(row.item) && sObjectMgr->GetItemTemplate(row.item))
                    bossItems.push_back({row.item, row.mincount, row.maxcount});
            }
        }

        if (!bossItems.empty())
        {
            totalItems += bossItems.size();
            s_bossLootCache[entry] = std::move(bossItems);
        }
    }

    LOG_INFO(DLR_LOG_TYPE, "DLR: Boss loot cache built — {} creature loot entries, {} total boss-specific items",
             s_bossLootCache.size(), totalItems);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static bool IsInDungeon(Map const* map)
{
    return map && map->IsDungeon() && !map->IsRaid();
}

static bool IsInRaid(Map const* map)
{
    return map && map->IsRaid();
}

/// Returns the boss creature if this is a dungeon/raid boss loot, nullptr otherwise.
static Creature* GetBossCreature(Player const* player, Loot& loot)
{
    if (!player || !player->GetMap() || !player->GetMap()->IsDungeon())
        return nullptr;

    ObjectGuid sourceGuid = loot.sourceWorldObjectGUID;
    if (!sourceGuid || !sourceGuid.IsCreature())
        return nullptr;

    Creature* creature = player->GetMap()->GetCreature(sourceGuid);
    if (!creature || !creature->IsDungeonBoss())
        return nullptr;

    return creature;
}

/// Format: "BossName (entry 12345, lootid 12345)"
static std::string FormatCreatureDebugName(Creature const* creature)
{
    if (!creature)
        return "<unknown>";
    return fmt::format("{} (entry {}, lootid {})",
                       creature->GetName(), creature->GetEntry(),
                       creature->GetCreatureTemplate()->lootid);
}

// ---------------------------------------------------------------------------
// WorldScript
// ---------------------------------------------------------------------------
class DynamicLootRates_WorldScript : public WorldScript
{
public:
    DynamicLootRates_WorldScript()
        : WorldScript("DynamicLootRates_WorldScript",
                       { WORLDHOOK_ON_BEFORE_CONFIG_LOAD, WORLDHOOK_ON_STARTUP }) {}

    void OnBeforeConfigLoad(bool reload) override
    {
        config.enabled                  = sConfigMgr->GetOption<bool>("DynamicLootRates.Enable", true);
        config.bossGuaranteedLoot       = sConfigMgr->GetOption<bool>("DynamicLootRates.Boss.GuaranteedLoot", false);
        config.dungeonLootGroupRate     = sConfigMgr->GetOption<uint32>("DynamicLootRates.Dungeon.Rate.GroupAmount", 1);
        config.dungeonLootReferenceRate = sConfigMgr->GetOption<uint32>("DynamicLootRates.Dungeon.Rate.ReferencedAmount", 1);
        config.raidLootGroupRate        = sConfigMgr->GetOption<uint32>("DynamicLootRates.Raid.Rate.GroupAmount", 1);
        config.raidLootReferenceRate    = sConfigMgr->GetOption<uint32>("DynamicLootRates.Raid.Rate.ReferencedAmount", 1);
        config.sharedThreshold          = sConfigMgr->GetOption<uint32>("DynamicLootRates.SharedThreshold", 3);

        LOG_INFO(DLR_LOG_TYPE, "DLR: Config {} — enabled={}, bossGuaranteed={}, sharedThreshold={}, "
                 "dungeonGroupRate={}, dungeonRefRate={}, raidGroupRate={}, raidRefRate={}",
                 reload ? "reloaded" : "loaded",
                 config.enabled, config.bossGuaranteedLoot, config.sharedThreshold,
                 config.dungeonLootGroupRate, config.dungeonLootReferenceRate,
                 config.raidLootGroupRate, config.raidLootReferenceRate);
    }

    void OnStartup() override
    {
        if (!config.enabled)
        {
            LOG_INFO(DLR_LOG_TYPE, "DLR: Module disabled, skipping cache build");
            return;
        }

        BuildUsageCaches();

        if (config.bossGuaranteedLoot)
            BuildBossLootCache();
    }
};

// ---------------------------------------------------------------------------
// GlobalScript — dungeon/raid rate multipliers ONLY (not boss guaranteed)
// ---------------------------------------------------------------------------
class DynamicLootRates_GlobalScript : public GlobalScript
{
public:
    DynamicLootRates_GlobalScript()
        : GlobalScript("DynamicLootRates_GlobalScript",
          {
              GLOBALHOOK_ON_AFTER_CALCULATE_LOOT_GROUP_AMOUNT,
              GLOBALHOOK_ON_AFTER_REF_COUNT
          }) {}

    void OnAfterCalculateLootGroupAmount(Player const* player, Loot& loot,
                                         uint16 /*lootMode*/, uint32& groupAmount,
                                         LootStore const& store) override
    {
        if (!config.enabled)
            return;

        // Only affect creature loot — not skinning, pickpocketing, fishing, etc.
        if (&store != &LootTemplates_Creature)
            return;

        // Boss guaranteed loot is handled entirely by post-process injection
        if (config.bossGuaranteedLoot && GetBossCreature(player, loot))
            return;

        Map const* map = player->GetMap();
        uint32 newAmount = groupAmount;

        if (IsInDungeon(map))
            newAmount = config.dungeonLootGroupRate;
        else if (IsInRaid(map))
            newAmount = config.raidLootGroupRate;

        if (newAmount != groupAmount)
        {
            LOG_DEBUG(DLR_LOG_TYPE, "DLR: [{}] Group amount {} -> {} ({})",
                      player->GetMap()->GetMapName(), groupAmount, newAmount,
                      IsInDungeon(map) ? "dungeon" : "raid");
            groupAmount = newAmount;
        }
    }

    void OnAfterRefCount(Player const* player, LootStoreItem* /*lootStoreItem*/,
                         Loot& loot, bool /*canRate*/, uint16 /*lootMode*/,
                         uint32& maxcount, LootStore const& store) override
    {
        if (!config.enabled)
            return;

        // Only affect creature loot
        if (&store != &LootTemplates_Creature)
            return;

        // Boss guaranteed loot is handled entirely by post-process injection
        if (config.bossGuaranteedLoot && GetBossCreature(player, loot))
            return;

        Map const* map = player->GetMap();
        uint32 newMaxcount = maxcount;

        if (IsInDungeon(map))
            newMaxcount = std::max(maxcount, config.dungeonLootReferenceRate);
        else if (IsInRaid(map))
            newMaxcount = std::max(maxcount, config.raidLootReferenceRate);

        if (newMaxcount != maxcount)
        {
            LOG_DEBUG(DLR_LOG_TYPE, "DLR: [{}] Ref maxcount {} -> {} ({})",
                      player->GetMap()->GetMapName(), maxcount, newMaxcount,
                      IsInDungeon(map) ? "dungeon" : "raid");
            maxcount = newMaxcount;
        }
    }
};

// ---------------------------------------------------------------------------
// MiscScript — post-process: inject missing boss-specific items
// ---------------------------------------------------------------------------
class DynamicLootRates_MiscScript : public MiscScript
{
public:
    DynamicLootRates_MiscScript()
        : MiscScript("DynamicLootRates_MiscScript",
                      { MISCHOOK_ON_AFTER_LOOT_TEMPLATE_PROCESS }) {}

    void OnAfterLootTemplateProcess(Loot* loot, LootTemplate const* tab,
                                    LootStore const& store, Player* lootOwner,
                                    bool /*personal*/, bool /*noEmptyError*/,
                                    uint16 /*lootMode*/) override
    {
        if (!config.enabled || !config.bossGuaranteedLoot)
            return;
        if (!loot || !lootOwner)
            return;

        // Only affect creature loot
        if (&store != &LootTemplates_Creature)
            return;

        Creature* creature = GetBossCreature(lootOwner, *loot);
        if (!creature)
            return;

        uint32 lootEntry = creature->GetCreatureTemplate()->lootid;
        std::string bossName = FormatCreatureDebugName(creature);

        auto cacheIt = s_bossLootCache.find(lootEntry);
        if (cacheIt == s_bossLootCache.end())
        {
            LOG_DEBUG(DLR_LOG_TYPE, "DLR: [{}] No boss-specific items cached, skipping", bossName);
            return;
        }

        auto const& cachedItems = cacheIt->second;

        // Build set of item IDs already present in both regular and quest loot
        std::unordered_set<uint32> existingItems;
        for (auto const& li : loot->items)
            existingItems.insert(li.itemid);
        for (auto const& li : loot->quest_items)
            existingItems.insert(li.itemid);

        uint32 alreadyPresent = 0;
        uint32 injected = 0;
        uint32 skippedCapacity = 0;
        uint32 skippedNoTemplate = 0;

        LOG_DEBUG(DLR_LOG_TYPE, "DLR: [{}] Post-process start — {} items generated, {} quest items, "
                  "{} boss-specific items in cache",
                  bossName, loot->items.size(), loot->quest_items.size(), cachedItems.size());

        for (auto const& bossItem : cachedItems)
        {
            if (existingItems.count(bossItem.itemid))
            {
                ++alreadyPresent;
                continue;
            }

            if (loot->items.size() >= MAX_NR_LOOT_ITEMS)
            {
                ++skippedCapacity;
                continue;
            }

            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(bossItem.itemid);
            if (!proto)
            {
                ++skippedNoTemplate;
                continue;
            }

            // Build a fully initialized LootItem
            LootItem li;
            li.itemid            = bossItem.itemid;
            li.itemIndex         = loot->items.size();
            li.count             = urand(bossItem.mincount, bossItem.maxcount);
            li.randomSuffix      = GenerateEnchSuffixFactor(bossItem.itemid);
            li.randomPropertyId  = Item::GenerateItemRandomPropertyId(bossItem.itemid);
            li.freeforall        = proto->HasFlag(ITEM_FLAG_MULTI_DROP);
            li.follow_loot_rules = proto->HasFlagCu(ITEM_FLAGS_CU_FOLLOW_LOOT_RULES);
            li.needs_quest       = false;
            li.is_looted         = false;
            li.is_blocked        = false;
            li.is_underthreshold = false;
            li.is_counted        = false;
            li.rollWinnerGUID    = ObjectGuid::Empty;
            li.groupid           = 0;

            // Copy conditions from the loot template if available
            if (tab)
                tab->CopyConditions(&li);

            loot->items.push_back(li);
            existingItems.insert(bossItem.itemid);

            // unlootedCount accounting — match Loot::AddItem() logic
            bool canSeeItem = false;
            if (Player* owner = ObjectAccessor::FindPlayer(loot->lootOwnerGUID))
            {
                if (Group* group = owner->GetGroup())
                {
                    for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
                    {
                        if (Player* member = itr->GetSource())
                        {
                            if (li.AllowedForPlayer(member, loot->sourceWorldObjectGUID))
                            {
                                canSeeItem = true;
                                break;
                            }
                        }
                    }
                }
                else if (li.AllowedForPlayer(owner, loot->sourceWorldObjectGUID))
                {
                    canSeeItem = true;
                }
            }

            if (canSeeItem && li.conditions.empty() && !proto->HasFlag(ITEM_FLAG_MULTI_DROP))
                ++loot->unlootedCount;

            ++injected;

            uint32 injCount = li.count;
            LOG_DEBUG(DLR_LOG_TYPE, "DLR:   [{}] INJECTED {} ({}) x{}", bossName,
                      bossItem.itemid, proto->Name1, injCount);
        }

        LOG_DEBUG(DLR_LOG_TYPE, "DLR: [{}] Post-process done — {} injected, {} already present, "
                  "{} skipped (cap), {} skipped (no template), {} total items, unlootedCount={}",
                  bossName, injected, alreadyPresent, skippedCapacity, skippedNoTemplate,
                  loot->items.size(), loot->unlootedCount);

        // If we somehow exceeded the client limit (shouldn't happen with the
        // capacity check above, but defensive), shuffle and trim
        if (loot->items.size() > MAX_NR_LOOT_ITEMS)
        {
            LOG_WARN(DLR_LOG_TYPE, "DLR: [{}] Exceeded MAX_NR_LOOT_ITEMS ({} > {}), trimming!",
                     bossName, loot->items.size(), MAX_NR_LOOT_ITEMS);

            std::shuffle(loot->items.begin(), loot->items.end(),
                         std::mt19937{std::random_device{}()});

            loot->items.resize(MAX_NR_LOOT_ITEMS);

            // Rebuild itemIndex and unlootedCount
            loot->unlootedCount = 0;
            for (uint32 i = 0; i < loot->items.size(); ++i)
            {
                loot->items[i].itemIndex = i;

                if (!loot->items[i].is_looted &&
                    !loot->items[i].freeforall &&
                     loot->items[i].conditions.empty())
                {
                    ItemTemplate const* p = sObjectMgr->GetItemTemplate(loot->items[i].itemid);
                    if (p && !p->HasFlag(ITEM_FLAG_MULTI_DROP))
                        ++loot->unlootedCount;
                }
            }
        }

        // Final loot table dump at debug level
        for (uint32 i = 0; i < loot->items.size(); ++i)
        {
            LootItem const& item = loot->items[i];
            ItemTemplate const* p = sObjectMgr->GetItemTemplate(item.itemid);
            uint32 dumpCount = item.count;
            uint32 dumpFfa   = item.freeforall ? 1 : 0;
            uint32 dumpQuest = item.needs_quest ? 1 : 0;
            LOG_DEBUG(DLR_LOG_TYPE, "DLR:   [{}] slot={} item={} ({}) x{} ffa={} quest={}",
                      bossName, i, item.itemid,
                      p ? p->Name1 : std::string("<unknown>"),
                      dumpCount, dumpFfa, dumpQuest);
        }
    }
};

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
void AddDynamicLootRateScripts()
{
    new DynamicLootRates_WorldScript();
    new DynamicLootRates_GlobalScript();
    new DynamicLootRates_MiscScript();
}