#pragma once
#include <windows.h>
#include <commctrl.h>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

// Settings forms declared in code, ImGui-style: pages, columns, titled sections and rows of controls, each control bound
// to an ini key. The form sizes everything from its text, builds the dialog templates and runs them modally; afterwards
// changes() lists the keys whose values the user changed. It is drawn like Sacred's own menus: dark stone, steel frames
// and bevels, gold lettering in the game's uncial, the red of its portal as the one accent: a banner on top, the page
// names stacked in a column on the left, the shown page in a steel frame beside them, plaques with glowing orbs for
// buttons. A controller works the form too: the D-pad or left stick moves between the controls (left and right change
// a combo box's value; right from a page name goes into its page, left from a page's first column back to its name), A
// clicks or opens, B closes an open list, LB and RB switch pages, Start is OK.
//
//     ui.page(L"General");
//     ui.group(L"Display");
//     ui.combo(L"Frame limit:", {L"Display", L"FpsLimit"}, {0, 30, 60}, Config::display.fpsLimit, fpsText).tip(L"...");
//     ui.check(L"VSync", {L"Display", L"VSync"}, Config::display.vsync).enabledIf(isD3d9);
namespace Ui
{
    class Form;
    class DialogTemplate;
    using Predicate = std::function<bool(const Form&)>;

    // The ini key a control edits. Controls sharing a key each edit one comma-separated `part` of its value; a
    // comma-separated `name` spreads the control's comma-separated value over those keys.
    struct Key
    {
        const wchar_t* section = nullptr;
        const wchar_t* name = nullptr;
        int part = -1;
    };

    struct Choice
    {
        std::wstring text;
        std::wstring value;
    };

    struct Change
    {
        std::wstring section;
        std::wstring name;
        std::wstring value;
    };

    // A declared control; the setters chain.
    class Control
    {
    public:
        Control& tip(std::wstring text);
        // Grayed out unless the predicate holds (re-evaluated after every change in the form).
        Control& enabledIf(Predicate predicate);
        // Fixed width of the control itself, in dialog units (otherwise from its text, or the room left in the row).
        Control& width(int dialogUnits);
        // Check box: the ini values for checked and unchecked (default "1" and "0").
        Control& values(const wchar_t* on, const wchar_t* off);
        // Edit box: digits only.
        Control& digits();

    private:
        friend class Form;
        enum class Type
        {
            Check,
            Combo,
            Edit,
            Label,
            Text,
            Button,
        };

        Type m_type = Type::Label;
        std::wstring m_text;     // caption, or the label in front of a combo or edit box
        Key m_key;
        std::vector<Choice> m_choices;
        std::wstring m_initial;  // the ini value at the start
        std::wstring m_current;  // the value when the form closed
        std::wstring m_on = L"1";
        std::wstring m_off = L"0";
        std::wstring m_tip;
        std::vector<Predicate> m_enabled;
        std::function<void(Form&)> m_click;
        int m_fixedWidth = 0;
        bool m_digits = false;
        bool m_dim = false;      // a label or paragraph whose control is grayed out

        int m_page = -1;  // -1 = the footer
        int m_id = 0;     // the label in front of it: m_id + 1
        short m_x = 0, m_y = 0, m_cx = 0, m_cy = 0;
        short m_labelX = 0, m_labelY = 0, m_labelCx = 0;
        HWND m_hwnd = nullptr;
        HWND m_label = nullptr;
    };

    class Form
    {
    public:
        enum class Result
        {
            Ok,
            Cancel,
            Failed,
        };

        // Layout: a page is one of the names in the column on the left, column() starts its next column, group() a
        // titled section; rows stack downwards. Controls declared after footer() go into the bottom row, left of the OK
        // and Cancel buttons.
        void page(const wchar_t* title);
        void column();
        void group(const wchar_t* title);
        void footer();
        void sameLine();   // the next control goes into the current row
        void indent();
        void unindent();
        // The controls up to endEnabledIf() are grayed out unless the predicate holds (nests).
        void beginEnabledIf(Predicate predicate);
        void endEnabledIf();

        Control& check(const wchar_t* text, Key key, bool value);
        // A value not among the choices is added as "Custom (value)".
        Control& combo(const wchar_t* label, Key key, std::vector<Choice> choices, const std::wstring& value);
        // Numbers, ascending, the value among them.
        Control& combo(const wchar_t* label, Key key, std::vector<int> numbers, int value,
            const std::function<std::wstring(int)>& text);
        Control& edit(const wchar_t* label, Key key, const std::wstring& value);
        Control& label(const wchar_t* text);
        Control& text(const wchar_t* text);  // a paragraph, wrapped to the width of its column
        Control& button(const wchar_t* text, std::function<void(Form&)> click);

        void buttons(const wchar_t* ok, const wchar_t* cancel);
        void icons(HICON big, HICON small);
        // The faces of the page names, titles and buttons (`display`) and of everything else (`body`); installed fonts
        // or ones the caller added privately. Palatino Linotype for both otherwise.
        void typefaces(std::wstring display, std::wstring body);
        // A strip across the top, `height` dialog units high, drawn by `paint` into the rect it gets (pixels).
        void banner(int height, std::function<void(HDC dc, const RECT& rect)> paint);

        // Shows the form modally. `module` holds the visual styles manifest (form.rc).
        Result run(HMODULE module, const wchar_t* title);

        // The value of a key as the form shows it (also after run()).
        std::wstring value(const wchar_t* section, const wchar_t* name) const;
        // A check box bound to the key is checked, or the value is something other than empty or "0".
        bool on(const wchar_t* section, const wchar_t* name) const;
        void set(const wchar_t* section, const wchar_t* name, const std::wstring& value);
        // Keys whose value differs from the declared one.
        std::vector<Change> changes() const;

    private:
        struct Row
        {
            std::vector<Control*> controls;
            int indent = 0;
        };
        struct Block
        {
            std::wstring title;  // empty: no group box
            std::vector<Row> rows;
            short x = 0, y = 0, cx = 0, cy = 0;
        };
        struct Column
        {
            std::vector<Block> blocks;
        };
        struct Page
        {
            std::wstring title;
            std::vector<Column> columns;
            HWND hwnd = nullptr;
        };
        struct Size
        {
            int cx, cy;
        };

        Control& add(Control control);
        Block& block();
        void layout();
        DialogTemplate mainTemplate(const wchar_t* title) const;
        DialogTemplate pageTemplate(const Page& page) const;
        void addControl(DialogTemplate& dialog, const Control& control) const;
        void init(HWND dlg);
        void createFonts();
        void showPage(int index);
        RECT frameRect() const;     // the shown page's steel frame, pixels of the window
        void ensureStone() const;
        void paintBackground(HDC dc, HWND control, const RECT& rect) const;     // what lies behind a control
        int navIndex(HWND control) const;   // -1: not a page button
        void command(WORD id);
        void updateEnabled();
        void addTooltips();
        void finish(int result);
        void gamepad();
        void padSideways(HWND focus, bool right);
        void padPage(int step);
        void focusOn(HWND control);
        void paint(HWND window, HDC dc);
        HBRUSH colors(UINT msg, HDC dc, HWND control) const;
        LRESULT customDraw(const NMCUSTOMDRAW& draw) const;
        std::wstring read(const Control& control) const;
        void write(Control& control, const std::wstring& value);
        std::vector<const Control*> bound(const wchar_t* section, const wchar_t* name) const;
        static INT_PTR CALLBACK dialogProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp);

        std::deque<Control> m_controls;
        std::vector<Page> m_pages;
        Block m_footer;
        bool m_inFooter = false;
        bool m_sameLine = false;
        int m_indent = 0;
        std::vector<Predicate> m_enabled;
        std::wstring m_ok = L"OK";
        std::wstring m_cancel = L"Cancel";
        HICON m_icons[2] = {};
        std::wstring m_display = L"Palatino Linotype";
        std::wstring m_body = L"Palatino Linotype";
        int m_bannerHeight = 0;
        std::function<void(HDC, const RECT&)> m_bannerPaint;

        HMODULE m_module = nullptr;
        Size m_page = {};    // page size, dialog units
        Size m_window = {};
        // Dialog units: the column of page names, the steel frame and the page inside it, the footer.
        int m_navX = 0, m_navY = 0, m_navWidth = 0, m_navItem = 0;
        int m_frameX = 0, m_frameY = 0, m_frameCx = 0, m_frameCy = 0;
        int m_pagesX = 0, m_pagesY = 0;
        int m_footerY = 0;
        HWND m_dlg = nullptr;
        std::vector<HWND> m_nav;
        int m_shown = 0;
        HWND m_tooltip = nullptr;
        HFONT m_headingFont = nullptr;     // the sections' titles
        HFONT m_navFont = nullptr;         // the page names
        HFONT m_buttonFont = nullptr;
        HBRUSH m_panel = nullptr;          // inside the frame
        HBRUSH m_input = nullptr;
        mutable HBITMAP m_stone = nullptr; // the window's background, made for its client size
        mutable SIZE m_stoneSize = {};
        uint32_t m_padHeld = 0;    // buttons, the left stick as the D-pad
        DWORD m_padRepeat = 0;     // when a held direction moves again
        bool m_padUsed = false;
    };
}
