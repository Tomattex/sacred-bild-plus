#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

// The settings window's banner: Sacred Gold's logo from the install (readme\Gold.jpg, gold letters on white, the white
// keyed out) on a dark leather texture made here, with a caption in its bottom right corner. Without the file, the
// game's title in gold type. No artwork ships with SacredBild.
namespace Ui
{
    class Banner
    {
    public:
        Banner(const std::wstring& gameDir, std::wstring caption);
        ~Banner();
        Banner(const Banner&) = delete;
        Banner& operator=(const Banner&) = delete;

        // Draws it into the rect (pixels); the picture is made again when the rect's size changes (a DPI change).
        void paint(HDC dc, const RECT& rect);

    private:
        void render(HDC dc, int width, int height);

        std::wstring m_caption;
        std::vector<uint32_t> m_logo;   // premultiplied BGRA
        int m_logoWidth = 0;
        int m_logoHeight = 0;
        HBITMAP m_bitmap = nullptr;
        int m_width = 0;
        int m_height = 0;
    };
}
