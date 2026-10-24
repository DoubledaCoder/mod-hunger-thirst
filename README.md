# Hunger & Thirst (mod-hunger-thirst)

## Description

Hunger & Thirst is a standalone [AzerothCore](https://www.azerothcore.org/) module for World of Warcraft: Wrath of the Lich King 3.3.5a. It gives every character a satiety gauge and a hydration gauge, both running from 0 to 100. The gauges fall over time, are restored by eating and drinking, and can apply exhaustion damage when they run low.

No client patch is required. A graphical addon is included but **optional**: without it, chat warnings, the login summary and the `.hungerthirst` command remain available.

This repository is a module repository, not a fork of the core. It is not bundled with AzerothCore and must be installed into the `modules/` directory of an existing WotLK 3.3.5a source tree.

## Features

- Per-character satiety and hydration gauges persisted in the characters database.
- Decay based on active play time, with separate rest-area and dungeon/raid multipliers.
- Configurable pauses for taxi flights, battlegrounds, arenas, duels and game masters.
- Warning and critical thresholds with hysteresis, chat warnings and an optional login summary.
- Three simulation modes: `off`, `advisory` and `survival`.
- Optional exhaustion damage with non-lethal and lethal variants, a lethal damage ramp and a double-empty multiplier.
- Food and drink credit from items, feasts, game objects and player-cast auras, with anti-double-credit windows.
- Optional client addon using the `HT` addon prefix and the built-in 3.3.5a textures only.
- No SQL file to import: the characters table is created automatically at startup.

## Requirements

- An AzerothCore WotLK 3.3.5a server source tree that builds `worldserver`.
- A writable `acore_characters` database. The module creates and uses the table `hunger_thirst_state`.
- CMake and the normal AzerothCore build toolchain. The module adds no external dependency.
- A 3.3.5a client (build 12340) only if you want to install the optional addon.

## Installation

This is a standalone module repository. The module is **not** shipped with the core, and the core does not know about it until its files are present in the `modules/` directory and CMake is re-run.

1. Clone (or copy) this repository into the `modules/` directory of your AzerothCore source tree, using the directory name `mod-hunger-thirst`:

   ```bash
   cd /path/to/azerothcore-wotlk
   git clone <repository-url> modules/mod-hunger-thirst
   ```

2. Re-run CMake so the module is discovered, then rebuild `worldserver`:

   ```bash
   cmake -S . -B build
   cmake --build build --target worldserver -j"$(nproc)"
   ```

   With the AzerothCore Docker setup, rebuild the worldserver image and recreate the containers instead:

   ```bash
   docker compose build ac-worldserver
   docker compose up -d
   ```

3. Copy the configuration file to the module configuration directory and edit it. For Docker, the supported path is `env/dist/etc/modules/mod_hunger_thirst.conf`:

   ```bash
   cp modules/mod-hunger-thirst/conf/mod_hunger_thirst.conf.dist \
      env/dist/etc/modules/mod_hunger_thirst.conf
   ```

   On a native install, use the module configuration directory read by your `worldserver` (`etc/modules/` in a default Linux install). The module is deliberately shipped with `HungerThirst.Mode = off`; set the mode you want in this file.

4. Restart `worldserver`. The module does not start tracking until the rebuilt binary and the `.conf` file are both in place. Later configuration changes can be applied with `.reload config` instead of a restart, except for the `Enable` transition described under [Configuration](#configuration).

5. Optional: install the client addon from `client/Interface/AddOns/HungerThirst` as described under [Addon](#addon-optional).

## Configuration

The configuration file is `conf/mod_hunger_thirst.conf.dist` in this repository (section `[worldserver]`). It is deployed as `env/dist/etc/modules/mod_hunger_thirst.conf` with Docker, or as the equivalent module configuration file on a native install.

Changes can be applied with `.reload config`; they take effect at the next tick. Exception: changing `HungerThirst.Enable` from `0` to `1` does not load state for players who are already online, because their state was dropped while the module was disabled. Those players must reconnect, or a game master can initialize them with `.hungerthirst set`; new logins are unaffected.

| Key | Default | Description |
|---|---|---|
| `HungerThirst.Enable` | `1` | Master switch. With `0` the module loads and saves nothing: no decay, warnings, damage, consumable credit or periodic addon updates, and the status command reports the module as disabled. If `AddonEnable=1`, one synthetic disabled snapshot is still sent at login so the addon can display `Disabled`. Commands remain registered, but `set` and `reset` refuse to run. |
| `HungerThirst.Mode` | `off` | Simulation mode: `off`, `advisory` or `survival` (case-insensitive). An unknown value falls back to `off` and logs an error. See [Modes](#modes). |
| `HungerThirst.Lethal` | `0` | Survival only. `0` keeps damage non-lethal: each hit is capped so at least 1 health remains, and a one-time warning is sent when that floor is reached. `1` allows damage to kill; a module-caused death is recorded as a non-PvP death and carries a Starvation, Dehydration or Both cause to the death hook. |
| `HungerThirst.LethalRampPct` | `0.50` | Lethal only. While at least one gauge is at 0, percentage of maximum health added to the `DehydratedDamagePct` base for each consecutive hit. `DoubleEmptyMultiplier` is applied once when both gauges are at 0. Capped by `LethalMaxDamagePct`; clamped to 0-100. |
| `HungerThirst.LethalMaxDamagePct` | `5.00` | Lethal only. Cap for a hit while at least one gauge is at 0, as a percentage of maximum health, ramp included. Critical-band hits without an empty gauge use `StarvingDamagePct` and are not capped by this value. Clamped to 0-100. |
| `HungerThirst.SatietyDecayPerMinute` | `0.50` | Satiety points lost per minute of active play, before the rest and instance multipliers. `0` disables satiety decay. Clamped to 0-100. |
| `HungerThirst.HydrationDecayPerMinute` | `0.75` | Hydration points lost per minute of active play, before multipliers. Hydration drains slightly faster than satiety at the default values. Clamped to 0-100. |
| `HungerThirst.RestMultiplier` | `0.50` | Decay multiplier while resting (`REST_FLAG_IN_TAVERN`, `REST_FLAG_IN_CITY` or `REST_FLAG_IN_FACTION_AREA`). Multiplies the per-tick decay. Clamped to 0-10. |
| `HungerThirst.InstanceMultiplier` | `1.00` | Decay multiplier inside a dungeon or raid map (`Map::IsDungeon`). Stacks multiplicatively with `RestMultiplier` if a rest area is also an instance. Clamped to 0-10. |
| `HungerThirst.PauseOnTaxi` | `1` | Suspend decay, warnings, damage and periodic addon updates while the player is on a taxi flight. |
| `HungerThirst.PauseInBattleground` | `1` | Suspend decay, warnings and periodic addon updates while the player is in a battleground or arena (the arena switch below is an additional guard). Exhaustion damage is always suppressed in both even when this is `0`; with both switches at `0`, decay and warnings keep running. |
| `HungerThirst.PauseInArena` | `1` | Suspend decay, warnings and periodic addon updates while the player is in an arena match. Exhaustion damage is always suppressed in arenas even when this and `PauseInBattleground` are `0`; decay and warnings then keep running. |
| `HungerThirst.ExemptGMs` | `1` | Skip the module entirely for accounts with game master security. Also prevents environmental damage while debugging. |
| `HungerThirst.WarningThreshold` | `25` | Upper bound of the warning band, in gauge points. A gauge above this value is normal; at or below it the player is hungry or thirsty. Must be `<= 100` and greater than `CriticalThreshold`. |
| `HungerThirst.CriticalThreshold` | `10` | Upper bound of the critical band, in gauge points. At or below it the player is very hungry or very thirsty and survival damage begins; at `0` the gauge is empty and damage uses the dehydrated path. Must be lower than `WarningThreshold`. |
| `HungerThirst.StarvingDamagePct` | `0.50` | Survival only. Damage per hit, as a percentage of maximum health, while at least one gauge is in the critical band (`0 < gauge <= CriticalThreshold`) and neither gauge is at 0. Clamped to 0-100. |
| `HungerThirst.StarvingIntervalMs` | `10000` | Survival only. Milliseconds between hits while a gauge is critical and neither gauge is at 0. Values below 1000 are clamped to 1000. |
| `HungerThirst.DehydratedDamagePct` | `1.00` | Survival only. Base damage per hit, as a percentage of maximum health, as soon as at least one gauge is at 0, whatever the other gauge is. Clamped to 0-100. |
| `HungerThirst.DehydratedIntervalMs` | `5000` | Survival only. Milliseconds between hits while at least one gauge is at 0. Values below 1000 are clamped to 1000. |
| `HungerThirst.DoubleEmptyMultiplier` | `1.50` | Survival only. When both gauges are at 0, the `DehydratedDamagePct` base is multiplied by this value once. The starvation and dehydration percentages are never added together. In lethal mode the same single multiplier applies to the ramp base. Clamped to 1.00-10.00. |
| `HungerThirst.FoodSatietyGain` | `30.00` | Satiety restored when a food aura is credited (`SPELL_SPECIFIC_FOOD` or `FOOD_AND_DRINK`). The gain is clamped to 0-100 and the gauge itself to 100. |
| `HungerThirst.DrinkHydrationGain` | `35.00` | Hydration restored when a drink aura is credited (`SPELL_SPECIFIC_DRINK` or `FOOD_AND_DRINK`). The gain is clamped to 0-100 and the gauge itself to 100. |
| `HungerThirst.RestoreCooldownMs` | `1000` | Anti-double-credit window for item-based food and drink only: the same cast item entry is credited at most once per window, so refresh callbacks for one bite are ignored. Auras without a cast item use `AuraCreditCooldownMs` instead. Values below 1000 are clamped to 1000. |
| `HungerThirst.AuraCreditCooldownMs` | `30000` | Anti-double-credit window for every food or drink aura without a cast item (feast, game object, player-cast aura). The credit is keyed by spell ID and caster GUID, and the actual window is `max(this value, aura duration)`, so one feast tick credits a player once rather than once per server tick. Values below 1000 are clamped to 1000. |
| `HungerThirst.StartingSatiety` | `100.00` | Satiety assigned to a character with no stored row (a new character, or the first login after installing the module). Clamped to 0-100. |
| `HungerThirst.StartingHydration` | `100.00` | Hydration assigned to a character with no stored row. Clamped to 0-100. |
| `HungerThirst.Hysteresis` | `2.00` | Points a gauge must gain strictly above the threshold it is leaving before its band improves. Bands always worsen immediately. `0` disables hysteresis. Clamped to 0-`(100 - WarningThreshold)` so a full gauge can always leave the warning band. |
| `HungerThirst.ShowLoginSummary` | `1` | Send a one-line chat summary (mode, gauges, bands) on login when the module is enabled and the mode is not `off`. |
| `HungerThirst.DebugLog` | `0` | Emit `LOG_DEBUG` lines for state load and save, consumable credits and exhaustion damage. |
| `HungerThirst.AddonEnable` | `1` | Send addon state packets when `Enable=1`, `AddonEnable=1` and the player's state is loaded. A login snapshot is sent even in mode `off` (`mode=0`), and one synthetic disabled snapshot is sent when the module is disabled, so the addon can display `Disabled`. Periodic packets run only while the tick is active (`advisory` or `survival`, not paused). The packet is a `CHAT_MSG_WHISPER` / `LANG_ADDON` message sent directly to the player. |
| `HungerThirst.AddonUpdateIntervalMs` | `1000` | Periodic addon packet interval in milliseconds while the tick is active. The login snapshot is sent immediately. Values below 1000 are clamped to 1000. |
| `HungerThirst.AddonPrefix` | `HT` | Prefix written as the first tab-separated field of the addon packet. It must match the addon's prefix. An empty value falls back to `HT`. The default is the string `HT`, written as `HungerThirst.AddonPrefix = "HT"` in the file. |

Validation performed while loading:

- An unknown `Mode` becomes `off` and logs an error.
- `CriticalThreshold` must be lower than `WarningThreshold`; otherwise the critical threshold is adjusted and an error is logged. `WarningThreshold` above 100 is clamped to 100.
- Gauges, percentages, gains and decay rates are clamped to 0-100.
- `RestMultiplier` and `InstanceMultiplier` are clamped to 0-10.
- `DoubleEmptyMultiplier` is clamped to 1.00-10.00.
- Intervals and cooldowns are clamped to a minimum of 1000 ms.
- `Hysteresis` is clamped to 0-`(100 - WarningThreshold)`.

## Commands

The command is available in game only, not from the server console. Subcommands are case-insensitive.

| Command | Access | Effect |
|---|---|---|
| `.hungerthirst` or `.hungerthirst status` | Player | Show the active mode, lethal flag, state loaded or not loaded, persistence ready or unavailable, both gauges with their bands, and a one-line explanation of the mode. |
| `.hungerthirst help` | Player | Show the command list, the warning and critical thresholds, and the active mode. |
| `.hungerthirst set <player> <satiety> <hydration>` | Game master (`SEC_GAMEMASTER`) | Set both gauges, clamped to 0-100, and force a save. The target must be online. |
| `.hungerthirst reset <player>` | Game master (`SEC_GAMEMASTER`) | Restore the target to `StartingSatiety` and `StartingHydration`, and force a save. The target must be online. |

`status` and `help` still answer when the module is disabled. `set` and `reset` fail with a message when the module is disabled, when the caller lacks game master security, or when the named player is offline.

## Modes

| `HungerThirst.Mode` | Decay | Warnings and login summary | Damage | Periodic addon updates |
|---|---|---|---|---|
| `off` (default) | No | No | No | No |
| `advisory` | Yes | Yes | No, even at 0/0 | Yes |
| `survival` | Yes | Yes | Yes, from the critical band; stronger when a gauge reaches 0 | Yes |

`off` keeps the module loaded: the state is persisted and can be queried, and the login addon snapshot is still sent with `mode=0`, but nothing is simulated. `Enable=0` is stronger than `Mode=off`: state is not loaded or saved at all and the module reports itself as disabled.

### Bands

| Band | Satiety label | Hydration label |
|---|---|---|
| Normal | Satiated | Hydrated |
| Warning | Hungry | Thirsty |
| Critical | Very hungry | Very thirsty |
| Empty | Starving | Dehydrated |

A gauge is empty at 0, critical at or below `CriticalThreshold`, in the warning band at or below `WarningThreshold`, and normal above it. Warning messages fire when a band worsens. Hysteresis prevents messages from flapping around a threshold: a band only improves once the gauge is strictly above the threshold it is leaving plus `Hysteresis`.

The status command uses the words `loaded` or `not loaded` for the state and `ready` or `unavailable` for persistence.

### Damage model

Damage is applied only in `survival` mode, and only while at least one gauge is in the critical band. Exhaustion damage is always suppressed in battlegrounds and arenas, even when `PauseInBattleground` or `PauseInArena` is `0`.

- While at least one gauge is critical but neither gauge is at 0, the module applies `StarvingDamagePct` percent of maximum health every `StarvingIntervalMs`.
- As soon as at least one gauge is at 0, it applies `DehydratedDamagePct` percent of maximum health every `DehydratedIntervalMs`.
- If both gauges are at 0, the dehydration base is multiplied once by `DoubleEmptyMultiplier`; the two percentages are never added together.
- With `Lethal=0`, every hit is capped so at least 1 health remains, and a one-time warning is sent when that floor is reached. The module can never kill in this configuration.
- With `Lethal=1`, a hit taken while at least one gauge is at 0 also gains an additive ramp of `LethalRampPct` percent of maximum health per consecutive hit, capped at `LethalMaxDamagePct`. When both gauges are at 0, the once-only `DoubleEmptyMultiplier` applies to that base. Damage can kill.

### Pauses and exemptions

- Dead characters, characters not yet in the world and characters whose state is still loading or failed to load are never decayed, warned or damaged.
- A player in a duel is always paused. There is no configuration key for this.
- A taxi flight pauses the module when `PauseOnTaxi=1`.
- A battleground or arena pauses the module when the corresponding switch is `1`. Exhaustion damage is separately and always suppressed there.
- GM accounts are skipped when `ExemptGMs=1`.

While paused, decay, warnings, damage and periodic addon updates stop. The login snapshot is unaffected.

## Persistence

State is stored in the `acore_characters` database, in the table `hunger_thirst_state`. The module creates the table at startup with `CREATE TABLE IF NOT EXISTS`; there is no SQL file to import.

| Column | Type | Description |
|---|---|---|
| `guid` | `INT UNSIGNED` (primary key) | Character low GUID. |
| `satiety` | `DECIMAL(5,2)` | Satiety from 0 to 100, default `100.00`. |
| `hydration` | `DECIMAL(5,2)` | Hydration from 0 to 100, default `100.00`. |
| `updated_at` | `BIGINT UNSIGNED` | Timestamp of the last save, in epoch milliseconds. |

Writes are conditional on `updated_at`, so an older save never overwrites a newer one. State is loaded before the character enters the world, saved periodically with the character, saved synchronously on logout, and written synchronously by the `set` and `reset` commands. The row is deleted together with the character.

If the table cannot be created or verified, the simulation is suspended for the affected players: no tick, decay, warning, damage, save, consumable credit or periodic addon update. Login is never blocked. With an active mode and `ShowLoginSummary=1`, the player receives a persistence-unavailable chat notice, and with `AddonEnable=1` the addon displays the same status. The state stays unloaded for the whole session, so a reconnect is required once the table is available again.

Gauges do not decay while the character is offline; only active play time advances the simulation.

## Addon (optional)

The addon is shipped in `client/Interface/AddOns/HungerThirst`. Copy that folder into the client's addon directory:

```bash
cp -r modules/mod-hunger-thirst/client/Interface/AddOns/HungerThirst \
      "/path/to/World of Warcraft/Interface/AddOns/"
```

The addon displays the satiety and hydration gauges and their band labels, and it only reads the state sent by the server. It uses the `HT` addon prefix; `HungerThirst.AddonPrefix` must match the prefix configured in the addon. Slash commands:

- `/hungerthirst` or `/ht` shows or hides the gauge window.
- `/hungerthirst reset` moves the window and the status icons back to their default positions and shows the window.
- `/hungerthirst icons` shows or hides the status icons; the choice is saved.

The window position, icon position and icon visibility are saved between sessions.

### Protocol

The server sends a `CHAT_MSG_WHISPER` / `LANG_ADDON` message directly to the player. The payload is the prefix followed by exactly eight tab-separated fields:

```text
HT<TAB>satiety<TAB>hydration<TAB>mode<TAB>lethal<TAB>loaded<TAB>persistence<TAB>satBand<TAB>hydBand
```

- Satiety and hydration use one decimal.
- Mode is `0` for `off`, `1` for `advisory` and `2` for `survival`.
- Bands are `0` for normal, `1` for warning, `2` for critical and `3` for empty.
- `lethal`, `loaded` and `persistence` are `0` or `1`.
- There are no color codes.

A login snapshot is sent immediately. In mode `off` it carries `mode=0` and no periodic updates follow. When the module is disabled, one synthetic snapshot tells the addon to display `Disabled`. When the state failed to load because persistence is unavailable, the snapshot reports `loaded=0` and `persistence=0`. Until any packet has been received, the addon stays on `Loading...`.

### Status icons

Two small status icons, one for satiety and one for hydration, are shown by default even when the gauge window is hidden. They sit at the bottom right of the screen, can be moved by dragging, and their position is saved. The border color indicates the band: gray for normal, yellow for warning, orange for critical and red for empty. The tooltip shows the value out of 100, the band and the active mode. A left click on either icon shows or hides the gauge window. The icons use only built-in 3.3.5a textures (`INV_Misc_Food_15` and `INV_Drink_05`); no custom texture or client patch is needed.

Without the addon, everything remains functional through chat warnings, the login summary and the `.hungerthirst` command.

## Compatibility

- The module is server-side. It needs no client patch, no custom MPQ, no custom texture and no SQL import.
- It uses public AzerothCore script hooks (world configuration and startup, player load/login/update/save/logout/delete, aura apply and unit death) and its own `hunger_thirst_state` table. No core file is modified.
- The configuration namespace (`HungerThirst.*`), the command (`.hungerthirst`) and the addon prefix (`HT`) are module-specific, so the module can be installed alongside other standalone modules.
- There is no direct coupling with `mod-hardcore`. The module never resurrects a character and never writes a death table. With `survival` **and** `Lethal=1`, a death caused by hunger or thirst is an ordinary player death, so a hardcore policy applies as usual. With `Lethal=0` (the default), the module can never kill a character.
- Each death caused by the module writes one `LOG_INFO` line containing the name, GUID, level, map ID, zone ID, cause, lethal flag, persistence status and final gauges with their bands. No global announcement is made and no death history is written to the database.

## Limitations

- The 3.3.5a client exposes no native hunger or thirst gauge. The visual display therefore depends on the optional addon; without it, only chat messages and commands are available.
- The module ships with `Mode = off`. Without explicit configuration it has no gameplay effect.
- Exhaustion damage exists only in `survival` mode, and `Lethal=0` prevents it from killing.
- Well Fed buffs and similar auras do not restore the gauges. Only Food and Drink effects are credited, including food and drink items, feast or game object auras, and player-cast auras.
- There are no global announcements and no database history for starvation or dehydration deaths; only the server log records them.
- Gauges do not decay while the character is offline.
- Commands work in game only, not from the server console.
- If persistence is unavailable, the affected session stays suspended even after the database recovers; a reconnect is required.

## Credits

- The [AzerothCore](https://www.azerothcore.org/) project and community, for the WotLK 3.3.5a core, the module skeleton and the script API.
- This module follows the AzerothCore skeleton-module repository layout and documentation structure.

See [LICENSE](LICENSE) for licensing information.
