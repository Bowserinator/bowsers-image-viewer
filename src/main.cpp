#include "butil/argparse.hpp"
#include "butil/log.hpp"
#include "butil/util.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include "config.hpp"
#include "platform/windows_argv.hpp"
#include "platform/windows_console.hpp"
#include "platform/windows_file_association.hpp"
#include "ui/app.hpp"
#include "util/color.hpp"
#include "util/file.hpp"

int main(int argc, char** argv) {
    // GUI-subsystem Windows build: reconnect stdout/stderr when started from a terminal.
    biv::platform::attach_parent_console();

    // On Windows, argv is decoded using the process's ANSI code page and
    // silently mangles any argument containing a character outside it --
    // including a file or folder path opened via double-click,
    // drag-and-drop, or "Open with", which is how most non-ASCII paths
    // reach this app on Windows. Re-derive argv from the real (UTF-16)
    // command line as UTF-8 instead; std::filesystem::path round-trips
    // UTF-8 losslessly on every platform (see path_from_utf8() below).
    // No-op on other platforms: argc/argv are left untouched.
    std::vector<std::string> utf8_argv_storage;
    std::vector<char*> utf8_argv_ptrs;
    if (auto utf8 = biv::platform::windows_utf8_argv()) {
        utf8_argv_storage = std::move(*utf8);
        utf8_argv_ptrs.reserve(utf8_argv_storage.size());
        for (auto& s : utf8_argv_storage)
            utf8_argv_ptrs.push_back(s.data());
        argc = static_cast<int>(utf8_argv_ptrs.size());
        argv = utf8_argv_ptrs.data();
    }

    using butil::Arg;
    using ArgAction = butil::ArgMeta::Action;

    butil::Parser parser(
        std::string(biv::kAPP_NAME), "A square, keyboard-driven image viewer with channel/analysis views.");
    parser.add_section("Options");
    parser.add_argument(Arg("--version", "Show version and exit").action(ArgAction::STORE_TRUE));
    parser.add_argument(Arg("--size", "Force window size (e.g. 1920x1080)").metavar("WxH"));
    parser.add_argument(Arg("--channel", "Initial channel/analysis mode")
            .choices({"rgb", "r", "g", "b", "luma", "sathue", "heatmap"})
            .default_val("rgb"));
    parser.add_argument(Arg("--fit", "Start in fit-to-window mode").action(ArgAction::STORE_TRUE));
    parser.add_argument(Arg("--jobs", "Number of threads to use for parallel image processing")
            .short_name("-j")
            .metavar("N")
            .default_val("8"));

    auto parsed = parser.parse(argc, argv);
    if (!parsed)
        return 1;  // --help was shown, or a required arg was missing

    // --version: print and exit
    if (butil::try_get(*parsed, std::string("--version"), std::string("0")) == "1") {
        std::cout << biv::kAPP_NAME << " " << biv::kAPP_VERSION << "\n";
        return 0;
    }

    // Parse --size WxH
    float force_w = 0, force_h = 0;
    if (auto size_str = butil::try_get(*parsed, std::string("--size"), std::string("")); !size_str.empty()) {
        auto sep = size_str.find_first_of("xX");
        if (sep != std::string::npos) {
            force_w = std::strtof(size_str.c_str(), nullptr);
            force_h = std::strtof(size_str.c_str() + sep + 1, nullptr);
        }
        if (force_w <= 0 || force_h <= 0) {
            std::cerr << "error: --size requires WxH (e.g. 1920x1080)\n";
            return 1;
        }
    }

    // Parse -j/--jobs (falls back to kDEFAULT_PARALLEL_THREADS on a bad value).
    const std::string jobs_str = butil::try_get(*parsed, std::string("--jobs"), std::string("8"));
    long jobs                  = std::strtol(jobs_str.c_str(), nullptr, 10);
    if (jobs <= 0) {
        std::cerr << "error: --jobs/-j requires a positive integer\n";
        return 1;
    }
    biv::colors::set_thread_count(static_cast<std::size_t>(jobs));

    biv::platform::register_windows_file_associations();

    // Wayland/X11 (xdg-compliant WMs, e.g. KWin on KDE) resolve a window's
    // title-bar/taskbar icon by matching this app id against an installed
    // .desktop file's name, then reading that file's Icon= key -- Wayland
    // has no per-window icon API, so without this the title bar falls back
    // to a generic icon. Must match the desktop entry's basename
    // (share/applications/bowsers_image_viewer.desktop) and be set before
    // the window is shown.
    slint::set_xdg_app_id(biv::kAPP_ID);

    auto window = AppWindow::create();
    // Same default as ui/theme.slint's Metrics.window-width-default/-height-default
    // (AppController::restore_window_size() overwrites this once it has read
    // a saved size off disk).
    const auto& metrics = window->global<Metrics>();
    window->window().set_size(
        slint::LogicalSize({metrics.get_window_width_default(), metrics.get_window_height_default()}));
    biv::AppController controller(window);

    // Open files from positional args
    const std::string args_str = butil::try_get(*parsed, std::string("args"), std::string(""));
    if (!args_str.empty()) {
        std::vector<std::filesystem::path> paths;
        std::istringstream iss(args_str);
        std::string token;
        // '\n' matches the delimiter butil::parse_args() uses to join
        // positional args; splitting on ' ' would break any path containing a space.
        while (std::getline(iss, token, '\n'))
            if (!token.empty())
                // token is UTF-8 (argv was re-derived as UTF-8 above on
                // Windows, and already is UTF-8 bytes on other platforms);
                // path_from_utf8() is the encoding-correct way to turn that
                // into a path on every platform. A plain
                // std::filesystem::path(token) would reinterpret those UTF-8
                // bytes via the ANSI code page on Windows, undoing the argv
                // fix above.
                paths.push_back(biv::path_from_utf8(token));

        if (paths.size() == 1)
            controller.open_path(paths[0]);
        else
            controller.open_files(paths);
    } else {
        butil::log.info("No path given; use Open to get started.");
    }

    // Apply --size after open (overrides persisted window size)
    if (force_w > 0 && force_h > 0)
        controller.set_initial_size(force_w, force_h);

    window->run();
    return 0;
}
