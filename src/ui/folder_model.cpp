#include "folder_model.hpp"

#include "butil/str.hpp"
#include "butil/util.hpp"

#include <algorithm>
#include <climits>
#include <system_error>
#include <thread>

#include "config.hpp"
#include "filesystem/directory_model.hpp"  // natural_less
#include "util/file.hpp"

namespace biv {

namespace fs = std::filesystem;

namespace {

// Listable media file (image, or video when the build has video support).
// The extension fits std::string's small-buffer, so butil::lower doesn't allocate.
bool is_image_name(std::string_view name) {
    const auto dot = name.rfind('.');
    return dot != std::string_view::npos && dot != 0 &&
           butil::contains(kALL_EXTENSIONS, butil::lower(name.substr(dot)));
}

// True if `name`'s extension is a video extension, for picking the "video"
// icon in folder view. Only videos that also pass is_image_name() (i.e. that
// are actually listed) reach this; with video support built out, kALL_EXTENSIONS
// excludes kVIDEO_EXTENSIONS entirely, so this never matches.
bool is_video_name(std::string_view name) {
    const auto dot = name.rfind('.');
    return dot != std::string_view::npos && dot != 0 &&
           butil::contains(kVIDEO_EXTENSIONS, butil::lower(name.substr(dot)));
}

}  // namespace

// ---------------------------------------------------------------------------
// Construction / helpers
// ---------------------------------------------------------------------------

std::shared_ptr<FolderModel> FolderModel::create() {
    return std::shared_ptr<FolderModel>(new FolderModel());
}

FolderModel::~FolderModel() {
    cancel_all_jobs();
}

std::string FolderModel::make_key(const fs::path& p) {
    std::error_code ec;
    fs::path abs = fs::absolute(p, ec);
    if (ec)
        abs = p;
    const auto u = abs.lexically_normal().generic_u8string();
    std::string s(reinterpret_cast<const char*>(u.data()), u.size());
    while (s.size() > 1 && s.back() == '/')
        s.pop_back();
    return s;
}

std::string_view FolderModel::name_of(const Listing& l, const Entry& e) const noexcept {
    return std::string_view(l.arena.data() + e.off, e.len);
}

std::optional<std::size_t> FolderModel::find_row(const RowRef& ref) const {
    for (std::size_t i = 0; i < m_rows.size(); ++i)
        if (m_rows[i] == ref)
            return i;
    return std::nullopt;
}

std::optional<FolderModel::RowRef> FolderModel::locate_current() const {
    if (m_cur_name.empty())
        return std::nullopt;
    for (std::size_t id = 0; id < m_listings.size(); ++id) {
        const auto& l = m_listings[id];
        if (!l || l->key != m_cur_dir)
            continue;
        for (std::size_t i = 0; i < l->entries.size(); ++i) {
            const auto& e = l->entries[i];
            if (!(e.flags & kDir) && name_of(*l, e) == m_cur_name)
                return RowRef{static_cast<std::uint32_t>(id), static_cast<std::uint32_t>(i)};
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// slint::Model
// ---------------------------------------------------------------------------

std::optional<FolderRow> FolderModel::row_data(std::size_t row) const {
    if (row >= m_rows.size())
        return std::nullopt;
    const RowRef r   = m_rows[row];
    const Listing& l = *m_listings[r.listing];
    const Entry& e   = l.entries[r.index];
    const auto nm    = name_of(l, e);

    FolderRow out;
    out.name     = slint::SharedString(nm);
    out.depth    = l.depth;
    out.is_dir   = (e.flags & kDir) != 0;
    out.is_video = (e.flags & kVideo) != 0;
    out.expanded = (e.flags & kExpanded) != 0;
    out.loading  = (e.flags & kLoading) != 0;
    out.current  = !out.is_dir && nm == m_cur_name && l.key == m_cur_dir;
    return out;
}

// ---------------------------------------------------------------------------
// Public operations
// ---------------------------------------------------------------------------

void FolderModel::set_root(const fs::path& dir) {
    cancel_all_jobs();
    const auto old_rows = m_rows.size();
    m_rows.clear();
    m_listings.clear();
    if (old_rows)
        notify_row_removed(0, old_rows);

    auto root   = std::make_unique<Listing>();
    root->dir   = dir;
    root->key   = make_key(dir);
    root->depth = 0;
    m_root_key  = root->key;
    m_listings.push_back(std::move(root));

    if (on_loading_changed)
        on_loading_changed(true);
    start_scan(0);
}

void FolderModel::set_sort(SortMode mode, bool descending) {
    if (mode == m_sort_mode && descending == m_sort_descending)
        return;
    m_sort_mode       = mode;
    m_sort_descending = descending;
    if (!m_listings.empty() && m_listings[0]) {
        const fs::path root = m_listings[0]->dir;  // copy: set_root() clears m_listings
        set_root(root);
    }
}

void FolderModel::show_file(const fs::path& file) {
    std::error_code ec;
    fs::path abs = fs::absolute(file, ec);
    if (ec)
        abs = file;
    const fs::path parent  = abs.parent_path();
    const std::string pkey = make_key(parent);

    const bool inside = !m_root_key.empty() &&
                        (pkey == m_root_key || (pkey.size() > m_root_key.size() && pkey.starts_with(m_root_key) &&
                                                   (m_root_key.back() == '/' || pkey[m_root_key.size()] == '/')));
    if (!inside)
        set_root(parent);

    set_current(pkey, path_to_utf8(abs.filename()));
}

void FolderModel::set_current(const std::string& dir_key, std::string name) {
    const auto before = locate_current();
    m_cur_dir         = dir_key;
    m_cur_name        = std::move(name);
    const auto after  = locate_current();
    if (before == after)
        return;
    for (const auto& r : {before, after}) {
        if (!r)
            continue;
        if (const auto pos = find_row(*r))
            notify_row_changed(*pos);
    }
}

void FolderModel::toggle(std::size_t row) {
    if (row >= m_rows.size())
        return;
    const RowRef r = m_rows[row];
    const Entry& e = m_listings[r.listing]->entries[r.index];
    if (!(e.flags & kDir))
        return;
    if (e.flags & (kExpanded | kLoading))
        collapse(row);  // also cancels an in-flight load
    else
        expand(row);
}

bool FolderModel::is_dir_row(std::size_t row) const {
    if (row >= m_rows.size())
        return false;
    const RowRef r = m_rows[row];
    return (m_listings[r.listing]->entries[r.index].flags & kDir) != 0;
}

std::optional<fs::path> FolderModel::file_at(std::size_t row) const {
    if (row >= m_rows.size())
        return std::nullopt;
    const RowRef r   = m_rows[row];
    const Listing& l = *m_listings[r.listing];
    const Entry& e   = l.entries[r.index];
    if (e.flags & kDir)
        return std::nullopt;
    return l.dir / path_from_utf8(name_of(l, e));
}

// ---------------------------------------------------------------------------
// Expand / collapse
// ---------------------------------------------------------------------------

void FolderModel::expand(std::size_t row) {
    const RowRef ref = m_rows[row];
    Listing& parent  = *m_listings[ref.listing];
    Entry& e         = parent.entries[ref.index];

    auto child    = std::make_unique<Listing>();
    child->dir    = parent.dir / path_from_utf8(name_of(parent, e));
    child->key    = make_key(child->dir);
    child->depth  = parent.depth + 1;
    child->parent = ref;
    const auto id = static_cast<std::uint32_t>(m_listings.size());
    m_listings.push_back(std::move(child));

    e.flags |= kLoading;
    notify_row_changed(row);
    start_scan(id);
}

void FolderModel::collapse(std::size_t row) {
    const RowRef ref = m_rows[row];
    Listing& owner   = *m_listings[ref.listing];
    Entry& e         = owner.entries[ref.index];
    e.flags          = static_cast<std::uint8_t>(e.flags & ~(kExpanded | kLoading));

    // Rows below `row` that are deeper than it are its descendants.
    const int depth = owner.depth;
    std::size_t end = row + 1;
    while (end < m_rows.size() && m_listings[m_rows[end].listing]->depth > depth)
        ++end;

    // Free every listing that owned a removed row (also cancels their scans).
    for (std::size_t i = row + 1; i < end; ++i)
        free_listing(m_rows[i].listing);
    // The direct child listing may own zero rows (empty dir / still loading).
    for (std::size_t id = 0; id < m_listings.size(); ++id)
        if (m_listings[id] && m_listings[id]->parent == ref)
            free_listing(static_cast<std::uint32_t>(id));
    prune_orphans();

    const std::size_t removed = end - (row + 1);
    if (removed)
        m_rows.erase(
            m_rows.begin() + static_cast<std::ptrdiff_t>(row + 1), m_rows.begin() + static_cast<std::ptrdiff_t>(end));
    notify_row_changed(row);
    if (removed)
        notify_row_removed(row + 1, removed);
}

void FolderModel::free_listing(std::uint32_t id) {
    if (id >= m_listings.size() || !m_listings[id])
        return;
    if (const auto it = m_jobs.find(id); it != m_jobs.end()) {
        it->second->cancel.store(true);
        m_jobs.erase(it);
    }
    m_listings[id].reset();
}

// Frees listings whose parent listing no longer exists, until nothing changes.
void FolderModel::prune_orphans() {
    for (bool changed = true; changed;) {
        changed = false;
        for (std::size_t id = 0; id < m_listings.size(); ++id) {
            const auto& l = m_listings[id];
            if (l && l->parent && !m_listings[l->parent->listing]) {
                free_listing(static_cast<std::uint32_t>(id));
                changed = true;
            }
        }
    }
}

void FolderModel::cancel_all_jobs() {
    for (auto& [id, job] : m_jobs)
        job->cancel.store(true);
    m_jobs.clear();
}

// ---------------------------------------------------------------------------
// Background scanning
// ---------------------------------------------------------------------------

FolderModel::ScanResult FolderModel::scan_directory(
    const fs::path& dir, SortMode mode, bool descending, const std::atomic<bool>& cancel) {
    ScanResult out;

    std::error_code ec;
    fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
    const fs::directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
        if (cancel.load(std::memory_order_relaxed))
            return {};

        const auto& de = *it;
        std::error_code e2;
        const bool is_dir      = de.is_directory(e2);
        const std::string name = path_to_utf8(de.path().filename());
        if (!is_dir) {
            // Cheap name check first; only stat/type-check plausible images.
            if (!is_image_name(name) || !de.is_regular_file(e2))
                continue;
        }
        if (out.arena.size() + name.size() >= UINT32_MAX)
            break;  // absurdly large; keep what we have
        std::uint8_t flags = static_cast<std::uint8_t>(is_dir ? kDir : 0);
        if (!is_dir && is_video_name(name))
            flags |= kVideo;
        std::error_code sec, mec;
        const auto size  = is_dir ? 0 : de.file_size(sec);
        const auto mtime = de.last_write_time(mec);
        out.entries.push_back(Entry{static_cast<std::uint32_t>(out.arena.size()),
            static_cast<std::uint32_t>(name.size()),
            flags,
            sec ? 0 : static_cast<std::uint64_t>(size),
            mec ? 0 : static_cast<std::int64_t>(mtime.time_since_epoch().count())});
        out.arena += name;
    }
    out.entries.shrink_to_fit();
    out.arena.shrink_to_fit();

    // Directories first, then files; natural order within each group.
    // stable_sort: safe even if the comparator isn't a perfect strict weak order.
    const auto name_at = [&out](const Entry& e) {
        return std::string_view(out.arena.data() + e.off, e.len);
    };
    const auto less = [&](const Entry& a, const Entry& b) {
        if (mode == SortMode::Size && a.size != b.size)
            return a.size < b.size;
        if (mode == SortMode::MTime && a.mtime != b.mtime)
            return a.mtime < b.mtime;
        return natural_less(name_at(a), name_at(b));
    };
    const auto mid = std::stable_partition(
        out.entries.begin(), out.entries.end(), [](const Entry& e) { return (e.flags & kDir) != 0; });
    std::stable_sort(out.entries.begin(), mid, less);
    std::stable_sort(mid, out.entries.end(), less);
    if (descending) {  // mirror image, same as DirectoryModel::sort_siblings
        std::reverse(out.entries.begin(), mid);
        std::reverse(mid, out.entries.end());
    }
    return out;
}

void FolderModel::start_scan(std::uint32_t listing_id) {
    auto job           = std::make_shared<Job>();
    m_jobs[listing_id] = job;

    std::thread([weak = weak_from_this(),
                    listing_id,
                    job,
                    dir        = m_listings[listing_id]->dir,
                    mode       = m_sort_mode,
                    descending = m_sort_descending] {
        auto result = std::make_shared<ScanResult>(scan_directory(dir, mode, descending, job->cancel));
        if (job->cancel.load())
            return;
        slint::invoke_from_event_loop([weak, listing_id, job, result] {
            if (auto self = weak.lock())
                self->finish_scan(listing_id, job, std::move(*result));
        });
    }).detach();
}

// UI thread. Drops results from cancelled/replaced scans, then publishes the
// whole (already sorted) listing with a single notification.
void FolderModel::finish_scan(std::uint32_t listing_id, const std::shared_ptr<Job>& job, ScanResult result) {
    const auto jit = m_jobs.find(listing_id);
    if (jit == m_jobs.end() || jit->second != job || job->cancel.load())
        return;
    m_jobs.erase(jit);
    if (listing_id >= m_listings.size() || !m_listings[listing_id])
        return;

    Listing& l          = *m_listings[listing_id];
    l.arena             = std::move(result.arena);
    l.entries           = std::move(result.entries);
    const std::size_t n = l.entries.size();

    std::vector<RowRef> refs;
    refs.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
        refs.push_back(RowRef{listing_id, static_cast<std::uint32_t>(i)});

    if (!l.parent) {  // root listing
        m_rows = std::move(refs);
        if (n)
            notify_row_added(0, n);
        if (on_loading_changed)
            on_loading_changed(false);
        return;
    }

    const auto pos = find_row(*l.parent);
    if (!pos) {  // the expanding row is gone
        m_listings[listing_id].reset();
        return;
    }
    Entry& pe = m_listings[l.parent->listing]->entries[l.parent->index];
    pe.flags  = static_cast<std::uint8_t>((pe.flags & ~kLoading) | kExpanded);

    m_rows.insert(m_rows.begin() + static_cast<std::ptrdiff_t>(*pos + 1), refs.begin(), refs.end());
    notify_row_changed(*pos);
    if (n)
        notify_row_added(*pos + 1, n);
}

}  // namespace biv