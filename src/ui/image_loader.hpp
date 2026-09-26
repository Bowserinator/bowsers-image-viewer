#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <vector>

#include "core/source.hpp"
#include "image/image_document.hpp"
#include "processing/channel_ops.hpp"

namespace biv {

struct MainLoadResult {
    std::uint64_t seq        = 0;
    std::uint64_t folder_gen = 0;
    std::size_t index        = 0;
    std::optional<ImageDocument> doc;         // nullopt on decode failure
    std::optional<colors::RgbaBuffer> thumb;  // <=128px RGB downscale of the doc
};

struct ThumbResult {
    std::uint64_t folder_gen = 0;
    std::size_t index        = 0;
    std::optional<colors::RgbaBuffer> pixels;
};

struct ResizeResult {
    std::uint64_t seq = 0;
    std::optional<colors::RgbaBuffer> pixels;  // nullopt if the resample failed
};

// Background workers: one decodes the main-view image (plus its bundled thumb),
// a small pool handles thumbnail decodes (so one huge file can't hold up every
// thumbnail behind it), and one resamples the on-screen image down to its
// zoomed-out display size. Workers produce plain buffers only; all Slint
// objects are constructed on the UI thread when results are drained.
class ImageLoader {
public:
    ImageLoader();
    ~ImageLoader();

    // Replaces any not-yet-started main request; returns seq to match on drain.
    [[nodiscard]] std::uint64_t request_main(std::size_t index, const ImageSource& src, std::uint64_t folder_gen);

    // Queues a thumbnail decode. Returns false if one is already queued/in-flight,
    // or if `index` is outside the current thumb window (see set_thumb_window).
    bool request_thumb(std::size_t index, const ImageSource& src, std::uint64_t folder_gen, std::int32_t priority);

    // Rate limit: only queued (not in-flight) jobs inside [first, last) are
    // kept. Fast scrolling drops work that is no longer nearby so the thumb
    // workers aren't stuck on off-screen decodes. The UI is responsible
    // for re-requesting any visible holes (resize, missed scroll, prefetch).
    void set_thumb_window(std::size_t first, std::size_t last);

    // Queues a display-quality downscale of `source` to width x height,
    // replacing any not-yet-started one; returns the seq to match on drain.
    // `source` is shared so the pixels outlive whatever the UI does meanwhile.
    [[nodiscard]] std::uint64_t request_resize(
        std::shared_ptr<const colors::RgbaBuffer> source, int width, int height);

    // Drops a not-yet-started resize. An in-flight one finishes; callers
    // discard its result by seq.
    void cancel_resize();

    // Drops queued jobs + dedupe set. In-flight jobs finish; callers discard
    // their results via the folder-gen check.
    void clear();

    std::vector<MainLoadResult> take_main_results();
    std::vector<ThumbResult> take_thumb_results();
    std::vector<ResizeResult> take_resize_results();

private:
    struct MainRequest {
        std::uint64_t seq = 0;
        std::size_t index = 0;
        ImageSource source;
        std::uint64_t folder_gen = 0;
    };

    struct ThumbJob {
        std::int32_t priority = 0;
        std::size_t index     = 0;
        ImageSource source;
        std::uint64_t folder_gen = 0;
    };

    struct ResizeRequest {
        std::uint64_t seq = 0;
        std::shared_ptr<const colors::RgbaBuffer> source;
        int width  = 0;
        int height = 0;
    };

    void main_loop(std::stop_token stop);
    void thumb_loop(std::stop_token stop);
    void resize_loop(std::stop_token stop);

    std::mutex m_mutex;
    std::condition_variable m_main_cv;   // used by main_loop
    std::condition_variable m_thumb_cv;  // used by thumb_loop (all thumb workers)
    std::condition_variable m_resize_cv;  // used by resize_loop

    std::optional<MainRequest> m_main_request;
    std::uint64_t m_last_main_seq = 0;
    std::deque<ThumbJob> m_thumb_jobs;
    std::set<std::size_t> m_pending_thumbs;
    std::size_t m_thumb_win_lo = 0;
    std::size_t m_thumb_win_hi = 0;  // half-open [lo, hi)

    std::optional<ResizeRequest> m_resize_request;
    std::uint64_t m_last_resize_seq = 0;

    std::deque<MainLoadResult> m_main_done;
    std::deque<ThumbResult> m_thumb_done;
    std::deque<ResizeResult> m_resize_done;

    // Must stay LAST: threads start in the constructor and must only see fully
    // constructed state above; on destruction they are stopped/joined first.
    std::jthread m_main_worker;
    std::vector<std::jthread> m_thumb_workers;
    std::jthread m_resize_worker;
};

}  // namespace biv