#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>

#include "util/color.hpp"

namespace biv {

// Decodes a PNG straight to thumbnail size (<= `max_dim` on the long edge).
//
// PNG has no reduced-size decode like JPEG's DCT scaling, so the pixels still
// have to be inflated and unfiltered; what this avoids is everything around
// that. libpng hands over one row at a time and each row is folded into the
// thumbnail immediately (channel::BoxDownscaler), so a 100 MP screenshot is
// never expanded to a 400 MB RGBA image and then resized: memory stays a few
// thumbnail-wide rows and there is no second pass over the full-size pixels.
// The file overload streams from disk instead of reading the whole file first.
//
// Returns nullopt when the data isn't a PNG this path can handle (corrupt,
// Adam7-interlaced, absurd dimensions); the caller falls back to the general
// decoder. Requires libpng (BIV_ENABLE_PNG).
[[nodiscard]] std::optional<colors::RgbaBuffer> decode_png_scaled(const std::filesystem::path& path, int max_dim);
[[nodiscard]] std::optional<colors::RgbaBuffer> decode_png_scaled(std::span<const std::byte> bytes, int max_dim);

}  // namespace biv
