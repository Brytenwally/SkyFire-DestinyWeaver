# mod-destiny-weaver for SkyFire 5.4.8

Per-character **open world scaling** and **experience bonus control** for
[SkyFire 5.4.8](https://github.com/Brytenwally/SkyFire_548), ported from the
AzerothCore CoA `mod-destiny-weaver`.

## What it does

- **Open world scaling, per character.** A character with scaling on meets every hostile or
  neutral creature at their own level minus 3 at least. The creature itself is not changed:
  each character is sent their own version of it (level, health, mana), and their fight follows
  that version (hit and resist rolls, armor, the damage it deals, the damage it takes, kill
  experience). Two characters can fight the same creature at different levels; everybody else
  sees it as authored.
  - Never scaled: pets, totems, summons, triggers, critters, world bosses, friendly creatures,
    heroic dungeons, raids, scenarios, battlegrounds and arenas.
  - Normal dungeons are scaled within ±3 of the character (they can also come down).
  - In a group the leader's switch decides whether the group scales; each member still gets
    their own level.
- **Quest scaling.** Quests play at the character's level: experience and money are paid at that
  level (money with a discount for quests far below it, `QuestMoneyKeepShare`), quest givers
  stop showing them as trivial, and the quest log shows the scaled level (the quest data the
  client caches is re-sent on login and whenever the character's scaling changes, as CoA does).
- **Item scaling.** Gear a character with scaling on loots below their level (and the item
  rewards of a scaled quest) is raised to the item level an item of its kind has at their
  level, and stays there, like an heirloom frozen at the level it was looted at. It keeps its
  name and its own stats, scaled; the tooltip shows the new item level and stats (the 5.4
  item upgrade, with this module's upgrade rows sent to clients as hotfixes). Upgrades only.
  A fixed random property ("of the Tiger") becomes the random suffix of the same name and
  stats, which grows with the item level like any suffix; random suffixes grow with the
  raised item level. Only items raised from then on.
- **Experience bonus control.** `.weaver xpbonus off` removes aura experience bonuses
  (heirlooms, guild perk, potions) for a character who wants the base rate.

## Commands (every player)

```
.weaver                     your current state
.weaver scaling [on|off]    open world scaling (no argument toggles)
.weaver xpbonus [on|off]    experience bonuses from auras
```

## Install

1. Needs the scaling hooks in the core (`LocalLevelScaling.h`, Brytenwally/SkyFire_548 `main`).
2. Clone this repository into the core as `modules/mod-destiny-weaver` and build
   (the SkyFire-build workflow does it when "Compile the Destiny Weaver module in" is on).
3. Copy `destiny_weaver.conf.dist` to `destiny_weaver.conf` next to `worldserver.conf` and set
   `DestinyWeaver.Enable = 1`. **The module is off by default.**
4. The `character_destiny_weaver` and `character_destiny_weaver_items` tables are created in
   the characters database at startup. The server needs `gtItemSocketCostPerLevel.dbc` in its
   dbc folder (loaded by the core).

## Not ported

- The Destiny Weaver NPC (the commands replace it).
- Ascension client features: the scaling opcode from the character creation screen and the
  creature display streaming.
- Recruit-a-friend in experience bonus control (SkyFire adds it after the script hook).

## License

GPL-2.0, like SkyFire.
