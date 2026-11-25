#include "game/effects_bar.h"
#include "config/ui.h"
#include "game/sacred_addr.h"
#include "game/ui_anchor.h"
#include "game/ui_canvas.h"
#include "game/ui_state.h"
#include "sacred/mouse.h"
#include "sacred/object.h"
#include "sacred/render.h"
#include "sacred/text.h"
#include "sacred/ui.h"

#include <windows.h>
#include <ddraw.h>
#include <d3d.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <string>

namespace
{
    using namespace Sacred;
    using W = cUI_Manager::Window;

    // A creature's timed effects (cStats +0x11A, creature +0x4C2): a vector of these, added by the engine's effect
    // function (ENG 00582D50, thiscall on the stats: kind, amount, value, seconds, source, ...), counted down by its
    // tick and dropped at 0. An art or spell adds one per value it changes, all with the same time and source; a
    // spell that hits (a monster's curse, ...) gives its own id as the source too.
    struct Effect
    {
        uint32_t kind;          // what it changes (0x17 and 0x1E: poisoned, the creature's poisoned flag)
        uint32_t amount;
        uint32_t ids[3];        // a vector of object ids (the caster), mostly empty
        float value;
        float left;             // seconds; the lasting ones 1e6
        uint32_t source;        // the art or spell (id in the low word), else 0 or the engine's 0x4321
    };
    static_assert(sizeof(Effect) == 0x20);
    constexpr uintptr_t kEffectList = 0x3A8 + 0x11A;    // creature: the vector's begin and end
    constexpr size_t kMaxEffects = 64;
    constexpr float kLasting = 100000.0f;               // seconds left from which an effect counts as lasting

    constexpr uint32_t kPoisonKinds[] = {0x17, 0x1E};
    constexpr uint32_t kPoisonKey = 0x10000;            // poison without an art or spell behind it
    constexpr char kPoisonTexture[] = "SIM_POISON.TGA"; // the poison sign the game shows over its people
    constexpr uint32_t kPoisonText = 0x439;             // "Poison"

    // The bar in the minimap window's 1024x768 layout: a row left of the hero's portrait (the window's column,
    // 932..1024), growing to the left, a new row below after kPerRow; moved left of (or below) the windows that open
    // there.
    constexpr int kIcon = 30;
    constexpr int kStep = 33;
    constexpr int kPerRow = 8;
    constexpr int kRight = 932 - 6;
    constexpr int kTops[] = {4, 68};                    // under the network game's portrait strip (0,0 896x64)
    constexpr uint16_t kFont = 7;                       // the game's small font (the menus' version)
    constexpr uint32_t kTimeColor = 0xFFF0E6C8;
    constexpr uint32_t kShadow = 0xC0000000;
    constexpr uint32_t kFrameColor = 0xFF6E5A32;
    constexpr uint32_t kPanelColor = 0xE0100C08;
    constexpr uint32_t kNameColor = 0xFFE8D4A0;
    // The windows the bar keeps out of.
    constexpr W kObstacles[] = {W::stats, W::equipment, W::inventory, W::blacksmith, W::merchant, W::master, W::chest,
        W::cube, W::trade, W::netPortraits, W::purchase, W::horse, W::questbook, W::networkInfo};

    struct Group
    {
        uint32_t key;           // the art's or spell's id, or kPoisonKey
        float left;             // the longest time of its effects
        uint32_t image;         // UI image of the icon, 0: texture instead
        uint32_t texture;       // texture handle, when there is no UI image
        uint32_t text;          // the name
    };

    bool readEffects(const cCreature* hero, Effect* out, size_t& count)
    {
        __try
        {
            const auto* range = reinterpret_cast<const uintptr_t*>(reinterpret_cast<const uint8_t*>(hero) + kEffectList);
            const uintptr_t begin = range[0], end = range[1];
            count = 0;
            if (!begin || end <= begin || (end - begin) % sizeof(Effect))
            {
                return true;
            }
            count = std::min((end - begin) / sizeof(Effect), kMaxEffects);
            std::memcpy(out, reinterpret_cast<const void*>(begin), count * sizeof(Effect));
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            // The game thread changed the vector under us: next frame.
            return false;
        }
    }

    // The art (id from 1000) or spell record with this id: its name and the coloured icon (the one the skill slot
    // shows while the ability is ready, the third of its three UI images).
    bool ability(uint32_t id, uint32_t& image, uint32_t& text)
    {
        const bool art = id >= 1000;
        const uint8_t* first = art ? Addr::abilityArts.get() : Addr::abilitySpells.get();
        const uint8_t* end = art ? Addr::abilityArtsEnd.get() : Addr::abilitySpellsEnd.get();
        const size_t stride = art ? 0x78 : 0x94;
        if (!id || id > 0xFFFF || !first || end <= first)
        {
            return false;
        }
        for (const uint8_t* r = first; r + stride <= end; r += stride)
        {
            if (*reinterpret_cast<const uint16_t*>(r) == id)
            {
                const int32_t icon = *reinterpret_cast<const int32_t*>(r + 0x38);
                image = icon > 0 ? static_cast<uint32_t>(icon) : 0;
                text = *reinterpret_cast<const uint32_t*>(r + 4);
                return true;
            }
        }
        return false;
    }

    uint32_t poisonTexture()
    {
        static uint32_t handle = 0;
        static bool looked = false;
        if (!looked && Addr::g_pTextureNames && *Addr::g_pTextureNames && Addr::cTextureNames_find)
        {
            looked = true;
            handle = Addr::cTextureNames_find(*Addr::g_pTextureNames, kPoisonTexture);
        }
        return handle;
    }

    // The effects by what gave them, in the order they came.
    size_t collect(const Effect* effects, size_t count, Group* groups)
    {
        size_t n = 0;
        for (size_t i = 0; i < count; ++i)
        {
            const Effect& e = effects[i];
            Group g{};
            const uint32_t source = e.source & 0xFFFF;
            if (ability(source, g.image, g.text))
            {
                g.key = source;
            }
            else if (std::find(std::begin(kPoisonKinds), std::end(kPoisonKinds), e.kind) != std::end(kPoisonKinds))
            {
                g.key = kPoisonKey;
                g.texture = poisonTexture();
                g.text = kPoisonText;
            }
            else
            {
                continue;   // the engine's own parts of an effect (the rest of an art's, hit reactions)
            }
            const float left = e.left < 0.0f ? kLasting : e.left;
            Group* found = std::find_if(groups, groups + n, [&](const Group& o) { return o.key == g.key; });
            if (found != groups + n)
            {
                found->left = std::max(found->left, left);
                continue;
            }
            g.left = left;
            groups[n++] = g;
        }
        return n;
    }

    // A quad in the UI's layout units, drawn through the device as the UI draws (mapped into the frame): the
    // texture's uv rect tinted by `color`, or plain `color` without a texture.
    void quad(IDirect3DDevice7* device, IDirectDrawSurface7* surface, float left, float top, float right, float bottom,
        const float* uv, uint32_t color)
    {
        struct Vertex
        {
            float x, y, z, rhw;
            DWORD color;
            float u, v;
        };
        const float u0 = uv ? uv[0] : 0.0f, v0 = uv ? uv[1] : 0.0f, u1 = uv ? uv[2] : 0.0f, v1 = uv ? uv[3] : 0.0f;
        const Vertex vertices[4] = {
            {left, top, 0.0f, 1.0f, color, u0, v0}, {right, top, 0.0f, 1.0f, color, u1, v0},
            {left, bottom, 0.0f, 1.0f, color, u0, v1}, {right, bottom, 0.0f, 1.0f, color, u1, v1},
        };
        struct RenderState
        {
            D3DRENDERSTATETYPE state;
            DWORD value, saved;
        };
        RenderState states[] = {
            {D3DRENDERSTATE_ALPHABLENDENABLE, TRUE}, {D3DRENDERSTATE_SRCBLEND, D3DBLEND_SRCALPHA},
            {D3DRENDERSTATE_DESTBLEND, D3DBLEND_INVSRCALPHA}, {D3DRENDERSTATE_ALPHATESTENABLE, FALSE},
            {D3DRENDERSTATE_ZENABLE, FALSE},
        };
        struct StageState
        {
            DWORD stage;
            D3DTEXTURESTAGESTATETYPE type;
            DWORD value, saved;
        };
        const DWORD arg = surface ? D3DTA_TEXTURE : D3DTA_DIFFUSE;
        const DWORD op = surface ? D3DTOP_MODULATE : D3DTOP_SELECTARG1;
        StageState stages[] = {
            {0, D3DTSS_COLOROP, op}, {0, D3DTSS_COLORARG1, arg}, {0, D3DTSS_COLORARG2, D3DTA_DIFFUSE},
            {0, D3DTSS_ALPHAOP, op}, {0, D3DTSS_ALPHAARG1, arg}, {0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE},
            {0, D3DTSS_MINFILTER, D3DTFN_LINEAR}, {0, D3DTSS_MAGFILTER, D3DTFG_LINEAR},
            {0, D3DTSS_ADDRESS, D3DTADDRESS_CLAMP}, {1, D3DTSS_COLOROP, D3DTOP_DISABLE},
        };
        IDirectDrawSurface7* texture = nullptr;
        device->GetTexture(0, &texture);
        for (RenderState& s : states)
        {
            device->GetRenderState(s.state, &s.saved);
            device->SetRenderState(s.state, s.value);
        }
        for (StageState& s : stages)
        {
            device->GetTextureStageState(s.stage, s.type, &s.saved);
            device->SetTextureStageState(s.stage, s.type, s.value);
        }
        device->SetTexture(0, surface);
        device->DrawPrimitive(D3DPT_TRIANGLESTRIP, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1,
            const_cast<Vertex*>(vertices), 4, 0);
        device->SetTexture(0, texture);
        if (texture)
        {
            texture->Release();
        }
        for (const StageState& s : stages)
        {
            device->SetTextureStageState(s.stage, s.type, s.saved);
        }
        for (const RenderState& s : states)
        {
            device->SetRenderState(s.state, s.saved);
        }
    }

    // The group's icon: its UI image (a part of a 256x256 sheet) or its whole texture.
    bool drawIcon(IDirect3DDevice7* device, const Group& g, float x, float y)
    {
        cTextureManager* textures = cTextureManager::instance();
        if (!textures)
        {
            return false;
        }
        float uv[4] = {0.0f, 0.0f, 1.0f, 1.0f};
        uint32_t handle = g.texture;
        if (g.image)
        {
            if (!Addr::uiImageTextures)
            {
                return false;
            }
            const uint8_t* record = reinterpret_cast<const uint8_t*>(Addr::uiImageTextures.get()) - 0x28 + g.image * 0x54;
            handle = *reinterpret_cast<const uint32_t*>(record + 0x28);
            std::memcpy(uv, record + 0x2C, sizeof(uv));
            // Texture coordinates once the UI has started (0x761580), pixels of the sheet before; pulled in by half a
            // texel so the neighbouring images of the sheet don't bleed in.
            const float scale = uv[2] > 1.5f || uv[3] > 1.5f ? 1.0f / 256.0f : 1.0f;
            const float inset = 0.5f / 256.0f;
            uv[0] = uv[0] * scale + inset;
            uv[1] = uv[1] * scale + inset;
            uv[2] = uv[2] * scale - inset;
            uv[3] = uv[3] * scale - inset;
        }
        cTexture* texture = handle ? textures->get(handle) : nullptr;
        if (!texture || !texture->surface)
        {
            return false;
        }
        quad(device, texture->surface, x, y, x + kIcon, y + kIcon, uv, 0xFFFFFFFF);
        return true;
    }

    // "12s", "1:05"; nothing for a lasting effect.
    bool timeText(float left, wchar_t (&out)[16])
    {
        if (left >= kLasting)
        {
            return false;
        }
        const int seconds = static_cast<int>(std::ceil(std::max(left, 0.0f)));
        if (seconds >= 60)
        {
            std::swprintf(out, std::size(out), L"%d:%02d", seconds / 60, seconds % 60);
        }
        else
        {
            std::swprintf(out, std::size(out), L"%ds", seconds);
        }
        return true;
    }

    std::wstring text(uint32_t id)
    {
        cTextResources* texts = cTextResources::instance();
        const wchar_t* const* range = texts ? texts->text(id) : nullptr;
        return range && range[0] && range[1] > range[0] ? std::wstring(range[0], range[1]) : std::wstring();
    }

    struct Rect
    {
        float left, top, right, bottom;
        bool overlaps(const Rect& o) const { return left < o.right && o.left < right && top < o.bottom && o.top < bottom; }
    };

    // The shown windows the bar keeps out of, in the bar's frame.
    size_t obstacles(const UiCanvas::Frame& frame, Rect* out)
    {
        size_t n = 0;
        for (W w : kObstacles)
        {
            cUI_Window2* window = UiState::shown(w) ? UiState::window(w) : nullptr;
            if (!window)
            {
                continue;
            }
            UiCanvas::Frame at{};
            UiAnchor::frame(window, at);
            const float dx = at.x - frame.x, dy = at.y - frame.y;
            out[n++] = {window->x + dx, window->y + dy, window->x + window->width + dx, window->y + window->height + dy};
        }
        return n;
    }

    // Where the bar's right edge and top go: next to the portrait, else left of the windows open there, at the
    // first height that has room; false if none does.
    bool place(const UiCanvas::Frame& frame, int width, int height, float& right, float& top)
    {
        Rect blocked[std::size(kObstacles)];
        const size_t count = obstacles(frame, blocked);
        for (int y : kTops)
        {
            right = static_cast<float>(kRight);
            top = static_cast<float>(y);
            for (size_t guard = 0; guard <= count; ++guard)
            {
                const Rect bar{right - width, top, right, top + height};
                const Rect* hit = std::find_if(blocked, blocked + count, [&](const Rect& r) { return r.overlaps(bar); });
                if (hit == blocked + count)
                {
                    return bar.left >= 0.0f;
                }
                right = hit->left - 4.0f;
                if (right - width < 0.0f)
                {
                    break;
                }
            }
        }
        return false;
    }
}

void EffectsBar::draw(cUI_Manager* manager, IDirect3DDevice7* device)
{
    if (!Config::ui.effectsBar || !manager || !(manager->flags & cUI_Manager::inGame) || !device ||
        UiState::mode() != UiState::Mode::Game || UiState::exclusiveWindow() || UiState::escapeMenu())
    {
        return;
    }
    cObjectManager* objects = cObjectManager::instance();
    cCreature* hero = objects ? objects->hero() : nullptr;
    cUI_Window2* minimap = UiState::window(W::minimap);
    cFontManager* fonts = cFontManager::instance();
    if (!hero || !minimap || !fonts || fonts->fonts.size() <= kFont || !fonts->fonts[kFont])
    {
        return;
    }
    Effect effects[kMaxEffects];
    size_t count = 0;
    Group groups[kMaxEffects];
    if (!readEffects(hero, effects, count))
    {
        return;
    }
    const size_t n = collect(effects, count, groups);
    if (!n)
    {
        return;
    }
    cFont* font = fonts->fonts[kFont];
    const int lineHeight = font->lineHeight(L'0');
    const int rows = static_cast<int>((n + kPerRow - 1) / kPerRow);
    const int rowHeight = kIcon + lineHeight + 3;
    const int width = static_cast<int>(std::min<size_t>(n, kPerRow)) * kStep - (kStep - kIcon);

    UiCanvas::Frame frame{};
    UiAnchor::frame(minimap, frame);
    UiCanvas::FrameScope scope{frame};
    float right = 0.0f, top = 0.0f;
    if (!place(frame, width, rows * rowHeight, right, top))
    {
        return;
    }
    const cMouse* mouse = cMouse::instance();
    const float mx = mouse->x - frame.x, my = mouse->y - frame.y;
    const Group* hovered = nullptr;
    float hoverX = 0.0f;
    for (size_t i = 0; i < n; ++i)
    {
        const Group& g = groups[i];
        const float x = right - kIcon - static_cast<float>(i % kPerRow) * kStep;
        const float y = top + static_cast<float>(i / kPerRow) * rowHeight;
        quad(device, nullptr, x - 1, y - 1, x + kIcon + 1, y + kIcon + 1, nullptr, kFrameColor);
        if (!drawIcon(device, g, x, y))
        {
            quad(device, nullptr, x, y, x + kIcon, y + kIcon, nullptr, kPanelColor);
        }
        wchar_t time[16];
        if (timeText(g.left, time))
        {
            const int x0 = static_cast<int>(x) + (kIcon - font->textWidth(time)) / 2;
            const int y0 = static_cast<int>(y) + kIcon + 1;
            font->draw(device, UiRect{x0 + 1, y0 + 1, 64, 32}, time, kShadow, 0);
            font->draw(device, UiRect{x0, y0, 64, 32}, time, kTimeColor, 0);
        }
        if (mx >= x && mx < x + kIcon && my >= y && my < y + kIcon)
        {
            hovered = &g;
            hoverX = x;
        }
    }
    if (!hovered)
    {
        return;
    }
    // The name under the bar, in a box like the game's tooltips.
    const std::wstring name = text(hovered->text);
    if (name.empty())
    {
        return;
    }
    const int textWidth = font->textWidth(name.c_str());
    const float boxWidth = static_cast<float>(textWidth + 12), boxHeight = static_cast<float>(lineHeight + 8);
    const float bx = std::clamp(hoverX + kIcon / 2.0f - boxWidth / 2.0f, 2.0f, 1022.0f - boxWidth);
    const float by = top + static_cast<float>(rows) * rowHeight + 2.0f;
    quad(device, nullptr, bx - 1, by - 1, bx + boxWidth + 1, by + boxHeight + 1, nullptr, kFrameColor);
    quad(device, nullptr, bx, by, bx + boxWidth, by + boxHeight, nullptr, kPanelColor);
    font->draw(device, UiRect{static_cast<int32_t>(bx) + 6, static_cast<int32_t>(by) + 4, static_cast<int16_t>(textWidth + 8),
        static_cast<int16_t>(lineHeight + 4)}, name.c_str(), kNameColor, 0);
}
