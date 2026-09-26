#include "video/video_player.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <format>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

// Only playback is needed from miniaudio; drop the rest to keep the TU small.
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MINIAUDIO_IMPLEMENTATION
#include "butil/log.hpp"

#include <miniaudio.h>

#include "image/thumbnail.hpp"
#include "util/file.hpp"
#include "util/image_limits.hpp"

namespace biv::video {

namespace fs = std::filesystem;

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kAudioRate                  = 48000;
constexpr int kAudioChannels              = 2;
constexpr std::size_t kRingFrames         = static_cast<std::size_t>(kAudioRate) * 2;  // 2 s
constexpr std::size_t kBacklogLimitFrames = static_cast<std::size_t>(kAudioRate) * 3;  // decoded, not yet in ring
constexpr std::size_t kFrameQueueBytes    = 96u << 20;
constexpr double kPresentTolerance        = 0.002;  // seconds a frame may be early and still shown
constexpr double kAudioAlignTolerance     = 0.010;  // audio/clock drift (s) tolerated before padding/skipping

// libswscale's SIMD kernels can write a little past the end of the last row of
// the destination image. Measured under Valgrind on a plain 480x270 H.264 clip:
// writes up to 16+ bytes beyond a tightly-sized 518,400-byte buffer, which
// corrupts the heap. Destination buffers get this much slack allocated.
constexpr std::size_t kSwsTailPadding = 128;  // > the widest SIMD store (AVX-512: 64 bytes)

// A poster frame should appear within a few dozen packets; a file that yields
// none in this many is treated as undecodable rather than read to the end.
constexpr int kPosterMaxPackets = 2000;

std::string av_err(int e) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(e, buf, sizeof buf);
    return buf;
}

void init_ffmpeg_logging() {
    static std::once_flag once;
    std::call_once(once, [] { av_log_set_level(AV_LOG_ERROR); });
}

struct FormatDel {
    void operator()(AVFormatContext* c) const { avformat_close_input(&c); }
};

struct CodecDel {
    void operator()(AVCodecContext* c) const { avcodec_free_context(&c); }
};

struct FrameDel {
    void operator()(AVFrame* f) const { av_frame_free(&f); }
};

struct PacketDel {
    void operator()(AVPacket* p) const { av_packet_free(&p); }
};

struct SwrDel {
    void operator()(SwrContext* s) const { swr_free(&s); }
};

using FormatPtr = std::unique_ptr<AVFormatContext, FormatDel>;
using CodecPtr  = std::unique_ptr<AVCodecContext, CodecDel>;
using FramePtr  = std::unique_ptr<AVFrame, FrameDel>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDel>;
using SwrPtr    = std::unique_ptr<SwrContext, SwrDel>;

CodecPtr open_decoder(const AVStream* st, bool threaded) {
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) {
        butil::log.error("No decoder for codec '{}'", avcodec_get_name(st->codecpar->codec_id));
        return nullptr;
    }
    CodecPtr ctx(avcodec_alloc_context3(codec));
    if (!ctx || avcodec_parameters_to_context(ctx.get(), st->codecpar) < 0)
        return nullptr;
    ctx->pkt_timebase = st->time_base;
    if (threaded) {
        ctx->thread_count = 0;  // auto
        ctx->thread_type  = FF_THREAD_FRAME | FF_THREAD_SLICE;
    }
    if (const int rc = avcodec_open2(ctx.get(), codec, nullptr); rc < 0) {
        butil::log.error("avcodec_open2 failed: {}", av_err(rc));
        return nullptr;
    }
    return ctx;
}

// Applies the sample (pixel) aspect ratio to a frame size: width grows for SAR > 1,
// height for SAR < 1, so anamorphic video isn't shown squeezed or stretched.
void apply_sar(int& w, int& h, AVRational sar) {
    if (sar.num <= 0 || sar.den <= 0 || sar.num == sar.den)
        return;
    // Widen for SAR > 1, heighten for SAR < 1. The ratio comes from the file, so keep
    // the result inside sane bounds.
    const bool widen   = sar.num > sar.den;
    int& dim           = widen ? w : h;
    const double ratio = widen ? static_cast<double>(sar.num) / sar.den : static_cast<double>(sar.den) / sar.num;
    dim = static_cast<int>(std::clamp<long long>(std::llround(dim * ratio), 1, kMAX_IMAGE_DIMENSION));
}

int sws_colorspace_for(const AVFrame* f) {
    switch (f->colorspace) {
        case AVCOL_SPC_BT709: return SWS_CS_ITU709;
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M: return SWS_CS_ITU601;
        case AVCOL_SPC_SMPTE240M: return SWS_CS_SMPTE240M;
        case AVCOL_SPC_FCC: return SWS_CS_FCC;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL: return SWS_CS_BT2020;
        default: return f->height >= 720 ? SWS_CS_ITU709 : SWS_CS_ITU601;
    }
}

// YUV/palette/RGB (any source pixel format) -> RGBA8 at a fixed output size.
class RgbaConverter {
public:
    RgbaConverter()                                = default;
    RgbaConverter(const RgbaConverter&)            = delete;
    RgbaConverter& operator=(const RgbaConverter&) = delete;

    ~RgbaConverter() { sws_freeContext(m_ctx); }

    // `sws_flags` picks the scaler; SWS_AREA is better when shrinking a lot (thumbnails).
    bool convert(const AVFrame* f, int out_w, int out_h, colors::RgbaBuffer& out, int sws_flags = SWS_BILINEAR) {
        // Frame and output sizes come from the stream: validate before allocating.
        const auto out_bytes = rgba_byte_size(out_w, out_h);
        if (!out_bytes || !rgba_byte_size(f->width, f->height) || f->format < 0)
            return false;

        const auto fmt = static_cast<AVPixelFormat>(f->format);
        m_ctx          = sws_getCachedContext(
            m_ctx, f->width, f->height, fmt, out_w, out_h, AV_PIX_FMT_RGBA, sws_flags, nullptr, nullptr, nullptr);
        if (!m_ctx)
            return false;

        // Correct YUV->RGB matrix/range (swscale defaults to BT.601 otherwise).
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(fmt);
        if (desc && !(desc->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_PAL))) {
            const int cs    = sws_colorspace_for(f);
            const int range = f->color_range == AVCOL_RANGE_JPEG ? 1 : 0;
            if (cs != m_cs || range != m_range || m_last_ctx != m_ctx) {
                sws_setColorspaceDetails(
                    m_ctx, sws_getCoefficients(cs), range, sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);
                m_cs    = cs;
                m_range = range;
            }
        }
        m_last_ctx = m_ctx;

        out.width  = out_w;
        out.height = out_h;
        out.pixels.reserve(*out_bytes + kSwsTailPadding);  // slack for swscale's tail writes (see kSwsTailPadding)
        out.pixels.resize(*out_bytes);
        std::uint8_t* dst[4]    = {out.pixels.data(), nullptr, nullptr, nullptr};
        const int dst_stride[4] = {out_w * 4, 0, 0, 0};
        return sws_scale(m_ctx, f->data, f->linesize, 0, f->height, dst, dst_stride) > 0;
    }

private:
    SwsContext* m_ctx            = nullptr;
    const SwsContext* m_last_ctx = nullptr;
    int m_cs                     = -1;
    int m_range                  = -1;
};

}  // namespace

// ===========================================================================
// VideoPlayer::Impl
// ===========================================================================

struct VideoPlayer::Impl {
    enum class Phase { Opening, Active, Failed, NotPlayable };
    enum class ClockMode { Audio, Wall };

    struct QFrame {
        Frame frame;
        std::uint32_t serial = 0;
        bool force           = false;  // first frame after a seek: show regardless of clock/pause
    };

    struct Cmd {
        enum class Kind { Abs, Relative } kind = Kind::Abs;
        double t                               = 0.0;
        std::uint32_t seq                      = 0;  // frames decoded for this command carry it as their serial
    };

    struct AudioChunk {
        std::vector<float> samples;  // interleaved stereo
        std::size_t offset = 0;      // frames already written to the ring
        double pts         = 0.0;
    };

    Impl(fs::path p, Options o) : path(std::move(p)), opt(o) {
        volume.store(o.volume);
        paused.store(!o.autoplay);
        ring.assign(kRingFrames * kAudioChannels, 0.0f);
        worker = std::jthread([this](std::stop_token st) { run(st); });
    }

    ~Impl() {
        worker.request_stop();
        room_cv.notify_all();
        if (worker.joinable())
            worker.join();
        if (audio_ready)
            ma_device_uninit(&device);
    }

    // ------------------------------------------------------------------
    // Public-facing (UI thread)
    // ------------------------------------------------------------------

    State state() const {
        switch (phase.load()) {
            case Phase::Opening: return State::Opening;
            case Phase::Failed: return State::Failed;
            case Phase::NotPlayable: return State::NotPlayable;
            case Phase::Active: break;
        }
        if (is_ended())
            return State::Ended;
        return paused.load() ? State::Paused : State::Playing;
    }

    bool audio_available() const { return audio_active.load(); }

    std::optional<StreamInfo> get_info() const {
        std::lock_guard l(info_mu);
        return info;
    }

    // Raw clock (offset space; keeps increasing across loops).
    double clock_now() {
        std::lock_guard l(clk_mu);
        if (paused.load())
            return paused_pos;
        if (awaiting.load())
            return await_pos;
        return running_pos_locked();
    }

    double position() {
        double p         = clock_now();
        const double dur = duration.load();
        if (dur > 0.0) {
            if (loop_enabled.load())
                p = std::fmod(std::max(p, 0.0), dur);
            else
                p = std::clamp(p, 0.0, dur);
        }
        return std::max(0.0, p);
    }

    std::optional<Frame> poll_frame() {
        if (phase.load() != Phase::Active)
            return std::nullopt;

        std::unique_lock ql(q_mu);
        const auto ser = serial.load();
        while (!q.empty() && q.front().serial != ser)
            q.pop_front();
        if (q.empty())
            return std::nullopt;

        const bool is_paused = paused.load();
        bool stepped         = false;
        if (!q.front().force) {
            if (is_paused) {
                if (step_pending.load() <= 0)
                    return std::nullopt;
                stepped = true;
            } else {
                const double t = clock_now();  // lock order: q_mu -> clk_mu
                if (q.front().frame.pts > t + kPresentTolerance)
                    return std::nullopt;
                // Skip frames that are already late; show the newest due one.
                while (q.size() > 1 && q[1].serial == ser && !q[1].force && q[1].frame.pts <= t + kPresentTolerance)
                    q.pop_front();
            }
        }
        QFrame qf = std::move(q.front());
        q.pop_front();
        ql.unlock();
        room_cv.notify_all();

        if (stepped) {
            step_pending.fetch_sub(1);
            std::lock_guard l(clk_mu);
            resync_needed = true;  // video moved without audio: resync on resume
        }
        on_frame_shown(qf.frame.pts, qf.force);
        return std::move(qf.frame);
    }

    void recycle(Frame&& f) {
        std::lock_guard l(pool_mu);
        if (pool.size() < 4)
            pool.push_back(std::move(f.image.pixels));
    }

    void set_paused(bool p) {
        const bool ended = is_ended();  // takes q_mu; must happen before clk_mu
        std::lock_guard l(clk_mu);
        if (p == paused.load() && !(ended && !p))  // "play" at the end restarts even though not paused
            return;
        if (p) {
            paused_pos = awaiting.load() ? await_pos : running_pos_locked();
            paused.store(true);
            return;
        }
        step_pending.store(0);
        double resume_at     = paused_pos;
        const bool need_seek = ended || resync_needed;
        if (ended)
            resume_at = 0.0;
        paused.store(false);
        if (need_seek) {
            resync_needed = false;
            awaiting      = true;
            await_pos     = resume_at;
            post_cmd({Cmd::Kind::Abs, resume_at});
        } else if (mode.load() == ClockMode::Wall) {
            wall_base = resume_at;
            wall_t0   = Clock::now();
        }
    }

    void seek(double t) {
        t                = std::max(0.0, t);
        const double dur = duration.load();
        if (dur > 0.0)
            t = std::min(t, dur);
        {
            std::lock_guard l(clk_mu);
            resync_needed = false;
            step_pending.store(0);
            if (paused.load()) {
                paused_pos = t;
            } else {
                awaiting  = true;
                await_pos = t;
            }
        }
        post_cmd({Cmd::Kind::Abs, t});
    }

    void step(int dir) {
        set_paused(true);
        if (dir > 0) {
            step_pending.fetch_add(1);
        } else if (dir < 0) {
            {
                std::lock_guard l(clk_mu);
                resync_needed = false;
                step_pending.store(0);
            }
            // Want the last frame at least half a frame before the one on screen.
            post_cmd({Cmd::Kind::Relative, last_shown_pts.load() - 0.5 * frame_dur.load()});
        }
    }

    void set_volume(float v) {
        v = std::clamp(v, 0.0f, 1.0f);
        volume.store(v);
        if (audio_ready)
            ma_device_set_master_volume(&device, v * v);  // squared: closer to perceived loudness
    }

    // ------------------------------------------------------------------
    // Clock helpers (clk_mu held)
    // ------------------------------------------------------------------

    // Audio mode: a counter of device frames played (silence included), so the
    // clock never stalls when audio is late, missing or finished. Wall mode is
    // used only when there is no audio stream/device.
    double running_pos_locked() {
        if (mode.load() == ClockMode::Audio) {
            std::lock_guard rl(ring_mu);
            return r_base + static_cast<double>(r_played) / kAudioRate;
        }
        return wall_base + std::chrono::duration<double>(Clock::now() - wall_t0).count();
    }

    void on_frame_shown(double pts, bool force) {
        std::lock_guard l(clk_mu);
        last_shown_pts.store(pts);
        if (force) {
            awaiting.store(false);
            if (paused.load()) {
                paused_pos = pts;
            } else if (mode.load() == ClockMode::Wall) {
                wall_base = pts;
                wall_t0   = Clock::now();
            }
        } else if (paused.load()) {
            paused_pos = pts;
        }
    }

    bool is_ended() const {
        if (!eof.load())
            return false;
        std::lock_guard l(q_mu);
        const auto ser = serial.load();
        for (const auto& f : q)
            if (f.serial == ser)
                return false;
        return !cmd_flag.load();
    }

    // ------------------------------------------------------------------
    // Commands to the decode thread
    // ------------------------------------------------------------------

    void post_cmd(Cmd c) {
        {
            std::lock_guard l(cmd_mu);
            c.seq = serial.fetch_add(1) + 1;  // everything queued so far is now stale
            cmd   = c;
        }
        cmd_flag.store(true);
        room_cv.notify_all();
    }

    bool take_cmd(Cmd& out) {
        if (!cmd_flag.load())
            return false;
        std::lock_guard l(cmd_mu);
        if (!cmd) {
            cmd_flag.store(false);
            return false;
        }
        out = *cmd;
        cmd.reset();
        eof.store(false);  // before clearing the flag, so is_ended() can't glitch true
        cmd_flag.store(false);
        return true;
    }

    // ------------------------------------------------------------------
    // Audio ring (producer: decode thread, consumer: audio callback)
    // ------------------------------------------------------------------

    void ring_flush(double base) {
        std::lock_guard l(ring_mu);
        r_rd     = 0;
        r_count  = 0;
        r_played = 0;
        r_base   = base;
    }

    // Appends `frames` of audio that starts at media time `pts`, aligned to the
    // clock: gaps are filled with silence and the part of a late chunk that is
    // already in the past is skipped. Returns how many frames of the chunk were
    // consumed (written or skipped); less than `frames` means the ring is full.
    std::size_t ring_write(const float* data, std::size_t frames, double pts) {
        std::lock_guard l(ring_mu);
        constexpr std::size_t ch = kAudioChannels;
        std::size_t consumed     = 0;

        const double expected =
            r_base + static_cast<double>(r_played + static_cast<std::int64_t>(r_count)) / kAudioRate;
        const double diff = pts - expected;
        if (diff < -kAudioAlignTolerance) {
            const auto skip = std::min(frames, static_cast<std::size_t>(-diff * kAudioRate));
            consumed        = skip;
            data += skip * ch;
            frames -= skip;
            if (frames == 0)
                return consumed;
        } else if (diff > 10.0) {
            return frames;  // implausible jump; drop the chunk rather than pad for ages
        } else if (diff > kAudioAlignTolerance) {
            const auto gap = static_cast<std::size_t>(diff * kAudioRate);
            const auto pad = std::min(gap, kRingFrames - r_count);
            ring_put(nullptr, pad);
            if (pad < gap)
                return consumed;  // ring full of padding; the rest is added on a later call
        }
        const std::size_t n = std::min(kRingFrames - r_count, frames);
        ring_put(data, n);
        return consumed + n;
    }

    // ring_mu held. `data == nullptr` appends silence.
    void ring_put(const float* data, std::size_t n) {
        constexpr std::size_t ch = kAudioChannels;
        const std::size_t wr     = (r_rd + r_count) % kRingFrames;
        const std::size_t first  = std::min(n, kRingFrames - wr);
        if (data) {
            std::memcpy(&ring[wr * ch], data, first * ch * sizeof(float));
            if (n > first)
                std::memcpy(&ring[0], data + first * ch, (n - first) * ch * sizeof(float));
        } else {
            std::memset(&ring[wr * ch], 0, first * ch * sizeof(float));
            if (n > first)
                std::memset(&ring[0], 0, (n - first) * ch * sizeof(float));
        }
        r_count += n;
    }

    void fill_audio(float* out, std::size_t frames) {
        if (paused.load(std::memory_order_relaxed) || awaiting.load(std::memory_order_relaxed) ||
            mode.load(std::memory_order_relaxed) != ClockMode::Audio) {
            std::memset(out, 0, frames * kAudioChannels * sizeof(float));
            return;
        }
        std::size_t got = 0;
        {
            std::lock_guard l(ring_mu);
            got                     = std::min(frames, r_count);
            const std::size_t first = std::min(got, kRingFrames - r_rd);
            std::memcpy(out, &ring[r_rd * kAudioChannels], first * kAudioChannels * sizeof(float));
            if (got > first)
                std::memcpy(out + first * kAudioChannels, &ring[0], (got - first) * kAudioChannels * sizeof(float));
            r_rd = (r_rd + got) % kRingFrames;
            r_count -= got;
            r_played += static_cast<std::int64_t>(frames);  // device time passes even when we had to play silence
        }
        if (got < frames)
            std::memset(out + got * kAudioChannels, 0, (frames - got) * kAudioChannels * sizeof(float));
        if (got)
            room_cv.notify_all();
    }

    static void audio_cb(ma_device* dev, void* out, const void* /*in*/, ma_uint32 frames) {
        static_cast<Impl*>(dev->pUserData)->fill_audio(static_cast<float*>(out), frames);
    }

    // ------------------------------------------------------------------
    // Decode thread
    // ------------------------------------------------------------------

    void run(std::stop_token stop) {
        if (!open_input()) {
            phase.store(Phase::Failed);
            return;
        }
        if (info_local.animated_image && vst->nb_frames == 1) {
            phase.store(Phase::NotPlayable);  // a one-frame GIF is just an image
            return;
        }
        setup_audio();

        {
            std::lock_guard l(info_mu);
            info            = info_local;
            info->has_audio = audio_active.load();
        }

        // Start exactly like a seek to 0 so the first frame is shown
        // immediately and the wall clock (if used) starts from it.
        {
            std::lock_guard l(clk_mu);
            mode.store(audio_active.load() ? ClockMode::Audio : ClockMode::Wall);
            wall_base  = 0.0;
            wall_t0    = Clock::now();
            paused_pos = 0.0;
            if (!paused.load()) {
                awaiting  = true;
                await_pos = 0.0;
            }
        }
        ring_flush(0.0);
        seeking     = true;
        seek_target = 0.0;
        phase.store(Phase::Active);

        while (!stop.stop_requested()) {
            Cmd c;
            if (take_cmd(c)) {
                do_seek(c);
                continue;
            }
            drain_backlog();
            if (!wait_for_room(stop))
                break;
            if (cmd_flag.load())
                continue;
            if (eof_local) {
                std::unique_lock l(q_mu);
                room_cv.wait_for(l, std::chrono::milliseconds(20));
                continue;
            }

            const int rc = av_read_frame(fmt.get(), pkt.get());
            if (rc < 0) {
                on_eof();
                continue;
            }
            if (pkt->stream_index == vidx)
                decode_video_packet(pkt.get());
            else if (actx && audio_active.load() && pkt->stream_index == aidx)
                decode_audio_packet(pkt.get());
            av_packet_unref(pkt.get());
        }
    }

    bool open_input() {
        init_ffmpeg_logging();
        AVFormatContext* raw = nullptr;
        int rc               = avformat_open_input(&raw, path_to_utf8(path).c_str(), nullptr, nullptr);
        if (rc < 0) {
            butil::log.error("Cannot open '{}': {}", path_to_utf8(path), av_err(rc));
            return false;
        }
        fmt.reset(raw);
        rc = avformat_find_stream_info(fmt.get(), nullptr);
        if (rc < 0) {
            butil::log.error("No stream info for '{}': {}", path_to_utf8(path), av_err(rc));
            return false;
        }

        vidx = av_find_best_stream(fmt.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (vidx < 0) {
            butil::log.error("'{}' has no video stream", path_to_utf8(path));
            return false;
        }
        vst = fmt->streams[vidx];
        if (vst->disposition & AV_DISPOSITION_ATTACHED_PIC)
            return false;  // cover art, not video
        aidx = av_find_best_stream(fmt.get(), AVMEDIA_TYPE_AUDIO, -1, vidx, nullptr, 0);

        vctx = open_decoder(vst, /*threaded=*/true);
        if (!vctx)
            return false;
        out_w = vctx->width > 0 ? vctx->width : vst->codecpar->width;
        out_h = vctx->height > 0 ? vctx->height : vst->codecpar->height;
        if (!rgba_byte_size(out_w, out_h)) {
            butil::log.error("'{}': unknown or unsupported video size {}x{}", path_to_utf8(path), out_w, out_h);
            return false;
        }
        if (vst->codecpar->codec_id != AV_CODEC_ID_GIF)  // GIF's pixel-aspect byte isn't honoured by anyone
            apply_sar(out_w, out_h, av_guess_sample_aspect_ratio(fmt.get(), vst, nullptr));

        pkt.reset(av_packet_alloc());
        frm.reset(av_frame_alloc());
        afrm.reset(av_frame_alloc());
        seek_cand.reset(av_frame_alloc());
        if (!pkt || !frm || !afrm || !seek_cand)
            return false;

        start_offset = fmt->start_time != AV_NOPTS_VALUE ? static_cast<double>(fmt->start_time) / AV_TIME_BASE : 0.0;
        double dur   = 0.0;
        if (fmt->duration > 0)
            dur = static_cast<double>(fmt->duration) / AV_TIME_BASE;
        else if (vst->duration > 0)
            dur = static_cast<double>(vst->duration) * av_q2d(vst->time_base);
        duration.store(dur);

        const AVRational fr = av_guess_frame_rate(fmt.get(), vst, nullptr);
        const double fps    = fr.num > 0 && fr.den > 0 ? av_q2d(fr) : 0.0;
        frame_dur.store(fps > 0.0 ? 1.0 / fps : 1.0 / 25.0);
        max_q = std::clamp<std::size_t>(
            kFrameQueueBytes / (static_cast<std::size_t>(out_w) * static_cast<std::size_t>(out_h) * 4), 3, 8);

        info_local.width          = out_w;
        info_local.height         = out_h;
        info_local.duration       = dur;
        info_local.fps            = fps;
        info_local.container      = fmt->iformat && fmt->iformat->name ? fmt->iformat->name : "";
        info_local.video_codec    = avcodec_get_name(vst->codecpar->codec_id);
        // GIF and WebP: looping, silent images. A still WebP never gets here (the
        // controller checks the VP8X animation flag first), and a one-frame one
        // becomes NotPlayable in run() rather than an endlessly "playing" video.
        info_local.animated_image =
            vst->codecpar->codec_id == AV_CODEC_ID_GIF || vst->codecpar->codec_id == AV_CODEC_ID_WEBP;
        loop_enabled.store(info_local.animated_image);
        return true;
    }

    void setup_audio() {
        if (!opt.allow_audio || aidx < 0)
            return;
        AVStream* ast = fmt->streams[aidx];
        actx          = open_decoder(ast, /*threaded=*/false);
        if (!actx)
            return;
        atb                    = av_q2d(ast->time_base);
        info_local.audio_codec = avcodec_get_name(ast->codecpar->codec_id);

        ma_device_config cfg  = ma_device_config_init(ma_device_type_playback);
        cfg.playback.format   = ma_format_f32;
        cfg.playback.channels = kAudioChannels;
        cfg.sampleRate        = kAudioRate;
        cfg.dataCallback      = &Impl::audio_cb;
        cfg.pUserData         = this;
        if (ma_device_init(nullptr, &cfg, &device) != MA_SUCCESS) {
            butil::log.warn("No audio output device; playing without sound");
            actx.reset();
            return;
        }
        ma_device_set_master_volume(&device, volume.load() * volume.load());
        if (ma_device_start(&device) != MA_SUCCESS) {
            butil::log.warn("Could not start audio device; playing without sound");
            ma_device_uninit(&device);
            actx.reset();
            return;
        }
        audio_ready = true;
        audio_active.store(true);
    }

    std::int64_t to_stream_ts(const AVStream* st, double seconds) const {
        return static_cast<std::int64_t>(std::llround(seconds / av_q2d(st->time_base)));
    }

    bool wait_for_room(const std::stop_token& stop) {
        std::unique_lock l(q_mu);
        while (!stop.stop_requested()) {
            if (cmd_flag.load())
                return true;
            if (q.size() < max_q && backlog_frames < kBacklogLimitFrames)
                return true;
            room_cv.wait_for(l, std::chrono::milliseconds(5));
            l.unlock();
            drain_backlog();
            l.lock();
        }
        return false;
    }

    void do_seek(const Cmd& c) {
        double local;
        if (c.kind == Cmd::Kind::Abs) {
            loop_offset  = 0.0;
            last_end_pts = 0.0;
            local        = std::max(0.0, c.t);
        } else {
            local = std::max(0.0, c.t - loop_offset);
        }

        avcodec_flush_buffers(vctx.get());
        if (actx)
            avcodec_flush_buffers(actx.get());
        swr.reset();
        backlog.clear();
        backlog_frames = 0;

        const std::int64_t ts = to_stream_ts(vst, local + start_offset);
        int rc                = av_seek_frame(fmt.get(), vidx, ts, AVSEEK_FLAG_BACKWARD);
        if (rc < 0)
            rc = avformat_seek_file(fmt.get(), vidx, INT64_MIN, ts, INT64_MAX, 0);
        if (rc < 0)
            butil::log.warn("Seek failed: {}", av_err(rc));

        dec_serial = c.seq;
        {
            std::lock_guard l(q_mu);
            q.clear();
        }
        seek_target = local + loop_offset;
        ring_flush(seek_target);
        audio_skip_until = seek_target;
        av_frame_unref(seek_cand.get());
        seeking   = true;
        eof_local = false;
        {
            std::lock_guard l(clk_mu);
            mode.store(audio_active.load() ? ClockMode::Audio : ClockMode::Wall);
        }
    }

    // ---- video ----

    void decode_video_packet(const AVPacket* p) {
        const int rc = avcodec_send_packet(vctx.get(), p);
        if (rc < 0 && rc != AVERROR(EAGAIN) && rc != AVERROR_EOF) {
            butil::log.warn("Video decode error: {}", av_err(rc));
            return;
        }
        drain_video_frames();
    }

    void drain_video_frames() {
        for (;;) {
            const int rc = avcodec_receive_frame(vctx.get(), frm.get());
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF)
                return;
            if (rc < 0) {
                butil::log.warn("Video decode error: {}", av_err(rc));
                return;
            }
            handle_video_frame(frm.get());
            av_frame_unref(frm.get());
            if (cmd_flag.load())
                return;  // a seek is pending; everything in flight is stale
        }
    }

    void handle_video_frame(AVFrame* f) {
        std::int64_t ts = f->best_effort_timestamp;
        if (ts == AV_NOPTS_VALUE)
            ts = f->pts;
        const double vtb = av_q2d(vst->time_base);
        double dur       = f->duration > 0 ? static_cast<double>(f->duration) * vtb : 0.0;
        if (dur > 0.0)
            frame_dur.store(dur);
        else
            dur = frame_dur.load();
        const double pts =
            ts == AV_NOPTS_VALUE ? next_pts_guess : static_cast<double>(ts) * vtb - start_offset + loop_offset;
        next_pts_guess = pts + dur;

        if (!seeking) {
            emit(f, pts, dur, false);
            return;
        }
        if (pts <= seek_target + 1e-4) {  // still before/at the target: remember, keep looking
            av_frame_unref(seek_cand.get());
            av_frame_ref(seek_cand.get(), f);
            seek_cand_pts = pts;
            seek_cand_dur = dur;
            return;
        }
        // First frame past the target: the candidate (last frame <= target) is the result.
        if (seek_cand->data[0]) {
            emit(seek_cand.get(), seek_cand_pts, seek_cand_dur, true);
            av_frame_unref(seek_cand.get());
            emit(f, pts, dur, false);
        } else {
            emit(f, pts, dur, true);
        }
        seeking = false;
    }

    void finish_seek_at_eof() {
        if (seek_cand->data[0])
            emit(seek_cand.get(), seek_cand_pts, seek_cand_dur, true);
        av_frame_unref(seek_cand.get());
        seeking = false;
    }

    void emit(const AVFrame* f, double pts, double dur, bool force) {
        QFrame qf;
        qf.serial    = dec_serial;
        qf.force     = force;
        qf.frame.pts = pts;
        {
            std::lock_guard l(pool_mu);
            if (!pool.empty()) {
                qf.frame.image.pixels = std::move(pool.back());
                pool.pop_back();
            }
        }
        if (!conv.convert(f, out_w, out_h, qf.frame.image))
            return;
        last_end_pts = std::max(last_end_pts, pts + dur);
        std::lock_guard l(q_mu);
        q.push_back(std::move(qf));
    }

    void on_eof() {
        avcodec_send_packet(vctx.get(), nullptr);
        drain_video_frames();
        if (actx && audio_active.load()) {
            avcodec_send_packet(actx.get(), nullptr);
            drain_audio_frames();
        }
        if (seeking)
            finish_seek_at_eof();
        if (cmd_flag.load())
            return;

        if (loop_enabled.load() && last_end_pts > 0.0) {
            // Gapless loop: rewind the file but keep timestamps increasing.
            loop_offset = last_end_pts;
            avcodec_flush_buffers(vctx.get());
            if (av_seek_frame(fmt.get(), vidx, to_stream_ts(vst, start_offset), AVSEEK_FLAG_BACKWARD) >= 0)
                return;
            loop_enabled.store(false);
        }
        eof_local = true;
        eof.store(true);
    }

    // ---- audio ----

    void decode_audio_packet(const AVPacket* p) {
        const int rc = avcodec_send_packet(actx.get(), p);
        if (rc < 0 && rc != AVERROR(EAGAIN) && rc != AVERROR_EOF)
            return;
        drain_audio_frames();
    }

    void drain_audio_frames() {
        while (audio_active.load() && avcodec_receive_frame(actx.get(), afrm.get()) == 0) {
            handle_audio_frame(afrm.get());
            av_frame_unref(afrm.get());
        }
    }

    // Builds the resampler (anything -> stereo float at kAudioRate). Inputs are
    // validated and set one option at a time: swr_alloc_set_opts2 only ever
    // reports a bare "Failed to set option", which says nothing useful.
    bool ensure_swr(const AVFrame* f) {
        if (swr)
            return true;
        const int rate = f->sample_rate > 0 ? f->sample_rate : actx->sample_rate;
        const int fmt =
            (f->format >= 0 && f->format < AV_SAMPLE_FMT_NB) ? f->format : static_cast<int>(actx->sample_fmt);

        // Input layout: the frame's, else the decoder's, else a default for the channel count.
        AVChannelLayout in_layout{};
        auto usable = [](const AVChannelLayout& l) {
            return l.nb_channels > 0 && av_channel_layout_check(&l) > 0;
        };
        if (usable(f->ch_layout))
            av_channel_layout_copy(&in_layout, &f->ch_layout);
        else if (usable(actx->ch_layout))
            av_channel_layout_copy(&in_layout, &actx->ch_layout);
        else
            av_channel_layout_default(
                &in_layout, std::max(1, std::max(f->ch_layout.nb_channels, actx->ch_layout.nb_channels)));
        AVChannelLayout out_layout{};
        av_channel_layout_default(&out_layout, kAudioChannels);

        char desc[128] = {};
        av_channel_layout_describe(&in_layout, desc, sizeof desc);

        SwrContext* s      = swr_alloc();
        int rc             = s ? 0 : AVERROR(ENOMEM);
        const char* failed = nullptr;
        auto step          = [&](const char* name, int r) {
            if (r < 0 && !failed) {
                failed = name;
                rc     = r;
            }
        };
        if (s) {
            step("in_chlayout", av_opt_set_chlayout(s, "in_chlayout", &in_layout, 0));
            step("out_chlayout", av_opt_set_chlayout(s, "out_chlayout", &out_layout, 0));
            step("in_sample_rate", av_opt_set_int(s, "in_sample_rate", rate, 0));
            step("out_sample_rate", av_opt_set_int(s, "out_sample_rate", kAudioRate, 0));
            step("in_sample_fmt", av_opt_set_sample_fmt(s, "in_sample_fmt", static_cast<AVSampleFormat>(fmt), 0));
            step("out_sample_fmt", av_opt_set_sample_fmt(s, "out_sample_fmt", AV_SAMPLE_FMT_FLT, 0));
            if (!failed)
                step("swr_init", swr_init(s));
        }
        av_channel_layout_uninit(&in_layout);
        av_channel_layout_uninit(&out_layout);
        if (failed) {
            swr_free(&s);
            disable_audio(std::format("resampler option '{}' rejected ({}): layout '{}', {} Hz, sample format {}",
                failed,
                av_err(rc),
                desc,
                rate,
                fmt));
            return false;
        }
        if (!s) {
            disable_audio("out of memory");
            return false;
        }
        swr.reset(s);
        return true;
    }

    // Gives up on sound (once) but keeps the video playing on the wall clock.
    void disable_audio(const std::string& why) {
        if (!audio_active.exchange(false))
            return;
        butil::log.warn("Playing without sound: {}", why);
        if (audio_ready)
            ma_device_stop(&device);
        backlog.clear();
        backlog_frames = 0;
        std::lock_guard l(clk_mu);
        if (mode.load() == ClockMode::Audio) {  // continue from where the audio clock was
            wall_base = paused.load() ? paused_pos : awaiting.load() ? await_pos : running_pos_locked();
            wall_t0   = Clock::now();
            mode.store(ClockMode::Wall);
        }
    }

    void handle_audio_frame(const AVFrame* f) {
        if (!audio_active.load() || !ensure_swr(f))
            return;
        const int max_out = std::max(1, swr_get_out_samples(swr.get(), f->nb_samples));
        std::vector<float> buf(static_cast<std::size_t>(max_out) * kAudioChannels);
        std::uint8_t* out[1] = {reinterpret_cast<std::uint8_t*>(buf.data())};
        // `const uint8_t**` in FFmpeg <= 6, `const uint8_t* const*` in newer ones.
        const std::uint8_t* const* in_data = f->extended_data;
        const int got = swr_convert(swr.get(), out, max_out, const_cast<const std::uint8_t**>(in_data), f->nb_samples);
        if (got <= 0)
            return;
        buf.resize(static_cast<std::size_t>(got) * kAudioChannels);

        double pts =
            f->pts != AV_NOPTS_VALUE ? static_cast<double>(f->pts) * atb - start_offset + loop_offset : audio_cursor;
        audio_cursor = pts + static_cast<double>(got) / kAudioRate;

        if (!std::isnan(audio_skip_until)) {  // trim audio that precedes a seek target
            const double end = pts + static_cast<double>(got) / kAudioRate;
            if (end <= audio_skip_until)
                return;
            if (pts < audio_skip_until) {
                const auto skip = static_cast<std::size_t>((audio_skip_until - pts) * kAudioRate);
                buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(skip * kAudioChannels));
                pts = audio_skip_until;
            }
            audio_skip_until = std::numeric_limits<double>::quiet_NaN();
        }
        backlog_frames += buf.size() / kAudioChannels;
        backlog.push_back({std::move(buf), 0, pts});
        drain_backlog();
    }

    void drain_backlog() {
        while (!backlog.empty()) {
            AudioChunk& c           = backlog.front();
            const std::size_t total = c.samples.size() / kAudioChannels;
            const std::size_t left  = total - c.offset;
            const std::size_t wrote = ring_write(
                c.samples.data() + c.offset * kAudioChannels, left, c.pts + static_cast<double>(c.offset) / kAudioRate);
            c.offset += wrote;
            backlog_frames -= wrote;
            if (c.offset < total)
                break;  // ring is full
            backlog.pop_front();
        }
    }

    // ------------------------------------------------------------------
    // State
    // ------------------------------------------------------------------

    fs::path path;
    Options opt;

    std::atomic<Phase> phase{Phase::Opening};
    mutable std::mutex info_mu;
    std::optional<StreamInfo> info;
    StreamInfo info_local;  // decode thread

    // FFmpeg (decode thread only after construction)
    FormatPtr fmt;
    CodecPtr vctx, actx;
    PacketPtr pkt;
    FramePtr frm, afrm, seek_cand;
    SwrPtr swr;
    RgbaConverter conv;
    AVStream* vst = nullptr;
    int vidx = -1, aidx = -1;
    int out_w = 0, out_h = 0;
    double start_offset = 0.0;
    double atb          = 0.0;
    std::atomic<double> duration{0.0};
    std::atomic<double> frame_dur{1.0 / 25.0};
    std::atomic<bool> loop_enabled{false};
    std::size_t max_q = 6;

    // decode-thread-only playback state
    bool seeking         = false;
    double seek_target   = 0.0;
    double seek_cand_pts = 0.0, seek_cand_dur = 0.0;
    double next_pts_guess    = 0.0;
    double loop_offset       = 0.0;
    double last_end_pts      = 0.0;
    double audio_cursor      = 0.0;
    double audio_skip_until  = std::numeric_limits<double>::quiet_NaN();
    bool eof_local           = false;
    std::uint32_t dec_serial = 0;  // serial of the command being served (frames are stamped with it)
    std::deque<AudioChunk> backlog;
    std::size_t backlog_frames = 0;

    // shared flags
    std::atomic<bool> eof{false};
    std::atomic<bool> audio_active{false};  // output device running and audio decoding OK
    std::atomic<bool> paused{false};
    std::atomic<float> volume{1.0f};
    std::atomic<int> step_pending{0};
    std::atomic<double> last_shown_pts{0.0};
    std::atomic<std::uint32_t> serial{0};  // latest posted command; frames with another serial are stale

    // frame queue
    mutable std::mutex q_mu;
    std::deque<QFrame> q;
    std::condition_variable room_cv;  // waited on with q_mu
    std::mutex pool_mu;
    std::vector<std::vector<std::uint8_t>> pool;

    // commands
    std::mutex cmd_mu;
    std::optional<Cmd> cmd;
    std::atomic<bool> cmd_flag{false};

    // audio
    ma_device device{};
    bool audio_ready = false;
    std::mutex ring_mu;
    std::vector<float> ring;
    std::size_t r_rd = 0, r_count = 0;
    std::int64_t r_played = 0;
    double r_base         = 0.0;

    // clock (clk_mu)
    std::mutex clk_mu;
    std::atomic<ClockMode> mode{ClockMode::Wall};
    double wall_base          = 0.0;
    Clock::time_point wall_t0 = Clock::now();
    double paused_pos         = 0.0;
    std::atomic<bool> awaiting{false};  // seek issued while playing; clock frozen until its frame is shown
    double await_pos   = 0.0;
    bool resync_needed = false;

    std::jthread worker;  // last: starts in the constructor
};

// ===========================================================================
// VideoPlayer (thin pimpl forwarding)
// ===========================================================================

VideoPlayer::VideoPlayer(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

VideoPlayer::~VideoPlayer() = default;

std::unique_ptr<VideoPlayer> VideoPlayer::open(fs::path path, Options opt) {
    return std::unique_ptr<VideoPlayer>(new VideoPlayer(std::make_unique<Impl>(std::move(path), opt)));
}

State VideoPlayer::state() const {
    return m_impl->state();
}

std::optional<StreamInfo> VideoPlayer::info() const {
    return m_impl->get_info();
}

bool VideoPlayer::audio_available() const {
    return m_impl->audio_available();
}

double VideoPlayer::position() const {
    return m_impl->position();
}

bool VideoPlayer::paused() const {
    return m_impl->paused.load();
}

std::optional<Frame> VideoPlayer::poll_frame() {
    return m_impl->poll_frame();
}

void VideoPlayer::recycle(Frame&& frame) {
    m_impl->recycle(std::move(frame));
}

void VideoPlayer::set_paused(bool paused) {
    m_impl->set_paused(paused);
}

void VideoPlayer::toggle_pause() {
    // From "ended", play restarts; otherwise flip the flag.
    m_impl->set_paused(m_impl->state() == State::Ended ? false : !m_impl->paused.load());
}

void VideoPlayer::seek(double seconds) {
    m_impl->seek(seconds);
}

void VideoPlayer::step(int direction) {
    m_impl->step(direction);
}

void VideoPlayer::set_volume(float volume) {
    m_impl->set_volume(volume);
}

// ===========================================================================
// Poster frame
// ===========================================================================

std::optional<colors::RgbaBuffer> decode_poster_frame(const fs::path& path, int max_dim) {
    init_ffmpeg_logging();
    AVFormatContext* raw = nullptr;
    if (avformat_open_input(&raw, path_to_utf8(path).c_str(), nullptr, nullptr) < 0)
        return std::nullopt;
    FormatPtr fmt(raw);
    if (avformat_find_stream_info(fmt.get(), nullptr) < 0)
        return std::nullopt;
    const int vidx = av_find_best_stream(fmt.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vidx < 0)
        return std::nullopt;
    const AVStream* st = fmt->streams[vidx];
    CodecPtr ctx       = open_decoder(st, /*threaded=*/false);
    if (!ctx)
        return std::nullopt;

    // Skip a possible black fade-in on longer clips. (The time base is
    // read from the file; only divide by it if it is usable.)
    const double time_base = av_q2d(st->time_base);
    if (fmt->duration > 4 * AV_TIME_BASE && time_base > 0.0) {
        const std::int64_t start  = fmt->start_time != AV_NOPTS_VALUE ? fmt->start_time : 0;
        const std::int64_t target = start + fmt->duration / 10;
        const std::int64_t ts =
            static_cast<std::int64_t>(std::llround(static_cast<double>(target) / AV_TIME_BASE / time_base));
        if (av_seek_frame(fmt.get(), vidx, ts, AVSEEK_FLAG_BACKWARD) >= 0)
            avcodec_flush_buffers(ctx.get());
    }

    PacketPtr pkt(av_packet_alloc());
    FramePtr frame(av_frame_alloc());
    if (!pkt || !frame)
        return std::nullopt;

    bool flushed = false;
    for (int packets_read = 0;;) {
        const int rc = avcodec_receive_frame(ctx.get(), frame.get());
        if (rc == 0) {
            int w = frame->width, h = frame->height;
            if (!rgba_byte_size(w, h))  // decoder-reported size: don't trust it
                return std::nullopt;
            if (st->codecpar->codec_id != AV_CODEC_ID_GIF)
                apply_sar(w, h, av_guess_sample_aspect_ratio(fmt.get(), const_cast<AVStream*>(st), frame.get()));

            // Thumbnails are scaled by swscale straight to their final size, so a
            // 4K/8K frame is never expanded to a full-size RGBA image first.
            const auto size = max_dim > 0 ? channel::thumbnail_size(w, h, max_dim) : channel::ThumbSize{w, h};
            RgbaConverter conv;
            colors::RgbaBuffer out;
            if (!conv.convert(frame.get(), size.width, size.height, out, max_dim > 0 ? SWS_AREA : SWS_BILINEAR))
                return std::nullopt;
            return out;
        }
        if (rc != AVERROR(EAGAIN) || flushed || ++packets_read > kPosterMaxPackets)
            return std::nullopt;
        if (av_read_frame(fmt.get(), pkt.get()) < 0) {
            avcodec_send_packet(ctx.get(), nullptr);
            flushed = true;
            continue;
        }
        if (pkt->stream_index == vidx)
            avcodec_send_packet(ctx.get(), pkt.get());
        av_packet_unref(pkt.get());
    }
}

}  // namespace biv::video
