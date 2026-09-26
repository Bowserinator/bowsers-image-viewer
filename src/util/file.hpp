#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace biv {

// UTF-8 <-> std::filesystem::path, correct on Windows too (where .string() is
// the ANSI code page). Used by the folder tree and the video player.
inline std::string path_to_utf8(const std::filesystem::path& p) {
    const auto u = p.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

inline std::filesystem::path path_from_utf8(std::string_view s) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

// fopen() that actually works with non-ASCII paths on Windows.
//
// std::fopen(path.string().c_str(), ...) is broken there: .string() goes
// through the ANSI code page, so any path byte outside it is lost before
// fopen even runs, and the C library's narrow fopen() decodes what's left
// with that same code page rather than UTF-8. Using path.c_str() directly
// sidesteps both problems: on Windows that's the path's native wchar_t*
// representation, fed to _wfopen(); everywhere else it's the native
// (UTF-8-on-sane-systems) char* representation, fed to plain fopen().
// `mode` is always plain ASCII ("rb", "wb", ...), so a byte-for-byte
// widening is enough there.
inline std::FILE* fopen_path(const std::filesystem::path& path, const char* mode) {
#if defined(_WIN32)
    std::wstring wmode(mode, mode + std::string_view(mode).size());
    return _wfopen(path.c_str(), wmode.c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

// Reads up to `limit` bytes from the start of a file.
//
// Returns nullopt if the file can't be opened. With `require_complete` (the
// default, for whole-file decodes) a file longer than `limit` is also refused
// rather than silently truncated; pass false to read just a prefix, e.g. to
// sniff the format. The size comes from the filesystem, never from the
// contents, and the result is trimmed to what fread() actually delivered.
inline std::optional<std::vector<std::byte>> read_file_bytes(
    const std::filesystem::path& path, std::uintmax_t limit, bool require_complete = true) {
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec || (require_complete && size > limit))
        return std::nullopt;

    std::FILE* f = fopen_path(path, "rb");
    if (!f)
        return std::nullopt;
    std::vector<std::byte> bytes(static_cast<std::size_t>(std::min(size, limit)));
    const std::size_t got = std::fread(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    bytes.resize(got);  // the file may have shrunk since file_size()
    return bytes;
}

inline std::string format_bytes(std::uintmax_t bytes) {
    constexpr double kKB = 1024.0;
    constexpr double kMB = 1024.0 * 1024.0;
    constexpr double kGB = 1024.0 * 1024.0 * 1024.0;

    char buf[32];
    if (bytes < kKB)
        std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
    else if (static_cast<double>(bytes) < kMB)
        std::snprintf(buf, sizeof(buf), "%.1f KB", static_cast<double>(bytes) / kKB);
    else if (static_cast<double>(bytes) < kGB)
        std::snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / kMB);
    else
        std::snprintf(buf, sizeof(buf), "%.1f GB", static_cast<double>(bytes) / kGB);
    return std::string(buf);
}

inline std::uintmax_t file_size_of(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    return ec ? 0 : size;
}

}  // namespace biv
