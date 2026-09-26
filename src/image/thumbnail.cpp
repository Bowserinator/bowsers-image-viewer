#include "thumbnail.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "image/box_downscaler.hpp"
#include "image/resize.hpp"
#include "util/image_limits.hpp"

namespace biv::channel {

ThumbSize thumbnail_size(int width, int height, int max_dim) {
    width   = std::max(width, 1);
    height  = std::max(height, 1);
    max_dim = std::max(max_dim, 1);
    if (std::max(width, height) <= max_dim)
        return {width, height};

    const double k = static_cast<double>(max_dim) / std::max(width, height);
    return {std::max(1, static_cast<int>(std::lround(width * k))), std::max(1, static_cast<int>(std::lround(height * k)))};
}

RgbaBuffer make_thumbnail(const std::uint8_t* rgba, int width, int height, std::size_t stride, int max_dim) {
    // Never trust the caller's dimensions: they usually came out of a file header.
    const std::size_t min_stride = static_cast<std::size_t>(std::max(width, 0)) * kBYTES_PER_PIXEL;
    if (!rgba || !rgba_byte_size(width, height) || stride < min_stride)
        return make_black_thumbnail();

    const auto [tw, th] = thumbnail_size(width, height, max_dim);

    // Big reductions (a full-size decode being shrunk to a tile): a single
    // streaming area-average pass costs a fraction of a general resample,
    // whose kernel grows with the reduction ratio, and at these ratios the two
    // look the same. Gentle reductions, such as the tail end of a JPEG DCT
    // scale, keep the smoother resampling kernel.
    if (width / tw >= kBOX_MIN_REDUCTION && height / th >= kBOX_MIN_REDUCTION) {
        BoxDownscaler scaler(width, height, max_dim);
        for (int y = 0; y < height; ++y)
            scaler.add_row(rgba + static_cast<std::size_t>(y) * stride, kBYTES_PER_PIXEL);
        return scaler.finish();
    }

    RgbaBuffer out;
    out.width  = tw;
    out.height = th;
    out.pixels.resize(static_cast<std::size_t>(tw) * static_cast<std::size_t>(th) * kBYTES_PER_PIXEL);

    if (tw == width && th == height) {
        for (int y = 0; y < th; ++y)
            std::memcpy(out.pixels.data() + static_cast<std::size_t>(y) * tw * kBYTES_PER_PIXEL,
                rgba + static_cast<std::size_t>(y) * stride,
                min_stride);
        return out;
    }

    if (!resize_rgba_srgb(rgba, width, height, stride, out.pixels.data(), tw, th))
        return make_black_thumbnail();
    return out;
}

RgbaBuffer make_thumbnail(const RgbaBuffer& src, int max_dim) {
    const auto bytes = rgba_byte_size(src.width, src.height);
    if (!bytes || src.pixels.size() < *bytes)  // buffer smaller than its own header claims
        return make_black_thumbnail();
    return make_thumbnail(
        src.pixels.data(), src.width, src.height, static_cast<std::size_t>(std::max(src.width, 0)) * kBYTES_PER_PIXEL, max_dim);
}

RgbaBuffer make_black_thumbnail() {
    // Just a square placeholder pixel buffer for a thumb that failed to
    // decode; widgets/thumbstrip.slint scales it to fit the cell
    // (`image-fit: contain`), so its pixel size has no UI-layout constraint
    // to stay in sync with — reuse the generic thumbnail dimension.
    const int dim = kDEFAULT_THUMBNAIL_MAX_DIM;
    RgbaBuffer out;
    out.width  = dim;
    out.height = dim;
    out.pixels.assign(static_cast<std::size_t>(dim) * dim * kBYTES_PER_PIXEL, 0);
    for (std::size_t i = 3; i < out.pixels.size(); i += kBYTES_PER_PIXEL)
        out.pixels[i] = 255;  // opaque
    return out;
}

}  // namespace biv::channel
