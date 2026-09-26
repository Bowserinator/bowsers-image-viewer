#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace biv::archive {

struct Entry {
    std::string name;    // path within the archive
    std::size_t size = 0;
    std::int64_t mtime = 0;  // seconds since Unix epoch (archive_entry_mtime); 0 if the
                              // archive format doesn't carry one
};

// True if the extension suggests a supported archive container. Cheap,
// extension-based check used before touching libarchive at all.
bool is_archive_path(const std::filesystem::path& path);

// Lists the image-like entries inside an archive (filters by common image
// extensions), in the archive's natural (on-disk) order.
std::vector<Entry> list_image_entries(const std::filesystem::path& archive_path);

// Extracts a single named entry into memory. Returns std::nullopt on error
// (missing entry, corrupt archive, unsupported codec, ...).
std::optional<std::vector<std::byte>> read_entry(
    const std::filesystem::path& archive_path, const std::string& entry_name);

}  // namespace biv::archive
