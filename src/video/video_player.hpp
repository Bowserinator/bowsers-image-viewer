#pragma once

// video_player.hpp
//
// FFmpeg demux/decode + miniaudio output. Handles video files (mkv, mp4, ...)
// and animated GIFs. No UI toolkit dependency: the UI thread just polls for
// frames and drives the control methods below.
//
// Threads
//   - one decode thread (demux + video decode + audio decode/resample)
//   - miniaudio's audio callback thread (drains a small ring buffer)
//   - the caller's UI thread (poll_frame() / position() / controls)
//
// Sync model: audio is the master clock (falls back to the wall clock when
// there is no audio, or once the audio ends). Video frames are presented when
// the clock reaches their timestamp; late frames are dropped.
//
// All public methods are safe to call from the UI thread.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "processing/channel_ops.hpp"

namespace biv::video {

struct StreamInfo {
    int width           = 0;
    int height          = 0;
    double duration     = 0.0;  // seconds, 0 = unknown
    double fps          = 0.0;
    bool has_audio      = false;  // an audio stream exists AND an output device opened
    bool animated_image = false;  // GIF-style: loops, no audio controls
    std::string container;
    std::string video_codec;
    std::string audio_codec;
};

struct Frame {
    colors::RgbaBuffer image;  // RGBA8, width*height*4 bytes
    double pts = 0.0;          // seconds from the start of the media
};

enum class State {
    Opening,  // worker is opening the file
    Playing,
    Paused,
    Ended,        // reached the end (not looping); toggle play restarts
    Failed,       // could not open/decode
    NotPlayable,  // opened fine but isn't really animated (e.g. 1-frame GIF)
};

struct Options {
    float volume     = 1.0f;  // 0..1
    bool autoplay    = true;
    bool allow_audio = true;
};

class VideoPlayer {
public:
    // Starts opening `path` on a worker thread and returns immediately.
    [[nodiscard]] static std::unique_ptr<VideoPlayer> open(std::filesystem::path path, Options opt = {});
    ~VideoPlayer();

    VideoPlayer(const VideoPlayer&)            = delete;
    VideoPlayer& operator=(const VideoPlayer&) = delete;

    [[nodiscard]] State state() const;
    // Valid once state() has left Opening (and not Failed).
    [[nodiscard]] std::optional<StreamInfo> info() const;
    // False when there is no sound: no audio stream/device, or audio was given
    // up because it couldn't be decoded (video keeps playing without it).
    [[nodiscard]] bool audio_available() const;

    // Current playback position in seconds (frozen while paused/seeking).
    [[nodiscard]] double position() const;
    [[nodiscard]] bool paused() const;

    // Returns the next frame that should be on screen now, if any. At most one
    // per call; stale/late frames are skipped internally.
    [[nodiscard]] std::optional<Frame> poll_frame();
    // Hands a frame's buffer back for reuse (optional, saves allocations).
    void recycle(Frame&& frame);

    void set_paused(bool paused);
    void toggle_pause();
    // Precise seek: shows the last frame at or before `seconds`.
    void seek(double seconds);
    // Pauses and moves exactly one frame forward (+1) or back (-1).
    void step(int direction);
    void set_volume(float volume);  // 0..1

private:
    struct Impl;
    explicit VideoPlayer(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};

// Decodes a representative still frame (used for thumbnails and as the
// pre-playback image). Seeks ~10% in for longer clips so the frame isn't
// a black fade-in. Thread-safe; runs on the calling thread.
//
// `max_dim` > 0 scales the frame down to fit that size on the long edge during
// conversion (cheap, and never builds the full-size RGBA image); 0 = full size.
[[nodiscard]] std::optional<colors::RgbaBuffer> decode_poster_frame(
    const std::filesystem::path& path, int max_dim = 0);

}  // namespace biv::video
