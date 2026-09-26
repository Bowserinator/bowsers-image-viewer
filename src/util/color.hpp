#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

#include "config.hpp"

namespace biv {
namespace colors {

// Worker-thread cap for parallel_for(), defaulting to kDEFAULT_PARALLEL_THREADS
// and overridable at startup via set_thread_count() (wired to the -j/--jobs
// CLI flag in main.cpp).
inline std::atomic<std::size_t>& thread_count_slot() {
    static std::atomic<std::size_t> count{kDEFAULT_PARALLEL_THREADS};
    return count;
}

inline void set_thread_count(std::size_t n) noexcept {
    thread_count_slot().store(std::max<std::size_t>(1, n), std::memory_order_relaxed);
}

inline std::size_t thread_count() noexcept {
    return thread_count_slot().load(std::memory_order_relaxed);
}

struct RgbaBuffer {
    int width  = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;  // RGBA8, width*height*4 bytes

    [[nodiscard]] std::size_t pixel_count() const noexcept {
        return static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    }

    [[nodiscard]] bool valid() const noexcept {
        return width > 0 && height > 0 && pixels.size() == pixel_count() * kBYTES_PER_PIXEL;
    }
};

struct Hsv {
    double h = 0.0;  // [0, 360)
    double s = 0.0;  // [0, 1]
    double v = 0.0;  // [0, 1]
};

struct Oklab {
    float L = 0.0f;
    float a = 0.0f;
    float b = 0.0f;
};

inline double srgb_to_linear(double value) noexcept {
    return value > 0.04045 ? std::pow((value + 0.055) / 1.055, 2.4) : value / 12.92;
}

inline double linear_to_srgb(double value) noexcept {
    value = std::clamp(value, 0.0, 1.0);
    return value > 0.0031308 ? 1.055 * std::pow(value, 1.0 / 2.4) - 0.055 : 12.92 * value;
}

inline Oklab rgb_to_oklab(std::uint8_t r_u8, std::uint8_t g_u8, std::uint8_t b_u8) noexcept {
    const double r = srgb_to_linear(r_u8 / kU8_MAX);
    const double g = srgb_to_linear(g_u8 / kU8_MAX);
    const double b = srgb_to_linear(b_u8 / kU8_MAX);

    const double l = 0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b;
    const double m = 0.2119034982 * r + 0.6806995451 * g + 0.1073969597 * b;
    const double s = 0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b;

    const double l_ = std::cbrt(l);
    const double m_ = std::cbrt(m);
    const double s_ = std::cbrt(s);

    return {
        static_cast<float>(0.2104542553 * l_ + 0.7936177850 * m_ - 0.0040720468 * s_),
        static_cast<float>(1.9779984951 * l_ - 2.4285922050 * m_ + 0.4505937099 * s_),
        static_cast<float>(0.0259040371 * l_ + 0.7827717662 * m_ - 0.8086757660 * s_),
    };
}

inline std::array<std::uint8_t, 3> oklab_to_rgb(Oklab lab) noexcept {
    const float l_ = lab.L + 0.3963377774f * lab.a + 0.2158037573f * lab.b;
    const float m_ = lab.L - 0.1055613458f * lab.a - 0.0638541728f * lab.b;
    const float s_ = lab.L - 0.0894841775f * lab.a - 1.2914855480f * lab.b;

    const float l = l_ * l_ * l_;
    const float m = m_ * m_ * m_;
    const float s = s_ * s_ * s_;

    const double r_lin = +4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s;
    const double g_lin = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s;
    const double b_lin = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s;

    return {
        static_cast<std::uint8_t>(std::clamp(linear_to_srgb(r_lin) * kU8_MAX, kU8_MIN, kU8_MAX)),
        static_cast<std::uint8_t>(std::clamp(linear_to_srgb(g_lin) * kU8_MAX, kU8_MIN, kU8_MAX)),
        static_cast<std::uint8_t>(std::clamp(linear_to_srgb(b_lin) * kU8_MAX, kU8_MIN, kU8_MAX)),
    };
}

inline Hsv rgb_to_hsv(double r, double g, double b) {
    const double max_c = std::max({r, g, b});
    const double min_c = std::min({r, g, b});
    const double delta = max_c - min_c;

    Hsv out;
    out.v = max_c;

    if (delta < kCHROMA_EPSILON) {
        out.h = 0.0;
        out.s = 0.0;
        return out;
    }

    out.s = delta / max_c;

    if (max_c == r)
        out.h = std::fmod((g - b) / delta, 6.0);
    else if (max_c == g)
        out.h = (b - r) / delta + 2.0;
    else
        out.h = (r - g) / delta + 4.0;

    out.h *= 60.0;
    if (out.h < 0.0)
        out.h += 360.0;
    return out;
}

inline void hsv_to_rgb(double h, double s, double v, double& r, double& g, double& b) {
    if (s < kCHROMA_EPSILON) {
        r = g = b = v;
        return;
    }
    const double i = std::floor(h * 6.0);
    const double f = h * 6.0 - i;
    const double p = v * (1.0 - s);
    const double q = v * (1.0 - f * s);
    const double t = v * (1.0 - (1.0 - f) * s);
    switch (static_cast<int>(i) % 6) {
        case 0:
            r = v;
            g = t;
            b = p;
            break;
        case 1:
            r = q;
            g = v;
            b = p;
            break;
        case 2:
            r = p;
            g = v;
            b = t;
            break;
        case 3:
            r = p;
            g = q;
            b = v;
            break;
        case 4:
            r = t;
            g = p;
            b = v;
            break;
        default:
            r = v;
            g = p;
            b = q;
            break;
    }
}

inline std::uint8_t clamp_u8(double v) {
    return static_cast<std::uint8_t>(std::clamp(v, kU8_MIN, kU8_MAX));
}

// Purple->red heatmap colormap for the overlay. t in [0,1].
inline void heatmap_purple_red(double t, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) {
    t = std::clamp(t, 0.0, 1.0);
    static constexpr std::array<std::array<double, 3>, 5> stops{{
        {0.30, 0.00, 0.50},  // dark purple
        {0.00, 0.00, 1.00},  // blue
        {0.00, 1.00, 0.00},  // green
        {1.00, 1.00, 0.00},  // yellow
        {1.00, 0.00, 0.00},  // red
    }};
    const double scaled  = t * (stops.size() - 1);
    const std::size_t i0 = static_cast<std::size_t>(scaled);
    const std::size_t i1 = std::min(i0 + 1, stops.size() - 1);
    const double frac    = scaled - static_cast<double>(i0);

    auto lerp = [frac](double a, double b_) {
        return a + (b_ - a) * frac;
    };
    r = clamp_u8(lerp(stops[i0][0], stops[i1][0]) * kU8_MAX);
    g = clamp_u8(lerp(stops[i0][1], stops[i1][1]) * kU8_MAX);
    b = clamp_u8(lerp(stops[i0][2], stops[i1][2]) * kU8_MAX);
}

// 256-entry colormap indexed by luma8, built once.
inline const std::array<std::array<std::uint8_t, 3>, kHEATMAP_LUT_SIZE>& heatmap_lut() {
    static const auto lut = [] {
        std::array<std::array<std::uint8_t, 3>, kHEATMAP_LUT_SIZE> t{};
        for (std::size_t i = 0; i < t.size(); ++i)
            heatmap_purple_red(static_cast<double>(i) / kU8_MAX, t[i][0], t[i][1], t[i][2]);
        return t;
    }();
    return lut;
}

// Splits [0, count) into contiguous chunks and runs fn(begin, end) on each,
// in parallel once `total_work` is big enough to amortise thread startup.
template <class F>
void parallel_for(std::size_t count, std::size_t total_work, F&& fn) {
    const std::size_t nt = std::min(thread_count(), count);
    if (nt <= 1 || total_work < kPARALLEL_THRESHOLD) {
        fn(std::size_t{0}, count);
        return;
    }
    const std::size_t chunk = (count + nt - 1) / nt;
    std::vector<std::jthread> pool;  // joins on scope exit
    pool.reserve(nt - 1);
    for (std::size_t t = 1; t < nt; ++t) {
        const std::size_t b = t * chunk;
        const std::size_t e = std::min(count, b + chunk);
        if (b >= e)
            break;
        pool.emplace_back([&fn, b, e] { fn(b, e); });
    }
    fn(std::size_t{0}, std::min(count, chunk));
}

// Integer Rec.709 luma (weights 54/183/19 of 256), result in [0, 255].
inline std::uint8_t luma8(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept {
    return static_cast<std::uint8_t>((kLUMA_INT_R * r + kLUMA_INT_G * g + kLUMA_INT_B * b + kLUMA_INT_ROUND) >> 8);
}

// Runs px(in_rgba, out_rgba) for every pixel (parallel), then copies alpha.
// The switch on mode stays outside the loop so each mode gets a tight loop.
template <class Px>
RgbaBuffer map_pixels(const RgbaBuffer& src, Px&& px) {
    RgbaBuffer out;
    out.width  = src.width;
    out.height = src.height;
    out.pixels.resize(src.pixels.size());

    const std::size_t n   = src.pixel_count();
    const std::uint8_t* s = src.pixels.data();
    std::uint8_t* d       = out.pixels.data();
    parallel_for(n, n, [&](std::size_t b, std::size_t e) {
        const std::uint8_t* in = s + b * kBYTES_PER_PIXEL;
        std::uint8_t* o        = d + b * kBYTES_PER_PIXEL;
        for (std::size_t i = b; i < e; ++i, in += kBYTES_PER_PIXEL, o += kBYTES_PER_PIXEL) {
            px(in, o);
            o[3] = in[3];
        }
    });
    return out;
}

// Applies the same content transforms used by the viewport: horizontal/vertical
// flips followed by clockwise rotation. Zoom and pan are intentionally excluded.
inline RgbaBuffer apply_orientation(const RgbaBuffer& src, int rotation_degrees, bool flip_h, bool flip_v) {
    if (!src.valid())
        return src;

    rotation_degrees %= 360;
    if (rotation_degrees < 0)
        rotation_degrees += 360;
    if (!flip_h && !flip_v && rotation_degrees == 0)
        return src;

    const int src_w = src.width;
    const int src_h = src.height;
    const bool swap_dims = rotation_degrees == 90 || rotation_degrees == 270;

    RgbaBuffer out;
    out.width = swap_dims ? src_h : src_w;
    out.height = swap_dims ? src_w : src_h;
    out.pixels.resize(out.pixel_count() * kBYTES_PER_PIXEL);

    const std::size_t src_stride = static_cast<std::size_t>(src_w) * kBYTES_PER_PIXEL;
    const std::size_t dst_stride = static_cast<std::size_t>(out.width) * kBYTES_PER_PIXEL;
    const std::uint8_t* p = src.pixels.data();
    std::uint8_t* d = out.pixels.data();

    parallel_for(static_cast<std::size_t>(src_h), src.pixel_count(), [&](std::size_t y0, std::size_t y1) {
        for (std::size_t y = y0; y < y1; ++y) {
            for (int x = 0; x < src_w; ++x) {
                const int sx = flip_h ? src_w - 1 - x : x;
                const int sy = flip_v ? src_h - 1 - static_cast<int>(y) : static_cast<int>(y);

                int dx = 0;
                int dy = 0;
                switch (rotation_degrees) {
                    case 90:
                        dx = src_h - 1 - sy;
                        dy = sx;
                        break;
                    case 180:
                        dx = src_w - 1 - sx;
                        dy = src_h - 1 - sy;
                        break;
                    case 270:
                        dx = sy;
                        dy = src_w - 1 - sx;
                        break;
                    default:
                        dx = sx;
                        dy = sy;
                        break;
                }

                const auto* in = p + y * src_stride + static_cast<std::size_t>(x) * kBYTES_PER_PIXEL;
                auto* out_px = d + static_cast<std::size_t>(dy) * dst_stride + static_cast<std::size_t>(dx) * kBYTES_PER_PIXEL;
                std::copy_n(in, kBYTES_PER_PIXEL, out_px);
            }
        }
    });

    return out;
}

// Normalised 1D Gaussian kernel of half-width `radius`.
inline std::vector<float> gaussian_kernel(int radius, double sigma) {
    const std::size_t size = 2 * static_cast<std::size_t>(radius) + 1;
    std::vector<double> wts(size);
    const double s2 = 2.0 * sigma * sigma;
    double sum      = 0.0;
    for (std::size_t i = 0; i < size; ++i) {
        const double x = static_cast<double>(i) - radius;
        wts[i]         = std::exp(-(x * x) / s2);
        sum += wts[i];
    }
    std::vector<float> kernel(size);
    for (std::size_t i = 0; i < size; ++i)
        kernel[i] = static_cast<float>(wts[i] / sum);
    return kernel;
}

// Separable Gaussian blur (clamp-to-edge) on a single-channel float buffer.
// Both passes run on contiguous rows so the tap loops auto-vectorise, and
// rows are split across threads.
inline std::vector<float> gaussian_blur(const std::vector<float>& src, int w, int h, double sigma) {
    if (sigma <= 0.0 || w <= 0 || h <= 0)
        return src;

    const int radius       = static_cast<int>(std::ceil(sigma * kBLUR_RADIUS_FACTOR));
    const auto kernel      = gaussian_kernel(radius, sigma);
    const std::size_t R    = static_cast<std::size_t>(radius);
    const std::size_t T    = 2 * R + 1;
    const std::size_t W    = static_cast<std::size_t>(w);
    const std::size_t H    = static_cast<std::size_t>(h);
    const std::size_t work = W * H * T;

    std::vector<float> tmp(src.size());
    std::vector<float> dst(src.size());

    // Horizontal: pad each row once (clamp-to-edge) so the tap loop is branch-free.
    parallel_for(H, work, [&](std::size_t y0, std::size_t y1) {
        std::vector<float> pad(W + 2 * R);
        for (std::size_t y = y0; y < y1; ++y) {
            const float* row = src.data() + y * W;
            std::fill_n(pad.data(), R, row[0]);
            std::copy_n(row, W, pad.data() + R);
            std::fill_n(pad.data() + R + W, R, row[W - 1]);

            float* out     = tmp.data() + y * W;
            const float* p = pad.data();
            for (std::size_t x = 0; x < W; ++x)
                out[x] = kernel[0] * p[x];
            for (std::size_t k = 1; k < T; ++k) {
                const float kv = kernel[k];
                for (std::size_t x = 0; x < W; ++x)
                    out[x] += kv * p[x + k];
            }
        }
    });

    // Vertical: accumulate whole rows (contiguous) instead of striding columns.
    parallel_for(H, work, [&](std::size_t y0, std::size_t y1) {
        for (std::size_t y = y0; y < y1; ++y) {
            float* out = dst.data() + y * W;
            for (std::size_t k = 0; k < T; ++k) {
                const std::size_t sy = (y + k < R) ? 0 : std::min(y + k - R, H - 1);
                const float* s       = tmp.data() + sy * W;
                const float kv       = kernel[k];
                if (k == 0)
                    for (std::size_t x = 0; x < W; ++x)
                        out[x] = kv * s[x];
                else
                    for (std::size_t x = 0; x < W; ++x)
                        out[x] += kv * s[x];
            }
        }
    });
    return dst;
}

// Luminance channel as float [0,255].
inline std::vector<float> luminance_float(const RgbaBuffer& src) {
    const std::size_t n = src.pixel_count();
    std::vector<float> lum(n);
    const std::uint8_t* p = src.pixels.data();
    float* l              = lum.data();
    parallel_for(n, n, [&](std::size_t b, std::size_t e) {
        for (std::size_t i = b; i < e; ++i)
            l[i] = kLUMINANCE_R * p[kBYTES_PER_PIXEL * i] + kLUMINANCE_G * p[kBYTES_PER_PIXEL * i + 1] +
                   kLUMINANCE_B * p[kBYTES_PER_PIXEL * i + 2];
    });
    return lum;
}

// Stretch a float buffer (size == pixel_count, non-empty) to [0,255] as RGBA8
// grayscale, taking alpha from `alpha_src`.
inline RgbaBuffer normalise_to_rgba(const std::vector<float>& data, const RgbaBuffer& alpha_src) {
    const std::size_t n = alpha_src.pixel_count();
    const auto [mn, mx] = std::minmax_element(data.begin(), data.end());
    const float lo      = *mn;
    const float range   = *mx - lo;
    const bool uniform  = range < kUNIFORM_RANGE_EPSILON;
    const float scale   = uniform ? 0.0f : kU8_MAX_F / range;

    RgbaBuffer out;
    out.width  = alpha_src.width;
    out.height = alpha_src.height;
    out.pixels.resize(alpha_src.pixels.size());

    const float* d        = data.data();
    const std::uint8_t* a = alpha_src.pixels.data();
    std::uint8_t* o       = out.pixels.data();
    parallel_for(n, n, [&](std::size_t b, std::size_t e) {
        for (std::size_t i = b; i < e; ++i) {
            const float v               = uniform ? kMID_GRAY : (d[i] - lo) * scale;
            const auto u                = static_cast<std::uint8_t>(std::clamp(v, kU8_MIN_F, kU8_MAX_F));
            o[kBYTES_PER_PIXEL * i + 0] = u;
            o[kBYTES_PER_PIXEL * i + 1] = u;
            o[kBYTES_PER_PIXEL * i + 2] = u;
            o[kBYTES_PER_PIXEL * i + 3] = a[kBYTES_PER_PIXEL * i + 3];
        }
    });
    return out;
}

// Blurs R, G and B independently with a Gaussian of the given sigma (alpha is left untouched)
inline RgbaBuffer blur_channels(const RgbaBuffer& src, double sigma) {
    const std::size_t n = src.pixel_count();
    std::vector<float> r(n), g(n), b(n);
    const std::uint8_t* p = src.pixels.data();
    parallel_for(n, n, [&](std::size_t b0, std::size_t e0) {
        for (std::size_t i = b0; i < e0; ++i) {
            r[i] = p[kBYTES_PER_PIXEL * i + 0];
            g[i] = p[kBYTES_PER_PIXEL * i + 1];
            b[i] = p[kBYTES_PER_PIXEL * i + 2];
        }
    });

    const auto br = gaussian_blur(r, src.width, src.height, sigma);
    const auto bg = gaussian_blur(g, src.width, src.height, sigma);
    const auto bb = gaussian_blur(b, src.width, src.height, sigma);

    RgbaBuffer out;
    out.width  = src.width;
    out.height = src.height;
    out.pixels.resize(src.pixels.size());
    std::uint8_t* o = out.pixels.data();
    parallel_for(n, n, [&](std::size_t b0, std::size_t e0) {
        for (std::size_t i = b0; i < e0; ++i) {
            o[kBYTES_PER_PIXEL * i + 0] = clamp_u8(br[i]);
            o[kBYTES_PER_PIXEL * i + 1] = clamp_u8(bg[i]);
            o[kBYTES_PER_PIXEL * i + 2] = clamp_u8(bb[i]);
            o[kBYTES_PER_PIXEL * i + 3] = p[kBYTES_PER_PIXEL * i + 3];
        }
    });
    return out;
}

}  // namespace colors
}  // namespace biv