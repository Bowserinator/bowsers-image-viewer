#pragma once

// screen.hpp
//
// Work-area size in physical pixels. Used to clamp the restored window so
// it never exceeds the monitor containing the window on Windows, and the
// primary display on other platforms.
//
//   Windows  SPI_GETWORKAREA (excludes the taskbar)
//   macOS    CGDisplayPixelsWide/High of the main display
//   Linux    X11 DisplayWidth/Height via dlopen (no libX11 link needed)
//
// `valid` is false when the size could not be queried — callers must not
// treat the struct defaults as a real monitor and shrink the window to them.
// Convert to Slint logical pixels with Window::scale_factor() at the call site.

#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#elif defined(__APPLE__)
    #include <CoreGraphics/CoreGraphics.h>
#else
    #include <dlfcn.h>
#endif

namespace biv::platform {

struct ScreenSize {
    float width  = 0.0f;
    float height = 0.0f;
    bool valid   = false;
};

#if defined(_WIN32)
inline ScreenSize work_area_at_px(long x, long y) {
    POINT point{static_cast<LONG>(x), static_cast<LONG>(y)};
    const HMONITOR monitor = MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
    if (!monitor)
        return {};

    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info))
        return {};

    const float w = static_cast<float>(info.rcWork.right - info.rcWork.left);
    const float h = static_cast<float>(info.rcWork.bottom - info.rcWork.top);
    if (w >= 1.0f && h >= 1.0f)
        return {w, h, true};
    return {};
}
#endif

inline ScreenSize primary_work_area_px() {
#if defined(_WIN32)
    RECT r{};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &r, 0)) {
        const float w = static_cast<float>(r.right - r.left);
        const float h = static_cast<float>(r.bottom - r.top);
        if (w >= 1.0f && h >= 1.0f)
            return {w, h, true};
    }
    const float w = static_cast<float>(GetSystemMetrics(SM_CXSCREEN));
    const float h = static_cast<float>(GetSystemMetrics(SM_CYSCREEN));
    if (w >= 1.0f && h >= 1.0f)
        return {w, h, true};
    return {};
#elif defined(__APPLE__)
    const auto id = CGMainDisplayID();
    const float w = static_cast<float>(CGDisplayPixelsWide(id));
    const float h = static_cast<float>(CGDisplayPixelsHigh(id));
    if (w >= 1.0f && h >= 1.0f)
        return {w, h, true};
    return {};
#else
    void* lib = dlopen("libX11.so.6", RTLD_LAZY);
    if (!lib)
        lib = dlopen("libX11.so", RTLD_LAZY);
    if (!lib)
        return {};

    using OpenFn          = void* (*)(const char*);
    using CloseFn         = int (*)(void*);
    using DefaultScreenFn = int (*)(void*);
    using DimFn           = int (*)(void*, int);

    auto x_open   = reinterpret_cast<OpenFn>(dlsym(lib, "XOpenDisplay"));
    auto x_close  = reinterpret_cast<CloseFn>(dlsym(lib, "XCloseDisplay"));
    auto x_screen = reinterpret_cast<DefaultScreenFn>(dlsym(lib, "XDefaultScreen"));
    auto x_width  = reinterpret_cast<DimFn>(dlsym(lib, "XDisplayWidth"));
    auto x_height = reinterpret_cast<DimFn>(dlsym(lib, "XDisplayHeight"));

    ScreenSize out{};
    if (x_open && x_close && x_screen && x_width && x_height) {
        if (void* dpy = x_open(nullptr)) {
            const int scr = x_screen(dpy);
            const float w = static_cast<float>(x_width(dpy, scr));
            const float h = static_cast<float>(x_height(dpy, scr));
            if (w >= 1.0f && h >= 1.0f)
                out = {w, h, true};
            x_close(dpy);
        }
    }
    dlclose(lib);
    return out;
#endif
}

}  // namespace biv::platform