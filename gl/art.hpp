// Artwork as GL textures: downloaded (or read from the disk cache), cut to the
// shape it is drawn in, decoded on worker threads and uploaded on the thread
// that owns the GL context.
//
// Every picture is stored once on disk already cut and scaled (a JPEG for
// opaque pictures, a PNG where transparency matters), so the second visit costs
// a small decode instead of a download. The GPU draws rounded corners itself, so
// nothing is pre-rounded and one file serves every screen size.

#pragma once

#include "gfx/renderer.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sx
{

enum class ArtKind
{
    poster,   // 2:3, up to 480x720 (cards)
    backdrop, // 16:9, up to 1920x1080 (the hero, a title's page)
    thumb,    // 16:9, up to 320x180 (episodes)
    icon,     // 192x192, fitted on transparency (addon logos)
    qr,       // scaled by whole steps to at least 400 px, never smoothed
};

class TexCache
{
  public:
    ~TexCache()
    {
        shutdown();
    }

    // `dir` is where the processed pictures are kept.
    bool init(const std::string &dir, hui::gfx::Renderer *renderer, int workers);
    void shutdown();

    // The texture, or 0 while it is still coming (the download is queued, and
    // newer requests are served first). Call from the GL thread only.
    std::uint32_t get(const std::string &url, ArtKind kind);
    // The texture if it is already on the GPU, without asking for it.
    std::uint32_t peek(const std::string &url, ArtKind kind) const;

    // Once per frame on the GL thread: uploads up to max_uploads decoded
    // pictures. Returns true if any arrived.
    bool pump(int max_uploads);
    // Deletes the least recently asked-for textures until the cache is under
    // `budget` bytes. Textures asked for in the last second stay.
    // Returns true if it deleted any: the owners of those ids must look again.
    bool trim(std::size_t budget);
    std::size_t bytes() const
    {
        return bytes_;
    }

  private:
    struct Entry
    {
        std::uint32_t id = 0;
        std::size_t bytes = 0;
        std::uint64_t used = 0;
        bool pending = false;
        bool failed = false;
    };
    struct Job
    {
        std::string url;
        std::string key;
        ArtKind kind;
    };
    struct Ready
    {
        std::string key;
        int w = 0;
        int h = 0;
        std::vector<unsigned char> pixels; // empty when it failed
    };

    std::string key_for(const std::string &url, ArtKind kind) const;
    void worker();
    bool make(const Job &job, Ready *out);

    std::string dir_;
    hui::gfx::Renderer *renderer_ = nullptr;
    std::map<std::string, Entry> entries_; // GL thread only
    std::size_t bytes_ = 0;
    std::uint64_t clock_ = 0;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<Job> queue_; // newest last: served first
    std::vector<Ready> ready_;
    std::vector<std::thread> threads_;
    bool stopping_ = false;
};

} // namespace sx
