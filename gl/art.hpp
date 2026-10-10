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
#include "http.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
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
    wall,     // 2:3, 216x324: the posters drifting behind the sign-in, made at the size they are drawn (moving, a
              // picture the GPU makes smaller shimmers); cut from the card's poster
};

// The address a picture is fetched from: a host's sharper copy of the same picture where it has one (metahub's 500 x 750
// poster for its 300 x 450 one).
std::string better_url(const std::string &url, ArtKind kind);

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
    // Makes sure the picture is ready on disk (downloaded, cut and stored), without using GPU memory: for pictures
    // that will probably be wanted soon (the wallpapers around the cursor, the posters of the start screen). They
    // wait behind everything get() asked for. When get() asks for it later, it is a quick read from disk.
    void warm(const std::string &url, ArtKind kind);
    // The texture if it is already on the GPU, without asking for it.
    std::uint32_t peek(const std::string &url, ArtKind kind) const;
    // Whether the picture is already here: on the GPU, or made and kept on disk (what warm() fetches).
    bool cached(const std::string &url, ArtKind kind) const;

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
        int priority = 0;       // 0 wanted on screen now, 1 ahead of time (warm)
        bool upload = true;     // the picture goes to the GPU when done (a warm one only to disk)
        double not_before = 0;  // a retry waits until then
        int attempts = 0;
        std::string host;
    };
    enum class Made
    {
        ok,
        retry,  // the host could not be reached, timed out or was busy: try again a little later
        failed, // not there or not a picture: give up
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
    Made make(const Job &job, Ready *out, HttpSession &http, bool *unreachable);
    Made fetch(const Job &job, HttpSession &http, bool *unreachable, std::vector<unsigned char> &src, int &w, int &h);
    int choose(double now) const; // the next job for a worker (mutex_ held), -1 if none can start yet

    std::string dir_;
    hui::gfx::Renderer *renderer_ = nullptr;
    std::map<std::string, Entry> entries_; // GL thread only
    std::set<std::string> warmed_;         // GL thread only: asked to warm once already
    std::size_t bytes_ = 0;
    std::uint64_t clock_ = 0;
    int uploads_logged_ = 0;
    bool upload_compared_ = false;
    int big_uploads_ = 0;
    bool direct_faster_ = false; // measured once on this console (pump)

    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<Job> queue_; // newest last: served first (within a priority)
    std::vector<Ready> ready_;
    std::map<std::string, bool> inflight_;          // keys being made, and whether the result goes to the GPU
    std::map<std::string, int> host_active_;        // jobs running per host
    std::map<std::string, double> host_down_until_; // a host that could not be reached is left alone until then
    std::vector<std::thread> threads_;
    bool stopping_ = false;
};

} // namespace sx
