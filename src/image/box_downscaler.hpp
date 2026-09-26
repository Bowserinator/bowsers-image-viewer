#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "util/color.hpp"

namespace biv::channel {

// Row-at-a-time area-average downscaler for thumbnails.
//
// Decoders that produce rows in order (PNG via libpng, most scanline codecs)
// can feed each row here as soon as it is decoded, so the full-size image is
// never materialised: no W*H*4 buffer, no separate resize pass over it. Memory
// is a few rows of the *thumbnail* width, however large the source is.
//
// Every output pixel is the exact area-weighted mean of the source pixels it
// covers (a true box filter, so fractional ratios don't alias), averaged in
// linear light with alpha weighting, matching what the stb_image_resize2 sRGB
// path used for thumbnails did. Never upscales: the output size is
// thumbnail_size(src_width, src_height, max_dim).
//
// Usage: construct, call add_row() exactly src_height times (top to bottom),
// then finish().
class BoxDownscaler {
public:
    // src_width/src_height must be positive; validate untrusted values first
    // (rgba_byte_size()).
    BoxDownscaler(int src_width, int src_height, int max_dim);

    [[nodiscard]] int width() const noexcept { return m_tw; }

    [[nodiscard]] int height() const noexcept { return m_th; }

    // `row` holds src_width interleaved 8-bit pixels of `channels` samples:
    // 1 = gray, 2 = gray+alpha, 3 = RGB, 4 = RGBA. Rows beyond src_height, a
    // null row or a bad channel count are ignored (complete() stays false).
    void add_row(const std::uint8_t* row, int channels);

    [[nodiscard]] bool complete() const noexcept { return m_rows_seen == m_sh; }

    // Moves the thumbnail out. If the rows were not all supplied the result is
    // the opaque black tile, never a half-written image.
    [[nodiscard]] colors::RgbaBuffer finish();

private:
    // Source columns feeding one output column. Pixels first+1..last-1 lie
    // fully inside it (weight w_mid each); `first` and `last` straddle its
    // edges. first == last means a single source pixel covers the column.
    struct Span {
        int first    = 0;
        int last     = 0;
        float w_first = 1.0f;
        float w_mid   = 0.0f;
        float w_last  = 0.0f;
    };

    template <int C>
    void reduce_row(const std::uint8_t* row);
    void copy_row(const std::uint8_t* row, int channels);
    void accumulate_vertical();
    void emit_row(int out_row, const float* acc);

    int m_sw = 1;
    int m_sh = 1;
    int m_tw = 1;
    int m_th = 1;
    bool m_identity = false;  // source already fits: rows are copied, not averaged

    int m_rows_seen = 0;
    int m_out_row   = 0;  // output row currently being accumulated

    std::vector<Span> m_cols;
    std::vector<float> m_hrow;  // one source row reduced to output width (premultiplied linear RGB + A)
    std::vector<float> m_cur;   // accumulators for output row m_out_row ...
    std::vector<float> m_next;  // ... and the one after it (a source row can straddle the boundary)
    colors::RgbaBuffer m_out;
};

}  // namespace biv::channel
