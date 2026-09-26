#pragma once

// video_controller.hpp
//
// UI-thread glue between video::VideoPlayer and the Slint window:
//   - starts/stops playback for the current image source (videos, animated
//     GIFs/WebPs)
//   - a timer pulls frames and pushes them into `current-image`
//   - keeps the control bar (play/pause, progress, time, volume) in sync
//
// Builds as a no-op stub when BIV_ENABLE_VIDEO is not defined.

#include <memory>

#include "app.h"  // generated from ui/app.slint
#include "core/source.hpp"
#include "processing/channel_ops.hpp"

namespace biv {

class VideoController {
public:
    explicit VideoController(slint::ComponentHandle<AppWindow> window);
    ~VideoController();

    VideoController(const VideoController&)            = delete;
    VideoController& operator=(const VideoController&) = delete;

    // Stops any current playback, then starts playing `src` if it is a video
    // or an animated GIF/WebP (asynchronous; the still image on screen stays
    // until the first frame arrives). For anything else this just stops.
    void start(const ImageSource& src);
    void stop();
    [[nodiscard]] bool active() const;

    void toggle_pause();
    void step(int direction);              // -1 / +1 frame (pauses)
    void seek(float seconds, bool final);  // final=false while the slider is being dragged
    void set_volume(float volume);         // 0..1
    [[nodiscard]] float volume() const;

    // Toggles the control bar's time label between elapsed ("MM:SS / MM:SS")
    // and time-remaining ("-MM:SS / MM:SS") display. Purely a display
    // preference; playback and seeking are unaffected.
    void toggle_time_display();
    // Current time-label preference (true = time-remaining); persisted/restored
    // by the caller via the store.
    [[nodiscard]] bool time_display_remaining() const;
    void set_time_display_remaining(bool remaining);

    // Channel/analysis mode and heatmap applied to every displayed frame.
    // Returns true if a live video frame is on screen and was re-rendered with
    // the new settings (so the caller must not paint its still image over it).
    bool set_view(channel::Mode mode, bool heatmap);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace biv
