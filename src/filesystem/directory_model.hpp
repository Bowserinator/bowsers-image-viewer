#pragma once

#include <filesystem>
#include <optional>
#include <vector>

#include "core/source.hpp"

namespace biv {

// Sort criteria for the sibling list produced by open() and re-applied in
// place by resort(). Order matters: it is persisted to disk
// as a plain int (see kSTORE_KEY_SORT_OPTION in src/ui/app.cpp), so the C++
// side converts it via an explicit switch keyed on these names, not a
// numeric clamp -- mirrors ui/theme.slint's SortOption (kept as a separate
// plain-C++ enum so this header doesn't need to include Slint's generated
// types; src/ui/app.cpp maps between the two, the same way it already does
// for ChannelMode/channel::Mode).
enum class SortMode { Name, Size, MTime };

// Enumerates the "siblings" of whatever was opened -- either the images in a
// filesystem folder, or the images inside an archive -- so Prev/Next and the
// thumbstrip behave identically for both
class DirectoryModel {
public:
    // Opens `path`, which may be:
    //  - a plain image file (siblings = other images in its folder)
    //  - a folder (siblings = images in it; empty folders are valid — no images)
    //  - an archive file, or an image path *inside* one written as
    //    "archive.zip/entry.png" (siblings = images in the archive)
    // An empty sibling list is a successful open; callers show an empty-state
    // label instead of treating it as a hard failure.
    static std::optional<DirectoryModel> open(
        const std::filesystem::path& path, SortMode sort = SortMode::Name, bool descending = false);

    // True when the file named by open() actually appears in the sibling
    // list. False when it exists but was filtered out (e.g. an unsupported
    // extension, or an archive entry that isn't listed) -- in that case
    // current_index() falls back to 0 and must NOT be treated as "the file
    // the user asked for". Callers show an error instead of displaying
    // whatever happens to be first in the folder.
    [[nodiscard]] bool target_found() const noexcept { return m_target_found; }

    [[nodiscard]] const std::vector<ImageSource>& siblings() const noexcept { return m_siblings; }

    [[nodiscard]] std::size_t current_index() const noexcept { return m_index; }

    [[nodiscard]] const ImageSource& current() const { return m_siblings.at(m_index); }

    // Wrapping prev/next over the sorted sibling list.
    const ImageSource& next();
    const ImageSource& prev();
    void jump_to(std::size_t index);

    // Re-sorts the current sibling list by `mode`/`descending` in place,
    // preserving which image is "current" by identity (its position
    // generally moves). Costs an extra stat() per file (or a full archive
    // re-listing for archive entries) to refresh size/mtime, since those
    // aren't cached on ImageSource itself.
    void resort(SortMode mode, bool descending = false);

    // Removes the current file from disk (folder mode only; archives are
    // read-only) and drops it from the sibling list. Returns false if the
    // delete failed or isn't applicable (e.g. current source is in an archive).
    bool delete_current();

private:
    DirectoryModel() = default;

    std::vector<ImageSource> m_siblings;
    std::size_t m_index = 0;
    bool m_target_found = true;
};

// Natural comparison ("file2" < "file10") built on butil::str, exposed for
// unit testing and for sorting the thumbstrip/prev-next order.
bool natural_less(std::string_view a, std::string_view b);

}  // namespace biv