// Stremio for the PS5 on the OpenGL toolkit: opens the display and the
// controller, then runs the frame loop. What the buttons mean is ui/shell.cpp,
// what the screens show is gl/session.cpp.

#include "chrome.hpp"
#include "core/input.hpp"
#include "dynamic_font.hpp"
#include "gfx/renderer.hpp"
#include "hwdec_ps5.h"
#include "platform/ps5/display_egl.hpp"
#include "platform/ps5/pad.hpp"
#include "platform/ps5/system.hpp"
#include "overlays.hpp"
#include "page.hpp"
#include "session.hpp"
#include "shell.hpp"
#include "theme.hpp"

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <span>
#include <string>

extern "C" void ps5_load_modules(void); // the keyboard dialog's module (native/ps5_modules.c)

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "stb_image.h"

namespace
{

constexpr const char *kRoot = "/app0";
// The title's own storage (param.json downloadDataSize): settings, progress, artwork.
constexpr const char *kData = "/download0/stremio";

struct Mode
{
    int width, height;
};

// 4K first, then what a 1080p television can show.
constexpr Mode kModes[] = {{3840, 2160}, {1920, 1080}};

std::uint32_t load_icon(hui::gfx::Renderer &renderer, const std::string &path)
{
    int w = 0, h = 0, comp = 0;
    unsigned char *pixels = stbi_load(path.c_str(), &w, &h, &comp, 4);
    if (!pixels)
    {
        hui::sys::log("[SX] cannot load %s", path.c_str());
        return 0;
    }
    const std::uint32_t texture = renderer.batch().create_texture(w, h, pixels);
    stbi_image_free(pixels);
    return texture;
}

} // namespace

int main()
{
    using namespace hui;
    sys::log("[SX] entry");
    const std::string root = kRoot;
    // A closed connection must fail a write quietly instead of killing us.
    std::signal(SIGPIPE, SIG_IGN);

    ps5::Display display;
    bool opened = false;
    for (const Mode &mode : kModes)
    {
        if (display.open(mode.width, mode.height))
        {
            opened = true;
            sys::log("[SX] display %dx%d", display.width(), display.height());
            break;
        }
        sys::log("[SX] display %dx%d failed", mode.width, mode.height);
    }
    if (!opened)
        sys::park();

    gfx::Renderer renderer;
    static sx::UiFonts ui_fonts;
    static ui::Fonts fonts;
    if (!renderer.init())
    {
        sys::log("[SX] fatal: renderer failed");
        sys::park();
    }

    // The boot animation starts as early as it can: its first frame is the picture the console shows
    // while the app starts (sce_sys/pic0 and pic1: the page's light and the mark), so nothing cuts when we
    // take over, and everything slow (the modules, fonts, storage, the network) happens after it, with a frame of the
    // animation drawn between the steps.
    static sx::Icons icons;
    const std::string icon_dir = root + "/icons";
    icons.logo_xl = load_icon(renderer, icon_dir + "/logo_xl.png");
    icons.name = load_icon(renderer, icon_dir + "/name.png");
    const std::int64_t boot_t0 = sys::monotonic_us();
    const auto boot_seconds = [&] { return static_cast<float>(sys::monotonic_us() - boot_t0) / 1e6f; };
    bool splash_hidden = false;
    const auto boot_frame = [&] {
        const sx::Env boot_env{fonts, icons};
        gfx::DrawList list;
        gfx::BackdropSpec backdrop;
        sx::apply_page_backdrop(backdrop, boot_seconds());
        sx::draw_boot(list, boot_env, boot_seconds(), 0.0f);
        renderer.begin();
        renderer.backdrop(backdrop);
        renderer.draw(list);
        renderer.present(0, display.width(), display.height());
        if (!display.swap())
        {
            sys::log("[SX] fatal: boot swap failed");
            sys::park();
        }
        if (!splash_hidden)
        {
            splash_hidden = true;
            sys::log("[SX] first picture; splash hidden: %d", sys::hide_splash_screen() ? 1 : 0);
        }
    };
    boot_frame();
    ps5_load_modules(); // the keyboard dialog
    boot_frame();
    HwDecoder::load_module(); // the hardware video decoder
    boot_frame();
    if (!sx::load_ui_fonts(renderer, root + "/fonts", root + "/hui-fonts", &ui_fonts, &fonts))
    {
        sys::log("[SX] fatal: fonts failed");
        sys::park();
    }
    boot_frame();

    const char *solid[5] = {"nav_board", "nav_discover", "nav_library", "nav_addons", "nav_settings"};
    for (int i = 0; i < 5; ++i)
    {
        icons.nav_solid[i] = load_icon(renderer, icon_dir + "/" + solid[i] + ".png");
        icons.nav_line[i] = load_icon(renderer, icon_dir + "/" + solid[i] + "_o.png");
    }
    icons.search = load_icon(renderer, icon_dir + "/search.png");
    icons.logo = load_icon(renderer, icon_dir + "/logo_l.png");
    boot_frame();

    static sx::Session session;
    if (!session.init(root, kData, &renderer))
    {
        sys::log("[SX] fatal: session failed");
        sys::park();
    }
    static sx::Env env{fonts, icons};
    static sx::Shell shell(env, session);
    shell.start_sound(root + "/sounds"); // the interface's sounds
    shell.start_boot_at(boot_seconds()); // the animation goes on from where the frames above left it

    ps5::Pad pad;
    pad.open();
    InputTracker tracker;
    PadSample samples[64];

    std::uint64_t frames = 0;
    std::int64_t last_start = sys::monotonic_us();
    std::int64_t window_start = last_start;
    int window_frames = 0;
    float worst_ms = 0.0f;
    for (;;)
    {
        const std::int64_t now = sys::monotonic_us();
        float dt = frames == 0 ? 1.0f / 60.0f : static_cast<float>(now - last_start) / 1e6f;
        last_start = now;
        const float frame_ms = dt * 1000.0f;
        dt = std::min(dt, 0.05f); // a hitch must not teleport the animations

        const std::size_t count = pad.read(samples);
        const InputFrame input = tracker.update(std::span<const PadSample>(samples, count), static_cast<std::uint64_t>(now));
        session.update(dt, shell.cursor());
        env.clock = session.clock;
        shell.update(input, dt);
        if (session.wants_exit())
            sys::quit();

        gfx::DrawList list;
        gfx::BackdropSpec backdrop;
        shell.draw(list, backdrop);
        sx::sync_fonts(renderer, ui_fonts, fonts);
        renderer.begin();
        renderer.backdrop(backdrop);
        renderer.draw(list);
        renderer.present(0, display.width(), display.height());
        if (!display.swap())
        {
            sys::log("[SX] fatal: swap failed at frame %llu", static_cast<unsigned long long>(frames));
            sys::park();
        }
        ++frames;
        if (frames == 1)
        {
            sys::log("[SX] first frame: %zu shapes, %zu draw calls", renderer.last_instances(), renderer.last_draw_calls());
        }

        // One line every ten seconds: how fast the frames came.
        ++window_frames;
        worst_ms = std::max(worst_ms, frame_ms);
        if (now - window_start >= 10'000'000)
        {
            const double seconds = static_cast<double>(now - window_start) / 1e6;
            sys::log("[SX] %.1f fps, worst frame %.1f ms, %zu shapes, %zu draw calls", window_frames / seconds, worst_ms,
                     renderer.last_instances(), renderer.last_draw_calls());
            window_start = now;
            window_frames = 0;
            worst_ms = 0.0f;
        }
    }
}
