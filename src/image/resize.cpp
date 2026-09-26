#include "resize.hpp"

// The only translation unit that compiles stb_image_resize2.
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize2.h>

#include "util/image_limits.hpp"

namespace biv::channel {

bool resize_rgba_srgb(
    const std::uint8_t* rgba, int width, int height, std::size_t stride, std::uint8_t* out, int out_w, int out_h) {
    return stbir_resize_uint8_srgb(rgba, width, height, static_cast<int>(stride), out, out_w, out_h, 0, STBIR_RGBA) !=
           nullptr;
}

std::optional<colors::RgbaBuffer> resize_rgba_sharp(const colors::RgbaBuffer& src, int out_w, int out_h) {
    const auto in_bytes  = rgba_byte_size(src.width, src.height);
    const auto out_bytes = rgba_byte_size(out_w, out_h);
    if (!in_bytes || !out_bytes || src.pixels.size() < *in_bytes)
        return std::nullopt;

    colors::RgbaBuffer out;
    out.width  = out_w;
    out.height = out_h;
    out.pixels.resize(*out_bytes);

    STBIR_RESIZE job;
    stbir_resize_init(&job,
        src.pixels.data(),
        src.width,
        src.height,
        src.width * kBYTES_PER_PIXEL,
        out.pixels.data(),
        out_w,
        out_h,
        out_w * kBYTES_PER_PIXEL,
        STBIR_RGBA,
        STBIR_TYPE_UINT8);
    stbir_set_filters(&job, STBIR_FILTER_CATMULLROM, STBIR_FILTER_CATMULLROM);
    if (!stbir_resize_extended(&job))
        return std::nullopt;
    return out;
}

}  // namespace biv::channel
