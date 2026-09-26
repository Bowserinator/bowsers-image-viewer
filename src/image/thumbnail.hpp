#pragma once

#include <cstddef>
#include <cstdint>

#include "config.hpp"
#include "processing/channel_ops.hpp"

namespace biv::channel {

struct ThumbSize {
    int width  = 1;
    int height = 1;
};

// Aspect-preserving size that fits `max_dim` on the long edge. Never upscales
// and never returns a zero-sized edge (a 100000x1 strip becomes 128x1, not
// 128x0). The one place this rounding lives: decoders use it to pick the
// reduced size to decode at, and make_thumbnail() to pick the final size.
[[nodiscard]] ThumbSize thumbnail_size(int width, int height, int max_dim);

// Downscales a tightly or loosely packed RGBA8 image (`stride` bytes between
// rows) to fit `max_dim`. Lets decoders scale straight from their own buffer
// instead of first copying a full-size image into an RgbaBuffer. Invalid
// dimensions or a failed resize give a black thumbnail, never garbage.
[[nodiscard]] RgbaBuffer make_thumbnail(
    const std::uint8_t* rgba, int width, int height, std::size_t stride, int max_dim = kDEFAULT_THUMBNAIL_MAX_DIM);

[[nodiscard]] RgbaBuffer make_thumbnail(const RgbaBuffer& src, int max_dim = kDEFAULT_THUMBNAIL_MAX_DIM);

// Opaque black tile shown when an image can't be decoded or rendered.
[[nodiscard]] RgbaBuffer make_black_thumbnail();

}  // namespace biv::channel
