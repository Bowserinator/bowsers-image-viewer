#pragma once

// cross_platform_dialog.hpp
//
// Header-only OS integration helpers (no UI toolkit dependency):
//
//   platform::reveal_in_file_manager(path)   "Open containing folder"
//   platform::print_image(original, opts)    "Print..."
//
// Reveal:
//   Linux    org.freedesktop.FileManager1.ShowItems over D-Bus (via `gdbus`),
//            selects the file; falls back to `xdg-open <parent dir>`.
//   Windows  explorer.exe /select,"<file>"
//   macOS    open -R <file>
//   For a file inside an archive, pass the archive's path, not the entry path.
//
// Print:
//   Prints the ORIGINAL decoded pixels in their ORIGINAL orientation: the
//   viewer's rotation / flip / channel-mode / heatmap state is deliberately
//   ignored, so pass the untouched decode, e.g.
//       platform::print_image(m_doc->view(channel::Mode::RGB));
//   The image is written to a one-page PDF (fit to page, centred, alpha
//   flattened onto white, page orientation chosen to match the image), then:
//     ShowDialog   (default) opens the PDF in the default viewer, whose own
//                  native print dialog (Ctrl/Cmd+P) does the printing.
//     PrintDirect  sends it to the default printer: `lp` (Linux/macOS) or the
//                  ShellExecute "print" verb (Windows). Falls back to ShowDialog.
//   Both functions are thread-safe and Slint-free, so they can run on a worker.
//
// Build notes:
//   - Windows links shell32 automatically under MSVC; with MinGW add `shell32`.
//   - Define BIV_PRINT_USE_ZLIB and link zlib to Flate-compress the PDF image
//     (much smaller temp files). Without it the pixels are stored uncompressed.

#include "butil/log.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <new>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "processing/channel_ops.hpp"

#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <shellapi.h>
    #include <windows.h>
    #if defined(_MSC_VER)
        #pragma comment(lib, "shell32.lib")
    #endif
#else
    #include <fcntl.h>
    #include <spawn.h>
    #include <sys/wait.h>
    #include <unistd.h>
    #if defined(__APPLE__)
        #include <crt_externs.h>
    #else
extern char** environ;
    #endif
#endif

#ifdef BIV_PRINT_USE_ZLIB
    #include <zlib.h>
#endif

namespace biv::platform {

struct PrintOptions {
    enum class Paper { Letter, A4 };
    enum class Action { ShowDialog, PrintDirect };

    Paper paper        = Paper::Letter;
    Action action      = Action::ShowDialog;
    double margin_pt   = 36.0;  // 0.5 inch on every side
    bool allow_upscale = true;  // false: never print larger than 96 DPI
};

namespace detail {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Process launching (POSIX). Arguments are passed as an argv vector, never
// through a shell, so odd characters in paths are safe.
// ---------------------------------------------------------------------------
#if !defined(_WIN32)

inline char** environment() {
    #if defined(__APPLE__)
    return *_NSGetEnviron();
    #else
    return environ;
    #endif
}

// Returns -1 if the program could not be started. With wait=true returns its
// exit code. With wait=false returns 0 immediately and reaps the child on a
// detached thread (GUI launchers like xdg-open may outlive the call).
inline int run(const std::vector<std::string>& args, bool wait) {
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& a : args)
        argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

    pid_t pid    = 0;
    const int rc = posix_spawnp(&pid, argv[0], &fa, nullptr, argv.data(), environment());
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0)
        return -1;

    auto reap = [pid]() -> int {
        int status = 0;
        while (waitpid(pid, &status, 0) < 0)
            if (errno != EINTR)
                return -1;
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    };

    if (wait)
        return reap();
    std::thread([reap] { reap(); }).detach();
    return 0;
}

// Percent-encodes a path into a file:// URI ('/' and unreserved chars kept).
inline std::string file_uri(const fs::path& p) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out              = "file://";
    for (unsigned char c : p.string()) {
        const bool keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '/' ||
                          c == '-' || c == '_' || c == '.' || c == '~';
        if (keep) {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 0xF];
        }
    }
    return out;
}

#endif  // !_WIN32

// ---------------------------------------------------------------------------
// Opening / printing a finished PDF
// ---------------------------------------------------------------------------

// Opens `file` with the default handler (its viewer provides the print dialog).
inline bool open_with_default_app(const fs::path& file) {
#if defined(_WIN32)
    const HINSTANCE h = ShellExecuteW(nullptr, L"open", file.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(h) > 32;
#elif defined(__APPLE__)
    return run({"open", file.string()}, false) == 0;
#else
    return run({"xdg-open", file.string()}, false) == 0;
#endif
}

// Sends `file` straight to the default printer.
inline bool print_to_default_printer(const fs::path& file) {
#if defined(_WIN32)
    const HINSTANCE h = ShellExecuteW(nullptr, L"print", file.c_str(), nullptr, nullptr, SW_HIDE);
    return reinterpret_cast<INT_PTR>(h) > 32;
#else
    return run({"lp", file.string()}, true) == 0;
#endif
}

// ---------------------------------------------------------------------------
// Minimal one-page PDF writer
// ---------------------------------------------------------------------------

inline bool valid_buffer(const colors::RgbaBuffer& b) noexcept {
    return b.valid();
}

// Locale-independent number formatting ('.' decimal point always).
inline std::string num(double v) {
    std::ostringstream os;
    os.imbue(std::locale::classic());
    os << std::fixed << std::setprecision(3) << v;
    return os.str();
}

inline void page_size_pt(PrintOptions::Paper p, double& w, double& h) {
    if (p == PrintOptions::Paper::A4) {
        w = 595.276;
        h = 841.890;
    } else {
        w = 612.0;
        h = 792.0;
    }
}

// RGBA8 -> RGB8 with alpha flattened onto white paper. Deflated when zlib is
// enabled (sets `flate`). Rows are converted one at a time to bound memory.
inline bool encode_rgb_stream(const colors::RgbaBuffer& img, std::vector<std::uint8_t>& out, bool& flate) {
    const auto W = static_cast<std::size_t>(img.width);
    const auto H = static_cast<std::size_t>(img.height);
    std::vector<std::uint8_t> row(W * 3);

    auto fill_row = [&](std::size_t y) {
        const std::uint8_t* s = img.pixels.data() + y * W * 4;
        std::uint8_t* d       = row.data();
        for (std::size_t x = 0; x < W; ++x, s += 4, d += 3) {
            const unsigned a = s[3];
            if (a == 255) {
                d[0] = s[0];
                d[1] = s[1];
                d[2] = s[2];
            } else {
                const unsigned inv = 255u * (255u - a) + 127u;
                d[0]               = static_cast<std::uint8_t>((s[0] * a + inv) / 255u);
                d[1]               = static_cast<std::uint8_t>((s[1] * a + inv) / 255u);
                d[2]               = static_cast<std::uint8_t>((s[2] * a + inv) / 255u);
            }
        }
    };

#ifdef BIV_PRINT_USE_ZLIB
    z_stream zs{};
    if (deflateInit(&zs, Z_BEST_SPEED) != Z_OK)
        return false;
    std::vector<std::uint8_t> chunk(1u << 16);
    auto pump = [&](int flush) -> bool {
        int rc;
        do {
            zs.next_out  = chunk.data();
            zs.avail_out = static_cast<uInt>(chunk.size());
            rc           = deflate(&zs, flush);
            if (rc < 0)
                return false;
            out.insert(out.end(), chunk.data(), chunk.data() + (chunk.size() - zs.avail_out));
        } while (rc == Z_OK && zs.avail_out == 0);
        return true;
    };
    bool ok = true;
    for (std::size_t y = 0; y < H && ok; ++y) {
        fill_row(y);
        zs.next_in  = row.data();
        zs.avail_in = static_cast<uInt>(row.size());
        ok          = pump(Z_NO_FLUSH);
    }
    if (ok) {
        zs.next_in  = nullptr;
        zs.avail_in = 0;
        ok          = pump(Z_FINISH);
    }
    deflateEnd(&zs);
    flate = true;
    return ok;
#else
    out.reserve(W * H * 3);
    for (std::size_t y = 0; y < H; ++y) {
        fill_row(y);
        out.insert(out.end(), row.begin(), row.end());
    }
    flate = false;
    return true;
#endif
}

// Writes a one-page PDF: image fit inside the margins, centred, portrait or
// landscape to match the image's aspect ratio. Pixels are drawn as stored.
inline bool write_pdf(const fs::path& path, const colors::RgbaBuffer& img, const PrintOptions& opt) {
    const double iw = img.width;
    const double ih = img.height;

    double pw = 0, ph = 0;
    page_size_pt(opt.paper, pw, ph);
    if (iw > ih)
        std::swap(pw, ph);  // landscape image -> landscape page

    const double avail_w = std::max(1.0, pw - 2.0 * opt.margin_pt);
    const double avail_h = std::max(1.0, ph - 2.0 * opt.margin_pt);
    double scale         = std::min(avail_w / iw, avail_h / ih);
    if (!opt.allow_upscale)
        scale = std::min(scale, 72.0 / 96.0);  // natural size at 96 DPI
    const double dw = iw * scale;
    const double dh = ih * scale;
    const double x  = (pw - dw) / 2.0;
    const double y  = (ph - dh) / 2.0;

    std::vector<std::uint8_t> data;
    bool flate = false;
    if (!encode_rgb_stream(img, data, flate))
        return false;

    const std::string content = "q\n" + num(dw) + " 0 0 " + num(dh) + " " + num(x) + " " + num(y) + " cm\n/Im0 Do\nQ\n";

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f)
        return false;

    std::vector<std::streamoff> offs(6, 0);
    auto begin_obj = [&](int n) {
        offs[static_cast<std::size_t>(n)] = f.tellp();
        f << n << " 0 obj\n";
    };

    f << "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";

    begin_obj(1);
    f << "<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";

    begin_obj(2);
    f << "<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n";

    begin_obj(3);
    f << "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " << num(pw) << " " << num(ph) << "] "
      << "/Resources << /XObject << /Im0 5 0 R >> >> /Contents 4 0 R >>\nendobj\n";

    begin_obj(4);
    f << "<< /Length " << content.size() << " >>\nstream\n" << content << "endstream\nendobj\n";

    begin_obj(5);
    f << "<< /Type /XObject /Subtype /Image /Width " << img.width << " /Height " << img.height
      << " /ColorSpace /DeviceRGB /BitsPerComponent 8 " << (flate ? "/Filter /FlateDecode " : "") << "/Length "
      << data.size() << " >>\nstream\n";
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    f << "\nendstream\nendobj\n";

    const std::streamoff xref = f.tellp();
    f << "xref\n0 6\n0000000000 65535 f \n";
    for (std::size_t n = 1; n <= 5; ++n) {
        char line[32];
        std::snprintf(line, sizeof line, "%010lld 00000 n \n", static_cast<long long>(offs[n]));
        f.write(line, 20);
    }
    f << "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n" << xref << "\n%%EOF\n";

    f.flush();
    return f.good();
}

// Removes stale print PDFs from earlier sessions (viewers read the file
// asynchronously, so the current one can't be deleted right after launching).
inline void cleanup_old_print_files(const fs::path& dir) {
    std::error_code ec;
    const auto now = fs::file_time_type::clock::now();
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const auto name = it->path().filename().string();
        if (name.rfind("biv_print_", 0) != 0 || it->path().extension() != ".pdf")
            continue;
        std::error_code e2;
        const auto mtime = fs::last_write_time(it->path(), e2);
        if (!e2 && now - mtime > std::chrono::hours(24))
            fs::remove(it->path(), e2);
    }
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// Opens the file manager showing `target` (file selected where supported).
// Directories are simply opened. Returns false if nothing could be launched.
[[nodiscard]] inline bool reveal_in_file_manager(const std::filesystem::path& target) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path abs = fs::absolute(target, ec).lexically_normal();
    if (ec || !fs::exists(abs, ec)) {
        butil::log.error("Cannot reveal '{}': path does not exist", target.string());
        return false;
    }
    const bool is_dir = fs::is_directory(abs, ec);

#if defined(_WIN32)
    abs.make_preferred();
    const std::wstring params = is_dir ? L"\"" + abs.native() + L"\"" : L"/select,\"" + abs.native() + L"\"";
    const HINSTANCE h = ShellExecuteW(nullptr, L"open", L"explorer.exe", params.c_str(), nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(h) > 32;
#elif defined(__APPLE__)
    return (is_dir ? detail::run({"open", abs.string()}, false) : detail::run({"open", "-R", abs.string()}, false)) ==
           0;
#else
    if (!is_dir) {
        // Selects the file in Nautilus, Dolphin, Nemo, Thunar, ... The URI is
        // fully percent-encoded, so it can't break out of the GVariant string.
        const std::string arg = "['" + detail::file_uri(abs) + "']";
        const int rc          = detail::run({"gdbus",
                                                "call",
                                                "--session",
                                                "--timeout",
                                                "3",
                                                "--dest",
                                                "org.freedesktop.FileManager1",
                                                "--object-path",
                                                "/org/freedesktop/FileManager1",
                                                "--method",
                                                "org.freedesktop.FileManager1.ShowItems",
                                                arg,
                                                ""},
            true);
        if (rc == 0)
            return true;
    }
    // No FileManager1 service (or gdbus missing): open the folder instead.
    return detail::run({"xdg-open", (is_dir ? abs : abs.parent_path()).string()}, false) == 0;
#endif
}

// Prints `original` (untransformed decode, original orientation). See the
// file header for behaviour. Returns false if the PDF could not be created or
// no viewer/printer could be launched.
[[nodiscard]] inline bool print_image(const colors::RgbaBuffer& original, const PrintOptions& opt = {}) {
    namespace fs = std::filesystem;
    if (!detail::valid_buffer(original)) {
        butil::log.error("print_image: invalid or empty image buffer");
        return false;
    }

    std::error_code ec;
    const fs::path tmp = fs::temp_directory_path(ec);
    if (ec) {
        butil::log.error("print_image: no temp directory available");
        return false;
    }
    detail::cleanup_old_print_files(tmp);

    const auto stamp   = std::chrono::system_clock::now().time_since_epoch().count();
    const fs::path pdf = tmp / ("biv_print_" + std::to_string(stamp) + ".pdf");

    try {
        if (!detail::write_pdf(pdf, original, opt)) {
            butil::log.error("print_image: failed to write '{}'", pdf.string());
            fs::remove(pdf, ec);
            return false;
        }
    } catch (const std::bad_alloc&) {
        butil::log.error("print_image: out of memory encoding image");
        fs::remove(pdf, ec);
        return false;
    }

    if (opt.action == PrintOptions::Action::PrintDirect && detail::print_to_default_printer(pdf))
        return true;
    return detail::open_with_default_app(pdf);
}

}  // namespace biv::platform
