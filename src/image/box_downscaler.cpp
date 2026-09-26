#include "box_downscaler.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#include "image/thumbnail.hpp"

namespace biv::channel {

namespace {

constexpr float kINV_255 = 1.0f / 255.0f;

// 8-bit sRGB -> linear light.
const float* srgb_to_linear_lut() {
    static const std::array<float, 256> lut = [] {
        std::array<float, 256> t{};
        for (std::size_t i = 0; i < t.size(); ++i)
            t[i] = static_cast<float>(colors::srgb_to_linear(static_cast<double>(i) / kU8_MAX));
        return t;
    }();
    return lut.data();
}

// One source pixel as {R, G, B} premultiplied by alpha, in linear light, plus
// alpha in [0, 1]. C is the sample count of the source row.
template <int C>
inline void load_pixel(const std::uint8_t* p, const float* lut, float out[4]) {
    if constexpr (C == 1) {
        const float v = lut[p[0]];
        out[0] = out[1] = out[2] = v;
        out[3]                   = 1.0f;
    } else if constexpr (C == 2) {
        const float a = static_cast<float>(p[1]) * kINV_255;
        const float v = lut[p[0]] * a;
        out[0] = out[1] = out[2] = v;
        out[3]                   = a;
    } else if constexpr (C == 3) {
        out[0] = lut[p[0]];
        out[1] = lut[p[1]];
        out[2] = lut[p[2]];
        out[3] = 1.0f;
    } else {
        const float a = static_cast<float>(p[3]) * kINV_255;
        out[0]        = lut[p[0]] * a;
        out[1]        = lut[p[1]] * a;
        out[2]        = lut[p[2]] * a;
        out[3]        = a;
    }
}

inline std::uint8_t linear_to_srgb8(float linear) {
    return static_cast<std::uint8_t>(std::lround(colors::linear_to_srgb(static_cast<double>(linear)) * kU8_MAX));
}

}  // namespace

BoxDownscaler::BoxDownscaler(int src_width, int src_height, int max_dim)
    : m_sw(std::max(src_width, 1)), m_sh(std::max(src_height, 1)) {
    const auto size = thumbnail_size(m_sw, m_sh, max_dim);
    m_tw            = size.width;
    m_th            = size.height;
    m_identity      = (m_tw == m_sw && m_th == m_sh);

    m_out.width  = m_tw;
    m_out.height = m_th;
    m_out.pixels.assign(static_cast<std::size_t>(m_tw) * static_cast<std::size_t>(m_th) * kBYTES_PER_PIXEL, 0);
    if (m_identity)
        return;

    // Horizontal spans. Work in units where source pixel x covers
    // [x*tw, (x+1)*tw) and output pixel o covers [o*sw, (o+1)*sw): both axes
    // then have integer edges and the weights below are exact overlaps / sw.
    const std::int64_t sw = m_sw;
    const std::int64_t tw = m_tw;
    m_cols.resize(static_cast<std::size_t>(m_tw));
    for (int o = 0; o < m_tw; ++o) {
        const std::int64_t lo = o * sw;
        const std::int64_t hi = lo + sw;
        Span& s               = m_cols[static_cast<std::size_t>(o)];
        s.first               = static_cast<int>(lo / tw);
        s.last                = static_cast<int>((hi - 1) / tw);  // pixel holding the last covered sample
        if (s.first == s.last) {
            s.w_first = 1.0f;
            continue;
        }
        s.w_first = static_cast<float>(static_cast<double>((s.first + 1) * tw - lo) / static_cast<double>(sw));
        s.w_mid   = static_cast<float>(static_cast<double>(tw) / static_cast<double>(sw));
        s.w_last  = static_cast<float>(static_cast<double>(hi - static_cast<std::int64_t>(s.last) * tw) / static_cast<double>(sw));
    }

    const std::size_t acc_size = static_cast<std::size_t>(m_tw) * kBYTES_PER_PIXEL;
    m_hrow.assign(acc_size, 0.0f);
    m_cur.assign(acc_size, 0.0f);
    m_next.assign(acc_size, 0.0f);
}

template <int C>
void BoxDownscaler::reduce_row(const std::uint8_t* row) {
    const float* lut = srgb_to_linear_lut();
    float* out       = m_hrow.data();

    for (int o = 0; o < m_tw; ++o, out += kBYTES_PER_PIXEL) {
        const Span& s = m_cols[static_cast<std::size_t>(o)];

        float first[4];
        load_pixel<C>(row + static_cast<std::size_t>(s.first) * C, lut, first);
        if (s.first == s.last) {
            std::copy_n(first, 4, out);
            continue;
        }

        float sum[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        const std::uint8_t* p = row + static_cast<std::size_t>(s.first + 1) * C;
        for (int x = s.first + 1; x < s.last; ++x, p += C) {
            float v[4];
            load_pixel<C>(p, lut, v);
            sum[0] += v[0];
            sum[1] += v[1];
            sum[2] += v[2];
            sum[3] += v[3];
        }

        float last[4];
        load_pixel<C>(row + static_cast<std::size_t>(s.last) * C, lut, last);
        for (int k = 0; k < 4; ++k)
            out[k] = s.w_first * first[k] + s.w_mid * sum[k] + s.w_last * last[k];
    }
}

void BoxDownscaler::copy_row(const std::uint8_t* row, int channels) {
    std::uint8_t* dst = m_out.pixels.data() + static_cast<std::size_t>(m_rows_seen) * static_cast<std::size_t>(m_tw) * kBYTES_PER_PIXEL;
    for (int x = 0; x < m_tw; ++x, row += channels, dst += kBYTES_PER_PIXEL) {
        switch (channels) {
            case 1: dst[0] = dst[1] = dst[2] = row[0]; dst[3] = 255; break;
            case 2: dst[0] = dst[1] = dst[2] = row[0]; dst[3] = row[1]; break;
            case 3: dst[0] = row[0]; dst[1] = row[1]; dst[2] = row[2]; dst[3] = 255; break;
            default: dst[0] = row[0]; dst[1] = row[1]; dst[2] = row[2]; dst[3] = row[3]; break;
        }
    }
}

void BoxDownscaler::emit_row(int out_row, const float* acc) {
    if (out_row < 0 || out_row >= m_th)
        return;
    std::uint8_t* dst = m_out.pixels.data() + static_cast<std::size_t>(out_row) * static_cast<std::size_t>(m_tw) * kBYTES_PER_PIXEL;
    for (int x = 0; x < m_tw; ++x, acc += kBYTES_PER_PIXEL, dst += kBYTES_PER_PIXEL) {
        const float a = acc[3];
        if (a <= 1e-6f) {  // fully transparent: colour is meaningless
            dst[0] = dst[1] = dst[2] = dst[3] = 0;
            continue;
        }
        const float inv = 1.0f / a;  // un-premultiply
        dst[0]          = linear_to_srgb8(acc[0] * inv);
        dst[1]          = linear_to_srgb8(acc[1] * inv);
        dst[2]          = linear_to_srgb8(acc[2] * inv);
        dst[3]          = static_cast<std::uint8_t>(std::lround(std::clamp(a, 0.0f, 1.0f) * kU8_MAX_F));
    }
}

void BoxDownscaler::accumulate_vertical() {
    // Source row y covers [y*th, (y+1)*th) and output row o covers
    // [o*sh, (o+1)*sh) in shared integer units; rows arrive in order so the
    // current output row is always lo / sh.
    const std::int64_t sh       = m_sh;
    const std::int64_t lo       = static_cast<std::int64_t>(m_rows_seen) * m_th;
    const std::int64_t hi       = lo + m_th;
    const std::int64_t boundary = (static_cast<std::int64_t>(m_out_row) + 1) * sh;

    float w0 = static_cast<float>(static_cast<double>(m_th) / static_cast<double>(sh));
    float w1 = 0.0f;
    if (hi > boundary) {  // the row straddles into the next output row
        w0 = static_cast<float>(static_cast<double>(boundary - lo) / static_cast<double>(sh));
        w1 = static_cast<float>(static_cast<double>(hi - boundary) / static_cast<double>(sh));
    }

    const std::size_t n = m_hrow.size();
    for (std::size_t i = 0; i < n; ++i)
        m_cur[i] += w0 * m_hrow[i];
    if (w1 > 0.0f)
        for (std::size_t i = 0; i < n; ++i)
            m_next[i] += w1 * m_hrow[i];

    if (hi >= boundary) {  // this output row has received all of its source rows
        emit_row(m_out_row, m_cur.data());
        std::swap(m_cur, m_next);
        std::fill(m_next.begin(), m_next.end(), 0.0f);
        ++m_out_row;
    }
}

void BoxDownscaler::add_row(const std::uint8_t* row, int channels) {
    if (!row || channels < 1 || channels > 4 || m_rows_seen >= m_sh)
        return;

    if (m_identity) {
        copy_row(row, channels);
        ++m_rows_seen;
        return;
    }

    switch (channels) {
        case 1: reduce_row<1>(row); break;
        case 2: reduce_row<2>(row); break;
        case 3: reduce_row<3>(row); break;
        default: reduce_row<4>(row); break;
    }
    accumulate_vertical();
    ++m_rows_seen;
}

colors::RgbaBuffer BoxDownscaler::finish() {
    if (!complete())
        return make_black_thumbnail();
    return std::move(m_out);
}

}  // namespace biv::channel
