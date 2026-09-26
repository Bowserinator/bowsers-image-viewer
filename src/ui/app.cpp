#include "app.hpp"

#include "butil/log.hpp"
#include "butil/str.hpp"
#include "butil/util.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <clip.h>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <portable-file-dialogs.h>
#include <ranges>
#include <string>
#include <thread>
#include <vector>

#include "config.hpp"
#include "platform/windows_folder_sort.hpp"
#include "ui/sidebar/metadata.hpp"
#include "processing/histogram.hpp"
#include "ui/slint_image.hpp"
#include "util/cross_platform_dialog.hpp"
#include "util/screen.hpp"
#include "util/store.hpp"
#include "util/store_sqlite.hpp"

namespace biv {

namespace {

// Slint's generated ChannelMode enum <-> our pure channel::Mode enum.
channel::Mode to_channel_mode(ChannelMode m) {
    switch (m) {
        case ChannelMode::Invert: return channel::Mode::Invert;
        case ChannelMode::Histogram: return channel::Mode::RGB;
        case ChannelMode::Red: return channel::Mode::ChannelR;
        case ChannelMode::Green: return channel::Mode::ChannelG;
        case ChannelMode::Blue: return channel::Mode::ChannelB;
        case ChannelMode::Luminance: return channel::Mode::Luminance;
        case ChannelMode::Saturation: return channel::Mode::Saturation;
        case ChannelMode::SatHue: return channel::Mode::SatHue;
        case ChannelMode::SobelEdge: return channel::Mode::SobelEdge;
        case ChannelMode::Dog: return channel::Mode::Dog;
        case ChannelMode::AdaptiveBinary: return channel::Mode::AdaptiveBinary;
        case ChannelMode::Superpixels: return channel::Mode::Superpixels;
        case ChannelMode::HighPass: return channel::Mode::HighPass;
        case ChannelMode::Lsb0: return channel::Mode::Lsb0;
        case ChannelMode::Lsb1: return channel::Mode::Lsb1;
        case ChannelMode::Rgb:
        default: return channel::Mode::RGB;
    }
}

// Background thumbnail job priorities.
constexpr std::int32_t kThumbPriorityAdjacent = 5;
constexpr std::int32_t kThumbPriorityVisible  = 10;

// Label for the file open_path() failed on: just its filename, falling back
// to the whole path when it has no filename component (e.g. "/").
std::string open_target_name(const std::filesystem::path& path) {
    return path.filename().empty() ? path.string() : path.filename().string();
}

// Persisted-int <-> Slint enum. sqlite only stores plain text, so these two
// enums (defined once in ui/theme.slint) still cross an int boundary at the
// store; an unrecognized/out-of-range int falls back to a sane default
// instead of relying on a separately hardcoded max to clamp against.
SidebarTab sidebar_tab_from_int(int v) {
    switch (v) {
        case 0: return SidebarTab::Ops;
        case 2: return SidebarTab::Folder;
        default: return SidebarTab::Metadata;
    }
}

BgColorMode bg_color_mode_from_int(int v) {
    switch (v) {
        case 1: return BgColorMode::White;
        case 2: return BgColorMode::Checkerboard;
        default: return BgColorMode::Black;
    }
}

SortMode sort_mode_from_int(int v) {
    switch (v) {
        case 1: return SortMode::Size;
        case 2: return SortMode::MTime;
        default: return SortMode::Name;
    }
}

// Slint's generated SortOption enum <-> our plain-C++ filesystem::SortMode
// (kept separate so directory_model.hpp/.cpp don't need to include Slint
// headers -- same reasoning as to_channel_mode() above).
SortMode to_sort_mode(SortOption o) {
    switch (o) {
        case SortOption::Size: return SortMode::Size;
        case SortOption::Mtime: return SortMode::MTime;
        case SortOption::Name:
        default: return SortMode::Name;
    }
}

SortOption to_sort_option(SortMode m) {
    switch (m) {
        case SortMode::Size: return SortOption::Size;
        case SortMode::MTime: return SortOption::Mtime;
        case SortMode::Name:
        default: return SortOption::Name;
    }
}

std::string source_extension_lower(const ImageSource& src) {
    const std::string name = std::visit(
        [](const auto& s) -> std::string {
            using T = std::decay_t<decltype(s)>;
            if constexpr (std::is_same_v<T, FileSource>)
                return s.path.extension().string();
            else
                return std::filesystem::path(s.entry_name).extension().string();
        },
        src);
    return butil::lower(name);
}

const char* bool_str(bool v) noexcept {
    return v ? "true" : "false";
}

}  // namespace

AppController::AppController(slint::ComponentHandle<AppWindow> window) : m_window(std::move(window)) {
    // ui/theme.slint's `Metrics` global is the single source of truth for
    // these layout constants; read them back once instead of keeping a
    // second, hand-synced copy in config.hpp. (Window min/default size and
    // checker-cell-base are already the *default* value of their Slint
    // properties, so they don't need to be pushed back into the UI here.)
    const auto& metrics = m_window->global<Metrics>();
    m_thumb_spacing     = metrics.get_spacing_sm();
    m_min_window_w      = metrics.get_window_width_min();
    m_min_window_h      = metrics.get_window_height_min();
    m_default_window_w  = metrics.get_window_width_default();
    m_default_window_h  = metrics.get_window_height_default();
    m_sidebar_width_min = metrics.get_sidebar_width_min();
    m_sidebar_width_max = metrics.get_sidebar_width_max();
    m_checker_cell_base = static_cast<int>(std::lround(metrics.get_checker_cell_base()));

    m_thumbstrip_height_min = metrics.get_thumbstrip_height_min();
    m_thumbstrip_height_max = metrics.get_thumbstrip_height_max();

    m_video = std::make_unique<VideoController>(m_window);
    m_store = std::make_unique<SQLiteStore>(default_store_path());
    if (m_store->was_recovered()) {
        // Non-blocking OS notification rather than a modal: this shouldn't
        // stop the user from getting into the app, just tell them why their
        // settings just reset.
        pfd::notify("Settings reset",
            "Your saved settings were corrupted and have been reset to defaults.\nThe old file was kept at:\n" +
                m_store->recovery_backup_path().string(),
            pfd::icon::warning);
    }
    apply_persisted_state();

    m_window->on_request_open([this] {
        // "*.png *.jpg ..." straight from the extension list (images + videos).
        const auto globs = butil::join(
            kALL_EXTENSIONS | std::views::transform([](std::string_view e) { return "*" + std::string(e); }), " ");
        pfd::open_file dialog(
            "Open Image or Video", "", {"Media Files", globs, "All Files", "*"}, pfd::opt::multiselect);
        auto result = dialog.result();
        if (result.size() > 1)
            open_files(std::vector<std::filesystem::path>(result.begin(), result.end()));
        else if (!result.empty())
            open_path(std::filesystem::path(result[0]));
    });
    m_window->on_request_new_tab([this] {
        // Whatever's already on screen always has a tab by this point (see
        // apply_main_document()), so there's nothing to seed here -- just
        // add the newly picked file(s) as additional tabs.
        const auto globs = butil::join(
            kALL_EXTENSIONS | std::views::transform([](std::string_view e) { return "*" + std::string(e); }), " ");
        pfd::open_file dialog(
            "Open Image or Video", "", {"Media Files", globs, "All Files", "*"}, pfd::opt::multiselect);
        auto result = dialog.result();
        for (const auto& r : result)
            add_tab(std::filesystem::path(r));
    });
    m_window->on_request_tab_clicked([this](int i) { switch_tab(i); });
    m_window->on_request_tab_closed([this](int i) { close_tab(i); });
    m_window->on_request_prev([this] { go_prev(); });
    m_window->on_request_next([this] { go_next(); });
    m_window->on_request_first([this] { go_first(); });
    m_window->on_request_last([this] { go_last(); });
    m_window->on_request_fit([this] { set_fit_mode(true); });
    m_window->on_request_zoom_100([this] {
        if (!m_doc)
            return;
        m_window->set_fit_mode(false);
        m_window->set_zoom_percent(100.0f);
        float img_w = static_cast<float>(m_doc->width());
        float img_h = static_cast<float>(m_doc->height());
        float vw    = m_window->get_viewport_w();
        float vh    = m_window->get_viewport_h();
        m_window->set_vp_x(std::max(0.0f, (img_w - vw) / 2.0f));
        m_window->set_vp_y(std::max(0.0f, (img_h - vh) / 2.0f));
    });
    m_window->on_request_channel_changed([this](ChannelMode m) { set_channel_mode(m); });
    m_window->on_request_thumb_clicked([this](int i) { jump_to(static_cast<std::size_t>(i)); });
    m_window->on_request_heatmap_changed([this](bool enabled) { set_heatmap(enabled); });
    m_window->on_request_thumbstrip_scrolled([this](float vx, float vw) { on_thumbstrip_scrolled(vx, vw); });
    m_window->on_request_copy([this] {
        if (!m_doc)
            return;

        auto buf = m_doc->view(m_channel_mode);
        if (m_heatmap_enabled)
            buf = channel::apply_heatmap(buf);
        buf = colors::apply_orientation(
            buf, m_window->get_rotation(), m_window->get_flip_h(), m_window->get_flip_v());

        std::thread([buf = std::move(buf)] {
            clip::image_spec spec;
            spec.width          = buf.width;
            spec.height         = buf.height;
            spec.bits_per_pixel = 32;
            spec.bytes_per_row  = buf.width * 4;
            spec.red_mask       = 0x000000FF;
            spec.green_mask     = 0x0000FF00;
            spec.blue_mask      = 0x00FF0000;
            spec.alpha_mask     = 0xFF000000;
            spec.red_shift      = 0;
            spec.green_shift    = 8;
            spec.blue_shift     = 16;
            spec.alpha_shift    = 24;
            clip::image img(buf.pixels.data(), spec);
            clip::set_image(img);
        }).detach();
    });
    m_window->on_request_video_toggle_pause([this] { m_video->toggle_pause(); });
    m_window->on_request_video_step([this](int direction) { m_video->step(direction); });
    m_window->on_request_video_seek([this](float seconds, bool final) { m_video->seek(seconds, final); });
    m_window->on_request_video_volume([this](float volume) { m_video->set_volume(volume); });
    m_window->on_request_video_volume_committed([this] { persist_state(); });
    m_window->on_request_video_time_display_toggle([this] { m_video->toggle_time_display(); });
    m_window->on_request_favorite([this] { toggle_favorite(); });
    m_window->on_request_filter_favorites([this](bool on) { set_filter_favorites(on); });
    m_window->on_request_sidebar_toggled([this](bool) { persist_state(); });
    m_window->on_request_sidebar_width_changed([this] { persist_state(); });
    m_window->on_request_thumbstrip_toggled([this](bool) { persist_state(); });
    m_window->on_request_thumbstrip_height_changed([this] { persist_state(); });
    m_window->on_request_sidebar_tab_changed([this](SidebarTab) { persist_state(); });
    m_window->on_request_dark_mode_changed([this](bool) {
        sync_widget_color_scheme();
        persist_state();
    });
    m_window->on_request_bg_color_changed([this](BgColorMode mode) { set_bg_color_mode(mode); });
    m_window->on_request_sort_changed([this](SortOption opt) { set_sort_mode(to_sort_mode(opt)); });
    m_window->on_request_sort_direction_changed([this](bool descending) { set_sort_descending(descending); });
    m_window->on_request_print([this] {
        if (!m_doc)
            return;
        auto buf = m_doc->view(channel::Mode::RGB);
        std::thread([buf = std::move(buf)] { (void)platform::print_image(buf); }).detach();
    });
    m_window->on_request_open_folder([this] {
        if (!m_doc)
            return;
        std::visit(
            [](const auto& s) {
                using T = std::decay_t<decltype(s)>;
                if constexpr (std::is_same_v<T, FileSource>)
                    (void)platform::reveal_in_file_manager(s.path);
                else if constexpr (std::is_same_v<T, ArchiveSource>)
                    (void)platform::reveal_in_file_manager(s.archive_path);
            },
            m_doc->source());
    });
    m_ui_timer.start(
        slint::TimerMode::Repeated, std::chrono::milliseconds(kUI_TIMER_MS), [this] { pump_async_results(); });

    m_folder_model                     = FolderModel::create();
    m_folder_model->on_loading_changed = [this](bool loading) {
        m_window->set_folder_loading(loading);
    };
    m_window->set_folder_rows(m_folder_model);
    m_folder_model->set_sort(m_sort_mode, m_sort_descending);  // sort loaded from the store before the model existed
    m_window->on_request_folder_row_clicked([this](int row) { on_folder_row_clicked(row); });
}

AppController::~AppController() {
    m_folder_model->on_loading_changed = nullptr;
    persist_state();
}

void AppController::apply_persisted_state() {
    if (!m_store)
        return;

    if (auto v = m_store->get(kSTORE_KEY_SIDEBAR_VISIBLE)) {
        if (auto b = butil::str_to<bool>(*v))
            m_window->set_sidebar_visible(*b);
    }
    if (auto v = m_store->get(kSTORE_KEY_SIDEBAR_WIDTH)) {
        if (auto n = butil::str_to<double>(*v))
            m_window->set_sidebar_width(static_cast<float>(std::clamp(
                *n, static_cast<double>(m_sidebar_width_min), static_cast<double>(m_sidebar_width_max))));
    }
    if (auto v = m_store->get(kSTORE_KEY_THUMBSTRIP_VISIBLE)) {
        if (auto b = butil::str_to<bool>(*v))
            m_window->set_thumbstrip_visible(*b);
    }
    if (auto v = m_store->get(kSTORE_KEY_THUMBSTRIP_HEIGHT)) {
        if (auto n = butil::str_to<double>(*v))
            m_window->set_thumbstrip_height(static_cast<float>(std::clamp(
                *n, static_cast<double>(m_thumbstrip_height_min), static_cast<double>(m_thumbstrip_height_max))));
    }
    if (auto v = m_store->get(kSTORE_KEY_SIDEBAR_TAB)) {
        if (auto t = butil::str_to<int>(*v))
            m_window->set_sidebar_active_tab(sidebar_tab_from_int(*t));
    }
    if (auto v = m_store->get(kSTORE_KEY_BG_COLOR_MODE)) {
        if (auto n = butil::str_to<int>(*v)) {
            m_bg_color_mode = bg_color_mode_from_int(*n);
            m_window->set_bg_color_mode(m_bg_color_mode);
        }
    }
    if (auto v = m_store->get(kSTORE_KEY_SORT_OPTION)) {
        if (auto n = butil::str_to<int>(*v)) {
            m_sort_mode = sort_mode_from_int(*n);
            m_window->set_sort_option(to_sort_option(m_sort_mode));
        }
    }
    if (auto v = m_store->get(kSTORE_KEY_SORT_DESCENDING)) {
        if (auto b = butil::str_to<bool>(*v)) {
            m_sort_descending = *b;
            m_window->set_sort_descending(m_sort_descending);
        }
    }
    if (auto v = m_store->get(kSTORE_KEY_DARK_MODE)) {
        if (auto b = butil::str_to<bool>(*v))
            m_window->set_dark_mode(*b);
    }
    if (auto v = m_store->get(kSTORE_KEY_VIDEO_VOLUME)) {
        if (auto n = butil::str_to<double>(*v))
            m_video->set_volume(static_cast<float>(std::clamp(*n, 0.0, 1.0)));
    }
    if (auto v = m_store->get(kSTORE_KEY_VIDEO_TIME_REMAINING)) {
        if (auto b = butil::str_to<bool>(*v))
            m_video->set_time_display_remaining(*b);
    }
    sync_widget_color_scheme();
    m_favorites.clear();
    if (auto v = m_store->get(kSTORE_KEY_FAVORITES)) {
        for (auto& p : decode_list(*v))
            if (!p.empty())
                m_favorites.insert(std::move(p));
    }
    update_favorite_button();
    restore_window_size();
}

void AppController::sync_widget_color_scheme() {
    auto scheme = m_window->get_dark_mode() ? slint::cbindgen_private::ColorScheme::Dark
                                            : slint::cbindgen_private::ColorScheme::Light;
    m_window->global<WidgetPalette>().set_color_scheme(scheme);
}

void AppController::persist_state() {
    if (!m_store)
        return;
    m_store->set(kSTORE_KEY_SIDEBAR_VISIBLE, bool_str(m_window->get_sidebar_visible()));
    m_store->set(kSTORE_KEY_SIDEBAR_WIDTH, std::to_string(m_window->get_sidebar_width()));
    m_store->set(kSTORE_KEY_THUMBSTRIP_VISIBLE, bool_str(m_window->get_thumbstrip_visible()));
    m_store->set(kSTORE_KEY_THUMBSTRIP_HEIGHT, std::to_string(m_window->get_thumbstrip_height()));
    m_store->set(kSTORE_KEY_SIDEBAR_TAB, std::to_string(static_cast<int>(m_window->get_sidebar_active_tab())));
    m_store->set(kSTORE_KEY_DARK_MODE, bool_str(m_window->get_dark_mode()));
    m_store->set(kSTORE_KEY_BG_COLOR_MODE, std::to_string(static_cast<int>(m_bg_color_mode)));
    m_store->set(kSTORE_KEY_SORT_OPTION, std::to_string(static_cast<int>(m_sort_mode)));
    m_store->set(kSTORE_KEY_SORT_DESCENDING, bool_str(m_sort_descending));
    m_store->set(kSTORE_KEY_VIDEO_VOLUME, std::to_string(m_video->volume()));
    m_store->set(kSTORE_KEY_VIDEO_TIME_REMAINING, bool_str(m_video->time_display_remaining()));

    std::vector<std::string> favs(m_favorites.begin(), m_favorites.end());
    std::sort(favs.begin(), favs.end());
    m_store->set(kSTORE_KEY_FAVORITES, encode_list(favs));

    // Use the last size observed while the window was alive. After run()
    // returns, Window::size() is often the initial default (or 0), which
    // would persist a narrower window than the user just had.
    if (m_saved_window_w >= m_min_window_w && m_saved_window_h >= m_min_window_h) {
        m_store->set(kSTORE_KEY_WINDOW_WIDTH, std::to_string(static_cast<int>(std::lround(m_saved_window_w))));
        m_store->set(kSTORE_KEY_WINDOW_HEIGHT, std::to_string(static_cast<int>(std::lround(m_saved_window_h))));
        m_store->set(kSTORE_KEY_WINDOW_SCALE, std::to_string(m_saved_window_scale));
    }
    if (m_saved_window_pos) {
        m_store->set(kSTORE_KEY_WINDOW_X, std::to_string(m_saved_window_x));
        m_store->set(kSTORE_KEY_WINDOW_Y, std::to_string(m_saved_window_y));
    }
    m_store->save();
}

std::string AppController::current_source_key() const {
    if (!m_dir || m_dir->siblings().empty())
        return {};
    return source_key(m_dir->current());
}

void AppController::update_favorite_button() {
    if (m_showing_empty) {
        m_window->set_is_favorited(false);
        return;
    }
    const auto key = current_source_key();
    m_window->set_is_favorited(!key.empty() && m_favorites.contains(key));
}

bool AppController::is_favorite_source(const ImageSource& src) const {
    return m_favorites.contains(source_key(src));
}

std::optional<std::size_t> AppController::display_index_of(std::size_t sibling) const {
    auto it = std::find(m_visible.begin(), m_visible.end(), sibling);
    if (it == m_visible.end())
        return std::nullopt;
    return static_cast<std::size_t>(it - m_visible.begin());
}

std::size_t AppController::sibling_at_display(std::size_t display) const {
    return m_visible.at(display);
}

void AppController::rebuild_visible() {
    m_visible.clear();
    if (!m_dir)
        return;
    const auto& sibs = m_dir->siblings();
    m_visible.reserve(sibs.size());
    for (std::size_t i = 0; i < sibs.size(); ++i)
        if (!m_filter_favorites || is_favorite_source(sibs[i]))
            m_visible.push_back(i);
}

void AppController::clear_viewport_image() {
    m_video->stop();
    m_doc.reset();
    m_has_transparency = false;
    drop_display_pixels();
    update_histogram_overlay();
    m_window->set_current_image(slint::Image());
    m_window->set_has_transparency(false);
    m_window->set_checkerboard_image(slint::Image());
    m_window->set_current_filename(slint::SharedString());
    m_window->set_image_width(0);
    m_window->set_image_height(0);
    m_window->set_source_label(slint::SharedString());
    m_window->set_current_file_path(slint::SharedString());
    m_window->set_current_file_aspect(slint::SharedString());
    m_window->set_current_file_size(slint::SharedString());
    m_window->set_current_file_format(slint::SharedString());
    m_window->set_current_file_source(slint::SharedString());
    m_window->set_current_exif_camera(slint::SharedString());
    m_window->set_current_exif_date(slint::SharedString());
    m_window->set_current_exif_exposure(slint::SharedString());
    m_window->set_current_exif_aperture(slint::SharedString());
    m_window->set_current_exif_iso(slint::SharedString());
    m_window->set_current_exif_focal(slint::SharedString());
    m_window->set_current_exif_gps(slint::SharedString());
    m_window->set_is_favorited(false);
    m_window->set_thumb_current_index(0);
}

void AppController::show_empty_state(std::string_view message) {
    m_showing_empty = true;
    ++m_load_seq;  // ignore in-flight main loads
    m_window->set_image_loading(false);
    clear_viewport_image();
    if (message.empty())
        m_window->set_viewport_message(slint::SharedString(empty_viewport_message()));
    else
        m_window->set_viewport_message(slint::SharedString(std::string(message)));
    rebuild_thumb_model();
}

const char* AppController::empty_viewport_message() const {
    // Favorites-filter empty label takes precedence over "no images in folder".
    if (m_filter_favorites)
        return m_favorites.empty() ? "No favorites" : "No favorites in current directory";
    return "No images";
}

std::optional<slint::LogicalSize> AppController::monitor_logical_size() const {
    platform::ScreenSize px;
#if defined(_WIN32)
    const auto pos = m_window->window().position();
    px = platform::work_area_at_px(pos.x, pos.y);
    if (!px.valid)
        px = platform::primary_work_area_px();
#else
    px = platform::primary_work_area_px();
#endif

    if (!px.valid || px.width < 1.0f || px.height < 1.0f)
        return std::nullopt;

    float scale = m_window->window().scale_factor();
    if (!(scale > 0.0f))
        scale = 1.0f;
    return slint::LogicalSize({px.width / scale, px.height / scale});
}

void AppController::set_window_size_clamped(float w, float h) {
    if (const auto mon = monitor_logical_size()) {
        w = std::min(w, static_cast<float>(mon->width));
        h = std::min(h, static_cast<float>(mon->height));
    }
    w              = std::max(w, m_min_window_w);
    h              = std::max(h, m_min_window_h);
    const auto cur = m_window->window().size();
    if (std::abs(w - static_cast<float>(cur.width)) > 0.5f || std::abs(h - static_cast<float>(cur.height)) > 0.5f)
        m_window->window().set_size(slint::LogicalSize({w, h}));
}

void AppController::restore_window_size() {
    int x = 0;
    int y = 0;
    bool have_x = false;
    bool have_y = false;
    if (auto v = m_store->get(kSTORE_KEY_WINDOW_X)) {
        if (auto n = butil::str_to<int>(*v)) {
            x = *n;
            have_x = true;
        }
    }
    if (auto v = m_store->get(kSTORE_KEY_WINDOW_Y)) {
        if (auto n = butil::str_to<int>(*v)) {
            y = *n;
            have_y = true;
        }
    }
    if (have_x && have_y)
        m_window->window().set_position(slint::PhysicalPosition({x, y}));

    float w = m_default_window_w;
    float h = m_default_window_h;
    float saved_scale = 0.0f;
    if (auto v = m_store->get(kSTORE_KEY_WINDOW_WIDTH)) {
        if (auto n = butil::str_to<double>(*v); n && *n > 0.0)
            w = static_cast<float>(*n);
    }
    if (auto v = m_store->get(kSTORE_KEY_WINDOW_HEIGHT)) {
        if (auto n = butil::str_to<double>(*v); n && *n > 0.0)
            h = static_cast<float>(*n);
    }
    if (auto v = m_store->get(kSTORE_KEY_WINDOW_SCALE)) {
        if (auto n = butil::str_to<double>(*v); n && *n > 0.0)
            saved_scale = static_cast<float>(*n);
    }

    const float current_scale = m_window->window().scale_factor();
    if (saved_scale > 0.0f && current_scale > 0.0f) {
        w *= current_scale / saved_scale;
        h *= current_scale / saved_scale;
    }

    m_saved_window_w = w;
    m_saved_window_h = h;
    m_saved_window_scale = current_scale > 0.0f ? current_scale : 1.0f;
    m_window->set_restored_width(w);
    m_window->set_restored_height(h);
    set_window_size_clamped(w, h);
}

void AppController::note_live_window_size() {
    if (!m_window_size_ready)
        return;
    const auto sz = m_window->window().size();
    const float w = static_cast<float>(sz.width);
    const float h = static_cast<float>(sz.height);
    if (w >= m_min_window_w && h >= m_min_window_h) {
        m_saved_window_w = w;
        m_saved_window_h = h;
        const float scale = m_window->window().scale_factor();
        if (scale > 0.0f)
            m_saved_window_scale = scale;
    }
    const auto pos = m_window->window().position();
    m_saved_window_x = pos.x;
    m_saved_window_y = pos.y;
    m_saved_window_pos = true;
}

void AppController::set_bg_color_mode(BgColorMode mode) {
    if (m_bg_color_mode == mode)
        return;
    m_bg_color_mode = mode;
    m_window->set_bg_color_mode(mode);
    if (mode == BgColorMode::Checkerboard && m_has_transparency && m_doc) {
        float zoom_frac = m_window->get_zoom_percent() / 100.0f;
        const float iw = static_cast<float>(m_doc->width());
        const float ih = static_cast<float>(m_doc->height());
        if (m_window->get_fit_mode()) {
            const float vw = m_window->get_viewport_w();
            const float vh = m_window->get_viewport_h();
            if (vw > 0.0f && vh > 0.0f)
                zoom_frac = std::min(vw / iw, vh / ih);
        }
        const int dw = std::max(1, static_cast<int>(std::lround(iw * zoom_frac)));
        const int dh = std::max(1, static_cast<int>(std::lround(ih * zoom_frac)));
        update_checkerboard(dw, dh);
    } else {
        m_window->set_checkerboard_image(slint::Image());
        m_last_checker_cell = 0;
        m_last_checker_w = 0;
        m_last_checker_h = 0;
    }
    persist_state();
}

// Shared by set_sort_mode()/set_sort_descending(): re-sorts m_dir per the
// current m_sort_mode/m_sort_descending and refreshes everything keyed off
// sibling index. Sibling indices just changed meaning; the sibling-indexed
// thumbnail cache and any in-flight decode jobs from before the resort
// would now label the wrong image, so drop them the same way opening a
// fresh folder does (adopt_directory()) and let the strip repopulate.
void AppController::resort_directory() {
    if (!m_dir)
        return;
    m_dir->resort(m_sort_mode, m_sort_descending);
    ++m_folder_gen;
    m_loader.clear();
    m_thumb_cache.assign(m_dir->siblings().size(), std::nullopt);
    m_thumb_lru.clear();
    m_thumb_lru_pos.clear();
    rebuild_visible();
    if (auto d = display_index_of(m_dir->current_index()))
        m_window->set_thumb_current_index(static_cast<int>(*d));
    refresh_thumbstrip(true, std::nullopt);
}

void AppController::set_sort_mode(SortMode mode) {
    if (m_sort_mode == mode)
        return;
    m_sort_mode = mode;
    m_window->set_sort_option(to_sort_option(mode));
    m_folder_model->set_sort(m_sort_mode, m_sort_descending);
    resort_directory();
    persist_state();
}

void AppController::set_sort_descending(bool descending) {
    if (m_sort_descending == descending)
        return;
    m_sort_descending = descending;
    m_window->set_sort_descending(descending);
    m_folder_model->set_sort(m_sort_mode, m_sort_descending);
    resort_directory();
    persist_state();
}

void AppController::set_filter_favorites(bool enabled) {
    if (m_filter_favorites == enabled)
        return;
    m_filter_favorites = enabled;
    m_window->set_filter_favorites(enabled);
    apply_favorites_filter();
}

void AppController::apply_favorites_filter() {
    if (!m_dir) {
        if (m_filter_favorites)
            show_empty_state();
        else {
            m_showing_empty = false;
            m_window->set_viewport_message(slint::SharedString());
        }
        return;
    }

    const auto prev_sib = m_dir->current_index();
    rebuild_visible();
    if (m_visible.empty()) {
        show_empty_state();
        return;
    }

    m_showing_empty = false;
    m_window->set_viewport_message(slint::SharedString());

    bool need_reload = !m_doc;
    if (!display_index_of(m_dir->current_index())) {
        auto it = std::lower_bound(m_visible.begin(), m_visible.end(), prev_sib);
        if (it == m_visible.end())
            --it;
        m_dir->jump_to(*it);
        need_reload = true;
    }

    rebuild_thumb_model();
    if (need_reload)
        refresh_current_image();
    else {
        if (auto d = display_index_of(m_dir->current_index()))
            m_window->set_thumb_current_index(static_cast<int>(*d));
        update_favorite_button();
    }
    refresh_thumbstrip(true, std::nullopt);
}

void AppController::toggle_favorite() {
    const auto key = current_source_key();
    if (key.empty() || m_showing_empty)
        return;
    if (m_favorites.contains(key))
        m_favorites.erase(key);
    else
        m_favorites.insert(key);
    update_favorite_button();
    persist_state();
    if (m_filter_favorites)
        apply_favorites_filter();
}

// Shared by open_path() (and, via that, add_tab()): resets loader/thumb state for a freshly
// opened DirectoryModel and shows its first visible entry (or the empty state).
// `error` non-empty means the file the user asked for could NOT be opened (e.g.
// unsupported extension): keep the folder's thumbstrip/sidebar context but show
// the error overlay instead of jumping to some other image in the folder.
void AppController::adopt_directory(std::optional<DirectoryModel> dir, std::string_view error) {
    m_dir = std::move(dir);
    ++m_folder_gen;
    m_loader.clear();
    m_thumb_cache.assign(m_dir->siblings().size(), std::nullopt);
    m_thumb_lru.clear();
    m_thumb_lru_pos.clear();
    rebuild_visible();
    if (!error.empty()) {
        show_empty_state(error);
        return;
    }
    if (m_visible.empty()) {
        show_empty_state();
        return;
    }
    m_showing_empty = false;
    m_window->set_viewport_message(slint::SharedString());
    if (!display_index_of(m_dir->current_index()))
        m_dir->jump_to(m_visible.front());
    refresh_current_image();
    refresh_thumbstrip(true, std::nullopt);
}

// Windows only: if Explorer already has a sort (column and direction) on
// file for `folder`, prefer that over whatever the app last had saved --
// otherwise (non-Windows, or nothing on file for this folder)
// m_sort_mode/m_sort_descending are left as-is, i.e. whatever was loaded
// from the store.
void AppController::apply_explorer_folder_sort(const std::filesystem::path& folder) {
    if (auto detected = platform::detect_explorer_folder_sort(folder);
        detected && (detected->mode != m_sort_mode || detected->descending != m_sort_descending)) {
        m_sort_mode       = detected->mode;
        m_sort_descending = detected->descending;
        m_window->set_sort_option(to_sort_option(m_sort_mode));
        m_window->set_sort_descending(m_sort_descending);
        m_folder_model->set_sort(m_sort_mode, m_sort_descending);
        persist_state();
    }
}

void AppController::open_path(const std::filesystem::path& path) {
    namespace fs           = std::filesystem;
    const fs::path folder  = fs::is_directory(path)     ? path
                            : path.parent_path().empty() ? fs::path(".")
                                                          : path.parent_path();
    apply_explorer_folder_sort(folder);

    auto dir = DirectoryModel::open(path, m_sort_mode, m_sort_descending);
    if (!dir) {
        butil::log.error("Could not open '{}'", path.string());
        const auto name = open_target_name(path);
        show_empty_state("File not found: " + name);
        m_window->set_current_filename(slint::SharedString(name));
        return;
    }
    if (!dir->target_found()) {
        // The file exists but isn't a sibling (unsupported extension, or an
        // archive entry the listing skipped): show an error rather than
        // falling through to whatever image happens to come first.
        const auto name = open_target_name(path);
        adopt_directory(std::move(dir), "Unsupported file type: " + name);
        m_window->set_current_filename(slint::SharedString(name));
        return;
    }
    adopt_directory(std::move(dir));
}

void AppController::open_files(const std::vector<std::filesystem::path>& paths) {
    // A multi-file selection (multi-select "Open with", or several paths
    // passed on the command line) used to be collapsed into one DirectoryModel restricted to
    // just those files, which left the thumbstrip showing only the
    // selection instead of the folder it came from. Open each file as its
    // own tab instead (same as on_request_new_tab()'s handling of a
    // multi-select "Open" dialog) -- add_tab() -> open_path() re-derives
    // each file's folder and applies that folder's remembered Explorer
    // sort individually, so this also does the right thing for a selection
    // spanning more than one folder.
    namespace fs      = std::filesystem;
    bool opened_any   = false;
    for (const auto& p : paths) {
        if (!fs::exists(p) || fs::is_directory(p))
            continue;
        add_tab(p);
        opened_any = true;
    }
    if (!opened_any)
        butil::log.error("No valid image files in selection");
}

std::optional<std::filesystem::path> AppController::current_open_path() const {
    if (!m_doc)
        return std::nullopt;
    return std::visit(
        [](const auto& s) -> std::filesystem::path {
            using T = std::decay_t<decltype(s)>;
            if constexpr (std::is_same_v<T, FileSource>)
                return s.path;
            else
                return s.archive_path;
        },
        m_doc->source());
}

void AppController::rebuild_tab_model() {
    std::vector<TabEntry> entries;
    entries.reserve(m_tabs.size());
    for (std::size_t i = 0; i < m_tabs.size(); ++i) {
        TabEntry e;
        e.name       = slint::SharedString(m_tabs[i].path.filename().string());
        e.path       = slint::SharedString(m_tabs[i].path.string());
        e.is_current = (static_cast<int>(i) == m_active_tab);
        entries.push_back(std::move(e));
    }
    m_window->set_tabs(std::make_shared<slint::VectorModel<TabEntry>>(std::move(entries)));
}

void AppController::sync_active_tab() {
    auto p = current_open_path();
    if (!p)
        return;

    // Whatever image is on screen always gets a tab -- even the very first
    // one, opened before the user ever touches the tab bar -- so the bar
    // never shows a blank strip while an image is displayed.
    if (m_tabs.empty()) {
        m_tabs.push_back(TabInfo{*p});
        m_active_tab = 0;
        rebuild_tab_model();
        return;
    }

    if (m_active_tab < 0 || m_active_tab >= static_cast<int>(m_tabs.size()))
        return;
    if (*p == m_tabs[static_cast<std::size_t>(m_active_tab)].path)
        return;
    m_tabs[static_cast<std::size_t>(m_active_tab)].path = *p;
    rebuild_tab_model();
}

void AppController::add_tab(const std::filesystem::path& path) {
    m_tabs.push_back(TabInfo{path});
    m_active_tab = static_cast<int>(m_tabs.size()) - 1;
    open_path(path);
    rebuild_tab_model();
}

void AppController::switch_tab(int index) {
    if (index < 0 || index >= static_cast<int>(m_tabs.size()) || index == m_active_tab)
        return;
    m_active_tab = index;
    open_path(m_tabs[static_cast<std::size_t>(index)].path);
    rebuild_tab_model();
}

void AppController::close_tab(int index) {
    if (index < 0 || index >= static_cast<int>(m_tabs.size()))
        return;
    const bool was_active = (index == m_active_tab);
    m_tabs.erase(m_tabs.begin() + index);

    if (m_tabs.empty()) {
        m_active_tab = -1;
        rebuild_tab_model();
        show_empty_state();
        return;
    }
    if (was_active) {
        m_active_tab = std::min(index, static_cast<int>(m_tabs.size()) - 1);
        open_path(m_tabs[static_cast<std::size_t>(m_active_tab)].path);
    } else if (index < m_active_tab) {
        --m_active_tab;
    }
    rebuild_tab_model();
}

void AppController::set_initial_size(float w, float h) {
    w                = std::clamp(w, m_min_window_w, 3840.0f);
    h                = std::clamp(h, m_min_window_h, 2160.0f);
    m_saved_window_w = w;
    m_saved_window_h = h;
    const float scale = m_window->window().scale_factor();
    if (scale > 0.0f)
        m_saved_window_scale = scale;
    m_window->set_restored_width(w);
    m_window->set_restored_height(h);
    set_window_size_clamped(w, h);
}

void AppController::go_next() {
    if (!m_dir || m_visible.size() < 2)
        return;
    auto d = display_index_of(m_dir->current_index());
    if (!d)
        return;
    const auto prev = m_dir->current_index();
    m_dir->jump_to(m_visible[(*d + 1) % m_visible.size()]);
    refresh_current_image();
    refresh_thumbstrip(true, prev);
}

void AppController::go_prev() {
    if (!m_dir || m_visible.size() < 2)
        return;
    auto d = display_index_of(m_dir->current_index());
    if (!d)
        return;
    const auto prev = m_dir->current_index();
    m_dir->jump_to(m_visible[(*d == 0 ? m_visible.size() : *d) - 1]);
    refresh_current_image();
    refresh_thumbstrip(true, prev);
}

void AppController::jump_to(std::size_t display_index) {
    if (!m_dir || display_index >= m_visible.size())
        return;
    const auto prev = m_dir->current_index();
    m_dir->jump_to(m_visible[display_index]);
    refresh_current_image();
    refresh_thumbstrip(false, prev);
}

void AppController::go_first() {
    if (!m_dir || m_visible.empty())
        return;
    jump_to(0);
}

void AppController::go_last() {
    if (!m_dir || m_visible.empty())
        return;
    jump_to(m_visible.size() - 1);
}

void AppController::set_channel_mode(ChannelMode mode) {
    m_channel_mode       = to_channel_mode(mode);
    m_histogram_enabled  = mode == ChannelMode::Histogram;
    update_histogram_overlay();
    if (m_video->set_view(m_channel_mode, m_heatmap_enabled))
        return;  // a live video frame is on screen and was re-rendered
    show_document_pixels();
}

void AppController::set_fit_mode(bool fit) {
    m_window->set_fit_mode(fit);
    if (fit) {
        m_window->set_zoom_percent(100.0f);
        m_window->set_vp_x(0.0);
        m_window->set_vp_y(0.0);
    }
}

void AppController::set_heatmap(bool enabled) {
    m_heatmap_enabled = enabled;
    if (m_video->set_view(m_channel_mode, m_heatmap_enabled))
        return;  // a live video frame is on screen and was re-rendered
    show_document_pixels();
}

void AppController::refresh_current_image() {
    if (!m_dir || m_dir->siblings().empty() || m_visible.empty())
        return;
    m_showing_empty = false;
    m_window->set_viewport_message(slint::SharedString());
    m_video->stop();  // navigating away from a video / animated GIF / animated WebP
    m_window->set_image_loading(true);
    drop_display_pixels();
    m_window->set_current_image(slint::Image());  // unload previous image while loading
    m_window->set_has_transparency(false);
    m_has_transparency = false;
    update_favorite_button();
    m_load_seq = m_loader.request_main(m_dir->current_index(), m_dir->current(), m_folder_gen);

    // Keep the sidebar tree in sync (re-roots it if this folder is outside the tree).
    if (const auto* f = std::get_if<FileSource>(&m_dir->current()))
        m_folder_model->show_file(f->path);
}

void AppController::on_folder_row_clicked(int row) {
    if (row < 0)
        return;
    const auto r = static_cast<std::size_t>(row);
    if (m_folder_model->is_dir_row(r)) {
        m_folder_model->toggle(r);
        return;
    }
    if (const auto p = m_folder_model->file_at(r))
        open_path(*p);
}

void AppController::pump_async_results() {
    if (!m_window_size_ready) {
        if (m_restore_frames_left > 0) {
            --m_restore_frames_left;
        } else {
            restore_window_size();
            m_window_size_ready = true;
        }
    } else {
        note_live_window_size();
    }

    // Apply any thumbstrip centering requested since the last tick (see
    // refresh_thumbstrip()) now that a layout pass has had a chance to run.
    if (m_pending_thumb_center) {
        m_window->invoke_center_thumbstrip(static_cast<int>(*m_pending_thumb_center));
        m_pending_thumb_center.reset();
    }

    for (auto& res : m_loader.take_main_results()) {
        if (res.seq == m_load_seq) {
            m_window->set_image_loading(false);
            apply_main_document(std::move(res));
        }
    }
    for (auto& res : m_loader.take_thumb_results()) {
        if (res.folder_gen != m_folder_gen || !m_dir)
            continue;
        if (res.index >= m_dir->siblings().size())
            continue;
        // Cache failures as empty so the reconcile loop doesn't retry forever.
        insert_thumb(res.index, res.pixels ? to_slint_image(std::move(*res.pixels)) : slint::Image());
    }

    for (auto& res : m_loader.take_resize_results()) {
        // Only the newest request is wanted; a failed resample just leaves the full-size image showing.
        // (Never over a playing video/animation: that frame is the video controller's to paint.)
        if (res.seq != m_resize_seq || !res.pixels || !m_display_src || m_window->get_video_active())
            continue;
        m_proxy_w     = res.pixels->width;
        m_proxy_h     = res.pixels->height;
        m_proxy_shown = true;
        m_window->set_current_image(to_slint_image(*res.pixels));
    }

    sync_thumb_window();
    update_display_proxy();

    // Zoom / fit / viewport can change entirely in Slint; rebuild the
    // checkerboard when the on-screen image size actually changes.
    if (m_has_transparency && m_doc) {
        const int iw = m_doc->width();
        const int ih = m_doc->height();
        if (iw > 0 && ih > 0) {
            float zoom_frac = m_window->get_zoom_percent() / 100.0f;
            if (m_window->get_fit_mode()) {
                const float vw = m_window->get_viewport_w();
                const float vh = m_window->get_viewport_h();
                if (vw > 0.0f && vh > 0.0f)
                    zoom_frac = std::min(vw / static_cast<float>(iw), vh / static_cast<float>(ih));
            }
            const int dw = std::max(1, static_cast<int>(std::lround(static_cast<float>(iw) * zoom_frac)));
            const int dh = std::max(1, static_cast<int>(std::lround(static_cast<float>(ih) * zoom_frac)));
            update_checkerboard(dw, dh);
        }
    }
}

void AppController::apply_main_document(MainLoadResult&& res) {
    if (m_showing_empty || res.folder_gen != m_folder_gen || !m_dir)
        return;
    if (!res.doc) {
        const auto& source = m_dir->siblings().at(res.index);
        butil::log.error("Failed to load '{}'", display_name(source));
        // Don't leave the previously-displayed file's image/metadata on
        // screen for what is now a different (failed) file: clear it and
        // show the same kind of viewport label used for other "nothing to
        // show" states (e.g. "No favorites in current directory"), naming
        // the file that failed.
        clear_viewport_image();
        m_window->set_viewport_message(slint::SharedString("Failed to load: " + display_name(source)));
        m_window->set_current_filename(slint::SharedString(display_name(source)));
        if (auto d = display_index_of(res.index))
            m_window->set_thumb_current_index(static_cast<int>(*d));
        update_favorite_button();
        return;
    }
    m_window->set_viewport_message(slint::SharedString());
    m_doc = std::move(res.doc);

    show_document_pixels();
    update_histogram_overlay();
    if (res.thumb)
        insert_thumb(res.index, to_slint_image(std::move(*res.thumb)));

    m_window->set_fit_mode(true);
    m_window->set_zoom_percent(100.0f);
    m_window->set_vp_x(0.0);
    m_window->set_vp_y(0.0);
    m_window->set_rotation(0);
    m_window->set_flip_h(false);
    m_window->set_flip_v(false);
    m_window->set_current_filename(slint::SharedString(display_name(m_doc->source())));
    if (auto d = display_index_of(res.index))
        m_window->set_thumb_current_index(static_cast<int>(*d));
    m_window->set_image_width(m_doc->width());
    m_window->set_image_height(m_doc->height());
    m_window->set_interpolation_mode(
        m_doc->should_use_nearest_neighbor() ? InterpolationMode::PixelPerfect : InterpolationMode::Regular);
    m_window->set_source_label(
        is_archive_entry(m_doc->source())
            ? slint::SharedString(
                  "archive: " + std::get<ArchiveSource>(m_doc->source()).archive_path.filename().string())
            : slint::SharedString(""));

    m_has_transparency = butil::contains(kTRANSPARENT_EXTENSIONS, source_extension_lower(m_doc->source()));
    m_window->set_has_transparency(m_has_transparency);
    m_last_checker_cell = 0;
    m_last_checker_w    = 0;
    m_last_checker_h    = 0;
    if (!m_has_transparency)
        m_window->set_checkerboard_image(slint::Image());

    const auto meta = sidebar::collect_metadata(m_doc->source());
    m_window->set_current_file_path(slint::SharedString(meta.path));
    m_window->set_current_file_aspect(
        slint::SharedString(sidebar::format_aspect_ratio(m_doc->width(), m_doc->height())));
    m_window->set_current_file_size(slint::SharedString(meta.size));
    m_window->set_current_file_format(slint::SharedString(meta.format));
    m_window->set_current_file_source(slint::SharedString(meta.source));
    m_window->set_current_exif_camera(slint::SharedString(meta.camera));
    m_window->set_current_exif_date(slint::SharedString(meta.date));
    m_window->set_current_exif_exposure(slint::SharedString(meta.exposure));
    m_window->set_current_exif_aperture(slint::SharedString(meta.aperture));
    m_window->set_current_exif_iso(slint::SharedString(meta.iso));
    m_window->set_current_exif_focal(slint::SharedString(meta.focal));
    m_window->set_current_exif_gps(slint::SharedString(meta.gps));
    update_favorite_button();

    // Videos and animated GIFs/WebPs: the still on screen is a poster / first
    // frame; this swaps in live playback (no-op for ordinary images).
    m_video->set_view(m_channel_mode, m_heatmap_enabled);
    m_video->start(m_doc->source());

    sync_active_tab();
}

void AppController::refresh_thumbstrip(bool center, std::optional<std::size_t> prev_sibling) {
    if (!m_dir)
        return;
    if (!m_thumb_model || m_model_gen != m_folder_gen || m_thumb_model->row_count() != m_visible.size()) {
        rebuild_thumb_model();
    } else if (prev_sibling.has_value() && *prev_sibling != m_dir->current_index()) {
        auto flip = [&](std::size_t sibling, bool cur) {
            auto d = display_index_of(sibling);
            if (!d || !m_thumb_model)
                return;
            auto row = m_thumb_model->row_data(*d);
            if (!row)
                return;
            row->is_current = cur;
            m_thumb_model->set_row_data(*d, *row);
        };
        flip(*prev_sibling, false);
        flip(m_dir->current_index(), true);
    }

    if (center) {
        // Don't center synchronously: when rebuild_thumb_model() just above
        // swapped in a freshly (re)built model -- e.g. the empty state's
        // 0-row model giving way to the first real directory, which is the
        // biggest jump this ever does -- the Thumbstrip's Flickable hasn't
        // laid out the new rows yet, so its viewport-width still reflects
        // the old (often empty/narrow) content. center-on-index() would
        // compute against that stale width and land in the wrong place, or
        // not move at all. Defer to the next pump_async_results() tick,
        // after Slint has had a chance to lay the new rows out.
        if (auto d = display_index_of(m_dir->current_index()))
            m_pending_thumb_center = *d;
    }
    sync_thumb_window();
}

void AppController::rebuild_thumb_model() {
    std::vector<ThumbEntry> entries;
    if (m_dir) {
        entries.reserve(m_visible.size());
        const auto cur = m_dir->current_index();
        for (std::size_t sib : m_visible) {
            ThumbEntry e;
            e.name       = slint::SharedString(display_name(m_dir->siblings()[sib]));
            e.is_current = (sib == cur);
            if (sib < m_thumb_cache.size() && m_thumb_cache[sib])
                e.image = *m_thumb_cache[sib];
            entries.push_back(std::move(e));
        }
    }
    m_thumb_model = std::make_shared<slint::VectorModel<ThumbEntry>>(std::move(entries));
    m_window->set_thumbnails(m_thumb_model);
    m_model_gen = m_folder_gen;
}

void AppController::submit_display_range(std::size_t first, std::size_t last, std::int32_t priority) {
    if (!m_dir)
        return;
    last  = std::min(last, m_visible.size());
    first = std::min(first, last);
    for (std::size_t d = first; d < last; ++d) {
        const auto sib = m_visible[d];
        if (sib < m_thumb_cache.size() && m_thumb_cache[sib])
            continue;
        m_loader.request_thumb(sib, m_dir->siblings()[sib], m_folder_gen, priority);
    }
}

void AppController::insert_thumb(std::size_t sibling_index, slint::Image image) {
    if (!m_dir || sibling_index >= m_dir->siblings().size())
        return;
    if (sibling_index >= m_thumb_cache.size())
        m_thumb_cache.resize(m_dir->siblings().size());

    if (m_thumb_model) {
        if (auto d = display_index_of(sibling_index)) {
            ThumbEntry e;
            e.name       = slint::SharedString(display_name(m_dir->siblings()[sibling_index]));
            e.image      = image;
            e.is_current = (sibling_index == m_dir->current_index());
            m_thumb_model->set_row_data(*d, e);
        }
    }

    const bool already_cached    = m_thumb_cache[sibling_index].has_value();
    m_thumb_cache[sibling_index] = std::move(image);
    if (already_cached)
        return;  // LRU position unchanged; just refreshed the cached pixels

    if (auto it = m_thumb_lru_pos.find(sibling_index); it != m_thumb_lru_pos.end())
        m_thumb_lru.erase(it->second);
    m_thumb_lru.push_front(sibling_index);
    m_thumb_lru_pos[sibling_index] = m_thumb_lru.begin();

    while (m_thumb_lru.size() > kTHUMB_CACHE_CAP) {
        const auto victim = m_thumb_lru.back();
        m_thumb_lru.pop_back();
        m_thumb_lru_pos.erase(victim);
        if (victim < m_thumb_cache.size())
            m_thumb_cache[victim].reset();
    }
}

void AppController::on_thumbstrip_scrolled(float viewport_x, float viewport_w) {
    if (!m_dir || m_visible.empty())
        return;

    const auto total   = m_visible.size();
    const float origin = m_thumb_spacing;
    const float stride = std::max(m_window->get_thumb_entry_width() + m_thumb_spacing, 1.0f);
    auto first         = static_cast<std::size_t>(std::max(0.0f, std::floor((viewport_x - origin) / stride)));
    auto last          = static_cast<std::size_t>(
        std::max(0.0f, std::ceil((viewport_x + std::max(viewport_w, 1.0f) - origin) / stride)));
    last  = std::min(total, last);
    first = std::min(first, last);

    const auto pad    = kTHUMBSTRIP_PREFETCH;
    const auto win_lo = first > pad ? first - pad : 0;
    const auto win_hi = std::min(total, last + pad);

    if (win_lo < win_hi) {
        const auto sib_lo = m_visible[win_lo];
        const auto sib_hi = m_visible[win_hi - 1] + 1;
        m_loader.set_thumb_window(sib_lo, sib_hi);
    } else {
        m_loader.set_thumb_window(0, 0);
    }

    submit_display_range(first, last, kThumbPriorityVisible);
    submit_display_range(win_lo, first, kThumbPriorityAdjacent);
    submit_display_range(last, win_hi, kThumbPriorityAdjacent);
}

void AppController::sync_thumb_window() {
    if (!m_dir || m_visible.empty())
        return;

    float vx = 0.0f;
    float vw = 0.0f;
    if (m_window->get_thumbstrip_visible()) {
        vx = m_window->get_thumb_viewport_x();
        vw = m_window->get_thumb_viewport_w();
    }
    if (vw < 1.0f)
        vw = std::max(m_window->get_viewport_w(), 1.0f);

    on_thumbstrip_scrolled(vx, vw);
}

std::shared_ptr<const colors::RgbaBuffer> AppController::current_view_pixels() {
    auto pixels = m_doc->view_shared(m_channel_mode);
    if (m_heatmap_enabled)
        return std::make_shared<const colors::RgbaBuffer>(channel::apply_heatmap(*pixels));
    return pixels;
}

void AppController::show_document_pixels() {
    if (!m_doc)
        return;
    m_display_src = current_view_pixels();
    m_full_image  = to_slint_image(*m_display_src);
    reset_display_proxy();
    m_proxy_immediate = true;  // a new picture: get the filtered copy going right away
    m_window->set_current_image(m_full_image);
}

void AppController::update_histogram_overlay() {
    if (!m_histogram_enabled || !m_doc) {
        m_window->set_histogram_visible(false);
        return;
    }
    const auto overlay = channel::compute_histogram_overlay(m_doc->view(channel::Mode::RGB));
    m_window->set_histogram_visible(true);
    m_window->set_histogram_path_r(slint::SharedString(overlay.path_r));
    m_window->set_histogram_path_g(slint::SharedString(overlay.path_g));
    m_window->set_histogram_path_b(slint::SharedString(overlay.path_b));
    m_window->set_histogram_path_k(slint::SharedString(overlay.path_k));
    m_window->set_histogram_label_r_text(slint::SharedString(overlay.label_r_text));
    m_window->set_histogram_label_g_text(slint::SharedString(overlay.label_g_text));
    m_window->set_histogram_label_b_text(slint::SharedString(overlay.label_b_text));
    m_window->set_histogram_label_k_text(slint::SharedString(overlay.label_k_text));
}

void AppController::drop_display_pixels() {
    m_display_src.reset();
    m_full_image = slint::Image();
    reset_display_proxy();
}

void AppController::reset_display_proxy() {
    m_loader.cancel_resize();
    m_resize_seq      = 0;
    m_proxy_shown     = false;
    m_proxy_w         = 0;
    m_proxy_h         = 0;
    m_want_w          = 0;
    m_want_h          = 0;
    m_req_w           = 0;
    m_req_h           = 0;
    m_proxy_immediate = false;
}

// Puts the full-size image back on screen and forgets any pending filtered copy.
void AppController::show_full_res_image() {
    if (m_resize_seq != 0) {
        m_loader.cancel_resize();
        m_resize_seq = 0;
    }
    m_req_w = 0;
    m_req_h = 0;
    if (m_proxy_shown) {
        m_proxy_shown = false;
        m_proxy_w     = 0;
        m_proxy_h     = 0;
        m_window->set_current_image(m_full_image);
    }
}

// Slint draws the image with bilinear sampling, which reads only 2x2 source
// pixels per screen pixel: fine down to about half size, but past that it skips
// detail and shimmers, and text-heavy images fall apart well before that. So
// while the image is drawn noticeably smaller than it is, hand Slint a copy
// already filtered (Catmull-Rom, on a worker thread) to the on-screen size in
// device pixels, which it then draws roughly 1:1. Zoom and window changes are
// debounced; the full-size image stays up until the copy is ready, and comes
// straight back if the view grows past the copy's resolution.
void AppController::update_display_proxy() {
    // Only for a still we hold pixels for: a video / animation frame is owned by
    // the video controller, which repaints it itself.
    if (!m_doc || !m_display_src || m_window->get_video_active())
        return;

    const int iw = m_doc->width();
    const int ih = m_doc->height();
    if (iw <= 0 || ih <= 0)
        return;

    // Same scale Slint uses (fit: contain-in-viewport; otherwise the zoom), in logical px...
    float scale = 0.0f;
    if (m_window->get_fit_mode()) {
        const float vw = m_window->get_viewport_w();
        const float vh = m_window->get_viewport_h();
        if (vw <= 0.0f || vh <= 0.0f)
            return;
        scale = std::min(vw / static_cast<float>(iw), vh / static_cast<float>(ih));
    } else {
        scale = m_window->get_zoom_percent() / 100.0f;
    }
    // ... then in device pixels, which is what the GPU actually samples to.
    float dpr = m_window->window().scale_factor();
    if (!(dpr > 0.0f))
        dpr = 1.0f;
    scale *= dpr;

    const int tw = std::max(1, static_cast<int>(std::lround(static_cast<float>(iw) * scale)));
    const int th = std::max(1, static_cast<int>(std::lround(static_cast<float>(ih) * scale)));
    const bool wanted = scale > 0.0f && scale < kZOOM_OUT_PROXY_MAX_SCALE && (tw < iw || th < ih) &&
                        static_cast<std::int64_t>(tw) * th <= kZOOM_OUT_PROXY_MAX_PIXELS;
    if (!wanted) {
        show_full_res_image();
        m_want_w = 0;
        m_want_h = 0;
        return;
    }

    // A copy smaller than what is now drawn would be enlarged and look soft:
    // the full-size image is better until a fresh copy arrives.
    if (m_proxy_shown && static_cast<float>(std::max(m_proxy_w, m_proxy_h)) < 0.98f * static_cast<float>(std::max(tw, th)))
        show_full_res_image();

    const auto now = std::chrono::steady_clock::now();
    if (tw != m_want_w || th != m_want_h) {
        m_want_w     = tw;
        m_want_h     = th;
        m_want_since = now;
    }
    if (tw == m_req_w && th == m_req_h)
        return;  // already asked for (or showing) this size
    if (!m_proxy_immediate && now - m_want_since < std::chrono::milliseconds(kZOOM_OUT_PROXY_DEBOUNCE_MS))
        return;

    m_proxy_immediate = false;
    m_req_w           = tw;
    m_req_h           = th;
    m_resize_seq      = m_loader.request_resize(m_display_src, tw, th);
}

void AppController::update_checkerboard(int displayed_w, int displayed_h) {
    if (!m_has_transparency || !m_doc || displayed_w <= 0 || displayed_h <= 0) {
        if (m_last_checker_cell != 0) {
            m_window->set_checkerboard_image(slint::Image());
            m_last_checker_cell = 0;
            m_last_checker_w    = 0;
            m_last_checker_h    = 0;
        }
        return;
    }

    // Screen-space cells stay a fixed power-of-two size, so zooming in
    // grows the on-screen image and tiles more squares across it.
    int cell = std::clamp(m_checker_cell_base, kCHECKER_MIN_CELL, kCHECKER_MAX_CELL);

    // The buffer is one pixel per cell, so at high zoom (e.g. 5000% on a 4000x3000
    // image) a fixed screen-space cell would mean tens of thousands of cells per
    // side: gigabytes of pixels, rebuilt on every wheel tick. Past the cap, grow
    // the cell (staying a power of two) so the buffer never exceeds
    // kCHECKER_MAX_CELLS_PER_SIDE in either direction.
    const int longest = std::max(displayed_w, displayed_h);
    while (cell < (1 << 24) && (longest + cell - 1) / cell > kCHECKER_MAX_CELLS_PER_SIDE)
        cell *= 2;

    int cols = (displayed_w + cell - 1) / cell;
    int rows = (displayed_h + cell - 1) / cell;
    cols     = std::max(cols, 1);
    rows     = std::max(rows, 1);

    // The pattern depends only on cell/cols/rows, so compare those rather than the
    // displayed size (which changes on every zoom tick).
    if (cell == m_last_checker_cell && cols == m_last_checker_w && rows == m_last_checker_h)
        return;

    m_last_checker_cell = cell;
    m_last_checker_w    = cols;
    m_last_checker_h    = rows;

    slint::SharedPixelBuffer<slint::Rgba8Pixel> pixels(
        static_cast<std::uint32_t>(cols), static_cast<std::uint32_t>(rows));
    slint::Rgba8Pixel* p = pixels.begin();
    for (int y = 0; y < rows; ++y) {
        for (int x = 0; x < cols; ++x) {
            const bool white = ((x + y) & 1) == 0;
            auto& pix        = p[y * cols + x];
            pix.r            = kCHECKER_WHITE;
            pix.g            = kCHECKER_WHITE;
            pix.b            = kCHECKER_WHITE;
            pix.a            = white ? kCHECKER_WHITE_ALPHA : kCHECKER_CLEAR_ALPHA;
        }
    }

    m_window->set_checker_cell(cell);
    m_window->set_checkerboard_image(slint::Image(std::move(pixels)));
}

}  // namespace biv