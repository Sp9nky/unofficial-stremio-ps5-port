#include "vrr.hpp"

#include "hdr_out.hpp"
#include "platform/ps5/system.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>

#ifdef PLATFORM_PS5_NATIVE
extern "C"
{
int32_t sceVideoOutGetResolutionStatus(int32_t handle, void *status);
int32_t sceVideoOutConfigureOutput(int32_t handle, uint32_t mode, const void *, const void *, const void *);
int32_t sceVideoOutWaitVblank(int32_t handle);
}
#endif

namespace sx::vrr
{

namespace
{

constexpr float kMenuHz = 59.94f;
constexpr float kRates[] = {48.0f, 50.0f, 59.94f, 60.0f, 71.928f}; // PS5MC's VRR modes
constexpr std::uint32_t kOutputModeDefault = 1;                      // the system's own mode

struct State
{
    bool started = false;
    bool link_vrr = false;
    float rate = 0.0f; // the rate to present at now (0: do not pace)
    float logged_rate = -1.0f;
    std::chrono::steady_clock::time_point next{};
    std::chrono::steady_clock::time_point checked{};
} g;

#ifdef PLATFORM_PS5_NATIVE
// SceVideoOutResolutionStatus (48 bytes): sizes, then the refresh rate as a code (what the system reports).
struct ResolutionStatus
{
    std::uint32_t full_w, full_h, pane_w, pane_h;
    std::uint64_t refresh;
    float inches;
    std::uint32_t reserved[4];
};

float refresh_from_code(std::uint64_t id)
{
    switch (id)
    {
    case 0x1:
        return 23.976f;
    case 0x2:
        return 50.0f;
    case 0x3:
        return 59.94f;
    case 0x4:
        return 29.97f;
    case 0xd:
        return 119.88f;
    case 0x23:
        return 89.91f;
    default:
        return 0.0f;
    }
}

float query_refresh()
{
    const std::int32_t handle = hdr::video_out_handle();
    if (handle < 0)
        return 0.0f;
    std::uint64_t raw[16] = {};
    if (sceVideoOutGetResolutionStatus(handle, raw) != 0)
        return 0.0f;
    return refresh_from_code(reinterpret_cast<const ResolutionStatus *>(raw)->refresh);
}
#endif

// The lowest of the VRR rates that is a whole multiple of the frame rate (to 0.05 %), else none.
float rate_for(double fps)
{
    if (fps < 10.0 || fps > 120.0)
        return 0.0f;
    for (const float r : kRates)
    {
        const double m = r / fps;
        if (m >= 0.99 && std::fabs(m - std::round(m)) < 0.0005 * m)
            return r;
    }
    return 0.0f;
}

} // namespace

void update(bool enabled, double video_fps)
{
#ifdef PLATFORM_PS5_NATIVE
    const std::int32_t handle = hdr::video_out_handle();
    if (handle < 0)
        return;
    const auto now = std::chrono::steady_clock::now();
    if (!g.started)
    {
        // As PS5MC does once the output exists: ask for the system's own mode (the one that is a VRR link when the
        // console's VRR is on), let two vblanks pass, and read what the output runs at.
        g.started = true;
        const float before = query_refresh();
        const int rc = sceVideoOutConfigureOutput(handle, kOutputModeDefault, nullptr, nullptr, nullptr);
        if (rc == 0)
            for (int i = 0; i < 2; ++i)
                sceVideoOutWaitVblank(handle);
        const float after = query_refresh();
        g.link_vrr = after > 100.0f;
        g.checked = now;
        hui::sys::log("[VRR] system mode requested (%#x): output %.3f Hz before, %.3f Hz after: %s", static_cast<unsigned>(rc), before,
                      after, g.link_vrr ? "a VRR link, presents are paced" : "no VRR link, fixed-rate output");
    }
    else if (now - g.checked > std::chrono::seconds(2))
    {
        // the console switches a title onto (or off) its VRR link when it likes
        g.checked = now;
        const float hz = query_refresh();
        if (hz > 0.0f && (hz > 100.0f) != g.link_vrr)
        {
            g.link_vrr = hz > 100.0f;
            hui::sys::log("[VRR] the output is %s a VRR link (%.3f Hz)", g.link_vrr ? "now" : "no longer", hz);
        }
    }
    float want = 0.0f;
    if (g.link_vrr)
    {
        want = kMenuHz; // a VRR link reports ~120 Hz: that is not the rate to run the menus at
        if (enabled && video_fps > 0.0)
            if (const float r = rate_for(video_fps); r > 0.0f)
                want = r;
    }
    g.rate = want;
    if (want != g.logged_rate)
    {
        g.logged_rate = want;
        if (want > 0.0f)
            hui::sys::log("[VRR] presenting at %.3f Hz on the VRR link%s", want, video_fps > 0.0 && want != kMenuHz ? " (for the video)" : "");
    }
#else
    (void)enabled;
    (void)video_fps;
#endif
}

void pace()
{
    if (g.rate <= 0.0f)
        return;
    const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(1.0 / g.rate));
    const auto now = std::chrono::steady_clock::now();
    if (g.next < now - period || g.next > now + 2 * period)
        g.next = now; // (re)start the cadence
    std::this_thread::sleep_until(g.next);
    g.next += period;
}

} // namespace sx::vrr
