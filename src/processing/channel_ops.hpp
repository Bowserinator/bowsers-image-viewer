#pragma once

// Pure, allocation-explicit RGBA8 pixel transforms used by the channel/
// analysis views. No Slint or decode dependencies here, so this header is
// trivially unit-testable on its own.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <new>
#include <vector>

#include "config.hpp"
#include "util/color.hpp"

using namespace biv::colors;

namespace biv::channel {

enum class Mode {
    RGB,
    Luminance,
    Saturation,
    SatHue,
    ChannelR,
    ChannelG,
    ChannelB,
    SobelEdge,
    Dog,
    Invert,
    AdaptiveBinary,
    Superpixels,
    HighPass,
    Lsb0,
    Lsb1
};
inline constexpr std::size_t kCHANNEL_MODE_COUNT = static_cast<std::size_t>(Mode::Lsb1) + 1;

// Sobel edge detection: luminance -> Gaussian blur -> Sobel Gx/Gy -> magnitude.
// Expects a valid() buffer; apply() checks that.
inline RgbaBuffer apply_sobel(const RgbaBuffer& src) {
    const std::size_t W = static_cast<std::size_t>(src.width);
    const std::size_t H = static_cast<std::size_t>(src.height);

    // Light Gaussian pre-blur (sigma=1.0) to suppress noise.
    auto blurred = gaussian_blur(luminance_float(src), src.width, src.height, kSOBEL_BLUR_SIGMA);
    std::vector<float> mag(blurred.size());

    parallel_for(H, W * H * 8, [&](std::size_t y0, std::size_t y1) {
        for (std::size_t y = y0; y < y1; ++y) {
            // Clamp-to-edge rows; columns are handled per-pixel below.
            const float* r0 = blurred.data() + (y > 0 ? y - 1 : 0) * W;
            const float* r1 = blurred.data() + y * W;
            const float* r2 = blurred.data() + (y + 1 < H ? y + 1 : H - 1) * W;
            float* out      = mag.data() + y * W;

            auto at = [&](std::size_t x, std::size_t xm, std::size_t xp) {
                const float gx = (r0[xp] - r0[xm]) + 2.0f * (r1[xp] - r1[xm]) + (r2[xp] - r2[xm]);
                const float gy = (r2[xm] + 2.0f * r2[x] + r2[xp]) - (r0[xm] + 2.0f * r0[x] + r0[xp]);
                return std::sqrt(gx * gx + gy * gy);
            };

            out[0] = at(0, 0, W > 1 ? 1 : 0);
            for (std::size_t x = 1; x + 1 < W; ++x)
                out[x] = at(x, x - 1, x + 1);
            if (W > 1)
                out[W - 1] = at(W - 1, W - 2, W - 1);
        }
    });

    std::vector<float>().swap(blurred);  // free before allocating the RGBA output
    return normalise_to_rgba(mag, src);
}

// Difference of Gaussians: subtract two blur levels to isolate edges/details.
// Expects a valid() buffer; apply() checks that.
inline RgbaBuffer apply_dog(const RgbaBuffer& src) {
    auto lum    = luminance_float(src);
    auto fine   = gaussian_blur(lum, src.width, src.height, kDOG_FINE_SIGMA);
    auto coarse = gaussian_blur(lum, src.width, src.height, kDOG_COARSE_SIGMA);
    std::vector<float>().swap(lum);

    // fine -= coarse, in place (no separate diff plane).
    float* f       = fine.data();
    const float* c = coarse.data();
    parallel_for(fine.size(), fine.size(), [&](std::size_t b, std::size_t e) {
        for (std::size_t i = b; i < e; ++i)
            f[i] -= c[i];
    });

    return normalise_to_rgba(fine, src);
}

inline RgbaBuffer apply_adaptive_binary(const RgbaBuffer& src) {
    const std::size_t n = src.pixel_count();
    if (n == 0)
        return src;

    // Adaptive radius based on image dimensions
    const double sigma = std::max(2.0, static_cast<double>(std::min(src.width, src.height)) / 50.0);
    std::vector<float> r(n), g(n), b(n);
    const std::uint8_t* p = src.pixels.data();

    parallel_for(n, n, [&](std::size_t b0, std::size_t e0) {
        for (std::size_t i = b0; i < e0; ++i) {
            r[i] = p[kBYTES_PER_PIXEL * i + 0];
            g[i] = p[kBYTES_PER_PIXEL * i + 1];
            b[i] = p[kBYTES_PER_PIXEL * i + 2];
        }
    });

    // Compute the local Gaussian mean for each channel
    const auto mu_r = gaussian_blur(r, src.width, src.height, sigma);
    const auto mu_g = gaussian_blur(g, src.width, src.height, sigma);
    const auto mu_b = gaussian_blur(b, src.width, src.height, sigma);

    RgbaBuffer out;
    out.width  = src.width;
    out.height = src.height;
    out.pixels.resize(src.pixels.size());
    std::uint8_t* o = out.pixels.data();

    // A small constant offset (C) prevents flat, uniform regions (like skies)
    // from turning into pure static noise due to microscopic variance.
    const float C = 4.0f;

    parallel_for(n, n, [&](std::size_t b0, std::size_t e0) {
        for (std::size_t i = b0; i < e0; ++i) {
            o[kBYTES_PER_PIXEL * i + 0] = (r[i] >= mu_r[i] - C) ? 255 : 0;
            o[kBYTES_PER_PIXEL * i + 1] = (g[i] >= mu_g[i] - C) ? 255 : 0;
            o[kBYTES_PER_PIXEL * i + 2] = (b[i] >= mu_b[i] - C) ? 255 : 0;
            o[kBYTES_PER_PIXEL * i + 3] = p[kBYTES_PER_PIXEL * i + 3];
        }
    });

    return out;
}

inline RgbaBuffer apply_superpixels(const RgbaBuffer& src) {
    const int width = src.width;
    const int height = src.height;
    const std::size_t n = src.pixel_count();
    if (n == 0)
        return src;

    const std::uint8_t* p = src.pixels.data();

    std::vector<Oklab> lab_image(n);
    parallel_for(n, n, [&](std::size_t b, std::size_t e) {
        for (std::size_t i = b; i < e; ++i)
            lab_image[i] = rgb_to_oklab(p[kBYTES_PER_PIXEL * i + 0],
                                        p[kBYTES_PER_PIXEL * i + 1],
                                        p[kBYTES_PER_PIXEL * i + 2]);
    });

    // SLIC grid interval derived from the configured target number of regions.
    const std::size_t target_superpixels = std::min<std::size_t>(kSUPERPIXEL_COUNT, n);
    const int S = std::max(1, static_cast<int>(std::sqrt(static_cast<double>(n) / target_superpixels)));
    constexpr float m_compactness = 0.15f;  // Oklab needs a much smaller value than CIELAB.
    const float spatial_weight = (m_compactness * m_compactness) /
                                 (static_cast<float>(S) * static_cast<float>(S));

    struct Cluster {
        float L, a, b;
        float x, y;
    };

    const std::size_t grid_w = (static_cast<std::size_t>(width) + S - 1) / S;
    const std::size_t grid_h = (static_cast<std::size_t>(height) + S - 1) / S;
    std::vector<Cluster> clusters;
    clusters.reserve(grid_w * grid_h);
    for (int y = S / 2; y < height; y += S) {
        for (int x = S / 2; x < width; x += S) {
            const auto& lab = lab_image[static_cast<std::size_t>(y) * width + x];
            clusters.push_back({lab.L, lab.a, lab.b, static_cast<float>(x), static_cast<float>(y)});
        }
    }

    const std::size_t num_clusters = clusters.size();
    if (num_clusters == 0)
        return src;

    // Shift each seed to the lowest local Oklab gradient in a 3x3 neighborhood.
    auto get_gradient = [&](int x, int y) -> float {
        if (x <= 0 || x >= width - 1 || y <= 0 || y >= height - 1)
            return std::numeric_limits<float>::max();

        const auto& left  = lab_image[static_cast<std::size_t>(y) * width + (x - 1)];
        const auto& right = lab_image[static_cast<std::size_t>(y) * width + (x + 1)];
        const auto& top   = lab_image[static_cast<std::size_t>(y - 1) * width + x];
        const auto& bottom = lab_image[static_cast<std::size_t>(y + 1) * width + x];

        const float dxL = right.L - left.L;
        const float dxa = right.a - left.a;
        const float dxb = right.b - left.b;
        const float dyL = bottom.L - top.L;
        const float dya = bottom.a - top.a;
        const float dyb = bottom.b - top.b;
        return dxL * dxL + dxa * dxa + dxb * dxb + dyL * dyL + dya * dya + dyb * dyb;
    };

    for (auto& c : clusters) {
        const int seed_x = static_cast<int>(c.x);
        const int seed_y = static_cast<int>(c.y);
        int best_x = seed_x;
        int best_y = seed_y;
        float min_grad = get_gradient(best_x, best_y);

        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const int nx = seed_x + dx;
                const int ny = seed_y + dy;
                const float grad = get_gradient(nx, ny);
                if (grad < min_grad) {
                    min_grad = grad;
                    best_x = nx;
                    best_y = ny;
                }
            }
        }

        c.x = static_cast<float>(best_x);
        c.y = static_cast<float>(best_y);
        const auto& best = lab_image[static_cast<std::size_t>(best_y) * width + best_x];
        c.L = best.L;
        c.a = best.a;
        c.b = best.b;
    }

    std::vector<int> labels(n, -1);
    std::vector<float> min_distances(n, std::numeric_limits<float>::max());

    // Standard SLIC assignment in 2S x 2S windows.
    for (int iter = 0; iter < kSLIC_ITERATIONS; ++iter) {
        std::fill(min_distances.begin(), min_distances.end(), std::numeric_limits<float>::max());

        for (std::size_t c = 0; c < num_clusters; ++c) {
            const auto& cluster = clusters[c];
            const int center_x = static_cast<int>(cluster.x);
            const int center_y = static_cast<int>(cluster.y);
            const int min_x = std::max(0, center_x - S);
            const int max_x = std::min(width - 1, center_x + S);
            const int min_y = std::max(0, center_y - S);
            const int max_y = std::min(height - 1, center_y + S);

            for (int y = min_y; y <= max_y; ++y) {
                for (int x = min_x; x <= max_x; ++x) {
                    const std::size_t idx = static_cast<std::size_t>(y) * width + x;
                    const auto& lab = lab_image[idx];

                    const float dL = lab.L - cluster.L;
                    const float da = lab.a - cluster.a;
                    const float db = lab.b - cluster.b;
                    const float d_color_sq = dL * dL + da * da + db * db;

                    const float dx = static_cast<float>(x) - cluster.x;
                    const float dy = static_cast<float>(y) - cluster.y;
                    const float d_spatial_sq = dx * dx + dy * dy;

                    const float dist = d_color_sq + spatial_weight * d_spatial_sq;
                    if (dist < min_distances[idx]) {
                        min_distances[idx] = dist;
                        labels[idx] = static_cast<int>(c);
                    }
                }
            }
        }

        std::vector<double> sum_L(num_clusters, 0.0), sum_a(num_clusters, 0.0), sum_b(num_clusters, 0.0);
        std::vector<double> sum_x(num_clusters, 0.0), sum_y(num_clusters, 0.0);
        std::vector<std::size_t> counts(num_clusters, 0);

        for (std::size_t y = 0; y < static_cast<std::size_t>(height); ++y) {
            for (std::size_t x = 0; x < static_cast<std::size_t>(width); ++x) {
                const std::size_t idx = y * width + x;
                const int label = labels[idx];
                if (label < 0)
                    continue;

                const auto& lab = lab_image[idx];
                sum_L[label] += lab.L;
                sum_a[label] += lab.a;
                sum_b[label] += lab.b;
                sum_x[label] += static_cast<double>(x);
                sum_y[label] += static_cast<double>(y);
                ++counts[label];
            }
        }

        for (std::size_t c = 0; c < num_clusters; ++c) {
            if (counts[c] == 0)
                continue;
            const double inv = 1.0 / static_cast<double>(counts[c]);
            clusters[c].L = static_cast<float>(sum_L[c] * inv);
            clusters[c].a = static_cast<float>(sum_a[c] * inv);
            clusters[c].b = static_cast<float>(sum_b[c] * inv);
            clusters[c].x = static_cast<float>(sum_x[c] * inv);
            clusters[c].y = static_cast<float>(sum_y[c] * inv);
        }
    }

    std::vector<std::array<std::uint8_t, 3>> cluster_rgb(num_clusters);
    for (std::size_t c = 0; c < num_clusters; ++c)
        cluster_rgb[c] = oklab_to_rgb({clusters[c].L, clusters[c].a, clusters[c].b});

    RgbaBuffer out;
    out.width  = src.width;
    out.height = src.height;
    out.pixels.resize(src.pixels.size());
    std::uint8_t* o = out.pixels.data();

    parallel_for(n, n, [&](std::size_t b0, std::size_t e0) {
        for (std::size_t i = b0; i < e0; ++i) {
            const std::size_t pi = kBYTES_PER_PIXEL * i;
            const int label = labels[i];
            if (label >= 0) {
                o[pi + 0] = cluster_rgb[label][0];
                o[pi + 1] = cluster_rgb[label][1];
                o[pi + 2] = cluster_rgb[label][2];
            } else {
                o[pi + 0] = p[pi + 0];
                o[pi + 1] = p[pi + 1];
                o[pi + 2] = p[pi + 2];
            }
            o[pi + 3] = p[pi + 3];
        }
    });

    return out;
}

// High-pass: original minus its own blur, biased back to mid-gray so the
// (mostly-zero) result stays visible instead of clipping to black.
// Expects a valid() buffer; apply() checks that.
inline RgbaBuffer apply_high_pass(const RgbaBuffer& src) {
    const auto blurred = blur_channels(src, kBLUR_FILTER_SIGMA);

    RgbaBuffer out;
    out.width  = src.width;
    out.height = src.height;
    out.pixels.resize(src.pixels.size());

    const std::size_t n    = src.pixel_count();
    const std::uint8_t* s  = src.pixels.data();
    const std::uint8_t* bl = blurred.pixels.data();
    std::uint8_t* o        = out.pixels.data();
    parallel_for(n, n, [&](std::size_t b0, std::size_t e0) {
        for (std::size_t i = b0; i < e0; ++i) {
            for (int c = 0; c < 3; ++c) {
                const int diff = static_cast<int>(s[kBYTES_PER_PIXEL * i + c]) -
                                 static_cast<int>(bl[kBYTES_PER_PIXEL * i + c]) + static_cast<int>(kMID_GRAY);
                o[kBYTES_PER_PIXEL * i + c] = clamp_u8(diff);
            }
            o[kBYTES_PER_PIXEL * i + 3] = s[kBYTES_PER_PIXEL * i + 3];
        }
    });
    return out;
}

// Bit-plane visualization for steganography analysis: a pixel is shown white
// when `bit` is set in the red, green or blue channel, and black otherwise.
// Shared by the LSB (bit 0) and next-bit (bit 1) filters.
// Expects a valid() buffer; apply() checks that.
inline RgbaBuffer apply_bit_plane(const RgbaBuffer& src, int bit) {
    return map_pixels(src, [bit](const std::uint8_t* in, std::uint8_t* o) {
        const std::uint8_t combined = in[0] | in[1] | in[2];
        const std::uint8_t v        = ((combined >> bit) & 1u) ? static_cast<std::uint8_t>(kU8_MAX) : 0;
        o[0] = o[1] = o[2] = v;
    });
}

// Isolates one RGB channel (0=R, 1=G, 2=B), zeroing the other two. Shared by
// the ChannelR/ChannelG/ChannelB modes
inline RgbaBuffer apply_channel_isolate(const RgbaBuffer& src, int channel) {
    return map_pixels(src, [channel](const std::uint8_t* in, std::uint8_t* o) {
        o[0] = channel == 0 ? in[0] : 0;
        o[1] = channel == 1 ? in[1] : 0;
        o[2] = channel == 2 ? in[2] : 0;
    });
}

// Applies `mode` to `src`, returning a freshly-allocated RGBA8 buffer of the
// same dimensions. `mode == Mode::RGB` returns a copy of `src` unchanged so
// callers can always treat the result uniformly. Invalid buffers and
// out-of-memory also return an unchanged copy rather than crashing.
inline RgbaBuffer apply(const RgbaBuffer& src, Mode mode) {
    if (mode == Mode::RGB || !src.valid())
        return src;

    try {
        switch (mode) {
            case Mode::SobelEdge: return apply_sobel(src);
            case Mode::Dog: return apply_dog(src);
            case Mode::AdaptiveBinary: return apply_adaptive_binary(src);
            case Mode::Superpixels: return apply_superpixels(src);
            case Mode::HighPass: return apply_high_pass(src);
            case Mode::Lsb0: return apply_bit_plane(src, 0);
            case Mode::Lsb1: return apply_bit_plane(src, 1);
            case Mode::Invert:
                return map_pixels(src, [](const std::uint8_t* in, std::uint8_t* o) {
                    o[0] = static_cast<std::uint8_t>(kU8_MAX) - in[0];
                    o[1] = static_cast<std::uint8_t>(kU8_MAX) - in[1];
                    o[2] = static_cast<std::uint8_t>(kU8_MAX) - in[2];
                });
            case Mode::Luminance:
                return map_pixels(src,
                    [](const std::uint8_t* in, std::uint8_t* o) { o[0] = o[1] = o[2] = luma8(in[0], in[1], in[2]); });
            case Mode::Saturation:
                return map_pixels(src, [](const std::uint8_t* in, std::uint8_t* o) {
                    const unsigned mx = std::max({in[0], in[1], in[2]});
                    const unsigned mn = std::min({in[0], in[1], in[2]});
                    o[0] = o[1] = o[2] = static_cast<std::uint8_t>(mx ? ((mx - mn) * 255u + mx / 2) / mx : 0u);
                });
            case Mode::SatHue:
                return map_pixels(src, [](const std::uint8_t* in, std::uint8_t* o) {
                    const auto hsv = rgb_to_hsv(in[0] / kU8_MAX, in[1] / kU8_MAX, in[2] / kU8_MAX);
                    double rr, gg, bb;
                    hsv_to_rgb(hsv.h / 360.0, hsv.s, kSAT_HUE_SATURATION, rr, gg, bb);
                    o[0] = clamp_u8(rr * kU8_MAX);
                    o[1] = clamp_u8(gg * kU8_MAX);
                    o[2] = clamp_u8(bb * kU8_MAX);
                });
            case Mode::ChannelR: return apply_channel_isolate(src, 0);
            case Mode::ChannelG: return apply_channel_isolate(src, 1);
            case Mode::ChannelB: return apply_channel_isolate(src, 2);
            case Mode::RGB: break;
        }
    } catch (const std::bad_alloc&) {
        // fall through: show the unmodified image instead of terminating
    }
    return src;
}

// Applies the purple->red heatmap colormap to `src`. Each pixel's luma is
// mapped through a precomputed 256-entry gradient. Used as a post-processing
// overlay when the heatmap toggle is enabled.
inline RgbaBuffer apply_heatmap(const RgbaBuffer& src) {
    if (!src.valid())
        return src;
    try {
        const auto& lut = heatmap_lut();
        return map_pixels(src, [&lut](const std::uint8_t* in, std::uint8_t* o) {
            const auto& c = lut[luma8(in[0], in[1], in[2])];
            o[0]          = c[0];
            o[1]          = c[1];
            o[2]          = c[2];
        });
    } catch (const std::bad_alloc&) { return src; }
}

}  // namespace biv::channel
