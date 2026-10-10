#include "art.hpp"

#include "gpu_upload.hpp"
#include "http.h"
#include "util.h"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

// The picture codecs are private to this file.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#include "stb_image.h"
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#ifdef HAVE_WEBP
#include <webp/decode.h>
#endif

namespace sx
{

namespace
{

constexpr int kPosterW = 480, kPosterH = 720;
// A wallpaper is cut between these widths: never narrower than a 1080p screen (a smaller picture is
// scaled up here, with a good filter, not left to the GPU), and no wider than 2880 (a 4K picture is
// 33 MB on the GPU, 2880 is 19 MB and still sharp on a 4K television).
constexpr int kBackdropMin = 1920, kBackdropMax = 2880;
constexpr int kThumbW = 320;
constexpr int kIconSize = 192;
constexpr int kQrMin = 400;
constexpr int kWallW = 216; // the sign-in's wall draws its posters 216 x 324 (ui/overlays.cpp)

// PNG and JPEG through stb_image; WebP (some addons serve posters as it) through libwebp.
bool decode(const std::string &data, std::vector<unsigned char> &px, int &w, int &h)
{
    const unsigned char *bytes = reinterpret_cast<const unsigned char *>(data.data());
#ifdef HAVE_WEBP
    if (data.size() >= 12 && std::memcmp(bytes, "RIFF", 4) == 0 && std::memcmp(bytes + 8, "WEBP", 4) == 0)
    {
        uint8_t *out = WebPDecodeRGBA(bytes, data.size(), &w, &h);
        if (!out)
            return false;
        px.assign(out, out + static_cast<std::size_t>(w) * h * 4);
        WebPFree(out);
        return true;
    }
#endif
    int comp = 0;
    unsigned char *out = stbi_load_from_memory(bytes, static_cast<int>(data.size()), &w, &h, &comp, 4);
    if (!out)
        return false;
    px.assign(out, out + static_cast<std::size_t>(w) * h * 4);
    stbi_image_free(out);
    return true;
}

// ---- Lanczos-3 resampling (premultiplied, so transparent edges do not fringe) ----

float lanczos3(float x)
{
    x = std::fabs(x);
    if (x < 1e-6f)
        return 1.0f;
    if (x >= 3.0f)
        return 0.0f;
    const float px = 3.14159265f * x;
    return 3.0f * std::sin(px) * std::sin(px / 3.0f) / (px * px);
}

struct Taps
{
    int first = 0;
    std::vector<float> w;
};

std::vector<Taps> make_taps(int n_out, int n_in, float start, float span)
{
    const float scale = span / static_cast<float>(n_out), wide = std::max(1.0f, scale);
    std::vector<Taps> taps(static_cast<std::size_t>(n_out));
    for (int i = 0; i < n_out; ++i)
    {
        const float c = start + (static_cast<float>(i) + 0.5f) * scale - 0.5f;
        int lo = std::max(static_cast<int>(std::floor(c - 3 * wide)) + 1, 0);
        const int hi = std::min(static_cast<int>(std::floor(c + 3 * wide)), n_in - 1);
        Taps &t = taps[static_cast<std::size_t>(i)];
        if (hi < lo)
        {
            lo = std::min(std::max(static_cast<int>(c + 0.5f), 0), n_in - 1);
            t.first = lo;
            t.w.assign(1, 1.0f);
            continue;
        }
        t.first = lo;
        t.w.resize(static_cast<std::size_t>(hi - lo + 1));
        float sum = 0;
        for (int k = lo; k <= hi; ++k)
            sum += t.w[static_cast<std::size_t>(k - lo)] = lanczos3((static_cast<float>(k) - c) / wide);
        if (sum <= 0)
        {
            t.w.assign(t.w.size(), 1.0f);
            sum = static_cast<float>(t.w.size());
        }
        for (float &v : t.w)
            v /= sum;
    }
    return taps;
}

// The source rectangle (sx, sy, sw, sh) into dw x dh.
void resample(const unsigned char *src, int src_w, int src_h, float sx, float sy, float sw, float sh,
              unsigned char *dst, int dw, int dh)
{
    const std::vector<Taps> xt = make_taps(dw, src_w, sx, sw), yt = make_taps(dh, src_h, sy, sh);
    int row0 = src_h, row1 = -1;
    for (const Taps &t : yt)
    {
        row0 = std::min(row0, t.first);
        row1 = std::max(row1, t.first + static_cast<int>(t.w.size()) - 1);
    }
    std::vector<float> mid(static_cast<std::size_t>(row1 - row0 + 1) * dw * 4);
    for (int r = row0; r <= row1; ++r)
    {
        const unsigned char *line = src + static_cast<std::size_t>(r) * src_w * 4;
        float *out = &mid[static_cast<std::size_t>(r - row0) * dw * 4];
        for (int x = 0; x < dw; ++x, out += 4)
        {
            const Taps &t = xt[static_cast<std::size_t>(x)];
            float acc[4] = {0, 0, 0, 0};
            const unsigned char *s = line + static_cast<std::size_t>(t.first) * 4;
            for (std::size_t k = 0; k < t.w.size(); ++k, s += 4)
            {
                const float a = static_cast<float>(s[3]) * (1.0f / 255.0f), w = t.w[k];
                acc[0] += s[0] * a * w;
                acc[1] += s[1] * a * w;
                acc[2] += s[2] * a * w;
                acc[3] += a * w;
            }
            std::memcpy(out, acc, sizeof(acc));
        }
    }
    for (int y = 0; y < dh; ++y)
    {
        const Taps &t = yt[static_cast<std::size_t>(y)];
        for (int x = 0; x < dw; ++x)
        {
            float acc[4] = {0, 0, 0, 0};
            for (std::size_t k = 0; k < t.w.size(); ++k)
            {
                const float *m = &mid[(static_cast<std::size_t>(t.first + static_cast<int>(k) - row0) * dw + x) * 4];
                const float w = t.w[k];
                acc[0] += m[0] * w;
                acc[1] += m[1] * w;
                acc[2] += m[2] * w;
                acc[3] += m[3] * w;
            }
            unsigned char *d = dst + (static_cast<std::size_t>(y) * dw + x) * 4;
            const float a = std::min(std::max(acc[3], 0.0f), 1.0f);
            if (a > 1e-4f)
            {
                const float inv = 1.0f / a;
                for (int c = 0; c < 3; ++c)
                    d[c] = static_cast<unsigned char>(std::min(std::max(acc[c] * inv, 0.0f) + 0.5f, 255.0f));
            }
            else
            {
                d[0] = d[1] = d[2] = 0;
            }
            d[3] = static_cast<unsigned char>(a * 255.0f + 0.5f);
        }
    }
}

// Fills the aspect aw:ah from the middle of the picture, between min_w and max_w wide (with min_w 0 a
// small picture is not scaled up: it is left to the GPU's smooth scaling).
void cover(const std::vector<unsigned char> &src, int w, int h, int aw, int ah, int max_w, std::vector<unsigned char> &out,
           int &ow, int &oh, int min_w = 0)
{
    float sx = 0, sy = 0, sw = static_cast<float>(w), sh = static_cast<float>(h);
    const float target = static_cast<float>(aw) / static_cast<float>(ah), aspect = sw / sh;
    if (aspect > target)
    {
        sw = sh * target;
        sx = (static_cast<float>(w) - sw) * 0.5f;
    }
    else
    {
        sh = sw / target;
        sy = (static_cast<float>(h) - sh) * 0.5f;
    }
    ow = std::max(std::max(1, min_w), std::min(max_w, static_cast<int>(sw)));
    oh = std::max(1, static_cast<int>(std::lround(static_cast<float>(ow) * static_cast<float>(ah) / static_cast<float>(aw))));
    out.resize(static_cast<std::size_t>(ow) * oh * 4);
    resample(src.data(), w, h, sx, sy, sw, sh, out.data(), ow, oh);
}

// A picture that was scaled up has lost its edges: a mild unsharp mask (a [1 2 1] blur, taken off
// the picture and a part of it put back) brings them back without a halo.
void sharpen(std::vector<unsigned char> &px, int w, int h, float amount)
{
    std::vector<unsigned char> blur(px.size());
    std::vector<unsigned char> tmp(px.size());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            const int xl = std::max(0, x - 1), xr = std::min(w - 1, x + 1);
            for (int c = 0; c < 3; ++c)
            {
                const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4 + static_cast<std::size_t>(c);
                tmp[i] = static_cast<unsigned char>((px[(static_cast<std::size_t>(y) * w + xl) * 4 + static_cast<std::size_t>(c)] +
                                                      2 * px[i] +
                                                      px[(static_cast<std::size_t>(y) * w + xr) * 4 + static_cast<std::size_t>(c)] + 2) /
                                                     4);
            }
        }
    for (int y = 0; y < h; ++y)
    {
        const int yu = std::max(0, y - 1), yd = std::min(h - 1, y + 1);
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c)
            {
                const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4 + static_cast<std::size_t>(c);
                blur[i] = static_cast<unsigned char>((tmp[(static_cast<std::size_t>(yu) * w + x) * 4 + static_cast<std::size_t>(c)] +
                                                       2 * tmp[i] +
                                                       tmp[(static_cast<std::size_t>(yd) * w + x) * 4 + static_cast<std::size_t>(c)] + 2) /
                                                      4);
            }
    }
    for (std::size_t i = 0; i < px.size(); i += 4)
        for (int c = 0; c < 3; ++c)
        {
            const float v = static_cast<float>(px[i + static_cast<std::size_t>(c)]);
            const float b = static_cast<float>(blur[i + static_cast<std::size_t>(c)]);
            px[i + static_cast<std::size_t>(c)] = static_cast<unsigned char>(std::clamp(v + amount * (v - b), 0.0f, 255.0f));
        }
}

// The whole picture inside a square on transparency.
void fit_square(const std::vector<unsigned char> &src, int w, int h, int size, std::vector<unsigned char> &out)
{
    const float aspect = static_cast<float>(w) / static_cast<float>(h);
    int fw = size, fh = size;
    if (aspect > 1.0f)
        fh = std::max(1, static_cast<int>(std::lround(static_cast<float>(size) / aspect)));
    else
        fw = std::max(1, static_cast<int>(std::lround(static_cast<float>(size) * aspect)));
    std::vector<unsigned char> fitted(static_cast<std::size_t>(fw) * fh * 4);
    resample(src.data(), w, h, 0, 0, static_cast<float>(w), static_cast<float>(h), fitted.data(), fw, fh);
    out.assign(static_cast<std::size_t>(size) * size * 4, 0);
    const int ox = (size - fw) / 2, oy = (size - fh) / 2;
    for (int y = 0; y < fh; ++y)
        std::memcpy(&out[(static_cast<std::size_t>(y + oy) * size + ox) * 4], &fitted[static_cast<std::size_t>(y) * fw * 4],
                    static_cast<std::size_t>(fw) * 4);
}

void to_string_sink(void *context, void *data, int size)
{
    static_cast<std::string *>(context)->append(static_cast<const char *>(data), static_cast<std::size_t>(size));
}

// How far apart two pictures are (0..255): the average difference of their brightness on a 32 x 18 grid of areas
// (each cell the mean of the pixels in it). Single pixels (as this once compared) are thrown by grain and
// compression noise: two copies of the same wallpaper measured 4.9 and 5.6 that way and 0.1 by areas, while
// different artwork measures 30 and more either way.
float luma_distance(const std::vector<unsigned char> &a, int aw, int ah, const std::vector<unsigned char> &b, int bw, int bh)
{
    const auto cells = [](const std::vector<unsigned char> &p, int w, int h, float *out) {
        double sum[32 * 18] = {};
        int count[32 * 18] = {};
        const int step = std::max(1, w / 640); // a few hundred thousand pixels are plenty for the means
        for (int y = 0; y < h; y += step)
        {
            const int cy = std::min(17, y * 18 / h);
            const unsigned char *row = &p[static_cast<std::size_t>(y) * w * 4];
            for (int x = 0; x < w; x += step)
            {
                const int c = cy * 32 + std::min(31, x * 32 / w);
                const unsigned char *q = row + static_cast<std::size_t>(x) * 4;
                sum[c] += (q[0] * 299 + q[1] * 587 + q[2] * 114) / 1000.0;
                ++count[c];
            }
        }
        for (int c = 0; c < 32 * 18; ++c)
            out[c] = count[c] ? static_cast<float>(sum[c] / count[c]) : 0.0f;
    };
    float ca[32 * 18], cb[32 * 18];
    cells(a, aw, ah, ca);
    cells(b, bw, bh, cb);
    float d = 0;
    for (int c = 0; c < 32 * 18; ++c)
        d += std::fabs(ca[c] - cb[c]);
    return d / (32.0f * 18.0f);
}

// The image host keeps a bigger copy of many wallpapers: the "medium" one is often 1280 wide, the
// "big" one 3840. Not always the same picture, though: for some titles "big" is other artwork (a poster-like
// still with the name in it), so it is only taken when it is the picture we already have, bigger.
//
// When the medium copy is already 1920 wide it is not asked: on 36 popular titles the big copy was then never any
// bigger (14 of 14), so it was 1-2 MB downloaded for nothing.
void use_bigger_copy(const std::string &url, std::vector<unsigned char> &src, int &w, int &h, HttpSession &http)
{
    static const std::string kMedium = "/background/medium/";
    const std::size_t at = url.find(kMedium);
    if (at == std::string::npos || w >= kBackdropMin)
        return;
    std::string big_url = url;
    big_url.replace(at, kMedium.size(), "/background/big/");
    const HttpResponse r = http.get(big_url, 40);
    if (!r.ok())
        return;
    std::vector<unsigned char> big;
    int bw = 0, bh = 0;
    if (!decode(r.body, big, bw, bh) || bw <= w || bh <= 0)
        return;
    if (luma_distance(src, w, h, big, bw, bh) < 3.0f)
    {
        src.swap(big);
        w = bw;
        h = bh;
    }
}

bool opaque_kind(ArtKind kind)
{
    return kind == ArtKind::poster || kind == ArtKind::backdrop || kind == ArtKind::thumb || kind == ArtKind::wall;
}

const char *suffix(ArtKind kind)
{
    switch (kind)
    {
    case ArtKind::poster:
        return "p";
    case ArtKind::backdrop:
        return "b2"; // (b: the first cuts, 1920 wide at most)
    case ArtKind::thumb:
        return "t";
    case ArtKind::icon:
        return "i";
    case ArtKind::qr:
        return "q";
    case ArtKind::wall:
        return "w";
    }
    return "x";
}

} // namespace

bool TexCache::init(const std::string &dir, hui::gfx::Renderer *renderer, int workers)
{
    dir_ = dir;
    renderer_ = renderer;
    gpu::prepare(); // the GL thread: the copy shader is ready before the first picture
    make_dirs(dir_);
    stopping_ = false;
    for (int i = 0; i < workers; ++i)
        threads_.emplace_back([this] { worker(); });
    return true;
}

void TexCache::shutdown()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        queue_.clear();
    }
    cv_.notify_all();
    for (std::thread &t : threads_)
        t.join();
    threads_.clear();
}

// A better copy of the same picture where the host has one. metahub's "small" poster is 300 x 450; a card on a 4K
// screen is about 464 pixels wide, so it was stretched by half again and looked soft. Its "medium" one is 500 x 750
// and is cut to 480 x 720, drawn at about its own size.
std::string better_url(const std::string &url, ArtKind kind)
{
    static const std::string kSmall = "metahub.space/poster/small/";
    if (kind != ArtKind::poster && kind != ArtKind::wall)
        return url;
    const std::size_t at = url.find(kSmall);
    if (at == std::string::npos)
        return url;
    std::string out = url;
    out.replace(at, kSmall.size(), "metahub.space/poster/medium/");
    return out;
}

std::string host_of(const std::string &url)
{
    const std::size_t a = url.find("://");
    const std::size_t from = a == std::string::npos ? 0 : a + 3;
    return url.substr(from, url.find('/', from) - from);
}

std::string TexCache::key_for(const std::string &url, ArtKind kind) const
{
    char name[64];
    std::snprintf(name, sizeof(name), "/%016llx%s.%s", static_cast<unsigned long long>(fnv1a(url)), suffix(kind),
                  opaque_kind(kind) ? "jpg" : "png");
    return dir_ + name;
}

std::uint32_t TexCache::get(const std::string &url0, ArtKind kind)
{
    if (url0.empty() || (!starts_with(url0, "http://") && !starts_with(url0, "https://")))
        return 0;
    const std::string url = better_url(url0, kind);
    const std::string key = key_for(url, kind);
    Entry &e = entries_[key];
    e.used = clock_;
    if (e.id != 0)
        return e.id;
    if (e.pending || e.failed)
        return 0;
    e.pending = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Already on its way to disk (warm): it goes on to the GPU, and to the front.
        bool found = false;
        for (Job &j : queue_)
            if (j.key == key)
            {
                j.upload = true;
                j.priority = 0;
                found = true;
            }
        auto in = inflight_.find(key);
        if (!found && in != inflight_.end())
        {
            in->second = true;
            found = true;
        }
        if (!found)
        {
            Job job;
            job.url = url;
            job.key = key;
            job.kind = kind;
            job.host = host_of(url);
            queue_.push_back(job);
        }
    }
    cv_.notify_one();
    return 0;
}

void TexCache::warm(const std::string &url0, ArtKind kind)
{
    if (url0.empty() || (!starts_with(url0, "http://") && !starts_with(url0, "https://")))
        return;
    const std::string url = better_url(url0, kind);
    const std::string key = key_for(url, kind);
    const auto it = entries_.find(key);
    if (it != entries_.end() && (it->second.id != 0 || it->second.pending || it->second.failed))
        return;
    if (!warmed_.insert(key).second)
        return; // asked before: on disk, or on its way
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Job job;
        job.url = url;
        job.key = key;
        job.kind = kind;
        job.priority = 1;
        job.upload = false;
        job.host = host_of(url);
        queue_.push_back(job);
    }
    cv_.notify_one();
}

bool TexCache::cached(const std::string &url0, ArtKind kind) const
{
    const std::string key = key_for(better_url(url0, kind), kind);
    const auto it = entries_.find(key);
    return (it != entries_.end() && it->second.id != 0) || file_exists(key);
}

std::uint32_t TexCache::peek(const std::string &url, ArtKind kind) const
{
    const auto it = entries_.find(key_for(better_url(url, kind), kind));
    return it == entries_.end() ? 0 : it->second.id;
}

bool TexCache::pump(int max_uploads)
{
    ++clock_;
    std::vector<Ready> batch;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::size_t take = std::min<std::size_t>(ready_.size(), static_cast<std::size_t>(max_uploads));
        batch.assign(std::make_move_iterator(ready_.begin()), std::make_move_iterator(ready_.begin() + static_cast<long>(take)));
        ready_.erase(ready_.begin(), ready_.begin() + static_cast<long>(take));
    }
    for (Ready &r : batch)
    {
        Entry &e = entries_[r.key];
        e.pending = false;
        if (r.pixels.empty())
        {
            e.failed = true;
            continue;
        }
        // Through the GPU (gpu_upload.hpp): the frame does not wait for the driver to tile the picture.
        const double t0 = now_seconds();
        const char *how = "through the GPU";
        e.id = direct_faster_ ? 0 : gpu::texture_from_rgba(r.w, r.h, r.pixels.data());
        if (e.id == 0)
        {
            e.id = renderer_->batch().create_texture(r.w, r.h, r.pixels.data());
            how = "directly";
        }
        const double ms = (now_seconds() - t0) * 1000.0;
        if (uploads_logged_ < 3 || ms > 25.0)
        {
            ++uploads_logged_;
            dlog("art: a %dx%d picture went up %s in %.1f ms", r.w, r.h, how, ms);
        }
        // Once, for the record: what the usual upload of the same big picture costs on this console.
        if (!upload_compared_ && r.w * r.h >= 1920 * 1080 && how[0] == 't' && ++big_uploads_ >= 2)
        {
            upload_compared_ = true;
            const double t1 = now_seconds();
            const std::uint32_t test = renderer_->batch().create_texture(r.w, r.h, r.pixels.data());
            const double direct = (now_seconds() - t1) * 1000.0;
            glDeleteTextures(1, &test);
            // Measured, not assumed: if the usual upload is clearly the faster one on this console, it is used from now on.
            direct_faster_ = direct < ms * 0.8;
            dlog("art: the same %dx%d picture uploaded directly took %.1f ms (through the GPU %.1f ms): %s", r.w, r.h, direct, ms,
                 direct_faster_ ? "uploading directly from now on" : "keeping the GPU copy");
        }
        e.bytes = r.pixels.size();
        e.used = clock_;
        bytes_ += e.bytes;
    }
    return !batch.empty();
}

bool TexCache::trim(std::size_t budget)
{
    if (bytes_ <= budget)
        return false;
    bool deleted = false;
    std::vector<std::map<std::string, Entry>::iterator> loaded;
    for (auto it = entries_.begin(); it != entries_.end(); ++it)
        if (it->second.id != 0 && it->second.used + 60 < clock_)
            loaded.push_back(it);
    std::sort(loaded.begin(), loaded.end(), [](const auto &a, const auto &b) { return a->second.used < b->second.used; });
    for (auto &it : loaded)
    {
        if (bytes_ <= budget)
            break;
        const GLuint id = it->second.id;
        glDeleteTextures(1, &id);
        bytes_ -= it->second.bytes;
        entries_.erase(it);
        deleted = true;
    }
    return deleted;
}

// What a worker takes next: what is wanted on screen before what is fetched ahead, the newest first, nothing that
// waits for a retry or for its host to come back, and no more than 4 of the 6 workers on one host while another
// host has work waiting (a slow or dead host must not hold up the rest, as a browser limits connections per host).
int TexCache::choose(double now) const
{
    constexpr int kPerHost = 4;
    int best = -1, best_any = -1;
    for (int i = static_cast<int>(queue_.size()) - 1; i >= 0; --i)
    {
        const Job &j = queue_[static_cast<std::size_t>(i)];
        if (j.not_before > now)
            continue;
        const auto down = host_down_until_.find(j.host);
        if (down != host_down_until_.end() && down->second > now)
            continue;
        if (best_any < 0 || j.priority < queue_[static_cast<std::size_t>(best_any)].priority)
            best_any = i;
        const auto active = host_active_.find(j.host);
        if (active != host_active_.end() && active->second >= kPerHost)
            continue;
        if (best < 0 || j.priority < queue_[static_cast<std::size_t>(best)].priority)
            best = i;
    }
    return best >= 0 ? best : best_any;
}

void TexCache::worker()
{
    HttpSession http; // this worker's connections stay open between pictures
    for (;;)
    {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            for (;;)
            {
                if (stopping_)
                    return;
                const double now = now_seconds();
                const int pick = choose(now);
                if (pick >= 0)
                {
                    job = std::move(queue_[static_cast<std::size_t>(pick)]);
                    queue_.erase(queue_.begin() + pick);
                    break;
                }
                // Nothing can start yet: wait for new work, or for the earliest retry.
                double next = 0;
                for (const Job &j : queue_)
                {
                    double at = j.not_before;
                    const auto down = host_down_until_.find(j.host);
                    if (down != host_down_until_.end())
                        at = std::max(at, down->second);
                    if (next == 0 || at < next)
                        next = at;
                }
                if (next > now)
                    cv_.wait_for(lock, std::chrono::milliseconds(static_cast<long>(std::min(5.0, next - now) * 1000.0) + 1));
                else
                    cv_.wait(lock);
            }
            inflight_[job.key] = job.upload;
            ++host_active_[job.host];
        }
        Ready r;
        r.key = job.key;
        bool unreachable = false;
        const Made made = make(job, &r, http, &unreachable);
        std::unique_lock<std::mutex> lock(mutex_);
        --host_active_[job.host];
        const bool upload = inflight_[job.key];
        inflight_.erase(job.key);
        if (made == Made::retry && job.attempts + 1 < 4)
        {
            // Again in 3, then 10, then 30 seconds; a host that could not be reached at all rests for 30 seconds.
            static const double kWait[] = {3.0, 10.0, 30.0};
            const double now = now_seconds();
            if (unreachable)
                host_down_until_[job.host] = now + 30.0;
            job.not_before = now + kWait[job.attempts];
            ++job.attempts;
            job.upload = upload;
            if (upload)
                job.priority = 0;
            queue_.push_back(std::move(job));
            lock.unlock();
            cv_.notify_all();
            continue;
        }
        if (!upload)
            continue; // to disk only: nothing for the GPU
        if (made != Made::ok)
            r.pixels.clear();
        else if (r.pixels.empty())
        {
            // It was only checked for on disk (a warm job that get() asked for meanwhile): read it now.
            lock.unlock();
            std::string data;
            if (!read_file(job.key, data) || !decode(data, r.pixels, r.w, r.h))
                r.pixels.clear();
            lock.lock();
        }
        ready_.push_back(std::move(r));
    }
}

TexCache::Made TexCache::make(const Job &job, Ready *out, HttpSession &http, bool *unreachable)
{
    if (!job.upload && file_exists(job.key))
        return Made::ok; // warm, and already on disk
    std::string data;
    if (read_file(job.key, data) && decode(data, out->pixels, out->w, out->h))
        return Made::ok; // already cut: use as it is

    std::vector<unsigned char> src;
    int w = 0, h = 0;
    if (job.kind == ArtKind::wall)
    {
        // From the card's poster: the one on disk, or fetched and cut now (and kept, so the card does not fetch it).
        Job card = job;
        card.kind = ArtKind::poster;
        card.key = key_for(job.url, ArtKind::poster);
        card.upload = true;
        Ready poster;
        const Made made = make(card, &poster, http, unreachable);
        if (made != Made::ok)
            return made;
        src = std::move(poster.pixels);
        w = poster.w;
        h = poster.h;
    }
    else if (const Made made = fetch(job, http, unreachable, src, w, h); made != Made::ok)
    {
        return made;
    }
    if (job.kind == ArtKind::backdrop)
        use_bigger_copy(job.url, src, w, h, http);

    std::vector<unsigned char> px;
    int ow = 0, oh = 0;
    switch (job.kind)
    {
    case ArtKind::poster:
        cover(src, w, h, 2, 3, kPosterW, px, ow, oh);
        break;
    case ArtKind::wall:
        cover(src, w, h, 2, 3, kWallW, px, ow, oh);
        break;
    case ArtKind::backdrop:
    {
        cover(src, w, h, 16, 9, kBackdropMax, px, ow, oh, kBackdropMin);
        // Was it made bigger? Then its edges need a little help.
        const float cropped_w = std::min(static_cast<float>(w), static_cast<float>(h) * 16.0f / 9.0f);
        if (static_cast<float>(ow) > cropped_w * 1.1f)
            sharpen(px, ow, oh, 0.45f);
        break;
    }
    case ArtKind::thumb:
        cover(src, w, h, 16, 9, kThumbW, px, ow, oh);
        break;
    case ArtKind::icon:
        fit_square(src, w, h, kIconSize, px);
        ow = oh = kIconSize;
        break;
    case ArtKind::qr:
    {
        const int step = std::max(1, (kQrMin + w - 1) / w);
        ow = w * step;
        oh = h * step;
        px.resize(static_cast<std::size_t>(ow) * oh * 4);
        for (int y = 0; y < oh; ++y)
            for (int x = 0; x < ow; ++x)
                std::memcpy(&px[(static_cast<std::size_t>(y) * ow + x) * 4], &src[(static_cast<std::size_t>(y / step) * w + x / step) * 4], 4);
        break;
    }
    }

    std::string file;
    if (opaque_kind(job.kind))
        stbi_write_jpg_to_func(to_string_sink, &file, ow, oh, 4, px.data(), job.kind == ArtKind::backdrop ? 93 : 90);
    else
        stbi_write_png_to_func(to_string_sink, &file, ow, oh, 4, px.data(), ow * 4);
    if (!file.empty())
        write_file(job.key, file);
    out->pixels = std::move(px);
    out->w = ow;
    out->h = oh;
    return Made::ok;
}

// The picture itself, from its host, decoded.
TexCache::Made TexCache::fetch(const Job &job, HttpSession &http, bool *unreachable, std::vector<unsigned char> &src,
                               int &w, int &h)
{
    const HttpResponse r = http.get(job.url, 30);
    if (!r.ok())
    {
        dlog("art: %s: %s", job.url.c_str(), r.describe().c_str());
        if (!r.error.empty())
        {
            // No answer: could not resolve, connect or finish in time. Worth another try later.
            const std::string e = lower(r.error);
            *unreachable = e.find("resolve") != std::string::npos || e.find("connect") != std::string::npos;
            return Made::retry;
        }
        // Busy or briefly broken (5xx, 408, 429): later. Not there (404, 403, 410...): no.
        return r.status >= 500 || r.status == 408 || r.status == 429 ? Made::retry : Made::failed;
    }
    if (!decode(r.body, src, w, h) || w <= 0 || h <= 0)
    {
        dlog("art: cannot decode %s", job.url.c_str());
        return Made::failed;
    }
    return Made::ok;
}

} // namespace sx
