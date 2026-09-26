#pragma once

#include <array>
#include <memory>
#include <optional>

#include "config.hpp"
#include "core/source.hpp"
#include "processing/channel_ops.hpp"

namespace biv {

// Owns one decoded image plus a small cache of its per-channel/analysis
// derivatives, so switching ChannelBar modes after the first computation is
// just a lookup, not a recompute.
class ImageDocument {
public:
    static std::optional<ImageDocument> load(ImageSource source);

    [[nodiscard]] const ImageSource& source() const noexcept { return m_source; }

    [[nodiscard]] int width() const noexcept { return m_original->width; }

    [[nodiscard]] int height() const noexcept { return m_original->height; }

    // Pixel-perfect interpolation for small images
    [[nodiscard]] bool should_use_nearest_neighbor() const;

    // Returns (and lazily computes + caches) the RGBA8 buffer for `mode`.
    const colors::RgbaBuffer& view(channel::Mode mode);

    // Same buffer, shared: lets a background job keep reading the pixels while
    // the UI moves on to another image. Buffers are immutable once created.
    [[nodiscard]] std::shared_ptr<const colors::RgbaBuffer> view_shared(channel::Mode mode);

private:
    explicit ImageDocument(ImageSource source, colors::RgbaBuffer original)
        : m_source(std::move(source)),
          m_original(std::make_shared<const colors::RgbaBuffer>(std::move(original))) {}

    ImageSource m_source;
    std::shared_ptr<const colors::RgbaBuffer> m_original;

    // One cache slot per Mode value (Mode is a small contiguous enum); null = not computed yet.
    std::array<std::shared_ptr<const colors::RgbaBuffer>, channel::kCHANNEL_MODE_COUNT> m_cache;
};

}  // namespace biv
