#pragma once

#include <RmlUi/Core.h>
#include <SDL.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "artcache.h"
#include "player.h"
#include "stremio.h"
#include "tasks.h"
#include "subtitles.h"
#include "util.h"

// ---------------------------------------------------------------------------
// Data model structs (names match assets/main.rml)

struct UiCard {
	std::string title, image, initials, badge, progress = "0%";
};
struct UiRow {
	std::string title, dom_id, message;
	bool see_all = false;
	std::vector<UiCard> cards;
};
struct UiChip {
	std::string value;
};
constexpr int kAddonCols = 4;  // addon tiles per row (browse.rcss)
constexpr int kDiscCols = 4;   // discover posters per row (browse.rcss #disc-grid)
struct UiAddon {
	std::string name, version, desc, types, logo, initials;
	bool local = false;
};
struct UiSetting {
	std::string label, hint, value;
};
struct UiEpisode {
	std::string title, sub, thumb;
	bool watched = false;
};
struct UiStream {
	std::string name, addon, desc;
};
struct UiMenuItem {
	std::string label;
	bool active = false;
};
struct UiKey {
	std::string label;
	bool wide = false;
};

// Something that can be opened: a catalog entry, a library item.
struct Item {
	std::string id, type, name, poster, background, logo;
	std::string description, release_info, runtime, imdb_rating;
	std::vector<std::string> genres;
	// Continue watching
	std::string video_id;
	double offset = 0, duration = 0;  // seconds
	int64_t last_watched = 0;          // epoch ms
	int times_watched = 0;
};
Item item_from_meta_json(const json& j);

enum class Btn { Up, Down, Left, Right, Cross, Circle, Square, Triangle, Options, L1, R1, L2, R2, L3, R3, Touchpad };

// Board / search rows: one per catalog.
struct BoardRow {
	std::string title;
	const Addon* addon = nullptr;
	std::string addon_url;
	Catalog catalog;
	std::vector<Item> items;
	bool continue_watching = false;
	bool loaded = false;
};

struct Settings {
	std::string server_url;
	// Torrents: the app's own engine (no server needed), or the server.
	bool builtin_torrents = true;
	std::string subtitle_langs = "eng";
	bool auto_subtitles = true;
	std::string sub_size = "sub-m";
	std::string audio_langs;
	bool autoplay_next = true;
	std::vector<std::string> extra_addons;
	std::string auth_key, user_email;
};

struct Progress {
	std::string type, name, poster, video_id;
	double time = 0, duration = 0;
	int64_t updated = 0;
};

class App {
public:
	bool init(Rml::Context* ctx, SDL_Renderer* renderer, const std::string& base_dir, const std::string& data_dir);
	void shutdown();

	void on_button(Btn b);
	void on_text(const std::string& utf8);  // text from the keyboard dialog
	void input_submit();                     // its Done (a Return key)
	void update();          // every frame, before the RmlUi update
	void after_layout();    // every frame, after the RmlUi update (scrolling)
	void render_video();    // before RmlUi renders
	bool wants_exit() const { return exit_; }
	bool watching() const { return watching_; }
	bool text_entry_active() const { return input_visible_; }

	// -------------------------------------------------------------------
	// Data model (bound by name)
	std::string zone = "nav", view = "home";
	int nav_sel = 0;
	std::string search_query, clock, user, banner;
	bool busy = false;

	std::vector<UiRow> home_rows;
	int home_row = 0, home_col = 0;
	std::vector<UiRow> search_rows;
	int search_row = 0, search_col = 0;
	std::string search_status;

	std::vector<UiChip> disc_chips;
	int disc_chip = 0;
	std::vector<UiCard> disc_cards;
	int disc_sel = 0;
	std::string disc_status;
	std::string dp_name, dp_logo, dp_still, dp_runtime, dp_year, dp_imdb, dp_genres, dp_desc;

	std::vector<UiChip> lib_chips;
	int lib_chip = 0;
	std::vector<UiCard> lib_cards;
	int lib_sel = 0;
	std::string lib_status;

	std::vector<UiAddon> addon_rows;
	int addon_sel = 0;

	std::vector<UiSetting> s_rows;
	int s_sel = 0;

	std::string d_background, d_logo, d_name, d_runtime, d_year, d_imdb, d_genres, d_cast, d_directors,
	    d_description, d_resume;
	bool d_series = false;
	std::string d_zone = "streams", d_season_label;
	std::vector<UiEpisode> d_episodes;
	int d_episode_sel = 0;
	std::string d_streams_title, d_streams_status;
	std::vector<UiStream> d_streams;
	int d_stream_sel = 0;
	// Streams come in two steps: pick a resolution, then a stream of it.
	bool d_pick_res = true;

	bool watching_ = false;
	std::string w_sub_rml, w_sub_size = "sub-m";
	bool info_visible = true, w_buffering = false, w_paused = false, menu_visible = false;
	std::string w_buffer_text, w_title, w_subtitle, w_progress = "0%", w_time, w_duration, w_tracks, w_stats;
	int m_col = 0;
	std::vector<UiMenuItem> m_audio, m_subs;
	int m_audio_sel = 0, m_sub_sel = 0;
	std::string m_delay;

	bool launch_visible = false;
	std::string launch_image, launch_title, launch_status;

	// Torrent statistics from the streaming server, while a torrent starts
	// or buffers (like the PC app's Statistics panel).
	bool t_visible = false;
	std::string t_peers = "0", t_speed = "0.00 MB/s", t_progress = "0.00 %", t_hash;

	bool dd_visible = false;
	std::string dd_title;
	std::vector<UiMenuItem> dd_options;
	int dd_sel = 0;

	bool login_visible = false;
	std::string login_qr, login_link, login_code, login_status;

	// Text entry through the PS5's own keyboard dialog (app_settings.cpp).
	bool input_visible_ = false, input_shown_ = false;
	double input_opened_ = 0;
	std::string input_value;

	std::string toast;

private:
	// app.cpp
	void bind_model();
	void dirty(const char* name);
	void dirty_all() { all_dirty_ = true; }
	void show_toast(const std::string& msg, double seconds = 4);
	void set_view(const std::string& v);
	void nav_button(Btn b);
	bool browse_shortcut(Btn b);
	void load_settings();
	void save_settings();
	void load_progress();
	void save_progress();
	void load_addons();
	void on_addons_loaded();
	void load_library();
	const Addon* find_addon(const std::string& url) const;
	void scroll_list(const char* id, int index, float pitch);
	float glide(float cur, float target) const;
	double last_layout_ = 0;
	float scroll_k_ = 1;
	std::map<std::string, int> strip_first_;  // first fully shown card, by strip id
	void scroll_strip(const std::string& id, int index, float pitch);
	// Local artwork path, "" until downloaded (then the page's images are
	// refreshed). With want=false only looks in the cache.
	std::string art(const std::string& url, ArtKind kind, bool want = true);
	void refresh_images();

	// Background work with the top bar's busy indicator.
	template <typename T>
	void bg(std::function<T()> work, std::function<void(T&)> done) {
		busy_count_++;
		g_tasks.run<T>(work, [this, done](T& v) {
			busy_count_--;
			done(v);
		});
	}
	int busy_count_ = 0;
	bool images_dirty_ = false;
	double images_refreshed_ = 0;
	std::string server() const;
	bool signed_in() const { return !settings_.auth_key.empty(); }
	std::vector<std::string> pref_sub_langs() const;
	std::vector<std::string> pref_audio_langs() const;

	// app_board.cpp
	void build_home();
	void refresh_home_cards();
	void board_button(Btn b);
	void open_item(const Item& it);
	void start_search(const std::string& q);
	void refresh_search_cards();
	void search_button(Btn b);
	UiCard make_card(const Item& it, bool show_progress);
	std::vector<Item> continue_watching() const;
	void remove_from_continue_watching(const Item& it);

	// app_browse.cpp
	void enter_discover();
	void discover_build_chips();
	void discover_load(bool more);
	void discover_preview();
	void discover_button(Btn b);
	void discover_see_all(const BoardRow& row);
	void enter_library();
	void library_refresh();
	void library_button(Btn b);
	void enter_addons();
	void addons_button(Btn b);
	void open_dropdown(const std::string& title, const std::vector<std::string>& options, int active,
	                   std::function<void(int)> chosen);
	void dropdown_button(Btn b);

	// app_detail.cpp
	void detail_open(const Item& it);
	void detail_apply_meta();
	void detail_set_season(int idx);
	void detail_load_streams(const std::string& video_id);
	void detail_refresh_streams();
	void detail_button(Btn b);
	void detail_close();
	void detail_update_resume();
	void play_stream(const Stream& st, bool transcode);
	std::string d_res_;                 // the resolution being shown (when !d_pick_res)
	std::vector<std::string> d_res_keys_;  // resolution list: row -> resolution
	std::vector<int> d_view_;           // stream list: row -> d_stream_list_ index
	int d_res_sel_ = 0;                 // row to return to in the resolution list
	std::string d_streams_base_title_;
	void torrent_stats_start(const std::string& url, const std::string& info_hash);
	void torrent_stats_poll();
	std::shared_ptr<std::atomic<bool>> launch_cancel_;
	std::string t_stats_url_;
	double t_next_poll_ = 0;
	bool t_polling_ = false;
	int t_gen_ = 0;
	bool watched(const std::string& video_id) const;
	void toggle_watched(const std::string& video_id);
	std::string episode_label(const Video& v) const;

	// app_settings.cpp
	void enter_settings();
	void settings_refresh();
	void settings_button(Btn b);
	void login_start();
	void login_poll();
	void login_close();
	void login_done(const std::string& auth_key);
	void sign_out();
	void open_input(const std::string& title, const std::string& value, const std::string& hint,
	                std::function<void(const std::string&)> done);
	void input_poll();
	void input_finish(bool ok);

	// app_watch.cpp
	// direct: the addon's own link (not through the streaming server)
	void watch_start(const std::string& url, const std::vector<std::string>& headers, double start,
	                 const std::string& title, const std::string& subtitle, bool direct = false);
	void watch_update();
	void watch_button(Btn b);
	void watch_stop(bool ended);
	void watch_save_progress(bool final);
	void watch_build_menu();
	void watch_menu_button(Btn b);
	void watch_select_sub(int idx);
	void watch_load_addon_subtitles();
	void watch_auto_subtitles();
	void watch_next_episode();
	void watch_show_info();

	// -------------------------------------------------------------------
	Rml::Context* ctx_ = nullptr;
	Rml::ElementDocument* doc_ = nullptr;
	Rml::DataModelHandle model_;
	SDL_Renderer* renderer_ = nullptr;
	std::string base_dir_, data_dir_;
	bool all_dirty_ = true;
	bool exit_ = false;
	double toast_until_ = 0;
	double last_clock_ = 0;
	int generation_ = 0;  // bumped to invalidate in-flight work for a page

	Settings settings_;
	std::map<std::string, Progress> progress_;  // by meta id
	std::set<std::string> watched_;              // video ids
	std::string config_note_;

	// Addons
	std::vector<std::unique_ptr<Addon>> addons_;
	bool addons_loading_ = false;
	int addons_gen_ = 0;

	// Library (account)
	std::map<std::string, json> library_;  // libraryItem by _id
	bool library_loaded_ = false;

	// Board / search. The UI rows skip empty catalogs; the maps give the
	// board_/search_ index of each UI row.
	std::vector<BoardRow> board_;
	std::vector<int> home_map_;
	int home_gen_ = 0;
	std::vector<BoardRow> search_;
	std::vector<int> search_map_;
	int search_gen_ = 0, search_pending_ = 0;

	// Discover
	struct DiscCatalog {
		const Addon* addon;
		const Catalog* catalog;
	};
	std::vector<std::string> disc_types_;
	std::string disc_type_;
	std::vector<DiscCatalog> disc_catalogs_;  // for disc_type_
	int disc_catalog_ = 0;
	std::string disc_genre_;  // "" = none
	std::vector<Item> disc_items_;
	bool disc_loading_ = false, disc_end_ = false;
	int disc_gen_ = 0;

	// Library
	std::vector<Item> lib_items_;
	std::string lib_type_;  // "" = all
	int lib_sort_ = 0;      // 0 recent, 1 A-Z, 2 most watched

	// Previous place, for Circle on the detail page.
	std::string return_view_ = "home", return_zone_ = "content";

	// Detail
	Item d_item_;
	Meta d_meta_;
	bool d_meta_loaded_ = false;
	std::vector<int> d_seasons_;
	int d_season_idx_ = 0;
	std::vector<const Video*> d_season_videos_;
	std::string d_video_id_;  // streams are for this (meta id for movies)
	std::vector<Stream> d_stream_list_;
	int d_gen_ = 0, d_pending_ = 0, d_answered_ = 0;  // streams
	int d_meta_gen_ = 0;

	// Dropdown
	std::function<void(int)> dd_chosen_;

	// Login
	std::string login_code_raw_, login_qr_url_;
	double login_next_poll_ = 0, login_expires_ = 0;
	bool login_polling_ = false;
	int login_gen_ = 0;

	// Input
	std::function<void(const std::string&)> input_done_;

	// Watch
	Player player_;
	Item w_item_;
	std::string w_video_id_;
	Stream w_stream_;
	bool w_transcode_ = false;
	double w_info_until_ = 0;
	double w_last_save_ = 0;
	double w_start_ = 0;
	bool w_started_ = false;
	bool w_heavy_warned_ = false;
	struct WatchSub {
		std::string label, lang;
		int embedded = -1;       // stream index, or
		std::string url;         // addon subtitle
		std::vector<Cue> cues;   // loaded
		bool loading = false, failed = false;
	};
	std::vector<WatchSub> w_subs_;
	int w_sub_active_ = -1;      // index in w_subs_, -1 off
	double w_sub_delay_ = 0;
	bool w_menu_built_ = false;
	bool w_auto_sub_done_ = false;
	int w_gen_ = 0;
	double w_last_ui_ = 0;
	double w_last_saved_pos_ = -1;
	std::string w_sub_shown_;

	// Next episode: play a stream like the last one once streams arrive.
	bool autoplay_pending_ = false;
	std::string autoplay_binge_, autoplay_addon_;
};
