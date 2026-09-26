#pragma once

#include <cstddef>
#include <cstring>
#include <span>
#include <string_view>

namespace biv {

// Bytes of a file that sniff_image_format() needs to see.
inline constexpr std::size_t kIMAGE_SNIFF_BYTES = 512;

enum class ImageFormat {
    Unknown,       // anything else: left to stb_image
    Jpeg,
    Png,
    Webp,          // still (or unrecognised) WebP
    WebpAnimated,  // VP8X header with the animation flag set
    Avif,          // any ISO-BMFF "ftyp" file (avif/avis/mif1/...)
    Svg,
};

inline bool sniff_looks_like_svg(std::span<const std::byte> head) {
    std::string_view text(reinterpret_cast<const char*>(head.data()), head.size());
    if (text.substr(0, 3) == std::string_view("\xEF\xBB\xBF", 3))
        text.remove_prefix(3);
    for (;;) {
        while (!text.empty() &&
            (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n'))
            text.remove_prefix(1);
        if (text.size() >= 4 && text[0] == '<' && text[1] == 's' && text[2] == 'v' && text[3] == 'g')
            return true;
        if (text.substr(0, 2) == "<?") {
            const auto end = text.find("?>");
            if (end == std::string_view::npos)
                return false;
            text.remove_prefix(end + 2);
            continue;
        }
        if (text.substr(0, 4) == "<!--") {
            const auto end = text.find("-->");
            if (end == std::string_view::npos)
                return false;
            text.remove_prefix(end + 3);
            continue;
        }
        if (text.substr(0, 2) == "<!") {
            const auto end = text.find('>');
            if (end == std::string_view::npos)
                return false;
            text.remove_prefix(end + 1);
            continue;
        }
        return false;
    }
}

// Identifies a file from its first kIMAGE_SNIFF_BYTES bytes (fewer is fine).
// The single place format magic numbers live: decoders dispatch on it, and the
// video controller uses WebpAnimated to tell a real animation from a still.
inline ImageFormat sniff_image_format(std::span<const std::byte> head) {
    const auto at = [&](std::size_t offset, std::string_view tag) {
        return head.size() >= offset + tag.size() && std::memcmp(head.data() + offset, tag.data(), tag.size()) == 0;
    };
    if (at(0, std::string_view("\xFF\xD8\xFF", 3)))
        return ImageFormat::Jpeg;
    if (at(0, std::string_view("\x89PNG\r\n\x1a\n", 8)))
        return ImageFormat::Png;
    if (at(0, "RIFF") && at(8, "WEBP")) {
        // Extended-format header: "VP8X", chunk size, then a flags byte at
        // offset 20 where bit 1 (0x02) means the file is animated.
        if (at(12, "VP8X") && head.size() > 20 && (std::to_integer<unsigned>(head[20]) & 0x02u) != 0)
            return ImageFormat::WebpAnimated;
        return ImageFormat::Webp;
    }
    if (head.size() >= 12 && at(4, "ftyp"))
        return ImageFormat::Avif;
    if (sniff_looks_like_svg(head))
        return ImageFormat::Svg;
    return ImageFormat::Unknown;
}

}  // namespace biv
