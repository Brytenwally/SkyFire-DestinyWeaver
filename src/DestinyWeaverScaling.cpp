/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*
* Open world scaling, per character.
*
* A creature keeps its authored level and stats for everyone. A character with scaling on
* is shown their own version of it, built from `creature_classlevelstats` at their level
* (minus the offset): level, health and mana are rewritten in the values update sent to
* that character alone (Unit::BuildValuesUpdate asks LocalLevelScaling), and their fight
* follows that version:
*
*   - the level hit, resist, glancing and kill experience are rolled at
*     (Creature::getLevelForTarget asks this module),
*   - the armor their blows meet (Unit::CalcArmorReducedDamage asks this module),
*   - their damage is scaled onto the creature's real health pool, so their health bar
*     falls by what they are shown,
*   - the creature's melee, spell and periodic damage to them is scaled to what its
*     version hits for.
*
* Everybody else keeps seeing and fighting the authored creature.
*/

#include "DestinyWeaver.h"

#include "Config.h"
#include "Creature.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "CellImpl.h"
#include "LocalLevelScaling.h"
#include "Log.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Util.h"

#include <algorithm>
#include <atomic>
#include <list>

namespace
{
    std::atomic<bool> g_active{ false };

    // How far around a character creatures are re-sent after their scaling changed.
    constexpr float REFRESH_RANGE = 120.0f;

    struct LevelStats
    {
        uint32 health;
        uint32 mana;
        uint32 armor;
        double hit;     // average melee hit before template and aura modifiers
    };

    bool StatsAt(uint8 level, CreatureTemplate const* info, LevelStats& out)
    {
        CreatureBaseStats const* stats = CreatureBaseStats::GetBaseStats(level, info->unit_class);
        if (!stats)
            return false;
        out.health = stats->GenerateHealth(info);
        // GetCreatureBaseStats answers a dummy row (health 1) for a missing level.
        if (out.health <= 1)
            return false;
        out.mana = stats->GenerateMana(info);
        out.armor = stats->GenerateArmor(info);
        float base = stats->BaseDamage[info->expansion < MAX_CREATURE_BASE_DAMAGE ? info->expansion : 0];
        if (!base)
            base = stats->BaseDamage[0];
        // Creature::SelectLevel: weapon base .. base * 1.5, plus attack power / 14 per second of swing.
        double const swing = double(std::max<uint32>(info->baseattacktime, 1000)) / 1000.0;
        out.hit = double(base) * 1.25 + double(stats->AttackPower) / 14.0 * swing;
        return true;
    }

    struct CreatureView
    {
        uint8 level;
        LevelStats own;
        LevelStats stats;
        uint32 maxHealth;           // the pool the viewer is shown
        double damageToPool;        // viewer's damage -> real pool
        double damageTaken;         // creature's damage -> viewer's version of it
    };

    bool ViewableCreature(Creature const* creature)
    {
        Map const* map = creature->GetMap();
        if (!map || map->IsBattlegroundOrArena() || map->IsRaid() || map->IsScenario())
            return false;
        if (map->IsDungeon() && !DestinyWeaver::ScaledDungeon(map))
            return false;

        // Somebody's pet, guardian, totem or a creature's summon keeps its level; so does scenery.
        if (creature->IsPet() || creature->IsTotem() || creature->IsTrigger() || creature->isWorldBoss()
            || creature->GetCharmerOrOwnerGUID())
            return false;
        uint32 const type = creature->GetCreatureType();
        return type != CREATURE_TYPE_CRITTER && type != CREATURE_TYPE_NON_COMBAT_PET;
    }

    bool ViewFor(Creature const* creature, Player const* viewer, CreatureView& view)
    {
        if (!g_active.load(std::memory_order_relaxed) || !creature || !viewer || !creature->IsInWorld())
            return false;
        if (!DestinyWeaver::LevelScalingEnabled(viewer) || !ViewableCreature(creature))
            return false;
        // Vendors, trainers, guards and other friendly creatures keep their level.
        if (viewer->GetReactionTo(creature) > REP_NEUTRAL)
            return false;

        Map const* map = creature->GetMap();
        uint8 const own = creature->getLevel();
        uint8 const offset = DestinyWeaver::GetConfig().offset;
        uint8 const viewerLevel = viewer->getLevel();
        uint8 const floor = viewerLevel > offset ? uint8(viewerLevel - offset) : uint8(1);
        uint8 level = std::max(own, floor);
        // A normal dungeon also comes down to a group that entered it low.
        if (map->IsDungeon())
            level = uint8(std::min<uint32>(level, uint32(viewerLevel) + offset));
        if (level == own)
            return false;

        CreatureTemplate const* info = creature->GetCreatureTemplate();
        if (!info || !StatsAt(own, info, view.own) || !StatsAt(level, info, view.stats))
            return false;

        uint32 const realMax = std::max<uint32>(creature->GetMaxHealth(), 1);
        view.level = level;
        view.maxHealth = std::max<uint32>(1, uint32(uint64(realMax) * view.stats.health / view.own.health));
        view.damageToPool = double(realMax) / double(view.maxHealth);
        view.damageTaken = view.own.hit > 0.0 ? view.stats.hit / view.own.hit : 1.0;
        return true;
    }

    uint8 ResolveLevel(Player const* viewer, Creature const* creature)
    {
        CreatureView view;
        return ViewFor(creature, viewer, view) ? view.level : 0;
    }

    bool ResolveArmor(Player const* viewer, Creature const* creature, float& armor)
    {
        CreatureView view;
        if (!ViewFor(creature, viewer, view) || !view.own.armor)
            return false;
        armor = std::max(0.0f, armor * float(view.stats.armor) / float(view.own.armor));
        return true;
    }

    uint32 Scale(uint32 value, uint32 to, uint32 from)
    {
        return from ? uint32(uint64(value) * to / from) : value;
    }

    bool ResolveField(Player const* viewer, Creature const* creature, uint16 index, uint32& value)
    {
        CreatureView view;
        if (!ViewFor(creature, viewer, view))
            return false;

        if (index == UNIT_FIELD_LEVEL)
            value = view.level;
        else if (index == UNIT_FIELD_MAX_HEALTH)
            value = view.maxHealth;
        else if (index == UNIT_FIELD_HEALTH)
            value = value ? std::max<uint32>(1, Scale(value, view.maxHealth, std::max<uint32>(creature->GetMaxHealth(), 1))) : 0;
        else
        {
            uint32 const manaIndex = creature->GetPowerIndex(POWER_MANA);
            if (manaIndex >= MAX_POWERS_PER_CLASS || !view.own.mana)
                return false;
            if (index == UNIT_FIELD_MAX_POWER + manaIndex || index == UNIT_FIELD_POWER + manaIndex)
                value = Scale(value, view.stats.mana, view.own.mana);
            else
                return false;
        }
        return true;
    }

    bool ResolveQuestScaling(Player const* player)
    {
        return g_active.load(std::memory_order_relaxed) && DestinyWeaver::LevelScalingEnabled(player);
    }

    Player const* OwningPlayer(Unit const* unit)
    {
        return unit ? unit->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
    }

    // Keeps fractional damage on average without per-creature state.
    uint32 ScaleDamage(uint32 damage, double factor)
    {
        double const scaled = double(damage) * factor;
        uint32 whole = uint32(scaled);
        if (rand_norm() < scaled - double(whole))
            ++whole;
        return whole;
    }
}

namespace DestinyWeaver
{
    void ApplyScalingConfig()
    {
        Config const& config = GetConfig();
        bool const active = config.enabled && config.levelScaling;
        g_active.store(active, std::memory_order_relaxed);

        LocalLevelScaling::ViewLevelOwner.store(active ? &ResolveLevel : nullptr);
        LocalLevelScaling::ViewArmorOwner.store(active ? &ResolveArmor : nullptr);
        LocalLevelScaling::ViewFieldOwner.store(active ? &ResolveField : nullptr);
        LocalLevelScaling::QuestScalingOwner.store(active ? &ResolveQuestScaling : nullptr);

        auto share = [](char const* key, int fallback)
        {
            return uint32(std::min(std::max(sConfigMgr->GetIntDefault(key, fallback), 0), 100));
        };
        LocalLevelScaling::QuestMoneyKeepSharePercent.store(share("DestinyWeaver.Scaling.QuestMoneyKeepShare", 60));
        LocalLevelScaling::QuestXpKeepSharePercent.store(share("DestinyWeaver.Scaling.QuestXpKeepShare", 100));
    }

    // Marks the level, health and mana of the creatures around a character as changed: the next
    // values update re-sends them, and each recipient gets their own version of them.
    void ResendNearbyCreatures(Player* player)
    {
        std::list<Unit*> units;
        Skyfire::AnyUnitInObjectRangeCheck check(player, REFRESH_RANGE);
        Skyfire::UnitListSearcher<Skyfire::AnyUnitInObjectRangeCheck> searcher(player, units, check);
        player->VisitNearbyObject(REFRESH_RANGE, searcher);

        for (Unit* unit : units)
        {
            Creature* creature = unit->ToCreature();
            if (!creature)
                continue;
            creature->ForceValuesUpdateAtIndex(UNIT_FIELD_LEVEL);
            creature->ForceValuesUpdateAtIndex(UNIT_FIELD_HEALTH);
            creature->ForceValuesUpdateAtIndex(UNIT_FIELD_MAX_HEALTH);
            uint32 const manaIndex = creature->GetPowerIndex(POWER_MANA);
            if (manaIndex < MAX_POWERS_PER_CLASS)
            {
                creature->ForceValuesUpdateAtIndex(UNIT_FIELD_POWER + manaIndex);
                creature->ForceValuesUpdateAtIndex(UNIT_FIELD_MAX_POWER + manaIndex);
            }
        }
    }
}

class destiny_weaver_unitscript : public UnitScript
{
public:
    destiny_weaver_unitscript() : UnitScript("destiny_weaver_unitscript") { }

    // A character's damage to their version of a creature, onto the real pool. Runs in
    // DealDamage, after the client was shown the number.
    void OnDamage(Unit* attacker, Unit* victim, uint32& damage) override
    {
        if (!damage || !g_active.load(std::memory_order_relaxed))
            return;
        Creature* creature = victim ? victim->ToCreature() : nullptr;
        Player const* player = OwningPlayer(attacker);
        if (!creature || !player || !creature->IsAlive())
            return;

        CreatureView view;
        if (ViewFor(creature, player, view))
            damage = ScaleDamage(damage, view.damageToPool);
    }

    void ModifyMeleeDamage(Unit* target, Unit* attacker, uint32& damage) override
    {
        ScaleTaken(target, attacker, damage);
    }

    void ModifySpellDamageTaken(Unit* target, Unit* attacker, int32& damage) override
    {
        if (damage <= 0)
            return;
        uint32 value = uint32(damage);
        ScaleTaken(target, attacker, value);
        damage = int32(value);
    }

    void ModifyPeriodicDamageAurasTick(Unit* target, Unit* attacker, uint32& damage) override
    {
        ScaleTaken(target, attacker, damage);
    }

private:
    // A creature's damage to a character (or their pet), as its version would hit.
    static void ScaleTaken(Unit* target, Unit* attacker, uint32& damage)
    {
        if (!damage || !g_active.load(std::memory_order_relaxed))
            return;
        Creature* creature = attacker ? attacker->ToCreature() : nullptr;
        Player const* player = OwningPlayer(target);
        if (!creature || !player)
            return;

        CreatureView view;
        if (ViewFor(creature, player, view))
            damage = std::max<uint32>(1, ScaleDamage(damage, view.damageTaken));
    }
};

void AddSC_destiny_weaver_scaling()
{
    new destiny_weaver_unitscript();
}
