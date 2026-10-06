// Stremio for PS5: entry point, window, input and the frame loop.

#include <RmlUi/Core.h>
#include <SDL.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <string>

#include "app.h"
#ifdef PLATFORM_PS5
#include "pad_ps5.h"
#endif
#include "artcache.h"
#include "http.h"
#include "hwdec_ps5.h"
#include "render_sdl.h"
#include "tasks.h"
#include "util.h"

static const int kWidth = 1920, kHeight = 1080;

#ifdef PLATFORM_PS5_NATIVE
extern "C" int sceSystemServiceGetAppIdOfRunningBigApp(void);
extern "C" int sceSystemServiceKillApp(int app_id, int how, int reason, int core_dump);
extern "C" void ps5_load_modules(void);  // native/ps5_modules.c
#endif

static std::string find_base_dir(const char* argv0) {
#ifdef PLATFORM_PS5
	(void)argv0;
	return "/app0";  // the title's own folder, as every app sees it
#else
	std::vector<std::string> candidates;
	if (const char* env = getenv("STREMIO_BASE")) candidates.push_back(env);
	if (argv0 && strchr(argv0, '/')) candidates.push_back(path_dir(argv0));
	char cwd[1024];
	if (getcwd(cwd, sizeof(cwd))) candidates.push_back(cwd);
	for (auto& c : candidates)
		if (file_exists(c + "/assets/main.rml")) return c;
	return ".";
#endif
}

static std::string find_data_dir(int argc, char** argv) {
	for (int i = 1; i + 1 < argc; i++)
		if (!strcmp(argv[i], "--data") || !strcmp(argv[i], "-d")) return argv[i + 1];
#ifdef PLATFORM_PS5
	// The app's own storage (param.json downloadDataSize); /data is outside
	// the sandbox.
	return "/download0/stremio";
#else
	const char* home = getenv("HOME");
	return std::string(home ? home : ".") + "/.stremio-ps5";
#endif
}

// ---------------------------------------------------------------------------
// Input: buttons with repeat for directions.

struct Repeat {
	bool held = false;
	Btn btn = Btn::Up;
	double next = 0;
};

static void press(App& app, Repeat& rep, Btn b) {
	app.on_button(b);
	if (b == Btn::Up || b == Btn::Down || b == Btn::Left || b == Btn::Right) {
		rep.held = true;
		rep.btn = b;
		rep.next = now_seconds() + 0.40;
	}
}

static void release(Repeat& rep, Btn b) {
	if (rep.held && rep.btn == b) rep.held = false;
}

static bool map_controller_button(Uint8 button, Btn& out) {
	switch (button) {
	case SDL_CONTROLLER_BUTTON_A: out = Btn::Cross; return true;
	case SDL_CONTROLLER_BUTTON_B: out = Btn::Circle; return true;
	case SDL_CONTROLLER_BUTTON_X: out = Btn::Square; return true;
	case SDL_CONTROLLER_BUTTON_Y: out = Btn::Triangle; return true;
	case SDL_CONTROLLER_BUTTON_START: out = Btn::Options; return true;
	case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: out = Btn::L1; return true;
	case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: out = Btn::R1; return true;
	case SDL_CONTROLLER_BUTTON_LEFTSTICK: out = Btn::L3; return true;
	case SDL_CONTROLLER_BUTTON_RIGHTSTICK: out = Btn::R3; return true;
	case SDL_CONTROLLER_BUTTON_TOUCHPAD: out = Btn::Touchpad; return true;
	case SDL_CONTROLLER_BUTTON_DPAD_UP: out = Btn::Up; return true;
	case SDL_CONTROLLER_BUTTON_DPAD_DOWN: out = Btn::Down; return true;
	case SDL_CONTROLLER_BUTTON_DPAD_LEFT: out = Btn::Left; return true;
	case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: out = Btn::Right; return true;
	default: return false;
	}
}

static bool map_key(SDL_Keycode key, bool text_entry, Btn& out) {
	switch (key) {
	case SDLK_UP: out = Btn::Up; return true;
	case SDLK_DOWN: out = Btn::Down; return true;
	case SDLK_LEFT: out = Btn::Left; return true;
	case SDLK_RIGHT: out = Btn::Right; return true;
	case SDLK_RETURN:
	case SDLK_KP_ENTER: out = text_entry ? Btn::Options : Btn::Cross; return true;
	case SDLK_ESCAPE: out = Btn::Circle; return true;
	default: break;
	}
	if (text_entry) return false;  // letters are typed, not buttons
	switch (key) {
	case SDLK_BACKSPACE: out = Btn::Circle; return true;
	case SDLK_SPACE: out = Btn::Cross; return true;
	case SDLK_s: out = Btn::Square; return true;
	case SDLK_t: out = Btn::Triangle; return true;
	case SDLK_o:
	case SDLK_TAB: out = Btn::Options; return true;
	case SDLK_q: out = Btn::L1; return true;
	case SDLK_e: out = Btn::R1; return true;
	case SDLK_1: out = Btn::L2; return true;
	case SDLK_3: out = Btn::R2; return true;
	default: return false;
	}
}

// One Stremio at a time: a second launch finds the port taken and leaves.
static bool claim_single_instance() {
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return true;  // can't tell; run anyway
	sockaddr_in a;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons(41337);
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(fd, (sockaddr*)&a, sizeof(a)) != 0 || listen(fd, 1) != 0) {
		close(fd);
		return false;
	}
	return true;  // fd stays open for the life of the process
}

int main(int argc, char** argv) {
	// A closed connection must fail a write quietly instead of killing us.
	signal(SIGPIPE, SIG_IGN);

	std::string base = find_base_dir(argc > 0 ? argv[0] : nullptr);
	std::string data = find_data_dir(argc, argv);
	make_dirs(data);
	log_open(data + "/log.txt");

#ifndef PLATFORM_PS5  // on the console the system runs one copy of an app
	if (!claim_single_instance()) return 0;
#endif
	dlog("Stremio for PS5 starting; app files in %s, data in %s", base.c_str(), data.c_str());

#ifdef PLATFORM_PS5_NATIVE
	ps5_load_modules();  // the on-screen keyboard, before SDL polls it
	HwDecoder::load_module();  // the hardware video decoder
#endif
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
#ifdef PLATFORM_PS5
	// The controller is read by PadPS5, not SDL (see pad_ps5.h).
	const Uint32 sdl_systems = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS;
#else
	const Uint32 sdl_systems = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS;
#endif
	if (SDL_Init(sdl_systems) != 0) {
		dlog("SDL_Init: %s", SDL_GetError());
		return 1;
	}
	Uint32 wflags = SDL_WINDOW_SHOWN;
#ifdef PLATFORM_PS5
	wflags |= SDL_WINDOW_FULLSCREEN;
#else
	wflags |= SDL_WINDOW_RESIZABLE;
#endif
	int ww = kWidth, wh = kHeight;
#ifndef PLATFORM_PS5
	ww = kWidth * 2 / 3;
	wh = kHeight * 2 / 3;
#endif
	SDL_Window* window = SDL_CreateWindow("Stremio", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, ww, wh, wflags);
	if (!window) {
		dlog("SDL_CreateWindow: %s", SDL_GetError());
		return 1;
	}
	SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC);
	if (!renderer) renderer = SDL_CreateRenderer(window, -1, 0);
	// SDL reports only "Couldn't find matching render driver" when every
	// driver fails; ask each one for its own reason, then draw in software
	// straight into the window's surface.
	bool surface_renderer = false;
	if (!renderer) {
		for (int i = 0; i < SDL_GetNumRenderDrivers() && !renderer; i++) {
			SDL_RendererInfo di;
			SDL_GetRenderDriverInfo(i, &di);
			renderer = SDL_CreateRenderer(window, i, 0);
			if (!renderer) dlog("render driver %s: %s", di.name, SDL_GetError());
		}
	}
	if (!renderer) {
		SDL_Surface* surface = SDL_GetWindowSurface(window);
		if (!surface) dlog("SDL_GetWindowSurface: %s", SDL_GetError());
		int sw = 0, sh = 0;
		SDL_GetWindowSizeInPixels(window, &sw, &sh);
		dlog("window is %dx%d", sw, sh);
		renderer = surface ? SDL_CreateSoftwareRenderer(surface) : nullptr;
		surface_renderer = renderer != nullptr;
	}
	if (!renderer) {
		dlog("SDL_CreateRenderer: %s", SDL_GetError());
		return 1;
	}
	SDL_RendererInfo info;
	if (SDL_GetRendererInfo(renderer, &info) == 0) dlog("renderer: %s", info.name);
	SDL_RenderSetLogicalSize(renderer, kWidth, kHeight);
	SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

	for (int i = 0; i < SDL_NumJoysticks(); i++)
		if (SDL_IsGameController(i)) SDL_GameControllerOpen(i);

	http_init(base + "/ca-bundle.crt");
	g_tasks.start(6);
	g_art.start(data + "/art", 4);

	RenderSDL render(renderer);
	SystemSDL system;
	Rml::SetRenderInterface(&render);
	Rml::SetSystemInterface(&system);
	Rml::Initialise();
	const char* fonts[] = {"NotoSans-Regular.ttf", "NotoSans-Bold.ttf", "NotoSans-Italic.ttf"};
	for (auto* f : fonts)
		if (!Rml::LoadFontFace(base + "/fonts/" + f)) dlog("can't load font %s", f);
	Rml::LoadFontFace(base + "/fonts/NotoEmoji-VariableFont_wght.ttf", true);
	// Arabic letters (Noto Sans has none: white boxes, issue #1), with the
	// joined forms bidi.h produces. Fallbacks: used only for missing glyphs.
	for (auto* f : {"NotoNaskhArabicUI-Regular.ttf", "NotoNaskhArabicUI-Bold.ttf"})
		if (!Rml::LoadFontFace(base + "/fonts/" + f, true)) dlog("can't load font %s", f);

	Rml::Context* ctx = Rml::CreateContext("main", Rml::Vector2i(kWidth, kHeight));
	if (!ctx) {
		dlog("Rml::CreateContext failed");
		return 1;
	}
	ctx->SetDensityIndependentPixelRatio(kUiScale);

	App app;
	if (!app.init(ctx, renderer, base, data)) {
		dlog("app init failed");
		return 1;
	}

	Repeat rep;
	bool stick_x = false, stick_y = false, l2 = false, r2 = false;
	Btn stick_x_btn = Btn::Left, stick_y_btn = Btn::Up;
	bool running = true;
	double last_frame = now_seconds();

#ifdef PLATFORM_PS5
	PadPS5 pad;
	bool have_pad = pad.init();
#endif

	while (running && !app.wants_exit()) {
#ifdef PLATFORM_PS5
		if (have_pad)
			pad.poll([&](Btn b) { press(app, rep, b); }, [&](Btn b) { release(rep, b); });
#endif
		SDL_Event ev;
		while (SDL_PollEvent(&ev)) {
			Btn b;
			switch (ev.type) {
			case SDL_QUIT: running = false; break;
			case SDL_CONTROLLERDEVICEADDED: SDL_GameControllerOpen(ev.cdevice.which); break;
			case SDL_CONTROLLERBUTTONDOWN:
				if (map_controller_button(ev.cbutton.button, b)) press(app, rep, b);
				break;
			case SDL_CONTROLLERBUTTONUP:
				if (map_controller_button(ev.cbutton.button, b)) release(rep, b);
				break;
			case SDL_CONTROLLERAXISMOTION: {
				int v = ev.caxis.value;
				switch (ev.caxis.axis) {
				// Both sticks move like the d-pad.
				case SDL_CONTROLLER_AXIS_RIGHTX:
				case SDL_CONTROLLER_AXIS_LEFTX:
					if (!stick_x && std::abs(v) > 20000) {
						stick_x = true;
						stick_x_btn = v < 0 ? Btn::Left : Btn::Right;
						press(app, rep, stick_x_btn);
					} else if (stick_x && std::abs(v) < 12000) {
						stick_x = false;
						release(rep, stick_x_btn);
					}
					break;
				case SDL_CONTROLLER_AXIS_RIGHTY:
				case SDL_CONTROLLER_AXIS_LEFTY:
					if (!stick_y && std::abs(v) > 20000) {
						stick_y = true;
						stick_y_btn = v < 0 ? Btn::Up : Btn::Down;
						press(app, rep, stick_y_btn);
					} else if (stick_y && std::abs(v) < 12000) {
						stick_y = false;
						release(rep, stick_y_btn);
					}
					break;
				case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
					if (!l2 && v > 16000) l2 = true, app.on_button(Btn::L2);
					else if (l2 && v < 8000) l2 = false;
					break;
				case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
					if (!r2 && v > 16000) r2 = true, app.on_button(Btn::R2);
					else if (r2 && v < 8000) r2 = false;
					break;
				}
				break;
			}
			case SDL_KEYDOWN:
				if (app.text_entry_active()) {
					// The keyboard dialog ends its text with a Return.
					if (ev.key.keysym.sym == SDLK_RETURN || ev.key.keysym.sym == SDLK_KP_ENTER) app.input_submit();
					break;
				}
				if (ev.key.repeat) {
					// Let the OS repeat arrows; our own repeat is for controllers.
					if (map_key(ev.key.keysym.sym, false, b) &&
					    (b == Btn::Up || b == Btn::Down || b == Btn::Left || b == Btn::Right))
						app.on_button(b);
					break;
				}
				if (map_key(ev.key.keysym.sym, false, b)) app.on_button(b);
				break;
			case SDL_TEXTINPUT:
				if (app.text_entry_active()) app.on_text(ev.text.text);
				break;
			default: break;
			}
		}
		double now = now_seconds();
		if (rep.held && now >= rep.next) {
			app.on_button(rep.btn);
			rep.next = now + 0.085;
		}

		app.update();
		ctx->Update();
		app.after_layout();

		SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
		SDL_RenderClear(renderer);
		app.render_video();
		render.BeginFrame();
		ctx->Render();
		SDL_RenderPresent(renderer);
		if (surface_renderer) SDL_UpdateWindowSurface(window);

		// Cap at 60 fps when vsync isn't there.
		double spent = now_seconds() - last_frame;
		if (spent < 1.0 / 60) SDL_Delay(Uint32((1.0 / 60 - spent) * 1000));
		last_frame = now_seconds();
	}

	dlog("exiting");
	app.shutdown();
	g_art.stop();
	g_tasks.stop();
	Rml::Shutdown();
	SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(window);
	SDL_Quit();
#ifdef PLATFORM_PS5_NATIVE
	// On 11.60 an app can't end itself: exit() is reported as a crash
	// (SIGSYS) and sceSystemServiceLoadExec("exit") fails. Ask the system to
	// close us, as the PS menu's Close does, and wait for it.
	int app_id = sceSystemServiceGetAppIdOfRunningBigApp();
	dlog("closing app 0x%x: 0x%x", app_id, sceSystemServiceKillApp(app_id, -1, 0, 0));
	for (;;) SDL_Delay(1000);
#endif
	return 0;
}
