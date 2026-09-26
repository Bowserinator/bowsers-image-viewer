#include "archive_source.hpp"

#include "butil/log.hpp"
#include "butil/str.hpp"

#include <string_view>

#include "config.hpp"

#ifdef BIV_ENABLE_ARCHIVES
    #include <archive.h>
    #include <archive_entry.h>
#endif

namespace biv::archive {

namespace {

bool has_image_extension(std::string_view name) {
    const auto lower_name = butil::lower(name);
    for (auto ext : kIMAGE_EXTENSIONS)
        if (lower_name.ends_with(ext))
            return true;
    return false;
}

#ifdef BIV_ENABLE_ARCHIVES
// RAII wrapper: libarchive's archive* has no unique_ptr deleter built in.
struct ArchiveReadHandle {
    struct archive* a = nullptr;

    ArchiveReadHandle() : a(archive_read_new()) {
        archive_read_support_format_zip(a);
        archive_read_support_format_rar5(a);
        archive_read_support_format_rar(a);
    }

    ~ArchiveReadHandle() {
        if (a) {
            archive_read_close(a);
            archive_read_free(a);
        }
    }

    ArchiveReadHandle(const ArchiveReadHandle&)            = delete;
    ArchiveReadHandle& operator=(const ArchiveReadHandle&) = delete;
};

// archive_read_open_filename() takes a narrow `const char*` everywhere,
// which libarchive decodes with the ANSI code page on Windows -- silently
// failing to open any archive path with a character outside it. path.c_str()
// is the path's native representation (wchar_t* on Windows, char* raw bytes
// elsewhere), so route it through the wide entry point there instead.
int open_archive_file(struct archive* a, const std::filesystem::path& path, std::size_t block_size) {
#if defined(_WIN32)
    return archive_read_open_filename_w(a, path.c_str(), block_size);
#else
    return archive_read_open_filename(a, path.c_str(), block_size);
#endif
}
#endif

}  // namespace

bool is_archive_path(const std::filesystem::path& path) {
    const auto ext = butil::lower(path.extension().string());
    return ext == ".zip" || ext == ".rar" || ext == ".cbz" || ext == ".cbr";
}

std::vector<Entry> list_image_entries(const std::filesystem::path& archive_path) {
    std::vector<Entry> out;
#ifndef BIV_ENABLE_ARCHIVES
    (void)archive_path;
    butil::log.warn("Archive support disabled at build time; cannot list '{}'", archive_path.string());
    return out;
#else
    ArchiveReadHandle handle;
    if (open_archive_file(handle.a, archive_path, kARCHIVE_BLOCK_SIZE) != ARCHIVE_OK) {
        butil::log.error("Failed to open archive '{}': {}", archive_path.string(), archive_error_string(handle.a));
        return out;
    }

    struct archive_entry* entry = nullptr;
    while (archive_read_next_header(handle.a, &entry) == ARCHIVE_OK) {
        // NULL when the name can't be represented in the current locale.
        const char* name = archive_entry_pathname(entry);
        if (name && has_image_extension(name))
            out.push_back(Entry{
                .name  = name,
                .size  = static_cast<std::size_t>(archive_entry_size(entry)),
                .mtime = static_cast<std::int64_t>(archive_entry_mtime(entry)),
            });
        archive_read_data_skip(handle.a);
    }
    return out;
#endif
}

std::optional<std::vector<std::byte>> read_entry(
    const std::filesystem::path& archive_path, const std::string& entry_name) {
#ifndef BIV_ENABLE_ARCHIVES
    (void)archive_path;
    (void)entry_name;
    butil::log.warn("Archive support disabled at build time; cannot read entries");
    return std::nullopt;
#else
    ArchiveReadHandle handle;
    if (open_archive_file(handle.a, archive_path, kARCHIVE_BLOCK_SIZE) != ARCHIVE_OK) {
        butil::log.error("Failed to open archive '{}': {}", archive_path.string(), archive_error_string(handle.a));
        return std::nullopt;
    }

    struct archive_entry* entry = nullptr;
    while (archive_read_next_header(handle.a, &entry) == ARCHIVE_OK) {
        const char* name = archive_entry_pathname(entry);
        if (!name || name != entry_name) {
            archive_read_data_skip(handle.a);
            continue;
        }

        // The entry size is whatever the archive header claims: never allocate
        // from it unchecked (a tiny zip can claim a multi-terabyte entry).
        const auto size = archive_entry_size(entry);
        if (size < 0 || static_cast<std::uintmax_t>(size) > kMAX_IMAGE_FILE_BYTES) {
            butil::log.error("Entry '{}' in '{}' claims an unreasonable size ({} bytes)", entry_name, archive_path.string(), size);
            return std::nullopt;
        }
        std::vector<std::byte> data(static_cast<std::size_t>(size));
        const auto read = archive_read_data(handle.a, data.data(), data.size());
        if (read < 0 || static_cast<std::size_t>(read) != data.size()) {
            butil::log.error("Short read on '{}' in '{}'", entry_name, archive_path.string());
            return std::nullopt;
        }
        return data;
    }

    butil::log.error("Entry '{}' not found in '{}'", entry_name, archive_path.string());
    return std::nullopt;
#endif
}

}  // namespace biv::archive
