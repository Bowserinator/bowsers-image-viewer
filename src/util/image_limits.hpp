#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "config.hpp"

namespace biv {

// Size in bytes of a tightly packed RGBA8 image, or nullopt when the
// dimensions are not usable: non-positive, larger than kMAX_IMAGE_DIMENSION on
// a side, or more than kMAX_IMAGE_PIXELS in total.
//
// Use this on every width/height that comes out of a file (image headers,
// codec parameters, decoder output) *before* allocating or indexing with it,
// and size buffers from the returned value rather than re-multiplying.
inline std::optional<std::size_t> rgba_byte_size(std::int64_t width, std::int64_t height) {
    if (width <= 0 || height <= 0 || width > kMAX_IMAGE_DIMENSION || height > kMAX_IMAGE_DIMENSION)
        return std::nullopt;
    const auto pixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (pixels > kMAX_IMAGE_PIXELS)
        return std::nullopt;
    return pixels * static_cast<std::size_t>(kBYTES_PER_PIXEL);
}

}  // namespace biv
