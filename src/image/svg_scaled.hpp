#pragma once

#include <cstddef>
#include <optional>
#include <span>

#include "util/color.hpp"

namespace biv {

[[nodiscard]] std::optional<colors::RgbaBuffer> decode_svg_scaled(std::span<const std::byte> bytes, int max_dim);

}  // namespace biv
