# CLAUDE.md

Notes for Claude sessions working on mod-destiny-weaver for SkyFire 5.4.8. **Keep this file
up to date** with anything future sessions need, in the same commit as the change.

## What this is

A SkyFire 5.4.8 module (lives in the core at `modules/mod-destiny-weaver`; loader
`Addmod_destiny_weaverScripts`) porting the AzerothCore CoA `mod-destiny-weaver`
(reference: Brytenwally/azerothcore-wotlk-coa `modules/mod-destiny-weaver`, read-only).
Project-wide notes are in SkyFire-playerbots' `CLAUDE.md`.

## Rules

- Commit and push straight to **`main`** (also in SkyFire_548 and SkyFire-build). No PRs
  unless the owner asks.
- Owner's choices: **disabled by default** (`DestinyWeaver.Enable = 0`), **no NPC**, players
  use the `.weaver` command (permission `RBAC_PERM_COMMAND_SAVE`, which the player role has).

## How it works

- Core side (SkyFire_548 `src/server/game/Miscellaneous/LocalLevelScaling.h`): resolver
  pointers the module installs (`ApplyScalingConfig`), all null when the module is off.
  Hooks: `Unit::BuildValuesUpdate` (level/health/power fields per recipient),
  `Creature::getLevelForTarget`, `Unit::CalcArmorReducedDamage`, glancing/resist/armor level
  in `UnitCombat.cpp`, kill XP (`Formulas.h`, KillRewarder grey check and expansion penalty
  skipped for scaled kills), `Player::isHonorOrXPTarget`, quests (`Quest::XPValue`,
  `Quest::GetRewMoney(Player const*)`, `Quest::GetQuestLevelFor`, `Player::GetQuestLevel`,
  gossip quest lists).
- `DestinyWeaverScaling.cpp`: `ViewFor` builds one character's version of a creature from
  `creature_classlevelstats` (ratios, so template modifiers cancel); damage hooks
  (`OnDamage` onto the real pool, `ModifyMeleeDamage` / `ModifySpellDamageTaken` /
  `ModifyPeriodicDamageAurasTick` for damage taken); `ResendNearbyCreatures` forces the
  view fields of creatures within 120 yd after a change.
- `DestinyWeaver.cpp`: config (`destiny_weaver.conf` merged with `LoadMore`, again on
  `.reload config`), choices cached per online character (`g_choices`, shared_mutex; the
  cache doubles as "who is online" for the leader rule), table `character_destiny_weaver`
  created at startup, `.weaver` commands, group hooks, XP bonus stripping in `OnGiveXP`
  (aura multipliers divided out; victim null = quest).
- Playerbots: bot sessions (`WorldSession::IsBot`) have no stored choice; grouped bots follow
  the leader, ungrouped ones use `DestinyWeaver.Scaling.UngroupedBots` (default off).

## Checking

Syntax-only compile against the core headers, as in SkyFire-playerbots' CLAUDE.md:

```sh
INC=$(find /path/SkyFire_548/src/server /path/SkyFire_548/src/common /path/SkyFire_548/dep -type d | sed 's|^|-I|')
g++-14 -std=c++23 -fsyntax-only -Wall -Wextra -I/tmp/gen -Isrc $INC -I/usr/include/mysql src/DestinyWeaver.cpp
```

Real builds: the SkyFire-build workflow. No server in the cloud container: runtime behaviour
is untested there.
