#pragma once

#include <filesystem>
#include <string>
#include <system_error>
#include <type_traits>
#include <variant>

namespace biv {

// A plain image file living on disk.
struct FileSource {
    std::filesystem::path path;
};

// An image entry living inside a .zip/.rar archive; `archive_path` is the
// archive on disk, `entry_name` is the path of the image *inside* the archive.
struct ArchiveSource {
    std::filesystem::path archive_path;
    std::string entry_name;
};

using ImageSource = std::variant<FileSource, ArchiveSource>;

// Human-readable label used in the status bar / window title.
inline std::string display_name(const ImageSource& src) {
    return std::visit(
        [](const auto& s) -> std::string {
            using T = std::decay_t<decltype(s)>;
            if constexpr (std::is_same_v<T, FileSource>)
                return s.path.filename().string();
            else
                return s.entry_name;
        },
        src);
}

inline bool is_archive_entry(const ImageSource& src) {
    return std::holds_alternative<ArchiveSource>(src);
}

// Stable identity used for favorites / config. Files are weakly-canonical
// generic paths; archive entries are `archive::entry`.
inline std::string source_key(const ImageSource& src) {
    auto abs_generic = [](const std::filesystem::path& p) {
        std::error_code ec;
        auto a = std::filesystem::weakly_canonical(p, ec);
        if (ec)
            a = std::filesystem::absolute(p, ec);
        if (ec)
            a = p;
        return a.lexically_normal().generic_string();
    };
    return std::visit(
        [&](const auto& s) -> std::string {
            using T = std::decay_t<decltype(s)>;
            if constexpr (std::is_same_v<T, FileSource>)
                return abs_generic(s.path);
            else
                return abs_generic(s.archive_path) + "::" + s.entry_name;
        },
        src);
}

}  // namespace biv
