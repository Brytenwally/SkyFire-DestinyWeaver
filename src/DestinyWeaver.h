/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*
* Destiny Weaver: per-character open world scaling and experience bonus control,
* ported from the AzerothCore CoA module for SkyFire 5.4.8.
*/

#ifndef SF_DESTINY_WEAVER_H
#define SF_DESTINY_WEAVER_H

#include "Define.h"

class Group;
class Player;

namespace DestinyWeaver
{
    struct Config
    {
        bool enabled = false;          // DestinyWeaver.Enable
        bool levelScaling = true;      // DestinyWeaver.LevelScaling
        bool scalingDefault = true;    // DestinyWeaver.LevelScaling.Default
        bool dungeons = true;          // DestinyWeaver.Scaling.Dungeons
        uint8 offset = 3;              // DestinyWeaver.Scaling.Offset
        bool ungroupedBots = false;    // DestinyWeaver.Scaling.UngroupedBots
        bool experienceBonusControl = true; // DestinyWeaver.ExperienceBonusControl
    };

    Config const& GetConfig();
    void LoadConfig(bool reload);

    // Open world scaling for this character: their own choice, or while grouped the leader's
    // switch (the level is always the character's own).
    bool LevelScalingEnabled(Player const* player);
    // The character's own stored choice (or the default), ignoring the group.
    bool PersonalLevelScaling(Player const* player);
    bool ExperienceBonusControlEnabled(Player const* player);

    void SetLevelScaling(Player* player, bool enabled);
    void SetExperienceBonusControl(Player* player, bool enabled);

    // Re-send the creatures around a character (and their quest giver states) after their
    // scaling state changed; served on the character's own update.
    void RefreshClient(Player* player);
    void RefreshGroup(Group* group);
    // Called from the character's update: re-sends what was requested.
    void ServeRefresh(Player* player);
    void ForgetRefresh(uint64 guid);
}

#endif
