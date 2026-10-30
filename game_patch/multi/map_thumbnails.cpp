#include <algorithm>
#include <atomic>
#include <deque>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>
#include <stb_image.h>
#include <xlog/xlog.h>
#include <common/utils/string-utils.h>
#include "map_thumbnails.h"
#include "faction_files.h"
#include "../bmpman/bmpman.h"
#include "../rf/gameseq.h"
#include "../rf/gr/gr.h"

namespace
{
    // Bounding box of the stored preview: large enough for the demo browser's preview pane,
    // small enough that a screenful of levels stays cheap.
    constexpr int max_thumb_w = 480;
    constexpr int max_thumb_h = 270;

    struct Job
    {
        std::string level_filename;
        unsigned generation = 0;
        std::vector<unsigned char> encoded; // in: cached download (empty = fetch it); out: the download
        std::vector<uint32_t> argb;         // out: downscaled pixels, empty if there is no usable image
        int w = 0;
        int h = 0;
        std::atomic<bool> done = false;
    };

    struct Thumb
    {
        enum class State
        {
            idle, // never requested, or its bitmap was released
            queued,
            loading,
            ready,
            unavailable,
        };
        State state = State::idle;
        std::vector<unsigned char> encoded;
        int bm = -1;
        int w = 0;
        int h = 0;
    };

    // Keyed by lowercase level filename
    std::unordered_map<std::string, Thumb> g_thumbs;
    std::deque<std::string> g_queue;
    std::shared_ptr<Job> g_job;
    // Bumped by map_thumbnails_release_bitmaps so a job finishing afterwards doesn't build a bitmap
    unsigned g_generation = 0;

    void decode_downscaled(Job& job)
    {
        int src_w = 0, src_h = 0, channels = 0;
        unsigned char* pixels = stbi_load_from_memory(job.encoded.data(), static_cast<int>(job.encoded.size()),
            &src_w, &src_h, &channels, 4);
        if (!pixels) {
            xlog::warn("[MapThumb] failed to decode image for {}: {}", job.level_filename, stbi_failure_reason());
            return;
        }

        const float scale = std::min({1.0f, static_cast<float>(max_thumb_w) / src_w,
            static_cast<float>(max_thumb_h) / src_h});
        job.w = std::max(1, static_cast<int>(src_w * scale));
        job.h = std::max(1, static_cast<int>(src_h * scale));
        job.argb.resize(static_cast<size_t>(job.w) * job.h);

        // Box filter: each output pixel averages the source pixels it covers
        for (int y = 0; y < job.h; ++y) {
            const int sy0 = y * src_h / job.h;
            const int sy1 = std::max(sy0 + 1, (y + 1) * src_h / job.h);
            for (int x = 0; x < job.w; ++x) {
                const int sx0 = x * src_w / job.w;
                const int sx1 = std::max(sx0 + 1, (x + 1) * src_w / job.w);
                unsigned r = 0, g = 0, b = 0, a = 0;
                for (int sy = sy0; sy < sy1; ++sy) {
                    for (int sx = sx0; sx < sx1; ++sx) {
                        const unsigned char* p = pixels + (static_cast<size_t>(sy) * src_w + sx) * 4;
                        r += p[0];
                        g += p[1];
                        b += p[2];
                        a += p[3];
                    }
                }
                const unsigned n = static_cast<unsigned>((sy1 - sy0) * (sx1 - sx0));
                job.argb[static_cast<size_t>(y) * job.w + x] =
                    ((a / n) << 24) | ((r / n) << 16) | ((g / n) << 8) | (b / n);
            }
        }
        stbi_image_free(pixels);
    }

    void run_job(const std::shared_ptr<Job>& job)
    {
        try {
            if (job->encoded.empty()) {
                FactionFilesClient client;
                const auto info = client.find_map(job->level_filename.c_str());
                if (info && !info->image_url.empty()) {
                    job->encoded = client.fetch_image(info->image_url);
                }
            }
            if (!job->encoded.empty()) {
                decode_downscaled(*job);
            }
        }
        catch (const std::exception& e) {
            // find_map throws on a 404 for levels FactionFiles doesn't have
            xlog::debug("[MapThumb] no image for {}: {}", job->level_filename, e.what());
        }
        job->done.store(true, std::memory_order_release);
    }

    void release_bitmap(Thumb& thumb)
    {
        if (thumb.bm != -1) {
            // bm::release does not evict the D3D11 slot-keyed texture cache
            rf::gr::mark_texture_dirty(thumb.bm);
            rf::bm::release(thumb.bm);
            thumb.bm = -1;
        }
    }

    void pump()
    {
        if (g_job && g_job->done.load(std::memory_order_acquire)) {
            Thumb& thumb = g_thumbs[string_to_lower(g_job->level_filename)];
            thumb.encoded = std::move(g_job->encoded);
            if (g_job->argb.empty()) {
                thumb.state = Thumb::State::unavailable;
            }
            else if (g_job->generation != g_generation) {
                thumb.state = Thumb::State::idle;
            }
            else {
                const Job& job = *g_job;
                thumb.bm = bm_create_procedural(job.w, job.h,
                    [&](int x, int y) { return job.argb[static_cast<size_t>(y) * job.w + x]; });
                thumb.w = job.w;
                thumb.h = job.h;
                thumb.state = thumb.bm != -1 ? Thumb::State::ready : Thumb::State::unavailable;
            }
            g_job.reset();
        }

        while (!g_job && !g_queue.empty()) {
            const std::string key = std::move(g_queue.front());
            g_queue.pop_front();
            Thumb& thumb = g_thumbs[key];
            if (thumb.state != Thumb::State::queued) {
                continue;
            }
            thumb.state = Thumb::State::loading;
            g_job = std::make_shared<Job>();
            g_job->level_filename = key;
            g_job->generation = g_generation;
            g_job->encoded = std::move(thumb.encoded);
            // Detached so a slow request never blocks the game; the job is shared with the thread
            std::thread{run_job, g_job}.detach();
        }
    }
}

int map_thumbnail_get(const std::string& level_filename, int* out_w, int* out_h)
{
    if (level_filename.empty()) {
        return -1;
    }
    std::string key = string_to_lower(level_filename);
    Thumb& thumb = g_thumbs[key];
    if (thumb.state == Thumb::State::idle) {
        thumb.state = Thumb::State::queued;
        g_queue.push_back(std::move(key));
    }
    pump();
    if (thumb.state != Thumb::State::ready) {
        return -1;
    }
    *out_w = thumb.w;
    *out_h = thumb.h;
    return thumb.bm;
}

void map_thumbnails_release_bitmaps()
{
    ++g_generation;
    g_queue.clear();
    // The bitmap system may already be torn down while quitting
    const bool can_release = rf::gameseq_get_state() != rf::GS_QUIT_GAME;
    for (auto& [key, thumb] : g_thumbs) {
        if (can_release) {
            release_bitmap(thumb);
        }
        if (thumb.state == Thumb::State::ready || thumb.state == Thumb::State::queued) {
            thumb.state = Thumb::State::idle;
        }
    }
}
