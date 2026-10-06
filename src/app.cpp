#include "app.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "artcache.h"
#include "http.h"
#include "tasks.h"
#include "torrent/engine.h"

// Pitches mirrored from the stylesheets (see the comments there), in dp,
// so times kUiScale in pixels.
static const float kSettingPitch = 104 * kUiScale;
static const float kAddonPitch = 424 * kUiScale;  // a row of addon tiles
static const float kGridRowPitch = 376 * kUiScale;
static const float kDiscRowPitch = 490 * kUiScale;  // #disc-grid: big posters, two rows
static const float kRowPitch = 540 * kUiScale;
static const float kCardPitch = 316 * kUiScale;
static const float kEpisodePitch = 124 * kUiScale;
static const float kStreamPitch = 136 * kUiScale;
static const float kMenuPitch = 66 * kUiScale;
static const float kDropdownPitch = 68 * kUiScale;

Item item_from_meta_json(const json& j) {
	Item it;
	it.id = jstr(j, "id", jstr(j, "imdb_id"));
	it.type = jstr(j, "type");
	it.name = jstr(j, "name");
	it.poster = jstr(j, "poster");
	it.background = jstr(j, "background");
	it.logo = jstr(j, "logo");
	it.description = jstr(j, "description");
	it.release_info = jstr(j, "releaseInfo", jstr(j, "year"));
	it.runtime = jstr(j, "runtime");
	it.imdb_rating = jstr(j, "imdbRating");
	const json& g = jobj(j, "genres");
	if (g.is_array())
		for (auto& v : g)
			if (v.is_string()) it.genres.push_back(v.get<std::string>());
	return it;
}

// ---------------------------------------------------------------------------
// Setup

bool App::init(Rml::Context* ctx, SDL_Renderer* renderer, const std::string& base_dir, const std::string& data_dir) {
	ctx_ = ctx;
	renderer_ = renderer;
	base_dir_ = base_dir;
	data_dir_ = data_dir;
	// The built-in torrent engine keeps a 1 GB rolling cache in the app's
	// storage (2 GB in param.json, shared with the artwork cache).
	bt::Engine::get().configure(data_dir_, 1ll << 30);

	load_settings();
	load_progress();
	bind_model();

	doc_ = ctx_->LoadDocument(base_dir_ + "/assets/main.rml");
	if (!doc_) {
		dlog("can't load %s/assets/main.rml", base_dir_.c_str());
		return false;
	}
	doc_->Show();

	clock = format_clock();
	user = settings_.user_email.empty() ? "" : std::string(1, char(toupper((unsigned char)settings_.user_email[0])));
	if (!config_note_.empty()) show_toast(config_note_, 8);

	view = "home";
	zone = "content";
	load_addons();
	if (signed_in()) load_library();
	dirty_all();
	return true;
}

void App::shutdown() {
	if (watching_) watch_stop(false);
	player_.close();
	bt::Engine::get().shutdown();
	save_progress();
}

void App::bind_model() {
	Rml::DataModelConstructor c = ctx_->CreateDataModel("app");

	if (auto s = c.RegisterStruct<UiCard>()) {
		s.RegisterMember("title", &UiCard::title);
		s.RegisterMember("image", &UiCard::image);
		s.RegisterMember("initials", &UiCard::initials);
		s.RegisterMember("badge", &UiCard::badge);
		s.RegisterMember("progress", &UiCard::progress);
	}
	c.RegisterArray<std::vector<UiCard>>();
	if (auto s = c.RegisterStruct<UiRow>()) {
		s.RegisterMember("title", &UiRow::title);
		s.RegisterMember("dom_id", &UiRow::dom_id);
		s.RegisterMember("message", &UiRow::message);
		s.RegisterMember("see_all", &UiRow::see_all);
		s.RegisterMember("cards", &UiRow::cards);
	}
	c.RegisterArray<std::vector<UiRow>>();
	if (auto s = c.RegisterStruct<UiChip>()) s.RegisterMember("value", &UiChip::value);
	c.RegisterArray<std::vector<UiChip>>();
	if (auto s = c.RegisterStruct<UiAddon>()) {
		s.RegisterMember("name", &UiAddon::name);
		s.RegisterMember("version", &UiAddon::version);
		s.RegisterMember("desc", &UiAddon::desc);
		s.RegisterMember("types", &UiAddon::types);
		s.RegisterMember("logo", &UiAddon::logo);
		s.RegisterMember("initials", &UiAddon::initials);
		s.RegisterMember("local", &UiAddon::local);
	}
	c.RegisterArray<std::vector<UiAddon>>();
	if (auto s = c.RegisterStruct<UiSetting>()) {
		s.RegisterMember("label", &UiSetting::label);
		s.RegisterMember("hint", &UiSetting::hint);
		s.RegisterMember("value", &UiSetting::value);
	}
	c.RegisterArray<std::vector<UiSetting>>();
	if (auto s = c.RegisterStruct<UiEpisode>()) {
		s.RegisterMember("title", &UiEpisode::title);
		s.RegisterMember("sub", &UiEpisode::sub);
		s.RegisterMember("thumb", &UiEpisode::thumb);
		s.RegisterMember("watched", &UiEpisode::watched);
	}
	c.RegisterArray<std::vector<UiEpisode>>();
	if (auto s = c.RegisterStruct<UiStream>()) {
		s.RegisterMember("name", &UiStream::name);
		s.RegisterMember("addon", &UiStream::addon);
		s.RegisterMember("desc", &UiStream::desc);
	}
	c.RegisterArray<std::vector<UiStream>>();
	if (auto s = c.RegisterStruct<UiMenuItem>()) {
		s.RegisterMember("label", &UiMenuItem::label);
		s.RegisterMember("active", &UiMenuItem::active);
	}
	c.RegisterArray<std::vector<UiMenuItem>>();
	if (auto s = c.RegisterStruct<UiKey>()) {
		s.RegisterMember("label", &UiKey::label);
		s.RegisterMember("wide", &UiKey::wide);
	}
	c.RegisterArray<std::vector<UiKey>>();

	c.Bind("zone", &zone);
	c.Bind("view", &view);
	c.Bind("nav_sel", &nav_sel);
	c.Bind("search_query", &search_query);
	c.Bind("clock", &clock);
	c.Bind("user", &user);
	c.Bind("banner", &banner);
	c.Bind("busy", &busy);

	c.Bind("home_rows", &home_rows);
	c.Bind("home_row", &home_row);
	c.Bind("home_col", &home_col);
	c.Bind("search_rows", &search_rows);
	c.Bind("search_row", &search_row);
	c.Bind("search_col", &search_col);
	c.Bind("search_status", &search_status);

	c.Bind("disc_chips", &disc_chips);
	c.Bind("disc_chip", &disc_chip);
	c.Bind("disc_cards", &disc_cards);
	c.Bind("disc_sel", &disc_sel);
	c.Bind("disc_status", &disc_status);
	c.Bind("dp_name", &dp_name);
	c.Bind("dp_logo", &dp_logo);
	c.Bind("dp_still", &dp_still);
	c.Bind("dp_runtime", &dp_runtime);
	c.Bind("dp_year", &dp_year);
	c.Bind("dp_imdb", &dp_imdb);
	c.Bind("dp_genres", &dp_genres);
	c.Bind("dp_desc", &dp_desc);

	c.Bind("lib_chips", &lib_chips);
	c.Bind("lib_chip", &lib_chip);
	c.Bind("lib_cards", &lib_cards);
	c.Bind("lib_sel", &lib_sel);
	c.Bind("lib_status", &lib_status);

	c.Bind("addon_rows", &addon_rows);
	c.Bind("addon_sel", &addon_sel);
	c.Bind("s_rows", &s_rows);
	c.Bind("s_sel", &s_sel);

	c.Bind("d_background", &d_background);
	c.Bind("d_logo", &d_logo);
	c.Bind("d_name", &d_name);
	c.Bind("d_runtime", &d_runtime);
	c.Bind("d_year", &d_year);
	c.Bind("d_imdb", &d_imdb);
	c.Bind("d_genres", &d_genres);
	c.Bind("d_cast", &d_cast);
	c.Bind("d_directors", &d_directors);
	c.Bind("d_description", &d_description);
	c.Bind("d_resume", &d_resume);
	c.Bind("d_series", &d_series);
	c.Bind("d_zone", &d_zone);
	c.Bind("d_season_label", &d_season_label);
	c.Bind("d_episodes", &d_episodes);
	c.Bind("d_episode_sel", &d_episode_sel);
	c.Bind("d_streams_title", &d_streams_title);
	c.Bind("d_streams", &d_streams);
	c.Bind("d_stream_sel", &d_stream_sel);
	c.Bind("d_pick_res", &d_pick_res);
	c.Bind("d_streams_status", &d_streams_status);

	c.Bind("watching", &watching_);
	c.Bind("w_sub_rml", &w_sub_rml);
	c.Bind("w_sub_size", &w_sub_size);
	c.Bind("info_visible", &info_visible);
	c.Bind("w_buffering", &w_buffering);
	c.Bind("w_buffer_text", &w_buffer_text);
	c.Bind("w_paused", &w_paused);
	c.Bind("menu_visible", &menu_visible);
	c.Bind("w_title", &w_title);
	c.Bind("w_subtitle", &w_subtitle);
	c.Bind("w_progress", &w_progress);
	c.Bind("w_time", &w_time);
	c.Bind("w_duration", &w_duration);
	c.Bind("w_tracks", &w_tracks);
	c.Bind("w_stats", &w_stats);
	c.Bind("m_col", &m_col);
	c.Bind("m_audio", &m_audio);
	c.Bind("m_audio_sel", &m_audio_sel);
	c.Bind("m_subs", &m_subs);
	c.Bind("m_sub_sel", &m_sub_sel);
	c.Bind("m_delay", &m_delay);

	c.Bind("launch_visible", &launch_visible);
	c.Bind("launch_image", &launch_image);
	c.Bind("launch_title", &launch_title);
	c.Bind("launch_status", &launch_status);
	c.Bind("t_visible", &t_visible);
	c.Bind("t_peers", &t_peers);
	c.Bind("t_speed", &t_speed);
	c.Bind("t_progress", &t_progress);
	c.Bind("t_hash", &t_hash);

	c.Bind("dd_visible", &dd_visible);
	c.Bind("dd_title", &dd_title);
	c.Bind("dd_options", &dd_options);
	c.Bind("dd_sel", &dd_sel);

	c.Bind("login_visible", &login_visible);
	c.Bind("login_qr", &login_qr);
	c.Bind("login_link", &login_link);
	c.Bind("login_code", &login_code);
	c.Bind("login_status", &login_status);

	c.Bind("toast", &toast);

	model_ = c.GetModelHandle();
}

void App::dirty(const char* name) { model_.DirtyVariable(name); }

void App::show_toast(const std::string& msg, double seconds) {
	toast = msg;
	toast_until_ = now_seconds() + seconds;
	dirty("toast");
}

// ---------------------------------------------------------------------------
// Persistence

void App::load_settings() {
	json j;
	if (load_json(data_dir_ + "/settings.json", j)) {
		settings_.server_url = jstr(j, "server_url");
		settings_.builtin_torrents = jbool(j, "builtin_torrents", settings_.builtin_torrents);
		settings_.subtitle_langs = jstr(j, "subtitle_languages", settings_.subtitle_langs);
		settings_.auto_subtitles = jbool(j, "auto_subtitles", settings_.auto_subtitles);
		settings_.sub_size = jstr(j, "subtitle_size", settings_.sub_size);
		settings_.audio_langs = jstr(j, "audio_languages");
		settings_.autoplay_next = jbool(j, "autoplay_next", settings_.autoplay_next);
		settings_.auth_key = jstr(j, "auth_key");
		settings_.user_email = jstr(j, "user_email");
		const json& ex = jobj(j, "extra_addons");
		if (ex.is_array())
			for (auto& v : ex)
				if (v.is_string()) settings_.extra_addons.push_back(v.get<std::string>());
	}

	// config.json next to the data (or the app) can preset things, e.g. a
	// server address or an account, without typing on the controller.
	json cfg;
	std::string err;
	std::string cfg_path = data_dir_ + "/config.json";
	if (!file_exists(cfg_path)) cfg_path = base_dir_ + "/config.json";
	if (file_exists(cfg_path)) {
		if (!load_json(cfg_path, cfg, &err)) {
			config_note_ = "config.json is not valid JSON (" + err + ")";
		} else {
			if (settings_.server_url.empty()) settings_.server_url = jstr(cfg, "server_url");
			if (cfg.contains("subtitle_languages")) settings_.subtitle_langs = jstr(cfg, "subtitle_languages");
			if (cfg.contains("audio_languages")) settings_.audio_langs = jstr(cfg, "audio_languages");
			const json& ex = jobj(cfg, "extra_addons");
			if (ex.is_array())
				for (auto& v : ex)
					if (v.is_string()) {
						std::string u = normalize_addon_url(v.get<std::string>());
						if (std::find(settings_.extra_addons.begin(), settings_.extra_addons.end(), u) ==
						    settings_.extra_addons.end())
							settings_.extra_addons.push_back(u);
					}
			std::string email = jstr(cfg, "login_email"), password = jstr(cfg, "login_password");
			if (!signed_in() && !email.empty() && !password.empty()) {
				ApiResult r = api_login(email, password);  // once, at startup
				if (r.ok) {
					settings_.auth_key = jstr(r.result, "authKey");
					settings_.user_email = jstr(jobj(r.result, "user"), "email", email);
				} else {
					config_note_ = "Sign-in from config.json failed: " + r.error;
				}
			}
		}
	}
	w_sub_size = settings_.sub_size;
	save_settings();
}

void App::save_settings() {
	json j;
	j["server_url"] = settings_.server_url;
	j["builtin_torrents"] = settings_.builtin_torrents;
	j["subtitle_languages"] = settings_.subtitle_langs;
	j["auto_subtitles"] = settings_.auto_subtitles;
	j["subtitle_size"] = settings_.sub_size;
	j["audio_languages"] = settings_.audio_langs;
	j["autoplay_next"] = settings_.autoplay_next;
	j["extra_addons"] = settings_.extra_addons;
	j["auth_key"] = settings_.auth_key;
	j["user_email"] = settings_.user_email;
	if (!save_json(data_dir_ + "/settings.json", j)) show_toast("Could not save settings to " + data_dir_);
}

void App::load_progress() {
	json j;
	if (!load_json(data_dir_ + "/progress.json", j)) return;
	const json& items = jobj(j, "items");
	if (items.is_object()) {
		for (auto it = items.begin(); it != items.end(); ++it) {
			Progress p;
			p.type = jstr(it.value(), "type");
			p.name = jstr(it.value(), "name");
			p.poster = jstr(it.value(), "poster");
			p.video_id = jstr(it.value(), "video_id");
			p.time = jnum(it.value(), "time");
			p.duration = jnum(it.value(), "duration");
			p.updated = int64_t(jnum(it.value(), "updated"));
			progress_[it.key()] = p;
		}
	}
	const json& w = jobj(j, "watched");
	if (w.is_array())
		for (auto& v : w)
			if (v.is_string()) watched_.insert(v.get<std::string>());
}

void App::save_progress() {
	json j;
	json items = json::object();
	for (auto& kv : progress_) {
		items[kv.first] = json{{"type", kv.second.type},         {"name", kv.second.name},
		                       {"poster", kv.second.poster},     {"video_id", kv.second.video_id},
		                       {"time", kv.second.time},         {"duration", kv.second.duration},
		                       {"updated", kv.second.updated}};
	}
	j["items"] = items;
	j["watched"] = json(std::vector<std::string>(watched_.begin(), watched_.end()));
	save_json(data_dir_ + "/progress.json", j);
}

std::string App::server() const {
	std::string s = trim(settings_.server_url);
	while (!s.empty() && s.back() == '/') s.pop_back();
	if (!s.empty() && !starts_with(s, "http://") && !starts_with(s, "https://")) s = "http://" + s;
	return s;
}

static std::vector<std::string> lang_list(const std::string& s) {
	std::vector<std::string> out;
	for (auto& part : split(s, ',')) {
		std::string p = trim(part);
		if (p.empty() || p == "-") continue;
		out.push_back(language_to_iso639_2(p));
	}
	return out;
}

std::vector<std::string> App::pref_sub_langs() const { return lang_list(settings_.subtitle_langs); }
std::vector<std::string> App::pref_audio_langs() const { return lang_list(settings_.audio_langs); }

// ---------------------------------------------------------------------------
// Addons and library

const Addon* App::find_addon(const std::string& url) const {
	for (auto& a : addons_)
		if (a->transport_url == url) return a.get();
	return nullptr;
}

void App::load_addons() {
	struct Result {
		std::vector<std::shared_ptr<Addon>> addons;
		std::vector<std::string> errors;
		bool auth_failed = false;
	};
	std::string key = settings_.auth_key;
	std::vector<std::string> extra = settings_.extra_addons;
	int gen = ++addons_gen_;
	addons_loading_ = true;
	if (home_rows.empty()) dirty_all();

	bg<Result>(
	    [key, extra]() {
		    Result r;
		    std::set<std::string> seen;
		    if (!key.empty()) {
			    ApiResult ar = api_addon_collection(key);
			    if (ar.ok) {
				    const json& list = jobj(ar.result, "addons");
				    if (list.is_array()) {
					    for (auto& e : list) {
						    auto a = std::make_shared<Addon>();
						    std::string url = jstr(e, "transportUrl");
						    if (parse_addon(jobj(e, "manifest"), url, *a)) {
							    a->from_account = true;
							    if (seen.insert(url).second) r.addons.push_back(a);
						    }
					    }
				    }
			    } else {
				    r.errors.push_back("Could not load your addons (" + ar.error + ")");
				    std::string l = lower(ar.error);
				    if (l.find("session") != std::string::npos || l.find("auth") != std::string::npos)
					    r.auth_failed = true;
			    }
		    }
		    std::vector<std::string> urls;
		    if (r.addons.empty()) urls = {kCinemetaUrl, kOpenSubtitlesUrl};
		    for (auto& u : extra) urls.push_back(u);
		    for (auto& u : urls) {
			    if (seen.count(u)) continue;
			    auto a = std::make_shared<Addon>();
			    std::string err;
			    if (fetch_addon(u, *a, err)) {
				    seen.insert(u);
				    r.addons.push_back(a);
			    } else {
				    r.errors.push_back("Could not reach " + u + " (" + err + ")");
			    }
		    }
		    return r;
	    },
	    [this, gen](Result& r) {
		    if (gen != addons_gen_) return;
		    addons_loading_ = false;
		    addons_.clear();
		    for (auto& a : r.addons) addons_.push_back(std::make_unique<Addon>(*a));
		    if (!r.errors.empty()) show_toast(r.errors.front(), 6);
		    if (r.auth_failed) show_toast("Your Stremio session expired. Sign in again in Settings.", 8);
		    dlog("addons: %zu loaded", addons_.size());
		    on_addons_loaded();
	    });
}

void App::on_addons_loaded() {
	// No banner asking to sign in; sign-in is in Settings. Torrents need a
	// server only when the built-in engine is off.
	if (signed_in() && !settings_.builtin_torrents && server().empty())
		banner = "Welcome! Open Settings in the menu on the left and enter your Stremio streaming server, e.g. "
		         "http://192.168.1.20:11470";
	else banner = "";
	build_home();
	if (view == "discover") enter_discover();
	if (view == "addons") enter_addons();
	if (view == "settings") settings_refresh();
	dirty_all();
}

void App::load_library() {
	std::string key = settings_.auth_key;
	if (key.empty()) return;
	bg<ApiResult>([key]() { return api_library_get(key); },
	              [this](ApiResult& r) {
		              if (!r.ok) {
			              show_toast("Could not load your library: " + r.error);
			              return;
		              }
		              library_.clear();
		              if (r.result.is_array())
			              for (auto& it : r.result) {
				              std::string id = jstr(it, "_id");
				              if (!id.empty()) library_[id] = it;
			              }
		              library_loaded_ = true;
		              dlog("library: %zu items", library_.size());
		              refresh_home_cards();
		              if (view == "library") library_refresh();
		              dirty_all();
	              });
}

// ---------------------------------------------------------------------------
// Views and input

void App::set_view(const std::string& v) {
	g_art.clear_queue();
	view = v;
	if (v == "home") nav_sel = 0, refresh_home_cards();
	else if (v == "discover") nav_sel = 1, enter_discover();
	else if (v == "library") nav_sel = 2, enter_library();
	else if (v == "addons") nav_sel = 3, enter_addons();
	else if (v == "settings") nav_sel = 4, enter_settings();
	dirty_all();
}

void App::nav_button(Btn b) {
	static const char* views[] = {"home", "discover", "library", "addons", "settings"};
	switch (b) {
	case Btn::Up:
		if (nav_sel > 0) set_view(views[nav_sel - 1]);
		break;
	case Btn::Down:
		if (nav_sel < 4) set_view(views[nav_sel + 1]);
		break;
	case Btn::Right:
	case Btn::Cross:
		if (view == "discover" || view == "library") {
			bool empty = view == "discover" ? disc_cards.empty() : lib_cards.empty();
			zone = empty ? "filters" : "content";
		} else {
			zone = "content";
		}
		break;
	default: break;
	}
	dirty_all();
}

// Buttons that do the same thing on every page outside the player.
bool App::browse_shortcut(Btn b) {
	static const char* views[] = {"home", "discover", "library", "addons", "settings"};
	switch (b) {
	case Btn::L1:  // previous / next page of the menu
	case Btn::R1: {
		if (view == "search") return false;
		int next = nav_sel + (b == Btn::L1 ? -1 : 1);
		if (next < 0 || next > 4) return true;
		set_view(views[next]);
		if (zone == "searchbox") zone = "content";
		return true;
	}
	case Btn::L2:  // page up / down
	case Btn::R2:
		if (zone != "content") return true;
		for (int i = 0; i < 5 && zone == "content"; i++) on_button(b == Btn::L2 ? Btn::Up : Btn::Down);
		return true;
	case Btn::Touchpad:  // open / close the side menu
		zone = zone == "nav" ? "content" : "nav";
		dirty_all();
		return true;
	case Btn::L3:  // back to the board
		set_view("home");
		zone = "content";
		return true;
	case Btn::Triangle:  // search
	case Btn::R3:
	case Btn::Options:
		zone = "searchbox";
		on_button(Btn::Cross);
		return true;
	default: return false;
	}
}

void App::on_button(Btn b) {
	if (input_visible_) return;  // the system keyboard dialog has the controller
	if (dd_visible) return dropdown_button(b);
	if (login_visible) {
		if (b == Btn::Circle) login_close();
		return;
	}
	if (launch_visible && !watching_) {  // the server is still preparing the stream
		if (b == Btn::Circle) {
			if (launch_cancel_) *launch_cancel_ = true;
			w_gen_++;
			launch_visible = false;
			t_visible = false;
			dirty_all();
		}
		return;
	}
	if (watching_) {
		if (menu_visible) return watch_menu_button(b);
		return watch_button(b);
	}
	if (view == "detail") {
		if (b == Btn::L3) {  // straight back to the board
			detail_close();
			set_view("home");
			zone = "content";
			return;
		}
		return detail_button(b);
	}
	if (browse_shortcut(b)) return;

	if (zone == "nav") return nav_button(b);
	if (zone == "searchbox") {
		switch (b) {
		case Btn::Cross:
			open_input("Search", view == "search" ? search_query : "", "Type, then press Enter",
			           [this](const std::string& q) {
				           if (!trim(q).empty()) start_search(trim(q));
			           });
			break;
		case Btn::Down: zone = "content"; break;
		case Btn::Left:
		case Btn::Circle: zone = "nav"; break;
		default: break;
		}
		dirty_all();
		return;
	}
	if (view == "home") board_button(b);
	else if (view == "search") search_button(b);
	else if (view == "discover") discover_button(b);
	else if (view == "library") library_button(b);
	else if (view == "addons") addons_button(b);
	else if (view == "settings") settings_button(b);
}

void App::on_text(const std::string& utf8) {
	if (!input_visible_) return;
	input_value += utf8;
}

// ---------------------------------------------------------------------------
// Frame

std::string App::art(const std::string& url, ArtKind kind, bool want) {
	if (url.empty()) return "";
	if (!want) return g_art.peek(url, kind);
	return g_art.get(url, kind, [this](const std::string& path) {
		if (!path.empty()) images_dirty_ = true;
	});
}

void App::refresh_images() {
	if (view == "home") refresh_home_cards();
	if (view == "search") refresh_search_cards();
	if (view == "discover") {
		for (size_t i = 0; i < disc_cards.size() && i < disc_items_.size(); i++) {
			bool near = int(i) / kDiscCols >= disc_sel / kDiscCols - 1 && int(i) / kDiscCols <= disc_sel / kDiscCols + 2;
			disc_cards[i].image = art(disc_items_[i].poster, ArtKind::PosterLarge, near);
		}
		discover_preview();
	}
	if (view == "library") {
		for (size_t i = 0; i < lib_cards.size() && i < lib_items_.size(); i++) {
			bool near = int(i) / 7 >= lib_sel / 7 - 2 && int(i) / 7 <= lib_sel / 7 + 3;
			lib_cards[i].image = art(lib_items_[i].poster, ArtKind::Poster, near);
		}
	}
	if (view == "detail") {
		d_background = art(d_meta_loaded_ ? d_meta_.background : d_item_.background, ArtKind::Backdrop);
		d_logo = art(d_meta_loaded_ ? d_meta_.logo : d_item_.logo, ArtKind::Logo);
		for (size_t i = 0; i < d_episodes.size() && i < d_season_videos_.size(); i++) {
			bool near = std::abs(int(i) - d_episode_sel) < 10;
			d_episodes[i].thumb = art(d_season_videos_[i]->thumbnail, ArtKind::Thumb, near);
		}
	}
	if (view == "addons")
		for (size_t i = 0; i < addon_rows.size() && i < addons_.size(); i++)
			addon_rows[i].logo = art(jstr(addons_[i]->manifest, "logo"), ArtKind::Icon);
	if (launch_visible) launch_image = art(w_item_.background, ArtKind::Background);
	dirty_all();
}

void App::update() {
	g_tasks.drain();
	input_poll();
	double now = now_seconds();

	if (now - last_clock_ > 1) {
		last_clock_ = now;
		std::string c = format_clock();
		if (c != clock) {
			clock = c;
			dirty("clock");
		}
	}
	if (!toast.empty() && now > toast_until_) {
		toast.clear();
		dirty("toast");
	}
	bool b = busy_count_ > 0 || addons_loading_;
	if (b != busy) {
		busy = b;
		dirty("busy");
	}
	if (login_visible) login_poll();
	if (t_visible) torrent_stats_poll();
	if (watching_ || launch_visible) watch_update();
	if (images_dirty_ && now - images_refreshed_ > 0.25) {
		images_dirty_ = false;
		images_refreshed_ = now;
		refresh_images();
	}
	if (all_dirty_) {
		model_.DirtyAllVariables();
		all_dirty_ = false;
	}
}

// Scrolling glides to where it should be instead of jumping: each frame
// covers a share of the distance (scroll_k_), whatever the frame rate.
float App::glide(float cur, float target) const {
	float d = target - cur;
	return std::fabs(d) < 0.5f ? target : cur + d * scroll_k_;
}

void App::scroll_list(const char* id, int index, float pitch) {
	Rml::Element* el = doc_->GetElementById(id);
	if (!el) return;
	float h = el->GetClientHeight();
	float top = index * pitch, cur = el->GetScrollTop(), want = cur;
	if (top < cur) want = top;
	else if (top + pitch > cur + h) want = top + pitch - h;
	if (want != cur) el->SetScrollTop(glide(cur, want));
}

// Strips are exactly a whole number of cards wide (board.rcss) and scroll by
// whole cards, so no card is ever cut at either edge.
void App::scroll_strip(const std::string& id, int index, float pitch) {
	Rml::Element* el = doc_->GetElementById(id);
	if (!el) return;
	float w = el->GetClientWidth();
	int visible = std::max(1, int(w / pitch + 0.5f));
	int last_first = std::max(0, int((el->GetScrollWidth() - w) / pitch + 0.01f));
	int& first = strip_first_[id];
	// Keep one card of context on either side of the selection.
	if (index - 1 < first) first = index - 1;
	else if (index + 1 > first + visible - 1) first = index + 2 - visible;
	first = std::max(0, std::min(first, last_first));
	float cur = el->GetScrollLeft(), want = first * pitch;
	if (want != cur) el->SetScrollLeft(glide(cur, want));
}

void App::after_layout() {
	if (!doc_) return;
	double now = now_seconds();
	double dt = std::min(0.1, std::max(0.0, now - last_layout_));
	last_layout_ = now;
	scroll_k_ = float(1.0 - std::exp(-dt * 18.0));
	if (dd_visible) scroll_list("dd-list", dd_sel, kDropdownPitch);
	if (watching_ && menu_visible) {
		scroll_list("menu-audio", m_audio_sel, kMenuPitch);
		scroll_list("menu-subs", m_sub_sel, kMenuPitch);
	}
	if (view == "home" || view == "search") {
		bool home = view == "home";
		Rml::Element* board = doc_->GetElementById(home ? "home-board" : "search-board");
		int row = home ? home_row : search_row, col = home ? home_col : search_col;
		if (board) board->SetScrollTop(glide(board->GetScrollTop(), row * kRowPitch));
		auto& rows = home ? home_rows : search_rows;
		if (row < int(rows.size())) scroll_strip(rows[row].dom_id, col, kCardPitch);
	} else if (view == "discover") {
		scroll_list("disc-grid", disc_sel / kDiscCols, kDiscRowPitch);
	} else if (view == "library") {
		scroll_list("lib-grid", lib_sel / 7, kGridRowPitch);
	} else if (view == "addons") {
		scroll_list("addon-list", addon_sel / kAddonCols, kAddonPitch);
	} else if (view == "settings") {
		scroll_list("settings-list", s_sel, kSettingPitch);
	} else if (view == "detail") {
		if (d_zone == "streams") scroll_list("stream-list", d_stream_sel, kStreamPitch);
		else scroll_list("episode-list", d_episode_sel, kEpisodePitch);
	}
}

void App::render_video() {
	if (watching_) player_.present(renderer_);
}
