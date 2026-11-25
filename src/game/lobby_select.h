#pragma once
#include <windows.h>

struct IDirect3DDevice7;

namespace Sacred
{
    struct cUI_Manager;
}

// The lobby server of online play (Closed and Open Internet), chosen in the game: the multiplayer menu shows the
// server under its entries, with arrows to switch and a list to pick, add (host[:port]) and remove servers. The list
// and the choice are kept in SacredBild-lobbies.txt next to SacredBild.ini, first filled from Settings.cfg's
// NETWORK_LOBBY and a servers.json in the game folder. When an Internet screen opens, the chosen server takes the
// place of NETWORK_LOBBY / NETWORK_LOBBYPORT (as Sacred Gear's realms do; a realm chosen there wins); the game then
// writes it into Settings.cfg itself at login. A server set in Settings.cfg from outside (another launcher) is taken
// over as the choice.
namespace LobbySelect
{
    // Hooks the network menu's settings; call inside a Patch transaction.
    void install();
    // After the UI manager drew, in its UI scope: the selector on the multiplayer menu.
    void draw(Sacred::cUI_Manager* manager, IDirect3DDevice7* device);
    // From the main window's procedure, before the game's: the selector's clicks and its typing. True: swallow it.
    bool message(UINT message, WPARAM wParam);
}
