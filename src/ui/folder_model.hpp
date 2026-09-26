#pragma once

// folder_model.hpp
//
// A slint::Model that presents a lazily-loaded directory tree as a flat list
// of rows, for use with a Slint ListView (which only instantiates the rows
// that are on screen).
//
//  - Only directories and image files (config.hpp: kALL_EXTENSIONS) are listed.
//  - A directory's contents are read only when it is expanded, on a worker
//    thread, then sorted (dirs first, natural order) and inserted in ONE
//    notification. Expand/collapse are single notify_row_added/removed calls.
//  - Entries are stored compactly (one name arena + 12-byte records per
//    listing, 8 bytes per visible row), so ~1M files is a few tens of MB.
//  - row_data() is O(1) and never touches the disk.
//
// All public methods must be called on the UI thread.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "app.h"  // generated from ui/app.slint: FolderRow
#include "filesystem/directory_model.hpp"  // SortMode

namespace biv {

class FolderModel final : public slint::Model<FolderRow>, public std::enable_shared_from_this<FolderModel> {
public:
    static std::shared_ptr<FolderModel> create();
    ~FolderModel() override;

    // Fired on the UI thread when the ROOT listing starts/finishes loading
    // (sub-directories show a per-row "loading" state instead).
    std::function<void(bool /*loading*/)> on_loading_changed;

    // slint::Model
    [[nodiscard]] std::size_t row_count() const override { return m_rows.size(); }

    [[nodiscard]] std::optional<FolderRow> row_data(std::size_t row) const override;

    // Replaces the whole tree with the contents of `dir` (async).
    void set_root(const std::filesystem::path& dir);

    // Sets the sort for all listings (dirs still first). Re-scans from the
    // current root, so expanded sub-folders collapse.
    void set_sort(SortMode mode, bool descending);

    // Marks `file` as the current image. If its folder is outside the current
    // tree, the tree is re-rooted at that folder first.
    void show_file(const std::filesystem::path& file);

    // Expands/collapses a directory row (no-op for file rows). Expanding while
    // a load is in flight cancels it.
    void toggle(std::size_t row);

    [[nodiscard]] bool is_dir_row(std::size_t row) const;
    [[nodiscard]] std::optional<std::filesystem::path> file_at(std::size_t row) const;

private:
    FolderModel() = default;

    struct Entry {
        std::uint32_t off  = 0;  // name offset into Listing::arena
        std::uint32_t len  = 0;  // name length in bytes (UTF-8)
        std::uint8_t flags = 0;  // kDir | kExpanded | kLoading | kVideo
        std::uint64_t size = 0;  // bytes (0 for dirs)
        std::int64_t mtime = 0;  // raw file_time_type ticks, only compared
    };

    enum : std::uint8_t { kDir = 1, kExpanded = 2, kLoading = 4, kVideo = 8 };

    struct RowRef {
        std::uint32_t listing                = 0;
        std::uint32_t index                  = 0;
        bool operator==(const RowRef&) const = default;
    };

    // The children of one directory.
    struct Listing {
        std::filesystem::path dir;
        std::string key;  // normalised dir path, for current-file matching
        int depth = 0;
        std::string arena;  // all names, back to back
        std::vector<Entry> entries;
        std::optional<RowRef> parent;  // the row that expanded it; nullopt = root
    };

    struct Job {
        std::atomic<bool> cancel{false};
    };

    struct ScanResult {
        std::string arena;
        std::vector<Entry> entries;
    };

    static ScanResult scan_directory(
        const std::filesystem::path& dir, SortMode mode, bool descending, const std::atomic<bool>& cancel);
    static std::string make_key(const std::filesystem::path& p);

    void start_scan(std::uint32_t listing_id);
    void finish_scan(std::uint32_t listing_id, const std::shared_ptr<Job>& job, ScanResult result);
    void expand(std::size_t row);
    void collapse(std::size_t row);
    void free_listing(std::uint32_t id);
    void prune_orphans();
    void cancel_all_jobs();
    void set_current(const std::string& dir_key, std::string name);

    [[nodiscard]] std::optional<std::size_t> find_row(const RowRef& ref) const;
    [[nodiscard]] std::optional<RowRef> locate_current() const;
    [[nodiscard]] std::string_view name_of(const Listing& l, const Entry& e) const noexcept;

    std::vector<RowRef> m_rows;                                      // visible rows, top to bottom
    std::vector<std::unique_ptr<Listing>> m_listings;                // index = listing id; null = freed
    std::unordered_map<std::uint32_t, std::shared_ptr<Job>> m_jobs;  // in-flight scans

    SortMode m_sort_mode   = SortMode::Name;
    bool m_sort_descending = false;

    std::string m_root_key;
    std::string m_cur_dir;   // key of the current image's folder
    std::string m_cur_name;  // current image's file name (UTF-8)
};

}  // namespace biv