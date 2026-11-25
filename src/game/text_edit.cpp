#include "game/text_edit.h"
#include "game/focus.h"
#include "game/sacred_addr.h"
#include "game/ui_canvas.h"
#include "log.h"
#include "patch.h"
#include "sacred/mouse.h"
#include "sacred/ui.h"

#include <ddraw.h>
#include <d3d.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cwctype>
#include <iterator>
#include <mutex>
#include <string>
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
    };
    std::mutex g_lock;
    Field g_field;

    // Set around the original render of a field we draw the text of: its font lookup finds no font, so it draws the
    // frame only.
    thread_local bool t_hideText = false;

    // The focused field we drew last, and when.
    std::atomic<const void*> g_drawn{nullptr};
    std::atomic<DWORD> g_drawnAt{0};

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
                moveCaret(ctrl ? wordLeft(g_field.caret) : g_field.caret - (g_field.caret > 0), shift);
            }
            break;
        case VK_RIGHT:
            if (!shift && !ctrl && g_field.caret != g_field.anchor)
            {
                moveCaret(selectionEnd(), false);
            }
            else
            {
                moveCaret(ctrl ? wordRight(g_field.caret) : g_field.caret + 1, shift);
            }
            break;
        case VK_HOME:
            moveCaret(0, shift);
            break;
        case VK_END:
            moveCaret(length, shift);
            break;
        case VK_DELETE:
            erase(control, ctrl ? wordRight(g_field.caret) : std::min(g_field.caret + 1, length));
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
            erase(control, g_field.caret - (g_field.caret > 0));
            return true;
        case 0x7F:                  // Ctrl+Backspace
            erase(control, wordLeft(g_field.caret));
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

    // The field's text from `scroll` as far as it fits, the selection behind it and the caret, where the game draws
    // its text (its own blinking "_" after the text included).
    void drawText(cUI_EditControl* control, IDirect3DDevice7* device)
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
        const std::wstring shown = (control->flags & cUI_EditControl::password)
            ? std::wstring(f.text.size(), L'*') : f.text;
        // Widths of the text's beginnings: what lies between two characters.
        std::vector<int> at(shown.size() + 1, 0);
        for (size_t i = 1; i <= shown.size(); ++i)
        {
            at[i] = font->textWidth(shown.substr(0, i).c_str());
        }
        const UiRect box = control->absoluteRect();
        const int left = box.x + 4 + control->textX, top = box.y + 2 + control->textY;
        const int room = std::max(box.width - 8 - control->textX, 1);
        const int caretWidth = font->textWidth(L"_");

        // The last gap that shows from `scroll` on.
        const auto lastGap = [&](size_t scroll) {
            size_t gap = scroll;
            while (gap < shown.size() && at[gap + 1] - at[scroll] <= room)
            {
                ++gap;
            }
            return gap;
        };

        // A click, then the drag after it: the caret to the gap nearest the cursor. A drag past an edge goes one
        // character further, so the text scrolls.
        cMouse* mouse = cMouse::instance();
        if (f.dragging && !(mouse->flags & cMouse::leftHeld))
        {
            f.dragging = false;
        }
        if (f.click || f.dragging)
        {
            const int cursor = f.click ? f.clickX : mouse->x - static_cast<int>(std::lround(UiCanvas::frame().x));
            const int x = cursor - left + at[f.scroll];     // from the text's start
            size_t gap = f.scroll;
            while (gap < shown.size() && x >= (at[gap] + at[gap + 1]) / 2)
            {
                ++gap;
            }
            const size_t last = lastGap(f.scroll);
            if (gap > last)
            {
                gap = f.click ? last : last + 1;
            }
            else if (x < at[f.scroll] && !f.click && f.scroll > 0)
            {
                gap = f.scroll - 1;
            }
            moveCaret(gap, f.click ? f.extend : true);
            f.click = false;
            f.dragging = true;
        }

        // Scrolled so the caret shows, and back as far as the text's end allows.
        f.scroll = std::min(f.scroll, f.caret);
        while (f.scroll < f.caret && at[f.caret] - at[f.scroll] + caretWidth > room)
        {
            ++f.scroll;
        }
        while (f.scroll > 0 && at[shown.size()] - at[f.scroll - 1] + caretWidth <= room)
        {
            --f.scroll;
        }
        const size_t end = lastGap(f.scroll);

        const int lineHeight = std::max<int>(font->lineHeight(L'A'), 1);
        const size_t from = std::clamp(selectionStart(), f.scroll, end), to = std::clamp(selectionEnd(), f.scroll, end);
        if (from < to)
        {
            const float bottom = static_cast<float>(std::min(top + lineHeight, box.y + box.height - 2));
            fillRect(device, static_cast<float>(left + at[from] - at[f.scroll]), static_cast<float>(top),
                static_cast<float>(left + at[to] - at[f.scroll]), bottom, kSelectionColor);
        }
        const std::wstring visible = shown.substr(f.scroll, end - f.scroll);
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
    }

    uint32_t __fastcall hookRender(cUI_EditControl* self, void* edx, IDirect3DDevice7* device)
    {
        if (!editable(self))
        {
            return g_origRender(self, edx, device);
        }
        g_drawn = self;
        g_drawnAt = GetTickCount();
        t_hideText = true;
        const uint32_t result = g_origRender(self, edx, device);
        t_hideText = false;
        drawText(self, device);
        return result;
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

bool TextEdit::translateKey(UINT& message, WPARAM& wParam)
{
    if ((message != WM_KEYDOWN && message != WM_KEYUP) || !fieldKey(wParam) || !g_origRender || !focusedField())
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
