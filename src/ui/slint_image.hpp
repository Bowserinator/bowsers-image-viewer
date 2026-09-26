#pragma once

#include <cstddef>
#include <cstring>

#include "app.h"  // generated from ui/app.slint
#include "processing/channel_ops.hpp"

namespace biv {

// RGBA8 buffer -> slint::Image (copies the pixels). UI thread only.
inline slint::Image to_slint_image(const colors::RgbaBuffer& buf) {
    slint::SharedPixelBuffer<slint::Rgba8Pixel> pixels(buf.width, buf.height);
    std::memcpy(pixels.begin(), buf.pixels.data(), buf.pixels.size());
    return slint::Image(pixels);
}

}  // namespace biv
