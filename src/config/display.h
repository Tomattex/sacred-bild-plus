#pragma once

// [Display] in SacredBild.ini (read by config.cpp).
namespace Config
{
    struct Display
    {
        // Render resolution; 0 = desktop size. 1024x768 runs the game unpatched.
        int width = 0;
        int height = 0;
        // [Display] Mode. Borderless: a frameless window over the whole monitor, the render resolution stretched to
        // it. Fullscreen: exclusive, the display switches to the render resolution (Direct3D 9 backend; a size that is
        // no display mode falls back to Borderless). Windowed: a framed window whose client area is the render
        // resolution.
        enum class Mode
        {
            Borderless,
            Fullscreen,
            Windowed,
        };
        Mode mode = Mode::Borderless;
        // Automation runs (-gear-auto): a framed window of the size above, placed at the work area's bottom-right
        // corner unless -gear-window=WxH+X+Y gave a position (client area's top-left, in screen pixels).
        bool automationWindow = false;
        bool hasPosition = false;
        int x = 0;
        int y = 0;
        // A framed window can be resized and maximized; the game keeps its resolution and the frame is scaled to the
        // window (aspect ratio kept, black bars). Not in automation runs.
        bool resizable = true;
        bool clipCursor = true;       // mouse confined to the game window while it is in the foreground
        int fpsLimit = 60;            // frame limit in game and in the menus (the game uses 60); 0 = off
        int fpsLimitInactive = 20;    // frame limit (menus and game) while none of the game's windows is in the foreground; 0 = off
        // Direct3D 9 backend: wait for the display's refresh when presenting, and how many frames the CPU may
        // queue ahead of the GPU (1 = lowest input latency).
        bool vsync = true;
        int maxFrameLatency = 1;
    };
    inline Display display;
}
