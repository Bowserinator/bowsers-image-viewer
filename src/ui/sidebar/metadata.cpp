#include "metadata.hpp"

#include "butil/str.hpp"
#include "butil/util.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <libexif/exif-data.h>
#include <system_error>
#include <vector>

#include "config.hpp"
#include "util/file.hpp"

namespace biv::sidebar {

namespace {

std::string format_label(const std::filesystem::path& path) {
    const auto ext = path.extension().string();
    if (ext.empty())
        return "Unknown";
    const auto upper = butil::upper(ext.substr(1));

    if (upper == "JPG" || upper == "JPEG")
        return "JPEG";
    if (upper == "TIF" || upper == "TIFF")
        return "TIFF";
    return upper;
}

// --- EXIF ------------------------------------------------------------------

std::string exif_value(ExifData* d, ExifTag tag) {
    if (!d)
        return {};
    ExifEntry* e = exif_data_get_entry(d, tag);
    if (!e)
        return {};
    char buf[1024] = {};
    exif_entry_get_value(e, buf, sizeof(buf));
    return std::string(buf);
}

// GPS coordinates are stored as three rationals (degrees/minutes/seconds).
std::optional<double> gps_component(ExifData* d, ExifTag tag) {
    if (!d)
        return std::nullopt;
    ExifEntry* e = exif_data_get_entry(d, tag);
    if (!e || e->format != EXIF_FORMAT_RATIONAL || e->size < 3 * sizeof(ExifRational))
        return std::nullopt;

    // exif_get_rational() decodes an 8-byte rational at an arbitrary (possibly
    // unaligned) byte offset and honours the file's actual byte order, unlike
    // a reinterpret_cast<const ExifRational*> onto raw bytes, which assumes
    // both natural alignment and native byte order -- neither guaranteed for
    // EXIF data written by an arbitrary camera.
    const ExifByteOrder order = exif_data_get_byte_order(d);
    const ExifRational deg_r  = exif_get_rational(e->data, order);
    const ExifRational min_r  = exif_get_rational(e->data + sizeof(ExifRational), order);
    const ExifRational sec_r  = exif_get_rational(e->data + 2 * sizeof(ExifRational), order);
    const double deg          = deg_r.denominator ? static_cast<double>(deg_r.numerator) / deg_r.denominator : 0.0;
    const double min          = min_r.denominator ? static_cast<double>(min_r.numerator) / min_r.denominator : 0.0;
    const double sec = sec_r.denominator ? static_cast<double>(sec_r.numerator) / sec_r.denominator : 0.0;
    return deg + min / 60.0 + sec / 3600.0;
}

// Returns "lat, lon" (W/S negative) or empty when either coordinate is absent.
std::string format_gps(ExifData* d) {
    const auto lat = gps_component(d, static_cast<ExifTag>(EXIF_TAG_GPS_LATITUDE));
    const auto lon = gps_component(d, static_cast<ExifTag>(EXIF_TAG_GPS_LONGITUDE));
    if (!lat && !lon)
        return {};

    auto lat_v         = lat.value_or(0.0);
    auto lon_v         = lon.value_or(0.0);
    const auto lat_ref = exif_value(d, static_cast<ExifTag>(EXIF_TAG_GPS_LATITUDE_REF));
    const auto lon_ref = exif_value(d, static_cast<ExifTag>(EXIF_TAG_GPS_LONGITUDE_REF));
    if (lat_ref.size() > 0 && (lat_ref[0] == 'S' || lat_ref[0] == 's'))
        lat_v = -lat_v;
    if (lon_ref.size() > 0 && (lon_ref[0] == 'W' || lon_ref[0] == 'w'))
        lon_v = -lon_v;

    char buf[64];
    if (lat && lon)
        std::snprintf(buf, sizeof(buf), "%.4f, %.4f", lat_v, lon_v);
    else if (lat)
        std::snprintf(buf, sizeof(buf), "%.4f", lat_v);
    else
        std::snprintf(buf, sizeof(buf), "%.4f", lon_v);
    return std::string(buf);
}

// --- Aspect ratio ------------------------------------------------------

struct CommonRatio {
    double value;       // width / height
    const char* label;  // "16:9", etc.
};

// Landscape entries plus their portrait reciprocals, ordered arbitrarily
// (the closest match wins regardless of order).
constexpr CommonRatio kCOMMON_RATIOS[] = {
    {1.0, "1:1"},
    {4.0 / 3.0, "4:3"},
    {3.0 / 4.0, "3:4"},
    {3.0 / 2.0, "3:2"},
    {2.0 / 3.0, "2:3"},
    {5.0 / 4.0, "5:4"},
    {4.0 / 5.0, "4:5"},
    {16.0 / 9.0, "16:9"},
    {9.0 / 16.0, "9:16"},
    {16.0 / 10.0, "16:10"},
    {10.0 / 16.0, "10:16"},
    {21.0 / 9.0, "21:9"},
    {9.0 / 21.0, "9:21"},
};

// Ratios within this relative tolerance of a common ratio are snapped to it.
constexpr double kASPECT_TOLERANCE = 0.006;

std::string join_with_space(const std::string& a, const std::string& b) {
    if (a.empty())
        return b;
    if (b.empty())
        return a;
    return a + " " + b;
}

// Reads EXIF tags from a file on disk. Empty strings for absent fields.
struct ExifTags {
    std::string camera;
    std::string date;
    std::string exposure;
    std::string aperture;
    std::string iso;
    std::string focal;
    std::string gps;
};

std::optional<ExifTags> read_exif(const std::filesystem::path& path) {
    if (path.empty())
        return std::nullopt;

    // exif_data_new_from_file() opens the path itself via libexif's own
    // narrow-path fopen, which is decoded with the ANSI code page on
    // Windows and can't open a path with a character outside it. Read the
    // file ourselves instead -- fopen_path() uses the path's native
    // representation, sidestepping that -- and hand libexif the bytes.
    std::FILE* f = fopen_path(path, "rb");
    if (!f)
        return std::nullopt;
    std::fseek(f, 0, SEEK_END);
    const auto size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        std::fclose(f);
        return std::nullopt;
    }
    std::vector<unsigned char> buf(static_cast<std::size_t>(size));
    const auto read = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    if (read != buf.size())
        return std::nullopt;

    ExifData* d = exif_data_new_from_data(buf.data(), static_cast<unsigned int>(buf.size()));
    if (!d)
        return std::nullopt;

    ExifTags out;
    out.camera   = join_with_space(exif_value(d, EXIF_TAG_MAKE), exif_value(d, EXIF_TAG_MODEL));
    out.date     = exif_value(d, EXIF_TAG_DATE_TIME_ORIGINAL);
    out.exposure = exif_value(d, EXIF_TAG_EXPOSURE_TIME);
    out.aperture = exif_value(d, EXIF_TAG_FNUMBER);
    out.iso      = exif_value(d, EXIF_TAG_ISO_SPEED_RATINGS);
    out.focal    = exif_value(d, EXIF_TAG_FOCAL_LENGTH);
    out.gps      = format_gps(d);

    exif_data_free(d);

    return out;
}

}  // namespace

FileMetadata collect_metadata(const ImageSource& src) {
    FileMetadata meta;

    std::optional<ExifTags> exif;
    std::visit(
        [&meta, &exif](const auto& s) {
            using T = std::decay_t<decltype(s)>;
            if constexpr (std::is_same_v<T, FileSource>) {
                meta.path   = s.path.string();
                meta.format = format_label(s.path);
                meta.size   = format_bytes(file_size_of(s.path));
                meta.source = "File";
                // Videos have no EXIF; don't let libexif scan a multi-GB file.
                if (!butil::contains(kVIDEO_EXTENSIONS, butil::lower(s.path.extension().string())))
                    exif = read_exif(s.path);
            } else {
                meta.path   = s.archive_path.string();
                meta.format = format_label(s.archive_path);
                meta.size   = format_bytes(file_size_of(s.archive_path));
                meta.source = "Archive";
                // EXIF for archive entries is skipped: reading it would require
                // re-extracting the entry on the UI thread.
            }
        },
        src);

    if (exif) {
        meta.camera   = exif->camera;
        meta.date     = exif->date;
        meta.exposure = exif->exposure;
        meta.aperture = exif->aperture;
        meta.iso      = exif->iso;
        meta.focal    = exif->focal;
        meta.gps      = exif->gps;
    }

    return meta;
}

std::string format_aspect_ratio(int width, int height) {
    if (width <= 0 || height <= 0)
        return {};

    const double ratio = static_cast<double>(width) / static_cast<double>(height);

    for (const auto& common : kCOMMON_RATIOS)
        if (std::abs(ratio - common.value) / common.value < kASPECT_TOLERANCE)
            return common.label;

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f : 1", ratio);
    return std::string(buf);
}

}  // namespace biv::sidebar