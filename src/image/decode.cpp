#include "decode.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

#include "config.hpp"

// stb rejects anything wider/taller than this while parsing the header,
// before it allocates.
#define STBI_MAX_DIMENSIONS biv::kMAX_IMAGE_DIMENSION
#define STB_IMAGE_IMPLEMENTATION
#define STBI_FAILURE_USERMSG
#include <stb_image.h>

#ifdef BIV_ENABLE_AVIF
    #include <avif/avif.h>
#endif

#ifdef BIV_ENABLE_WEBP
    #include <webp/decode.h>
    #include <webp/demux.h>
#endif

#include "butil/log.hpp"
#include "butil/str.hpp"
#include "butil/util.hpp"

#include "filesystem/archive_source.hpp"
#include "image/thumbnail.hpp"
#include "util/file.hpp"
#include "util/image_format.hpp"
#include "util/image_limits.hpp"

#ifdef BIV_ENABLE_JPEG
    #include "image/jpeg_scaled.hpp"
#endif

#ifdef BIV_ENABLE_PNG
    #include "image/png_scaled.hpp"
#endif

#ifdef BIV_ENABLE_SVG
    #include "image/svg_scaled.hpp"
#endif

#ifdef BIV_ENABLE_VIDEO
    #include "video/video_player.hpp"
#endif

namespace biv {

namespace {

// Hands a decoder's RGBA8 pixels (rows `stride` bytes apart) to the caller:
// shrunk to `max_dim` straight from the decoder's own buffer when thumbnailing
// (no full-size copy), or copied tightly packed for a full decode. The caller
// has already validated width/height with rgba_byte_size().
colors::RgbaBuffer to_rgba(const std::uint8_t* pixels, int width, int height, std::size_t stride, int max_dim) {
    if (max_dim > 0)
        return channel::make_thumbnail(pixels, width, height, stride, max_dim);

    const std::size_t row = static_cast<std::size_t>(width) * kBYTES_PER_PIXEL;
    colors::RgbaBuffer buf;
    buf.width  = width;
    buf.height = height;
    buf.pixels.resize(row * static_cast<std::size_t>(height));
    for (int y = 0; y < height; ++y)
        std::memcpy(buf.pixels.data() + static_cast<std::size_t>(y) * row, pixels + static_cast<std::size_t>(y) * stride, row);
    return buf;
}

#ifdef BIV_ENABLE_AVIF
struct AvifDecoderDeleter {
    void operator()(avifDecoder* d) const { avifDecoderDestroy(d); }
};

// avifRGBImageFreePixels() is a no-op if the pixels were never allocated.
struct AvifRgbImage {
    avifRGBImage image{};
    ~AvifRgbImage() { avifRGBImageFreePixels(&image); }
};

std::optional<colors::RgbaBuffer> decode_avif(std::span<const std::byte> bytes, int max_dim) {
    std::unique_ptr<avifDecoder, AvifDecoderDeleter> decoder(avifDecoderCreate());
    if (!decoder) {
        butil::log.error("Failed to create AVIF decoder");
        return std::nullopt;
    }
    decoder->imageSizeLimit = static_cast<std::uint32_t>(kMAX_IMAGE_PIXELS);

    avifResult result =
        avifDecoderSetIOMemory(decoder.get(), reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
    if (result != AVIF_RESULT_OK) {
        butil::log.error("avifDecoderSetIOBuffer failed: {}", avifResultToString(result));
        return std::nullopt;
    }

    result = avifDecoderParse(decoder.get());
    if (result != AVIF_RESULT_OK) {
        butil::log.error("avifDecoderParse failed: {}", avifResultToString(result));
        return std::nullopt;
    }
    // Reject absurd headers before spending time decoding them.
    if (!rgba_byte_size(decoder->image->width, decoder->image->height)) {
        butil::log.error("AVIF has unsupported dimensions {}x{}", decoder->image->width, decoder->image->height);
        return std::nullopt;
    }

    result = avifDecoderNextImage(decoder.get());
    if (result != AVIF_RESULT_OK) {
        butil::log.error("avifDecoderNextImage failed: {}", avifResultToString(result));
        return std::nullopt;
    }

    AvifRgbImage rgb;
    avifRGBImageSetDefaults(&rgb.image, decoder->image);
    rgb.image.format = AVIF_RGB_FORMAT_RGBA;
    rgb.image.depth  = 8;
    if (!rgba_byte_size(rgb.image.width, rgb.image.height)) {
        butil::log.error("AVIF has unsupported dimensions {}x{}", rgb.image.width, rgb.image.height);
        return std::nullopt;
    }

    result = avifRGBImageAllocatePixels(&rgb.image);
    if (result != AVIF_RESULT_OK) {
        butil::log.error("avifRGBImageAllocatePixels failed: {}", avifResultToString(result));
        return std::nullopt;
    }
    result = avifImageYUVToRGB(decoder->image, &rgb.image);
    if (result != AVIF_RESULT_OK) {
        butil::log.error("avifImageYUVToRGB failed: {}", avifResultToString(result));
        return std::nullopt;
    }

    return to_rgba(rgb.image.pixels,
        static_cast<int>(rgb.image.width),
        static_cast<int>(rgb.image.height),
        rgb.image.rowBytes,
        max_dim);
}
#endif

#ifdef BIV_ENABLE_WEBP
// Decodes just the first frame of an animated WebP via the dedicated
// animation decoder (used as the still poster; video_controller separately
// gives FFmpeg a shot at full playback, mirroring how animated GIFs are handled).
std::optional<colors::RgbaBuffer> decode_webp_first_frame(
    std::span<const std::byte> bytes, const WebPBitstreamFeatures& features, int max_dim) {
    WebPData webp_data{reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()};

    WebPAnimDecoderOptions dec_options;
    WebPAnimDecoderOptionsInit(&dec_options);
    dec_options.color_mode = MODE_RGBA;

    std::unique_ptr<WebPAnimDecoder, void (*)(WebPAnimDecoder*)> dec(
        WebPAnimDecoderNew(&webp_data, &dec_options), &WebPAnimDecoderDelete);  // also frees the frame below
    WebPAnimInfo info;
    if (!dec || !WebPAnimDecoderGetInfo(dec.get(), &info)) {
        butil::log.error("WebPAnimDecoderNew failed");
        return std::nullopt;
    }
    // The canvas size is untrusted and is what sizes the frame buffer libwebp
    // hands back: it must be sane and agree with the container header.
    if (!rgba_byte_size(info.canvas_width, info.canvas_height) || static_cast<int>(info.canvas_width) != features.width ||
        static_cast<int>(info.canvas_height) != features.height) {
        butil::log.error("Animated WebP has inconsistent dimensions");
        return std::nullopt;
    }

    uint8_t* frame = nullptr;
    int timestamp  = 0;
    if (!WebPAnimDecoderHasMoreFrames(dec.get()) || !WebPAnimDecoderGetNext(dec.get(), &frame, &timestamp) || !frame) {
        butil::log.error("WebPAnimDecoderGetNext failed");
        return std::nullopt;
    }
    return to_rgba(frame,
        static_cast<int>(info.canvas_width),
        static_cast<int>(info.canvas_height),
        static_cast<std::size_t>(info.canvas_width) * kBYTES_PER_PIXEL,
        max_dim);
}

std::optional<colors::RgbaBuffer> decode_webp(std::span<const std::byte> bytes, int max_dim) {
    const auto* raw = reinterpret_cast<const uint8_t*>(bytes.data());

    // WebPGetFeatures parses and validates the container and reports animation;
    // WebPDecode only understands a single still frame.
    WebPBitstreamFeatures features;
    if (WebPGetFeatures(raw, bytes.size(), &features) != VP8_STATUS_OK) {
        butil::log.error("WebPGetFeatures failed");
        return std::nullopt;
    }
    if (!rgba_byte_size(features.width, features.height)) {
        butil::log.error("WebP has unsupported dimensions {}x{}", features.width, features.height);
        return std::nullopt;
    }
    if (features.has_animation)
        return decode_webp_first_frame(bytes, features, max_dim);

    // Decode straight into our own buffer at the final size. libwebp checks
    // that buffer against the bitstream, and when thumbnailing it scales row by
    // row inside the decoder, so a full-size RGBA image is never built.
    const auto size = max_dim > 0 ? channel::thumbnail_size(features.width, features.height, max_dim)
                                  : channel::ThumbSize{features.width, features.height};
    colors::RgbaBuffer buf;
    buf.width  = size.width;
    buf.height = size.height;
    buf.pixels.resize(*rgba_byte_size(size.width, size.height));

    WebPDecoderConfig config;
    if (!WebPInitDecoderConfig(&config)) {
        butil::log.error("WebPInitDecoderConfig failed");
        return std::nullopt;
    }
    config.options.use_threads = 1;
    if (size.width != features.width || size.height != features.height) {
        config.options.use_scaling    = 1;
        config.options.scaled_width   = size.width;
        config.options.scaled_height  = size.height;
    }
    config.output.colorspace         = MODE_RGBA;
    config.output.is_external_memory = 1;
    config.output.u.RGBA.rgba        = buf.pixels.data();
    config.output.u.RGBA.stride      = size.width * kBYTES_PER_PIXEL;
    config.output.u.RGBA.size        = buf.pixels.size();
    if (WebPDecode(raw, bytes.size(), &config) != VP8_STATUS_OK) {
        butil::log.error("WebPDecode failed");
        return std::nullopt;
    }
    return buf;
}
#endif

std::optional<colors::RgbaBuffer> decode_stb(std::span<const std::byte> bytes, int max_dim) {
    const auto* data = reinterpret_cast<const stbi_uc*>(bytes.data());
    const int len    = static_cast<int>(bytes.size());  // bounded by kMAX_IMAGE_FILE_BYTES

    // Read the header first: it's cheap, and lets us refuse an absurd size
    // before stb tries to allocate for it.
    int w = 0, h = 0, channels_in_file = 0;
    if (!stbi_info_from_memory(data, len, &w, &h, &channels_in_file)) {
        butil::log.error("stbi_info_from_memory failed: {}", stbi_failure_reason());
        return std::nullopt;
    }
    if (!rgba_byte_size(w, h)) {
        butil::log.error("Image has unsupported dimensions {}x{}", w, h);
        return std::nullopt;
    }

    stbi_uc* pixels = stbi_load_from_memory(data, len, &w, &h, &channels_in_file, /*desired_channels=*/4);
    if (!pixels) {
        butil::log.error("stbi_load_from_memory failed: {}", stbi_failure_reason());
        return std::nullopt;
    }
    std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> owner(pixels, &stbi_image_free);
    if (!rgba_byte_size(w, h))  // whatever the decoder actually produced
        return std::nullopt;
    return to_rgba(pixels, w, h, static_cast<std::size_t>(w) * kBYTES_PER_PIXEL, max_dim);
}

// max_dim > 0: a thumbnail no larger than max_dim, decoded as cheaply as the
// format allows. max_dim == 0: the full-size image.
std::optional<colors::RgbaBuffer> decode_source(const ImageSource& src, int max_dim) {
    return std::visit(
        [max_dim](const auto& s) -> std::optional<colors::RgbaBuffer> {
            using T = std::decay_t<decltype(s)>;
            if constexpr (std::is_same_v<T, FileSource>) {
                [[maybe_unused]] const std::string ext = butil::lower(path_to_utf8(s.path.extension()));
#ifdef BIV_ENABLE_VIDEO
                // Videos: a representative still frame (thumbnails, and the image
                // shown until playback starts). Playback itself is video/video_player.
                if (butil::contains(kVIDEO_EXTENSIONS, ext))
                    return video::decode_poster_frame(s.path, max_dim);
#endif
#ifdef BIV_ENABLE_JPEG
                // Streams from disk and decodes at reduced size. If this can't
                // handle the file (misnamed, CMYK, corrupt) fall through to the
                // general path below.
                if (max_dim > 0 && (ext == ".jpg" || ext == ".jpeg"))
                    if (auto thumb = decode_jpeg_scaled(s.path, max_dim))
                        return thumb;
#endif
#ifdef BIV_ENABLE_PNG
                // Same idea for PNG: streams from disk and folds each decoded
                // row straight into the thumbnail, so the full image never
                // exists. Interlaced or odd files fall through to stb_image.
                if (max_dim > 0 && ext == ".png")
                    if (auto thumb = decode_png_scaled(s.path, max_dim))
                        return thumb;
#endif
                const auto bytes = read_file_bytes(s.path, kMAX_IMAGE_FILE_BYTES);
                if (!bytes || bytes->empty()) {
                    butil::log.error("Failed to read '{}'", path_to_utf8(s.path));
                    return std::nullopt;
                }
                return decode_image_memory(*bytes, max_dim);
            } else {
#ifdef BIV_ENABLE_ARCHIVES
                auto blob = archive::read_entry(s.archive_path, s.entry_name);
                if (!blob) {
                    butil::log.error("Failed to extract '{}' from '{}'", s.entry_name, path_to_utf8(s.archive_path));
                    return std::nullopt;
                }
                return decode_image_memory(*blob, max_dim);
#else
                butil::log.error("Archive support disabled at build time (BIV_ENABLE_ARCHIVES=OFF)");
                return std::nullopt;
#endif
            }
        },
        src);
}

}  // namespace

std::optional<colors::RgbaBuffer> decode_image_memory(std::span<const std::byte> bytes, int max_dim) {
    switch (sniff_image_format(bytes.first(std::min(bytes.size(), kIMAGE_SNIFF_BYTES)))) {
#ifdef BIV_ENABLE_AVIF
        case ImageFormat::Avif: return decode_avif(bytes, max_dim);
#endif
#ifdef BIV_ENABLE_WEBP
        case ImageFormat::Webp:
        case ImageFormat::WebpAnimated: return decode_webp(bytes, max_dim);
#endif
#ifdef BIV_ENABLE_JPEG
        case ImageFormat::Jpeg:
            if (max_dim > 0)
                if (auto thumb = decode_jpeg_scaled(bytes, max_dim))
                    return thumb;
            break;
#endif
#ifdef BIV_ENABLE_PNG
        case ImageFormat::Png:
            if (max_dim > 0)
                if (auto thumb = decode_png_scaled(bytes, max_dim))
                    return thumb;
            break;
#endif
#ifdef BIV_ENABLE_SVG
        case ImageFormat::Svg: return decode_svg_scaled(bytes, max_dim);
#endif
        default: break;
    }
    return decode_stb(bytes, max_dim);
}

std::optional<colors::RgbaBuffer> decode_image(const ImageSource& src) {
    return decode_source(src, /*max_dim=*/0);
}

std::optional<colors::RgbaBuffer> decode_thumbnail(const ImageSource& src, int max_dim) {
    return decode_source(src, std::max(max_dim, 1));
}

}  // namespace biv
