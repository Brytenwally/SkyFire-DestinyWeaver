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
  gossip quest lists, and the quest query response). `Player::RefreshQuestLogQueries`
  re-sends the query response of every quest in the log (CoA's fix for the client's cached
  quest levels); `ServeRefresh` calls it, and every login queues a refresh because the
  client cache may hold another character's levels.
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
- `DestinyWeaverItems.cpp`: item scaling. Owner's rules: upgrades only (never downgrade), no
  random suffixes or extra stats, items keep their own stats scaled; logic in the module, only
  hooks in the core (`ItemScalingHooks.h`, `Item::SetUpgradeId`, `PlayerScript::OnLootItem` /
  `OnQuestRewardItem`). An item's raise is the 5.4 upgrade modifier with this module's
  ItemUpgrade.db2 rows (ids 901001..901600 = +1..+600 item levels, path 124, hotfixes with a
  fixed timestamp: bump `HOTFIX_TIMESTAMP` if rows change). Client display needs mask 7
  (core). Stats: the client's formula (RandPropPoints x stat_unk1 / 10000 minus int(socket
  multiplier x gtItemSocketCostPerLevel)), multiplier recovered to a quarter since the DB
  rounds it; checked on 14 tooltip values. Target item level: median item level of items of
  the same class/subclass/slot/quality requiring that level (widening to subclass, class,
  +-10 levels), the owner's Level-60-World reference index. Owner's rule: loot and quest
  rewards both target the player's own level (loot only where creatures scale,
  `ScaledContent`; quest rewards when the quest is scaled). Owner's item level caps per
  player level (`DestinyWeaver.ItemScaling.ItemLevelCaps`,
  default 60:59, 70:90, 80:180, 85:315, 90:440) clip the target so low-level farming cannot
  make raid-grade gear. Random properties and suffixes (owner's report: a level 12 shortsword
  "of the Tiger" kept +1 strength at 46): a static property is a fixed enchant set, so the raise
  switches it to the tier of the same property (same name, same enchant types/stats) closest
  to its points x the budget ratio, never smaller (`RaisedProperty`, saved with the item; it
  stays if the module is turned off later). A suffix's size is the item's suffix factor, which
  the core recomputes from the base template on every load, so `ApplyRaisedSuffix` sets the
  budget at the raised level, at the raise and in ItemLoaded. Inspecting another player needs
  the core's inspect packet to carry the modifiers, property and suffix factor (fixed in
  core). Shifts persist in `character_destiny_weaver_items` (cleaned of deleted items at
  startup), restored by the ItemLoaded hook. Items loaded before OnStartup (auction house,
  guild bank) get their raise back on their next load. Bots too (owner's choice): they follow
  the same switch as for creatures (group leader's, else `Scaling.UngroupedBots`);
  SkyFire-playerbots values raised items through `Item::GetStatTemplate`, and bots roll on
  the raised version of a drop (`ItemScalingHooks::LootPreview`, same rules as `OnLootItem`
  via `RaiseFor`).
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
