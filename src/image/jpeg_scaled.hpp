#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>

#include "processing/channel_ops.hpp"

namespace biv {

// Decodes a JPEG straight to thumbnail size (<= `max_dim` on the long edge).
//
// libjpeg's DCT-domain scaling decodes at 1/2, 1/4 or 1/8 size directly, so a
// 50 MP photo is never expanded to a 200 MB RGBA image only to be shrunk to
// 128 px: it costs a fraction of a full decode and needs a fraction of the
// memory. The file overload streams from disk instead of loading the whole file.
//
// Returns nullopt when the data isn't a JPEG this path can handle (corrupt,
// CMYK, absurd dimensions); the caller falls back to the general decoder.
// Requires libjpeg-turbo (BIV_ENABLE_JPEG).
[[nodiscard]] std::optional<colors::RgbaBuffer> decode_jpeg_scaled(const std::filesystem::path& path, int max_dim);
[[nodiscard]] std::optional<colors::RgbaBuffer> decode_jpeg_scaled(std::span<const std::byte> bytes, int max_dim);

}  // namespace biv
