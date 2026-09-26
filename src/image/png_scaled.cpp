#include "png_scaled.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

#include <png.h>

#include "image/box_downscaler.hpp"
#include "util/file.hpp"
#include "util/image_limits.hpp"

#ifndef PNG_SETJMP_SUPPORTED
    #error "libpng built without setjmp support is not usable; configure with -DBIV_ENABLE_PNG=OFF"
#endif

namespace biv {

namespace {

// stdio's default buffer (often 4 KiB) means a syscall per few KiB of a
// multi-megabyte IDAT stream.
constexpr std::size_t kFILE_BUFFER_BYTES = 256 * 1024;

struct MemoryReader {
    const std::uint8_t* data = nullptr;
    std::size_t size         = 0;
    std::size_t pos          = 0;
};

void read_from_memory(png_structp png, png_bytep out, png_size_t n) {
    auto* reader = static_cast<MemoryReader*>(png_get_io_ptr(png));
    if (n > reader->size - reader->pos)
        png_error(png, "unexpected end of data");
    std::memcpy(out, reader->data + reader->pos, n);
    reader->pos += n;
}

// libpng reports errors by calling this, which must not return. The
// documented way to recover is longjmp back to the setjmp in run().
void on_error(png_structp png, png_const_charp) {
    png_longjmp(png, 1);
}

// Warnings (bad ancillary chunks, sRGB profile quirks, ...) don't matter for a thumbnail.
void on_warning(png_structp, png_const_charp) {}

struct PngReader {
    png_structp png = nullptr;
    png_infop info  = nullptr;

    PngReader() {
        png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, on_error, on_warning);
        if (png)
            info = png_create_info_struct(png);
    }

    ~PngReader() { png_destroy_read_struct(&png, &info, nullptr); }

    PngReader(const PngReader&)            = delete;
    PngReader& operator=(const PngReader&) = delete;
};

// Exactly one of `file` / `mem` is the source. This is the one function that
// libpng may longjmp back into, so everything with a destructor (the reader,
// the scaler, the row buffer) is owned by the caller and passed in by
// reference: a longjmp never has to skip a destructor. Between setjmp() and
// any libpng call there are no C++ frames of ours, only libpng's own.
bool run(PngReader& reader,
    std::FILE* file,
    MemoryReader* mem,
    int max_dim,
    std::optional<channel::BoxDownscaler>& scaler,
    std::vector<png_byte>& row) {
    png_structp png = reader.png;
    png_infop info  = reader.info;
    if (!png || !info)
        return false;
    if (setjmp(png_jmpbuf(png)))
        return false;

    if (file)
        png_init_io(png, file);
    else
        png_set_read_fn(png, mem, read_from_memory);

    // A thumbnail doesn't need integrity checks; skipping the per-chunk CRC
    // also matches stb_image, which never verifies them.
    png_set_crc_action(png, PNG_CRC_QUIET_USE, PNG_CRC_QUIET_USE);
    png_set_user_limits(png, kMAX_IMAGE_DIMENSION, kMAX_IMAGE_DIMENSION);
    png_read_info(png, info);

    png_uint_32 width = 0, height = 0;
    int bit_depth = 0, color_type = 0, interlace = 0;
    png_get_IHDR(png, info, &width, &height, &bit_depth, &color_type, &interlace, nullptr, nullptr);

    // The header is untrusted: validate before choosing sizes. Adam7 needs the
    // whole image resident to deinterlace, so it's left to stb_image.
    if (interlace != PNG_INTERLACE_NONE || !rgba_byte_size(width, height))
        return false;

    // Ask libpng for plain 8-bit samples: palette and low-bit gray expanded,
    // tRNS turned into an alpha channel, 16-bit scaled down. Gray stays gray
    // and RGB stays RGB (BoxDownscaler takes 1-4 channels), which saves libpng
    // a gray->RGB and an add-alpha pass over every row. Gamma is ignored, as
    // stb_image does.
    if (color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
        png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(png);
    if (bit_depth == 16)
        png_set_scale_16(png);
    png_read_update_info(png, info);

    const int channels = png_get_channels(png, info);
    if (png_get_bit_depth(png, info) != 8 || channels < 1 || channels > 4 ||
        png_get_rowbytes(png, info) != static_cast<std::size_t>(width) * static_cast<std::size_t>(channels))
        return false;

    row.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(channels));
    scaler.emplace(static_cast<int>(width), static_cast<int>(height), max_dim);
    for (png_uint_32 y = 0; y < height; ++y) {
        png_read_row(png, row.data(), nullptr);
        scaler->add_row(row.data(), channels);
    }
    return true;  // trailing chunks (png_read_end) are of no interest
}

std::optional<colors::RgbaBuffer> decode(std::FILE* file, MemoryReader* mem, int max_dim) {
    PngReader reader;
    std::optional<channel::BoxDownscaler> scaler;
    std::vector<png_byte> row;
    if (!run(reader, file, mem, max_dim, scaler, row) || !scaler || !scaler->complete())
        return std::nullopt;
    return scaler->finish();
}

}  // namespace

std::optional<colors::RgbaBuffer> decode_png_scaled(const std::filesystem::path& path, int max_dim) {
    std::FILE* file = fopen_path(path, "rb");
    if (!file)
        return std::nullopt;
    std::vector<char> buffer(kFILE_BUFFER_BYTES);  // must outlive the FILE
    std::setvbuf(file, buffer.data(), _IOFBF, buffer.size());
    auto result = decode(file, nullptr, max_dim);
    std::fclose(file);
    return result;
}

std::optional<colors::RgbaBuffer> decode_png_scaled(std::span<const std::byte> bytes, int max_dim) {
    MemoryReader mem{reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(), 0};
    return decode(nullptr, &mem, max_dim);
}

}  // namespace biv
