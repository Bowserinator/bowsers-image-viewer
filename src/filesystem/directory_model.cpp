#include "directory_model.hpp"

#include "butil/log.hpp"
#include "butil/str.hpp"
#include "butil/util.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <string_view>
#include <system_error>

#include "archive_source.hpp"
#include "config.hpp"

namespace biv {

namespace {

bool has_image_extension(const std::filesystem::path& p) {
    return butil::contains(kALL_EXTENSIONS, butil::lower(p.extension().string()));
}

// A sibling paired with what sort_siblings() needs to order it by Size/MTime.
// mtime_ticks is a raw clock tick count, not a calendar time: it is only
// ever compared against other ticks from the same clock within one sort
// call, never persisted or shown to the user, so no epoch conversion is
// needed.
struct SortableSibling {
    ImageSource source;
    std::uintmax_t size      = 0;
    std::int64_t mtime_ticks = 0;
};

// Primary key per `mode`; ties (and Name mode itself) fall back to
// natural_less on the display name, so the result is always fully
// deterministic regardless of mode. Always sorted ascending first, then
// reversed as a whole when `descending` is set -- including the name
// tie-break -- so descending is a mirror image of ascending rather than
// leaving ties in their original (name-ascending) order.
void sort_siblings(std::vector<SortableSibling>& items, SortMode mode, bool descending) {
    std::ranges::sort(items, [mode](const SortableSibling& a, const SortableSibling& b) {
        switch (mode) {
            case SortMode::Size:
                if (a.size != b.size)
                    return a.size < b.size;
                break;
            case SortMode::MTime:
                if (a.mtime_ticks != b.mtime_ticks)
                    return a.mtime_ticks < b.mtime_ticks;
                break;
            case SortMode::Name:
                break;
        }
        return natural_less(display_name(a.source), display_name(b.source));
    });
    if (descending)
        std::ranges::reverse(items);
}

std::vector<ImageSource> extract_sources(std::vector<SortableSibling>&& items) {
    std::vector<ImageSource> out;
    out.reserve(items.size());
    for (auto& it : items)
        out.push_back(std::move(it.source));
    return out;
}

// Lists the image files directly inside `folder` (no recursion), sorted per
// `sort`. Size/mtime come straight off directory_iterator's cached stat
// (the OS has already fetched them to answer is_regular_file()), so this
// costs no syscalls beyond the old name-only listing.
std::vector<ImageSource> list_folder_images(const std::filesystem::path& folder, SortMode sort, bool descending) {
    std::vector<SortableSibling> items;
    for (auto& entry : std::filesystem::directory_iterator(folder)) {
        if (!entry.is_regular_file() || !has_image_extension(entry.path()))
            continue;
        std::error_code sec, mec;
        const auto size  = entry.file_size(sec);
        const auto mtime = entry.last_write_time(mec);
        items.push_back(SortableSibling{
            .source      = FileSource{entry.path()},
            .size        = sec ? 0 : size,
            .mtime_ticks = mec ? 0 : mtime.time_since_epoch().count(),
        });
    }
    sort_siblings(items, sort, descending);
    return extract_sources(std::move(items));
}

// Sorts a freshly-listed archive's entries per `sort` and converts them to
// ArchiveSource siblings.
std::vector<ImageSource> sorted_archive_siblings(const std::filesystem::path& archive_path,
    const std::vector<archive::Entry>& entries, SortMode sort, bool descending) {
    std::vector<SortableSibling> items;
    items.reserve(entries.size());
    for (const auto& e : entries)
        items.push_back(SortableSibling{
            .source      = ArchiveSource{.archive_path = archive_path, .entry_name = e.name},
            .size        = e.size,
            .mtime_ticks = e.mtime,
        });
    sort_siblings(items, sort, descending);
    return extract_sources(std::move(items));
}

}  // namespace

// Splits a name into alternating alpha/digit runs and compares digit runs
// numerically, so "file2.png" sorts before "file10.png".
bool natural_less(std::string_view a, std::string_view b) {
    std::size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        const unsigned char ca = static_cast<unsigned char>(a[i]);
        const unsigned char cb = static_cast<unsigned char>(b[j]);

        if (std::isdigit(ca) && std::isdigit(cb)) {
            std::size_t si = i, sj = j;
            while (i < a.size() && std::isdigit(static_cast<unsigned char>(a[i])))
                ++i;
            while (j < b.size() && std::isdigit(static_cast<unsigned char>(b[j])))
                ++j;

            // butil::str only specializes str_converter for int/double/bool/
            // string(_view); digit runs here can exceed int range in theory,
            // so parse with std::from_chars directly rather than butil::str_to.
            const auto run_a = a.substr(si, i - si);
            const auto run_b = b.substr(sj, j - sj);
            long long num_a = 0, num_b = 0;
            const auto [pa, ea] = std::from_chars(run_a.data(), run_a.data() + run_a.size(), num_a);
            const auto [pb, eb] = std::from_chars(run_b.data(), run_b.data() + run_b.size(), num_b);
            if (ea != std::errc{} || eb != std::errc{}) {
                if (run_a != run_b)
                    return run_a < run_b;
                continue;
            }
            if (num_a != num_b)
                return num_a < num_b;
            continue;
        }

        if (ca != cb)
            return ca < cb;
        ++i;
        ++j;
    }
    return a.size() < b.size();
}

std::optional<DirectoryModel> DirectoryModel::open(const std::filesystem::path& path, SortMode sort, bool descending) {
    DirectoryModel model;
    namespace fs = std::filesystem;

    // "archive.zip/entry.png" style path: split at the archive extension.
    // NOTE: don't rely on has_parent_path() to terminate this loop -- on
    // some libstdc++ versions parent_path() of the root ("/") returns "/"
    // itself while has_parent_path() still reports true, which spins
    // forever. Detect the fixed point explicitly instead.
    for (fs::path probe = path;; probe = probe.parent_path()) {
        if (archive::is_archive_path(probe) && fs::exists(probe) && !fs::is_directory(probe)) {
            const auto entries = archive::list_image_entries(probe);
            model.m_siblings   = sorted_archive_siblings(probe, entries, sort, descending);

            const auto wanted = path.lexically_relative(probe).generic_string();
            auto it           = std::ranges::find_if(
                model.m_siblings, [&](const ImageSource& s) { return display_name(s) == wanted || wanted.empty(); });
            model.m_index = it != model.m_siblings.end() ? static_cast<std::size_t>(it - model.m_siblings.begin()) : 0;
            // lexically_relative() yields "." (or "" on failure) when the
            // archive itself was opened rather than an entry inside it: the
            // first entry is then the intended target, not a failed lookup.
            model.m_target_found =
                it != model.m_siblings.end() || wanted.empty() || wanted == ".";

            if (model.m_siblings.empty())
                butil::log.warn("No image entries found in archive '{}'", probe.string());
            return model;
        }

        const fs::path parent = probe.parent_path();
        if (parent.empty() || parent == probe)
            break;  // reached the filesystem root
    }

    if (fs::is_directory(path)) {
        model.m_siblings = list_folder_images(path, sort, descending);
    } else if (fs::exists(path)) {
        const auto folder = path.parent_path().empty() ? fs::path(".") : path.parent_path();
        model.m_siblings  = list_folder_images(folder, sort, descending);
    } else {
        butil::log.error("Path does not exist: '{}'", path.string());
        return std::nullopt;
    }

    // Compare absolute + lexically-normalized forms: sibling paths keep the
    // form of `folder` (relative when the caller passed one), while `path`
    // may be written differently ("./img.png", "sub/../img.png"), so a raw
    // == would miss the requested file and misreport it as unopenable.
    const fs::path want = fs::absolute(path).lexically_normal();
    auto matches        = [&](const ImageSource& s) {
        if (!std::holds_alternative<FileSource>(s))
            return false;
        return fs::absolute(std::get<FileSource>(s).path).lexically_normal() == want;
    };
    auto it = std::ranges::find_if(model.m_siblings, matches);
#if defined(_WIN32)
    // Windows paths are case-insensitive (NTFS): a wrong-case CLI argument is
    // still the same file. Fallback only, so on POSIX "Img.png" and "img.png"
    // (which can both exist) never collide.
    if (it == model.m_siblings.end()) {
        const auto want_lower = butil::lower(want.generic_string());
        it                    = std::ranges::find_if(model.m_siblings, [&](const ImageSource& s) {
            if (!std::holds_alternative<FileSource>(s))
                return false;
            return butil::lower(fs::absolute(std::get<FileSource>(s).path).lexically_normal().generic_string()) ==
                   want_lower;
        });
    }
#endif
    model.m_index = it != model.m_siblings.end() ? static_cast<std::size_t>(it - model.m_siblings.begin()) : 0;
    // Opening a folder means index 0 is a legitimate target; an existing
    // *file* that isn't in the list was filtered out (unsupported extension)
    // and callers must show an error instead of treating index 0 as it.
    model.m_target_found = fs::is_directory(path) || it != model.m_siblings.end();

    if (model.m_siblings.empty()) {
        butil::log.warn("No images found alongside '{}'", path.string());
        return model;
    }
    return model;
}

const ImageSource& DirectoryModel::next() {
    if (m_siblings.empty())
        return current();
    m_index = (m_index + 1) % m_siblings.size();
    return current();
}

const ImageSource& DirectoryModel::prev() {
    if (m_siblings.empty())
        return current();
    m_index = (m_index == 0 ? m_siblings.size() : m_index) - 1;
    return current();
}

void DirectoryModel::jump_to(std::size_t index) {
    if (index < m_siblings.size())
        m_index = index;
}

void DirectoryModel::resort(SortMode mode, bool descending) {
    if (m_siblings.empty())
        return;

    // Identity (not index) of the currently-shown image, so it can be
    // relocated afterward regardless of where the new order puts it.
    const auto current_key = source_key(current());

    if (is_archive_entry(m_siblings.front())) {
        // No per-entry size/mtime is cached on ArchiveSource itself, so this
        // re-lists the archive once (the list_image_entries() call itself,
        // same as open() already pays on first opening it).
        const auto& archive_path = std::get<ArchiveSource>(m_siblings.front()).archive_path;
        m_siblings =
            sorted_archive_siblings(archive_path, archive::list_image_entries(archive_path), mode, descending);
    } else {
        std::vector<SortableSibling> items;
        items.reserve(m_siblings.size());
        for (auto& s : m_siblings) {
            const auto& path = std::get<FileSource>(s).path;
            std::error_code sec, mec;
            const auto size  = std::filesystem::file_size(path, sec);
            const auto mtime = std::filesystem::last_write_time(path, mec);
            items.push_back(SortableSibling{
                .source      = std::move(s),
                .size        = sec ? 0 : size,
                .mtime_ticks = mec ? 0 : mtime.time_since_epoch().count(),
            });
        }
        sort_siblings(items, mode, descending);
        m_siblings = extract_sources(std::move(items));
    }

    auto it = std::ranges::find_if(m_siblings, [&](const ImageSource& s) { return source_key(s) == current_key; });
    m_index = it != m_siblings.end() ? static_cast<std::size_t>(it - m_siblings.begin()) : 0;
}

bool DirectoryModel::delete_current() {
    if (m_siblings.empty() || is_archive_entry(current()))
        return false;  // archives are read-only in this viewer

    const auto& path = std::get<FileSource>(current()).path;
    std::error_code ec;
    std::filesystem::remove(path, ec);
    if (ec) {
        butil::log.error("Failed to delete '{}': {}", path.string(), ec.message());
        return false;
    }

    m_siblings.erase(m_siblings.begin() + static_cast<long>(m_index));
    if (m_index >= m_siblings.size() && !m_siblings.empty())
        m_index = m_siblings.size() - 1;
    return true;
}

}  // namespace biv