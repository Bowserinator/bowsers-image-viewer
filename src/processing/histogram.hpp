#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>

#include "util/color.hpp"

using namespace biv::colors;

namespace biv::channel {

inline constexpr int kHISTOGRAM_W              = 400;
inline constexpr int kHISTOGRAM_H              = 300;
inline constexpr int kHISTOGRAM_MARGIN         = 6;
inline constexpr int kHISTOGRAM_LABEL_ROW_H    = 16;
inline constexpr int kHISTOGRAM_LABEL_TOP      = 4;
inline constexpr int kHISTOGRAM_LABEL_AREA_H   = kHISTOGRAM_LABEL_TOP + kHISTOGRAM_LABEL_ROW_H * 4;

struct HistogramOverlay {
    std::string path_r, path_g, path_b, path_k;
    std::string label_r_text, label_g_text, label_b_text, label_k_text;
};

inline std::string build_histogram_path(const std::array<std::uint64_t, 256>& counts, std::uint64_t max_count,
                                         int plot_x, int plot_y, int plot_w, int plot_h) {
    std::string out;
    for (int x = 0; x < plot_w; ++x) {
        const int bin     = std::clamp((x * 256) / plot_w, 0, 255);
        const double frac = max_count > 0 ? static_cast<double>(counts[bin]) / static_cast<double>(max_count) : 0.0;
        const double y    = plot_y + plot_h - frac * plot_h;
        out += (x == 0 ? "M " : " L ");
        out += std::to_string(plot_x + x);
        out += ",";
        out += std::to_string(y);
    }
    return out;
}

inline int histogram_median_bin(const std::array<std::uint64_t, 256>& counts, std::uint64_t total) {
    if (total == 0)
        return 0;
    const std::uint64_t half = total / 2;
    std::uint64_t running    = 0;
    for (int i = 0; i < 256; ++i) {
        running += counts[i];
        if (running >= half)
            return i;
    }
    return 255;
}

inline double histogram_mean(const std::array<std::uint64_t, 256>& counts, std::uint64_t total) {
    if (total == 0)
        return 0.0;
    std::uint64_t sum = 0;
    for (int i = 0; i < 256; ++i)
        sum += static_cast<std::uint64_t>(i) * counts[i];
    return static_cast<double>(sum) / static_cast<double>(total);
}

inline std::string histogram_label(char channel, int median, double mean) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%c  mu: %5.1f  med: %3d", channel, mean, median);
    return buf;
}

inline HistogramOverlay compute_histogram_overlay(const RgbaBuffer& src) {
    HistogramOverlay result;
    if (!src.valid())
        return result;

    std::array<std::array<std::uint64_t, 256>, 4> counts{};
    const std::size_t n   = src.pixel_count();
    const std::uint8_t* p = src.pixels.data();
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t r = p[kBYTES_PER_PIXEL * i + 0];
        const std::uint8_t g = p[kBYTES_PER_PIXEL * i + 1];
        const std::uint8_t b = p[kBYTES_PER_PIXEL * i + 2];
        counts[0][r]++;
        counts[1][g]++;
        counts[2][b]++;
        counts[3][luma8(r, g, b)]++;
    }

    std::uint64_t max_count = 0;
    for (const auto& c : counts)
        for (const auto v : c)
            max_count = std::max(max_count, v);

    const int plot_x = kHISTOGRAM_MARGIN;
    const int plot_y = kHISTOGRAM_LABEL_AREA_H;
    const int plot_w = kHISTOGRAM_W - 2 * kHISTOGRAM_MARGIN;
    const int plot_h = kHISTOGRAM_H - kHISTOGRAM_LABEL_AREA_H - kHISTOGRAM_MARGIN;

    result.path_r = build_histogram_path(counts[0], max_count, plot_x, plot_y, plot_w, plot_h);
    result.path_g = build_histogram_path(counts[1], max_count, plot_x, plot_y, plot_w, plot_h);
    result.path_b = build_histogram_path(counts[2], max_count, plot_x, plot_y, plot_w, plot_h);
    result.path_k = build_histogram_path(counts[3], max_count, plot_x, plot_y, plot_w, plot_h);

    const int median_r = histogram_median_bin(counts[0], n);
    const int median_g = histogram_median_bin(counts[1], n);
    const int median_b = histogram_median_bin(counts[2], n);
    const int median_k = histogram_median_bin(counts[3], n);

    result.label_r_text = histogram_label('R', median_r, histogram_mean(counts[0], n));
    result.label_g_text = histogram_label('G', median_g, histogram_mean(counts[1], n));
    result.label_b_text = histogram_label('B', median_b, histogram_mean(counts[2], n));
    result.label_k_text = histogram_label('K', median_k, histogram_mean(counts[3], n));

    return result;
}

}  // namespace biv::channel
