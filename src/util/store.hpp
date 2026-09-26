#pragma once

// store.hpp
//
// Abstract key/value store interface, shared store keys, and the
// cross-platform config directory helpers. The concrete implementation
// (SQLiteStore, backed by the vendored third_party/sqlite3 amalgamation)
// lives in store_sqlite.hpp/.cpp.
//
// Default location (via default_store_path()):
//   Linux    $XDG_CONFIG_HOME/bowsers_image_viewer/config.sqlite3
//            (falls back to ~/.config/bowsers_image_viewer/config.sqlite3)
//   macOS    ~/Library/Application Support/bowsers_image_viewer/config.sqlite3
//   Windows  %APPDATA%/bowsers_image_viewer/config.sqlite3

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "config.hpp"

namespace biv {

inline constexpr std::string_view kSTORE_FILENAME = "config.sqlite3";

inline constexpr std::string_view kSTORE_KEY_BG_COLOR_MODE        = "bg_color_mode";
inline constexpr std::string_view kSTORE_KEY_FAVORITES            = "favorites";
inline constexpr std::string_view kSTORE_KEY_SIDEBAR_VISIBLE      = "sidebar_visible";
inline constexpr std::string_view kSTORE_KEY_SIDEBAR_WIDTH        = "sidebar_width";
inline constexpr std::string_view kSTORE_KEY_SIDEBAR_TAB          = "sidebar_tab";
inline constexpr std::string_view kSTORE_KEY_THUMBSTRIP_VISIBLE   = "thumbstrip_visible";
inline constexpr std::string_view kSTORE_KEY_SORT_OPTION          = "sort_option";
inline constexpr std::string_view kSTORE_KEY_SORT_DESCENDING      = "sort_descending";
inline constexpr std::string_view kSTORE_KEY_WINDOW_WIDTH         = "window_width";
inline constexpr std::string_view kSTORE_KEY_WINDOW_HEIGHT        = "window_height";
inline constexpr std::string_view kSTORE_KEY_WINDOW_X             = "window_x";
inline constexpr std::string_view kSTORE_KEY_WINDOW_Y             = "window_y";
inline constexpr std::string_view kSTORE_KEY_WINDOW_SCALE         = "window_scale";
inline constexpr std::string_view kSTORE_KEY_DARK_MODE            = "dark_mode";
inline constexpr std::string_view kSTORE_KEY_VIDEO_VOLUME         = "video_volume";
inline constexpr std::string_view kSTORE_KEY_VIDEO_TIME_REMAINING = "video_time_display_remaining";
inline constexpr std::string_view kSTORE_KEY_THUMBSTRIP_HEIGHT    = "thumbstrip_height";

class Store {
public:
    virtual ~Store() = default;

    [[nodiscard]] virtual std::optional<std::string> get(std::string_view key) const = 0;
    virtual void set(std::string_view key, std::string_view value)                   = 0;
    virtual bool erase(std::string_view key)                                         = 0;
    virtual void load()                                                              = 0;
    virtual void save() const                                                        = 0;

    // True if this store found its backing data corrupted on open and
    // replaced it with a fresh, empty one (the user's settings were reset).
    // Default false; backends with nothing to recover from need not
    // override it.
    [[nodiscard]] virtual bool was_recovered() const noexcept { return false; }

    [[nodiscard]] virtual std::filesystem::path recovery_backup_path() const { return {}; }
};

// Cross-platform per-user config directory for this app.
inline std::filesystem::path default_config_dir() {
#if defined(_WIN32)
    if (const char* appdata = std::getenv("APPDATA"); appdata && *appdata)
        return std::filesystem::path(appdata) / kAPP_ID;
    if (const char* profile = std::getenv("USERPROFILE"); profile && *profile)
        return std::filesystem::path(profile) / "AppData" / "Roaming" / kAPP_ID;
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home) / "Library" / "Application Support" / kAPP_ID;
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return std::filesystem::path(xdg) / kAPP_ID;
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home) / ".config" / kAPP_ID;
#endif
    return std::filesystem::current_path() / ("." + std::string(kAPP_ID));
}

inline std::filesystem::path default_store_path() {
    return default_config_dir() / kSTORE_FILENAME;
}

// Encode a list into a single store value. Items are joined with '|';
// '\' and '|' inside an item are escaped as '\\' and '\|'.
inline std::string encode_list(const std::vector<std::string>& items) {
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i)
            out.push_back('|');
        for (char c : items[i]) {
            if (c == '\\' || c == '|')
                out.push_back('\\');
            out.push_back(c);
        }
    }
    return out;
}

inline std::vector<std::string> decode_list(std::string_view encoded) {
    std::vector<std::string> items;
    if (encoded.empty())
        return items;
    std::string cur;
    for (std::size_t i = 0; i < encoded.size(); ++i) {
        if (encoded[i] == '\\' && i + 1 < encoded.size()) {
            cur.push_back(encoded[++i]);
            continue;
        }
        if (encoded[i] == '|') {
            items.push_back(std::move(cur));
            cur.clear();
            continue;
        }
        cur.push_back(encoded[i]);
    }
    items.push_back(std::move(cur));
    return items;
}

}  // namespace biv