// Settings, signing in with a code, and text entry (on-screen keyboard).

#include <algorithm>

#include "app.h"

enum SettingRow {
	kAccount,
	kTorrents,
	kServer,
	kSubLangs,
	kAutoSubs,
	kSubSize,
	kAudioLangs,
	kAutoplay,
	kAddAddon,
	kReload,
	kExit,
	kSettingCount
};

static bool g_confirm_sign_out = false;

static std::string size_label(const std::string& s) {
	if (s == "sub-s") return "Small";
	if (s == "sub-l") return "Large";
	return "Medium";
}

void App::enter_settings() {
	g_confirm_sign_out = false;
	settings_refresh();
}

void App::settings_refresh() {
	s_rows.assign(kSettingCount, UiSetting());
	s_rows[kAccount] = {"Stremio account",
	                    signed_in() ? (g_confirm_sign_out ? "Press Cross again to sign out." : "Press Cross to sign out.")
	                                : "Sign in to sync your addons, library and Continue Watching. Press Cross.",
	                    signed_in() ? settings_.user_email : "Not signed in"};
	s_rows[kTorrents] = {"Play torrents with",
	                     settings_.builtin_torrents
	                         ? "The app downloads torrents itself; no server needed. Press Cross to use your server."
	                         : "Your Stremio streaming server downloads torrents. Press Cross to use the app's own.",
	                     settings_.builtin_torrents ? "This app" : "Streaming server"};
	s_rows[kServer] = {"Streaming server",
	                   settings_.builtin_torrents
	                       ? "Optional: for server transcoding. Your Stremio server, e.g. http://192.168.1.20:11470."
	                       : "Your Stremio server, e.g. http://192.168.1.20:11470. Press Cross to change.",
	                   settings_.server_url.empty() ? "Not set" : settings_.server_url};
	s_rows[kSubLangs] = {"Subtitle languages", "Preferred order, e.g. \"gre, eng\". Press Cross to change.",
	                     settings_.subtitle_langs.empty() ? "None" : settings_.subtitle_langs};
	s_rows[kAutoSubs] = {"Subtitles on automatically", "Turn subtitles on automatically in your preferred language.",
	                     settings_.auto_subtitles ? "On" : "Off"};
	s_rows[kSubSize] = {"Subtitle size", "", size_label(settings_.sub_size)};
	s_rows[kAudioLangs] = {"Audio languages", "Preferred audio track language, e.g. \"eng\". Leave empty for the default.",
	                       settings_.audio_langs.empty() ? "Default" : settings_.audio_langs};
	s_rows[kAutoplay] = {"Play next episode automatically", "", settings_.autoplay_next ? "On" : "Off"};
	s_rows[kAddAddon] = {"Add addon by URL",
	                     settings_.extra_addons.empty()
	                         ? "Addon manifest URL. Remove added addons from the Addons page."
	                         : std::to_string(settings_.extra_addons.size()) + " added here. Remove them from the Addons page.",
	                     ""};
	s_rows[kReload] = {"Reload addons and catalogs", "", ""};
	s_rows[kExit] = {"Exit Stremio", "", ""};
	if (s_sel >= kSettingCount) s_sel = kSettingCount - 1;
	dirty_all();
}

void App::settings_button(Btn b) {
	switch (b) {
	case Btn::Up:
		if (s_sel > 0) s_sel--;
		else zone = "searchbox";
		g_confirm_sign_out = false;
		break;
	case Btn::Down:
		if (s_sel + 1 < kSettingCount) s_sel++;
		g_confirm_sign_out = false;
		break;
	case Btn::Left:
	case Btn::Circle:
		zone = "nav";
		g_confirm_sign_out = false;
		break;
	case Btn::Cross:
		switch (s_sel) {
		case kAccount:
			if (!signed_in()) login_start();
			else if (!g_confirm_sign_out) g_confirm_sign_out = true;
			else {
				g_confirm_sign_out = false;
				sign_out();
			}
			break;
		case kTorrents:
			settings_.builtin_torrents = !settings_.builtin_torrents;
			save_settings();
			on_addons_loaded();  // the banner depends on it
			settings_refresh();
			break;
		case kServer:
			open_input("Streaming server address", settings_.server_url.empty() ? "http://" : settings_.server_url,
			           "Stremio on a computer on your network serves on port 11470, e.g. http://192.168.1.20:11470",
			           [this](const std::string& v) {
				           // The keyboard dialog starts empty: nothing typed keeps the address.
				           std::string u = trim(v);
				           if (u.empty()) return;
				           if (u.find("://") == std::string::npos) u = "http://" + u;
				           settings_.server_url = u;
				           save_settings();
				           on_addons_loaded();  // the banner depends on it
				           settings_refresh();
			           });
			break;
		case kSubLangs:
			open_input("Subtitle languages", settings_.subtitle_langs, "Subtitle languages, best first (e.g. gre, eng)",
			           [this](const std::string& v) {
				           if (trim(v).empty()) return;
				           settings_.subtitle_langs = trim(v);
				           save_settings();
				           settings_refresh();
			           });
			break;
		case kAutoSubs:
			settings_.auto_subtitles = !settings_.auto_subtitles;
			save_settings();
			break;
		case kSubSize: {
			static const char* sizes[] = {"sub-s", "sub-m", "sub-l"};
			int cur = settings_.sub_size == "sub-s" ? 0 : settings_.sub_size == "sub-l" ? 2 : 1;
			open_dropdown("Subtitle size", {"Small", "Medium", "Large"}, cur, [this](int i) {
				settings_.sub_size = sizes[i];
				w_sub_size = settings_.sub_size;
				save_settings();
				settings_refresh();
			});
			break;
		}
		case kAudioLangs:
			open_input("Audio languages", settings_.audio_langs, "Preferred audio languages (e.g. eng), or \"-\" for the default",
			           [this](const std::string& v) {
				           if (trim(v).empty()) return;
				           settings_.audio_langs = trim(v) == "-" ? "" : trim(v);
				           save_settings();
				           settings_refresh();
			           });
			break;
		case kAutoplay:
			settings_.autoplay_next = !settings_.autoplay_next;
			save_settings();
			break;
		case kAddAddon:
			open_input("Addon manifest URL", "https://", "e.g. https://torrentio.strem.fun/manifest.json",
			           [this](const std::string& v) {
				           std::string url = normalize_addon_url(v);
				           if (trim(v).empty() || trim(v) == "https://") return;
				           show_toast("Loading addon...");
				           bg<std::string>(
				               [url]() {
					               Addon a;
					               std::string err;
					               if (!fetch_addon(url, a, err)) return "Could not add the addon: " + err;
					               return std::string();
				               },
				               [this, url](std::string& err) {
					               if (!err.empty()) {
						               show_toast(err, 6);
						               return;
					               }
					               if (std::find(settings_.extra_addons.begin(), settings_.extra_addons.end(), url) ==
					                   settings_.extra_addons.end())
						               settings_.extra_addons.push_back(url);
					               save_settings();
					               show_toast("Addon added");
					               load_addons();
					               settings_refresh();
				               });
			           });
			break;
		case kReload:
			show_toast("Reloading addons...");
			load_addons();
			if (signed_in()) load_library();
			break;
		case kExit: exit_ = true; break;
		}
		break;
	default: break;
	}
	settings_refresh();
}

// ---------------------------------------------------------------------------
// Sign in with a code

void App::login_start() {
	int gen = ++login_gen_;
	login_visible = true;
	login_code.clear();
	login_code_raw_.clear();
	login_qr_url_.clear();
	login_qr.clear();
	login_link = "link.stremio.com";
	login_status = "Loading...";
	login_polling_ = false;
	dirty_all();
	bg<LinkCode>([]() { return link_create(); },
	             [this, gen](LinkCode& lc) {
		             if (gen != login_gen_ || !login_visible) return;
		             if (!lc.ok) {
			             login_status = "Could not get a code: " + lc.error;
			             dirty_all();
			             return;
		             }
		             login_code_raw_ = lc.code;
		             login_code = lc.code;
		             std::string shown = lc.link;
		             if (starts_with(shown, "https://")) shown = shown.substr(8);
		             login_link = shown;
		             login_qr_url_ = "https://link.stremio.com/qr?data=" + url_encode(lc.link);
		             login_qr = art(login_qr_url_, ArtKind::Qr);
		             login_status = "Waiting for you to approve the sign-in on your phone or computer...";
		             login_next_poll_ = now_seconds() + 3;
		             login_expires_ = now_seconds() + 10 * 60;
		             dirty_all();
	             });
}

void App::login_poll() {
	if (login_qr.empty() && !login_qr_url_.empty()) {
		// The QR image may have arrived since.
		std::string q = art(login_qr_url_, ArtKind::Qr, false);
		if (!q.empty()) {
			login_qr = q;
			dirty("login_qr");
		}
	}
	if (login_code_raw_.empty() || login_polling_ || now_seconds() < login_next_poll_) return;
	if (now_seconds() > login_expires_) {
		login_status = "The sign-in code expired. Try again from Settings.";
		login_code_raw_.clear();
		dirty_all();
		return;
	}
	login_polling_ = true;
	int gen = login_gen_;
	std::string code = login_code_raw_;
	g_tasks.run<ApiResult>([code]() { return link_read(code); },
	                       [this, gen](ApiResult& r) {
		                       login_polling_ = false;
		                       login_next_poll_ = now_seconds() + 3;
		                       if (gen != login_gen_ || !login_visible) return;
		                       if (r.ok) login_done(jstr(r.result, "authKey"));
	                       });
}

void App::login_close() {
	login_gen_++;
	login_visible = false;
	login_code_raw_.clear();
	dirty_all();
}

void App::login_done(const std::string& auth_key) {
	login_close();
	settings_.auth_key = auth_key;
	save_settings();
	show_toast("Signed in");
	bg<ApiResult>([auth_key]() { return api_get_user(auth_key); },
	              [this](ApiResult& r) {
		              if (!r.ok) return;
		              settings_.user_email = jstr(r.result, "email");
		              user = settings_.user_email.empty() ? "" : std::string(1, char(toupper((unsigned char)settings_.user_email[0])));
		              save_settings();
		              settings_refresh();
		              show_toast("Signed in as " + settings_.user_email);
	              });
	library_.clear();
	library_loaded_ = false;
	load_addons();
	load_library();
	settings_refresh();
}

void App::sign_out() {
	std::string key = settings_.auth_key;
	g_tasks.run([key]() { api_logout(key); });
	settings_.auth_key.clear();
	settings_.user_email.clear();
	user.clear();
	library_.clear();
	library_loaded_ = false;
	save_settings();
	show_toast("Signed out");
	load_addons();
	settings_refresh();
}

// ---------------------------------------------------------------------------
// Text entry

// Only the console's own keyboard: SDL_StartTextInput opens the PS5 IME
// dialog (SDL's ps5 keyboard driver). It starts empty; on Done SDL sends
// the whole text as SDL_TEXTINPUT events and then a Return key
// (input_submit); on cancel it just closes, which input_poll notices.
// While it's up the dialog owns the controller.
void App::open_input(const std::string& title, const std::string& value, const std::string& hint,
                     std::function<void(const std::string&)> done) {
	(void)title;
	(void)value;
	(void)hint;
	input_value.clear();
	input_done_ = done;
	input_visible_ = true;
	input_shown_ = false;
	input_opened_ = now_seconds();
	SDL_StartTextInput();
}

void App::input_finish(bool ok) {
	input_visible_ = false;
	SDL_StopTextInput();
	auto fn = input_done_;
	input_done_ = nullptr;
	std::string v = input_value;
	dirty_all();
	if (ok && fn) fn(v);
}

void App::input_submit() {
	if (input_visible_) input_finish(true);
}

void App::input_poll() {
	if (!input_visible_) return;
	SDL_Window* w = SDL_RenderGetWindow(renderer_);
	bool shown = w && SDL_IsScreenKeyboardShown(w);
	if (shown) {
		input_shown_ = true;
	} else if (input_shown_ || now_seconds() - input_opened_ > 3) {
		// Closed without Done (cancelled), or it never opened.
		input_finish(false);
	}
}
