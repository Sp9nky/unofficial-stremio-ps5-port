// Renders the new screens to PNG pictures for review, with the same drawing
// code the console runs, through Mesa's software OpenGL (surfaceless EGL).
// It draws pictures only: it is not a playable version of the app.
//
// usage: preview <hui assets> <sample art> <icons> <output dir> [screen]
//
// Screens: board (default: all of them).

#include "addons.hpp"
#include "board.hpp"
#include "chrome.hpp"
#include "core/save_file.hpp"
#include "detail.hpp"
#include "discover.hpp"
#include "dynamic_font.hpp"
#include "gfx/gl_program.hpp"
#include "gfx/renderer.hpp"
#include "library.hpp"
#include "overlays.hpp"
#include "profiles.hpp"
#include "update.hpp"
#include "../../gl/hdr_out.hpp"
#include "page.hpp"
#include "player.hpp"
#include "hdr_out.hpp"
#include "yuv_convert.h"
#include "gpu_upload.hpp"
#include "art.hpp"
#include "http.h"
#include "util.h"
namespace sx
{
class Session;
}
namespace sx::preview
{
void test_login_done(Session &session, const std::string &auth_key);
}
#include "sample.hpp"
#include "scripts_scene.hpp"
#include "worst_cases.hpp"
#include "settings.hpp"
#include "shell.hpp"
#include "../../gl/session.hpp"
#include "ui/fonts.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glcorearb.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../third_party/hui/third_party/stb/stb_image_write.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <tuple>
#include <functional>
#include <filesystem>
#include <cstdlib>
#include <string>
#include <vector>

namespace
{

bool open_context()
{
    auto get_platform_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
        eglGetProcAddress("eglGetPlatformDisplayEXT"));
    EGLDisplay display = get_platform_display != nullptr
                             ? get_platform_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr)
                             : eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0;
    EGLint minor = 0;
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor) || !eglBindAPI(EGL_OPENGL_API))
        return false;
    const EGLint attributes[] = {EGL_CONTEXT_MAJOR_VERSION,
                                 4,
                                 EGL_CONTEXT_MINOR_VERSION,
                                 5,
                                 EGL_CONTEXT_OPENGL_PROFILE_MASK,
                                 EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                                 EGL_NONE};
    EGLContext context = eglCreateContext(display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attributes);
    return context != EGL_NO_CONTEXT && eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context);
}

bool load_font(hui::gfx::Renderer &renderer, const std::string &path, hui::gfx::Font *font, hui::ui::FontRef *ref)
{
    std::string data;
    if (!hui::save::read_file(path, &data) || !font->load(data))
    {
        std::fprintf(stderr, "cannot load font %s\n", path.c_str());
        return false;
    }
    ref->font = font;
    ref->texture = renderer.batch().create_font_texture(*font);
    return true;
}

struct Rig
{
    hui::gfx::Renderer renderer;
    GLuint framebuffer = 0;
    int width = 1920;
    int height = 1080;
    std::string output;
    std::vector<unsigned char> pixels;
    bool ok = true;
    std::function<void()> sync; // sends glyphs made since the last picture to the GPU

    bool init(int w, int h)
    {
        width = w;
        height = h;
        GLuint color = 0;
        glGenFramebuffers(1, &framebuffer);
        glGenRenderbuffers(1, &color);
        glBindRenderbuffer(GL_RENDERBUFFER, color);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color);
        pixels.resize(static_cast<std::size_t>(width) * height * 4);
        stbi_flip_vertically_on_write(1);
        return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    }

    // Presents a finished draw list and writes it as <name>.png.
    void shot(const std::string &name, const hui::gfx::BackdropSpec &backdrop, const hui::gfx::DrawList &list)
    {
        if (sync)
            sync();
        renderer.begin();
        renderer.backdrop(backdrop);
        renderer.draw(list);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        renderer.present(framebuffer, width, height);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        for (std::size_t i = 3; i < pixels.size(); i += 4)
            pixels[i] = 255;
        const std::string path = output + "/" + name + ".png";
        ok = stbi_write_png(path.c_str(), width, height, 4, pixels.data(), width * 4) != 0 && ok;
        std::fprintf(stderr, "wrote %s (%zu shapes, %zu draw calls)\n", path.c_str(), renderer.last_instances(),
                     renderer.last_draw_calls());
    }
};

constexpr float kDt = 1.0f / 60.0f;

hui::InputFrame idle_input()
{
    hui::InputFrame in;
    in.connected = true;
    return in;
}

// One button press, then idle frames so every spring settles.
template <typename Screen> void press(Screen &screen, hui::Direction direction, int frames = 70)
{
    hui::InputFrame in = idle_input();
    in.nav = direction;
    screen.update(in, kDt);
    for (int i = 1; i < frames; ++i)
        screen.update(idle_input(), kDt);
}

template <typename Screen> void settle(Screen &screen, int frames)
{
    for (int i = 0; i < frames; ++i)
        screen.update(idle_input(), kDt);
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 5)
    {
        std::fprintf(stderr, "usage: %s <hui assets> <sample art> <icons> <output dir> [screen]\n", argv[0]);
        return 2;
    }
    const std::string assets = argv[1];
    const std::string art = argv[2];
    const std::string icons_dir = argv[3];
    const std::string only = argc > 5 ? argv[5] : "all";

    if (!open_context())
    {
        std::fprintf(stderr, "no surfaceless EGL OpenGL 4.5 context\n");
        return 1;
    }
    hui::gfx::set_glsl_prefix("#version 450 core\n");

    Rig rig;
    rig.output = argv[4];
    hui::save::ensure_directory(rig.output);
    sx::UiFonts ui_fonts;
    hui::ui::Fonts fonts;
    // The app's fonts folder is two levels up from the icons (app/assets/icons_4k).
    if (!rig.renderer.init() || !rig.init(1920, 1080) ||
        !sx::load_ui_fonts(rig.renderer, icons_dir + "/../../fonts", assets + "/fonts", &ui_fonts, &fonts))
        return 1;
    rig.sync = [&] { sx::sync_fonts(rig.renderer, ui_fonts, fonts); };

    sx::Icons icons;
    const char *solid[5] = {"nav_board", "nav_discover", "nav_library", "nav_addons", "nav_settings"};
    for (int i = 0; i < 5; ++i)
    {
        icons.nav_solid[i] = sx::preview::load_texture(rig.renderer, icons_dir + "/" + solid[i] + ".png");
        icons.nav_line[i] = sx::preview::load_texture(rig.renderer, icons_dir + "/" + solid[i] + "_o.png");
    }
    icons.search = sx::preview::load_texture(rig.renderer, icons_dir + "/search.png");
    icons.logo = sx::preview::load_texture(rig.renderer, icons_dir + "/logo_l.png");
    icons.logo_xl = sx::preview::load_texture(rig.renderer, icons_dir + "/logo_xl.png");
    icons.name = sx::preview::load_texture(rig.renderer, icons_dir + "/name.png");
    sx::Env env{fonts, icons};

    using hui::Direction;
    const auto wants = [&](const char *name) { return only == "all" || only == name; };
    // Renders one picture of a screen in its current state.
    const auto shoot = [&](auto &screen, const char *name) {
        hui::gfx::DrawList list;
        hui::gfx::BackdropSpec backdrop;
        screen.draw(list, backdrop);
        rig.shot(name, backdrop, list);
    };

    if (wants("board"))
    {
        sx::BoardContent content;
        if (!sx::preview::load_board(rig.renderer, art, &content))
            return 1;
        if (!content.rows.empty())
            content.rows[0].key = "continue";
        sx::Board board(env);
        board.set_content(&content);
        board.enter();
        settle(board, 90);
        shoot(board, "board-1-home");
        press(board, Direction::right);
        press(board, Direction::right);
        shoot(board, "board-2-focus-moved");
        press(board, Direction::down);
        shoot(board, "board-3-second-row");
        press(board, Direction::up);
        for (int i = 0; i < 3; ++i)
            press(board, Direction::left);
        shoot(board, "board-4-rail-focused");
        // Continue Watching: what the buttons ask, as dialogs in the middle of the screen.
        press(board, Direction::right);
        for (int i = 0; i < 3; ++i)
        {
            hui::gfx::DrawList list;
            hui::gfx::BackdropSpec backdrop;
            board.draw(list, backdrop);
            sx::DropdownState dd;
            dd.active = -1;
            dd.dialog = true;
            dd.poster = content.rows[0].items[0].poster;
            dd.progress = 0.42f;
            dd.title = "The Lantern Keepers";
            if (i == 0)
            {
                dd.message = "S2 E5  \xC2\xB7  42% watched";
                dd.options = {"Play now", "Choose another episode", "Choose another stream"};
            }
            else if (i == 1)
            {
                dd.message = "S2 E5";
                dd.options = {"Remove from Continue Watching", "Cancel"};
            }
            else
            {
                dd.message = "Remove it from Continue Watching?";
                dd.options = {"Remove", "Keep"};
                dd.danger = 0;
                dd.selected = 1;
            }
            sx::draw_dropdown(list, env, dd, 1.0f);
            rig.shot(i == 0 ? "board-5-continue" : i == 1 ? "board-6-options" : "board-7-confirm", backdrop, list);
        }    }
    if (wants("signin"))
    {
        // The sign-in screen before the Board (sample content): up, coming in, and without a code yet.
        sx::BoardContent content;
        if (!sx::preview::load_board(rig.renderer, art, &content))
            return 1;
        std::vector<sx::WallTile> wall;
        for (const auto &row : content.rows)
            for (const auto &item : row.items)
                if (item.poster != 0 && wall.size() < 36)
                    wall.push_back({item.poster, 1.0f});
        sx::SignInState in;
        in.qr = sx::preview::load_texture(rig.renderer, art + "/qr.png");
        in.code = "A7KP";
        in.link = "link.stremio.com";
        sx::SignInState failed;
        failed.link = "link.stremio.com";
        failed.status = "Could not get a code: Could not resolve host: link.stremio.com";
        failed.failed = true;
        const struct
        {
            const sx::SignInState *state;
            float t;
            const char *name;
        } shots[] = {{&in, 1.0f, "signin-screen"}, {&in, 0.5f, "signin-screen-coming-in"}, {&failed, 1.0f, "signin-screen-no-code"}};
        for (const auto &shot : shots)
        {
            hui::gfx::DrawList list;
            hui::gfx::BackdropSpec backdrop;
            sx::apply_page_backdrop(backdrop, 0.6f);
            sx::draw_sign_in_screen(list, env, *shot.state, wall, 0.6f, shot.t);
            rig.shot(shot.name, backdrop, list);
        }
    }
    if (wants("discover"))
    {
        sx::DiscoverContent content;
        if (!sx::preview::load_discover(rig.renderer, art, &content))
            return 1;
        sx::Discover page(env);
        page.set_content(&content);
        page.enter();
        settle(page, 90);
        shoot(page, "discover-1-home");
        press(page, Direction::right);
        press(page, Direction::down);
        shoot(page, "discover-2-moved");
        press(page, Direction::down);
        press(page, Direction::down);
        shoot(page, "discover-3-scrolled");
        for (int i = 0; i < 4; ++i)
            press(page, Direction::up);
        press(page, Direction::right);
        shoot(page, "discover-4-filters");
    }
    if (wants("library"))
    {
        sx::LibraryContent content;
        if (!sx::preview::load_library(rig.renderer, art, &content))
            return 1;
        sx::Library page(env);
        page.set_content(&content);
        page.enter();
        settle(page, 90);
        shoot(page, "library-1-home");
        press(page, Direction::right);
        press(page, Direction::down);
        press(page, Direction::down);
        shoot(page, "library-2-scrolled");
        for (int i = 0; i < 3; ++i)
            press(page, Direction::up);
        shoot(page, "library-3-filters");
    }
    if (wants("addons"))
    {
        sx::AddonsContent content;
        sx::preview::load_addons(&content);
        sx::Addons page(env);
        page.set_content(&content);
        page.enter();
        settle(page, 90);
        shoot(page, "addons-1-home");
        press(page, Direction::right);
        press(page, Direction::down);
        shoot(page, "addons-2-moved");
        press(page, Direction::down);
        shoot(page, "addons-3-last-row");
    }
    if (wants("settings"))
    {
        sx::SettingsContent content;
        sx::preview::load_settings(&content);
        sx::Settings page(env);
        page.set_content(&content);
        page.enter();
        settle(page, 90);
        shoot(page, "settings-1-home");
        for (int i = 0; i < 3; ++i)
            press(page, Direction::down, 40);
        settle(page, 30);
        shoot(page, "settings-2-switch");
        for (int i = 0; i < 6; ++i)
            press(page, Direction::down, 40);
        settle(page, 40);
        shoot(page, "settings-3-end");
    }
    // One tap of a button, then idle frames.
    const auto tap = [&](auto &screen, hui::Action action, int frames = 60) {
        hui::InputFrame in = idle_input();
        in.pressed = hui::action_bit(action);
        screen.update(in, kDt);
        settle(screen, frames - 1);
    };
    using hui::gfx::kFullUv;

    if (wants("detail"))
    {
        sx::DetailContent series, movie;
        if (!sx::preview::load_detail(rig.renderer, art, &series, &movie))
            return 1;
        {
            sx::Detail page(env);
            page.set_content(&movie);
            page.enter();
            settle(page, 90);
            shoot(page, "detail-1-movie-streams");
            press(page, Direction::down);
            press(page, Direction::down);
            press(page, Direction::down);
            shoot(page, "detail-2-movie-streams-moved");
        }
        {
            sx::Detail page(env);
            page.set_content(&series);
            page.enter();
            settle(page, 90);
            for (int i = 0; i < 4; ++i)
                press(page, Direction::down, 25);
            settle(page, 40);
            shoot(page, "detail-3-series-episodes");
            tap(page, hui::Action::confirm, 70);
            press(page, Direction::down, 40);
            shoot(page, "detail-4-series-streams");
        }
    }
    if (wants("search"))
    {
        sx::SearchContent found, none;
        if (!sx::preview::load_search(rig.renderer, art, &found, &none))
            return 1;
        {
            sx::Board page(env);
            page.set_content(&found.results);
            page.set_search(&found.query);
            page.enter();
            settle(page, 90);
            shoot(page, "search-1-results");
            press(page, Direction::right);
            press(page, Direction::down);
            shoot(page, "search-2-results-moved");
        }
        {
            sx::Board page(env);
            page.set_content(&none.results);
            page.set_search(&none.query);
            page.enter();
            settle(page, 90);
            shoot(page, "search-3-no-results");
        }
    }
    if (only == "artcache")
    {
        // The artwork pipeline against a pretend image host (tools/tests/run_art_test.sh starts it).
        const char *port = std::getenv("ART_PORT");
        const std::string base = std::string("http://127.0.0.1:") + (port ? port : "18996");
        const std::string dir = "/tmp/artcache-test";
        std::system(("rm -rf " + dir).c_str());
        sx::TexCache cache;
        cache.init(dir, &rig.renderer, 6);
        int bad = 0;
        const auto check = [&](bool ok, const std::string &what) {
            std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
            if (!ok)
                ++bad;
        };
        const auto wait_tex = [&](const std::string &url, sx::ArtKind kind, double secs) -> std::uint32_t {
            const auto t0 = std::chrono::steady_clock::now();
            while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < secs)
            {
                if (const std::uint32_t id = cache.get(url, kind))
                    return id;
                cache.pump(12);
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            return 0;
        };
        const auto tex_w = [](std::uint32_t id) {
            GLint w = 0;
            glBindTexture(GL_TEXTURE_2D, id);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
            return int(w);
        };
        const auto hits = [&](const std::string &path) {
            const json j = json::parse(http_get(base + "/__stats", 5).body, nullptr, false);
            return j.is_object() && j["hits"].contains(path) ? j["hits"][path].get<int>() : 0;
        };
        using sx::ArtKind;
        check(sx::better_url("https://images.metahub.space/poster/small/tt1/img", ArtKind::poster) ==
                  "https://images.metahub.space/poster/medium/tt1/img",
              "metahub's 300 x 450 posters are asked as its 500 x 750 ones");
        check(sx::better_url("https://images.metahub.space/background/medium/tt1/img", ArtKind::backdrop) ==
                  "https://images.metahub.space/background/medium/tt1/img",
              "other addresses are left as they are");
        const std::uint32_t same = wait_tex(base + "/background/medium/same/img", ArtKind::backdrop, 20);
        check(same != 0 && tex_w(same) == 2560, "a 1280 wallpaper whose big copy is the same picture comes out 2560 wide (got " +
                                                    std::to_string(same ? tex_w(same) : 0) + ")");
        const std::uint32_t diff = wait_tex(base + "/background/medium/diff/img", ArtKind::backdrop, 20);
        check(diff != 0 && tex_w(diff) == 1920, "a big copy that is other artwork is left out (1920 wide)");
        const std::uint32_t wide = wait_tex(base + "/background/medium/wide/img", ArtKind::backdrop, 20);
        check(wide != 0 && tex_w(wide) == 1920 && hits("/background/big/wide/img") == 0,
              "a 1920 wallpaper does not fetch its big copy");
        wait_tex(base + "/missing/img", ArtKind::poster, 3);
        check(cache.peek(base + "/missing/img", ArtKind::poster) == 0 && hits("/missing/img") == 1,
              "a picture that is not there (404) is asked once and given up");
        const std::uint32_t flaky = wait_tex(base + "/flaky/img", ArtKind::poster, 25);
        check(flaky != 0 && hits("/flaky/img") == 3, "a busy host (503 twice) is asked again later, and the picture comes");
        cache.warm(base + "/poster/medium/tt2/img", ArtKind::poster);
        for (int i = 0; i < 100 && hits("/poster/medium/tt2/img") == 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        cache.pump(12);
        check(cache.peek(base + "/poster/medium/tt2/img", ArtKind::poster) == 0, "a warmed picture is not put on the GPU");
        const std::uint32_t warmed = wait_tex(base + "/poster/medium/tt2/img", ArtKind::poster, 10);
        check(warmed != 0 && hits("/poster/medium/tt2/img") == 1, "asked for later, it comes from disk, not downloaded again");
        check(cache.cached(base + "/poster/medium/tt2/img", ArtKind::poster) && !cache.cached(base + "/never/asked/img", ArtKind::poster),
              "a picture counts as cached once it is on disk, and not before");
        const std::uint32_t wall = wait_tex(base + "/poster/medium/tt2/img", ArtKind::wall, 10);
        check(wall != 0 && tex_w(wall) == 216 && hits("/poster/medium/tt2/img") == 1,
              "the sign-in wall's copy is cut from the poster on disk (216 wide, no second download)");
        const std::uint32_t wall_first = wait_tex(base + "/poster/medium/tt1/img", ArtKind::wall, 10);
        const std::uint32_t card_after = wait_tex(base + "/poster/medium/tt1/img", ArtKind::poster, 10);
        check(wall_first != 0 && card_after != 0 && tex_w(card_after) == 480 && hits("/poster/medium/tt1/img") == 1,
              "a wall copy asked for first fetches the poster once and keeps it for the card");
        {
            const int w = 301, h = 173; // odd sizes on purpose
            std::vector<unsigned char> px(size_t(w) * h * 4);
            for (size_t i = 0; i < px.size(); ++i)
                px[i] = static_cast<unsigned char>((i * 2654435761u) >> 13);
            const std::uint32_t id = sx::gpu::texture_from_rgba(w, h, px.data());
            std::vector<unsigned char> back(px.size());
            GLuint fbo = 0;
            glGenFramebuffers(1, &fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, id, 0);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, back.data());
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glDeleteFramebuffers(1, &fbo);
            check(id != 0 && back == px, "the GPU copy of a picture is bit for bit the picture, the right way up");
        }
        const json st = json::parse(http_get(base + "/__stats", 5).body, nullptr, false);
        int requests = 0;
        for (auto it = st["hits"].begin(); it != st["hits"].end(); ++it)
            if (it.key() != "/__stats")
                requests += it.value().get<int>();
        check(st["connections"].get<int>() < requests, "connections are kept open between pictures (" +
                                                          std::to_string(st["connections"].get<int>()) + " connections for " +
                                                          std::to_string(requests) + " requests)");
        cache.shutdown();
        std::printf(bad ? "artcache: %d check(s) failed\n" : "artcache OK\n", bad);
        return bad ? 1 : 0;
    }
    if (wants("sdrconv"))
    {
        // The GPU conversion of 10-bit SDR planes against the CPU one, on colour bars with a gradient, low- and high-aligned.
        const int w = 128, h = 64, ow = w / 2, oh = h / 2;
        int worst_all = 0;
        for (int variant = 0; variant < 4; ++variant)
        {
            const bool high = variant & 1, bt601 = variant & 2;
            std::vector<std::uint16_t> y(size_t(w) * h), uv(size_t(w / 2) * (h / 2) * 2);
            const double kr = bt601 ? 0.299 : 0.2126, kb = bt601 ? 0.114 : 0.0722, kg = 1.0 - kr - kb;
            const double bars[8][3] = {{1, 1, 1}, {1, 1, 0}, {0, 1, 1}, {0, 1, 0}, {1, 0, 1}, {1, 0, 0}, {0, 0, 1}, {0.2, 0.2, 0.2}};
            auto encode = [&](int px, int py, double* yy, double* cb, double* cr) {
                const double* c = bars[(px * 8) / w];
                const double g = 0.2 + 0.8 * double(py) / h;  // a vertical ramp inside each bar
                const double r = c[0] * g, gg = c[1] * g, b = c[2] * g;
                *yy = kr * r + kg * gg + kb * b;
                *cb = (b - *yy) / (2 * (1 - kb));
                *cr = (r - *yy) / (2 * (1 - kr));
            };
            for (int py = 0; py < h; ++py)
                for (int px = 0; px < w; ++px)
                {
                    double yy, cb, cr;
                    encode(px, py, &yy, &cb, &cr);
                    int v = int(64 + 876 * yy + 0.5);
                    y[size_t(py) * w + px] = std::uint16_t(high ? v << 6 : v);
                }
            for (int py = 0; py < h / 2; ++py)
                for (int px = 0; px < w / 2; ++px)
                {
                    double yy, cb, cr;
                    encode(px * 2, py * 2, &yy, &cb, &cr);
                    int u = int(512 + 896 * cb + 0.5), v = int(512 + 896 * cr + 0.5);
                    uv[(size_t(py) * (w / 2) + px) * 2] = std::uint16_t(high ? u << 6 : u);
                    uv[(size_t(py) * (w / 2) + px) * 2 + 1] = std::uint16_t(high ? v << 6 : v);
                }
            sx::hdr::VideoPlanes planes;
            planes.high_aligned = high;
            if (!sx::hdr::planes_upload(&planes, w, h, y.data(), uv.data()))
            {
                std::fprintf(stderr, "sdrconv: planes refused\n");
                return 1;
            }
            sx::hdr::SdrTarget target;
            if (!sx::hdr::sdr_convert(&target, planes, ow, oh, bt601))
            {
                std::fprintf(stderr, "sdrconv: convert failed\n");
                return 1;
            }
            std::vector<std::uint8_t> gpu(size_t(ow) * oh * 4), cpu(size_t(ow) * oh * 4);
            glBindFramebuffer(GL_FRAMEBUFFER, target.fbo);
            glReadPixels(0, 0, ow, oh, GL_RGBA, GL_UNSIGNED_BYTE, gpu.data());
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            YuvPicture yp;
            yp.y = reinterpret_cast<const std::uint8_t*>(y.data());
            yp.u = reinterpret_cast<const std::uint8_t*>(uv.data());
            yp.y_stride = yp.c_stride = w * 2;
            yp.interleaved = true;
            yp.wide = true;
            yp.shift = high ? 6 : 0;
            yp.width = w;
            yp.height = h;
            yuv_to_rgba(yp, bt601 ? YuvColors::Bt601 : YuvColors::Bt709, 2, cpu.data(), ow, 0, oh);
            int worst = 0;
            long sum = 0;
            for (size_t i = 0; i < gpu.size(); ++i)
                if (i % 4 != 3)
                {
                    const int d = std::abs(int(gpu[i]) - int(cpu[i]));
                    worst = std::max(worst, d);
                    sum += d;
                }
            std::printf("sdrconv %s %s: GPU vs CPU, largest difference %d of 255, mean %.2f\n", high ? "high-aligned" : "low-aligned",
                        bt601 ? "BT.601" : "BT.709", worst, double(sum) / double(gpu.size() * 3 / 4));
            worst_all = std::max(worst_all, worst);
            sx::hdr::sdr_release(&target);
        }
        std::printf(worst_all <= 6 ? "sdrconv OK\n" : "sdrconv FAILED\n");
        return worst_all <= 6 ? 0 : 1;
    }
    if (wants("subtitles"))
    {
        // What a subtitle looks like in each script, as the player draws it (text in logical order, as the cues hold it).
        struct Case
        {
            const char *name, *text;
        };
        const Case cases[] = {
            {"arabic", "مرحبا بالعالم، كيف حالك اليوم؟\n(ضحك) لا أعرف ماذا أقول 25 دقيقة"},
            {"arabic-name-first", "Tom قال لي إن الطقس جميل اليوم في المدينة، أليس كذلك؟"},
            {"hebrew", "שלום עולם, מה שלומך היום?\nאני לא יודע מה לומר (בבקשה)"},
            {"japanese", "千と千尋の神隠し\n「どうして私がこんなことをしなければならないの？」"},
            {"japanese-long", "これは非常に長い日本語の字幕で、画面の幅に収まるように自動的に折り返されなければなりません。そうでなければ読めません。"},
            {"chinese-simplified", "你好，世界！这是一个简体中文字幕的例子。\n我们必须确保它显示正确。"},
            {"chinese-traditional", "你好，世界！這是一個繁體中文字幕的例子。\n我們必須確保它顯示正確。"},
            {"korean", "안녕하세요, 세계! 오늘 기분이 어떠세요?\n한국어 자막이 올바르게 표시되어야 합니다."},
            {"thai", "สวัสดีชาวโลก วันนี้คุณเป็นอย่างไรบ้าง\nคำบรรยายภาษาไทยต้องแสดงอย่างถูกต้อง"},
            {"english-long", "This is a very long English subtitle line that goes on and on and must be wrapped to fit on the screen without running off either side of it."},
            {"mixed", "Matrix – ماتريكس (1999) · マトリックス · 黑客帝国"},
        };
        const std::uint32_t frame = sx::preview::load_frame(rig.renderer, art, 1);
        sx::PlayerUi ui(env);
        ui.state = sx::preview::sample_player();
        ui.state.controls = false;
        for (const Case &c : cases)
        {
            ui.state.line = c.text;
            ui.snap();
            for (int i = 0; i < 4; ++i)
                ui.update(kDt);
            hui::gfx::DrawList list;
            hui::gfx::BackdropSpec backdrop;
            list.image(frame, {0, 0, 1920, 1080}, kFullUv, hui::gfx::Color::rgb(0xffffff));
            ui.draw(list);
            rig.shot((std::string("subtitle-") + c.name).c_str(), backdrop, list);
        }
    }
    if (wants("player"))
    {
        const std::uint32_t frame = sx::preview::load_frame(rig.renderer, art, 1);
        sx::PlayerUi ui(env);
        ui.state = sx::preview::sample_player();
        const auto run = [&](int frames) {
            for (int i = 0; i < frames; ++i)
                ui.update(kDt);
        };
        const auto shot = [&](const char *name) {
            hui::gfx::DrawList list;
            hui::gfx::BackdropSpec backdrop;
            list.image(frame, {0, 0, 1920, 1080}, kFullUv, hui::gfx::Color::rgb(0xffffff));
            ui.draw(list);
            rig.shot(name, backdrop, list);
        };
        ui.state.controls = true;
        ui.snap();
        run(10);
        shot("player-1-controls");
        ui.state.controls = false;
        ui.snap();
        run(10);
        shot("player-2-subtitles-only");
        ui.state.paused = true;
        ui.state.controls = true;
        ui.snap();
        run(10);
        shot("player-3-paused");
        ui.state.paused = false;
        ui.state.controls = false;
        ui.state.buffering = 62;
        ui.snap();
        run(30);
        shot("player-4-buffering");
        ui.state.buffering = -1;
        ui.state.menu = true;
        ui.state.controls = true;
        ui.state.menu_column = 1;
        ui.state.menu_row = 2;
        ui.snap();
        run(10);
        shot("player-5-track-menu");
    }
    if (wants("launch"))
    {
        const std::uint32_t backdrop = sx::preview::load_frame(rig.renderer, art, 1);
        const std::uint32_t poster = sx::preview::load_poster(rig.renderer, art, 1);
        sx::LaunchState state = sx::preview::sample_launch(backdrop, poster);
        hui::gfx::BackdropSpec backdrop_spec;
        {
            hui::gfx::DrawList list;
            sx::draw_launch(list, env, state, 1.2f, 1.0f);
            rig.shot("launch-1-buffering", backdrop_spec, list);
        }
        // Earlier in the start: the buffer is not filling yet.
        state.stage = 1;
        state.progress = -1.0f;
        state.done = "0%";
        state.peers = "4";
        state.speed = "310 KB/s";
        {
            hui::gfx::DrawList list;
            sx::draw_launch(list, env, state, 1.2f, 1.0f);
            rig.shot("launch-2-connecting", backdrop_spec, list);
        }
    }    if (wants("scripts"))
    {
        hui::gfx::DrawList list;
        hui::gfx::BackdropSpec backdrop;
        sx::preview::draw_scripts(list, backdrop, fonts);
        rig.shot("scripts-1-sample", backdrop, list);
    }    if (wants("worst"))
    {
        sx::preview::WorstCases worst;
        if (!sx::preview::load_worst(rig.renderer, art, &worst))
            return 1;
        {
            sx::Board page(env);
            page.set_content(&worst.board);
            page.enter();
            settle(page, 90);
            shoot(page, "worst-board-1-hero");
            press(page, Direction::down);
            shoot(page, "worst-board-2-long-row");
            press(page, Direction::right);
            press(page, Direction::right);
            press(page, Direction::right);
            press(page, Direction::right);
            shoot(page, "worst-board-3-no-art");
            press(page, Direction::down);
            press(page, Direction::down);
            shoot(page, "worst-board-4-empty-row");
        }
        {
            sx::Discover page(env);
            page.set_content(&worst.discover);
            page.enter();
            settle(page, 90);
            shoot(page, "worst-discover-1");
            press(page, Direction::right);
            press(page, Direction::right);
            press(page, Direction::right);
            press(page, Direction::down);
            shoot(page, "worst-discover-2");
        }
        {
            sx::Library page(env);
            page.set_content(&worst.library);
            page.enter();
            settle(page, 90);
            shoot(page, "worst-library-1");
        }
        {
            sx::Addons page(env);
            page.set_content(&worst.addons);
            page.enter();
            settle(page, 90);
            shoot(page, "worst-addons-1");
            press(page, Direction::right);
            shoot(page, "worst-addons-2");
        }
        {
            sx::Settings page(env);
            page.set_content(&worst.settings);
            page.enter();
            settle(page, 90);
            shoot(page, "worst-settings-1");
            press(page, Direction::down);
            shoot(page, "worst-settings-2");
        }
        {
            sx::Detail page(env);
            page.set_content(&worst.detail);
            page.enter();
            settle(page, 90);
            shoot(page, "worst-detail-1-episodes");
            tap(page, hui::Action::confirm, 70);
            shoot(page, "worst-detail-2-streams");
        }
        {
            sx::PlayerUi ui(env);
            ui.state = worst.player;
            const std::uint32_t frame = sx::preview::load_frame(rig.renderer, art, 1);
            const auto run = [&](int frames) {
                for (int i = 0; i < frames; ++i)
                    ui.update(kDt);
            };
            const auto shot = [&](const char *name) {
                hui::gfx::DrawList list;
                hui::gfx::BackdropSpec backdrop;
                list.image(frame, {0, 0, 1920, 1080}, kFullUv, hui::gfx::Color::rgb(0xffffff));
                ui.draw(list);
                rig.shot(name, backdrop, list);
            };
            ui.state.controls = true;
            ui.snap();
            run(10);
            shot("worst-player-1");
            ui.state.menu = true;
            ui.state.menu_column = 1;
            ui.snap();
            run(10);
            shot("worst-player-2-tracks");
        }
        {
            hui::gfx::DrawList list;
            hui::gfx::BackdropSpec backdrop;
            sx::draw_launch(list, env, worst.launch, 1.2f, 1.0f);
            rig.shot("worst-launch-1", backdrop, list);
        }
    }
    // The real app against the real addons (needs the network): drives the shell with
    // button presses and takes pictures of what the session shows. Not part of "all".
    if (only == "live")
    {
        const std::string data_dir = "/root/live-data";
        hui::save::ensure_directory(data_dir);
        sx::Session session;
        if (!session.init(icons_dir + "/../..", data_dir, &rig.renderer))
            return 1;
        sx::Shell shell(env, session);
        const auto frame = [&](const hui::InputFrame &in) {
            session.update(kDt, shell.cursor());
            env.clock = session.clock;
            shell.update(in, kDt);
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        };
        const auto wait = [&](double seconds) {
            for (int i = 0; i < static_cast<int>(seconds * 60.0); ++i)
                frame(idle_input());
        };
        const auto send = [&](hui::Action action, Direction nav, int frames) {
            hui::InputFrame in = idle_input();
            in.nav = nav;
            if (action != hui::Action::count)
                in.pressed = hui::action_bit(action);
            frame(in);
            for (int i = 1; i < frames; ++i)
                frame(idle_input());
        };
        const auto look = [&](const char *name) {
            hui::gfx::DrawList list;
            hui::gfx::BackdropSpec backdrop;
            shell.draw(list, backdrop);
            rig.shot(name, backdrop, list);
        };
        const hui::Action none = hui::Action::count;
        for (int i = 0; i < 40 * 60 && session.board.rows.size() < 3; ++i)
            frame(idle_input());
        std::fprintf(stderr, "board rows: %zu, status: %s\n", session.board.rows.size(), session.board.status.c_str());
        wait(10);
        look("live-1-board");
        send(none, Direction::right, 70);
        send(none, Direction::right, 70);
        wait(3);
        look("live-2-board-moved");
        send(none, Direction::down, 80);
        wait(4);
        look("live-3-board-series-row");
        // The series row: open a title
        send(hui::Action::confirm, Direction::none, 60);
        wait(8);
        look("live-4-detail-series");
        send(hui::Action::confirm, Direction::none, 60); // episode -> streams
        wait(12);
        look("live-5-streams");
        send(hui::Action::back, Direction::none, 60);
        send(hui::Action::page_next, Direction::none, 60);
        wait(3);
        look("live-6-season-2");
        send(hui::Action::back, Direction::none, 60); // out of the page
        send(none, Direction::left, 70);
        send(none, Direction::down, 40);
        send(hui::Action::confirm, Direction::none, 80); // Discover
        wait(10);
        look("live-7-discover");
        send(none, Direction::up, 60);
        send(none, Direction::up, 60);
        send(hui::Action::confirm, Direction::none, 60); // a filter's list
        wait(1);
        look("live-8-dropdown");
        send(hui::Action::back, Direction::none, 60);
        send(none, Direction::down, 60);
        send(none, Direction::left, 70);
        send(none, Direction::down, 40);
        send(none, Direction::down, 40);
        send(hui::Action::confirm, Direction::none, 80); // Library
        wait(3);
        look("live-9-library");
        send(none, Direction::left, 70);
        send(none, Direction::down, 40);
        send(none, Direction::down, 40);
        send(hui::Action::confirm, Direction::none, 80); // Addons
        wait(4);
        look("live-10-addons");
        send(none, Direction::left, 70);
        send(none, Direction::down, 40);
        send(hui::Action::confirm, Direction::none, 80); // Settings
        wait(1);
        look("live-11-settings");
        send(none, Direction::up, 30);
        send(hui::Action::confirm, Direction::none, 60); // the account row: sign in
        wait(6);
        look("live-12-sign-in");
        session.shutdown();
    }
    // The start when no one is signed in (needs the network): a fresh app, the boot, the sign-in screen with a real
    // code and its wall, then Circle to the Board. Not part of "all".
    if (only == "startsignin")
    {
        const char *home = std::getenv("HOME");
        const std::string data_dir = std::string(home ? home : "/tmp") + "/signin-data";
        std::filesystem::remove_all(data_dir);
        hui::save::ensure_directory(data_dir);
        sx::Session session;
        if (!session.init(icons_dir + "/../..", data_dir, &rig.renderer))
            return 1;
        sx::Shell shell(env, session);
        double seconds = 0.0;
        const auto frame = [&](const hui::InputFrame &in) {
            session.update(kDt, shell.cursor());
            env.clock = session.clock;
            shell.update(in, kDt);
            seconds += kDt;
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        };
        const auto run_for = [&](int frames) {
            for (int i = 0; i < frames; ++i)
                frame(idle_input());
        };
        const auto look = [&](const char *name) {
            hui::gfx::DrawList list;
            hui::gfx::BackdropSpec backdrop;
            shell.draw(list, backdrop);
            rig.shot(name, backdrop, list);
        };
        std::fprintf(stderr, "start: sign-in screen asked for: %d\n", session.sign_in_screen ? 1 : 0);
        run_for(60);
        look("start-1-boot");
        for (int i = 0; i < 15 * 60 && !session.boot_ready(); ++i)
            frame(idle_input());
        std::size_t ready = 0;
        for (const std::uint32_t id : session.wall)
            ready += id != 0 ? 1 : 0;
        std::fprintf(stderr, "start: ready after %.1f s: qr %s, code '%s', wall %zu posters (%zu there), rows %zu\n", seconds,
                     session.sign_in.qr != 0 ? "yes" : "no", session.sign_in.code.c_str(), session.wall.size(), ready,
                     session.board.rows.size());
        run_for(14);
        look("start-2-handoff");
        run_for(46);
        look("start-3-sign-in");
        run_for(120);
        look("start-4-two-seconds-later");
        hui::InputFrame circle = idle_input();
        circle.pressed = hui::action_bit(hui::Action::back);
        frame(circle);
        std::fprintf(stderr, "start: after Circle: sign-in screen %d, sign-in open %d\n", session.sign_in_screen ? 1 : 0,
                     session.sign_in_open ? 1 : 0);
        run_for(5);
        look("start-5-leaving");
        run_for(90);
        look("start-6-board");
        session.shutdown();
    }
    // Moving the Board's cursor to a title whose wallpaper arrives late: the old one stays up until the new one is there,
    // then blends over half a second. Measured on the wallpaper's own part of the screen. Not part of "all".
    if (only == "wallfade")
    {
        sx::BoardContent content;
        if (!sx::preview::load_board(rig.renderer, art, &content))
            return 1;
        const std::uint32_t tex_a = content.rows[0].items[0].backdrop, tex_b = content.rows[0].items[1].backdrop;
        bool all_good = true;
        for (int row = 0; row < 2; ++row)
        {
            // The focused title's wallpaper changes from A to B, B arriving late (the cursor stays, so nothing else moves).
            const auto make = [&](std::uint32_t id) {
                content.rows[static_cast<std::size_t>(row)].items[0].backdrop = id;
            };
            const auto draw_mean = [&](sx::Board &board) {
                hui::gfx::DrawList list;
                hui::gfx::BackdropSpec backdrop;
                board.draw(list, backdrop);
                if (rig.sync)
                    rig.sync();
                rig.renderer.begin();
                rig.renderer.backdrop(backdrop);
                rig.renderer.draw(list);
                glBindFramebuffer(GL_FRAMEBUFFER, rig.framebuffer);
                glClearColor(0, 0, 0, 1);
                glClear(GL_COLOR_BUFFER_BIT);
                rig.renderer.present(rig.framebuffer, rig.width, rig.height);
                glBindFramebuffer(GL_FRAMEBUFFER, rig.framebuffer);
                glReadPixels(0, 0, rig.width, rig.height, GL_RGBA, GL_UNSIGNED_BYTE, rig.pixels.data());
                double sum = 0;
                int n = 0;
                for (int y = 0; y < rig.height; y += 6)
                    for (int x = 0; x < rig.width; x += 6)
                    {
                        const unsigned char *px = &rig.pixels[(static_cast<std::size_t>(y) * rig.width + x) * 4];
                        sum += 0.2126 * px[0] + 0.7152 * px[1] + 0.0722 * px[2];
                        ++n;
                    }
                return sum / n;
            };
            const auto start_board = [&](sx::Board &board) {
                board.set_content(&content);
                board.enter();
                settle(board, 90);
                if (row == 1)
                {
                    hui::InputFrame down = idle_input();
                    down.nav = hui::Direction::down;
                    board.update(down, kDt);
                }
                settle(board, 150);
            };
            // What the screen is when B has been the wallpaper all along.
            make(tex_b);
            double fresh_b = 0;
            {
                sx::Board fresh(env);
                start_board(fresh);
                fresh_b = draw_mean(fresh);
            }
            make(tex_a);
            sx::Board board(env);
            start_board(board);
            std::vector<double> lum;
            lum.push_back(draw_mean(board));
            const double before = lum.back();
            make(0); // the next picture is not here yet
            for (int i = 0; i < 40; ++i)
            {
                board.update(idle_input(), kDt);
                lum.push_back(draw_mean(board));
            }
            const std::size_t arrived_at = lum.size();
            make(tex_b);
            for (int i = 0; i < 60; ++i)
            {
                board.update(idle_input(), kDt);
                lum.push_back(draw_mean(board));
            }
            const double after = lum.back();
            double held = 0, outside = 0, biggest = 0;
            for (std::size_t i = 0; i < arrived_at; ++i)
                held = std::max(held, std::fabs(lum[i] - before));
            for (std::size_t i = arrived_at; i < lum.size(); ++i)
            {
                outside = std::max(outside, std::max(std::min(before, after) - lum[i], lum[i] - std::max(before, after)));
                if (i > arrived_at)
                    biggest = std::max(biggest, std::fabs(lum[i] - lum[i - 1]));
            }
            const double change = std::fabs(after - before);
            std::printf("wallfade row %d: A %.2f -> B %.2f (B all along: %.2f), held-up deviation %.3f, outside range %.3f, last step %.3f, "
                        "biggest step %.3f of %.2f\n",
                        row, before, after, fresh_b, held, outside, std::fabs(lum.back() - lum[lum.size() - 2]), biggest, change);
            const bool good = change > 1.0 && held < 0.05 && outside < 0.05 * change + 0.05 &&
                              std::fabs(after - fresh_b) < 0.02 * change + 0.02 && biggest < 0.2 * change;
            std::printf(good ? "wallfade row %d OK\n" : "wallfade row %d FAILED\n", row);
            all_good = all_good && good;
        }
        if (!all_good)
            return 1;
    }
    // The sign-in approved on the phone (simulated): the boot animation must come back and stay until the account's Board
    // has answered, for at least 2.5 s, then lift onto Home. Not part of "all".
    if (only == "startlogin")
    {
        const char *home = std::getenv("HOME");
        const std::string data_dir = std::string(home ? home : "/tmp") + "/signin-data";
        std::filesystem::remove_all(data_dir);
        hui::save::ensure_directory(data_dir);
        sx::Session session;
        if (!session.init(icons_dir + "/../..", data_dir, &rig.renderer))
            return 1;
        sx::Shell shell(env, session);
        double seconds = 0;
        const auto frame = [&](const hui::InputFrame &in) {
            session.update(kDt, shell.cursor());
            env.clock = session.clock;
            shell.update(in, kDt);
            seconds += kDt;
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        };
        const auto look = [&](const char *name) {
            hui::gfx::DrawList list;
            hui::gfx::BackdropSpec backdrop;
            shell.draw(list, backdrop);
            rig.shot(name, backdrop, list);
        };
        for (int i = 0; i < 15 * 60 && !session.boot_ready(); ++i)
            frame(idle_input());
        for (int i = 0; i < 90; ++i)
            frame(idle_input());
        look("login-1-sign-in");
        sx::preview::test_login_done(session, "test-key");
        const double signed_at = seconds;
        for (int i = 0; i < 20; ++i)
            frame(idle_input());
        std::fprintf(stderr, "login: 0.3 s after sign-in: boot_ready %d\n", session.boot_ready() ? 1 : 0);
        look("login-2-boot-again");
        for (int i = 0; i < 12 * 60 && !session.boot_ready(); ++i)
            frame(idle_input());
        std::fprintf(stderr, "login: the wait lasted %.1f s\n", seconds - signed_at);
        for (int i = 0; i < 20; ++i)
            frame(idle_input());
        look("login-3-lifting");
        for (int i = 0; i < 90; ++i)
            frame(idle_input());
        look("login-4-board");
        session.shutdown();
    }
    // Circle on the sign-in screen the moment it can be pressed, before the Board is ready: the boot animation must come
    // back until the Board is, then lift onto it. Not part of "all".
    if (only == "startquick")
    {
        const char *home = std::getenv("HOME");
        const std::string data_dir = std::string(home ? home : "/tmp") + "/signin-data";
        std::filesystem::remove_all(data_dir);
        hui::save::ensure_directory(data_dir);
        sx::Session session;
        if (!session.init(icons_dir + "/../..", data_dir, &rig.renderer))
            return 1;
        sx::Shell shell(env, session);
        const auto frame = [&](const hui::InputFrame &in) {
            session.update(kDt, shell.cursor());
            env.clock = session.clock;
            shell.update(in, kDt);
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        };
        const auto look = [&](const char *name) {
            hui::gfx::DrawList list;
            hui::gfx::BackdropSpec backdrop;
            shell.draw(list, backdrop);
            rig.shot(name, backdrop, list);
        };
        for (int i = 0; i < 15 * 60 && !session.boot_ready(); ++i)
            frame(idle_input());
        for (int i = 0; i < 24; ++i)
            frame(idle_input());
        look("quick-1-sign-in");
        hui::InputFrame circle = idle_input();
        circle.pressed = hui::action_bit(hui::Action::back);
        frame(circle);
        std::fprintf(stderr, "quick: Circle pressed; boot_ready %d, rows %zu\n", session.boot_ready() ? 1 : 0, session.board.rows.size());
        for (int i = 0; i < 12; ++i)
            frame(idle_input());
        look("quick-2-just-after");
        int frames = 0;
        for (; frames < 12 * 60 && !session.boot_ready(); ++frames)
            frame(idle_input());
        std::fprintf(stderr, "quick: waited %.2f s more for the Board\n", frames / 60.0);
        look("quick-3-waiting-ended");
        for (int i = 0; i < 20; ++i)
            frame(idle_input());
        look("quick-4-lifting");
        for (int i = 0; i < 90; ++i)
            frame(idle_input());
        look("quick-5-board");
        session.shutdown();
    }
    // Playing a real stream (needs the network): opens one title through the session,
    // starts a stream and looks at what the player shows. Not part of "all".
    if (only == "play")
    {
        const std::string data_dir = "/root/live-data";
        hui::save::ensure_directory(data_dir);
        sx::Session session;
        if (!session.init(icons_dir + "/../..", data_dir, &rig.renderer))
            return 1;
        sx::Shell shell(env, session);
        const auto frame = [&](const hui::InputFrame &in) {
            session.update(kDt, shell.cursor());
            env.clock = session.clock;
            shell.update(in, kDt);
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        };
        const auto wait = [&](double seconds) {
            for (int i = 0; i < static_cast<int>(seconds * 60.0); ++i)
                frame(idle_input());
        };
        const auto look = [&](const char *name) {
            hui::gfx::DrawList list;
            hui::gfx::BackdropSpec backdrop;
            shell.draw(list, backdrop);
            rig.shot(name, backdrop, list);
        };
        wait(6);
        sx::Title t;
        t.id = std::getenv("PLAY_ID") ? std::getenv("PLAY_ID") : "tt1727587";
        t.type = std::getenv("PLAY_TYPE") ? std::getenv("PLAY_TYPE") : "movie";
        t.name = "Sintel";
        session.open_title(t);
        for (int i = 0; i < 25 * 60 && session.detail.streams.empty(); ++i)
            frame(idle_input());
        wait(8);
        std::fprintf(stderr, "streams: %zu (%s)\n", session.detail.streams.size(), session.detail.streams_status.c_str());
        int pick = -1;
        for (std::size_t i = 0; i < session.detail.streams.size() && pick < 0; ++i)
            if (session.detail.streams[i].resolution == "720p")
                pick = static_cast<int>(i);
        if (pick < 0)
            pick = 0;
        if (session.detail.streams.empty())
            return 1;
        std::fprintf(stderr, "playing #%d: %s | %s | %s\n", pick, session.detail.streams[static_cast<std::size_t>(pick)].name.c_str(),
                     session.detail.streams[static_cast<std::size_t>(pick)].detail.c_str(),
                     session.detail.streams[static_cast<std::size_t>(pick)].size.c_str());
        session.detail_play(pick, false);
        for (int i = 0; i < 90 * 60 && !(session.watching && !session.launch_open); ++i)
        {
            frame(idle_input());
            if (i % 300 == 0)
                std::fprintf(stderr, "starting: stage %d progress %.2f peers %s speed %s\n", session.launch.stage,
                             static_cast<double>(session.launch.progress), session.launch.peers.c_str(),
                             session.launch.speed.c_str());
            if (i == 600)
                look("play-1-launch");
        }
        std::fprintf(stderr, "player up: %d, position %.1f / %.1f\n", session.watching && !session.launch_open ? 1 : 0,
                     session.playing.position, session.playing.duration);
        wait(5);
        look("play-2-playing");
        wait(10);
        std::fprintf(stderr, "position %.1f / %.1f, audio '%s', subs '%s'\n", session.playing.position, session.playing.duration,
                     session.playing.audio.c_str(), session.playing.subs.c_str());
        look("play-3-later");
        hui::InputFrame menu = idle_input();
        menu.nav = Direction::up;
        frame(menu);
        wait(2);
        look("play-4-menu");
        session.shutdown();
    }
    // A search through the real addons (the keyboard is the console's, so the text is given).
    if (only == "search")
    {
        const std::string data_dir = "/root/live-data";
        hui::save::ensure_directory(data_dir);
        sx::Session session;
        if (!session.init(icons_dir + "/../..", data_dir, &rig.renderer))
            return 1;
        sx::Shell shell(env, session);
        const auto frame = [&](const hui::InputFrame &in) {
            session.update(kDt, shell.cursor());
            env.clock = session.clock;
            shell.update(in, kDt);
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        };
        const auto wait = [&](double seconds) {
            for (int i = 0; i < static_cast<int>(seconds * 60.0); ++i)
                frame(idle_input());
        };
        const auto look = [&](const char *name) {
            hui::gfx::DrawList list;
            hui::gfx::BackdropSpec backdrop;
            shell.draw(list, backdrop);
            rig.shot(name, backdrop, list);
        };
        wait(8);
        session.start_search("lantern");
        wait(2);
        look("search-1-searching");
        wait(8);
        look("search-2-results");
        session.start_search("zzzxqwj nothing");
        wait(8);
        look("search-3-none");
        session.shutdown();
    }
    // The Library with nothing in it, and the rail's behaviour when pages change.
    if (only == "emptylib")
    {
        const std::string data_dir = "/root/empty-data";
        hui::save::ensure_directory(data_dir);
        sx::Session session;
        if (!session.init(icons_dir + "/../..", data_dir, &rig.renderer))
            return 1;
        sx::Shell shell(env, session);
        const auto frame = [&](const hui::InputFrame &in) {
            session.update(kDt, shell.cursor());
            env.clock = session.clock;
            shell.update(in, kDt);
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        };
        const auto send = [&](hui::Action action, Direction nav, int frames) {
            hui::InputFrame in = idle_input();
            in.nav = nav;
            if (action != hui::Action::count)
                in.pressed = hui::action_bit(action);
            frame(in);
            for (int i = 1; i < frames; ++i)
                frame(idle_input());
        };
        const auto look = [&](const char *name) {
            hui::gfx::DrawList list;
            hui::gfx::BackdropSpec backdrop;
            shell.draw(list, backdrop);
            rig.shot(name, backdrop, list);
        };
        const hui::Action none = hui::Action::count;
        send(none, Direction::none, 300);
        // Board -> Discover -> Library by the rail, then back to Discover: the rail must rest on the page.
        send(none, Direction::left, 60);
        send(none, Direction::down, 40);
        send(hui::Action::confirm, Direction::none, 60); // Discover
        send(none, Direction::left, 60);
        send(none, Direction::down, 40);
        look("empty-1-rail-on-library");
        send(hui::Action::confirm, Direction::none, 90); // Library
        look("empty-2-library");
        send(none, Direction::left, 60);
        send(none, Direction::up, 40);
        send(hui::Action::confirm, Direction::none, 90); // back to Discover
        look("empty-3-discover-again");
        session.shutdown();
    }
    // The Board's hero with real artwork, for comparing how the picture ends.
    if (only == "hero")
    {
        const std::string data_dir = "/root/live-data";
        sx::Session session;
        if (!session.init(icons_dir + "/../..", data_dir, &rig.renderer))
            return 1;
        sx::Shell shell(env, session);
        const auto frame = [&](const hui::InputFrame &in) {
            session.update(kDt, shell.cursor());
            env.clock = session.clock;
            shell.update(in, kDt);
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        };
        for (int i = 0; i < 40 * 60 && session.board.rows.size() < 3; ++i)
            frame(idle_input());
        for (int i = 0; i < 12 * 60; ++i)
            frame(idle_input());
        const auto look = [&](const char *name) {
            hui::gfx::DrawList list;
            hui::gfx::BackdropSpec backdrop;
            shell.draw(list, backdrop);
            rig.shot(name, backdrop, list);
        };
        look("hero-a");
        hui::InputFrame down = idle_input();
        down.nav = Direction::down;
        frame(down);
        for (int i = 0; i < 100; ++i)
            frame(idle_input());
        for (int i = 0; i < 4 * 60; ++i)
            frame(idle_input());
        look("hero-b");
        session.shutdown();
    }
    // The Board's hero as the focus moves from title to title: stills and a frame from the middle of the swap.
    if (only == "hero2")
    {
        const std::string data_dir = "/root/live-data2";
        sx::Session session;
        if (!session.init(icons_dir + "/../..", data_dir, &rig.renderer))
            return 1;
        sx::Shell shell(env, session);
        const auto frame = [&](const hui::InputFrame &in) {
            session.update(kDt, shell.cursor());
            env.clock = session.clock;
            shell.update(in, kDt);
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        };
        const auto look = [&](const char *name) {
            hui::gfx::DrawList list;
            hui::gfx::BackdropSpec backdrop;
            shell.draw(list, backdrop);
            rig.shot(name, backdrop, list);
        };
        for (int i = 0; i < 40 * 60 && session.board.rows.size() < 3; ++i)
            frame(idle_input());
        for (int i = 0; i < 12 * 60; ++i)
            frame(idle_input());
        look("cycle-1");
        for (int k = 2; k <= 4; ++k)
        {
            hui::InputFrame right = idle_input();
            right.nav = Direction::right;
            frame(right);
            for (int i = 0; i < 10; ++i)
                frame(idle_input());
            if (k == 2)
                look("cycle-swap-mid");
            for (int i = 0; i < 6 * 60; ++i)
                frame(idle_input());
            look(k == 2 ? "cycle-2" : k == 3 ? "cycle-3" : "cycle-4");
        }
        session.shutdown();
    }
    if (only == "cwstreams")
    {
        // Cross on a card of Continue Watching with nothing remembered: that episode's streams at once.
        const std::string data_dir = "/root/live-data2";
        sx::Session session;
        if (!session.init(icons_dir + "/../..", data_dir, &rig.renderer))
            return 1;
        sx::Shell shell(env, session);
        const auto frame = [&](const hui::InputFrame &in) {
            session.update(kDt, shell.cursor());
            env.clock = session.clock;
            shell.update(in, kDt);
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        };
        const auto look = [&](const char *name) {
            hui::gfx::DrawList list;
            hui::gfx::BackdropSpec backdrop;
            shell.draw(list, backdrop);
            rig.shot(name, backdrop, list);
        };
        for (int i = 0; i < 40 * 60 && session.board.rows.size() < 3; ++i)
            frame(idle_input());
        for (int i = 0; i < 6 * 60; ++i)
            frame(idle_input());
        look("cw-1-board");
        hui::InputFrame ok = idle_input();
        ok.pressed = hui::action_bit(hui::Action::confirm);
        frame(ok);
        for (int i = 0; i < 14 * 60; ++i)
            frame(idle_input());
        look("cw-2-after-cross");
        session.shutdown();
    }
    if (wants("boot"))
    {
        for (const auto &[name, t, out] : {std::tuple{"boot-1-first", 0.0f, 0.0f}, std::tuple{"boot-2-glow", 0.7f, 0.0f},
                                         std::tuple{"boot-3-waiting", 2.2f, 0.0f}, std::tuple{"boot-4-lifting", 2.6f, 0.45f}})
        {
            hui::gfx::DrawList list;
            hui::gfx::BackdropSpec backdrop;
            sx::apply_page_backdrop(backdrop, t);
            sx::draw_boot(list, env, t, out);
            rig.shot(name, backdrop, list);
        }
        // The page's light alone: what the console's start-up picture is made from.
        hui::gfx::DrawList empty;
        hui::gfx::BackdropSpec light;
        sx::apply_page_backdrop(light, 0.0f);
        rig.shot("bootlight", light, empty);
    }
    if (wants("overlays"))
    {
        sx::DiscoverContent discover;
        sx::LibraryContent library;
        sx::SettingsContent settings;
        if (!sx::preview::load_discover(rig.renderer, art, &discover) ||
            !sx::preview::load_library(rig.renderer, art, &library))
            return 1;
        sx::preview::load_settings(&settings);
        {
            sx::Discover page(env);
            page.set_content(&discover);
            page.enter();
            settle(page, 90);
            hui::gfx::DrawList list;
            hui::gfx::BackdropSpec backdrop;
            page.draw(list, backdrop);
            sx::DropdownState dd;
            dd.title = "Genre";
            dd.options = {"All genres", "Action", "Adventure", "Animation", "Comedy", "Crime", "Documentary", "Drama", "Family", "Fantasy", "History", "Horror"};
            dd.active = 0;
            dd.selected = 3;
            sx::draw_dropdown(list, env, dd, 1.0f);
            rig.shot("overlay-1-dropdown", backdrop, list);
        }
        {
            sx::Settings page(env);
            page.set_content(&settings);
            page.enter();
            settle(page, 90);
            hui::gfx::DrawList list;
            hui::gfx::BackdropSpec backdrop;
            page.draw(list, backdrop);
            sx::SignInState in;
            in.qr = sx::preview::load_texture(rig.renderer, art + "/qr.png");
            in.link = "stremio.com/link";
            in.code = "A7KP";
            in.status = "Waiting for you to sign in";
            sx::draw_sign_in(list, env, in, 0.7f, 1.0f);
            rig.shot("overlay-2-sign-in", backdrop, list);
        }
        {
            // Torbox's code sign-in: the same dialog, its own title and a longer code.
            sx::Settings page(env);
            page.set_content(&settings);
            page.enter();
            settle(page, 90);
            hui::gfx::DrawList list;
            hui::gfx::BackdropSpec backdrop;
            page.draw(list, backdrop);
            sx::SignInState in;
            in.title = "Connect Torbox";
            in.logo = false;
            in.qr = sx::preview::load_texture(rig.renderer, art + "/qr.png");
            in.link = "tor.box/link";
            in.code = "ABCD-1234";
            in.status = "Waiting for you to approve it on your phone or computer...";
            sx::draw_sign_in(list, env, in, 0.7f, 1.0f);
            rig.shot("overlay-2b-torbox", backdrop, list);
        }
        {
            sx::Library page(env);
            page.set_content(&library);
            page.enter();
            settle(page, 90);
            hui::gfx::DrawList list;
            hui::gfx::BackdropSpec backdrop;
            page.draw(list, backdrop);
            sx::ToastState toast;
            toast.message = "No streams found for this episode. Try another addon.";
            toast.error = true;
            sx::draw_toast(list, env, toast, 1.0f);
            rig.shot("overlay-4-toast-error", backdrop, list);
            list.clear();
            page.draw(list, backdrop);
            toast.message = "Addons reloaded: 7 addons ready";
            toast.error = false;
            sx::draw_toast(list, env, toast, 1.0f);
            rig.shot("overlay-5-toast-info", backdrop, list);
        }
    }

    if (wants("profiles"))
    {
        sx::ProfilesState st;
        st.items = {{"Alex", "alex@example.com", 0x7b5bf5}, {"Maria", "maria@example.com", 0xc0475c},
                    {"Kids", "kids@example.com", 0x1fa37a}, {"Nikos", "", 0xe0803a}};
        const auto picker = [&](float focus, bool leave, const char *name) {
            hui::gfx::DrawList list;
            hui::gfx::BackdropSpec backdrop;
            sx::apply_page_backdrop(backdrop, 0.0f);
            st.focus = focus;
            st.can_leave = leave;
            sx::draw_profile_picker(list, env, st, 1.0f);
            rig.shot(name, backdrop, list);
        };
        picker(0.0f, false, "profiles-1-first");
        picker(1.0f, false, "profiles-2-second");
        picker(1.45f, false, "profiles-3-gliding");
        picker(3.0f, true, "profiles-4-last-not-signed-in");
        picker(4.0f, true, "profiles-5-add");
        const auto editor = [&](int index, bool is_new, int row, const char *name) {
            hui::gfx::DrawList list;
            hui::gfx::BackdropSpec backdrop;
            sx::apply_page_backdrop(backdrop, 0.0f);
            st.focus = static_cast<float>(index < 0 ? 4 : index);
            st.can_leave = true;
            sx::draw_profile_picker(list, env, st, 1.0f);
            sx::EditorState e;
            e.is_new = is_new;
            e.profile = is_new ? sx::ProfileEntry{"Profile 5", "", 0x2fb0b8} : st.items[static_cast<std::size_t>(index)];
            e.colour_index = sx::profile_colour_index(e.profile.colour);
            e.signed_in = !e.profile.email.empty();
            e.row = row;
            sx::draw_profile_editor(list, env, e, 1.0f);
            rig.shot(name, backdrop, list);
        };
        editor(0, false, 1, "profiles-6-editor");
        editor(3, false, 2, "profiles-7-editor-not-signed-in");
        editor(-1, true, 0, "profiles-8-editor-new");
        {
            sx::SettingsContent sc;
            sx::preview::load_settings(&sc);
            sx::SettingRow r;
            r.kind = sx::SettingRow::Kind::value;
            r.label = "Change profile";
            r.value = "Alex";
            r.help = "Choose who is watching, or add a profile. Each profile has its own Stremio account, library, Continue Watching and addons. Press Cross.";
            sc.rows.insert(sc.rows.begin() + 1, r);
            sx::Settings sp(env);
            sp.set_content(&sc);
            sp.enter();
            settle(sp, 90);
            press(sp, Direction::down, 40);
            settle(sp, 30);
            shoot(sp, "profiles-9-settings-row");
        }
    }
    if (wants("update"))
    {
        sx::BoardContent content;
        if (!sx::preview::load_board(rig.renderer, art, &content))
            return 1;
        sx::Board board(env);
        board.set_content(&content);
        board.enter();
        settle(board, 90);
        const auto dialog = [&](sx::UpdateState u, float clock, const char *name) {
            hui::gfx::DrawList list;
            hui::gfx::BackdropSpec backdrop;
            board.draw(list, backdrop);
            sx::draw_update(list, env, u, clock, 1.0f);
            rig.shot(name, backdrop, list);
        };
        sx::UpdateState u;
        u.version = "1.1";
        u.current = "1.0";
        u.size = "29.5 MB";
        u.notes = {"Profiles: one Stremio account per person", "A new Who's watching? screen", "Update the app from inside the app"};
        dialog(u, 0.0f, "update-1-offer");
        u.selected = 1;
        dialog(u, 0.0f, "update-2-offer-later");
        u.selected = 0;
        u.test_build = true;
        dialog(u, 0.0f, "update-3-offer-test");
        u.phase = sx::UpdateState::Phase::downloading;
        u.progress = 0.42f;
        u.detail = "12.4 of 29.5 MB";
        u.left = "about 8 s left";
        dialog(u, 0.0f, "update-4-downloading");
        u.phase = sx::UpdateState::Phase::installing;
        dialog(u, 1.2f, "update-5-installing");
        u.phase = sx::UpdateState::Phase::closing;
        dialog(u, 0.0f, "update-6-closing");
        u.phase = sx::UpdateState::Phase::failed;
        u.message = "The payload loader did not answer (port 9021). Is it running?";
        dialog(u, 0.0f, "update-7-failed");
        u.phase = sx::UpdateState::Phase::current;
        dialog(u, 0.0f, "update-8-current");
    }
    if (wants("hdr"))
    {
        // The HDR final pass: a known video texture and a known interface, read back and unpacked.
        const int W = 1920, H = 1080;
        std::vector<std::uint32_t> words(960 * 540);
        auto pack = [](int r, int g, int b) { return std::uint32_t(r) | (std::uint32_t(g) << 10) | (std::uint32_t(b) << 20) | (3u << 30); };
        for (std::size_t i = 0; i < words.size(); ++i)
            words[i] = pack(600, 300, 900); // a PQ colour: R 600, G 300, B 900 of 1023
        std::uint32_t tex = 0;
        int tw = 0, th = 0;
        if (!sx::hdr::video_upload(&tex, &tw, &th, 960, 540, words.data()))
        {
            std::fprintf(stderr, "HDR TEST: 10-bit texture failed\n");
            return 1;
        }
        hui::gfx::DrawList list;
        list.rounded_rect({100, 100, 300, 200}, 0, hui::gfx::Color::rgb(0xffffff)); // white UI, outside the video
        list.rounded_rect({600, 400, 200, 200}, 0, hui::gfx::Color::rgb(0xffffff, 0.5f)); // half-white over the video
        rig.renderer.begin();
        rig.renderer.draw(list);
        const hui::gfx::Rect rect{480, 270, 960, 540}; // the video, centred
        glBindFramebuffer(GL_FRAMEBUFFER, rig.framebuffer);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        if (!sx::hdr::present(rig.renderer, W, H, tex, nullptr, rect, rig.framebuffer))
        {
            std::fprintf(stderr, "HDR TEST: present failed\n");
            return 1;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, rig.framebuffer);
        std::vector<unsigned char> px(static_cast<std::size_t>(W) * H * 4);
        glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        // (x, y) from the top left, as the picture is laid out; glReadPixels starts at the bottom.
        auto at = [&](int x, int y) {
            const unsigned char *p = &px[(static_cast<std::size_t>(H - 1 - y) * W + x) * 4];
            const std::uint32_t w = std::uint32_t(p[2]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[0]) << 16) | (std::uint32_t(p[3]) << 24);
            return w;
        };
        auto show = [&](const char *name, int x, int y) {
            const std::uint32_t w = at(x, y);
            std::fprintf(stderr, "HDR TEST %-22s R %4u G %4u B %4u A %u\n", name, w & 1023, (w >> 10) & 1023, (w >> 20) & 1023, w >> 30);
        };
        const std::uint32_t keep_v = at(1000, 300), keep_o = at(300, 800), keep_u = at(200, 200);
        show("video (expect 600 300 900)", 1000, 300);
        show("outside video (expect 0)", 300, 800);
        show("white UI (expect ~594)", 200, 200);
        show("half UI over video", 700, 500);
        show("video top-left corner", 485, 275);
        // The same through the planes path: Y 573 (limited range, 0.581 of the signal) with neutral colour: PQ grey 594.
        auto planes_test = [&](const char *name, bool high, bool hlg, int y10, int expect) {
            const int pw = 960, ph = 540;
            std::vector<std::uint16_t> yp(static_cast<std::size_t>(pw) * ph, static_cast<std::uint16_t>(y10 << (high ? 6 : 0)));
            std::vector<std::uint16_t> uvp(static_cast<std::size_t>(pw / 2) * (ph / 2) * 2, static_cast<std::uint16_t>(512 << (high ? 6 : 0)));
            sx::hdr::VideoPlanes planes;
            planes.high_aligned = high;
            planes.hlg = hlg;
            if (!sx::hdr::planes_upload(&planes, pw, ph, yp.data(), uvp.data()))
            {
                std::fprintf(stderr, "HDR TEST %s: planes failed\n", name);
                return false;
            }
            hui::gfx::DrawList empty;
            rig.renderer.begin();
            glBindFramebuffer(GL_FRAMEBUFFER, rig.framebuffer);
            glClear(GL_COLOR_BUFFER_BIT);
            if (!sx::hdr::present(rig.renderer, W, H, 0, &planes, rect, rig.framebuffer))
                return false;
            glBindFramebuffer(GL_FRAMEBUFFER, rig.framebuffer);
            glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            const std::uint32_t w = at(1000, 300);
            const int r = int(w & 1023), g = int((w >> 10) & 1023), b = int((w >> 20) & 1023);
            std::fprintf(stderr, "HDR TEST planes %-28s R %4d G %4d B %4d (expect ~%d)\n", name, r, g, b, expect);
            return std::abs(r - expect) <= 3 && std::abs(g - expect) <= 3 && std::abs(b - expect) <= 3;
        };
        bool planes_ok = planes_test("HDR10 low-aligned", false, false, 573, 594);
        planes_ok = planes_test("HDR10 high-aligned", true, false, 573, 594) && planes_ok;
        planes_ok = planes_test("HLG reference white", false, true, 721, 594) && planes_ok;
        const std::uint32_t v = keep_v, o = keep_o, u = keep_u;
        const bool ok = planes_ok && (v & 1023) == 600 && ((v >> 10) & 1023) == 300 && ((v >> 20) & 1023) == 900 && (v >> 30) == 3 && (o & 1023) == 0 &&
                        std::abs(int(u & 1023) - 594) <= 4;
        std::fprintf(stderr, ok ? "HDR TEST OK\n" : "HDR TEST FAIL\n");
        return ok ? 0 : 1;
    }
    return rig.ok ? 0 : 1;
}
