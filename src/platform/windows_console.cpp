#include "platform/windows_console.hpp"

#if defined(_WIN32)

    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef _CRT_SECURE_NO_WARNINGS
        #define _CRT_SECURE_NO_WARNINGS  // std::freopen
    #endif
    #include <windows.h>

    #include <cstdio>
    #include <iostream>

namespace biv::platform {

namespace {

// A GUI-subsystem process gets null/invalid standard handles unless the parent
// redirected them.
bool handle_missing(DWORD which) noexcept {
    const HANDLE h = GetStdHandle(which);
    return h == nullptr || h == INVALID_HANDLE_VALUE;
}

}  // namespace

void attach_parent_console() noexcept {
    const bool need_out = handle_missing(STD_OUTPUT_HANDLE);
    const bool need_err = handle_missing(STD_ERROR_HANDLE);
    const bool need_in  = handle_missing(STD_INPUT_HANDLE);
    if (!need_out && !need_err && !need_in)
        return;  // everything redirected (or we already own a console)

    // Fails when there is no parent console, e.g. launched from Explorer. Fine.
    if (!AttachConsole(ATTACH_PARENT_PROCESS))
        return;

    if (need_out)
        std::freopen("CONOUT$", "w", stdout);
    if (need_err)
        std::freopen("CONOUT$", "w", stderr);
    if (need_in)
        std::freopen("CONIN$", "r", stdin);

    // iostreams stay in sync with stdio by default; just drop any error state
    // they picked up while the handles were invalid.
    std::cout.clear();
    std::cerr.clear();
    std::clog.clear();
    std::cin.clear();
}

}  // namespace biv::platform

#else  // !_WIN32

namespace biv::platform {

void attach_parent_console() noexcept {}

}  // namespace biv::platform

#endif
