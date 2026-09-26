#include "video_controller.hpp"

#include "config.hpp"

#ifdef BIV_ENABLE_VIDEO

    #include "butil/log.hpp"
    #include "butil/str.hpp"
    #include "butil/util.hpp"

    #include <algorithm>
    #include <chrono>
    #include <cmath>
    #include <cstdio>
    #include <optional>
    #include <private/slint_timer.h>
    #include <string>

    #include "ui/slint_image.hpp"
    #include "util/file.hpp"
    #include "util/image_format.hpp"
    #include "video/video_player.hpp"

namespace biv {

namespace {

using Clock = std::chrono::steady_clock;

// "MM:SS", or "HH:MM:SS" when the media is an hour or longer.
std::string format_clock(double seconds, bool with_hours) {
    const long total = std::lround(std::floor(std::max(0.0, seconds)));
    const long h     = total / 3600;
    const long m     = (total / 60) % 60;
    const long s     = total % 60;
    char buf[32];
    if (with_hours)
        std::snprintf(buf, sizeof buf, "%02ld:%02ld:%02ld", h, m, s);
    else
        std::snprintf(buf, sizeof buf, "%02ld:%02ld", m + h * 60, s);
    return buf;
}

}  // namespace

struct VideoController::Impl {
    explicit Impl(slint::ComponentHandle<AppWindow> w) : window(std::move(w)) {}

    ~Impl() { stop(); }

    void start(const ImageSource& src) {
        stop();
        const auto* file = std::get_if<FileSource>(&src);  // videos inside archives aren't supported
        if (!file)
            return;
        const auto ext      = butil::lower(file->path.extension().string());
        const bool is_video = butil::contains(kVIDEO_EXTENSIONS, ext);
        // GIFs/WebPs play only if they turn out to be animated; FFmpeg falls
        // back to NotPlayable/Failed for a plain still GIF, and tick() below
        // leaves the already-decoded first frame on screen in that case.
        if (!is_video && ext != ".gif" && ext != ".webp")
            return;
        // FFmpeg opens a still WebP as a zero-length "video" whose clock never
        // stops, so decide from the file header (VP8X animation flag) instead
        // and leave stills as the plain image they are.
        if (ext == ".webp") {
            const auto head = read_file_bytes(file->path, kIMAGE_SNIFF_BYTES, /*require_complete=*/false);
            if (!head || sniff_image_format(*head) != ImageFormat::WebpAnimated)
                return;
        }

        video::Options opt;
        opt.volume     = volume;
        player         = video::VideoPlayer::open(file->path, opt);
        info_published = false;
        window->set_video_position(0.0f);
        window->set_video_duration(0.0f);
        window->set_video_time_text(slint::SharedString());
        window->set_video_playing(false);
        // Show the bar right away for real videos so the viewport doesn't resize
        // once the file has opened. (GIFs/WebPs wait: a still image stays a still image.)
        window->set_video_active(is_video);
        timer.start(slint::TimerMode::Repeated, std::chrono::milliseconds(kVIDEO_TIMER_MS), [this] { tick(); });
    }

    void stop() {
        timer.stop();
        player.reset();
        last.reset();
        info_published = false;
        has_audio_ui   = false;
        scrubbing      = false;
        playing_ui     = false;
        last_text.clear();
        window->set_video_active(false);
        window->set_video_playing(false);
        window->set_video_has_audio(false);
        window->set_video_position(0.0f);
        window->set_video_duration(0.0f);
        window->set_video_time_text(slint::SharedString());
        window->set_current_file_encoding(slint::SharedString());
    }

    void tick() {
        if (!player)
            return;
        switch (player->state()) {
            case video::State::Opening: return;
            case video::State::Failed:
                butil::log.warn("Could not play this file");
                stop();
                return;
            case video::State::NotPlayable:
                stop();  // e.g. a single-frame GIF/WebP: keep the still image
                return;
            default: break;
        }

        if (!info_published) {
            const auto info = player->info();
            if (!info) {
                stop();
                return;
            }
            duration   = info->duration;
            with_hours = duration >= 3600.0;
            window->set_video_duration(static_cast<float>(duration));
            window->set_video_has_audio(info->has_audio);
            has_audio_ui = info->has_audio;
            window->set_video_active(true);

            std::string encoding = butil::upper(info->video_codec);
            if (info->fps > 0.0) {
                char fps_buf[32];
                std::snprintf(fps_buf, sizeof fps_buf, "%.2f fps", info->fps);
                encoding += ", ";
                encoding += fps_buf;
            }
            window->set_current_file_encoding(slint::SharedString(encoding));

            info_published = true;
        }

        if (auto frame = player->poll_frame()) {
            render(frame->image);
            if (last)
                player->recycle(std::move(*last));
            last = std::move(*frame);
        }

        if (has_audio_ui && !player->audio_available()) {  // audio was given up (e.g. undecodable track)
            window->set_video_has_audio(false);
            has_audio_ui = false;
        }
        publish_state();
    }

    void render(const colors::RgbaBuffer& raw) {
        if (mode == channel::Mode::RGB && !heatmap) {
            window->set_current_image(to_slint_image(raw));
            return;
        }
        colors::RgbaBuffer processed = mode == channel::Mode::RGB ? raw : channel::apply(raw, mode);
        if (heatmap)
            processed = channel::apply_heatmap(processed);
        window->set_current_image(to_slint_image(processed));
    }

    void publish_state() {
        const bool playing = player->state() == video::State::Playing;
        if (playing != playing_ui) {
            window->set_video_playing(playing);
            playing_ui = playing;
        }
        publish_position(player->position());
    }

    void publish_position(double pos) {
        window->set_video_position(static_cast<float>(pos));
        std::string text = show_remaining ? "-" + format_clock(std::max(0.0, duration - pos), with_hours) + " / " +
                                                format_clock(duration, with_hours)
                                          : format_clock(pos, with_hours) + " / " + format_clock(duration, with_hours);
        if (text != last_text) {
            window->set_video_time_text(slint::SharedString(text));
            last_text = std::move(text);
        }
    }

    // Flips the time label between elapsed and time-remaining display, and
    // repaints it immediately (rather than waiting for the next tick).
    void toggle_time_display() { set_time_display_remaining(!show_remaining); }

    void set_time_display_remaining(bool remaining) {
        show_remaining = remaining;
        if (player)
            publish_position(player->position());
    }

    void seek(float seconds, bool final) {
        if (!player)
            return;
        const auto now = Clock::now();
        if (!final) {
            if (!scrubbing) {  // first drag event: freeze playback while previewing
                scrubbing          = true;
                resume_after_scrub = player->state() == video::State::Playing;
                player->set_paused(true);
            }
            if (now - last_seek < std::chrono::milliseconds(kVIDEO_SCRUB_INTERVAL_MS))
                return;
        }
        last_seek = now;
        player->seek(seconds);
        publish_position(seconds);  // optimistic, so the slider doesn't snap back
        if (final && scrubbing) {
            scrubbing = false;
            if (resume_after_scrub)
                player->set_paused(false);
        }
    }

    bool set_view(channel::Mode m, bool h) {
        mode    = m;
        heatmap = h;
        if (!player || !last)
            return false;
        render(last->image);  // re-render the frame on screen (works while paused)
        return true;
    }

    slint::ComponentHandle<AppWindow> window;
    slint::Timer timer;
    std::unique_ptr<video::VideoPlayer> player;
    std::optional<video::Frame> last;  // frame currently on screen (raw RGBA)

    float volume       = kDEFAULT_VIDEO_VOLUME;
    channel::Mode mode = channel::Mode::RGB;
    bool heatmap       = false;

    bool info_published = false;
    bool has_audio_ui   = false;
    bool with_hours     = false;
    double duration     = 0.0;
    bool playing_ui     = false;
    std::string last_text;
    bool show_remaining = false;  // time label: elapsed vs. time-remaining

    bool scrubbing          = false;
    bool resume_after_scrub = false;
    Clock::time_point last_seek{};
};

VideoController::VideoController(slint::ComponentHandle<AppWindow> window)
    : m_impl(std::make_unique<Impl>(std::move(window))) {
    m_impl->window->set_video_volume(m_impl->volume);
}

VideoController::~VideoController() = default;

void VideoController::start(const ImageSource& src) {
    m_impl->start(src);
}

void VideoController::stop() {
    m_impl->stop();
}

bool VideoController::active() const {
    return m_impl->player != nullptr;
}

void VideoController::toggle_pause() {
    if (m_impl->player)
        m_impl->player->toggle_pause();
}

void VideoController::step(int direction) {
    if (m_impl->player)
        m_impl->player->step(direction);
}

void VideoController::seek(float seconds, bool final) {
    m_impl->seek(seconds, final);
}

void VideoController::set_volume(float volume) {
    m_impl->volume = std::clamp(volume, 0.0f, 1.0f);
    m_impl->window->set_video_volume(m_impl->volume);
    if (m_impl->player)
        m_impl->player->set_volume(m_impl->volume);
}

float VideoController::volume() const {
    return m_impl->volume;
}

void VideoController::toggle_time_display() {
    m_impl->toggle_time_display();
}

bool VideoController::time_display_remaining() const {
    return m_impl->show_remaining;
}

void VideoController::set_time_display_remaining(bool remaining) {
    m_impl->set_time_display_remaining(remaining);
}

bool VideoController::set_view(channel::Mode mode, bool heatmap) {
    return m_impl->set_view(mode, heatmap);
}

}  // namespace biv

#else  // !BIV_ENABLE_VIDEO -------------------------------------------------

namespace biv {

struct VideoController::Impl {};

VideoController::VideoController(slint::ComponentHandle<AppWindow>) : m_impl(std::make_unique<Impl>()) {}

VideoController::~VideoController() = default;

void VideoController::start(const ImageSource&) {}

void VideoController::stop() {}

bool VideoController::active() const {
    return false;
}

void VideoController::toggle_pause() {}

void VideoController::step(int) {}

void VideoController::seek(float, bool) {}

void VideoController::set_volume(float) {}

float VideoController::volume() const {
    return kDEFAULT_VIDEO_VOLUME;
}

void VideoController::toggle_time_display() {}

bool VideoController::time_display_remaining() const {
    return false;
}

void VideoController::set_time_display_remaining(bool) {}

bool VideoController::set_view(channel::Mode, bool) {
    return false;
}

}  // namespace biv

#endif
