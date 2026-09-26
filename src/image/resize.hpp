#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "util/color.hpp"

namespace biv::channel {

// Thumbnail-quality resize of tightly or loosely packed RGBA8 (`stride` bytes
// between rows) into `out` (out_w * out_h * 4 bytes, tightly packed).
// Averages in linear light with alpha weighting. Returns false if the resize
// itself failed.
[[nodiscard]] bool resize_rgba_srgb(
    const std::uint8_t* rgba, int width, int height, std::size_t stride, std::uint8_t* out, int out_w, int out_h);

// Display-quality downscale of `src` to out_w x out_h, for showing a large
// image zoomed out. Uses a sharper (Catmull-Rom) kernel than the thumbnail
// path and works on the stored values directly, which is what people expect
// from a viewer and is a good deal cheaper than linear light. Alpha weighted.
// Returns nullopt for unusable sizes or a failed resize.
[[nodiscard]] std::optional<colors::RgbaBuffer> resize_rgba_sharp(const colors::RgbaBuffer& src, int out_w, int out_h);

}  // namespace biv::channel
