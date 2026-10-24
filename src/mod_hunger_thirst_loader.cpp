/*
 * mod-hunger-thirst loader.
 *
 * The build derives the loader symbol from the module folder name:
 * "mod-hunger-thirst" becomes "Addmod_hunger_thirstScripts". It must exist and
 * forward to the module's real registration function.
 *
 * Released under the MIT license; see LICENSE.
 */

void AddHungerThirstScripts();

void Addmod_hunger_thirstScripts()
{
    AddHungerThirstScripts();
}
