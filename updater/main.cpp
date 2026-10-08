// The update helper: a payload that the app sends to the console's payload loader (loopback port 9021) when
// you press "Update now". The loader runs it outside the app's sandbox and gives it the connection as its
// standard input and output; the app and the helper talk over that connection.
//
// The app cannot replace its own image: it runs from it, and its sandbox cannot write to /data/homebrew.
// This program can. It receives the new image, checks it, waits until the app has closed, and swaps the
// file in one rename, then deletes the old image (set aside as <title>.ffpfsc.old). It writes what it does to
// /data/stremio-update.log, and tells you the result in a console notification.
//
// What the app sends (lines end with \n):    SXU1, title id, the app's process id, size, SHA-256, version name
// What the helper answers:                    ready  (or: fail <why>)
// The app then sends the image as pieces: a 4-byte big-endian length, then that many bytes; length 0 ends.
// The helper answers:                         staged  (or: fail <why>)
// The app sends:                              apply
// The helper answers:                         applying
// ...the app closes, and the helper does the swap.

#include "../src/sha256.h"

#include <cerrno>
#include <csignal>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

extern "C"
{
    int sceKernelSendNotificationRequest(int device, void *request, std::size_t size, int blocking);
    int klog_puts(const char *text);
}

namespace
{

constexpr const char *kDir = "/data/homebrew";
constexpr const char *kLogPath = "/data/stremio-update.log";
constexpr std::uint64_t kMaxImage = 600ull << 20;

void say(const char *fmt, ...)
{
    char line[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    std::string text = std::string("[stremio-update] ") + line + "\n";
    klog_puts(text.c_str());
    if (FILE *f = std::fopen(kLogPath, "a"))
    {
        std::fputs(text.c_str(), f);
        std::fclose(f);
    }
}

struct NotificationRequest
{
    std::uint8_t reserved[45];
    char message[3075];
};

void notify(const std::string &message)
{
    static NotificationRequest request;
    std::memset(&request, 0, sizeof(request));
    std::snprintf(request.message, sizeof(request.message), "%s", message.c_str());
    (void)sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
}

bool read_exact(void *data, std::size_t size)
{
    unsigned char *p = static_cast<unsigned char *>(data);
    while (size > 0)
    {
        const ssize_t n = read(STDIN_FILENO, p, size);
        if (n <= 0)
            return false;
        p += n;
        size -= static_cast<std::size_t>(n);
    }
    return true;
}

bool write_all(int fd, const void *data, std::size_t size)
{
    const unsigned char *p = static_cast<const unsigned char *>(data);
    while (size > 0)
    {
        const ssize_t n = write(fd, p, size);
        if (n <= 0)
            return false;
        p += n;
        size -= static_cast<std::size_t>(n);
    }
    return true;
}

bool read_line(std::string &out)
{
    out.clear();
    for (;;)
    {
        char c;
        if (!read_exact(&c, 1))
            return false;
        if (c == '\n')
            return true;
        if (out.size() > 512)
            return false;
        out += c;
    }
}

void reply(const std::string &line)
{
    const std::string text = line + "\n";
    (void)write_all(STDOUT_FILENO, text.data(), text.size());
}

void fail(const std::string &why)
{
    say("fail: %s", why.c_str());
    reply("fail " + why);
}

bool valid_title(const std::string &t)
{
    if (t.size() != 9 || t.compare(0, 4, "PPSA") != 0)
        return false;
    for (std::size_t i = 4; i < 9; ++i)
        if (t[i] < '0' || t[i] > '9')
            return false;
    return true;
}

// Every mounted file system that mentions the title, written to the log. Returns their mount points.
std::vector<std::string> title_mounts(const std::string &title, bool log_all)
{
    std::vector<std::string> found;
    const int count = getfsstat(nullptr, 0, MNT_NOWAIT);
    if (count <= 0)
        return found;
    std::vector<struct statfs> list(static_cast<std::size_t>(count));
    const int got = getfsstat(list.data(), static_cast<long>(list.size() * sizeof(struct statfs)), MNT_NOWAIT);
    for (int i = 0; i < got; ++i)
    {
        const std::string from = list[static_cast<std::size_t>(i)].f_mntfromname;
        const std::string on = list[static_cast<std::size_t>(i)].f_mntonname;
        const bool ours = from.find(title) != std::string::npos || on.find(title) != std::string::npos;
        if (log_all || ours)
            say("mount: %s on %s (%s)%s", from.c_str(), on.c_str(), list[static_cast<std::size_t>(i)].f_fstypename, ours ? "  <- this app" : "");
        if (ours)
            found.push_back(on);
    }
    return found;
}

bool exists(const std::string &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

std::string list_images()
{
    std::string names;
    if (DIR *d = opendir(kDir))
    {
        while (const dirent *e = readdir(d))
        {
            const std::string n = e->d_name;
            if (n.size() > 7 && n.compare(n.size() - 7, 7, ".ffpfsc") == 0)
                names += (names.empty() ? "" : ", ") + n;
        }
        closedir(d);
    }
    return names.empty() ? "none" : names;
}

} // namespace

int main()
{
    std::signal(SIGPIPE, SIG_IGN);
    std::string magic, title, pid_text, size_text, sha, version;
    if (!read_line(magic) || magic != "SXU1" || !read_line(title) || !read_line(pid_text) || !read_line(size_text) || !read_line(sha) ||
        !read_line(version))
    {
        say("bad request");
        return 1;
    }
    const long pid = std::strtol(pid_text.c_str(), nullptr, 10);
    const std::uint64_t size = std::strtoull(size_text.c_str(), nullptr, 10);
    say("update to %s requested for %s (app pid %ld, %llu bytes)", version.c_str(), title.c_str(), pid, static_cast<unsigned long long>(size));
    if (!valid_title(title) || sha.size() != 64 || size == 0 || size > kMaxImage || pid <= 1)
    {
        fail("The update request was not valid");
        return 1;
    }
    const std::string image = std::string(kDir) + "/" + title + ".ffpfsc";
    const std::string staged = image + ".new";
    const std::string old = image + ".old";
    if (!exists(image))
    {
        fail("The image " + image + " was not found (images there: " + list_images() + ")");
        return 1;
    }
    struct statfs fs;
    if (statfs(kDir, &fs) == 0 && static_cast<std::uint64_t>(fs.f_bavail) * static_cast<std::uint64_t>(fs.f_bsize) < size + (32ull << 20))
    {
        fail("There is not enough free space on the console for the update");
        return 1;
    }
    unlink(staged.c_str());
    unlink(old.c_str());
    const int out = open(staged.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0777);
    if (out < 0)
    {
        fail(std::string("Could not write to ") + kDir + " (" + std::strerror(errno) + ")");
        return 1;
    }
    reply("ready");

    Sha256 hash;
    std::uint64_t got = 0;
    std::vector<unsigned char> piece(1 << 20);
    for (;;)
    {
        unsigned char head[4];
        if (!read_exact(head, 4))
        {
            say("the app went away during the download");
            close(out);
            unlink(staged.c_str());
            return 1;
        }
        const std::uint32_t length = (std::uint32_t(head[0]) << 24) | (std::uint32_t(head[1]) << 16) | (std::uint32_t(head[2]) << 8) | head[3];
        if (length == 0)
            break;
        if (length > piece.size() || got + length > size || !read_exact(piece.data(), length) || !write_all(out, piece.data(), length))
        {
            fail("The download could not be saved");
            close(out);
            unlink(staged.c_str());
            return 1;
        }
        hash.add(piece.data(), length);
        got += length;
    }
    fsync(out);
    close(out);
    const std::string digest = hash.finish();
    if (got != size || digest != sha)
    {
        fail("The download does not match its checksum");
        unlink(staged.c_str());
        return 1;
    }
    say("received and verified: %llu bytes, sha256 %s", static_cast<unsigned long long>(got), digest.c_str());
    reply("staged");

    std::string command;
    if (!read_line(command) || command != "apply")
    {
        say("cancelled before apply");
        unlink(staged.c_str());
        return 0;
    }
    reply("applying");
    title_mounts(title, true);

    // Wait until the app has closed: its process is gone, and with it, hopefully, its mount.
    say("waiting for the app (pid %ld) to close", pid);
    int waited_ms = 0;
    while (kill(static_cast<pid_t>(pid), 0) == 0 || errno != ESRCH)
    {
        if (waited_ms >= 120000)
        {
            say("the app did not close");
            unlink(staged.c_str());
            notify("Stremio was not updated: it did not close in time. Nothing was changed.");
            return 1;
        }
        usleep(200 * 1000);
        waited_ms += 200;
    }
    say("the app closed after %d ms", waited_ms);
    usleep(2000 * 1000);

    // Still mounted? Ask for the image to be unmounted (a plain request: it fails if anything is using it).
    std::vector<std::string> mounted = title_mounts(title, false);
    for (int attempt = 0; attempt < 5 && !mounted.empty(); ++attempt)
    {
        for (const std::string &on : mounted)
        {
            const int rc = unmount(on.c_str(), 0);
            say("unmount %s -> %d (%s)", on.c_str(), rc, rc == 0 ? "ok" : std::strerror(errno));
        }
        usleep(1000 * 1000);
        mounted = title_mounts(title, false);
    }

    if (rename(image.c_str(), old.c_str()) != 0)
    {
        say("could not move the old image aside: %s", std::strerror(errno));
        unlink(staged.c_str());
        notify("Stremio was not updated: the old image could not be moved. Nothing was changed.");
        return 1;
    }
    if (rename(staged.c_str(), image.c_str()) != 0)
    {
        say("could not put the new image in place: %s", std::strerror(errno));
        rename(old.c_str(), image.c_str());
        unlink(staged.c_str());
        notify("Stremio was not updated: the new image could not be put in place. The old one is back.");
        return 1;
    }
    chmod(image.c_str(), 0777);
    // The new image is in place: the old one is no longer needed. If it is still mounted, its space comes back
    // once it is unmounted (the next restart at the latest).
    if (unlink(old.c_str()) == 0)
        say("deleted the old image %s", old.c_str());
    else
        say("could not delete the old image %s: %s", old.c_str(), std::strerror(errno));
    say("done: %s is now version %s", image.c_str(), version.c_str());
    title_mounts(title, true);
    notify("Stremio was updated to " + version + ". Open it again." +
           (mounted.empty() ? "" : " If the old version opens, restart the console once."));
    return 0;
}
