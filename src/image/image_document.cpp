#include "image_document.hpp"

#include <filesystem>

#include "butil/log.hpp"
#include "butil/str.hpp"

#include "decode.hpp"
#include "util/file.hpp"

namespace biv {

namespace {

bool is_svg_source(const ImageSource& source) {
    const std::string ext = std::visit(
        [](const auto& s) -> std::string {
            using T = std::decay_t<decltype(s)>;
            if constexpr (std::is_same_v<T, FileSource>)
                return path_to_utf8(s.path.extension());
            else
                return std::filesystem::path(s.entry_name).extension().string();
        },
        source);
    return butil::lower(ext) == ".svg";
}

}  // namespace

bool ImageDocument::should_use_nearest_neighbor() const {
    if (is_svg_source(m_source))
        return false;
    return width() < kPIXEL_PERFECT_THRESHOLD || height() < kPIXEL_PERFECT_THRESHOLD;
}

std::optional<ImageDocument> ImageDocument::load(ImageSource source) {
    auto decoded = decode_image(source);
    if (!decoded) {
        butil::log.error("ImageDocument::load failed for '{}'", display_name(source));
        return std::nullopt;
    }
    return ImageDocument(std::move(source), std::move(*decoded));
}

const colors::RgbaBuffer& ImageDocument::view(channel::Mode mode) {
    // The document keeps the buffer alive (original or cache slot), so the
    // reference stays valid for as long as the document does.
    return *view_shared(mode);
}

std::shared_ptr<const colors::RgbaBuffer> ImageDocument::view_shared(channel::Mode mode) {
    if (mode == channel::Mode::RGB)
        return m_original;

    if (!m_spare || m_spare_mode != mode) {
        m_spare_mode = mode;
        m_spare      = std::make_shared<const colors::RgbaBuffer>(channel::apply(*m_original, mode));
    }
    return m_spare;
}

}  // namespace biv
