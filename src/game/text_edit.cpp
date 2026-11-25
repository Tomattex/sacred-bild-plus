#include "game/text_edit.h"
#include "game/focus.h"
#include "game/sacred_addr.h"
#include "game/ui_canvas.h"
#include "game/ui_windows.h"
#include "log.h"
#include "patch.h"
#include "sacred/mouse.h"
#include "sacred/ui.h"

#include <ddraw.h>
#include <d3d.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <iterator>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace
{
    using namespace Sacred;

    decltype(Addr::cUI_EditControl_receiveEvent)::Ptr g_origReceiveEvent = nullptr;
    decltype(Addr::cUI_EditControl_render)::Ptr g_origRender = nullptr;
    using FontLookup = cFont*(__fastcall*)(void* fonts, void* edx, uint32_t id);
    FontLookup g_fontLookup = nullptr;

    // The keys a text field never gets from the game (the window procedures post nothing for them) reach it as
    // characters of the private use area: kKey + the virtual key, plus kShift / kCtrl.
    constexpr wchar_t kKey = 0xE000, kShift = 0x100, kCtrl = 0x200, kKeyEnd = kKey + 0x400;
    constexpr DWORD kBlinkMs = 300;                 // the game's caret blink
    constexpr uint32_t kSelectionColor = 0x803399FF;
    constexpr size_t kMaxLength = 255;              // past it the field's buffer is always zero (setText's padding)
    constexpr DWORD kShownMs = 250;                 // a field drawn this recently is on the screen

    // The palette of the character name field (right click): colours and some of the UI's sprites as the font's codes.
    constexpr uint32_t kColors[] = {
        0xFFFFFFFF, 0xFFA0A0A0, 0xFFE03030, 0xFFFF8C1A, 0xFFFFD34D, 0xFFFFFF66,
        0xFF4CD94C, 0xFF33CCB3, 0xFF4DB8FF, 0xFF3366FF, 0xFFB366FF, 0xFFFF66CC,
    };
    constexpr uint16_t kIcons[] = {127, 218, 206, 207, 208, 406, 432, 421, 120, 121, 122, 123, 412, 413, 418, 176};
    constexpr int kCell = 20, kPad = 5;
    constexpr uint32_t kPanelColor = 0xEC140F0A, kBorderColor = 0xFF8C6A3A, kHoverColor = 0x40FFFFFF;
    constexpr uint32_t kDimColor = 0xC8141210, kCountColor = 0xFFB8B0A0, kFullColor = 0xFFE0A040;

    // The caret and selection of the field that has the keyboard focus. Its events (characters, clicks) run on the
    // thread that hands the UI its events: the window's in the menus, the engine's render thread in game. Its render
    // runs on the UI's drawing thread, which in the menus is another one.
    struct Field
    {
        cUI_EditControl* control = nullptr;
        std::wstring text;          // the field's text when we last saw it: anything else was set behind our back
        size_t caret = 0;
        size_t anchor = 0;          // the selection lies between anchor and caret
        size_t scroll = 0;          // the first character drawn
        bool click = false;         // a click at clickX (layout units, the field's frame) to place the caret
        bool extend = false;        // ... with Shift: the selection grows to it
        int clickX = 0;
        bool dragging = false;      // since the click, while the left button is held: the caret follows the cursor
        DWORD blinkFrom = 0;
        bool palette = false;       // the palette is open (the name field only)
    };
    std::mutex g_lock;
    Field g_field;

    // The palette as last drawn, in canvas units (cMouse's): its rect and its cells. What a cell inserts: a colour
    // (kind Color), the field's own colour again (Reset) or a sprite (Icon).
    struct Cell
    {
        enum Kind { Color, Reset, Icon } kind;
        uint32_t value;
        float x, y;
    };
    struct Palette
    {
        bool shown = false;
        float x = 0, y = 0, w = 0, h = 0;
        std::vector<Cell> cells;
    };
    Palette g_palette;
    float g_nameRect[4] = {};       // the name field, in canvas units, as last drawn
    // The name field drawn this UI frame with its palette open: the palette is drawn over the rest of the UI.
    bool g_paletteWanted = false;
    cFont* g_paletteFont = nullptr;
    uint32_t g_paletteTextColor = 0;
    size_t g_paletteLimit = 0;
    bool g_swallowUp[2] = {};       // the palette had this button's (left, right) down: its up too
    bool g_swallowEscape = false;   // Esc closed the palette: its character and key up go nowhere

    // Set around the original render of a field we draw the text of: its font lookup finds no font, so it draws the
    // frame only.
    thread_local bool t_hideText = false;

    // The focused field we drew last, and when; and the character name field (cUI_Character's) when it is that one.
    std::atomic<const void*> g_drawn{nullptr};
    std::atomic<DWORD> g_drawnAt{0};
    std::atomic<const void*> g_nameField{nullptr};

    // A text field that takes typing and draws its text from the left: shown, focused, enabled, not read-only, the
    // plain class (not cUI_DigitEdit), no other alignment than the game's own.
    bool editable(const cUI_Control2* control)
    {
        constexpr uint32_t kNeeded = cUI_Control2::visibleFlag | cUI_Control2::enabledFlag | cUI_Control2::focusFlag;
        if (!control || reinterpret_cast<uintptr_t>(control->vtable->render) != Addr::cUI_EditControl_render ||
            (control->flags & kNeeded) != kNeeded || (control->flags & cUI_EditControl::readOnly))
        {
            return false;
        }
        // 0x4000 only matters for centered text (the game sets it when the field loses the focus).
        return (static_cast<const cUI_EditControl*>(control)->fontFlags & ~0x4000u) == 0;
    }

    // The editable field that has the keyboard focus, if it is on the screen: the game leaves the focus on the field of
    // a window it closed, and its windows' flags don't tell (the lobby's parent screen is hidden while it shows).
    cUI_EditControl* focusedField()
    {
        cUI_Manager* manager = cUI_Manager::instance();
        cUI_Control2* control = manager ? manager->focus : nullptr;
        if (!control || control != g_drawn.load() || GetTickCount() - g_drawnAt.load() > kShownMs || !editable(control))
        {
            return nullptr;
        }
        return static_cast<cUI_EditControl*>(control);
    }

    // Under g_lock. A field we haven't seen, or whose text changed behind our back (the game set it, the IME typed):
    // the caret at the end, as the game has it.
    void sync(cUI_EditControl* control)
    {
        const size_t length = wcsnlen(control->text, std::size(control->text));
        if (control == g_field.control && g_field.text.compare(0, std::wstring::npos, control->text, length) == 0)
        {
            return;
        }
        g_field = Field{};
        g_field.control = control;
        g_field.text.assign(control->text, length);
        g_field.caret = g_field.anchor = length;
        g_field.blinkFrom = GetTickCount();
    }

    size_t selectionStart() { return std::min(g_field.caret, g_field.anchor); }
    size_t selectionEnd() { return std::max(g_field.caret, g_field.anchor); }

    // The font's codes, each edited as one character (cFontTTF2 draw ENG 0064E9C0, checked by 00650C30, read by
    // 00650AA0): \cAARRGGBB a colour, \r the text's own colour again, \gPPPPPPPPIIIIIIII sprite I of the UI's table,
    // \w and \d with 8 digits, \n; the digits are hex or spaces. The length of the code or character at i.
    size_t codeLength(const std::wstring& t, size_t i)
    {
        if (i + 1 >= t.size() || t[i] != L'\\')
        {
            return 1;
        }
        size_t length = 0;
        switch (t[i + 1])
        {
        case L'c': case L'w': case L'd': length = 10; break;
        case L'g': length = 18; break;
        case L'r': case L'n': return 2;
        default: return 1;
        }
        if (i + length > t.size())
        {
            return 1;
        }
        for (size_t k = i + 2; k < i + length; ++k)
        {
            if (!std::iswxdigit(t[k]) && t[k] != L' ')
            {
                return 1;
            }
        }
        return length;
    }

    // The stops of the text (where its characters and codes start, and its end) around i.
    size_t nextStop(size_t i)
    {
        const std::wstring& t = g_field.text;
        return i < t.size() ? i + codeLength(t, i) : t.size();
    }

    size_t stopAtOrBefore(size_t i)
    {
        size_t at = 0;
        while (at < g_field.text.size() && nextStop(at) <= i)
        {
            at = nextStop(at);
        }
        return at;
    }

    size_t stopAtOrAfter(size_t i)
    {
        const size_t before = stopAtOrBefore(i);
        return before == i ? i : nextStop(before);
    }

    size_t prevStop(size_t i)
    {
        return i ? stopAtOrBefore(i - 1) : 0;
    }

    void moveCaret(size_t to, bool extend)
    {
        g_field.caret = std::min(to, g_field.text.size());
        if (!extend)
        {
            g_field.anchor = g_field.caret;
        }
        g_field.blinkFrom = GetTickCount();
    }

    bool wordChar(wchar_t ch) { return std::iswalnum(ch) || ch == L'_'; }

    size_t wordLeft(size_t i)
    {
        const std::wstring& t = g_field.text;
        while (i > 0 && !wordChar(t[i - 1])) --i;
        while (i > 0 && wordChar(t[i - 1])) --i;
        return i;
    }

    size_t wordRight(size_t i)
    {
        const std::wstring& t = g_field.text;
        while (i < t.size() && wordChar(t[i])) ++i;
        while (i < t.size() && !wordChar(t[i])) ++i;
        return i;
    }

    // Writes the text into the field. Stays NUL-terminated throughout for a render reading it meanwhile.
    void store(cUI_EditControl* control, std::wstring text)
    {
        text.resize(std::min(text.size(), kMaxLength));
        for (size_t i = 0; i < text.size(); ++i)
        {
            control->text[i] = text[i];
        }
        control->text[text.size()] = 0;
        g_field.text = std::move(text);
    }

    // Replaces the selection with `typed`, filtered as the game filters typing and cut at the field's length.
    void replaceSelection(cUI_EditControl* control, std::wstring_view typed)
    {
        const size_t from = selectionStart(), to = selectionEnd();
        const size_t limit = std::min<size_t>(control->maxLength, kMaxLength);
        const size_t kept = g_field.text.size() - (to - from);
        std::wstring add;
        for (wchar_t ch : typed)
        {
            if (kept + add.size() >= limit)
            {
                break;
            }
            if (ch < 0x20 || ch == 0x7F || (ch >= kKey && ch < kKeyEnd) || (control->noDot && ch == L'.') ||
                (control->noSlash && (ch == L'\\' || ch == L'/')))
            {
                continue;
            }
            add += (control->flags & cUI_EditControl::upperCase) ? static_cast<wchar_t>(std::towupper(ch)) : ch;
        }
        store(control, g_field.text.substr(0, from) + add + g_field.text.substr(to));
        moveCaret(from + add.size(), false);
    }

    // Deletes the selection, or from the caret to `to` when there is none.
    void erase(cUI_EditControl* control, size_t to)
    {
        if (g_field.caret == g_field.anchor)
        {
            g_field.anchor = to;
        }
        replaceSelection(control, {});
    }

    void copySelection(const cUI_EditControl* control)
    {
        const size_t from = selectionStart(), to = selectionEnd();
        if (from == to || (control->flags & cUI_EditControl::password))
        {
            return;
        }
        const size_t bytes = (to - from + 1) * sizeof(wchar_t);
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
        auto* out = memory ? static_cast<wchar_t*>(GlobalLock(memory)) : nullptr;
        if (!out)
        {
            return;
        }
        g_field.text.copy(out, to - from, from);
        out[to - from] = 0;
        GlobalUnlock(memory);
        bool ok = false;
        if (OpenClipboard(Focus::window()))
        {
            EmptyClipboard();
            ok = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;   // the clipboard owns the memory after this
            CloseClipboard();
        }
        if (!ok)
        {
            GlobalFree(memory);
        }
    }

    // The clipboard's text with line breaks and tabs as single spaces (a field is one line).
    std::wstring clipboardText()
    {
        std::wstring text;
        if (!OpenClipboard(Focus::window()))
        {
            return text;
        }
        if (HANDLE data = GetClipboardData(CF_UNICODETEXT))
        {
            if (const auto* wide = static_cast<const wchar_t*>(GlobalLock(data)))
            {
                for (const wchar_t* p = wide; *p && text.size() <= kMaxLength; ++p)
                {
                    const bool space = *p == L'\r' || *p == L'\n' || *p == L'\t';
                    if (!space)
                    {
                        text += *p;
                    }
                    else if (text.empty() || text.back() != L' ')
                    {
                        text += L' ';
                    }
                }
                GlobalUnlock(data);
            }
        }
        CloseClipboard();
        return text;
    }

    // One of our keys (kKey + virtual key + modifiers).
    void editKey(cUI_EditControl* control, wchar_t code)
    {
        const bool shift = code & kShift, ctrl = code & kCtrl;
        const size_t length = g_field.text.size();
        switch (code & 0xFF)
        {
        case VK_LEFT:
            if (!shift && !ctrl && g_field.caret != g_field.anchor)
            {
                moveCaret(selectionStart(), false);
            }
            else
            {
                moveCaret(ctrl ? stopAtOrBefore(wordLeft(g_field.caret)) : prevStop(g_field.caret), shift);
            }
            break;
        case VK_RIGHT:
            if (!shift && !ctrl && g_field.caret != g_field.anchor)
            {
                moveCaret(selectionEnd(), false);
            }
            else
            {
                moveCaret(ctrl ? stopAtOrAfter(wordRight(g_field.caret)) : nextStop(g_field.caret), shift);
            }
            break;
        case VK_HOME:
            moveCaret(0, shift);
            break;
        case VK_END:
            moveCaret(length, shift);
            break;
        case VK_DELETE:
            erase(control, ctrl ? stopAtOrAfter(wordRight(g_field.caret)) : nextStop(g_field.caret));
            break;
        default:
            break;
        }
    }

    // A typed character: true if it is handled here, false for the game (Enter, Esc, Tab, and what it leaves to the
    // window around the field).
    bool typeCharacter(cUI_EditControl* control, const cEventKey& key)
    {
        const wchar_t ch = key.ch;
        switch (ch)
        {
        case 0x01:                  // Ctrl+A
            g_field.anchor = 0;
            moveCaret(g_field.text.size(), true);
            return true;
        case 0x03:                  // Ctrl+C
            copySelection(control);
            return true;
        case 0x18:                  // Ctrl+X
            if (!(control->flags & cUI_EditControl::password))
            {
                copySelection(control);
                replaceSelection(control, {});
            }
            return true;
        case 0x16:                  // Ctrl+V
            if (const std::wstring text = clipboardText(); !text.empty())
            {
                replaceSelection(control, text);
            }
            return true;
        case 0x08:                  // Backspace
            erase(control, prevStop(g_field.caret));
            return true;
        case 0x7F:                  // Ctrl+Backspace
            erase(control, stopAtOrBefore(wordLeft(g_field.caret)));
            return true;
        default:
            break;
        }
        // The game's: control characters, its Up / Down in game (0x100 / 0x101), '.' in a field without dots and any
        // character with modifiers 0x40 are not taken (the window gets them).
        if (ch < 0x20 || (ch >= 0x100 && ch <= 0x103) || (control->noDot && ch == L'.') || key.modifiers == 0x40)
        {
            return false;
        }
        replaceSelection(control, std::wstring_view(&ch, 1));
        return true;
    }

    uint32_t __fastcall hookReceiveEvent(cUI_EditControl* self, void* edx, cEvent* event)
    {
        cEventKey* key = cEventKey::of(event);
        if (key && (key->flags & cEventKey::typed))
        {
            if (editable(self))
            {
                std::scoped_lock lock(g_lock);
                sync(self);
                if (key->ch >= kKey && key->ch < kKeyEnd)
                {
                    editKey(self, key->ch);
                    return 1;
                }
                if (typeCharacter(self, *key))
                {
                    return 1;
                }
            }
        }
        const uint32_t handled = g_origReceiveEvent(self, edx, event);
        // A click into the field (it has the focus now): the caret goes there when the field is drawn next.
        const cEventMouse* mouse = cEventMouse::of(event);
        if (mouse && mouse->vtable == Addr::cEventMouseDown_vtable && editable(self) &&
            self->vtable->isInside(self, nullptr, mouse->x, mouse->y))
        {
            std::scoped_lock lock(g_lock);
            sync(self);
            g_field.click = true;
            g_field.extend = Focus::keyDown(VK_SHIFT);
            g_field.clickX = mouse->x;
        }
        return handled;
    }

    cFont* __fastcall fontForText(void* fonts, void* edx, uint32_t id)
    {
        return t_hideText ? nullptr : g_fontLookup(fonts, edx, id);
    }

    // A translucent rectangle in the UI's layout units, through the device as the UI draws (mapped into the canvas).
    void fillRect(IDirect3DDevice7* device, float left, float top, float right, float bottom, uint32_t color)
    {
        struct Vertex
        {
            float x, y, z, rhw;
            DWORD color;
        };
        const Vertex quad[4] = {
            {left, top, 0.0f, 1.0f, color}, {right, top, 0.0f, 1.0f, color},
            {left, bottom, 0.0f, 1.0f, color}, {right, bottom, 0.0f, 1.0f, color},
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
        StageState stages[] = {
            {0, D3DTSS_COLOROP, D3DTOP_SELECTARG1}, {0, D3DTSS_COLORARG1, D3DTA_DIFFUSE},
            {0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1}, {0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE},
            {1, D3DTSS_COLOROP, D3DTOP_DISABLE},
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
        device->SetTexture(0, nullptr);
        device->DrawPrimitive(D3DPT_TRIANGLESTRIP, D3DFVF_XYZRHW | D3DFVF_DIFFUSE, const_cast<Vertex*>(quad), 4, 0);
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

    struct Applied;
    Applied apply(const Cell& cell);
    size_t appliedLength(const Cell& cell);

    // The palette under (or over) the name field (`box`, canvas units): a row of colours and the field's own, a row of
    // sprites; the cell under the cursor lit. Its cells are kept for the window procedure's clicks.
    void drawPalette(uint32_t textColor, cFont* font, IDirect3DDevice7* device, const UiRect& box, size_t limit)
    {
        const int width = 2 * kPad + static_cast<int>(std::size(kIcons)) * kCell;
        const int height = 3 * kPad + 2 * kCell;
        const int x = std::clamp<int>(box.x, 0, 1024 - width);
        int y = box.y + box.height + 3;
        if (y + height > 768)
        {
            y = box.y - height - 3;
        }
        const UiCanvas::Frame frame = UiCanvas::frame();
        const cMouse* mouse = cMouse::instance();
        const float mx = mouse->x - frame.x, my = mouse->y - frame.y;
        const auto fx = [](int v) { return static_cast<float>(v); };

        fillRect(device, fx(x - 1), fx(y - 1), fx(x + width + 1), fx(y + height + 1), kBorderColor);
        fillRect(device, fx(x), fx(y), fx(x + width), fx(y + height), kPanelColor);
        Palette p;
        p.shown = true;
        p.x = x + frame.x;
        p.y = y + frame.y;
        p.w = static_cast<float>(width);
        p.h = static_cast<float>(height);
        // A cell; false if what it does no longer fits the field (it is drawn dimmed).
        const auto cell = [&](Cell::Kind kind, uint32_t value, int cx, int cy) {
            const Cell made{kind, value, cx + frame.x, cy + frame.y};
            const bool fits = appliedLength(made) <= limit;
            if (fits && mx >= cx && mx < cx + kCell && my >= cy && my < cy + kCell)
            {
                fillRect(device, fx(cx), fx(cy), fx(cx + kCell), fx(cy + kCell), kHoverColor);
            }
            p.cells.push_back(made);
            return fits;
        };
        const auto dim = [&](int cx, int cy) {
            fillRect(device, fx(cx), fx(cy), fx(cx + kCell), fx(cy + kCell), kDimColor);
        };
        int cx = x + kPad, cy = y + kPad;
        for (uint32_t color : kColors)
        {
            const bool fits = cell(Cell::Color, color, cx, cy);
            fillRect(device, fx(cx + 3), fx(cy + 3), fx(cx + kCell - 3), fx(cy + kCell - 3), 0xFF000000);
            fillRect(device, fx(cx + 4), fx(cy + 4), fx(cx + kCell - 4), fx(cy + kCell - 4), color);
            if (!fits)
            {
                dim(cx, cy);
            }
            cx += kCell;
        }
        const bool resetFits = cell(Cell::Reset, 0, cx, cy);
        font->draw(device, UiRect{cx + 5, cy + 1, kCell, kCell}, L"A", textColor, 0);
        if (!resetFits)
        {
            dim(cx, cy);
        }
        // How much of the field the name takes: amber once a sprite no longer fits.
        const size_t used = g_field.text.size();
        const std::wstring count = std::to_wstring(used) + L"/" + std::to_wstring(limit);
        font->draw(device, UiRect{cx + kCell + 6, cy + 2, 64, kCell}, count.c_str(),
            used + 18 > limit ? kFullColor : kCountColor, 0);
        cx = x + kPad;
        cy += kCell + kPad;
        for (uint16_t icon : kIcons)
        {
            const bool fits = cell(Cell::Icon, icon, cx, cy);
            wchar_t code[24];
            std::swprintf(code, std::size(code), L"\\gFFFFFFFF%08X", icon);
            font->draw(device, UiRect{cx + 2, cy + 1, kCell, kCell}, code, 0xFFFFFFFF, 0);
            if (!fits)
            {
                dim(cx, cy);
            }
            cx += kCell;
        }
        g_palette = std::move(p);
    }

    // The field's text from the scroll position as far as it fits, the selection behind it and the caret, where the
    // game draws its text (its own blinking "_" after the text included). Characters and codes are measured as wholes:
    // a gap is only ever between two of them.
    void drawText(cUI_EditControl* control, IDirect3DDevice7* device, bool nameField)
    {
        cFontManager* fonts = cFontManager::instance();
        cFont* font = fonts && control->font < fonts->fonts.size() ? fonts->fonts[control->font] : nullptr;
        if (!font)
        {
            return;
        }
        std::scoped_lock lock(g_lock);
        sync(control);
        Field& f = g_field;
        const size_t length = f.text.size();
        const std::wstring shown = (control->flags & cUI_EditControl::password) ? std::wstring(length, L'*') : f.text;
        // Widths of the text's beginnings at each stop; inside a code, its start's.
        std::vector<int> at(length + 1, 0);
        for (size_t i = 0; i < length;)
        {
            const size_t next = nextStop(i);
            at[next] = font->textWidth(shown.substr(0, next).c_str());
            std::fill(at.begin() + i + 1, at.begin() + next, at[i]);
            i = next;
        }
        const UiRect box = control->absoluteRect();
        const int left = box.x + 4 + control->textX, top = box.y + 2 + control->textY;
        const int room = std::max(box.width - 8 - control->textX, 1);
        const int caretWidth = font->textWidth(L"_");

        // The last stop that shows from `scroll` on.
        const auto lastStop = [&](size_t scroll) {
            size_t stop = scroll;
            while (stop < length && at[nextStop(stop)] - at[scroll] <= room)
            {
                stop = nextStop(stop);
            }
            return stop;
        };

        // A click, then the drag after it: the caret to the stop nearest the cursor. A drag past an edge goes one
        // further, so the text scrolls.
        cMouse* mouse = cMouse::instance();
        if (f.dragging && !(mouse->flags & cMouse::leftHeld))
        {
            f.dragging = false;
        }
        if (f.click || f.dragging)
        {
            const int cursor = f.click ? f.clickX : mouse->x - static_cast<int>(std::lround(UiCanvas::frame().x));
            const int x = cursor - left + at[f.scroll];     // from the text's start
            size_t stop = f.scroll;
            while (stop < length && x >= (at[stop] + at[nextStop(stop)]) / 2)
            {
                stop = nextStop(stop);
            }
            const size_t last = lastStop(f.scroll);
            if (stop > last)
            {
                stop = f.click ? last : nextStop(last);
            }
            else if (x < at[f.scroll] && !f.click && f.scroll > 0)
            {
                stop = prevStop(f.scroll);
            }
            moveCaret(stop, f.click ? f.extend : true);
            f.click = false;
            f.dragging = true;
        }

        // Scrolled so the caret shows, and back as far as the text's end allows.
        f.scroll = std::min(f.scroll, f.caret);
        while (f.scroll < f.caret && at[f.caret] - at[f.scroll] + caretWidth > room)
        {
            f.scroll = nextStop(f.scroll);
        }
        while (f.scroll > 0 && at[length] - at[prevStop(f.scroll)] + caretWidth <= room)
        {
            f.scroll = prevStop(f.scroll);
        }
        const size_t end = lastStop(f.scroll);

        const int lineHeight = std::max<int>(font->lineHeight(L'A'), 1);
        const size_t from = std::clamp(selectionStart(), f.scroll, end), to = std::clamp(selectionEnd(), f.scroll, end);
        if (from < to)
        {
            const float bottom = static_cast<float>(std::min(top + lineHeight, box.y + box.height - 2));
            fillRect(device, static_cast<float>(left + at[from] - at[f.scroll]), static_cast<float>(top),
                static_cast<float>(left + at[to] - at[f.scroll]), bottom, kSelectionColor);
        }
        // A colour set before the part that shows still holds in it.
        std::wstring visible;
        for (size_t i = 0; i < f.scroll; i = nextStop(i))
        {
            if (nextStop(i) - i == 10 && shown[i + 1] == L'c')
            {
                visible = shown.substr(i, 10);
            }
            else if (nextStop(i) - i == 2 && shown[i + 1] == L'r')
            {
                visible.clear();
            }
        }
        visible += shown.substr(f.scroll, end - f.scroll);
        if (!visible.empty())
        {
            font->draw(device, UiRect{left, top, box.width, box.height}, visible.c_str(), control->color,
                control->fontFlags);
        }
        if ((GetTickCount() - f.blinkFrom) / kBlinkMs % 2 == 0)
        {
            font->draw(device, UiRect{left + at[f.caret] - at[f.scroll], top, box.width, box.height}, L"_",
                control->color, control->fontFlags);
        }

        if (nameField)
        {
            const UiCanvas::Frame frame = UiCanvas::frame();
            g_nameRect[0] = box.x + frame.x;
            g_nameRect[1] = box.y + frame.y;
            g_nameRect[2] = box.width;
            g_nameRect[3] = box.height;
            g_paletteWanted = f.palette;
            g_paletteFont = font;
            g_paletteTextColor = control->color;
            g_paletteLimit = std::min<size_t>(control->maxLength, kMaxLength);
        }
    }

    uint32_t __fastcall hookRender(cUI_EditControl* self, void* edx, IDirect3DDevice7* device)
    {
        if (!editable(self))
        {
            return g_origRender(self, edx, device);
        }
        // The character name field: the field of the character menu (cUI_Character).
        const UiWindows::Owner owner = UiWindows::current();
        const bool nameField = owner.kind == UiWindows::Kind::Menu && owner.index == 1;
        g_nameField = nameField ? self : nullptr;
        g_drawn = self;
        g_drawnAt = GetTickCount();
        t_hideText = true;
        const uint32_t result = g_origRender(self, edx, device);
        t_hideText = false;
        drawText(self, device, nameField);
        return result;
    }

    // A colour code (\c or \r) of `length` at i.
    bool colorCode(const std::wstring& t, size_t i, size_t length)
    {
        return (length == 10 && t[i + 1] == L'c') || (length == 2 && t[i + 1] == L'r');
    }

    // The text with its colour codes taken out.
    std::wstring withoutColors(const std::wstring& t)
    {
        std::wstring out;
        for (size_t i = 0; i < t.size();)
        {
            const size_t length = codeLength(t, i);
            if (!colorCode(t, i, length))
            {
                out.append(t, i, length);
            }
            i += length;
        }
        return out;
    }

    // The colour code in effect at i (the last one before it); empty for the field's own colour.
    std::wstring colorAt(const std::wstring& t, size_t i)
    {
        std::wstring color;
        for (size_t k = 0; k < i;)
        {
            const size_t length = codeLength(t, k);
            if (length == 10 && t[k + 1] == L'c')
            {
                color = t.substr(k, 10);
            }
            else if (length == 2 && t[k + 1] == L'r')
            {
                color.clear();
            }
            k += length;
        }
        return color;
    }

    // Under g_lock: what a palette cell makes of the text, and the selection after it. Colours replace colours instead
    // of piling up: without a selection a colour (or the field's own) applies to the whole name and the others go; with
    // one it applies to the selection, whose colours go, the colour after it comes back, and the selection stays on it
    // to try another. A sprite goes in at the caret.
    struct Applied
    {
        std::wstring text;
        size_t anchor, caret;
    };

    Applied apply(const Cell& cell)
    {
        const std::wstring& t = g_field.text;
        const size_t from = selectionStart(), to = selectionEnd();
        wchar_t buffer[24] = {};
        if (cell.kind == Cell::Icon)
        {
            std::swprintf(buffer, std::size(buffer), L"\\gFFFFFFFF%08X", cell.value);
            const std::wstring code = buffer;
            return {t.substr(0, from) + code + t.substr(to), from + code.size(), from + code.size()};
        }
        if (cell.kind == Cell::Color)
        {
            std::swprintf(buffer, std::size(buffer), L"\\c%08X", cell.value);
        }
        const std::wstring code = buffer;
        if (from == to)
        {
            const std::wstring text = code + withoutColors(t);
            return {text, text.size(), text.size()};
        }
        const std::wstring tail = t.substr(to);
        const std::wstring after = colorAt(t, to);
        const std::wstring open = cell.kind == Cell::Color ? code : colorAt(t, from).empty() ? L"" : L"\\r";
        const std::wstring middle = withoutColors(t.substr(from, to - from));
        std::wstring close;
        if (!tail.empty() && !colorCode(tail, 0, codeLength(tail, 0)))
        {
            close = !after.empty() ? after : cell.kind == Cell::Color ? L"\\r" : L"";
        }
        return {t.substr(0, from) + open + middle + close + tail, from, from + open.size() + middle.size()};
    }

    // The most the field takes.
    size_t fieldLimit(const cUI_EditControl* control)
    {
        return std::min<size_t>(control->maxLength, kMaxLength);
    }

    size_t appliedLength(const Cell& cell)
    {
        return apply(cell).text.size();
    }

    // Under g_lock: a palette cell applied, if its result fits the field.
    void insertCode(cUI_EditControl* control, const Cell& cell)
    {
        Applied result = apply(cell);
        if (result.text.size() > fieldLimit(control))
        {
            return;
        }
        store(control, std::move(result.text));
        g_field.anchor = result.anchor;
        moveCaret(result.caret, true);
    }

    // Clicks and Esc for the palette. True: the message is the palette's.
    bool paletteMessage(UINT message, WPARAM wParam)
    {
        // The game's window class takes double clicks: the second of two quick clicks comes as one.
        const bool down = message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN || message == WM_LBUTTONDBLCLK ||
            message == WM_RBUTTONDBLCLK;
        const int button = message == WM_RBUTTONDOWN || message == WM_RBUTTONDBLCLK || message == WM_RBUTTONUP ? 1 : 0;
        if (message == WM_LBUTTONUP || message == WM_RBUTTONUP)
        {
            return std::exchange(g_swallowUp[button], false);
        }
        if (message == WM_CHAR && wParam == 0x1B && g_swallowEscape)
        {
            return true;
        }
        if (message == WM_KEYUP && wParam == VK_ESCAPE && g_swallowEscape)
        {
            g_swallowEscape = false;
            return true;
        }
        const bool escape = message == WM_KEYDOWN && wParam == VK_ESCAPE;
        if (!down && !escape)
        {
            return false;
        }
        cUI_EditControl* field = focusedField();
        std::scoped_lock lock(g_lock);
        if (!field || field != g_nameField.load())
        {
            return false;
        }
        sync(field);
        const cMouse* mouse = cMouse::instance();
        const float mx = static_cast<float>(mouse->x), my = static_cast<float>(mouse->y);
        const Palette& p = g_palette;
        if (escape)
        {
            if (!g_field.palette)
            {
                return false;
            }
            g_field.palette = false;
            g_swallowEscape = true;
            return true;
        }
        if (button == 1 && mx >= g_nameRect[0] && mx < g_nameRect[0] + g_nameRect[2] && my >= g_nameRect[1] &&
            my < g_nameRect[1] + g_nameRect[3])
        {
            g_field.palette = !g_field.palette;
            g_swallowUp[1] = true;
            return true;
        }
        if (!g_field.palette || !p.shown)
        {
            return false;
        }
        if (button == 0 && mx >= p.x && mx < p.x + p.w && my >= p.y && my < p.y + p.h)
        {
            for (const Cell& cell : p.cells)
            {
                if (mx >= cell.x && mx < cell.x + kCell && my >= cell.y && my < cell.y + kCell)
                {
                    insertCode(field, cell);
                }
            }
            g_swallowUp[0] = true;
            return true;
        }
        g_field.palette = false;    // a click elsewhere closes it and goes on
        return false;
    }

    bool fieldKey(WPARAM key)
    {
        return key == VK_LEFT || key == VK_RIGHT || key == VK_HOME || key == VK_END || key == VK_DELETE;
    }
}

void TextEdit::install()
{
    const uintptr_t site = Addr::editRenderFontCall;
    if (!site || !Patch::verify(site, {0xE8}))
    {
        LOG("Text fields: the font lookup in their render was not found, left as they are");
        return;
    }
    g_fontLookup = reinterpret_cast<FontLookup>(site + 5 + *reinterpret_cast<const int32_t*>(site + 1));
    if (Patch::redirectCall(site, reinterpret_cast<const void*>(&fontForText)))
    {
        Patch::hook(g_origRender, Addr::cUI_EditControl_render, &hookRender, "cUI_EditControl::render");
        Patch::hook(g_origReceiveEvent, Addr::cUI_EditControl_receiveEvent, &hookReceiveEvent,
            "cUI_EditControl::receiveEvent");
    }
}

bool TextEdit::message(UINT& message, WPARAM& wParam)
{
    if (!g_origRender)
    {
        return false;
    }
    if (paletteMessage(message, wParam))
    {
        return true;
    }
    if ((message != WM_KEYDOWN && message != WM_KEYUP) || !fieldKey(wParam) || !focusedField())
    {
        return false;
    }
    if (message == WM_KEYUP)
    {
        return true;    // the field had the key (the menus act on some keys when they come up)
    }
    message = WM_CHAR;
    wParam = kKey + wParam + (GetKeyState(VK_SHIFT) < 0 ? kShift : 0) + (GetKeyState(VK_CONTROL) < 0 ? kCtrl : 0);
    return false;
}

void TextEdit::drawOverlay(IDirect3DDevice7* device)
{
    std::scoped_lock lock(g_lock);
    if (!std::exchange(g_paletteWanted, false) || !g_paletteFont || !device)
    {
        g_palette.shown = false;
        return;
    }
    const UiCanvas::Frame frame = UiCanvas::frame();
    const UiRect box{static_cast<int32_t>(std::lround(g_nameRect[0] - frame.x)),
        static_cast<int32_t>(std::lround(g_nameRect[1] - frame.y)), static_cast<int16_t>(g_nameRect[2]),
        static_cast<int16_t>(g_nameRect[3])};
    drawPalette(g_paletteTextColor, g_paletteFont, device, box, g_paletteLimit);
}
