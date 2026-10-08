// Updating the app. The check asks GitHub for the newest release of the repository; on "Update now" the
// app downloads that release's image and hands it, piece by piece, to a helper program that the console's
// payload loader runs outside the sandbox (updater/main.cpp). The helper checks it and, once the app has
// closed, swaps it for the installed image in /data/homebrew.

#include "session.hpp"

#include "http.h"
#include "sha256.h"
#include "version.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace sx
{

// What the worker thread tells the screen.
struct Session::UpdateJob
{
    enum Phase
    {
        starting,
        downloading,
        installing,
        closing,
        failed,
        cancelled,
    };
    std::atomic<int> phase{starting};
    std::atomic<std::uint64_t> done{0}, total{0};
    std::atomic<bool> cancel{false};
    std::mutex lock;
    std::string error;
    double started = 0.0;
    double closing_since = 0.0;
};

namespace
{

std::vector<int> version_numbers(const std::string &text)
{
    std::vector<int> v;
    int cur = -1;
    for (const char c : text)
    {
        if (c >= '0' && c <= '9')
            cur = (cur < 0 ? 0 : cur * 10) + (c - '0');
        else
        {
            if (cur >= 0)
                v.push_back(cur);
            cur = -1;
            if (c == '-' || c == '+' || c == ' ')
                break;
        }
    }
    if (cur >= 0)
        v.push_back(cur);
    return v;
}

bool newer(const std::string &a, const std::string &b) // a is newer than b
{
    const std::vector<int> x = version_numbers(a), y = version_numbers(b);
    for (std::size_t i = 0; i < std::max(x.size(), y.size()); ++i)
    {
        const int p = i < x.size() ? x[i] : 0, q = i < y.size() ? y[i] : 0;
        if (p != q)
            return p > q;
    }
    return false;
}

std::string megabytes(std::uint64_t bytes)
{
    char t[32];
    std::snprintf(t, sizeof(t), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return t;
}

std::string time_left(double seconds)
{
    if (seconds < 3)
        return "a few seconds left";
    char t[48];
    if (seconds < 90)
        std::snprintf(t, sizeof(t), "about %d s left", static_cast<int>(seconds + 0.5));
    else
        std::snprintf(t, sizeof(t), "about %d min left", static_cast<int>(seconds / 60.0 + 0.5));
    return t;
}

// The lines of a release's text, without the markdown around them.
std::vector<std::string> note_lines(const std::string &body)
{
    std::vector<std::string> out;
    for (std::string line : split(body, '\n'))
    {
        line = trim(line);
        while (!line.empty() && (line[0] == '#' || line[0] == '-' || line[0] == '*' || line[0] == ' '))
            line.erase(0, 1);
        std::string clean;
        for (std::size_t i = 0; i < line.size(); ++i)
        {
            if (line[i] == '*' || line[i] == '`')
                continue;
            clean += line[i];
        }
        clean = trim(clean);
        if (!clean.empty())
            out.push_back(clean);
        if (out.size() >= 5)
            break;
    }
    return out;
}

bool send_all(int fd, const void *data, std::size_t size)
{
    const char *p = static_cast<const char *>(data);
    while (size > 0)
    {
        const ssize_t n = send(fd, p, size, 0);
        if (n <= 0)
            return false;
        p += n;
        size -= static_cast<std::size_t>(n);
    }
    return true;
}

bool read_line(int fd, std::string *out)
{
    out->clear();
    for (;;)
    {
        char c;
        const ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0)
            return false;
        if (c == '\n')
            return true;
        if (out->size() > 600)
            return false;
        *out += c;
    }
}

} // namespace

struct Session::UpdateOffer
{
    std::string version, tag, url, sha256;
    std::uint64_t size = 0;
    std::vector<std::string> notes;
    std::string error; // the check failed
    bool found = false;
};

// ---------------------------------------------------------------------------

void Session::update_check(bool manual)
{
    if (update_job_ || update_open)
        return;
    if (manual)
        say("Checking for updates...", 3.0);
    update_manual_ = manual;
    bg<UpdateOffer>(
        []() {
            UpdateOffer o;
            const HttpResponse r = http_get(std::string("https://api.github.com/repos/") + kUpdateRepo + "/releases/latest", 20, nullptr,
                                            {"Accept: application/vnd.github+json"});
            if (!r.ok())
            {
                o.error = r.describe();
                return o;
            }
            const json j = json::parse(r.body, nullptr, false);
            if (j.is_discarded() || !j.is_object())
            {
                o.error = "unreadable answer";
                return o;
            }
            o.tag = jstr(j, "tag_name");
            o.version = o.tag;
            if (!o.version.empty() && (o.version[0] == 'v' || o.version[0] == 'V'))
                o.version.erase(0, 1);
            o.notes = note_lines(jstr(j, "body"));
            const json &assets = jobj(j, "assets");
            if (assets.is_array())
                for (const auto &a : assets)
                {
                    const std::string name = jstr(a, "name");
                    if (!ends_with(name, ".ffpfsc"))
                        continue;
                    if (!o.url.empty() && name != std::string(kTitleId) + ".ffpfsc")
                        continue;
                    o.url = jstr(a, "browser_download_url");
                    o.size = static_cast<std::uint64_t>(jnum(a, "size"));
                    std::string d = jstr(a, "digest"); // "sha256:<hex>"
                    if (starts_with(d, "sha256:"))
                        d = d.substr(7);
                    o.sha256 = lower(d);
                }
            o.found = !o.version.empty() && !o.url.empty();
            if (o.found && (o.sha256.size() != 64 || o.size == 0))
            {
                o.found = false;
                o.error = "the release has no checksum";
            }
            return o;
        },
        [this](UpdateOffer &o) {
            const bool manual = update_manual_;
            if (!o.error.empty() || !o.found)
            {
                dlog("update check: %s", o.error.empty() ? "no release image found" : o.error.c_str());
                if (manual)
                {
                    update_dialog = UpdateState();
                    update_dialog.phase = UpdateState::Phase::failed;
                    update_dialog.message = o.error.empty() ? "No downloadable release was found." : "Could not check for updates (" + o.error + ").";
                    update_open = true;
                }
                return;
            }
            const bool is_newer = newer(o.version, kAppVersion);
            if (!is_newer && !(kUpdateOfferAnyVersion && o.version != kAppVersion))
            {
                dlog("update check: %s is the newest (latest release %s)", kAppVersion, o.version.c_str());
                if (manual)
                {
                    update_dialog = UpdateState();
                    update_dialog.phase = UpdateState::Phase::current;
                    update_dialog.current = kAppVersion;
                    update_open = true;
                }
                return;
            }
            update_offer_ = std::make_shared<UpdateOffer>(o);
            update_dialog = UpdateState();
            update_dialog.phase = UpdateState::Phase::offer;
            update_dialog.version = o.version;
            update_dialog.current = kAppVersion;
            update_dialog.size = megabytes(o.size);
            update_dialog.notes = o.notes;
            update_dialog.test_build = !is_newer;
            update_offer_pending_ = true;
        });
}

void Session::update_start()
{
    if (!update_offer_ || update_job_)
        return;
    const std::shared_ptr<UpdateJob> job = std::make_shared<UpdateJob>();
    job->started = now_seconds();
    job->total = update_offer_->size;
    update_job_ = job;
    update_dialog.phase = UpdateState::Phase::downloading;
    update_dialog.progress = 0.0f;
    update_dialog.detail = "Starting...";
    update_dialog.left.clear();
    const UpdateOffer offer = *update_offer_;
    const std::string elf = base_dir_ + "/stremio-updater.elf";
    const long pid = static_cast<long>(getpid());
    g_tasks.run([job, offer, elf, pid]() {
        const auto fail = [&](const std::string &why) {
            std::lock_guard<std::mutex> g(job->lock);
            job->error = why;
            job->phase = UpdateJob::failed;
        };
        std::string program;
        if (!read_file(elf, program) || program.size() < 64)
            return fail("The update helper is missing from the app.");
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            return fail("No network connection could be opened.");
        timeval tv{60, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(9021);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0)
        {
            close(fd);
            return fail("The payload loader did not answer (port 9021). Is it running?");
        }
        // The loader reads the program, then hands the rest of this connection to it.
        if (!send_all(fd, program.data(), program.size()))
        {
            close(fd);
            return fail("The update helper could not be started.");
        }
        char head[900];
        std::snprintf(head, sizeof(head), "SXU1\n%s\n%ld\n%llu\n%s\n%s\n", kTitleId, pid, static_cast<unsigned long long>(offer.size),
                      offer.sha256.c_str(), offer.version.c_str());
        std::string line;
        if (!send_all(fd, head, std::strlen(head)) || !read_line(fd, &line))
        {
            close(fd);
            return fail("The update helper did not answer.");
        }
        if (line != "ready")
        {
            close(fd);
            return fail(starts_with(line, "fail ") ? line.substr(5) : "The update helper refused the update.");
        }
        job->phase = UpdateJob::downloading;
        Sha256 hash;
        std::uint64_t received = 0;
        std::string pending;
        bool lost = false;
        const auto flush = [&]() {
            if (pending.empty())
                return true;
            unsigned char len[4] = {static_cast<unsigned char>(pending.size() >> 24), static_cast<unsigned char>(pending.size() >> 16),
                                    static_cast<unsigned char>(pending.size() >> 8), static_cast<unsigned char>(pending.size())};
            const bool ok = send_all(fd, len, 4) && send_all(fd, pending.data(), pending.size());
            pending.clear();
            if (!ok)
                lost = true;
            return ok;
        };
        const HttpResponse r = http_download(
            offer.url,
            [&](const char *data, std::size_t n) {
                hash.add(data, n);
                received += n;
                job->done = received;
                pending.append(data, n);
                if (pending.size() >= (256u << 10))
                    return flush();
                return true;
            },
            &job->cancel);
        if (job->cancel)
        {
            close(fd);
            job->phase = UpdateJob::cancelled;
            return;
        }
        if (!flush() || lost)
        {
            close(fd);
            return fail("The update helper stopped.");
        }
        if (!r.ok() || received != offer.size || hash.finish() != offer.sha256)
        {
            close(fd);
            return fail(!r.ok() ? "The download failed (" + r.describe() + ")." : "The download does not match the release's checksum.");
        }
        const unsigned char end[4] = {0, 0, 0, 0};
        job->phase = UpdateJob::installing;
        if (!send_all(fd, end, 4) || !read_line(fd, &line))
        {
            close(fd);
            return fail("The update helper stopped.");
        }
        if (line != "staged")
        {
            close(fd);
            return fail(starts_with(line, "fail ") ? line.substr(5) : "The update could not be prepared.");
        }
        if (job->cancel)
        {
            send_all(fd, "cancel\n", 7);
            close(fd);
            job->phase = UpdateJob::cancelled;
            return;
        }
        if (!send_all(fd, "apply\n", 6) || !read_line(fd, &line) || line != "applying")
        {
            close(fd);
            return fail("The update helper did not start the swap.");
        }
        // The connection stays open until the app has closed: the helper is waiting for that.
        job->closing_since = now_seconds();
        job->phase = UpdateJob::closing;
    });
}

void Session::update_poll()
{
    if (update_offer_pending_ && !watching && !update_open)
    {
        update_offer_pending_ = false;
        update_open = true;
    }
    if (!update_job_)
        return;
    UpdateJob &job = *update_job_;
    const int phase = job.phase;
    const double now = now_seconds();
    if (phase == UpdateJob::downloading || phase == UpdateJob::starting)
    {
        const std::uint64_t done = job.done, total = job.total;
        update_dialog.progress = total > 0 ? static_cast<float>(done) / static_cast<float>(total) : 0.0f;
        update_dialog.detail = megabytes(done) + " of " + megabytes(total);
        const double elapsed = now - job.started;
        if (done > 0 && elapsed > 2.0)
            update_dialog.left = time_left(static_cast<double>(total - done) / (static_cast<double>(done) / elapsed));
    }
    else if (phase == UpdateJob::installing)
    {
        update_dialog.phase = UpdateState::Phase::installing;
    }
    else if (phase == UpdateJob::closing)
    {
        if (update_dialog.phase != UpdateState::Phase::closing)
        {
            update_dialog.phase = UpdateState::Phase::closing;
            cue(hui::audio::Cue::saved);
        }
        // The message is on screen for a moment, then the app closes; the helper takes over.
        if (now - job.closing_since > 2.0)
        {
            save_prefs();
            save_progress();
            exit_ = true;
        }
    }
    else if (phase == UpdateJob::failed)
    {
        {
            std::lock_guard<std::mutex> g(job.lock);
            update_dialog.message = job.error;
        }
        update_dialog.phase = UpdateState::Phase::failed;
        update_dialog.selected = 0;
        update_job_.reset();
    }
    else if (phase == UpdateJob::cancelled)
    {
        update_open = false;
        update_job_.reset();
        say("Update cancelled");
    }
}

void Session::update_confirm()
{
    switch (update_dialog.phase)
    {
    case UpdateState::Phase::offer:
        if (update_dialog.selected == 0)
            update_start();
        else
            update_open = false;
        break;
    case UpdateState::Phase::failed:
    case UpdateState::Phase::current:
        update_open = false;
        break;
    default:
        break;
    }
}

void Session::update_back()
{
    switch (update_dialog.phase)
    {
    case UpdateState::Phase::offer:
    case UpdateState::Phase::failed:
    case UpdateState::Phase::current:
        update_open = false;
        break;
    case UpdateState::Phase::downloading:
        if (update_job_)
            update_job_->cancel = true;
        break;
    default:
        break;
    }
}

} // namespace sx
