#pragma once

#include <cstddef>
#include <optional>
#include <span>

#include "core/source.hpp"
#include "processing/channel_ops.hpp"

namespace biv {

// Decodes any ImageSource (plain file, or an entry pulled out of an archive)
// into an RGBA8 buffer. Returns std::nullopt on failure; check butil::log for
// the reason (decode.cpp logs via butil::log.error()).
std::optional<colors::RgbaBuffer> decode_image(const ImageSource& src);

// Decodes a thumbnail no larger than `max_dim` on the long edge, without
// building the full-size image where the format allows it: JPEG decodes at
// 1/2-1/8 scale straight from disk, WebP scales inside libwebp, videos are
// scaled by swscale, everything else is shrunk straight from the decoder's own
// buffer. Returns std::nullopt on failure; the caller shows a black thumbnail.
std::optional<colors::RgbaBuffer> decode_thumbnail(const ImageSource& src, int max_dim);

// Decodes an already-in-memory blob (used for archive entries once
// archive_source.hpp has extracted them). `max_dim` > 0 makes it a thumbnail
// decode as above; 0 means full size.
std::optional<colors::RgbaBuffer> decode_image_memory(std::span<const std::byte> bytes, int max_dim = 0);

}  // namespace biv
