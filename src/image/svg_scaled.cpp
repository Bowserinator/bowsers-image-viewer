#include "svg_scaled.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <lunasvg.h>

#include "butil/log.hpp"

#include "config.hpp"
#include "util/image_limits.hpp"

namespace biv {

std::optional<colors::RgbaBuffer> decode_svg_scaled(std::span<const std::byte> bytes, int max_dim) {
    auto document = lunasvg::Document::loadFromData(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (!document) {
        butil::log.error("lunasvg::Document::loadFromData failed");
        return std::nullopt;
    }

    double w = document->width();
    double h = document->height();
    if (!std::isfinite(w) || !std::isfinite(h) || w <= 0.0 || h <= 0.0) {
        w = h = max_dim > 0 ? kDEFAULT_THUMBNAIL_MAX_DIM : kSVG_RENDER_MIN_DIM;
    }

    if (max_dim > 0) {
        const double k = std::min(1.0, static_cast<double>(max_dim) / std::max(w, h));
        w = w * k;
        h = h * k;
    } else {
        const double shortest = std::min(w, h);
        if (shortest < kSVG_RENDER_MIN_DIM) {
            w = w / shortest * kSVG_RENDER_MIN_DIM;
            h = h / shortest * kSVG_RENDER_MIN_DIM;
        }
        const double longest = std::max(w, h);
        if (longest > kSVG_RENDER_MAX_DIM) {
            w = w / longest * kSVG_RENDER_MAX_DIM;
            h = h / longest * kSVG_RENDER_MAX_DIM;
        }
    }

    const int out_w = std::max(1, static_cast<int>(std::lround(w)));
    const int out_h = std::max(1, static_cast<int>(std::lround(h)));
    if (!rgba_byte_size(out_w, out_h)) {
        butil::log.error("SVG has unsupported dimensions {}x{}", out_w, out_h);
        return std::nullopt;
    }

    auto bitmap = document->renderToBitmap(out_w, out_h);
    if (!bitmap.valid() || !bitmap.data()) {
        butil::log.error("lunasvg renderToBitmap failed");
        return std::nullopt;
    }
    bitmap.convertToRGBA();

    const int width  = static_cast<int>(bitmap.width());
    const int height = static_cast<int>(bitmap.height());
    if (!rgba_byte_size(width, height))
        return std::nullopt;

    const std::size_t row = static_cast<std::size_t>(width) * kBYTES_PER_PIXEL;
    colors::RgbaBuffer buf;
    buf.width  = width;
    buf.height = height;
    buf.pixels.resize(row * static_cast<std::size_t>(height));
    for (int y = 0; y < height; ++y)
        std::memcpy(buf.pixels.data() + static_cast<std::size_t>(y) * row,
            bitmap.data() + static_cast<std::size_t>(y) * bitmap.stride(),
            row);
    return buf;
}

}  // namespace biv
