#pragma once

struct IDirect3DDevice7;

namespace Sacred
{
    struct cUI_Manager;
}

// The menus show the game's version ("2.28 ASE") in the bottom right corner; "SacredBild+" goes under it, in gold.
namespace VersionLabel
{
    // Moves the version up a line; call inside a Patch transaction.
    void install();
    // After the UI manager drew (its version text included), in its UI scope.
    void draw(Sacred::cUI_Manager* manager, IDirect3DDevice7* device);
}
