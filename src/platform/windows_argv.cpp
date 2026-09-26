#include "platform/windows_argv.hpp"

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

namespace biv::platform {

std::optional<std::vector<std::string>> windows_utf8_argv() {
    int wargc      = 0;
    LPWSTR* wargv  = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (!wargv || wargc <= 0)
        return std::nullopt;

    std::vector<std::string> out;
    out.reserve(static_cast<std::size_t>(wargc));
    for (int i = 0; i < wargc; ++i) {
        // -1 asks for the source to be treated as NUL-terminated and the
        // returned length to include the terminator.
        const int len = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
        if (len <= 0) {
            out.emplace_back();
            continue;
        }
        std::string s(static_cast<std::size_t>(len - 1), '\0');  // exclude the NUL; std::string adds its own
        WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, s.data(), len, nullptr, nullptr);
        out.push_back(std::move(s));
    }
    LocalFree(wargv);
    return out;
}

}  // namespace biv::platform

#else  // !_WIN32

namespace biv::platform {

std::optional<std::vector<std::string>> windows_utf8_argv() { return std::nullopt; }

}  // namespace biv::platform

#endif
