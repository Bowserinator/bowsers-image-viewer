#pragma once

#include <string>

#include "core/source.hpp"

namespace biv::sidebar {

// Fields displayed by the sidebar's metadata page. EXIF fields are empty when
// the source has no EXIF (or is an archive entry — EXIF is read from the file
// on disk only).
struct FileMetadata {
    std::string path;    // on-disk file path (archive path for archive entries)
    std::string size;    // human-readable, e.g. "3.4 MB"
    std::string format;  // uppercase extension, e.g. "JPEG", "PNG", "WEBP", ...
    std::string source;  // "File" | "Archive"

    // EXIF (empty when absent)
    std::string camera;    // "Make Model"
    std::string date;      // original date-time
    std::string exposure;  // e.g. "1/250 s"
    std::string aperture;  // e.g. "f/2.8"
    std::string iso;       // e.g. "100"
    std::string focal;     // e.g. "50 mm"
    std::string gps;       // e.g. "51.508, -0.125"
};

[[nodiscard]] FileMetadata collect_metadata(const ImageSource& src);

// Formats width:height as a human-friendly aspect ratio for the metadata
// page. When the ratio is close to a well-known one (16:9, 4:3, ...) that
// ratio's integers are shown (e.g. "16:9"); otherwise the ratio is shown as
// a decimal against 1, rounded to three places (e.g. "1.415 : 1"). Returns
// an empty string when either dimension is non-positive.
[[nodiscard]] std::string format_aspect_ratio(int width, int height);

}  // namespace biv::sidebar