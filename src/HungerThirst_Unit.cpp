/*
 * Hunger & Thirst - unit hooks.
 *
 * Eating and drinking are credited through the aura apply hook so the manager
 * can restore satiety/hydration without the module inspecting spells itself.
 * The module no longer causes damage or death, so it needs no death hook.
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
            UNITHOOK_ON_AURA_APPLY
        })
    {
    }

    void OnAuraApply(Unit* unit, Aura* aura) override
    {
        if (unit->IsPlayer())
            CreditConsumable(unit->ToPlayer(), aura);
    }
};
}

void AddHungerThirstUnitScript()
{
    new HungerThirst_UnitScript();
}
}
