#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <list>
#include <memory>
#include <optional>
#include <private/slint_timer.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "app.h"  // generated from ui/app.slint by slint_target_sources()
#include "config.hpp"
#include "filesystem/directory_model.hpp"
#include "image/image_document.hpp"
#include "ui/folder_model.hpp"
#include "ui/image_loader.hpp"
#include "ui/video_controller.hpp"

namespace biv {

class Store;

// One open tab. Only the path is kept -- the tab's folder is re-derived from
// it (same as open_path()) whenever the tab becomes active.
struct TabInfo {
    std::filesystem::path path;
};

class AppController {
public:
    explicit AppController(slint::ComponentHandle<AppWindow> window);
    ~AppController();

    void open_path(const std::filesystem::path& path);
    // Opens each of `paths` in its own new tab (see add_tab()) rather than
    // merging them into a single directory listing restricted to just this
    // selection.
    void open_files(const std::vector<std::filesystem::path>& paths);
    void set_initial_size(float w, float h);

    void go_next();
    void go_prev();
    void go_first();
    void go_last();
    void jump_to(std::size_t display_index);
    void set_channel_mode(ChannelMode mode);
    void set_fit_mode(bool fit);
    void set_heatmap(bool enabled);
    void on_thumbstrip_scrolled(float viewport_x, float viewport_w);

private:
    void adopt_directory(std::optional<DirectoryModel> dir, std::string_view error = {});
    void apply_explorer_folder_sort(const std::filesystem::path& folder);
    void refresh_current_image();
    void on_folder_row_clicked(int row);

    // ---- Tabs ----
    // The image currently on screen, in a form open_path() accepts (the
    // archive file itself for an archive entry, since that's the unit tabs
    // switch between).
    [[nodiscard]] std::optional<std::filesystem::path> current_open_path() const;
    void add_tab(const std::filesystem::path& path);
    void switch_tab(int index);
    void close_tab(int index);
    // Keeps the active tab's path following whatever image ends up on
    // screen (prev/next, thumbnail clicks, the main Open button, ...) so
    // switching away and back lands where the user left off.
    void sync_active_tab();
    void rebuild_tab_model();

    void refresh_thumbstrip(bool center, std::optional<std::size_t> prev_sibling);
    void pump_async_results();  // UI-thread drain, fired by m_ui_timer
    void apply_main_document(MainLoadResult&& res);
    void rebuild_thumb_model();
    void insert_thumb(std::size_t sibling_index, slint::Image image);
    void submit_display_range(std::size_t first, std::size_t last, std::int32_t priority);
    void sync_thumb_window();
    void update_checkerboard(int displayed_w, int displayed_h);

    // Zoomed-out rendering. The GPU only bilinear-samples, which aliases badly
    // once the image is drawn much smaller than it is; while zoomed out we show
    // a properly filtered copy at exactly the on-screen size instead.
    void show_document_pixels();  // channel/heatmap view of m_doc -> viewport
    void update_histogram_overlay();  // pushes histogram chart data to the viewport overlay
    void drop_display_pixels();
    void reset_display_proxy();
    void show_full_res_image();
    void update_display_proxy();  // UI-thread poll, fired from pump_async_results()
    [[nodiscard]] std::shared_ptr<const colors::RgbaBuffer> current_view_pixels();

    void apply_persisted_state();
    void persist_state();
    void sync_widget_color_scheme();
    void set_bg_color_mode(BgColorMode mode);
    void set_sort_mode(SortMode mode);
    void set_sort_descending(bool descending);
    void resort_directory();  // shared by set_sort_mode()/set_sort_descending()
    void sync_folder_sort();  // pushes m_sort_mode/m_sort_descending to the sidebar folder tree
    void toggle_favorite();
    void set_filter_favorites(bool enabled);
    void rebuild_visible();
    void apply_favorites_filter();
    // Shows the "nothing on screen" viewport label. With `message`, shows that
    // error text instead of the empty-state label (see adopt_directory()).
    void show_empty_state(std::string_view message = {});
    void clear_viewport_image();
    void update_favorite_button();
    [[nodiscard]] const char* empty_viewport_message() const;
    void restore_window_size();
    void set_window_size_clamped(float w, float h);
    void note_live_window_size();
    [[nodiscard]] std::optional<slint::LogicalSize> monitor_logical_size() const;
    [[nodiscard]] std::string current_source_key() const;
    [[nodiscard]] bool is_favorite_source(const ImageSource& src) const;
    [[nodiscard]] std::optional<std::size_t> display_index_of(std::size_t sibling) const;
    [[nodiscard]] std::size_t sibling_at_display(std::size_t display) const;

    slint::ComponentHandle<AppWindow> m_window;
    std::unique_ptr<VideoController> m_video;  // videos + animated GIFs/WebPs
    std::optional<DirectoryModel> m_dir;
    std::optional<ImageDocument> m_doc;
    channel::Mode m_channel_mode = channel::Mode::RGB;
    bool m_heatmap_enabled       = false;
    bool m_histogram_enabled     = false;

    BgColorMode m_bg_color_mode = BgColorMode::Black;
    SortMode m_sort_mode        = SortMode::Name;
    bool m_sort_descending      = false;

    // Cached at construction from ui/theme.slint's `Metrics` global, which is
    // the single source of truth for these values (see config.hpp).
    // The thumbstrip entry width is not cached: it follows the strip's
    // (user-resizable) height, so on_thumbstrip_scrolled() reads it from the
    // window each time.
    float m_thumb_spacing     = 0.0f;  // == Metrics.spacing-sm
    float m_min_window_w      = 0.0f;
    float m_min_window_h      = 0.0f;
    float m_default_window_w  = 0.0f;
    float m_default_window_h  = 0.0f;
    float m_sidebar_width_min = 0.0f;
    float m_sidebar_width_max = 0.0f;
    int m_checker_cell_base   = 0;

    float m_thumbstrip_height_min = 0.0f;
    float m_thumbstrip_height_max = 0.0f;

    std::uint64_t m_folder_gen = 0;  // bumped on every open_path
    std::uint64_t m_load_seq   = 0;  // seq of the most recently requested main load
    std::uint64_t m_model_gen  = 0;  // folder gen the thumb model was built for
    ImageLoader m_loader;

    // Display index to center the thumbstrip on, applied on the next
    // pump_async_results() tick rather than immediately -- see the comment
    // in refresh_thumbstrip() for why.
    std::optional<std::size_t> m_pending_thumb_center;
    slint::Timer m_ui_timer;
    std::shared_ptr<FolderModel> m_folder_model;

    // Persistent thumbnail model updated per-row as results arrive.
    std::shared_ptr<slint::VectorModel<ThumbEntry>> m_thumb_model;

    // LRU thumbnail image cache (small downscales), keyed by sibling index.
    std::vector<std::optional<slint::Image>> m_thumb_cache;
    std::list<std::size_t> m_thumb_lru;
    std::unordered_map<std::size_t, std::list<std::size_t>::iterator> m_thumb_lru_pos;

    // Zoomed-out rendering state (all UI-thread only).
    std::shared_ptr<const colors::RgbaBuffer> m_display_src;  // pixels behind m_full_image
    slint::Image m_full_image;                                // full-size image, kept to swap back to
    bool m_proxy_shown = false;                               // filtered copy on screen instead of it
    int m_proxy_w      = 0;                                   // ... and its size
    int m_proxy_h      = 0;
    int m_want_w       = 0;  // on-screen size seen on the last tick
    int m_want_h       = 0;
    std::chrono::steady_clock::time_point m_want_since;
    int m_req_w                = 0;  // size of the last request (0 = none outstanding)
    int m_req_h                = 0;
    std::uint64_t m_resize_seq = 0;  // the only resize result still wanted (0 = none)
    bool m_proxy_immediate     = false;  // first request for a new image skips the debounce

    // Checkerboard transparency grid state.
    bool m_has_transparency = false;
    int m_last_checker_cell = 0;
    int m_last_checker_w    = 0;
    int m_last_checker_h    = 0;

    std::unique_ptr<Store> m_store;
    std::unordered_set<std::string> m_favorites;

    bool m_filter_favorites   = false;
    bool m_showing_empty      = false;
    bool m_window_size_ready  = false;
    int m_restore_frames_left = 8;
    float m_saved_window_w    = 0.0f;
    float m_saved_window_h    = 0.0f;
    int m_saved_window_x      = 0;
    int m_saved_window_y      = 0;
    bool m_saved_window_pos   = false;
    float m_saved_window_scale = 1.0f;
    std::vector<std::size_t> m_visible;  // sibling indices currently shown in the strip

    // Open tabs; empty means the tab bar is hidden and the app behaves as a
    // single-image viewer. m_active_tab indexes m_tabs, or -1 when empty.
    std::vector<TabInfo> m_tabs;
    int m_active_tab = -1;
};

}  // namespace biv