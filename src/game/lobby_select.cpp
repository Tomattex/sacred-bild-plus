#include "game/lobby_select.h"
#include "game/sacred_addr.h"
#include "game/ui_canvas.h"
#include "game/ui_draw.h"
#include "config.h"
#include "log.h"
#include "patch.h"
#include "sacred/mouse.h"
#include "sacred/ui.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace
{
    using namespace Sacred;

    constexpr uint32_t kDefaultPort = 7066;         // every Sacred lobby's
    constexpr size_t kMaxHost = 120;
    constexpr size_t kIpField = 0x38, kPortField = 0x3C, kHostField = 0x39E4;  // in the network object
    constexpr uint32_t kInternet = 1;               // cNet_openScreen's flag for Closed and Open Internet

    constexpr uint16_t kFont = 1;                   // the menu entries' typeface, smaller
    constexpr int kRow = 24, kListWidth = 320, kArrow = 22, kRemoveWidth = 22;
    constexpr uint32_t kGold = 0xFFE6C45A, kGoldLit = 0xFFFFEAA8, kDim = 0xFFB8B0A0, kText = 0xFFD8D0BC;
    constexpr uint32_t kBad = 0xFFE06060, kHover = 0x40FFFFFF;

    struct Server
    {
        std::string host;
        uint32_t port = kDefaultPort;

        std::string text() const { return port == kDefaultPort ? host : host + ":" + std::to_string(port); }
        std::string key() const { return host + ":" + std::to_string(port); }
    };

    // The list, the choice (past the end: none, the game's own NETWORK_LOBBY) and the options' lobby when SacredBild
    // last saw it (`seen`, host:port): when the options say something else that isn't the choice, it was set outside.
    std::mutex g_lock;
    std::vector<Server> g_servers;
    size_t g_chosen = 0;
    std::string g_seen;
    std::wstring g_path;

    // The selector: the list open, a server being typed, and where its parts were drawn (canvas units).
    enum class Part { Previous, Next, Toggle, Row, Remove, Add };
    struct Hit
    {
        Part part;
        size_t index;
        float left, top, right, bottom;
    };
    bool g_open = false;
    bool g_adding = false;
    std::string g_typed;
    std::vector<Hit> g_hits;
    bool g_swallowUp = false;
    bool g_swallowEscape = false;

    // The hooks run on the engine's main thread (the network menu).
    decltype(Addr::cNet_openScreen)::Ptr g_origOpen = nullptr;
    decltype(Addr::cNet_loadSettings)::Ptr g_origLoad = nullptr;
    using PortFn = uint32_t(__fastcall*)(void* options, void* edx);
    PortFn g_origPort = nullptr;
    bool g_inOpen = false;
    uint32_t g_openFlag = 0;
    uint32_t g_port = 0;                            // the port the lobby connection gets, 0: the options'

    bool validHost(const std::string& host)
    {
        if (host.empty() || host.size() > kMaxHost)
        {
            return false;
        }
        return std::all_of(host.begin(), host.end(), [](char ch) {
            return std::isalnum(static_cast<unsigned char>(ch)) || ch == '.' || ch == '-' || ch == '_';
        });
    }

    // "host" or "host:port"; false if it isn't a server.
    bool parse(const std::string& text, Server& out)
    {
        const size_t colon = text.find(':');
        out.host = text.substr(0, colon);
        out.port = kDefaultPort;
        if (colon != std::string::npos)
        {
            char* end = nullptr;
            const unsigned long port = std::strtoul(text.c_str() + colon + 1, &end, 10);
            if (!end || *end || port < 1 || port > 65535)
            {
                return false;
            }
            out.port = static_cast<uint32_t>(port);
        }
        return validHost(out.host);
    }

    // Under g_lock: the index of `server` in the list, added if it isn't there.
    size_t find(const Server& server)
    {
        for (size_t i = 0; i < g_servers.size(); ++i)
        {
            if (_stricmp(g_servers[i].key().c_str(), server.key().c_str()) == 0)
            {
                return i;
            }
        }
        g_servers.push_back(server);
        return g_servers.size() - 1;
    }

    std::string readFile(const std::wstring& path)
    {
        HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE)
        {
            return {};
        }
        std::string text;
        char buffer[4096];
        DWORD n = 0;
        while (ReadFile(f, buffer, sizeof buffer, &n, nullptr) && n)
        {
            text.append(buffer, n);
        }
        CloseHandle(f);
        return text;
    }

    std::vector<std::string> lines(const std::string& text)
    {
        std::vector<std::string> out;
        size_t start = 0;
        while (start < text.size())
        {
            size_t end = text.find('\n', start);
            if (end == std::string::npos)
            {
                end = text.size();
            }
            std::string line = text.substr(start, end - start);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            {
                line.pop_back();
            }
            out.push_back(line);
            start = end + 1;
        }
        return out;
    }

    // Under g_lock.
    void save()
    {
        std::string text = "; SacredBild: the lobby servers of the multiplayer menu (Closed and Open Internet). * is the one in use.\r\n";
        text += "; seen: " + g_seen + "\r\n";
        for (size_t i = 0; i < g_servers.size(); ++i)
        {
            text += (i == g_chosen ? "*" : "") + g_servers[i].text() + "\r\n";
        }
        const std::wstring temp = g_path + L".tmp";
        HANDLE f = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD written = 0;
        bool ok = f != INVALID_HANDLE_VALUE &&
            WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size();
        if (f != INVALID_HANDLE_VALUE)
        {
            CloseHandle(f);
        }
        ok = ok && MoveFileExW(temp.c_str(), g_path.c_str(), MOVEFILE_REPLACE_EXISTING);
        if (!ok)
        {
            DeleteFileW(temp.c_str());
            LOG("Lobby: SacredBild-lobbies.txt cannot be written");
        }
    }

    // The value of `key` in Settings.cfg's "KEY : value" lines (the game's folder, as the game reads it).
    std::string settingsValue(const std::string& settings, const char* key)
    {
        for (const std::string& line : lines(settings))
        {
            const size_t colon = line.find(':');
            if (colon == std::string::npos)
            {
                continue;
            }
            std::string name = line.substr(0, colon), value = line.substr(colon + 1);
            name.erase(name.find_last_not_of(" \t") + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            if (name == key)
            {
                return value;
            }
        }
        return {};
    }

    // The servers of a servers.json in the game folder ([{"IP": "...", "Port": 7066}, ...], another launcher's).
    std::vector<Server> launcherServers()
    {
        std::vector<Server> out;
        const std::string json = readFile(L"servers.json");
        for (size_t at = json.find("\"IP\""); at != std::string::npos; at = json.find("\"IP\"", at + 1))
        {
            const size_t open = json.find('"', json.find(':', at) + 1);
            const size_t close = open == std::string::npos ? open : json.find('"', open + 1);
            if (close == std::string::npos)
            {
                break;
            }
            Server s;
            s.host = json.substr(open + 1, close - open - 1);
            const size_t next = json.find("\"IP\"", close);
            const size_t port = json.find("\"Port\"", close);
            if (port != std::string::npos && port < next)
            {
                s.port = static_cast<uint32_t>(std::strtoul(json.c_str() + json.find(':', port) + 1, nullptr, 10));
            }
            if (validHost(s.host) && s.port >= 1 && s.port <= 65535)
            {
                out.push_back(s);
            }
        }
        return out;
    }

    // The list from SacredBild-lobbies.txt; the first time from Settings.cfg's lobby and servers.json.
    void load()
    {
        std::scoped_lock lock(g_lock);
        const std::string text = readFile(g_path);
        if (!text.empty())
        {
            g_chosen = SIZE_MAX;
            for (const std::string& line : lines(text))
            {
                if (line.rfind("; seen: ", 0) == 0)
                {
                    g_seen = line.substr(8);
                    continue;
                }
                Server s;
                const bool chosen = !line.empty() && line[0] == '*';
                if (line.empty() || line[0] == ';' || !parse(line.substr(chosen ? 1 : 0), s))
                {
                    continue;
                }
                const size_t index = find(s);
                if (chosen)
                {
                    g_chosen = index;
                }
            }
            return;
        }
        const std::string settings = readFile(L"Settings.cfg");
        Server own;
        own.host = settingsValue(settings, "NETWORK_LOBBY");
        const std::string port = settingsValue(settings, "NETWORK_LOBBYPORT");
        own.port = port.empty() ? kDefaultPort : static_cast<uint32_t>(std::strtoul(port.c_str(), nullptr, 10));
        g_chosen = SIZE_MAX;
        if (validHost(own.host) && own.port >= 1 && own.port <= 65535)
        {
            g_chosen = find(own);
            g_seen = own.key();
        }
        for (const Server& s : launcherServers())
        {
            find(s);
        }
        if (!g_servers.empty())
        {
            save();
        }
    }

    // ---- The network object (no C++ objects in the functions with __try) ----

    bool readLobby(uint8_t* net, char* host, size_t size, uint32_t* port)
    {
        __try
        {
            *port = *reinterpret_cast<uint32_t*>(net + kPortField);
            const char* begin = *reinterpret_cast<const char**>(net + kHostField);
            size_t n = 0;
            while (begin && n + 1 < size && begin[n])
            {
                host[n] = begin[n];
                ++n;
            }
            host[n] = 0;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool writeLobby(uint8_t* net, const char* host, size_t length, uint32_t port)
    {
        __try
        {
            char allocator = 0;
            Addr::cString_assign(net + kHostField, host, host + length, &allocator);
            *reinterpret_cast<uint32_t*>(net + kIpField) = 0;
            *reinterpret_cast<uint32_t*>(net + kPortField) = port;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void __fastcall hookOpenScreen(void* net, void* edx, uint32_t flag)
    {
        const bool outer = !g_inOpen;
        g_inOpen = true;
        g_openFlag = flag;
        g_origOpen(net, edx, flag);
        if (outer)
        {
            g_inOpen = false;
            g_port = 0;
        }
    }

    // The options' lobby is in the object now: the chosen server takes its place for an Internet screen.
    void __fastcall hookLoadSettings(void* net, void* edx)
    {
        g_origLoad(net, edx);
        if (!g_inOpen || g_openFlag != kInternet)
        {
            return;
        }
        char own[kMaxHost + 8];
        uint32_t ownPort = 0;
        if (!readLobby(static_cast<uint8_t*>(net), own, sizeof own, &ownPort))
        {
            LOG("Lobby: the network object's lobby cannot be read");
            return;
        }
        Server chosen;
        {
            std::scoped_lock lock(g_lock);
            Server options;
            options.host = own;
            options.port = ownPort;
            if (validHost(options.host) && options.key() != g_seen)
            {
                // Set outside since SacredBild last looked (another launcher, Settings.cfg edited): it is the choice.
                const bool ours = g_chosen < g_servers.size() && g_servers[g_chosen].key() == options.key();
                if (!ours)
                {
                    g_chosen = find(options);
                    LOG("Lobby: {} set in Settings.cfg, chosen", options.text());
                }
                g_seen = options.key();
                save();
            }
            if (g_chosen >= g_servers.size())
            {
                return;
            }
            chosen = g_servers[g_chosen];
        }
        if (writeLobby(static_cast<uint8_t*>(net), chosen.host.c_str(), chosen.host.size(), chosen.port))
        {
            g_port = chosen.port;
            LOG("Lobby: {} for this Internet screen", chosen.text());
        }
        else
        {
            LOG("Lobby: {} could not be set", chosen.text());
        }
    }

    uint32_t __fastcall hookPort(void* options, void* edx)
    {
        if (g_inOpen && g_openFlag == kInternet && g_port)
        {
            return g_port;
        }
        return g_origPort(options, edx);
    }

    // ---- The selector ----

    // The start menu on its multiplayer screen, alone (no other menu screen or message box over it).
    cUI_MainMenu* multiplayerMenu(cUI_Manager* manager)
    {
        constexpr uint32_t kMultiplayer = 1;
        if (!manager || !(manager->flags & cUI_Manager::inMenus) || !(manager->flags & cUI_Manager::menusCreated))
        {
            return nullptr;
        }
        auto* menu = static_cast<cUI_MainMenu*>(manager->menus[0]);
        if (!menu || !menu->visible() || menu->screen != kMultiplayer || menu->entries[kMultiplayer].size() == 0)
        {
            return nullptr;
        }
        for (int i = 1; i < cUI_Manager::menuCount; ++i)
        {
            if (manager->menus[i] && manager->menus[i]->visible())
            {
                return nullptr;
            }
        }
        const auto* dialog = reinterpret_cast<const cUI_Control2*>(manager->dialog);
        return dialog && dialog->visible() ? nullptr : menu;
    }

    std::wstring widen(const std::string& s)
    {
        return {s.begin(), s.end()};
    }

    // `text` cut to `width` with ".." at the end.
    std::wstring fitted(cFont* font, std::wstring text, int width)
    {
        if (font->textWidth(text.c_str()) <= width)
        {
            return text;
        }
        while (!text.empty() && font->textWidth((text + L"..").c_str()) > width)
        {
            text.pop_back();
        }
        return text + L"..";
    }

    // Under g_lock: a server typed and Enter pressed.
    void commitTyped()
    {
        Server s;
        if (!parse(g_typed, s))
        {
            return;
        }
        g_chosen = find(s);
        save();
        LOG("Lobby: {} added and chosen", s.text());
        g_adding = false;
        g_typed.clear();
    }

    // Under g_lock.
    void choose(size_t index)
    {
        if (index < g_servers.size() && index != g_chosen)
        {
            g_chosen = index;
            save();
            LOG("Lobby: {} chosen", g_servers[index].text());
        }
    }

    void remove(size_t index)
    {
        if (index >= g_servers.size())
        {
            return;
        }
        LOG("Lobby: {} removed", g_servers[index].text());
        g_servers.erase(g_servers.begin() + static_cast<ptrdiff_t>(index));
        if (g_chosen == index)
        {
            g_chosen = g_servers.empty() ? SIZE_MAX : std::min(index, g_servers.size() - 1);
        }
        else if (g_chosen != SIZE_MAX && g_chosen > index)
        {
            --g_chosen;
        }
        save();
    }

    // The clipboard's text (one line).
    std::string clipboardText()
    {
        std::string text;
        if (!OpenClipboard(nullptr))
        {
            return text;
        }
        if (HANDLE data = GetClipboardData(CF_UNICODETEXT))
        {
            if (const auto* wide = static_cast<const wchar_t*>(GlobalLock(data)))
            {
                for (const wchar_t* p = wide; *p && *p != L'\r' && *p != L'\n' && text.size() < kMaxHost + 6; ++p)
                {
                    if (*p < 0x80 && *p > 0x20)
                    {
                        text += static_cast<char>(*p);
                    }
                }
                GlobalUnlock(data);
            }
        }
        CloseClipboard();
        return text;
    }
}

void LobbySelect::install()
{
    if (!Addr::cNet_openScreen || !Addr::cNet_loadSettings || !Addr::cString_assign || !Addr::lobbyPortCall ||
        !Patch::verify(Addr::lobbyPortCall, {0xE8}))
    {
        LOG("Lobby: the network menu's settings were not found: no lobby selector");
        return;
    }
    const std::wstring& ini = ConfigFile::path();
    g_path = ini.substr(0, ini.find_last_of(L"\\/") + 1) + L"SacredBild-lobbies.txt";
    load();
    const uintptr_t site = Addr::lobbyPortCall;
    g_origPort = reinterpret_cast<PortFn>(site + 5 + *reinterpret_cast<const int32_t*>(site + 1));
    if (!Patch::redirectCall(site, reinterpret_cast<const void*>(&hookPort)))
    {
        return;
    }
    Patch::hook(g_origOpen, Addr::cNet_openScreen, &hookOpenScreen, "cNet::openScreen");
    Patch::hook(g_origLoad, Addr::cNet_loadSettings, &hookLoadSettings, "cNet::loadSettings");
    std::scoped_lock lock(g_lock);
    LOG("Lobby: {} servers, {}", g_servers.size(),
        g_chosen < g_servers.size() ? "using " + g_servers[g_chosen].text() : std::string("the game's own"));
}

void LobbySelect::draw(cUI_Manager* manager, IDirect3DDevice7* device)
{
    cUI_MainMenu* menu = multiplayerMenu(manager);
    cFontManager* fonts = cFontManager::instance();
    cFont* font = fonts && fonts->fonts.size() > kFont ? fonts->fonts[kFont] : nullptr;
    std::scoped_lock lock(g_lock);
    g_hits.clear();
    if (!menu || !font || !device || !g_origLoad)
    {
        g_open = g_adding = false;
        return;
    }
    const UiCanvas::Frame frame = UiCanvas::frame();
    const cMouse* mouse = cMouse::instance();
    const float mx = mouse->x - frame.x, my = mouse->y - frame.y;
    const auto over = [&](float l, float t, float r, float b) { return mx >= l && mx < r && my >= t && my < b; };
    const auto hit = [&](Part part, size_t index, float l, float t, float r, float b) {
        g_hits.push_back({part, index, l + frame.x, t + frame.y, r + frame.x, b + frame.y});
        return over(l, t, r, b);
    };
    const auto text = [&](const std::wstring& s, int x, int y, uint32_t color) {
        font->draw(device, UiRect{x, y, 600, kRow}, s.c_str(), color, 0);
    };

    // The row under the screen's last entry (Back): < Lobby  host >
    cUI_StaticText64FX* last = menu->entries[1][menu->entries[1].size() - 1];
    const UiRect entry = last->absoluteRect();
    const int center = entry.x + entry.width / 2, top = entry.y + entry.height + 12;
    const bool any = g_chosen < g_servers.size();
    const std::wstring label = L"Lobby  ";
    const std::wstring host = fitted(font, widen(any ? g_servers[g_chosen].text() : std::string("as in Settings.cfg")), 240);
    const int labelWidth = font->textWidth(label.c_str()), hostWidth = font->textWidth(host.c_str());
    const int left = center - (labelWidth + hostWidth) / 2;
    const float rowTop = static_cast<float>(top), rowBottom = static_cast<float>(top + kRow);
    const bool hoverToggle = hit(Part::Toggle, 0, static_cast<float>(left), rowTop,
        static_cast<float>(left + labelWidth + hostWidth), rowBottom);
    text(label, left, top, kDim);
    text(host, left + labelWidth, top, hoverToggle || g_open ? kGoldLit : kGold);
    if (g_servers.size() > 1)
    {
        const int previous = left - kArrow - 6, next = left + labelWidth + hostWidth + 6;
        const bool hoverPrevious = hit(Part::Previous, 0, static_cast<float>(previous), rowTop,
            static_cast<float>(previous + kArrow), rowBottom);
        const bool hoverNext = hit(Part::Next, 0, static_cast<float>(next), rowTop, static_cast<float>(next + kArrow),
            rowBottom);
        text(L"<", previous + 6, top, hoverPrevious ? kGoldLit : kGold);
        text(L">", next + 6, top, hoverNext ? kGoldLit : kGold);
    }
    if (!g_open)
    {
        return;
    }

    // The list: a row per server (x removes it), then one to add a server.
    const int listLeft = center - kListWidth / 2, listTop = top + kRow + 6;
    const int rows = static_cast<int>(g_servers.size()) + 1;
    UiDraw::panel(device, static_cast<float>(listLeft), static_cast<float>(listTop),
        static_cast<float>(listLeft + kListWidth), static_cast<float>(listTop + rows * kRow + 8));
    int y = listTop + 4;
    for (size_t i = 0; i < g_servers.size(); ++i, y += kRow)
    {
        const float t = static_cast<float>(y), b = static_cast<float>(y + kRow);
        const float removeLeft = static_cast<float>(listLeft + kListWidth - kRemoveWidth - 4);
        const bool hoverRemove = hit(Part::Remove, i, removeLeft, t, removeLeft + kRemoveWidth, b);
        const bool hoverRow = hit(Part::Row, i, static_cast<float>(listLeft + 4), t, removeLeft, b);
        if (hoverRow || hoverRemove)
        {
            UiDraw::fillRect(device, static_cast<float>(listLeft + 4), t, static_cast<float>(listLeft + kListWidth - 4),
                b, kHover);
        }
        text(fitted(font, widen(g_servers[i].text()), kListWidth - kRemoveWidth - 24), listLeft + 10, y + 1,
            i == g_chosen ? kGold : kText);
        text(L"x", static_cast<int>(removeLeft) + 7, y + 1, hoverRemove ? kBad : kDim);
    }
    const float t = static_cast<float>(y), b = static_cast<float>(y + kRow);
    const bool hoverAdd = hit(Part::Add, 0, static_cast<float>(listLeft + 4), t,
        static_cast<float>(listLeft + kListWidth - 4), b);
    if (g_adding)
    {
        Server s;
        const bool valid = parse(g_typed, s);
        const bool caret = GetTickCount() / 300 % 2 == 0;
        text(fitted(font, widen(g_typed + (caret ? "_" : "")), kListWidth - 20), listLeft + 10, y + 1,
            valid || g_typed.empty() ? kText : kBad);
    }
    else
    {
        if (hoverAdd)
        {
            UiDraw::fillRect(device, static_cast<float>(listLeft + 4), t, static_cast<float>(listLeft + kListWidth - 4),
                b, kHover);
        }
        text(L"+ Add a server (host or host:port)", listLeft + 10, y + 1, kDim);
    }
}

bool LobbySelect::message(UINT message, WPARAM wParam)
{
    std::scoped_lock lock(g_lock);
    if (message == WM_LBUTTONUP)
    {
        return std::exchange(g_swallowUp, false);
    }
    if (g_swallowEscape && ((message == WM_CHAR && wParam == 0x1B) || (message == WM_KEYUP && wParam == VK_ESCAPE)))
    {
        g_swallowEscape = message != WM_KEYUP;
        return true;
    }
    // Typing a server: every key is the selector's (the menu acts on Enter and Esc).
    if (g_adding && g_open && !g_hits.empty() && (message == WM_KEYDOWN || message == WM_KEYUP || message == WM_CHAR))
    {
        if (message == WM_CHAR)
        {
            const auto ch = static_cast<wchar_t>(wParam);
            if (ch == 0x0D)
            {
                commitTyped();
            }
            else if (ch == 0x1B)
            {
                g_adding = false;
                g_typed.clear();
            }
            else if (ch == 0x08)
            {
                if (!g_typed.empty())
                {
                    g_typed.pop_back();
                }
            }
            else if (ch == 0x16)
            {
                g_typed += clipboardText();
            }
            else if (ch > 0x20 && ch < 0x7F && g_typed.size() < kMaxHost + 6)
            {
                g_typed += static_cast<char>(ch);
            }
        }
        return true;
    }
    if (g_open && message == WM_KEYDOWN && wParam == VK_ESCAPE)
    {
        g_open = false;
        g_swallowEscape = true;
        return true;
    }
    if ((message != WM_LBUTTONDOWN && message != WM_LBUTTONDBLCLK) || g_hits.empty())
    {
        return false;
    }
    const cMouse* mouse = cMouse::instance();
    const float mx = static_cast<float>(mouse->x), my = static_cast<float>(mouse->y);
    for (const Hit& h : g_hits)
    {
        if (mx < h.left || mx >= h.right || my < h.top || my >= h.bottom)
        {
            continue;
        }
        const size_t count = g_servers.size();
        switch (h.part)
        {
        case Part::Previous:
            choose((g_chosen < count ? g_chosen + count - 1 : 0) % count);
            break;
        case Part::Next:
            choose(g_chosen < count ? (g_chosen + 1) % count : 0);
            break;
        case Part::Toggle:
            g_open = !g_open;
            g_adding = false;
            break;
        case Part::Row:
            choose(h.index);
            g_open = false;
            break;
        case Part::Remove:
            remove(h.index);
            break;
        case Part::Add:
            if (g_adding)
            {
                commitTyped();
            }
            else
            {
                g_adding = true;
                g_typed.clear();
            }
            break;
        }
        g_swallowUp = true;
        return true;
    }
    // A click elsewhere closes the list and goes on to the menu.
    g_open = g_adding = false;
    return false;
}
