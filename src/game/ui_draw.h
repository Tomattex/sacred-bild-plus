#pragma once
#include <cstdint>

struct IDirect3DDevice7;

// Drawing of SacredBild's own bits of UI through the device the game's UI draws with (mapped into the canvas like the
// game's own UI draws).
namespace UiDraw
{
    // A translucent rectangle in the UI's layout units; the device's state comes back as it was.
    void fillRect(IDirect3DDevice7* device, float left, float top, float right, float bottom, uint32_t color);
    // A panel as SacredBild's (the name field's palette, the lobby list): dark, with a bronze line around it.
    void panel(IDirect3DDevice7* device, float left, float top, float right, float bottom);
}
