#include "jpeg_scaled.hpp"

#include <csetjmp>
#include <cstdio>
#include <cstdlib>

#include <jpeglib.h>

#include "image/thumbnail.hpp"
#include "util/file.hpp"
#include "util/image_limits.hpp"

// JCS_EXT_RGBA (decode straight to RGBA) is a libjpeg-turbo extension; it is an
// enum value, so libjpeg-turbo's JCS_ALPHA_EXTENSIONS macro is the feature test.
#ifndef JCS_ALPHA_EXTENSIONS
    #error "libjpeg-turbo is required for BIV_ENABLE_JPEG; configure with -DBIV_ENABLE_JPEG=OFF"
#endif

namespace biv {

namespace {

// libjpeg reports errors by calling error_exit(), which must not return. The
// documented way to recover is longjmp back to the caller.
struct JpegErrorManager {
    jpeg_error_mgr base;
    std::jmp_buf recover;
};

void on_jpeg_error(j_common_ptr cinfo) {
    std::longjmp(reinterpret_cast<JpegErrorManager*>(cinfo->err)->recover, 1);
}

// Corrupt-data warnings are expected for damaged files; don't spam stderr.
void on_jpeg_message(j_common_ptr, int) {}
void on_jpeg_output(j_common_ptr) {}

struct DecodedJpeg {
    std::uint8_t* pixels = nullptr;  // malloc'd RGBA8, caller frees
    int width            = 0;
    int height           = 0;
};

// Exactly one of `file` / `memory` is the source. Everything between setjmp()
// and the returns is plain C (no C++ objects with destructors), because a
// longjmp would skip them; the pixel buffer is malloc'd and freed on failure.
bool decode(std::FILE* file, const std::uint8_t* memory, std::size_t memory_size, int max_dim, DecodedJpeg* out) {
    // `cinfo` is volatile: it's mutated by every jpeg_*() call between
    // setjmp() and a possible longjmp() out of on_jpeg_error(), and
    // jpeg_destroy_decompress() in the recovery block below needs to read
    // back exactly those mutations (err, mem, output state, ...) to free
    // libjpeg's internal buffers correctly. A plain (non-volatile) automatic
    // object in that position has indeterminate value once control resumes
    // at setjmp() per the C/C++ standard; volatile is what rules that out.
    // libjpeg's own C API takes a plain jpeg_decompress_struct*, so raw()
    // strips the qualifier back off at each call site -- the same tradeoff
    // any volatile-qualified value makes when it's handed to code that
    // doesn't know about volatile.
    volatile jpeg_decompress_struct cinfo {};
    const auto raw = [&cinfo] { return const_cast<jpeg_decompress_struct*>(&cinfo); };
    JpegErrorManager err;
    std::uint8_t* volatile pixels = nullptr;

    raw()->err               = jpeg_std_error(&err.base);
    err.base.error_exit     = on_jpeg_error;
    err.base.emit_message   = on_jpeg_message;
    err.base.output_message = on_jpeg_output;
    if (setjmp(err.recover)) {
        jpeg_destroy_decompress(raw());
        std::free(pixels);
        return false;
    }

    jpeg_create_decompress(raw());
    if (file)
        jpeg_stdio_src(raw(), file);
    else
        jpeg_mem_src(raw(), memory, memory_size);
    jpeg_read_header(raw(), TRUE);

    // libjpeg can't convert CMYK/YCCK to RGB; leave those to stb_image.
    // The header dimensions are untrusted: validate before choosing sizes.
    if (cinfo.jpeg_color_space == JCS_CMYK || cinfo.jpeg_color_space == JCS_YCCK ||
        !rgba_byte_size(cinfo.image_width, cinfo.image_height)) {
        jpeg_destroy_decompress(raw());
        return false;
    }

    // Pick the largest reduction (1/1, 1/2, 1/4, 1/8) whose output is still at
    // least thumbnail-sized, so the final resize only ever shrinks.
    const auto target = channel::thumbnail_size(static_cast<int>(cinfo.image_width), static_cast<int>(cinfo.image_height), max_dim);
    unsigned denom = 1;
    while (denom < 8 && cinfo.image_width / (denom * 2) >= static_cast<unsigned>(target.width) &&
           cinfo.image_height / (denom * 2) >= static_cast<unsigned>(target.height))
        denom *= 2;

    raw()->scale_num           = 1;
    raw()->scale_denom         = denom;
    raw()->out_color_space     = JCS_EXT_RGBA;
    raw()->dct_method          = JDCT_IFAST;  // thumbnails: speed over the last bit of accuracy
    raw()->do_fancy_upsampling = FALSE;
    jpeg_start_decompress(raw());

    const auto bytes = rgba_byte_size(cinfo.output_width, cinfo.output_height);
    if (cinfo.output_components != kBYTES_PER_PIXEL || !bytes) {
        jpeg_destroy_decompress(raw());
        return false;
    }
    pixels = static_cast<std::uint8_t*>(std::malloc(*bytes));
    if (!pixels) {
        jpeg_destroy_decompress(raw());
        return false;
    }

    const std::size_t stride = static_cast<std::size_t>(cinfo.output_width) * kBYTES_PER_PIXEL;
    while (cinfo.output_scanline < cinfo.output_height) {
        JSAMPROW row = pixels + static_cast<std::size_t>(cinfo.output_scanline) * stride;
        jpeg_read_scanlines(raw(), &row, 1);
    }
    jpeg_finish_decompress(raw());

    out->pixels = pixels;
    out->width  = static_cast<int>(cinfo.output_width);
    out->height = static_cast<int>(cinfo.output_height);
    jpeg_destroy_decompress(raw());
    return true;
}

// Takes ownership of `decoded`: the reduced image is trimmed to the exact
// thumbnail size and the malloc'd buffer released.
colors::RgbaBuffer finish(DecodedJpeg decoded, int max_dim) {
    auto thumb = channel::make_thumbnail(
        decoded.pixels, decoded.width, decoded.height, static_cast<std::size_t>(decoded.width) * kBYTES_PER_PIXEL, max_dim);
    std::free(decoded.pixels);
    return thumb;
}

}  // namespace

std::optional<colors::RgbaBuffer> decode_jpeg_scaled(const std::filesystem::path& path, int max_dim) {
    std::FILE* file = fopen_path(path, "rb");
    if (!file)
        return std::nullopt;
    DecodedJpeg decoded;
    const bool ok = decode(file, nullptr, 0, max_dim, &decoded);
    std::fclose(file);
    if (!ok)
        return std::nullopt;
    return finish(decoded, max_dim);
}

std::optional<colors::RgbaBuffer> decode_jpeg_scaled(std::span<const std::byte> bytes, int max_dim) {
    DecodedJpeg decoded;
    if (!decode(nullptr, reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(), max_dim, &decoded))
        return std::nullopt;
    return finish(decoded, max_dim);
}

}  // namespace biv
