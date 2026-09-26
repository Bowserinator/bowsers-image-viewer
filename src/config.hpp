#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace biv {

// ---- App metadata ----
inline constexpr std::string_view kAPP_NAME    = "Bowser's Image Viewer";
inline constexpr std::string_view kAPP_ID      = "bowsers_image_viewer";
inline constexpr std::string_view kAPP_VERSION = "1.0.0";

// ---- Image extensions (single source of truth) ----
// Both directory_model.cpp and archive_source.cpp must use this list.
inline constexpr std::array<std::string_view, 13> kIMAGE_EXTENSIONS{
    ".png",
    ".jpg",
    ".jpeg",
    ".bmp",
    ".gif",
    ".tga",
    ".psd",
    ".hdr",
    ".avif",
    ".heic",
    ".webp",
    ".qoi",
    ".svg",
};

// ---- Video extensions ----
// Played through FFmpeg (video/video_player.hpp). ".gif" and ".webp" stay in
// kIMAGE_EXTENSIONS: they load as a still image and switch to playback when
// the file turns out to be animated.
inline constexpr std::array<std::string_view, 14> kVIDEO_EXTENSIONS{
    ".mp4",
    ".mkv",
    ".webm",
    ".mov",
    ".avi",
    ".m4v",
    ".wmv",
    ".flv",
    ".mpg",
    ".mpeg",
    ".ogv",
    ".3gp",
    ".m2ts",
    ".mts",
};

namespace detail {
// Compile-time std::array concatenation (butil::vec_concat is vector-only).
template <typename T, std::size_t N, std::size_t M>
constexpr std::array<T, N + M> concat_arrays(const std::array<T, N>& a, const std::array<T, M>& b) {
    std::array<T, N + M> out{};
    for (std::size_t i = 0; i < N; ++i)
        out[i] = a[i];
    for (std::size_t i = 0; i < M; ++i)
        out[N + i] = b[i];
    return out;
}
}  // namespace detail

// Every extension this build can list and open: images, plus videos when built
// with BIV_ENABLE_VIDEO. Compare lowercase, dot-included: butil::contains(kALL_EXTENSIONS, ".mkv").
#ifdef BIV_ENABLE_VIDEO
inline constexpr auto kALL_EXTENSIONS = detail::concat_arrays(kIMAGE_EXTENSIONS, kVIDEO_EXTENSIONS);
#else
inline constexpr auto kALL_EXTENSIONS = kIMAGE_EXTENSIONS;
#endif

// Extensions whose format can contain an alpha channel.
inline constexpr std::array<std::string_view, 8> kTRANSPARENT_EXTENSIONS{
    ".png",
    ".gif",
    ".tga",
    ".psd",
    ".avif",
    ".heic",
    ".webp",
    ".svg",
};

// ---- RGBA pixel constants ----
inline constexpr int kBYTES_PER_PIXEL          = 4;
inline constexpr float kU8_MAX_F               = 255.0f;
inline constexpr float kU8_MIN_F               = 0.0f;
inline constexpr double kU8_MAX                = 255.0;
inline constexpr double kU8_MIN                = 0.0;
inline constexpr float kMID_GRAY               = 127.5f;
inline constexpr std::size_t kHEATMAP_LUT_SIZE = 256;

// ---- Luminance weights (Rec. 709) ----
inline constexpr float kLUMINANCE_R = 0.2126f;
inline constexpr float kLUMINANCE_G = 0.7152f;
inline constexpr float kLUMINANCE_B = 0.0722f;
// Integer approximation: 0.2126*256=54, 0.7152*256=183, 0.0722*256=19
inline constexpr std::uint32_t kLUMA_INT_R     = 54u;
inline constexpr std::uint32_t kLUMA_INT_G     = 183u;
inline constexpr std::uint32_t kLUMA_INT_B     = 19u;
inline constexpr std::uint32_t kLUMA_INT_ROUND = 128u;

// ---- HSV thresholds ----
inline constexpr double kCHROMA_EPSILON       = 1e-9;
inline constexpr float kUNIFORM_RANGE_EPSILON = 1e-6f;
inline constexpr float kSAT_HUE_SATURATION    = 0.5f;

// ---- Gaussian blur ----
inline constexpr double kSOBEL_BLUR_SIGMA   = 1.0;
inline constexpr double kDOG_FINE_SIGMA     = 0.5;
inline constexpr double kDOG_COARSE_SIGMA   = 3.0;
inline constexpr double kBLUR_RADIUS_FACTOR = 2.0;
// Blur / High-pass filters: sigma chosen so the kernel radius (ceil(sigma *
// kBLUR_RADIUS_FACTOR)) works out to 5px, per the "5px gaussian blur" spec.
inline constexpr double kBLUR_FILTER_SIGMA = 2.5;

// ---- Parallelism ----
inline constexpr std::size_t kPARALLEL_THRESHOLD = std::size_t{1} << 18;
// Number of worker threads parallel_for() splits work across; overridable at
// startup with -j/--jobs (see main.cpp / util/color.hpp::set_thread_count()).
inline constexpr std::size_t kDEFAULT_PARALLEL_THREADS = 8;

// ---- Image analysis ----
inline constexpr std::size_t kSUPERPIXEL_COUNT   = 64;
// Fixed iteration count for the SLIC superpixel clustering loop (assign +
// recenter each iteration); SLIC typically converges well before this.
inline constexpr int kSLIC_ITERATIONS = 10;

// ---- Thumbnails ----
inline constexpr int kDEFAULT_THUMBNAIL_MAX_DIM = 128;
inline constexpr std::size_t kTHUMB_CACHE_CAP   = 512;
// Upper bound on background thumbnail decoder threads (ui/image_loader.cpp
// uses half the hardware threads, at least 2). One slow file, such as a huge
// PNG, must not hold up every thumbnail queued behind it.
inline constexpr std::size_t kMAX_THUMB_WORKERS = 4;
// When a decoded image is at least this many times larger than its thumbnail
// on both axes, image/thumbnail.cpp shrinks it with one streaming area-average
// pass instead of a general resample (much cheaper, same result at that ratio).
inline constexpr int kBOX_MIN_REDUCTION = 4;
// Thumbstrip spacing and default entry width are defined in ui/theme.slint
// (Metrics.spacing-sm, Metrics.thumb-entry-width). The strip is user-resizable
// (Metrics.thumbstrip-height-min/-max; the max keeps thumbnails at or below
// kDEFAULT_THUMBNAIL_MAX_DIM), and the entry width follows its height, so
// AppController reads the live width from the window's `thumb-entry-width` for
// its prefetch-window math instead of keeping a second copy here.
// Extra thumbs decoded on each side of the visible strip so resize/scroll
// doesn't leave black tiles. Keep in step with thumbstrip padding math
inline constexpr std::size_t kTHUMBSTRIP_PREFETCH = 8;

inline constexpr int kSVG_RENDER_MIN_DIM = 512;
inline constexpr int kSVG_RENDER_MAX_DIM = 9000;

// ---- Untrusted-input limits ----
// Dimensions and sizes come straight from file headers and can lie. Every
// decoder validates them through rgba_byte_size() (util/image_limits.hpp)
// before allocating, so a corrupt or hostile file fails to decode (and shows a
// black thumbnail) instead of triggering a huge allocation or an integer
// overflow in a size calculation.
inline constexpr int kMAX_IMAGE_DIMENSION                 = 65535;  // per side; JPEG's own format limit
inline constexpr std::size_t kMAX_IMAGE_PIXELS            = std::size_t{1} << 28;  // 16384 x 16384 = 1 GiB as RGBA8
inline constexpr std::uintmax_t kMAX_IMAGE_FILE_BYTES     = std::uintmax_t{1} << 30;  // whole-file reads (1 GiB)

// ---- UI ----
// Default/min window size and sidebar width bounds are defined in
// ui/theme.slint (Metrics.window-*-default, Metrics.window-*-min,
// Metrics.sidebar-width-min/-max). AppController caches them from
// `global<Metrics>()` at startup (m_default_window_w/h, m_min_window_w/h,
// m_sidebar_width_min/max) rather than redefining them here.
inline constexpr int kUI_TIMER_MS             = 25;
inline constexpr int kPIXEL_PERFECT_THRESHOLD = 256;

// ---- Zoomed-out rendering ----
// The GPU only bilinear-samples the image, which skips source pixels once it is
// drawn at under half size and looks harsh a little before that. Below this
// on-screen scale (device pixels per image pixel) a properly filtered copy at
// exactly the on-screen size is shown instead (ui/app.cpp).
inline constexpr float kZOOM_OUT_PROXY_MAX_SCALE = 0.75f;
// Skip the copy when it would itself be huge (image far larger than the
// screen): the GPU path is all that is left for those, and it is what a
// 16 MP+ texture costs anyway.
inline constexpr std::int64_t kZOOM_OUT_PROXY_MAX_PIXELS = std::int64_t{16} << 20;
// Zoom/resize must hold still this long before the filtered copy is rebuilt.
inline constexpr int kZOOM_OUT_PROXY_DEBOUNCE_MS = 120;

// ---- Checkerboard transparency grid ----
// Cell size is in screen pixels and stays fixed as you zoom (so zooming in
// tiles more squares across the larger on-screen image). Must be a power of
// 2; the starting size is Metrics.checker-cell-base in ui/theme.slint,
// cached as AppController::m_checker_cell_base. kCHECKER_MIN_CELL/MAX_CELL
// below are pure C++ clamps with no Slint equivalent.
inline constexpr int kCHECKER_MIN_CELL     = 2;
inline constexpr int kCHECKER_MAX_CELL     = 256;
// Upper bound on checkerboard cells per side. When zoomed in far enough to exceed
// it, the cell size is doubled until it fits, so the buffer stays small (<= ~4 MB).
inline constexpr int kCHECKER_MAX_CELLS_PER_SIDE = 1024;
// One cell fully transparent; the other is white at 10% opacity.
inline constexpr std::uint8_t kCHECKER_WHITE       = 255;
inline constexpr std::uint8_t kCHECKER_WHITE_ALPHA = 14;
inline constexpr std::uint8_t kCHECKER_CLEAR_ALPHA = 0;

// ---- Video playback ----
inline constexpr int kVIDEO_TIMER_MS          = 8;   // frame-presentation poll (~120 Hz)
inline constexpr int kVIDEO_SCRUB_INTERVAL_MS = 40;  // max seek rate while dragging the slider
inline constexpr float kDEFAULT_VIDEO_VOLUME  = 1.0f;

// ---- Archives ----
inline constexpr std::size_t kARCHIVE_BLOCK_SIZE = 16 * 1024;

}  // namespace biv