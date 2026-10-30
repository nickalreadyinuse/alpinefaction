#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>
#include <windows.h>
#include <stb_image.h>
#include <xlog/xlog.h>
#include <common/utils/string-utils.h>
#include "map_thumbnails.h"
#include "faction_files.h"
#include "../bmpman/bmpman.h"
#include "../rf/file/file.h"
#include "../rf/gameseq.h"
#include "../rf/gr/gr.h"

namespace
{
    // Bounding box of the stored preview: large enough for the demo browser's preview pane,
    // small enough that a screenful of levels stays cheap.
    constexpr int max_thumb_w = 480;
    constexpr int max_thumb_h = 270;
    // Levels fetched at once
    constexpr int max_jobs = 3;
    // Matches FactionFilesClient::fetch_image's download cap
    constexpr std::uintmax_t max_cached_image_size = 8 * 1024 * 1024;
    // A level FactionFiles didn't have is asked about again after this long, in case it was added
    constexpr auto missing_recheck_age = std::chrono::hours{24 * 7};
    // Least recently used images are deleted past this; a cache hit refreshes the file's mtime
    constexpr std::uintmax_t max_cache_size = 512ull * 1024 * 1024;

    struct Job
    {
        std::string level_filename;
        std::string cache_dir; // empty = no disk cache
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
    std::vector<std::shared_ptr<Job>> g_jobs;
    std::optional<std::string> g_cache_dir; // resolved on first use; "" = unavailable
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

    // <rf_root>\screenshots\thumbnails, created on demand; "" if it can't be
    const std::string& cache_dir()
    {
        if (!g_cache_dir) {
            g_cache_dir.emplace();
            const std::string screenshots_dir = std::format("{}\\screenshots", rf::root_path);
            const std::string dir = screenshots_dir + "\\thumbnails";
            for (const std::string& path : {screenshots_dir, dir}) {
                if (!CreateDirectoryA(path.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
                    xlog::warn("[MapThumb] failed to create {}, error {}", path, GetLastError());
                    return *g_cache_dir;
                }
            }
            *g_cache_dir = dir;
        }
        return *g_cache_dir;
    }

    // Level filenames come from demo headers, so only plain filenames name a cache file
    bool is_cacheable_name(const std::string& level_filename)
    {
        return !level_filename.empty() && level_filename.size() <= 64
            && level_filename.find_first_of("\\/:*?\"<>|") == std::string::npos
            && level_filename.find("..") == std::string::npos;
    }

    bool read_file(const std::filesystem::path& path, std::vector<unsigned char>& out)
    {
        std::error_code ec;
        const auto size = std::filesystem::file_size(path, ec);
        if (ec || size == 0 || size > max_cached_image_size) {
            return false;
        }
        std::ifstream file{path, std::ios::binary};
        out.resize(static_cast<size_t>(size));
        if (!file.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size))) {
            out.clear();
            return false;
        }
        return true;
    }

    // Written under a temporary name and renamed, so an interrupted write never leaves a
    // truncated file under the real name
    void write_file(const std::filesystem::path& path, const std::vector<unsigned char>& data)
    {
        std::filesystem::path tmp_path = path;
        tmp_path += ".tmp";
        {
            std::ofstream file{tmp_path, std::ios::binary | std::ios::trunc};
            if (!file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()))) {
                xlog::warn("[MapThumb] failed to write {}", tmp_path.string());
                return;
            }
        }
        std::error_code ec;
        std::filesystem::rename(tmp_path, path, ec);
        if (ec) {
            xlog::warn("[MapThumb] failed to write {}: {}", path.string(), ec.message());
            std::filesystem::remove(tmp_path, ec);
        }
    }

    // Deletes the least recently used files until the cache fits max_cache_size. Runs after each
    // download, on the job threads, so it is serialized.
    void prune_cache(const std::string& dir)
    {
        static std::mutex mutex;
        const std::lock_guard lock{mutex};

        struct CachedFile
        {
            std::filesystem::path path;
            std::filesystem::file_time_type mtime;
            std::uintmax_t size;
        };
        std::vector<CachedFile> files;
        std::uintmax_t total = 0;
        std::error_code ec;
        for (const auto& item : std::filesystem::directory_iterator{dir, ec}) {
            std::error_code item_ec;
            if (!item.is_regular_file(item_ec)) {
                continue;
            }
            const auto size = item.file_size(item_ec);
            const auto mtime = item.last_write_time(item_ec);
            if (!item_ec) {
                files.push_back({item.path(), mtime, size});
                total += size;
            }
        }
        if (total <= max_cache_size) {
            return;
        }
        std::sort(files.begin(), files.end(), [](const CachedFile& a, const CachedFile& b) { return a.mtime < b.mtime; });
        for (const CachedFile& file : files) {
            if (total <= max_cache_size) {
                break;
            }
            if (std::filesystem::remove(file.path, ec)) {
                total -= file.size;
            }
        }
        xlog::debug("[MapThumb] pruned cache to {} bytes", total);
    }

    bool is_recent(const std::filesystem::path& path)
    {
        std::error_code ec;
        const auto mtime = std::filesystem::last_write_time(path, ec);
        return !ec && std::filesystem::file_time_type::clock::now() - mtime < missing_recheck_age;
    }

    void run_job(const std::shared_ptr<Job>& job)
    {
        // The cache holds the image as downloaded (<level>.img), or an empty <level>.none
        // when FactionFiles has no image for the level
        const bool use_cache = !job->cache_dir.empty() && is_cacheable_name(job->level_filename);
        const std::filesystem::path img_path = std::filesystem::path{job->cache_dir} / (job->level_filename + ".img");
        const std::filesystem::path none_path = std::filesystem::path{job->cache_dir} / (job->level_filename + ".none");
        bool from_cache = !job->encoded.empty();
        try {
            if (job->encoded.empty() && use_cache) {
                if (read_file(img_path, job->encoded)) {
                    from_cache = true;
                    std::error_code ec;
                    std::filesystem::last_write_time(img_path, std::filesystem::file_time_type::clock::now(), ec);
                }
                else {
                    from_cache = is_recent(none_path);
                }
            }
            if (!from_cache) {
                bool missing = false;
                try {
                    FactionFilesClient client;
                    const auto info = client.find_map(job->level_filename.c_str());
                    missing = !info || info->image_url.empty();
                    if (!missing) {
                        job->encoded = client.fetch_image(info->image_url);
                    }
                }
                catch (const std::exception& e) {
                    // find_map throws on a 404 for levels FactionFiles doesn't have; any other
                    // failure is left uncached so the next session tries again
                    missing = std::string_view{e.what()}.find("404") != std::string_view::npos;
                    xlog::debug("[MapThumb] no image for {}: {}", job->level_filename, e.what());
                }
                if (use_cache && !job->encoded.empty()) {
                    write_file(img_path, job->encoded);
                    prune_cache(job->cache_dir);
                }
                else if (use_cache && missing) {
                    write_file(none_path, {});
                }
            }
            if (!job->encoded.empty()) {
                decode_downscaled(*job);
                if (job->argb.empty() && from_cache && use_cache) {
                    // Undecodable cached file: drop it so the next session downloads it again
                    std::error_code ec;
                    std::filesystem::remove(img_path, ec);
                }
            }
        }
        catch (const std::exception& e) {
            xlog::warn("[MapThumb] failed to load image for {}: {}", job->level_filename, e.what());
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
        for (auto it = g_jobs.begin(); it != g_jobs.end();) {
            const Job& job = **it;
            if (!job.done.load(std::memory_order_acquire)) {
                ++it;
                continue;
            }
            Thumb& thumb = g_thumbs[job.level_filename];
            thumb.encoded = std::move((*it)->encoded);
            if (job.argb.empty()) {
                thumb.state = Thumb::State::unavailable;
            }
            else if (job.generation != g_generation) {
                thumb.state = Thumb::State::idle;
            }
            else {
                thumb.bm = bm_create_procedural(job.w, job.h,
                    [&](int x, int y) { return job.argb[static_cast<size_t>(y) * job.w + x]; });
                thumb.w = job.w;
                thumb.h = job.h;
                thumb.state = thumb.bm != -1 ? Thumb::State::ready : Thumb::State::unavailable;
            }
            it = g_jobs.erase(it);
        }

        while (static_cast<int>(g_jobs.size()) < max_jobs && !g_queue.empty()) {
            const std::string key = std::move(g_queue.front());
            g_queue.pop_front();
            Thumb& thumb = g_thumbs[key];
            if (thumb.state != Thumb::State::queued) {
                continue;
            }
            thumb.state = Thumb::State::loading;
            auto job = std::make_shared<Job>();
            job->level_filename = key;
            job->cache_dir = cache_dir();
            job->generation = g_generation;
            job->encoded = std::move(thumb.encoded);
            // Detached so a slow request never blocks the game; the job is shared with the thread
            std::thread{run_job, job}.detach();
            g_jobs.push_back(std::move(job));
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
