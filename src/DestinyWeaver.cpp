/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*
* Destiny Weaver: the per-character choices, their storage, the `.weaver` command,
* experience bonus control and the group rules. The creature view itself is in
* DestinyWeaverScaling.cpp.
*/

#include "DestinyWeaver.h"

#include "Chat.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Group.h"
#include "GroupReference.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "RBAC.h"
#include "ScriptMgr.h"
#include "SpellAuraEffects.h"
#include "WorldSession.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace DestinyWeaver
{
    void ApplyScalingConfig();   // DestinyWeaverScaling.cpp
}

namespace
{
    DestinyWeaver::Config g_config;

    // Stored per character in `character_destiny_weaver`; cached while the character is online,
    // so the cache also says who is online (the group leader's switch only counts while they are).
    struct Choice
    {
        int8 scaling = -1;          // -1 never chose, 0 off, 1 on
        bool noXpBonus = false;
        bool bot = false;           // playerbots session: no stored choice of its own
    };

    std::shared_mutex g_choiceLock;
    std::unordered_map<uint32, Choice> g_choices;

    std::mutex g_refreshLock;
    std::unordered_set<uint64> g_refresh;

    std::string ConfigPath()
    {
        std::string mainConfig = sConfigMgr->GetFilename();
        std::string::size_type slash = mainConfig.find_last_of("/\\");
        std::string directory = slash != std::string::npos ? mainConfig.substr(0, slash + 1) : "";
        return directory + "destiny_weaver.conf";
    }

    bool LookupChoice(uint32 lowGuid, Choice& choice)
    {
        std::shared_lock<std::shared_mutex> guard(g_choiceLock);
        auto it = g_choices.find(lowGuid);
        if (it == g_choices.end())
            return false;
        choice = it->second;
        return true;
    }

    bool PersonalFromChoice(Choice const& choice)
    {
        if (choice.bot)
            return g_config.ungroupedBots;
        return choice.scaling < 0 ? g_config.scalingDefault : choice.scaling == 1;
    }

    void SaveChoice(Player* player)
    {
        Choice choice;
        if (!LookupChoice(player->GetGUIDLow(), choice) || choice.bot)
            return;
        CharacterDatabase.PExecute("REPLACE INTO character_destiny_weaver (guid, level_scaling, xp_bonus_off) "
            "VALUES (%u, %d, %u)", player->GetGUIDLow(), int32(choice.scaling), choice.noXpBonus ? 1u : 0u);
    }

    void Tell(Player* player, std::string const& line, bool centered)
    {
        if (!player || !player->GetSession() || player->GetSession()->IsBot())
            return;
        ChatHandler(player->GetSession()).SendSysMessage(line.c_str());
        if (centered)
            player->GetSession()->SendNotification("%s", line.c_str());
    }

    void TellScaling(Player* player, bool grouped)
    {
        bool const enabled = DestinyWeaver::LevelScalingEnabled(player);
        std::string const state = enabled ? "|cff00ff00enabled|r" : "|cffff2020disabled|r";
        if (grouped)
            Tell(player, "|cffffff00Your group has open world scaling " + state, true);
        else
            Tell(player, "|cffffff00You have " + state + "|cffffff00 open world creature scaling.|r", true);
    }

    // Bonus experience from auras (heirlooms, guild perk, potions): the core multiplied
    // the award by each aura's percentage before GiveXP, so divide it back out.
    uint32 StripAuraBonus(Player* player, uint32 amount, AuraType type)
    {
        float factor = 1.0f;
        for (AuraEffect const* effect : player->GetAuraEffectsByType(type))
            if (effect->GetAmount() > 0)
                factor *= 1.0f + float(effect->GetAmount()) / 100.0f;
        return factor > 1.0f ? std::max<uint32>(1, uint32(float(amount) / factor)) : amount;
    }
}

namespace DestinyWeaver
{
    Config const& GetConfig()
    {
        return g_config;
    }

    void LoadConfig(bool reload)
    {
        std::string const path = ConfigPath();
        if (!sConfigMgr->LoadMore(path.c_str()))
            SF_LOG_INFO("modules", "[mod-destiny-weaver] No '%s'; the module stays disabled.", path.c_str());

        g_config.enabled = sConfigMgr->GetBoolDefault("DestinyWeaver.Enable", false);
        g_config.levelScaling = sConfigMgr->GetBoolDefault("DestinyWeaver.LevelScaling", true);
        g_config.scalingDefault = sConfigMgr->GetBoolDefault("DestinyWeaver.LevelScaling.Default", true);
        g_config.dungeons = sConfigMgr->GetBoolDefault("DestinyWeaver.Scaling.Dungeons", true);
        g_config.offset = uint8(std::min(std::max(sConfigMgr->GetIntDefault("DestinyWeaver.Scaling.Offset", 3), 0), 20));
        g_config.ungroupedBots = sConfigMgr->GetBoolDefault("DestinyWeaver.Scaling.UngroupedBots", false);
        g_config.experienceBonusControl = sConfigMgr->GetBoolDefault("DestinyWeaver.ExperienceBonusControl", true);

        ApplyScalingConfig();

        SF_LOG_INFO("modules", "[mod-destiny-weaver] %s: module %s, open world scaling %s (offset %u, dungeons %s, "
            "default %s), experience bonus control %s.", reload ? "Reloaded" : "Loaded",
            g_config.enabled ? "enabled" : "disabled",
            g_config.enabled && g_config.levelScaling ? "on" : "off", uint32(g_config.offset),
            g_config.dungeons ? "on" : "off", g_config.scalingDefault ? "on" : "off",
            g_config.enabled && g_config.experienceBonusControl ? "on" : "off");
    }

    bool PersonalLevelScaling(Player const* player)
    {
        if (!player || !g_config.enabled || !g_config.levelScaling)
            return false;
        Choice choice;
        if (!LookupChoice(player->GetGUIDLow(), choice))
            return g_config.scalingDefault;
        return PersonalFromChoice(choice);
    }

    bool LevelScalingEnabled(Player const* player)
    {
        if (!player || !g_config.enabled || !g_config.levelScaling)
            return false;

        // The leader decides whether the group scales; each member keeps their own level.
        if (Group const* group = player->GetGroup())
        {
            uint64 const leader = group->GetLeaderGUID();
            if (leader && leader != player->GetGUID())
            {
                Choice choice;
                if (LookupChoice(GUID_LOPART(leader), choice))
                    return PersonalFromChoice(choice);
            }
        }
        return PersonalLevelScaling(player);
    }

    bool ExperienceBonusControlEnabled(Player const* player)
    {
        if (!player || !g_config.enabled || !g_config.experienceBonusControl)
            return false;
        Choice choice;
        return LookupChoice(player->GetGUIDLow(), choice) && choice.noXpBonus;
    }

    void SetLevelScaling(Player* player, bool enabled)
    {
        {
            std::unique_lock<std::shared_mutex> guard(g_choiceLock);
            g_choices[player->GetGUIDLow()].scaling = enabled ? 1 : 0;
        }
        SaveChoice(player);

        Group* group = player->GetGroup();
        bool const follower = group && group->GetLeaderGUID() != player->GetGUID();
        if (follower)
            Tell(player, std::string("|cffffff00Saved: open world scaling ") + (enabled ? "on" : "off")
                + " when you play alone. Your group follows its leader's switch.|r", false);
        else
            TellScaling(player, false);

        if (group && !follower)
            RefreshGroup(group);
        else
            RefreshClient(player);
    }

    void SetExperienceBonusControl(Player* player, bool enabled)
    {
        {
            std::unique_lock<std::shared_mutex> guard(g_choiceLock);
            g_choices[player->GetGUIDLow()].noXpBonus = enabled;
        }
        SaveChoice(player);
        Tell(player, std::string("|cffffff00Experience bonuses ") + (enabled ? "|cffff2020disabled|r" : "|cff00ff00enabled|r")
            + "|cffffff00 (heirlooms, guild perk and other experience auras).|r", false);
    }

    void RefreshClient(Player* player)
    {
        if (!player)
            return;
        std::lock_guard<std::mutex> guard(g_refreshLock);
        g_refresh.insert(player->GetGUID());
    }

    void RefreshGroup(Group* group)
    {
        if (!group)
            return;
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
            if (Player* member = itr->GetSource())
                RefreshClient(member);
    }

    void ForgetRefresh(uint64 guid)
    {
        std::lock_guard<std::mutex> guard(g_refreshLock);
        g_refresh.erase(guid);
    }

    bool TakeRefresh(Player* player)
    {
        std::lock_guard<std::mutex> guard(g_refreshLock);
        return g_refresh.erase(player->GetGUID()) != 0;
    }
}

namespace DestinyWeaver
{
    void ResendNearbyCreatures(Player* player);   // DestinyWeaverScaling.cpp

    void ServeRefresh(Player* player)
    {
        if (!player || !player->IsInWorld() || !TakeRefresh(player))
            return;
        ResendNearbyCreatures(player);
        player->UpdateForQuestWorldObjects();
        // The client caches each quest's level from its query response: re-send them so the
        // quest log shows the level the quests now play at for this character.
        if (player->GetSession() && !player->GetSession()->IsBot())
            player->RefreshQuestLogQueries();
    }
}

class destiny_weaver_commandscript : public CommandScript
{
public:
    destiny_weaver_commandscript() : CommandScript("destiny_weaver_commandscript") { }

    // Player-level commands: same permission as `.save`, which every player role has.
    std::vector<ChatCommand> GetCommands() const override
    {
        static std::vector<ChatCommand> weaverTable =
        {
            { "scaling", rbac::RBAC_PERM_COMMAND_SAVE, false, &HandleScaling, "" },
            { "xpbonus", rbac::RBAC_PERM_COMMAND_SAVE, false, &HandleXpBonus, "" },
            { "",        rbac::RBAC_PERM_COMMAND_SAVE, false, &HandleStatus,  "" },
        };
        static std::vector<ChatCommand> commandTable =
        {
            { "weaver", rbac::RBAC_PERM_COMMAND_SAVE, false, nullptr, "", weaverTable },
        };
        return commandTable;
    }

    static Player* Self(ChatHandler* handler)
    {
        if (!DestinyWeaver::GetConfig().enabled)
        {
            handler->SendSysMessage("Destiny Weaver is disabled on this realm.");
            handler->SetSentErrorMessage(true);
            return nullptr;
        }
        WorldSession* session = handler->GetSession();
        return session ? session->GetPlayer() : nullptr;
    }

    // on / off / nothing (toggle). Returns -1 for anything else.
    static int ParseSwitch(char const* args, bool current)
    {
        std::string word = args ? args : "";
        word.erase(0, word.find_first_not_of(' '));
        word.erase(word.find_last_not_of(' ') + 1);
        for (char& c : word)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        if (word.empty())
            return current ? 0 : 1;
        if (word == "on" || word == "1" || word == "enable")
            return 1;
        if (word == "off" || word == "0" || word == "disable")
            return 0;
        return -1;
    }

    static bool HandleStatus(ChatHandler* handler, char const* /*args*/)
    {
        Player* player = Self(handler);
        if (!player)
            return false;
        DestinyWeaver::Config const& config = DestinyWeaver::GetConfig();

        if (!config.levelScaling)
            handler->SendSysMessage("Open world scaling: not available on this realm.");
        else
        {
            handler->PSendSysMessage("Open world scaling: %s (your choice: %s).",
                DestinyWeaver::LevelScalingEnabled(player) ? "|cff00ff00on|r" : "|cffff2020off|r",
                DestinyWeaver::PersonalLevelScaling(player) ? "on" : "off");
            Group* group = player->GetGroup();
            if (group && group->GetLeaderGUID() != player->GetGUID())
                handler->SendSysMessage("You are grouped: the group leader's switch decides; creatures still scale to your own level.");
            handler->PSendSysMessage("Creatures are lifted to your level - %u.", uint32(config.offset));
        }
        if (config.experienceBonusControl)
            handler->PSendSysMessage("Experience bonuses: %s.",
                DestinyWeaver::ExperienceBonusControlEnabled(player) ? "|cffff2020off|r" : "|cff00ff00on|r");
        handler->SendSysMessage("Usage: .weaver scaling [on|off], .weaver xpbonus [on|off]");
        return true;
    }

    static bool HandleScaling(ChatHandler* handler, char const* args)
    {
        Player* player = Self(handler);
        if (!player)
            return false;
        if (!DestinyWeaver::GetConfig().levelScaling)
        {
            handler->SendSysMessage("Open world scaling is not available on this realm.");
            handler->SetSentErrorMessage(true);
            return false;
        }
        int const value = ParseSwitch(args, DestinyWeaver::PersonalLevelScaling(player));
        if (value < 0)
        {
            handler->SendSysMessage("Usage: .weaver scaling [on|off]");
            handler->SetSentErrorMessage(true);
            return false;
        }
        DestinyWeaver::SetLevelScaling(player, value == 1);
        return true;
    }

    static bool HandleXpBonus(ChatHandler* handler, char const* args)
    {
        Player* player = Self(handler);
        if (!player)
            return false;
        if (!DestinyWeaver::GetConfig().experienceBonusControl)
        {
            handler->SendSysMessage("Experience bonus control is not available on this realm.");
            handler->SetSentErrorMessage(true);
            return false;
        }
        // "xpbonus on" = receive bonuses = control off.
        int const value = ParseSwitch(args, !DestinyWeaver::ExperienceBonusControlEnabled(player));
        if (value < 0)
        {
            handler->SendSysMessage("Usage: .weaver xpbonus [on|off]");
            handler->SetSentErrorMessage(true);
            return false;
        }
        DestinyWeaver::SetExperienceBonusControl(player, value == 0);
        return true;
    }
};

class destiny_weaver_playerscript : public PlayerScript
{
public:
    destiny_weaver_playerscript() : PlayerScript("destiny_weaver_playerscript") { }

    void OnLogin(Player* player, bool /*firstLogin*/) override
    {
        Choice choice;
        choice.bot = player->GetSession() && player->GetSession()->IsBot();
        if (!choice.bot)
            if (QueryResult result = CharacterDatabase.PQuery(
                "SELECT level_scaling, xp_bonus_off FROM character_destiny_weaver WHERE guid = %u", player->GetGUIDLow()))
            {
                Field* fields = result->Fetch();
                choice.scaling = fields[0].GetInt8();
                choice.noXpBonus = fields[1].GetUInt8() != 0;
            }
        {
            std::unique_lock<std::shared_mutex> guard(g_choiceLock);
            g_choices[player->GetGUIDLow()] = choice;
        }

        // The client may still hold quest levels cached on another character: send this one's.
        if (DestinyWeaver::GetConfig().enabled && DestinyWeaver::GetConfig().levelScaling && !choice.bot)
            DestinyWeaver::RefreshClient(player);

        // A leader coming online brings their switch to the group.
        if (Group* group = player->GetGroup())
            if (group->GetLeaderGUID() == player->GetGUID())
                DestinyWeaver::RefreshGroup(group);
    }

    void OnLogout(Player* player) override
    {
        {
            std::unique_lock<std::shared_mutex> guard(g_choiceLock);
            g_choices.erase(player->GetGUIDLow());
        }
        DestinyWeaver::ForgetRefresh(player->GetGUID());

        // Members go back to their own choice while the leader is away.
        if (Group* group = player->GetGroup())
            if (group->GetLeaderGUID() == player->GetGUID())
                DestinyWeaver::RefreshGroup(group);
    }

    void OnUpdate(Player* player, uint32 /*diff*/) override
    {
        DestinyWeaver::ServeRefresh(player);
    }

    void OnLevelChanged(Player* player, uint8 /*oldLevel*/) override
    {
        if (DestinyWeaver::LevelScalingEnabled(player))
            DestinyWeaver::RefreshClient(player);
    }

    void OnGiveXP(Player* player, uint32& amount, Unit* victim) override
    {
        if (!amount || !DestinyWeaver::ExperienceBonusControlEnabled(player))
            return;
        // Kills carry a victim; quest rewards do not.
        amount = StripAuraBonus(player, amount, victim ? SPELL_AURA_MOD_XP_PCT : SPELL_AURA_MOD_XP_QUEST_PCT);
    }
};

class destiny_weaver_groupscript : public GroupScript
{
public:
    destiny_weaver_groupscript() : GroupScript("destiny_weaver_groupscript") { }

    static void Changed(Player* member, bool grouped)
    {
        if (!member)
            return;
        DestinyWeaver::RefreshClient(member);
        if (DestinyWeaver::GetConfig().enabled && DestinyWeaver::GetConfig().levelScaling)
            TellScaling(member, grouped);
    }

    void OnAddMember(Group* group, uint64 guid) override
    {
        if (group && group->GetLeaderGUID() != guid)
            Changed(ObjectAccessor::FindPlayer(guid), true);
    }

    void OnRemoveMember(Group* group, uint64 guid, RemoveMethod /*method*/, uint64 /*kicker*/, char const* /*reason*/) override
    {
        if (group && group->GetLeaderGUID() == guid)
            DestinyWeaver::RefreshGroup(group);
        Changed(ObjectAccessor::FindPlayer(guid), false);
    }

    void OnChangeLeader(Group* group, uint64 /*newLeaderGuid*/, uint64 /*oldLeaderGuid*/) override
    {
        DestinyWeaver::RefreshGroup(group);
    }

    void OnDisband(Group* group) override
    {
        DestinyWeaver::RefreshGroup(group);
    }
};

class destiny_weaver_worldscript : public WorldScript
{
public:
    destiny_weaver_worldscript() : WorldScript("destiny_weaver_worldscript") { }

    void OnStartup() override
    {
        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS `character_destiny_weaver` ("
            "`guid` INT UNSIGNED NOT NULL, "
            "`level_scaling` TINYINT NOT NULL DEFAULT -1 COMMENT '-1 never chose, 0 off, 1 on', "
            "`xp_bonus_off` TINYINT UNSIGNED NOT NULL DEFAULT 0, "
            "PRIMARY KEY (`guid`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-destiny-weaver choices'");
        DestinyWeaver::LoadConfig(false);
    }

    // `.reload config` re-reads worldserver.conf only; merge this module's file again.
    void OnConfigLoad(bool reload) override
    {
        if (reload)
            DestinyWeaver::LoadConfig(true);
    }
};

void AddSC_destiny_weaver()
{
    new destiny_weaver_commandscript();
    new destiny_weaver_playerscript();
    new destiny_weaver_groupscript();
    new destiny_weaver_worldscript();
}
