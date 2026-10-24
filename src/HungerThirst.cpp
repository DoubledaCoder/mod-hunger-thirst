/*
 * Hunger & Thirst - implementation.
 *
 * The module keeps one hunger/thirst state per character in memory, keyed by
 * the low guid, and persists a single row per character in
 * `acore_characters`.`hunger_thirst_state`. All player-facing entry points are
 * declared in HungerThirst.h; player/unit hooks and commands only go through
 * that interface.
 *
 * Design notes:
 *   - The cache is touched from map update threads (Tick) and from the world
 *     thread (commands). Every access is protected by a mutex; no re-entrant
 *     player API (damage, chat, script hooks) is called while that mutex is
 *     held, because a damage application can run script hooks that re-enter
 *     this module.
 *   - Writes are conditional on `updated_at` so a stale save (for example a
 *     logout racing a periodic save) can never overwrite newer gauges.
 *   - A DB error is not treated as "new character": the state stays
 *     Loaded=false and is neither ticked nor saved until a successful load.
 *
 * Released under GNU GPL v2 or (at your option) any later version.
 */

#include "HungerThirst.h"

#include "Chat.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "Map.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "StringFormat.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace HungerThirst
{
    Config& GetConfig()
    {
        static Config config;
        return config;
    }

    namespace
    {
        constexpr uint32 MaxTickDiffMs      = 10000;
        constexpr uint32 MinIntervalMs      = 1000;
        constexpr double MinGaugeValue      = 0.00;
        constexpr double MaxGaugeValue      = 100.00;
        constexpr uint32 MaxRampTicks       = 1000;
        constexpr std::size_t MaxCachedAuras = 64;

        struct State
        {
            double Satiety         = MaxGaugeValue;
            double Hydration       = MaxGaugeValue;
            bool   Loaded          = false;
            bool   Dirty           = false;
            uint64 UpdatedAtMs     = 0;
            uint64 DamageTimerMs   = 0;
            uint32 RampTicks       = 0;
            Band   SatietyBand     = Band::Normal;
            Band   HydrationBand   = Band::Normal;
            uint64 AddonTimerMs    = 0;
            bool   AtOneHpWarned   = false;

            // Anti-double-credit bookkeeping.
            std::map<uint32, uint64> ItemCredits; // item entry -> expiry epoch ms
            std::map<std::pair<uint32, uint64>, uint64> AuraCredits; // (spellId, caster guid) -> expiry epoch ms
        };

        std::unordered_map<uint32, State> g_states;
        std::mutex g_stateMutex;

        std::mutex g_schemaMutex;
        std::atomic<bool> g_persistenceReady{ false };

        thread_local DeathCause g_pendingDeathCause = DeathCause::None;

        uint64 NowMs()
        {
            return static_cast<uint64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        }

        double ClampGauge(double value)
        {
            if (!std::isfinite(value))
                value = MinGaugeValue;

            return std::clamp(value, MinGaugeValue, MaxGaugeValue);
        }

        double ClampRange(double value, double minValue, double maxValue, char const* key)
        {
            if (!std::isfinite(value))
            {
                LOG_ERROR("module.hunger-thirst", "HungerThirst.{} is not a finite number; using {}.", key, minValue);
                return minValue;
            }

            if (value < minValue)
            {
                LOG_ERROR("module.hunger-thirst", "HungerThirst.{} = {} is below {}; clamping.", key, value, minValue);
                return minValue;
            }

            if (value > maxValue)
            {
                LOG_ERROR("module.hunger-thirst", "HungerThirst.{} = {} is above {}; clamping.", key, value, maxValue);
                return maxValue;
            }

            return value;
        }

        uint32 ClampInterval(uint32 value, char const* key)
        {
            if (value < MinIntervalMs)
            {
                LOG_ERROR("module.hunger-thirst", "HungerThirst.{} = {} ms is below the minimum of {} ms; clamping.",
                    key, value, MinIntervalMs);
                return MinIntervalMs;
            }

            return value;
        }

        Band RawBand(double value, Config const& cfg)
        {
            if (value <= MinGaugeValue)
                return Band::Empty;

            if (value <= double(cfg.CriticalThreshold))
                return Band::Critical;

            if (value <= double(cfg.WarningThreshold))
                return Band::Warning;

            return Band::Normal;
        }

        // Bands worsen immediately. They only improve once the gauge is
        // strictly above the current band's exit threshold plus Hysteresis.
        Band ResolveBand(double value, Band current, Config const& cfg)
        {
            Band const raw = RawBand(value, cfg);

            if (raw > current)
                return raw; // worse band: apply immediately

            if (raw == current)
                return current;

            // raw < current: improvement candidate. Stay in the current band
            // until the gauge is strictly above threshold + Hysteresis.
            double exitAt = 0.00;
            switch (current)
            {
                case Band::Empty:
                    exitAt = MinGaugeValue;
                    break;
                case Band::Critical:
                    exitAt = double(cfg.CriticalThreshold) + cfg.Hysteresis;
                    break;
                case Band::Warning:
                    exitAt = double(cfg.WarningThreshold) + cfg.Hysteresis;
                    break;
                case Band::Normal:
                    return raw;
            }

            // A full gauge always recovers, even with the maximum allowed
            // hysteresis.
            if (value >= MaxGaugeValue)
                return raw;

            return value > exitAt ? raw : current;
        }

        char const* BandMessage(bool hydration, Band band)
        {
            switch (band)
            {
                case Band::Warning:
                    return hydration
                        ? "You are starting to get thirsty."
                        : "You are starting to get hungry.";
                case Band::Critical:
                    return hydration
                        ? "You are very thirsty. Drink something soon."
                        : "You are very hungry. Eat something soon.";
                case Band::Empty:
                    return hydration
                        ? "You are dehydrated."
                        : "You are starving.";
                case Band::Normal:
                    break;
            }

            return nullptr;
        }

        char const* CauseName(DeathCause cause)
        {
            switch (cause)
            {
                case DeathCause::Starvation:
                    return "starvation";
                case DeathCause::Dehydration:
                    return "dehydration";
                case DeathCause::Both:
                    return "starvation and dehydration";
                case DeathCause::None:
                    break;
            }

            return "none";
        }

        void SendChatLine(Player* player, std::string_view message)
        {
            if (!player || message.empty() || !player->GetSession())
                return;

            ChatHandler(player->GetSession())
                .SendSysMessage(Acore::StringFormat("|cffffcc00[Hunger & Thirst]|r {}", message));
        }

        template <typename Key>
        void PruneExpiredMap(std::map<Key, uint64>& credits, uint64 now)
        {
            for (auto it = credits.begin(); it != credits.end();)
            {
                if (it->second <= now)
                    it = credits.erase(it);
                else
                    ++it;
            }
        }

        void PruneExpiredCredits(State& state, uint64 now)
        {
            PruneExpiredMap(state.ItemCredits, now);
            PruneExpiredMap(state.AuraCredits, now);
        }
    } // namespace

    // Public contract implementation (declared in HungerThirst.h).

    void LoadConfig(bool reload)
    {
        Config& cfg = GetConfig();

        cfg.Enable = sConfigMgr->GetOption<bool>("HungerThirst.Enable", true);

        std::string mode = sConfigMgr->GetOption<std::string>("HungerThirst.Mode", "off");
        std::transform(mode.begin(), mode.end(), mode.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (mode == "off")
            cfg.CurrentMode = Mode::Off;
        else if (mode == "advisory")
            cfg.CurrentMode = Mode::Advisory;
        else if (mode == "survival")
            cfg.CurrentMode = Mode::Survival;
        else
        {
            LOG_ERROR("module.hunger-thirst", "Unknown HungerThirst.Mode '{}'; falling back to off.", mode);
            cfg.CurrentMode = Mode::Off;
        }

        cfg.Lethal = sConfigMgr->GetOption<bool>("HungerThirst.Lethal", false);
        cfg.LethalRampPct = ClampRange(
            sConfigMgr->GetOption<float>("HungerThirst.LethalRampPct", 0.50),
            0.00, 100.00, "LethalRampPct");
        cfg.LethalMaxDamagePct = ClampRange(
            sConfigMgr->GetOption<float>("HungerThirst.LethalMaxDamagePct", 5.00),
            0.00, 100.00, "LethalMaxDamagePct");

        cfg.SatietyDecayPerMinute = ClampRange(
            sConfigMgr->GetOption<float>("HungerThirst.SatietyDecayPerMinute", 0.50),
            0.00, 100.00, "SatietyDecayPerMinute");
        cfg.HydrationDecayPerMinute = ClampRange(
            sConfigMgr->GetOption<float>("HungerThirst.HydrationDecayPerMinute", 0.75),
            0.00, 100.00, "HydrationDecayPerMinute");

        cfg.RestMultiplier = ClampRange(
            sConfigMgr->GetOption<float>("HungerThirst.RestMultiplier", 0.50),
            0.00, 10.00, "RestMultiplier");
        cfg.InstanceMultiplier = ClampRange(
            sConfigMgr->GetOption<float>("HungerThirst.InstanceMultiplier", 1.00),
            0.00, 10.00, "InstanceMultiplier");

        cfg.PauseOnTaxi = sConfigMgr->GetOption<bool>("HungerThirst.PauseOnTaxi", true);
        cfg.PauseInBattleground = sConfigMgr->GetOption<bool>("HungerThirst.PauseInBattleground", true);
        cfg.PauseInArena = sConfigMgr->GetOption<bool>("HungerThirst.PauseInArena", true);
        cfg.ExemptGMs = sConfigMgr->GetOption<bool>("HungerThirst.ExemptGMs", true);

        uint32 warningThreshold = sConfigMgr->GetOption<uint32>("HungerThirst.WarningThreshold", 25);
        if (warningThreshold > 100)
        {
            LOG_ERROR("module.hunger-thirst",
                "HungerThirst.WarningThreshold = {} is above 100; clamping.", warningThreshold);
            warningThreshold = 100;
        }

        uint32 criticalThreshold = sConfigMgr->GetOption<uint32>("HungerThirst.CriticalThreshold", 10);
        if (criticalThreshold >= warningThreshold)
        {
            uint32 const adjusted = warningThreshold > 0 ? warningThreshold - 1 : 0;
            LOG_ERROR("module.hunger-thirst",
                "HungerThirst.CriticalThreshold ({}) must be lower than HungerThirst.WarningThreshold ({}); using {}.",
                criticalThreshold, warningThreshold, adjusted);
            criticalThreshold = adjusted;
        }

        cfg.WarningThreshold = warningThreshold;
        cfg.CriticalThreshold = criticalThreshold;

        cfg.StarvingDamagePct = ClampRange(
            sConfigMgr->GetOption<float>("HungerThirst.StarvingDamagePct", 0.50),
            0.00, 100.00, "StarvingDamagePct");
        cfg.StarvingIntervalMs = ClampInterval(
            sConfigMgr->GetOption<uint32>("HungerThirst.StarvingIntervalMs", 10000), "StarvingIntervalMs");

        cfg.DehydratedDamagePct = ClampRange(
            sConfigMgr->GetOption<float>("HungerThirst.DehydratedDamagePct", 1.00),
            0.00, 100.00, "DehydratedDamagePct");
        cfg.DehydratedIntervalMs = ClampInterval(
            sConfigMgr->GetOption<uint32>("HungerThirst.DehydratedIntervalMs", 5000), "DehydratedIntervalMs");

        cfg.DoubleEmptyMultiplier = ClampRange(
            sConfigMgr->GetOption<float>("HungerThirst.DoubleEmptyMultiplier", 1.50),
            1.00, 10.00, "DoubleEmptyMultiplier");

        cfg.FoodSatietyGain = ClampRange(
            sConfigMgr->GetOption<float>("HungerThirst.FoodSatietyGain", 30.00),
            0.00, 100.00, "FoodSatietyGain");
        cfg.DrinkHydrationGain = ClampRange(
            sConfigMgr->GetOption<float>("HungerThirst.DrinkHydrationGain", 35.00),
            0.00, 100.00, "DrinkHydrationGain");

        cfg.RestoreCooldownMs = ClampInterval(
            sConfigMgr->GetOption<uint32>("HungerThirst.RestoreCooldownMs", 1000), "RestoreCooldownMs");
        cfg.AuraCreditCooldownMs = ClampInterval(
            sConfigMgr->GetOption<uint32>("HungerThirst.AuraCreditCooldownMs", 30000), "AuraCreditCooldownMs");

        cfg.StartingSatiety = ClampRange(
            sConfigMgr->GetOption<float>("HungerThirst.StartingSatiety", 100.00),
            0.00, 100.00, "StartingSatiety");
        cfg.StartingHydration = ClampRange(
            sConfigMgr->GetOption<float>("HungerThirst.StartingHydration", 100.00),
            0.00, 100.00, "StartingHydration");

        double const maxHysteresis = std::max(0.00, MaxGaugeValue - double(cfg.WarningThreshold));
        cfg.Hysteresis = ClampRange(
            sConfigMgr->GetOption<float>("HungerThirst.Hysteresis", 2.00),
            0.00, maxHysteresis, "Hysteresis");

        cfg.ShowLoginSummary = sConfigMgr->GetOption<bool>("HungerThirst.ShowLoginSummary", true);
        cfg.DebugLog = sConfigMgr->GetOption<bool>("HungerThirst.DebugLog", false);

        cfg.AddonEnable = sConfigMgr->GetOption<bool>("HungerThirst.AddonEnable", true);
        cfg.AddonUpdateIntervalMs = ClampInterval(
            sConfigMgr->GetOption<uint32>("HungerThirst.AddonUpdateIntervalMs", 1000), "AddonUpdateIntervalMs");
        cfg.AddonPrefix = sConfigMgr->GetOption<std::string>("HungerThirst.AddonPrefix", "HT");
        if (cfg.AddonPrefix.empty())
        {
            LOG_ERROR("module.hunger-thirst", "HungerThirst.AddonPrefix is empty; using \"HT\".");
            cfg.AddonPrefix = "HT";
        }

        LOG_INFO("module.hunger-thirst", "Configuration {}: enable {}, mode {}, lethal {}, addon {}.",
            reload ? "reloaded" : "loaded", cfg.Enable, ModeName(cfg.CurrentMode), cfg.Lethal, cfg.AddonEnable);
    }

    void EnsureSchema()
    {
        std::lock_guard<std::mutex> lock(g_schemaMutex);
        if (g_persistenceReady.load())
            return;

        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS `hunger_thirst_state` ("
            "`guid` INT UNSIGNED NOT NULL, "
            "`satiety` DECIMAL(5,2) NOT NULL DEFAULT 100.00, "
            "`hydration` DECIMAL(5,2) NOT NULL DEFAULT 100.00, "
            "`updated_at` BIGINT UNSIGNED NOT NULL DEFAULT 0, "
            "PRIMARY KEY (`guid`)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;");

        // An empty table makes the probe return no row even though the table
        // exists, so COUNT(*) is used to distinguish "empty" from
        // "missing/DB error" (it always returns exactly one row on success).
        bool ready = static_cast<bool>(CharacterDatabase.Query(
            "SELECT 1 FROM `hunger_thirst_state` LIMIT 1"));

        if (!ready)
        {
            ready = static_cast<bool>(CharacterDatabase.Query(
                "SELECT COUNT(*) FROM `hunger_thirst_state`"));
        }

        g_persistenceReady.store(ready);

        if (ready)
            LOG_INFO("module.hunger-thirst", "Persistence ready (table `hunger_thirst_state`).");
        else
            LOG_ERROR("module.hunger-thirst",
                "Persistence unavailable: `hunger_thirst_state` could not be created or verified.");
    }

    bool IsPersistenceReady()
    {
        return g_persistenceReady.load();
    }

    bool LoadState(Player* player)
    {
        if (!player)
            return false;

        Config const& cfg = GetConfig();
        uint32 const guid = player->GetGUID().GetCounter();

        if (!cfg.Enable)
        {
            RemoveStateByGuid(guid);
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            auto const it = g_states.find(guid);
            if (it != g_states.end() && it->second.Loaded)
                return true;
        }

        if (!IsPersistenceReady())
            EnsureSchema();

        State state;
        state.Satiety = ClampGauge(cfg.StartingSatiety);
        state.Hydration = ClampGauge(cfg.StartingHydration);
        state.SatietyBand = RawBand(state.Satiety, cfg);
        state.HydrationBand = RawBand(state.Hydration, cfg);
        state.Loaded = false;
        state.Dirty = false;

        if (!IsPersistenceReady())
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            g_states[guid] = state;
            LOG_ERROR("module.hunger-thirst", "Cannot load state for {} ({}): persistence is unavailable.",
                player->GetName(), guid);
            return false;
        }

        QueryResult row = CharacterDatabase.Query(
            "SELECT `satiety`, `hydration`, `updated_at` FROM `hunger_thirst_state` WHERE `guid` = {}", guid);

        if (row)
        {
            Field* fields = row->Fetch();
            state.Satiety = ClampGauge(fields[0].Get<double>());
            state.Hydration = ClampGauge(fields[1].Get<double>());
            state.UpdatedAtMs = fields[2].Get<uint64>();
            state.SatietyBand = RawBand(state.Satiety, cfg);
            state.HydrationBand = RawBand(state.Hydration, cfg);
            state.Loaded = true;
            state.Dirty = false;
        }
        else
        {
            // Query() returns null both for "no row" and for a DB error;
            // COUNT(*) returns exactly one row on success, so it tells the
            // two cases apart.
            QueryResult countResult = CharacterDatabase.Query(
                "SELECT COUNT(*) FROM `hunger_thirst_state` WHERE `guid` = {}", guid);

            if (!countResult)
            {
                std::lock_guard<std::mutex> lock(g_stateMutex);
                g_states[guid] = state;
                LOG_ERROR("module.hunger-thirst", "Database error while loading state for {} ({}).",
                    player->GetName(), guid);
                return false;
            }

            uint64 const rowCount = countResult->Fetch()->Get<uint64>();
            if (rowCount == 0)
            {
                state.Loaded = true;
                state.Dirty = true;
            }
            else
            {
                std::lock_guard<std::mutex> lock(g_stateMutex);
                g_states[guid] = state;
                LOG_ERROR("module.hunger-thirst", "State row exists but could not be read for {} ({}).",
                    player->GetName(), guid);
                return false;
            }
        }

        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            g_states[guid] = state;
        }

        if (cfg.DebugLog)
        {
            LOG_DEBUG("module.hunger-thirst",
                "Loaded state for {} ({}): satiety {:.2f}, hydration {:.2f}, updated_at {}.",
                player->GetName(), guid, state.Satiety, state.Hydration, state.UpdatedAtMs);
        }

        return true;
    }

    void SaveState(Player* player, bool force)
    {
        if (!player)
            return;

        Config const& cfg = GetConfig();
        if (!cfg.Enable || !IsPersistenceReady())
            return;

        uint32 const guid = player->GetGUID().GetCounter();
        double satiety = 0.00;
        double hydration = 0.00;
        uint64 updatedAt = 0;

        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            auto const it = g_states.find(guid);
            if (it == g_states.end() || !it->second.Loaded)
                return;

            State& state = it->second;
            if (!state.Dirty && !force)
                return;

            satiety = state.Satiety;
            hydration = state.Hydration;
            updatedAt = std::max<uint64>(NowMs(), state.UpdatedAtMs + 1);
            state.UpdatedAtMs = updatedAt;
            state.Dirty = false;
        }

        // Conditional upsert: a stale writer (older timestamp) is ignored, so
        // concurrent saves cannot roll the gauges back. Forced saves (logout,
        // GM commands) run synchronously so an immediate reconnect cannot
        // read the stale row; periodic saves stay asynchronous.
        char const* const query =
            "INSERT INTO `hunger_thirst_state` (`guid`, `satiety`, `hydration`, `updated_at`) "
            "VALUES ({}, {}, {}, {}) "
            "ON DUPLICATE KEY UPDATE "
            "`satiety` = IF(VALUES(`updated_at`) >= `updated_at`, VALUES(`satiety`), `satiety`), "
            "`hydration` = IF(VALUES(`updated_at`) >= `updated_at`, VALUES(`hydration`), `hydration`), "
            "`updated_at` = GREATEST(`updated_at`, VALUES(`updated_at`))";

        if (force)
            CharacterDatabase.DirectExecute(query, guid, satiety, hydration, updatedAt);
        else
            CharacterDatabase.Execute(query, guid, satiety, hydration, updatedAt);

        if (cfg.DebugLog)
        {
            LOG_DEBUG("module.hunger-thirst",
                "Saved state for guid {}: satiety {:.2f}, hydration {:.2f}, updated_at {}.",
                guid, satiety, hydration, updatedAt);
        }
    }

    void RemoveState(Player* player)
    {
        if (!player)
            return;

        RemoveStateByGuid(player->GetGUID().GetCounter());
    }

    void RemoveStateByGuid(uint32 guid)
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_states.erase(guid);
    }

    void Tick(Player* player, uint32 diff)
    {
        Config const& cfg = GetConfig();
        if (!player || !cfg.Enable || cfg.CurrentMode == Mode::Off)
            return;

        if (!player->IsInWorld() || !player->IsAlive())
            return;

        bool const inBattleground = player->GetBattlegroundId() != 0;
        bool const inArena = player->InArena();

        if (cfg.ExemptGMs && player->GetSession() && player->GetSession()->IsGMAccount())
            return;

        if (cfg.PauseOnTaxi && player->IsInFlight())
            return;

        if (cfg.PauseInBattleground && inBattleground)
            return;

        if (cfg.PauseInArena && inArena)
            return;

        if (player->duel)
            return;

        uint32 const effectiveDiff = std::min<uint32>(diff, MaxTickDiffMs);
        if (!effectiveDiff)
            return;

        bool const inDungeon = player->GetMap() && player->GetMap()->IsDungeon();
        bool const resting = player->HasRestFlag(REST_FLAG_IN_TAVERN) ||
                             player->HasRestFlag(REST_FLAG_IN_CITY) ||
                             player->HasRestFlag(REST_FLAG_IN_FACTION_AREA);

        double multiplier = 1.00;
        if (resting)
            multiplier *= cfg.RestMultiplier;
        if (inDungeon)
            multiplier *= cfg.InstanceMultiplier;

        uint32 const guid = player->GetGUID().GetCounter();
        uint32 const playerHealth = player->GetHealth();
        uint32 const playerMaxHealth = player->GetMaxHealth();

        char const* satietyMessage = nullptr;
        char const* hydrationMessage = nullptr;
        bool sendAddon = false;
        bool atOneHpWarning = false;
        bool satietyCritical = false;
        bool hydrationCritical = false;
        bool satietyEmpty = false;
        bool hydrationEmpty = false;
        bool anyEmpty = false;
        bool bothEmpty = false;
        uint32 damageToApply = 0;
        bool const lethal = cfg.Lethal;

        {
            std::lock_guard<std::mutex> lock(g_stateMutex);

            auto const it = g_states.find(guid);
            if (it == g_states.end() || !it->second.Loaded)
                return;

            State& state = it->second;

            double const minutes = double(effectiveDiff) / 60000.0;
            double const satietyLoss = std::max(0.00, cfg.SatietyDecayPerMinute * minutes * multiplier);
            double const hydrationLoss = std::max(0.00, cfg.HydrationDecayPerMinute * minutes * multiplier);

            double const oldSatiety = state.Satiety;
            double const oldHydration = state.Hydration;
            Band const oldSatietyBand = state.SatietyBand;
            Band const oldHydrationBand = state.HydrationBand;

            state.Satiety = ClampGauge(state.Satiety - satietyLoss);
            state.Hydration = ClampGauge(state.Hydration - hydrationLoss);

            if (state.Satiety != oldSatiety || state.Hydration != oldHydration)
                state.Dirty = true;

            state.SatietyBand = ResolveBand(state.Satiety, state.SatietyBand, cfg);
            state.HydrationBand = ResolveBand(state.Hydration, state.HydrationBand, cfg);

            if (state.SatietyBand > oldSatietyBand)
                satietyMessage = BandMessage(false, state.SatietyBand);
            if (state.HydrationBand > oldHydrationBand)
                hydrationMessage = BandMessage(true, state.HydrationBand);

            satietyCritical = state.Satiety <= double(cfg.CriticalThreshold);
            hydrationCritical = state.Hydration <= double(cfg.CriticalThreshold);
            satietyEmpty = state.Satiety <= MinGaugeValue;
            hydrationEmpty = state.Hydration <= MinGaugeValue;
            anyEmpty = satietyEmpty || hydrationEmpty;
            bothEmpty = satietyEmpty && hydrationEmpty;

            if (cfg.CurrentMode == Mode::Survival && (satietyCritical || hydrationCritical)
                && !inBattleground && !inArena)
            {
                // The warning flag is only re-armed when the critical
                // situation ends (else branch) or a food/drink credit lands,
                // so health regeneration does not repeat the message.

                // The lethal ramp only advances while at least one gauge is
                // actually empty; a critical-but-nonzero gauge does not ramp.
                if (!anyEmpty)
                    state.RampTicks = 0;

                uint32 const interval = anyEmpty ? cfg.DehydratedIntervalMs : cfg.StarvingIntervalMs;

                state.DamageTimerMs += effectiveDiff;
                if (state.DamageTimerMs >= interval)
                {
                    // One damage event per tick at most; the remainder is kept
                    // so the cadence stays honest across short updates.
                    state.DamageTimerMs -= interval;

                    double pct = anyEmpty ? cfg.DehydratedDamagePct : cfg.StarvingDamagePct;
                    if (bothEmpty)
                        pct *= cfg.DoubleEmptyMultiplier;

                    if (lethal && anyEmpty)
                    {
                        pct = (cfg.DehydratedDamagePct + cfg.LethalRampPct * double(state.RampTicks)) *
                            (bothEmpty ? cfg.DoubleEmptyMultiplier : 1.00);
                        pct = std::min(pct, cfg.LethalMaxDamagePct);
                        state.RampTicks = std::min<uint32>(state.RampTicks + 1, MaxRampTicks);
                    }

                    uint32 const maxHealth = playerMaxHealth;
                    uint32 damage = maxHealth
                        ? uint32(std::ceil(double(maxHealth) * std::max(0.00, pct) / 100.00))
                        : 1;
                    damage = std::max<uint32>(1, damage);

                    if (!lethal)
                    {
                        uint32 const health = playerHealth;
                        if (health <= 1)
                        {
                            damage = 0;
                            if (!state.AtOneHpWarned)
                                atOneHpWarning = true;
                            state.AtOneHpWarned = true;
                        }
                        else if (damage >= health - 1)
                        {
                            damage = health - 1;
                            if (!state.AtOneHpWarned)
                                atOneHpWarning = true;
                            state.AtOneHpWarned = true;
                        }
                    }

                    damageToApply = damage;
                }
            }
            else
            {
                state.DamageTimerMs = 0;
                state.RampTicks = 0;
                state.AtOneHpWarned = false;
            }

            if (cfg.AddonEnable && cfg.AddonUpdateIntervalMs > 0)
            {
                state.AddonTimerMs += effectiveDiff;
                if (state.AddonTimerMs >= cfg.AddonUpdateIntervalMs)
                {
                    state.AddonTimerMs %= cfg.AddonUpdateIntervalMs;
                    sendAddon = true;
                }
            }
            else
                state.AddonTimerMs = 0;
        }

        if (satietyMessage)
            SendChatLine(player, satietyMessage);

        if (hydrationMessage)
            SendChatLine(player, hydrationMessage);

        if (atOneHpWarning)
            SendChatLine(player,
                "You are at 1 health because of hunger and thirst. "
                "Eat and drink immediately!");

        if (damageToApply > 0)
        {
            DeathCause const cause = bothEmpty ? DeathCause::Both
                : satietyEmpty ? DeathCause::Starvation
                : hydrationEmpty ? DeathCause::Dehydration
                : satietyCritical ? DeathCause::Starvation
                : DeathCause::Dehydration;

            // Set before applying: a lethal hit runs the unit death hook
            // synchronously from inside EnvironmentalDamage, and that hook
            // must be able to consume the cause.
            if (lethal)
            {
                player->SetPvPDeath(false);
                SetPendingDeathCause(cause);
            }

            uint32 const dealt = player->EnvironmentalDamage(DAMAGE_EXHAUSTED, damageToApply);

            if (!player->IsAlive())
            {
                // The environmental death is not a PvP death. Set it again in
                // case the damage handler created the corpse before returning.
                player->SetPvPDeath(false);
            }
            else if (lethal)
            {
                // The hit did not kill after all (immunity, absorb, ...).
                SetPendingDeathCause(DeathCause::None);
            }

            if (cfg.DebugLog)
            {
                LOG_DEBUG("module.hunger-thirst",
                    "Exhaustion damage {} requested {} for {} (cause {}, lethal {}).",
                    dealt, damageToApply, player->GetName(), CauseName(cause), lethal);
            }
        }

        if (sendAddon)
            SendAddonState(player);
    }

    void CreditConsumable(Player* player, Aura* aura)
    {
        if (!player || !aura)
            return;

        // Auras restored from the database are re-applied while the session is
        // still loading; they are not a new consumption.
        if (!player->GetSession() || player->GetSession()->PlayerLoading())
            return;

        Config const& cfg = GetConfig();
        if (!cfg.Enable || cfg.CurrentMode == Mode::Off)
            return;

        SpellInfo const* spellInfo = aura->GetSpellInfo();
        if (!spellInfo || !spellInfo->IsPositive())
            return;

        SpellSpecificType const specific = spellInfo->GetSpellSpecific();
        bool const food = specific == SPELL_SPECIFIC_FOOD || specific == SPELL_SPECIFIC_FOOD_AND_DRINK;
        bool const drink = specific == SPELL_SPECIFIC_DRINK || specific == SPELL_SPECIFIC_FOOD_AND_DRINK;
        if (!food && !drink)
            return;

        uint32 const guid = player->GetGUID().GetCounter();
        uint32 const spellId = spellInfo->Id;
        uint32 const itemEntry = aura->GetCastItemEntry();
        ObjectGuid const casterGuid = aura->GetCasterGUID();
        uint64 const now = NowMs();
        double creditedSatiety = 0.00;
        double creditedHydration = 0.00;

        {
            std::lock_guard<std::mutex> lock(g_stateMutex);

            auto const it = g_states.find(guid);
            if (it == g_states.end() || !it->second.Loaded)
                return;

            State& state = it->second;

            if (itemEntry != 0)
            {
                // Item path: refresh callbacks fire several times for the same
                // bite, so the same cast item entry is credited once per
                // RestoreCooldownMs.
                auto const itemCredit = state.ItemCredits.find(itemEntry);
                if (itemCredit != state.ItemCredits.end() && now < itemCredit->second)
                    return;

                state.ItemCredits[itemEntry] = now + cfg.RestoreCooldownMs;
                if (state.ItemCredits.size() > MaxCachedAuras)
                    PruneExpiredCredits(state, now);
            }
            else
            {
                // No cast item (feast, gameobject, player-cast aura): one
                // credit per (spellId, casterGuid) for at least the aura's
                // full/remaining duration or AuraCreditCooldownMs.
                std::pair<uint32, uint64> const key(spellId, casterGuid.GetRawValue());
                uint64 const auraDurationMs = aura->GetDuration() > 0 ? uint64(aura->GetDuration()) : 0;
                uint64 const cooldown = std::max<uint64>(cfg.AuraCreditCooldownMs, auraDurationMs);

                auto const credit = state.AuraCredits.find(key);
                if (credit != state.AuraCredits.end() && now < credit->second)
                    return;

                state.AuraCredits[key] = now + cooldown;
                if (state.AuraCredits.size() > MaxCachedAuras)
                    PruneExpiredCredits(state, now);
            }

            if (food)
                state.Satiety = ClampGauge(state.Satiety + cfg.FoodSatietyGain);
            if (drink)
                state.Hydration = ClampGauge(state.Hydration + cfg.DrinkHydrationGain);

            state.SatietyBand = ResolveBand(state.Satiety, state.SatietyBand, cfg);
            state.HydrationBand = ResolveBand(state.Hydration, state.HydrationBand, cfg);
            state.Dirty = true;

            if (state.Satiety > MinGaugeValue || state.Hydration > MinGaugeValue)
                state.AtOneHpWarned = false;

            creditedSatiety = state.Satiety;
            creditedHydration = state.Hydration;
        }

        if (cfg.DebugLog)
        {
            LOG_DEBUG("module.hunger-thirst",
                "Credited {} (spell {}, item {}, caster {}) for {}: satiety {:.2f}, hydration {:.2f}.",
                food && drink ? "food and drink" : food ? "food" : "drink",
                spellId, itemEntry, casterGuid.ToString(), player->GetName(),
                creditedSatiety, creditedHydration);
        }

        // Refresh the client immediately. Tick is the periodic sender, but it
        // is deliberately skipped for exempt GMs and paused players; without
        // this push, eating would update the server state but leave the bars
        // frozen until the next command or login.
        SendAddonState(player);
    }

    bool GetSnapshot(Player* player, Snapshot& out)
    {
        out = Snapshot();

        if (!player)
            return false;

        Config const& cfg = GetConfig();
        uint32 const guid = player->GetGUID().GetCounter();

        std::lock_guard<std::mutex> lock(g_stateMutex);

        auto const it = g_states.find(guid);
        if (it == g_states.end())
        {
            out.CurrentMode = cfg.CurrentMode;
            out.Lethal = cfg.Lethal;
            out.PersistenceReady = IsPersistenceReady();
            return false;
        }

        State const& state = it->second;
        out.Satiety = state.Satiety;
        out.Hydration = state.Hydration;
        out.CurrentMode = cfg.CurrentMode;
        out.Lethal = cfg.Lethal;
        out.Loaded = state.Loaded;
        out.PersistenceReady = IsPersistenceReady();
        out.SatietyBand = state.SatietyBand;
        out.HydrationBand = state.HydrationBand;
        return true;
    }

    void SetState(Player* player, double satiety, double hydration)
    {
        if (!player)
            return;

        Config const& cfg = GetConfig();
        if (!cfg.Enable)
            return;

        uint32 const guid = player->GetGUID().GetCounter();
        double const newSatiety = ClampGauge(satiety);
        double const newHydration = ClampGauge(hydration);
        Band satietyBand = Band::Normal;
        Band hydrationBand = Band::Normal;

        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            State& state = g_states[guid];
            state.Satiety = newSatiety;
            state.Hydration = newHydration;
            state.Loaded = true;
            state.Dirty = true;
            state.DamageTimerMs = 0;
            state.RampTicks = 0;
            state.AtOneHpWarned = false;
            state.SatietyBand = RawBand(newSatiety, cfg);
            state.HydrationBand = RawBand(newHydration, cfg);
            satietyBand = state.SatietyBand;
            hydrationBand = state.HydrationBand;
        }

        // A GM jump can land directly inside a warning band without Tick
        // observing the crossing, so emit the corresponding warning once.
        if (cfg.CurrentMode != Mode::Off)
        {
            if (satietyBand >= Band::Warning)
            {
                if (char const* message = BandMessage(false, satietyBand))
                    SendChatLine(player, message);
            }

            if (hydrationBand >= Band::Warning)
            {
                if (char const* message = BandMessage(true, hydrationBand))
                    SendChatLine(player, message);
            }
        }
    }

    void ResetState(Player* player)
    {
        if (!player)
            return;

        Config const& cfg = GetConfig();
        if (!cfg.Enable)
            return;

        uint32 const guid = player->GetGUID().GetCounter();

        std::lock_guard<std::mutex> lock(g_stateMutex);
        State& state = g_states[guid];
        state.Satiety = ClampGauge(cfg.StartingSatiety);
        state.Hydration = ClampGauge(cfg.StartingHydration);
        state.Loaded = true;
        state.Dirty = true;
        state.DamageTimerMs = 0;
        state.RampTicks = 0;
        state.AddonTimerMs = 0;
        state.AtOneHpWarned = false;
        state.SatietyBand = RawBand(state.Satiety, cfg);
        state.HydrationBand = RawBand(state.Hydration, cfg);
    }

    void SendAddonState(Player* player)
    {
        if (!player || !player->GetSession())
            return;

        Config const& cfg = GetConfig();
        if (!cfg.AddonEnable)
            return;

        if (!cfg.Enable)
        {
            // Master switch off: send a synthetic disabled state so the client
            // leaves "Loading..." (loaded=1, mode=0, persistence=0).
            std::string const payload = Acore::StringFormat(
                "{}\t0.0\t0.0\t0\t0\t1\t0\t0\t0", cfg.AddonPrefix);

            WorldPacket data;
            ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, payload);
            player->SendDirectMessage(&data);
            return;
        }

        Snapshot snapshot;
        if (!GetSnapshot(player, snapshot))
            return;

        // An unloaded state (persistence down) is still advertised so the
        // client leaves "Loading..." and shows the placeholder gauges;
        // loaded and persistence both report 0 in that case.
        std::string const payload = Acore::StringFormat(
            "{}\t{:.1f}\t{:.1f}\t{}\t{}\t{}\t{}\t{}\t{}",
            cfg.AddonPrefix,
            snapshot.Satiety,
            snapshot.Hydration,
            uint32(snapshot.CurrentMode),
            snapshot.Lethal ? 1 : 0,
            snapshot.Loaded ? 1 : 0,
            (snapshot.Loaded && snapshot.PersistenceReady) ? 1 : 0,
            uint32(snapshot.SatietyBand),
            uint32(snapshot.HydrationBand));

        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, payload);
        player->SendDirectMessage(&data);
    }

    void SendLoginSummary(Player* player)
    {
        if (!player || !player->GetSession())
            return;

        Config const& cfg = GetConfig();
        if (!cfg.Enable || cfg.CurrentMode == Mode::Off || !cfg.ShowLoginSummary)
            return;

        Snapshot snapshot;
        if (!GetSnapshot(player, snapshot))
            return;

        if (!snapshot.Loaded)
        {
            // Persistence was unavailable when the state was loaded: the
            // gauges stay frozen until the database is back and the player
            // logs in again.
            SendChatLine(player,
                "Persistence unavailable: hunger and thirst are suspended "
                "until the database is restored and you log in again.");
            return;
        }

        ChatHandler handler(player->GetSession());
        handler.PSendSysMessage(
            "|cffffcc00[Hunger & Thirst]|r Mode: {} | Satiety: {:.1f}/100 ({}) | Hydration: {:.1f}/100 ({}).",
            ModeName(snapshot.CurrentMode),
            snapshot.Satiety, BandName(snapshot.SatietyBand, false),
            snapshot.Hydration, BandName(snapshot.HydrationBand, true));

        if (snapshot.SatietyBand >= Band::Critical)
        {
            if (char const* message = BandMessage(false, snapshot.SatietyBand))
                SendChatLine(player, message);
        }

        if (snapshot.HydrationBand >= Band::Critical)
        {
            if (char const* message = BandMessage(true, snapshot.HydrationBand))
                SendChatLine(player, message);
        }

        if (!snapshot.PersistenceReady)
        {
            handler.SendSysMessage(
                "|cffff0000[Hunger & Thirst]|r Persistence unavailable: progress will not be saved.");
        }
    }

    void SendStatus(ChatHandler* handler, Player* player)
    {
        if (!handler)
            return;

        Config const& cfg = GetConfig();
        if (!cfg.Enable)
        {
            handler->SendSysMessage("Hunger & Thirst: disabled by configuration.");
            return;
        }

        if (!player)
        {
            handler->SendSysMessage("Hunger & Thirst: no player selected.");
            return;
        }

        Snapshot snapshot;
        if (!GetSnapshot(player, snapshot))
        {
            handler->PSendSysMessage("Hunger & Thirst: no state loaded for {}.", player->GetName());
            return;
        }

        if (!snapshot.Loaded)
        {
            handler->PSendSysMessage(
                "Hunger & Thirst: the state of {} is not loaded (persistence {}).",
                player->GetName(), snapshot.PersistenceReady ? "ready" : "unavailable");
            return;
        }

        handler->PSendSysMessage(
            "Hunger & Thirst: mode {}, lethal {}, state {}, persistence {}.",
            ModeName(snapshot.CurrentMode),
            snapshot.Lethal ? "yes" : "no",
            snapshot.Loaded ? "loaded" : "not loaded",
            snapshot.PersistenceReady ? "ready" : "unavailable");

        handler->PSendSysMessage(
            "Satiety {:.1f}/100 ({}) | Hydration {:.1f}/100 ({}).",
            snapshot.Satiety, BandName(snapshot.SatietyBand, false),
            snapshot.Hydration, BandName(snapshot.HydrationBand, true));

        if (cfg.CurrentMode == Mode::Off)
            handler->SendSysMessage("Mode off: gauges are not simulated.");
        else if (cfg.CurrentMode == Mode::Advisory)
            handler->SendSysMessage("Advisory: warnings only, no damage.");
        else if (!snapshot.Lethal)
            handler->SendSysMessage("Survival: non-lethal damage (floored at 1 health).");
        else
            handler->SendSysMessage("Survival: lethal damage.");
    }

    void SetPendingDeathCause(DeathCause cause)
    {
        g_pendingDeathCause = cause;
    }

    DeathCause ConsumePendingDeathCause()
    {
        DeathCause const cause = g_pendingDeathCause;
        g_pendingDeathCause = DeathCause::None;
        return cause;
    }

    void LogDeath(Player* player, DeathCause cause, Snapshot const& snapshot)
    {
        if (!player)
            return;

        LOG_INFO("module.hunger-thirst",
            "{} ({}) died from {} at level {} in map {} (zone {}): "
            "satiety {:.1f} ({}), hydration {:.1f} ({}), lethal {}, persistence {}.",
            player->GetName(), player->GetGUID().ToString(), CauseName(cause),
            uint32(player->GetLevel()), player->GetMapId(), player->GetZoneId(),
            snapshot.Satiety, BandName(snapshot.SatietyBand, false),
            snapshot.Hydration, BandName(snapshot.HydrationBand, true),
            snapshot.Lethal ? "on" : "off",
            snapshot.PersistenceReady ? "ready" : "unavailable");
    }

    char const* ModeName(Mode mode)
    {
        switch (mode)
        {
            case Mode::Off:
                return "off";
            case Mode::Advisory:
                return "advisory";
            case Mode::Survival:
                return "survival";
        }

        return "unknown";
    }

    char const* BandName(Band band, bool hydration)
    {
        switch (band)
        {
            case Band::Normal:
                return hydration ? "Hydrated" : "Satiated";
            case Band::Warning:
                return hydration ? "Thirsty" : "Hungry";
            case Band::Critical:
                return hydration ? "Very thirsty" : "Very hungry";
            case Band::Empty:
                return hydration ? "Dehydrated" : "Starving";
        }

        return "unknown";
    }
} // namespace HungerThirst

// =====================================================================
//  WorldScript: configuration and schema bootstrap.
// =====================================================================
class HungerThirstWorldScript : public WorldScript
{
public:
    HungerThirstWorldScript() : WorldScript("HungerThirst_WorldScript",
        {
            WORLDHOOK_ON_AFTER_CONFIG_LOAD,
            WORLDHOOK_ON_STARTUP
        })
    {
    }

    void OnAfterConfigLoad(bool reload) override
    {
        HungerThirst::LoadConfig(reload);
    }

    void OnStartup() override
    {
        HungerThirst::EnsureSchema();
    }
};

// =====================================================================
//  Registration
// =====================================================================
void AddHungerThirstScripts()
{
    new HungerThirstWorldScript();

    HungerThirst::AddHungerThirstPlayerScripts();
    HungerThirst::AddHungerThirstCommandScript();
}
