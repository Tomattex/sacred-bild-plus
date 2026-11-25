#include "game/version_label.h"
#include "game/sacred_addr.h"
#include "log.h"
#include "patch.h"
#include "sacred/ui.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>

namespace
{
    using namespace Sacred;

    constexpr uint16_t kVersionFont = 7;    // the game's for the version
    constexpr uint16_t kLabelFont = 1;      // the menu entries' typeface, smaller
    constexpr wchar_t kLabel[] = L"SacredBild+";
    constexpr int32_t kRight = 1024 - 6;    // the label's right end in the 1024x768 layout
    constexpr int32_t kBottom = 768 - 2;
    constexpr uint32_t kShadow = 0x99000000;
    // The shades of the menu's gold lettering, pale gold to bronze and back: they run across the label, a full turn
    // every kPeriodMs, the label showing a bit less than one turn at a time.
    constexpr uint32_t kStops[] = {0xFFFFF1C9, 0xFFE8B84A, 0xFFA8641E, 0xFFE8B84A};
    constexpr uint32_t kPeriodMs = 2400;
    constexpr float kSpan = 0.75f;

    constexpr size_t kCount = std::size(kLabel) - 1;

    // Placed once the fonts can be measured: the label's top left and where each of its characters starts.
    bool g_placed = false;
    int32_t g_left = 0, g_top = 0;
    int32_t g_x[kCount] = {};

    uint32_t lerp(uint32_t a, uint32_t b, float t)
    {
        uint32_t out = 0;
        for (int shift = 0; shift < 32; shift += 8)
        {
            const float ca = static_cast<float>((a >> shift) & 0xFF), cb = static_cast<float>((b >> shift) & 0xFF);
            out |= static_cast<uint32_t>(ca + (cb - ca) * t + 0.5f) << shift;
        }
        return out;
    }

    // Character i of `count` at `phase` (0..1) of the turn.
    uint32_t gradient(size_t i, size_t count, float phase)
    {
        float t = static_cast<float>(i) / static_cast<float>(count) * kSpan - phase;
        t -= std::floor(t);
        const float scaled = t * std::size(kStops);
        const size_t stop = static_cast<size_t>(scaled) % std::size(kStops);
        return lerp(kStops[stop], kStops[(stop + 1) % std::size(kStops)], scaled - std::floor(scaled));
    }

    // Puts the label in the bottom right corner and the version above it, both starting at the label's left.
    void place(cFont* versionFont, cFont* labelFont)
    {
        g_placed = true;
        const int32_t width = labelFont->textWidth(kLabel);
        const int32_t labelHeight = labelFont->lineHeight(L'S'), versionHeight = versionFont->lineHeight(L'2');
        g_left = kRight - width;
        g_top = kBottom - labelHeight;
        for (size_t i = 1; i < kCount; ++i)
        {
            wchar_t prefix[kCount + 1] = {};
            std::copy_n(kLabel, i, prefix);
            g_x[i] = labelFont->textWidth(prefix);
        }
        const uintptr_t at = Addr::menuVersionPosition;
        if (!Patch::imm32(at + 4, 0x3C9, static_cast<uint32_t>(g_left)) ||
            !Patch::imm32(at + 12, 0x2F0, static_cast<uint32_t>(g_top - versionHeight)))
        {
            LOG("Version label: the version's position was not found");
        }
    }
}

void VersionLabel::install()
{
    if (!Addr::menuVersionPosition)
    {
        g_placed = true;    // no label without a place for it
    }
}

void VersionLabel::draw(cUI_Manager* manager, IDirect3DDevice7* device)
{
    if (!manager || !(manager->flags & cUI_Manager::inMenus) || !device)
    {
        return;
    }
    cFontManager* fonts = cFontManager::instance();
    if (!fonts || fonts->fonts.size() <= std::max(kVersionFont, kLabelFont))
    {
        return;
    }
    cFont* versionFont = fonts->fonts[kVersionFont];
    cFont* labelFont = fonts->fonts[kLabelFont];
    if (!versionFont || !labelFont)
    {
        return;
    }
    if (!g_placed)
    {
        place(versionFont, labelFont);
    }
    if (!g_left)
    {
        return;
    }
    // A character at a time, each in its colour of the gradient, over a soft shadow.
    const float phase = static_cast<float>(GetTickCount() % kPeriodMs) / static_cast<float>(kPeriodMs);
    wchar_t one[2] = {};
    for (int pass = 0; pass < 2; ++pass)
    {
        const int32_t offset = pass == 0 ? 1 : 0;
        for (size_t i = 0; i < kCount; ++i)
        {
            one[0] = kLabel[i];
            labelFont->draw(device, UiRect{g_left + g_x[i] + offset, g_top + offset, 64, 64}, one,
                pass == 0 ? kShadow : gradient(i, kCount, phase), 0);
        }
    }
}
