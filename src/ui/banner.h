#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

// The settings window's banner: Sacred Gold's logo from the install (readme\Gold.jpg, gold letters on white, the white
// keyed out) over the red glow of the main menu's portal, made here, with a caption in its bottom right corner. Without
// the file, the game's title in gold type. No artwork ships with SacredBild.
namespace Ui
{
    class Banner
    {
    public:
        // `face`: the caption's and the fallback title's typeface (the game's uncial where the caller loaded it).
        Banner(const std::wstring& gameDir, std::wstring caption, std::wstring face);
        ~Banner();
        Banner(const Banner&) = delete;
        Banner& operator=(const Banner&) = delete;

        // Draws it into the rect (pixels); the picture is made again when the rect's size changes (a DPI change).
        void paint(HDC dc, const RECT& rect);

    private:
        void render(HDC dc, int width, int height);

        std::wstring m_caption;
        std::wstring m_face;
        std::vector<uint32_t> m_logo;   // premultiplied BGRA
        int m_logoWidth = 0;
        int m_logoHeight = 0;
        HBITMAP m_bitmap = nullptr;
        int m_width = 0;
        int m_height = 0;
    };
}
