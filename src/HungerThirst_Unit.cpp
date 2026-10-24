/*
 * Hunger & Thirst - unit hooks.
 *
 * Eating and drinking are credited through the aura apply hook so the manager
 * can restore satiety/hydration without the module inspecting spells itself.
 * Deaths are only interesting when the manager flagged the pending cause:
 * the snapshot is then handed to the manager for logging.
 *
 * Released under the MIT license; see LICENSE.
 */

#include "HungerThirst.h"
#include "Player.h"
#include "ScriptMgr.h"

namespace HungerThirst
{
namespace
{
class HungerThirst_UnitScript : public UnitScript
{
public:
    HungerThirst_UnitScript() : UnitScript("HungerThirst_UnitScript", true,
        {
            UNITHOOK_ON_AURA_APPLY,
            UNITHOOK_ON_UNIT_DEATH
        })
    {
    }

    void OnAuraApply(Unit* unit, Aura* aura) override
    {
        if (unit->IsPlayer())
            CreditConsumable(unit->ToPlayer(), aura);
    }

    void OnUnitDeath(Unit* unit, Unit* /*killer*/) override
    {
        if (!unit->IsPlayer())
            return;

        Player* player = unit->ToPlayer();
        DeathCause cause = ConsumePendingDeathCause();
        if (cause == DeathCause::None)
            return;

        // A lethal hit inside Spirit of Redemption keeps the player alive even
        // though the death hook runs, so logging must not depend on IsAlive().
        // Any other surviving player is not a death: drop the cause silently.
        if (player->IsAlive()
            && !player->HasSpiritOfRedemptionAura()
            && !player->HasAura(27827))
            return;

        Snapshot snapshot;
        if (GetSnapshot(player, snapshot))
            LogDeath(player, cause, snapshot);
    }
};
}

void AddHungerThirstUnitScript()
{
    new HungerThirst_UnitScript();
}
}
