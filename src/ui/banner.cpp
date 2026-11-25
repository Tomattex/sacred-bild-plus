#include "ui/banner.h"
#include "log.h"

#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace
{
    // Not exported by every SDK library this links against.
    constexpr GUID kClsidWicImagingFactory = {0xcacaf262, 0x9370, 0x4615, {0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a}};
    constexpr GUID kWicPixelFormat32bppBGRA = {0x6fddc324, 0x4e03, 0x4bfe, {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0f}};
    constexpr GUID kWicPixelFormat32bppPBGRA = {0x6fddc324, 0x4e03, 0x4bfe, {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x10}};

    constexpr const wchar_t* kSerif = L"Palatino Linotype";
    constexpr COLORREF kGold = RGB(214, 176, 104);
    constexpr COLORREF kCaption = RGB(168, 140, 94);
    constexpr uint32_t kLeather = 0x221B15;     // the texture's middle tone, 0xRRGGBB
    constexpr uint32_t kEdge = 0x68503A;        // the line along the bottom

    ComPtr<IWICImagingFactory> wic()
    {
        ComPtr<IWICImagingFactory> factory;
        CoCreateInstance(kClsidWicImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
        return factory;
    }

    // The image as BGRA rows, or empty.
    std::vector<uint32_t> load(const std::wstring& path, int& width, int& height)
    {
        std::vector<uint32_t> pixels;
        ComPtr<IWICImagingFactory> factory = wic();
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> converter;
        UINT w = 0, h = 0;
        if (!factory ||
            FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) ||
            FAILED(decoder->GetFrame(0, &frame)) || FAILED(factory->CreateFormatConverter(&converter)) ||
            FAILED(converter->Initialize(frame.Get(), kWicPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                WICBitmapPaletteTypeCustom)) ||
            FAILED(converter->GetSize(&w, &h)) || !w || !h)
        {
            return pixels;
        }
        pixels.resize(size_t(w) * h);
        if (FAILED(converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(pixels.size() * 4), reinterpret_cast<BYTE*>(pixels.data()))))
        {
            pixels.clear();
            return pixels;
        }
        width = static_cast<int>(w);
        height = static_cast<int>(h);
        return pixels;
    }

    // The logo's white background out, as premultiplied BGRA. The background is the pure white that touches the image's
    // edge or fills a letter's counter (the inside of an O), grown over the near white around it (the glows, the
    // letters' soft edges); there a pixel is taken as its colour over white with the least alpha that explains it
    // (alpha = 255 - its lowest channel). The rest, the gold inside the letters' dark outlines, stays opaque.
    void keyWhite(std::vector<uint32_t>& pixels, int width, int height)
    {
        constexpr int kWhite = 238, kNearWhite = 200, kCounter = 150;
        const size_t count = pixels.size();
        std::vector<uint8_t> low(count);
        for (size_t i = 0; i < count; ++i)
        {
            const uint32_t p = pixels[i];
            low[i] = static_cast<uint8_t>(std::min({p & 0xFF, (p >> 8) & 0xFF, (p >> 16) & 0xFF}));
        }
        std::vector<uint8_t> background(count), seen(count);
        std::vector<int> grow, part;
        const auto neighbours = [&](int i, auto&& visit) {
            const int x = i % width, y = i / width;
            if (x > 0) visit(i - 1);
            if (x + 1 < width) visit(i + 1);
            if (y > 0) visit(i - width);
            if (y + 1 < height) visit(i + width);
        };
        for (int start = 0; start < static_cast<int>(count); ++start)
        {
            if (seen[start] || low[start] <= kWhite)
            {
                continue;
            }
            part.assign(1, start);
            seen[start] = 1;
            bool edge = false;
            for (size_t k = 0; k < part.size(); ++k)
            {
                const int i = part[k];
                const int x = i % width, y = i / width;
                edge = edge || x == 0 || y == 0 || x == width - 1 || y == height - 1;
                neighbours(i, [&](int j) {
                    if (!seen[j] && low[j] > kWhite)
                    {
                        seen[j] = 1;
                        part.push_back(j);
                    }
                });
            }
            if (edge || part.size() >= kCounter)
            {
                for (int i : part)
                {
                    background[i] = 1;
                    grow.push_back(i);
                }
            }
        }
        for (size_t k = 0; k < grow.size(); ++k)
        {
            neighbours(grow[k], [&](int j) {
                if (!background[j] && low[j] > kNearWhite)
                {
                    background[j] = 1;
                    grow.push_back(j);
                }
            });
        }
        for (size_t i = 0; i < count; ++i)
        {
            const uint32_t p = pixels[i] & 0xFFFFFF;
            if (!background[i])
            {
                pixels[i] = p | 0xFF000000;
                continue;
            }
            // Premultiplied: the colour over white minus the white's share, which is the lowest channel.
            const uint32_t l = low[i];
            pixels[i] = ((255 - l) << 24) | ((((p >> 16) & 0xFF) - l) << 16) | ((((p >> 8) & 0xFF) - l) << 8) | ((p & 0xFF) - l);
        }
    }

    // Premultiplied BGRA scaled to the size, or empty.
    std::vector<uint32_t> scale(const std::vector<uint32_t>& pixels, int width, int height, int toWidth, int toHeight)
    {
        std::vector<uint32_t> out;
        ComPtr<IWICImagingFactory> factory = wic();
        ComPtr<IWICBitmap> bitmap;
        ComPtr<IWICBitmapScaler> scaler;
        if (!factory || toWidth <= 0 || toHeight <= 0 ||
            FAILED(factory->CreateBitmapFromMemory(width, height, kWicPixelFormat32bppPBGRA, width * 4,
                static_cast<UINT>(pixels.size() * 4), reinterpret_cast<BYTE*>(const_cast<uint32_t*>(pixels.data())), &bitmap)) ||
            FAILED(factory->CreateBitmapScaler(&scaler)) ||
            FAILED(scaler->Initialize(bitmap.Get(), toWidth, toHeight, WICBitmapInterpolationModeFant)))
        {
            return out;
        }
        out.resize(size_t(toWidth) * toHeight);
        if (FAILED(scaler->CopyPixels(nullptr, toWidth * 4, static_cast<UINT>(out.size() * 4), reinterpret_cast<BYTE*>(out.data()))))
        {
            out.clear();
        }
        return out;
    }

    uint32_t hash(uint32_t x)
    {
        x ^= x >> 16;
        x *= 0x7FEB352D;
        x ^= x >> 15;
        x *= 0x846CA68B;
        return x ^ (x >> 16);
    }

    float noise(int x, int y)
    {
        return static_cast<float>(hash(static_cast<uint32_t>(x) * 73856093u ^ static_cast<uint32_t>(y) * 19349663u) & 0xFFFF) / 65535.0f;
    }

    // Dark leather: a soft mottle a fifth of the height across, a fine grain, darker towards the edges.
    void leather(uint32_t* out, int width, int height)
    {
        const float cell = std::max(height / 5.0f, 4.0f);
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                const float fx = x / cell, fy = y / cell;
                const int ix = static_cast<int>(fx), iy = static_cast<int>(fy);
                const float tx = fx - ix, ty = fy - iy;
                const float sx = tx * tx * (3 - 2 * tx), sy = ty * ty * (3 - 2 * ty);
                const float top = noise(ix, iy) + (noise(ix + 1, iy) - noise(ix, iy)) * sx;
                const float bottom = noise(ix, iy + 1) + (noise(ix + 1, iy + 1) - noise(ix, iy + 1)) * sx;
                const float mottle = top + (bottom - top) * sy;
                const float dx = (x - width / 2.0f) / (width / 2.0f), dy = (y - height / 2.0f) / (height / 2.0f);
                const float vignette = std::clamp(1.12f - 0.5f * std::sqrt(dx * dx + dy * dy), 0.3f, 1.0f);
                const float light = (0.86f + 0.28f * mottle) * vignette;
                const float grain = (noise(x + 7919, y) - 0.5f) * 7.0f;
                uint32_t pixel = 0xFF000000;
                for (int shift : {16, 8, 0})
                {
                    const float c = ((kLeather >> shift) & 0xFF) * light + grain;
                    pixel |= static_cast<uint32_t>(std::clamp(c, 0.0f, 255.0f)) << shift;
                }
                out[size_t(y) * width + x] = pixel;
            }
        }
    }

    HFONT serif(int height, int weight, bool italic)
    {
        return CreateFontW(-height, 0, 0, 0, weight, italic, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, kSerif);
    }
}

Ui::Banner::Banner(const std::wstring& gameDir, std::wstring caption) : m_caption(std::move(caption))
{
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    m_logo = load(gameDir + L"\\readme\\Gold.jpg", m_logoWidth, m_logoHeight);
    if (m_logo.empty())
    {
        LOG("Settings window: no readme\\Gold.jpg, the banner draws the title instead");
    }
    else
    {
        keyWhite(m_logo, m_logoWidth, m_logoHeight);
    }
    if (SUCCEEDED(com))
    {
        CoUninitialize();
    }
}

Ui::Banner::~Banner()
{
    if (m_bitmap)
    {
        DeleteObject(m_bitmap);
    }
}

void Ui::Banner::render(HDC dc, int width, int height)
{
    if (m_bitmap)
    {
        DeleteObject(m_bitmap);
        m_bitmap = nullptr;
    }
    BITMAPINFO info = {};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB};
    void* bits = nullptr;
    m_bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!m_bitmap)
    {
        return;
    }
    m_width = width;
    m_height = height;
    auto* out = static_cast<uint32_t*>(bits);
    leather(out, width, height);

    // The logo, 88 % of the height, centered.
    bool logo = false;
    if (!m_logo.empty())
    {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const int lh = height * 88 / 100, lw = m_logoWidth * lh / m_logoHeight;
        const std::vector<uint32_t> scaled = scale(m_logo, m_logoWidth, m_logoHeight, lw, lh);
        if (SUCCEEDED(com))
        {
            CoUninitialize();
        }
        const int left = (width - lw) / 2, top = (height - lh) / 2;
        for (int y = 0; y < lh && !scaled.empty(); ++y)
        {
            for (int x = 0; x < lw; ++x)
            {
                if (left + x < 0 || left + x >= width || top + y < 0 || top + y >= height)
                {
                    continue;
                }
                const uint32_t s = scaled[size_t(y) * lw + x];
                uint32_t& d = out[size_t(top + y) * width + left + x];
                const uint32_t keep = 255 - (s >> 24);
                uint32_t pixel = 0xFF000000;
                for (int shift : {16, 8, 0})
                {
                    const uint32_t c = ((s >> shift) & 0xFF) + ((d >> shift) & 0xFF) * keep / 255;
                    pixel |= std::min(c, 255u) << shift;
                }
                d = pixel;
            }
        }
        logo = !scaled.empty();
    }
    for (int x = 0; x < width; ++x)
    {
        out[size_t(height - 1) * width + x] = 0xFF000000 | kEdge;
    }

    HDC mem = CreateCompatibleDC(dc);
    const HGDIOBJ oldBitmap = SelectObject(mem, m_bitmap);
    SetBkMode(mem, TRANSPARENT);
    if (!logo)
    {
        // The title where the logo is missing, with a shadow.
        HFONT title = serif(height * 40 / 100, FW_BOLD, false);
        const HGDIOBJ oldFont = SelectObject(mem, title);
        RECT r = {0, 0, width, height - height / 10};
        OffsetRect(&r, 2, 2);
        SetTextColor(mem, RGB(8, 6, 4));
        DrawTextW(mem, L"SACRED GOLD", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        OffsetRect(&r, -2, -2);
        SetTextColor(mem, kGold);
        DrawTextW(mem, L"SACRED GOLD", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(mem, oldFont);
        DeleteObject(title);
    }
    if (!m_caption.empty())
    {
        HFONT captionFont = serif(std::max(height * 17 / 100, 12), FW_NORMAL, true);
        const HGDIOBJ oldFont = SelectObject(mem, captionFont);
        const int margin = height / 12;
        RECT r = {0, 0, width - margin, height - margin};
        SetTextColor(mem, kCaption);
        DrawTextW(mem, m_caption.c_str(), -1, &r, DT_RIGHT | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(mem, oldFont);
        DeleteObject(captionFont);
    }
    SelectObject(mem, oldBitmap);
    DeleteDC(mem);
}

void Ui::Banner::paint(HDC dc, const RECT& rect)
{
    const int width = rect.right - rect.left, height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0)
    {
        return;
    }
    if (!m_bitmap || width != m_width || height != m_height)
    {
        render(dc, width, height);
    }
    if (!m_bitmap)
    {
        return;
    }
    HDC mem = CreateCompatibleDC(dc);
    const HGDIOBJ old = SelectObject(mem, m_bitmap);
    BitBlt(dc, rect.left, rect.top, width, height, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteDC(mem);
}
