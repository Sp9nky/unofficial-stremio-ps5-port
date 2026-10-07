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
#include "page.hpp"
#include "player.hpp"
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
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <tuple>
#include <functional>
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
    return rig.ok ? 0 : 1;
}
