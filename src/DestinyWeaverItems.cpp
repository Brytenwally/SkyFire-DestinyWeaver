/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*
* Item scaling: gear a character with open world scaling loots (or gets from a scaled
* quest) below their level is raised to the item level an item of its kind has at that
* level, and stays there: heirloom-like, but frozen at the level it was looted at.
*
* The raise is the 5.4 item upgrade modifier on that one item (Item::SetUpgradeId). The
* upgrade ids are this module's own ItemUpgrade.db2 rows, sent to clients as hotfixes
* (ItemScalingHooks), so the client recomputes the tooltip; the server applies the same
* numbers through ItemScalingHooks::StatTemplate. Upgrades only, never downgrades.
*
* Stats follow the client's formula for upgraded items, checked against the client in
* game (14 tooltip values on a 5.4 robe and a level 20 shirt): RandPropPoints(item level,
* quality, slot) x allocation / 10000, minus int(socket cost multiplier x
* gtItemSocketCostPerLevel(item level)), rounded. Armor and weapon damage come from the
* item level tables, as for any item.
*
* The target item level for a character level is the median item level of the items of
* the same kind (class, subclass, slot, quality) that require that level, from the item
* template store, widening to the subclass, then the class, and up to 10 levels away (the
* reference index of the owner's Azerothcore-Level-60-World item scaler).
*/

#include "DestinyWeaver.h"

#include "ByteBuffer.h"
#include "Config.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "Item.h"
#include "ItemPrototype.h"
#include "ItemScalingHooks.h"
#include "LocalLevelScaling.h"
#include "Log.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"
#include "ScriptMgr.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

// ObjectMgr.cpp: the item damage tables by item level (ItemDamage*.dbc).
void FillItemDamageFields(float* minDamage, float* maxDamage, float* dps, uint32 itemLevel, uint32 itemClass,
    uint32 itemSubClass, uint32 quality, uint32 delay, float statScalingFactor, uint32 inventoryType, uint32 flags2);

namespace
{
    // Synthetic ItemUpgrade.db2 rows: id = UPGRADE_ID_BASE + ID_OFFSET + shift, item level
    // bonus = shift. ItemUpgrade.db2 (5.4.8 build 18273): ID, ItemUpgradePathID,
    // ItemLevelBonus (total), PrevUpgradeID, CurrencyID, CurrencyCost. The path must be one
    // of ItemUpgradePath.dbc's (a DBC, not hotfixable): 124 is the 5.4 one.
    constexpr uint32 UPGRADE_ID_BASE = 900000;
    constexpr int32 ID_OFFSET = 1000;
    constexpr uint32 UPGRADE_PATH = 124;
    constexpr int32 MAX_SHIFT = 600;                // rows announced: +1 .. +600 item levels
    constexpr uint32 HOTFIX_TIMESTAMP = 1761980400; // bump when the rows change, so clients re-fetch them

    bool g_itemScaling = false;          // DestinyWeaver.ItemScaling (and the module enabled)
    bool g_questRewards = true;          // DestinyWeaver.ItemScaling.QuestRewards

    bool IsModuleUpgradeId(uint32 id)
    {
        return id > UPGRADE_ID_BASE + ID_OFFSET && id <= UPGRADE_ID_BASE + ID_OFFSET + MAX_SHIFT;
    }

    int32 ShiftOf(uint32 upgradeId)
    {
        return IsModuleUpgradeId(upgradeId) ? int32(upgradeId - UPGRADE_ID_BASE) - ID_OFFSET : 0;
    }

    uint32 UpgradeIdFor(int32 shift)
    {
        return shift > 0 && shift <= MAX_SHIFT ? UPGRADE_ID_BASE + uint32(ID_OFFSET + shift) : 0;
    }

    // ---- The client's stat formula ----------------------------------------------------

    // RandPropPoints column for a slot, as the 5.4.8 client picks it (bows and guns are
    // two-handers, wands one-handers); -1 = no budget.
    int32 BudgetColumn(ItemTemplate const* proto)
    {
        switch (proto->InventoryType)
        {
            case INVTYPE_HEAD: case INVTYPE_BODY: case INVTYPE_CHEST: case INVTYPE_LEGS:
            case INVTYPE_RANGED: case INVTYPE_2HWEAPON: case INVTYPE_ROBE: case INVTYPE_THROWN:
                return 0;
            case INVTYPE_RANGEDRIGHT:
                return proto->SubClass == ITEM_SUBCLASS_WEAPON_WAND ? 3 : 0;
            case INVTYPE_WEAPON: case INVTYPE_WEAPONMAINHAND: case INVTYPE_WEAPONOFFHAND:
                return 3;
            case INVTYPE_SHOULDERS: case INVTYPE_WAIST: case INVTYPE_FEET: case INVTYPE_HANDS:
            case INVTYPE_TRINKET:
                return 1;
            case INVTYPE_NECK: case INVTYPE_WRISTS: case INVTYPE_FINGER: case INVTYPE_SHIELD:
            case INVTYPE_CLOAK: case INVTYPE_HOLDABLE:
                return 2;
            case INVTYPE_RELIC:
                return 4;
            default:
                return -1;
        }
    }

    // Stat budget of an item of this quality and slot at an item level; 0 for poor and
    // common items, which the client does not scale either.
    float Budget(ItemTemplate const* proto, uint32 itemLevel)
    {
        int32 const column = BudgetColumn(proto);
        RandomPropertiesPointsEntry const* points = sRandomPropertiesPointsStore.LookupEntry(itemLevel);
        if (column < 0 || !points)
            return 0.0f;
        switch (proto->Quality)
        {
            case ITEM_QUALITY_UNCOMMON:
                return float(points->UncommonPropertiesPoints[column]);
            case ITEM_QUALITY_RARE:
            case ITEM_QUALITY_HEIRLOOM:
                return float(points->RarePropertiesPoints[column]);
            case ITEM_QUALITY_EPIC:
            case ITEM_QUALITY_LEGENDARY:
            case ITEM_QUALITY_ARTIFACT:
                return float(points->EpicPropertiesPoints[column]);
            default:
                return 0.0f;
        }
    }

    float SocketCost(uint32 itemLevel)
    {
        GtItemSocketCostPerLevelEntry const* entry = itemLevel ? sGtItemSocketCostPerLevelStore.LookupEntry(itemLevel - 1) : nullptr;
        return entry ? entry->cost : 0.0f;
    }

    // One stat at another item level as the client computes it. The database keeps the
    // socket cost multiplier rounded (1.5 as 2, 0.75 as 1), so it is recovered from the
    // item's own value at its base item level, to a quarter. False without allocation data.
    bool ClientStatValue(ItemTemplate const* proto, uint8 index, uint32 itemLevel, int32& value)
    {
        _ItemStat const& stat = proto->ItemStat[index];
        float const baseBudget = Budget(proto, proto->ItemLevel);
        float const newBudget = Budget(proto, itemLevel);
        if (stat.ItemStatUnk1 <= 0 || baseBudget <= 0.0f || newBudget <= 0.0f)
            return false;

        float multiplier = 0.0f;
        if (stat.ItemStatUnk2)
            if (float const baseCost = SocketCost(proto->ItemLevel); baseCost > 0.0f)
            {
                float const deducted = baseBudget * float(stat.ItemStatUnk1) * 0.0001f - float(stat.ItemStatValue);
                multiplier = std::max(0.0f, std::round(deducted / baseCost * 4.0f) / 4.0f);
            }

        float result = newBudget * float(stat.ItemStatUnk1) * 0.0001f;
        result -= float(int32(multiplier * SocketCost(itemLevel)));
        value = int32(std::floor(result + 0.5f));
        return true;
    }

    std::mutex g_templateLock;
    std::map<std::pair<uint32, int32>, std::unique_ptr<ItemTemplate>> g_templates;

    // The template at ItemLevel + shift; cached, valid for the server's lifetime.
    ItemTemplate const* ShiftedTemplate(ItemTemplate const* proto, int32 shift)
    {
        if (!proto || shift <= 0)
            return proto;

        std::lock_guard<std::mutex> guard(g_templateLock);
        auto key = std::make_pair(proto->ItemId, shift);
        auto it = g_templates.find(key);
        if (it != g_templates.end())
            return it->second.get();

        auto shifted = std::make_unique<ItemTemplate>(*proto);
        shifted->ItemLevel = proto->ItemLevel + uint32(shift);

        // Items without allocation data scale by the budget ratio instead.
        float const oldBudget = Budget(proto, proto->ItemLevel);
        float const newBudget = Budget(proto, shifted->ItemLevel);
        double const factor = oldBudget > 0.0f && newBudget > 0.0f ? double(newBudget) / double(oldBudget)
            : double(shifted->ItemLevel) / double(std::max<uint32>(proto->ItemLevel, 1));
        for (uint8 i = 0; i < MAX_ITEM_PROTO_STATS; ++i)
        {
            if (!proto->ItemStat[i].ItemStatValue)
                continue;
            int32 value = 0;
            if (!ClientStatValue(proto, i, shifted->ItemLevel, value))
                value = int32(std::lround(double(proto->ItemStat[i].ItemStatValue) * factor));
            shifted->ItemStat[i].ItemStatValue = value;
        }

        // Armor: ItemTemplate::GetArmor() reads ItemLevel. Weapon damage: the damage tables.
        if (proto->Class == ITEM_CLASS_WEAPON)
        {
            float minDamage = 0.0f, maxDamage = 0.0f, dps = 0.0f;
            FillItemDamageFields(&minDamage, &maxDamage, &dps, shifted->ItemLevel, proto->Class, proto->SubClass,
                proto->Quality, proto->Delay, proto->StatScalingFactor, proto->InventoryType, proto->Flags2);
            if (dps > 0.0f)
            {
                shifted->DamageMin = minDamage;
                shifted->DamageMax = maxDamage;
                shifted->DPS = dps;
            }
        }

        ItemTemplate const* result = shifted.get();
        g_templates.emplace(key, std::move(shifted));
        return result;
    }

    // ---- Target item level per character level ---------------------------------------

    bool IsJunkName(std::string name)
    {
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        for (size_t at = name.find("test"); at != std::string::npos; at = name.find("test", at + 1))
            if (at == 0 || !std::isalpha(static_cast<unsigned char>(name[at - 1])))
                return true;
        return name.rfind("qa", 0) == 0 || name.find("deprecated") != std::string::npos
            || name.rfind("monster -", 0) == 0;
    }

    // Item levels by required level, in three ever broader buckets (all keep the quality).
    struct ReferenceIndex
    {
        using ByLevel = std::map<uint32, std::vector<uint32>>;
        std::map<uint64, ByLevel> exact;     // class, subclass, slot, quality
        std::map<uint64, ByLevel> subclass;  // class, subclass, quality
        std::map<uint64, ByLevel> klass;     // class, quality
    };

    uint64 Key(uint32 a, uint32 b, uint32 c, uint32 d)
    {
        return (uint64(a) << 48) | (uint64(b & 0xFFFF) << 32) | (uint64(c & 0xFFFF) << 16) | uint64(d & 0xFFFF);
    }

    ReferenceIndex const& Index()
    {
        static ReferenceIndex index;
        static std::once_flag built;
        std::call_once(built, []
        {
            uint32 count = 0;
            for (auto const& pair : *sObjectMgr->GetItemTemplateStore())
            {
                ItemTemplate const& proto = pair.second;
                if (proto.Class != ITEM_CLASS_WEAPON && proto.Class != ITEM_CLASS_ARMOR)
                    continue;
                if (proto.Quality < ITEM_QUALITY_UNCOMMON || proto.Quality > ITEM_QUALITY_EPIC)
                    continue;
                if (!proto.RequiredLevel || !proto.ItemLevel || proto.ScalingStatDistribution || IsJunkName(proto.Name1))
                    continue;
                index.exact[Key(proto.Class, proto.SubClass, proto.InventoryType, proto.Quality)][proto.RequiredLevel].push_back(proto.ItemLevel);
                index.subclass[Key(proto.Class, proto.SubClass, 0, proto.Quality)][proto.RequiredLevel].push_back(proto.ItemLevel);
                index.klass[Key(proto.Class, 0, 0, proto.Quality)][proto.RequiredLevel].push_back(proto.ItemLevel);
                ++count;
            }
            for (auto* buckets : { &index.exact, &index.subclass, &index.klass })
                for (auto& bucket : *buckets)
                    for (auto& level : bucket.second)
                        std::sort(level.second.begin(), level.second.end());
            SF_LOG_INFO("modules", "[mod-destiny-weaver] Item scaling: reference index of %u items.", count);
        });
        return index;
    }

    // Median item level of an item of this kind for a character of this level; 0 if none.
    uint32 TargetItemLevel(ItemTemplate const* proto, uint32 level)
    {
        ReferenceIndex const& index = Index();
        std::map<uint64, ReferenceIndex::ByLevel> const* stages[] = { &index.exact, &index.subclass, &index.klass };
        uint64 const keys[] = { Key(proto->Class, proto->SubClass, proto->InventoryType, proto->Quality),
                                Key(proto->Class, proto->SubClass, 0, proto->Quality),
                                Key(proto->Class, 0, 0, proto->Quality) };
        for (uint32 stage = 0; stage < 3; ++stage)
        {
            auto bucket = stages[stage]->find(keys[stage]);
            if (bucket == stages[stage]->end())
                continue;
            for (uint32 radius = 0; radius <= 10; ++radius)
            {
                std::vector<uint32> found;
                for (int32 level2 : { int32(level) - int32(radius), int32(level) + int32(radius) })
                {
                    if (level2 < 1 || (radius == 0 && level2 != int32(level)) || (radius && level2 == int32(level)))
                        continue;
                    auto it = bucket->second.find(uint32(level2));
                    if (it != bucket->second.end())
                        found.insert(found.end(), it->second.begin(), it->second.end());
                    if (radius == 0)
                        break;
                }
                if (found.empty())
                    continue;
                std::sort(found.begin(), found.end());
                return found[found.size() / 2];
            }
        }
        return 0;
    }

    // ---- Per-item persistence ---------------------------------------------------------

    std::shared_mutex g_shiftLock;
    std::unordered_map<uint32, int32> g_shifts;      // item guid (low) -> shift

    int32 StoredShift(uint32 itemGuid)
    {
        std::shared_lock<std::shared_mutex> guard(g_shiftLock);
        auto it = g_shifts.find(itemGuid);
        return it != g_shifts.end() ? it->second : 0;
    }

    // ---- Hooks the core calls ---------------------------------------------------------

    ItemTemplate const* StatTemplateFor(Item const* item)
    {
        int32 const shift = ShiftOf(item->GetUpgradeId());
        return shift > 0 ? ShiftedTemplate(item->GetTemplate(), shift) : nullptr;
    }

    void ItemLoaded(Item* item)
    {
        if (int32 const shift = StoredShift(item->GetGUIDLow()))
            if (!item->GetUpgradeId())
                item->SetUpgradeId(UpgradeIdFor(shift));
    }

    void UpgradeHotfixList(std::vector<std::pair<uint32, uint32>>& rows)
    {
        rows.reserve(rows.size() + MAX_SHIFT);
        for (int32 shift = 1; shift <= MAX_SHIFT; ++shift)
            rows.emplace_back(UpgradeIdFor(shift), HOTFIX_TIMESTAMP);
    }

    bool UpgradeRowWriter(uint32 upgradeId, ByteBuffer& record, uint32& timestamp)
    {
        if (!IsModuleUpgradeId(upgradeId))
            return false;
        record << uint32(upgradeId);
        record << uint32(UPGRADE_PATH);
        record << int32(ShiftOf(upgradeId));            // item level bonus
        record << uint32(0);                            // previous upgrade
        record << uint32(0);                            // currency
        record << uint32(0);                            // cost
        timestamp = HOTFIX_TIMESTAMP;
        return true;
    }

    // ---- Stamping -----------------------------------------------------------------------

    // Content where the world scales for this character (as creature views): not raids,
    // scenarios, battlegrounds, arenas, nor heroic dungeons.
    bool ScaledContent(Player* player)
    {
        Map const* map = player->GetMap();
        if (!map || map->IsBattlegroundOrArena() || map->IsRaid() || map->IsScenario())
            return false;
        return !map->IsDungeon() || (DestinyWeaver::GetConfig().dungeons && map->IsRegularDifficulty());
    }

    void Stamp(Player* player, Item* item, uint32 targetLevel, char const* source)
    {
        ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
        if (!proto || item->GetUpgradeId() || proto->ScalingStatDistribution)
            return;
        if (proto->Class != ITEM_CLASS_WEAPON && proto->Class != ITEM_CLASS_ARMOR)
            return;
        if (Budget(proto, proto->ItemLevel) <= 0.0f)
            return;                                     // poor/common or no budget: client would not scale it

        uint32 const target = TargetItemLevel(proto, targetLevel);
        int32 const shift = std::min<int32>(int32(target) - int32(proto->ItemLevel), MAX_SHIFT);
        if (shift <= 0)
            return;                                     // upgrades only

        bool const equipped = item->IsEquipped() && !item->IsBroken();
        if (equipped)
            player->_ApplyItemMods(item, item->GetSlot(), false);
        item->SetUpgradeId(UpgradeIdFor(shift));
        if (equipped)
            player->_ApplyItemMods(item, item->GetSlot(), true);

        {
            std::unique_lock<std::shared_mutex> guard(g_shiftLock);
            g_shifts[item->GetGUIDLow()] = shift;
        }
        CharacterDatabase.PExecute("REPLACE INTO character_destiny_weaver_items (item_guid, shift) VALUES (%u, %d)",
            item->GetGUIDLow(), shift);

        SF_LOG_INFO("modules", "[mod-destiny-weaver] %s's %s (%s) raised from item level %u to %u for level %u.",
            player->GetName().c_str(), proto->Name1.c_str(), source, proto->ItemLevel, proto->ItemLevel + shift,
            targetLevel);
    }

    bool ItemScalingFor(Player* player)
    {
        return g_itemScaling && player && player->GetSession() && !player->GetSession()->IsBot()
            && DestinyWeaver::LevelScalingEnabled(player);
    }
}

namespace DestinyWeaver
{
    void ApplyItemConfig()
    {
        Config const& config = GetConfig();
        g_itemScaling = config.enabled && config.levelScaling && sConfigMgr->GetBoolDefault("DestinyWeaver.ItemScaling", true);
        g_questRewards = sConfigMgr->GetBoolDefault("DestinyWeaver.ItemScaling.QuestRewards", true);

        // Raised items keep their stats as long as the module is enabled, even if a
        // character turns scaling off later.
        bool const keep = config.enabled;
        ItemScalingHooks::StatTemplate.store(keep ? &StatTemplateFor : nullptr);
        ItemScalingHooks::ItemLoaded.store(keep ? &ItemLoaded : nullptr);
        ItemScalingHooks::UpgradeHotfixList.store(keep ? &UpgradeHotfixList : nullptr);
        ItemScalingHooks::UpgradeRowWriter.store(keep ? &UpgradeRowWriter : nullptr);
    }

    void LoadItemShifts()
    {
        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS `character_destiny_weaver_items` ("
            "`item_guid` INT UNSIGNED NOT NULL, "
            "`shift` SMALLINT NOT NULL COMMENT 'item levels added (upgrade modifier)', "
            "PRIMARY KEY (`item_guid`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-destiny-weaver raised items'");
        // Items deleted since the last start.
        CharacterDatabase.DirectExecute("DELETE FROM character_destiny_weaver_items "
            "WHERE item_guid NOT IN (SELECT guid FROM item_instance)");

        std::unique_lock<std::shared_mutex> guard(g_shiftLock);
        g_shifts.clear();
        if (QueryResult result = CharacterDatabase.Query("SELECT item_guid, shift FROM character_destiny_weaver_items"))
        {
            do
            {
                Field* fields = result->Fetch();
                g_shifts[fields[0].GetUInt32()] = int32(fields[1].GetInt16());
            } while (result->NextRow());
        }
        SF_LOG_INFO("modules", "[mod-destiny-weaver] Item scaling: %u raised item(s) loaded.", uint32(g_shifts.size()));
    }

    bool ItemScalingActive()
    {
        return g_itemScaling;
    }
}

class destiny_weaver_items_playerscript : public PlayerScript
{
public:
    destiny_weaver_items_playerscript() : PlayerScript("destiny_weaver_items_playerscript") { }

    // Loot where the world scales comes up to what the character's creatures are at.
    void OnLootItem(Player* player, Item* item, uint32 /*count*/) override
    {
        if (!ItemScalingFor(player) || !ScaledContent(player))
            return;
        uint8 const offset = DestinyWeaver::GetConfig().offset;
        uint32 const level = player->getLevel() > offset ? player->getLevel() - offset : 1;
        Stamp(player, item, level, "loot");
    }

    // A scaled quest pays at the character's level; so do its item rewards.
    void OnQuestRewardItem(Player* player, Item* item, Quest const* quest) override
    {
        if (!g_questRewards || !ItemScalingFor(player) || !LocalLevelScaling::QuestScalingEnabled(player))
            return;
        if (quest->GetQuestLevel() > 0 && quest->GetQuestLevel() >= int32(player->getLevel()))
            return;
        Stamp(player, item, player->getLevel(), "quest reward");
    }
};

void AddSC_destiny_weaver_items()
{
    new destiny_weaver_items_playerscript();
}
