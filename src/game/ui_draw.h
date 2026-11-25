#pragma once
#include <cstdint>

struct IDirect3DDevice7;
struct IDirectDrawSurface7;

// Drawing of SacredBild's own bits of UI through the device the game's UI draws with (mapped into the canvas like the
// game's own UI draws).
namespace UiDraw
{
    // A quad in the UI's layout units: the texture's uv rect (u0, v0, u1, v1) tinted by `color` (filtered, clamped),
    // or plain `color` without a texture (`surface` and `uv` null). The device's state comes back as it was.
    void quad(IDirect3DDevice7* device, IDirectDrawSurface7* surface, float left, float top, float right, float bottom,
        const float* uv, uint32_t color);
    // A translucent rectangle in the UI's layout units.
    void fillRect(IDirect3DDevice7* device, float left, float top, float right, float bottom, uint32_t color);
    // A panel as SacredBild's (the name field's palette, the lobby list): dark, with a bronze line around it.
    void panel(IDirect3DDevice7* device, float left, float top, float right, float bottom);
}
