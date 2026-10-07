// The app's state and everything that goes out to the network for it: the
// account, the addons, the catalogs, the library, a title's details and
// streams. It turns all of that into the plain content the screens draw
// (ui/content.hpp) and changes it in place as answers arrive; the shell reads
// what changed and tells the screens.
//
// One thread owns it (the one that draws). Blocking work runs on the task
// pool and its answers are delivered back to that thread in update().

#pragma once

#include "art.hpp"
#include "content.hpp"
#include "ime.hpp"
#include "model.hpp"
#include "overlays.hpp"
#include "player.h"
#include "subtitles.h"
#include "tasks.h"
#include "audio/cues.hpp"
#include "core/input.hpp"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace sx
{

// How a stream from an addon is listed: resolution badge, size, seeds, tags.
Stream stream_view(const ::Stream &stream);

class Session
{
  public:
    // Where the cursor is on each screen, so the artwork around it is fetched first.
    struct Cursor
    {
        int board_row = 0, board_col = 0;
        int discover = 0;
        int library = 0;
        int episode = 0;
        int search_row = 0, search_col = 0;
    };

    // What changed since take_changed() last said. kDiscoverNew / kLibraryNew mean
    // the list is a different one (a filter changed): the screen starts at the top.
    enum Changed : unsigned
    {
        kBoard = 1,
        kDiscover = 2,
        kLibrary = 4,
        kAddons = 8,
        kSettings = 16,
        kDetail = 32,
        kDiscoverNew = 64,
        kLibraryNew = 128,
        kSearch = 256,
        kSearchNew = 512,
    };

    bool init(const std::string &base_dir, const std::string &data_dir, hui::gfx::Renderer *renderer);
    void shutdown();
    void update(float dt, const Cursor &cursor);

    unsigned take_changed()
    {
        const unsigned c = changed_;
        changed_ = 0;
        return c;
    }
    // A page the app wants shown (after "See all" on the Board).
    bool take_goto(Page *page)
    {
        if (!goto_)
            return false;
        *page = *goto_;
        goto_.reset();
        return true;
    }
    // The sounds the screens should make, in the order things happened (the shell plays them).
    void cue(hui::audio::Cue c)
    {
        if (cues_.size() < 16)
            cues_.push_back(c);
    }
    std::vector<hui::audio::Cue> take_cues()
    {
        std::vector<hui::audio::Cue> v;
        v.swap(cues_);
        return v;
    }
    bool ui_sounds() const
    {
        return prefs_.ui_sounds;
    }
    bool wants_exit() const
    {
        return exit_;
    }

    // ---- what the screens draw ----
    BoardContent board;
    DiscoverContent discover;
    LibraryContent library;
    AddonsContent addons;
    SettingsContent settings;
    DetailContent detail;
    BoardContent search; // the results of a search, rows like the Board's
    std::string search_query;
    bool search_open = false; // the results are on show
    std::string clock; // "21:45"

    // ---- playing ----
    // The launch screen while a stream starts, then the player over the video.
    bool launch_open = false;
    LaunchState launch;
    bool watching = false; // the player is up (also during the launch screen)
    PlayerState playing;
    std::uint32_t video_texture = 0; // the current picture, 0 until the first one
    int video_w = 0, video_h = 0;
    bool typing() const
    {
        return ime_.active();
    }

    // ---- pop-ups ----
    bool dropdown_open = false;
    DropdownState dropdown;
    bool sign_in_open = false;
    SignInState sign_in;
    bool toast_open = false;
    ToastState toast;

    // ---- what the screens ask for ----
    void page_opened(Page page);
    // streams_video: for a series, the episode whose streams are shown at once (Continue Watching).
    void open_title(const Title &title, const std::string &streams_video = std::string());
    void close_detail();
    void detail_pick_episode(int index);
    void detail_change_season(int delta);
    void detail_toggle_watched(int index);
    void detail_play(int index, bool with_server);
    void cancel_autoplay()
    {
        autoplay_pending_ = false;
    }
    void filter_activate(Page page, int filter);
    void settings_activate(int row);
    void addons_reload();
    // Cross on an addon: one added by URL can be removed; the account's are managed in Stremio.
    void addon_activate(int index);
    // Square on the Board: "See all" (a catalog opens in Discover, Continue Watching in the Library).
    void board_square(int row, int col);
    // Whether a row of the Board is Continue Watching.
    bool board_is_continue(int row) const;
    // Cross on a card of Continue Watching: what was playing plays again (after asking), or the
    // streams of the next episode are shown.
    void continue_press(int row, int col);
    // Continue Watching asks for a title's page to be opened (the shell does it).
    bool take_open(Title *title, std::string *streams_video)
    {
        if (!open_req_)
            return false;
        *title = open_req_->first;
        *streams_video = open_req_->second;
        open_req_.reset();
        return true;
    }
    // Options on a card of Continue Watching: a menu, then a question, before it is removed.
    // False when the card has no options (the caller then treats the button as search).
    bool board_options(int row, int col);
    // True once the first screen has its catalogs and the posters in view (or has waited long enough).
    bool boot_ready() const
    {
        return boot_done_;
    }
    void dropdown_confirm();
    void dropdown_cancel();
    void sign_in_cancel();
    // The buttons while a stream starts or plays.
    void watch_input(const hui::InputFrame &input);
    // Asks for a search with the console's keyboard.
    void begin_search();
    void start_search(const std::string &query); // searches for a text already typed
    void close_search();
    void say(const std::string &message, double seconds = 4.0, bool error = false);

  private:
    template <typename T> void bg(std::function<T()> work, std::function<void(T &)> done);

    // persistence and account
    void load_prefs();
    void save_prefs();
    void load_progress();
    void save_progress();
    bool signed_in() const
    {
        return !prefs_.auth_key.empty();
    }
    void load_addons();
    void on_addons_loaded();
    void load_library();
    const ::Addon *find_addon(const std::string &url) const;
    std::vector<Item> continue_watching() const;

    // board
    void build_board();
    void refresh_board();
    void continue_remove(const Item &item);
    void resume_play(const Title &title, const Item &item);
    void start_remembered_stream();
    bool saved_stream(const std::string &item_id, const std::string &video_id, ::Stream *out) const;
    std::string next_episode_id() const;
    bool boot_check(double seconds) const;
    // discover
    void enter_discover();
    void discover_build_chips();
    void discover_load(bool more);
    // library
    void refresh_library();
    // addons, settings
    void refresh_addons();
    void refresh_settings();
    void open_dropdown(const std::string &title, const std::vector<std::string> &options, int active,
                       std::function<void(int)> chosen);
    void login_start();
    void login_poll();
    void login_done(const std::string &auth_key);
    void sign_out();
    // detail
    void detail_apply_meta();
    void detail_set_season(int index);
    void detail_load_streams(const std::string &video_id, const std::string &heading);
    void detail_refresh_streams();
    void detail_fill_title();
    bool watched(const std::string &video_id) const;
    // artwork
    void refresh_art(const Cursor &cursor);

    // search and typing
    void ask_text(const std::function<void(const std::string &)> &done);
    void refresh_search();
    void poll_typing();

    // playing (session_watch.cpp)
    struct WatchSub
    {
        std::string label, lang;
        int embedded = -1;      // stream index in the file, or
        std::string url;        // an addon's subtitle file
        std::vector<Cue> cues;  // once loaded
        bool loading = false, failed = false;
    };
    void play_stream(const ::Stream &stream, bool transcode);
    void watch_start(const std::string &url, const std::vector<std::string> &headers, double start,
                     const std::string &title, const std::string &subtitle, bool direct);
    void watch_update();
    void watch_stop(bool ended);
    void watch_save_progress(bool final, bool force_finished = false);
    void watch_show_info();
    void watch_build_menu();
    void watch_select_sub(int index);
    void watch_load_addon_subtitles();
    void watch_auto_subtitles();
    void watch_next_episode();
    void upload_video();
    void torrent_stats_start(const std::string &url, const std::string &info_hash);
    void torrent_stats_poll();
    std::vector<std::string> pref_langs(const std::string &list) const;
    std::string server() const;

    Title make_title(const Item &item, bool with_progress) const;
    // The library's items carry no backdrop or description: both are asked of the
    // addons for the title under the cursor, kept here and filled in everywhere.
    void apply_meta(Item &item) const;
    void want_meta(const Item &item);

    std::string base_dir_, data_dir_;
    hui::gfx::Renderer *renderer_ = nullptr;
    TexCache tex_;
    Prefs prefs_;
    std::map<std::string, Progress> progress_;
    std::set<std::string> watched_;
    unsigned changed_ = 0;
    Page page_ = Page::board; // the page on show
    std::optional<Page> goto_;
    bool exit_ = false;
    std::vector<hui::audio::Cue> cues_;
    std::optional<std::pair<Title, std::string>> open_req_;
    std::string want_streams_video_; // open_title: show these streams as soon as the details are in
    bool resume_play_ = false;       // open_title: play the remembered stream as soon as the details are in
    bool resuming_ = false;          // the launch screen is up before play_stream has been called
    bool resumed_from_board_ = false; // the title's page is open behind the Board only for playing
    std::string resume_video_;
    double clock_ = 0;
    Cursor last_cursor_;
    double last_art_ = 0;
    bool art_dirty_ = true;
    double toast_until_ = 0;

    // addons and library
    std::vector<std::unique_ptr<::Addon>> addons_;
    bool addons_loading_ = false;
    bool board_loading_ = false; // some catalog of the Board has not answered yet
    bool boot_done_ = false;
    double boot_start_ = 0.0;
    int addons_gen_ = 0;
    std::map<std::string, json> library_; // library items by _id
    bool library_loaded_ = false;

    // details of titles the library listed without them, by "type:id"
    std::map<std::string, Item> meta_cache_;
    std::set<std::string> meta_asked_;

    // board
    std::vector<BoardRow> board_;
    std::vector<int> board_map_; // content row -> board_ index
    int home_gen_ = 0;

    // discover
    struct DiscCatalog
    {
        const ::Addon *addon;
        const Catalog *catalog;
    };
    std::vector<std::string> disc_types_;
    std::string disc_type_;
    std::vector<DiscCatalog> disc_catalogs_;
    int disc_catalog_ = 0;
    std::string disc_genre_;
    std::vector<Item> disc_items_;
    bool disc_loading_ = false, disc_end_ = false;
    int disc_gen_ = 0;

    // library
    std::vector<Item> lib_items_;
    std::string lib_type_;
    int lib_sort_ = 0; // 0 recent, 1 A-Z, 2 most watched

    // dropdown
    std::function<void(int)> dropdown_chosen_;

    // sign-in
    std::string login_code_raw_, login_qr_url_;
    double login_next_poll_ = 0, login_expires_ = 0;
    bool login_polling_ = false;
    int login_gen_ = 0;
    bool confirm_sign_out_ = false;

    // detail
    Item d_item_;
    ::Meta d_meta_;
    bool d_meta_loaded_ = false;
    std::vector<int> d_seasons_;
    int d_season_idx_ = 0;
    std::vector<const ::Video *> d_season_videos_;
    std::string d_video_id_;
    std::string d_stream_heading_;
    std::vector<::Stream> d_streams_; // in the order the screen shows them
    std::vector<::Stream> d_found_;   // as the addons sent them
    int d_gen_ = 0, d_pending_ = 0, d_answered_ = 0, d_meta_gen_ = 0;
    bool d_open_ = false;

    // search
    Ime ime_;
    std::function<void(const std::string &)> text_done_;
    std::vector<BoardRow> search_;
    std::vector<int> search_map_;
    int search_gen_ = 0, search_pending_ = 0;

    // playing
    Player player_;
    Item w_item_;
    std::string w_video_id_;
    ::Stream w_stream_;
    std::vector<WatchSub> w_subs_;
    int w_sub_active_ = -1; // index in w_subs_, -1 off
    double w_sub_delay_ = 0;
    double w_info_until_ = 0, w_last_save_ = 0, w_last_saved_pos_ = -1;
    bool w_started_ = false, w_menu_built_ = false, w_auto_sub_done_ = false, w_heavy_warned_ = false;
    bool w_menu_dirty_ = false;
    double w_stats_at_ = 0, w_stats_logged_ = 0;
    int w_gen_ = 0;
    std::shared_ptr<std::atomic<bool>> launch_cancel_;
    std::string subtitle_shown_;
    // the torrent's statistics while it starts or buffers
    std::string t_stats_url_;
    int t_gen_ = 0;
    double t_next_poll_ = 0;
    bool t_polling_ = false, t_visible_ = false;
    int t_peers_ = 0, t_known_ = -1;
    double t_speed_ = 0, t_progress_ = -1;
    bool t_has_meta_ = false;
    // the next episode, picked once its streams are in
    bool autoplay_pending_ = false;
    std::string autoplay_binge_, autoplay_addon_;

    // "See all" on the Board: which catalog Discover should open on.
    std::string see_all_addon_, see_all_catalog_, see_all_type_;
    std::vector<std::string> addon_logos_;
};

template <typename T> void Session::bg(std::function<T()> work, std::function<void(T &)> done)
{
    g_tasks.run<T>(std::move(work), std::move(done));
}

} // namespace sx
