/*
 * Hunger & Thirst - player hooks.
 *
 * Bridges the player lifecycle into the HungerThirst manager:
 *   - the state is loaded from the database before the character is added to
 *     the map, and saved periodically, on logout and when the character is
 *     deleted;
 *   - login only sends the summary and the addon payload, never reloads state.
 *
 * The hooks only use the manager API; they never touch the state cache.
 *
 * Released under the MIT license; see LICENSE.
 */

#include "DatabaseEnv.h"
#include "HungerThirst.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "StringFormat.h"

namespace HungerThirst
{
namespace
{
class HungerThirst_PlayerScript : public PlayerScript
{
public:
    HungerThirst_PlayerScript() : PlayerScript("HungerThirst_PlayerScript",
        {
            PLAYERHOOK_ON_LOAD_FROM_DB,
            PLAYERHOOK_ON_LOGIN,
            PLAYERHOOK_ON_UPDATE,
            PLAYERHOOK_ON_SAVE,
            PLAYERHOOK_ON_LOGOUT,
            PLAYERHOOK_ON_DELETE_FROM_DB
        })
    {
    }

    void OnPlayerLoadFromDB(Player* player) override
    {
        LoadState(player);
    }

    void OnPlayerLogin(Player* player) override
    {
        SendLoginSummary(player);
        SendAddonState(player);
    }

    void OnPlayerUpdate(Player* player, uint32 diff) override
    {
        Tick(player, diff);
    }

    void OnPlayerSave(Player* player) override
    {
        SaveState(player, false);
    }

    void OnPlayerLogout(Player* player) override
    {
        SaveState(player, true);
        RemoveState(player);
    }

    void OnPlayerDeleteFromDB(CharacterDatabaseTransaction trans, uint32 guid) override
    {
        RemoveStateByGuid(guid);

        if (!HungerThirst::IsPersistenceReady())
            return;

        trans->Append(Acore::StringFormat("DELETE FROM `hunger_thirst_state` WHERE `guid` = {}", guid));
    }
};
}

// Defined in HungerThirst_Unit.cpp; registers the unit-side hooks.
void AddHungerThirstUnitScript();

void AddHungerThirstPlayerScripts()
{
    new HungerThirst_PlayerScript();
    AddHungerThirstUnitScript();
}
}
