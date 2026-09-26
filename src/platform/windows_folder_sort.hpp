#pragma once

#include <filesystem>
#include <optional>

#include "filesystem/directory_model.hpp"  // SortMode

namespace biv::platform {

// Column plus direction read back from Explorer's own per-folder view
// state (see detect_explorer_folder_sort() below).
struct ExplorerFolderSort {
    biv::SortMode mode;
    bool descending = false;
};

// Best-effort read of the sort column (and direction) Windows Explorer
// currently has on file for `folder` -- the same per-folder "shellbag"
// view-state memory Explorer itself uses to reopen a folder the way it was
// last left, read straight out of the registry (there is no supported API
// to query it for a folder that isn't currently open in an Explorer
// window).
//
// Returns std::nullopt whenever nothing usable is on record for this exact
// folder: it was never browsed in Explorer, its primary sort column isn't
// one of name/size/date-modified (the only columns SortMode can express), or
// (always, on non-Windows builds) the platform has no such thing. Both the
// binary SORTCOLUMN and the textual "SortColumns" encodings are understood. Callers are expected to fall back to their
// own saved sort preference (and direction) in that case.
std::optional<ExplorerFolderSort> detect_explorer_folder_sort(const std::filesystem::path& folder);

}  // namespace biv::platform
