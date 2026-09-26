#include "image_loader.hpp"

#include "butil/log.hpp"

#include <algorithm>
#include <exception>

#include "config.hpp"
#include "image/decode.hpp"
#include "image/resize.hpp"
#include "image/thumbnail.hpp"

namespace biv {

namespace {

// Thumbnail decodes are independent single-threaded jobs, so a few of them
// side by side is close to a linear speed-up on a folder of big files. Leave
// the rest of the CPU to the UI, the main-view decode and parallel_for().
std::size_t thumb_worker_count() {
    return std::clamp<std::size_t>(std::thread::hardware_concurrency() / 2, 2, kMAX_THUMB_WORKERS);
}

}  // namespace

ImageLoader::ImageLoader()
    : m_main_worker([this](std::stop_token st) { main_loop(st); }),
      m_resize_worker([this](std::stop_token st) { resize_loop(st); }) {
    const auto n = thumb_worker_count();
    m_thumb_workers.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
        m_thumb_workers.emplace_back([this](std::stop_token st) { thumb_loop(st); });
}

ImageLoader::~ImageLoader() {
    m_main_worker.request_stop();
    for (auto& w : m_thumb_workers)
        w.request_stop();
    m_resize_worker.request_stop();
    // Take the lock so a worker between its predicate check and wait() can't miss the wake-up.
    { std::lock_guard lk(m_mutex); }
    m_main_cv.notify_all();
    m_thumb_cv.notify_all();
    m_resize_cv.notify_all();
}

std::uint64_t ImageLoader::request_main(std::size_t index, const ImageSource& src, std::uint64_t folder_gen) {
    std::lock_guard lk(m_mutex);
    const auto seq = ++m_last_main_seq;
    m_main_request = MainRequest{seq, index, src, folder_gen};
    m_main_cv.notify_one();
    return seq;
}

bool ImageLoader::request_thumb(
    std::size_t index, const ImageSource& src, std::uint64_t folder_gen, std::int32_t priority) {
    std::lock_guard lk(m_mutex);
    if (index < m_thumb_win_lo || index >= m_thumb_win_hi)
        return false;  // not nearby; never enqueue
    if (m_pending_thumbs.count(index)) {
        // Re-request while still queued: raise its priority so thumbnails that
        // just became visible aren't stuck behind lower-priority work.
        for (auto& job : m_thumb_jobs) {
            if (job.index == index) {
                if (priority > job.priority)
                    job.priority = priority;
                break;
            }
        }
        return false;
    }
    m_pending_thumbs.insert(index);
    m_thumb_jobs.push_back(ThumbJob{priority, index, src, folder_gen});
    m_thumb_cv.notify_one();
    return true;
}

void ImageLoader::set_thumb_window(std::size_t first, std::size_t last) {
    std::lock_guard lk(m_mutex);
    m_thumb_win_lo = first;
    m_thumb_win_hi = last;
    std::erase_if(m_thumb_jobs, [&](const ThumbJob& j) {
        if (j.index >= first && j.index < last)
            return false;
        m_pending_thumbs.erase(j.index);
        return true;
    });
}

std::uint64_t ImageLoader::request_resize(std::shared_ptr<const colors::RgbaBuffer> source, int width, int height) {
    std::lock_guard lk(m_mutex);
    const auto seq  = ++m_last_resize_seq;
    m_resize_request = ResizeRequest{seq, std::move(source), width, height};
    m_resize_cv.notify_one();
    return seq;
}

void ImageLoader::cancel_resize() {
    std::lock_guard lk(m_mutex);
    m_resize_request.reset();
}

void ImageLoader::clear() {
    std::lock_guard lk(m_mutex);
    m_main_request.reset();
    m_resize_request.reset();
    m_thumb_jobs.clear();
    m_pending_thumbs.clear();
    m_thumb_win_lo = 0;
    m_thumb_win_hi = 0;
}

std::vector<MainLoadResult> ImageLoader::take_main_results() {
    std::lock_guard lk(m_mutex);
    std::vector<MainLoadResult> out;
    out.reserve(m_main_done.size());
    for (auto& r : m_main_done)
        out.push_back(std::move(r));
    m_main_done.clear();
    return out;
}

std::vector<ThumbResult> ImageLoader::take_thumb_results() {
    std::lock_guard lk(m_mutex);
    std::vector<ThumbResult> out;
    out.reserve(m_thumb_done.size());
    for (auto& r : m_thumb_done)
        out.push_back(std::move(r));
    m_thumb_done.clear();
    return out;
}

std::vector<ResizeResult> ImageLoader::take_resize_results() {
    std::lock_guard lk(m_mutex);
    std::vector<ResizeResult> out;
    out.reserve(m_resize_done.size());
    for (auto& r : m_resize_done)
        out.push_back(std::move(r));
    m_resize_done.clear();
    return out;
}

void ImageLoader::main_loop(std::stop_token stop) {
    while (!stop.stop_requested()) {
        std::optional<MainRequest> req;
        {
            std::unique_lock lk(m_mutex);
            m_main_cv.wait(lk, [&] { return stop.stop_requested() || m_main_request.has_value(); });
            if (stop.stop_requested())
                break;
            req = std::move(m_main_request);
            m_main_request.reset();
        }

        if (!req.has_value())
            continue;

        MainLoadResult out;
        out.seq        = req->seq;
        out.folder_gen = req->folder_gen;
        out.index      = req->index;

        // A worker that lets an exception escape takes the whole app down with
        // std::terminate (bad_alloc on a huge image, say): report it as a failed load.
        try {
            auto decoded = ImageDocument::load(req->source);
            if (decoded) {
                out.thumb = channel::make_thumbnail(decoded->view(channel::Mode::RGB));
                out.doc   = std::move(decoded);
            } else {
                butil::log.error("Async load failed for '{}'", display_name(req->source));
            }
        } catch (const std::exception& e) {
            out.doc.reset();
            out.thumb.reset();
            butil::log.error("Async load of '{}' threw: {}", display_name(req->source), e.what());
        }

        std::lock_guard lk(m_mutex);
        m_main_done.push_back(std::move(out));
    }
}

void ImageLoader::thumb_loop(std::stop_token stop) {
    while (!stop.stop_requested()) {
        std::optional<ThumbJob> job;
        {
            std::unique_lock lk(m_mutex);
            m_thumb_cv.wait(lk, [&] { return stop.stop_requested() || !m_thumb_jobs.empty(); });
            if (stop.stop_requested())
                break;
            auto best = m_thumb_jobs.begin();
            for (auto it = m_thumb_jobs.begin(); it != m_thumb_jobs.end(); ++it)
                if (it->priority > best->priority)
                    best = it;
            job = std::move(*best);
            m_thumb_jobs.erase(best);
        }

        ThumbResult out;
        out.folder_gen = job->folder_gen;
        out.index      = job->index;
        // Decode straight to thumbnail size (never the full image). Anything that
        // goes wrong -- unsupported/corrupt file, absurd dimensions, out of memory --
        // becomes a black tile so the strip stays consistent and the file isn't retried.
        try {
            out.pixels = decode_thumbnail(job->source, kDEFAULT_THUMBNAIL_MAX_DIM);
        } catch (const std::exception& e) {
            butil::log.error("Thumbnail for '{}' threw: {}", display_name(job->source), e.what());
        }
        if (!out.pixels)
            out.pixels = channel::make_black_thumbnail();

        std::lock_guard lk(m_mutex);
        m_pending_thumbs.erase(job->index);
        m_thumb_done.push_back(std::move(out));
    }
}

void ImageLoader::resize_loop(std::stop_token stop) {
    while (!stop.stop_requested()) {
        std::optional<ResizeRequest> req;
        {
            std::unique_lock lk(m_mutex);
            m_resize_cv.wait(lk, [&] { return stop.stop_requested() || m_resize_request.has_value(); });
            if (stop.stop_requested())
                break;
            req = std::move(m_resize_request);
            m_resize_request.reset();
        }
        if (!req)
            continue;

        ResizeResult out;
        out.seq = req->seq;
        try {
            out.pixels = channel::resize_rgba_sharp(*req->source, req->width, req->height);
        } catch (const std::exception& e) {
            butil::log.error("Display resize to {}x{} threw: {}", req->width, req->height, e.what());
        }
        req.reset();  // drop our share of the source pixels before publishing

        std::lock_guard lk(m_mutex);
        m_resize_done.push_back(std::move(out));
    }
}

}  // namespace biv