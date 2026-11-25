#pragma once

struct IDirect3DDevice7;

namespace Sacred
{
    struct cUI_Manager;
}

// The effects on the local hero (buffs, auras, potions, debuffs) as a row of the game's icons on the HUD, with the
// time each has left and its name when the cursor is over it.
namespace EffectsBar
{
    // After the UI manager drew, in its UI scope.
    void draw(Sacred::cUI_Manager* manager, IDirect3DDevice7* device);
}
