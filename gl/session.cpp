// The session's core: setup, the account, addons, the library, settings, the
// pop-ups and the artwork. The Board, Discover and Library are session_browse.cpp,
// a title's page is session_detail.cpp.

#include "session.hpp"

#include "http.h"
#include "hdr_out.hpp"
#include "netstream.h"
#include "version.hpp"
#include "torrent/engine.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace sx
{

namespace
{

constexpr std::size_t kTextureBudget = 640u << 20; // bytes of artwork kept on the GPU

enum SettingIndex
{
    kAccount,
    kProfile,
    kTorrents,
    kServer,
    kTorbox,
    kSubLangs,
    kAutoSubs,
    kSubSize,
    kAudioLangs,
    kAutoplay,
    kUiSounds,
    kHdr,
    kVrr,
    kAddAddon,
    kUpdates,
    kReload,
    kExit,
    kSettingCount
};

std::string size_label(const std::string &s)
{
    if (s == "sub-s")
        return "Small";
    if (s == "sub-l")
        return "Large";
    return "Medium";
}

// The first letter of a name (one UTF-8 character): an addon's mark when it has no logo.
std::string first_letter(const std::string &name)
{
    std::size_t len = name.empty() ? 0 : 1;
    while (len < name.size() && (static_cast<unsigned char>(name[len]) & 0xC0) == 0x80)
        ++len;
    return name.substr(0, len);
}

std::uint32_t colour_for(const std::string &name)
{
    static const std::uint32_t palette[] = {0x7b5bf5, 0x3b82f6, 0x0ea5a4, 0xe0568a, 0xe08a3c, 0x6d6ae0};
    return palette[fnv1a(name) % (sizeof(palette) / sizeof(palette[0]))];
}

} // namespace

Item item_from_meta_json(const json &j)
{
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
    const json &g = jobj(j, "genres");
    if (g.is_array())
        for (const auto &v : g)
            if (v.is_string())
                it.genres.push_back(v.get<std::string>());
    return it;
}

// ---------------------------------------------------------------------------
// Setup

bool Session::init(const std::string &base_dir, const std::string &data_dir, hui::gfx::Renderer *renderer)
{
    base_dir_ = base_dir;
    data_dir_ = data_dir;
    renderer_ = renderer;
    make_dirs(data_dir_);
    log_open(data_dir_ + "/log.txt");
    http_init(base_dir_ + "/ca-bundle.crt");
    g_tasks.start(6);
    boot_start_ = now_seconds();
    tex_.init(data_dir_ + "/art", renderer_, 6);
    // The app's storage holds the read-ahead: the built-in torrent engine's 6 GB
    // rolling cache, and up to 6 GB for a stream from a server or a direct link.
    bt::Engine::get().configure(data_dir_, 6ll << 30);
    NetStream::set_cache_dir(data_dir_);

    load_profiles();
    load_prefs();
    load_progress();
    clock = format_clock();
    board.status = "Loading your catalogs...";
    refresh_settings();
    refresh_library();
    refresh_addons();
    load_addons();
    if (signed_in())
        load_library();
    // No account and no one to choose between: the sign-in screen comes before the Board, its code asked for now so
    // that it is there when the boot animation ends. (With several profiles it comes after the one picked, if needed.)
    if (profiles().size() <= 1)
        start_sign_in_screen();
    return true;
}

void Session::shutdown()
{
    if (watching)
        watch_stop(false);
    player_.close();
    bt::Engine::get().shutdown();
    g_tasks.stop();
    tex_.shutdown();
    save_progress();
}

// ---------------------------------------------------------------------------
// Persistence

void Session::load_prefs()
{
    json j;
    if (load_json(profile_file("settings.json"), j))
    {
        prefs_.server_url = jstr(j, "server_url");
        prefs_.builtin_torrents = jbool(j, "builtin_torrents", prefs_.builtin_torrents);
        prefs_.subtitle_langs = jstr(j, "subtitle_languages", prefs_.subtitle_langs);
        prefs_.auto_subtitles = jbool(j, "auto_subtitles", prefs_.auto_subtitles);
        prefs_.sub_size = jstr(j, "subtitle_size", prefs_.sub_size);
        prefs_.audio_langs = jstr(j, "audio_languages");
        prefs_.autoplay_next = jbool(j, "autoplay_next", prefs_.autoplay_next);
        prefs_.ui_sounds = jbool(j, "ui_sounds", prefs_.ui_sounds);
        prefs_.hdr_output = jbool(j, "hdr_output", prefs_.hdr_output);
        prefs_.match_refresh = jbool(j, "match_refresh", prefs_.match_refresh);
        prefs_.auth_key = jstr(j, "auth_key");
        prefs_.user_email = jstr(j, "user_email");
        prefs_.torbox_key = jstr(j, "torbox_key");
        const json &ex = jobj(j, "extra_addons");
        if (ex.is_array())
            for (const auto &v : ex)
                if (v.is_string())
                    prefs_.extra_addons.push_back(v.get<std::string>());
    }
}

json Session::prefs_json(const Prefs &p)
{
    json j;
    j["server_url"] = p.server_url;
    j["builtin_torrents"] = p.builtin_torrents;
    j["subtitle_languages"] = p.subtitle_langs;
    j["auto_subtitles"] = p.auto_subtitles;
    j["subtitle_size"] = p.sub_size;
    j["audio_languages"] = p.audio_langs;
    j["autoplay_next"] = p.autoplay_next;
    j["ui_sounds"] = p.ui_sounds;
    j["hdr_output"] = p.hdr_output;
    j["match_refresh"] = p.match_refresh;
    j["extra_addons"] = p.extra_addons;
    j["auth_key"] = p.auth_key;
    j["user_email"] = p.user_email;
    if (!p.torbox_key.empty())
        j["torbox_key"] = p.torbox_key;
    return j;
}

void Session::save_prefs()
{
    if (!save_json(profile_file("settings.json"), prefs_json(prefs_)))
        say("Could not save settings to " + data_dir_, 5.0, true);
}

void Session::load_progress()
{
    json j;
    if (!load_json(profile_file("progress.json"), j))
        return;
    const json &items = jobj(j, "items");
    if (items.is_object())
        for (auto it = items.begin(); it != items.end(); ++it)
        {
            Progress p;
            p.type = jstr(it.value(), "type");
            p.name = jstr(it.value(), "name");
            p.poster = jstr(it.value(), "poster");
            p.video_id = jstr(it.value(), "video_id");
            p.time = jnum(it.value(), "time");
            p.duration = jnum(it.value(), "duration");
            p.updated = static_cast<std::int64_t>(jnum(it.value(), "updated"));
            p.stream_json = jstr(it.value(), "stream");
            p.stream_video_id = jstr(it.value(), "stream_video");
            p.up_next = jstr(it.value(), "up_next");
            progress_[it.key()] = p;
        }
    const json &w = jobj(j, "watched");
    if (w.is_array())
        for (const auto &v : w)
            if (v.is_string())
                watched_.insert(v.get<std::string>());
}

void Session::save_progress()
{
    json j;
    json items = json::object();
    for (const auto &kv : progress_)
        items[kv.first] = json{{"type", kv.second.type},         {"name", kv.second.name},
                               {"poster", kv.second.poster},     {"video_id", kv.second.video_id},
                               {"time", kv.second.time},         {"duration", kv.second.duration},
                               {"updated", kv.second.updated},   {"stream", kv.second.stream_json},
                               {"stream_video", kv.second.stream_video_id}, {"up_next", kv.second.up_next}};
    j["items"] = items;
    j["watched"] = json(std::vector<std::string>(watched_.begin(), watched_.end()));
    save_json(profile_file("progress.json"), j);
}

// ---------------------------------------------------------------------------
// Addons and the library

const ::Addon *Session::find_addon(const std::string &url) const
{
    for (const auto &a : addons_)
        if (a->transport_url == url)
            return a.get();
    return nullptr;
}

void Session::load_addons()
{
    struct Result
    {
        std::vector<std::shared_ptr<::Addon>> addons;
        std::vector<std::string> errors;
        bool auth_failed = false;
    };
    const std::string key = prefs_.auth_key;
    const std::vector<std::string> extra = prefs_.extra_addons;
    const int gen = ++addons_gen_;
    addons_loading_ = true;
    bg<Result>(
        [key, extra]() {
            Result r;
            std::set<std::string> seen;
            if (!key.empty())
            {
                const ApiResult ar = api_addon_collection(key);
                if (ar.ok)
                {
                    const json &list = jobj(ar.result, "addons");
                    if (list.is_array())
                        for (const auto &e : list)
                        {
                            auto a = std::make_shared<::Addon>();
                            const std::string url = jstr(e, "transportUrl");
                            if (parse_addon(jobj(e, "manifest"), url, *a))
                            {
                                a->from_account = true;
                                if (seen.insert(url).second)
                                    r.addons.push_back(a);
                            }
                        }
                }
                else
                {
                    r.errors.push_back("Could not load your addons (" + ar.error + ")");
                    const std::string l = lower(ar.error);
                    if (l.find("session") != std::string::npos || l.find("auth") != std::string::npos)
                        r.auth_failed = true;
                }
            }
            std::vector<std::string> urls;
            if (r.addons.empty())
                urls = {kCinemetaUrl, kOpenSubtitlesUrl};
            for (const auto &u : extra)
                urls.push_back(u);
            for (const auto &u : urls)
            {
                if (seen.count(u))
                    continue;
                auto a = std::make_shared<::Addon>();
                std::string err;
                if (fetch_addon(u, *a, err))
                {
                    seen.insert(u);
                    r.addons.push_back(a);
                }
                else
                {
                    r.errors.push_back("Could not reach " + u + " (" + err + ")");
                }
            }
            return r;
        },
        [this, gen](Result &r) {
            if (gen != addons_gen_)
                return;
            addons_loading_ = false;
            addons_.clear();
            for (const auto &a : r.addons)
                addons_.push_back(std::make_unique<::Addon>(*a));
            if (!r.errors.empty())
                say(r.errors.front(), 6.0, true);
            if (r.auth_failed)
                say("Your Stremio session expired. Sign in again in Settings.", 8.0, true);
            dlog("addons: %zu loaded", addons_.size());
            on_addons_loaded();
        });
}

void Session::on_addons_loaded()
{
    build_board();
    // Discover starts again from the new catalogs the next time it opens.
    disc_items_.clear();
    discover.items.clear();
    disc_types_.clear();
    discover.status.clear();
    changed_ |= kDiscover | kDiscoverNew;
    refresh_addons();
    refresh_settings();
    if (page_ == Page::discover)
        enter_discover();
}

void Session::load_library()
{
    const std::string key = prefs_.auth_key;
    if (key.empty())
        return;
    bg<ApiResult>([key]() { return api_library_get(key); },
                  [this](ApiResult &r) {
                      if (!r.ok)
                      {
                          say("Could not load your library: " + r.error, 5.0, true);
                          return;
                      }
                      library_.clear();
                      if (r.result.is_array())
                          for (const auto &it : r.result)
                          {
                              const std::string id = jstr(it, "_id");
                              if (!id.empty())
                                  library_[id] = it;
                          }
                      library_loaded_ = true;
                      dlog("library: %zu items", library_.size());
                      refresh_board();
                      refresh_library();
                  });
}

void Session::addons_reload()
{
    say("Reloading addons...");
    load_addons();
    if (signed_in())
        load_library();
}

void Session::addon_activate(int index)
{
    if (index < 0 || index >= static_cast<int>(addons_.size()))
        return;
    const std::string url = addons_[static_cast<std::size_t>(index)]->transport_url;
    const auto it = std::find(prefs_.extra_addons.begin(), prefs_.extra_addons.end(), url);
    if (it == prefs_.extra_addons.end())
    {
        say("Install and remove addons in the Stremio app on your phone or computer; they sync here. "
            "Press Square to reload them now.",
            7.0);
        return;
    }
    open_dropdown(addons_[static_cast<std::size_t>(index)]->name, {"Remove addon", "Keep"}, 1, [this, url](int i) {
        if (i != 0)
            return;
        auto &ex = prefs_.extra_addons;
        ex.erase(std::remove(ex.begin(), ex.end(), url), ex.end());
        save_prefs();
        load_addons();
    });
}

// ---------------------------------------------------------------------------
// Addons and settings screens

void Session::refresh_addons()
{
    addons.items.clear();
    addon_logos_.clear();
    for (const auto &a : addons_)
    {
        sx::Addon row;
        row.name = a->name;
        row.version = a->version.empty() ? "" : "v" + a->version;
        row.description = a->description;
        std::vector<std::string> types;
        for (const auto &t : a->types)
            types.push_back(t);
        row.types = join(types, " ");
        row.local = !a->from_account;
        row.initial = first_letter(a->name);
        row.colour = colour_for(a->name);
        addons.items.push_back(row);
        addon_logos_.push_back(jstr(a->manifest, "logo"));
    }
    changed_ |= kAddons;
    art_dirty_ = true;
}

void Session::refresh_settings()
{
    settings.account = prefs_.user_email;
    settings.rows.assign(kSettingCount, SettingRow());
    const auto set = [&](int i, SettingRow::Kind kind, const char *label, const std::string &value, const std::string &help,
                         bool on = false) {
        SettingRow &r = settings.rows[static_cast<std::size_t>(i)];
        r.kind = kind;
        r.label = label;
        r.value = value;
        r.help = help;
        r.on = on;
    };
    using Kind = SettingRow::Kind;
    set(kAccount, Kind::value, "Stremio account", signed_in() ? prefs_.user_email : "Not signed in",
        signed_in() ? (confirm_sign_out_ ? "Press Cross again to sign out." : "Press Cross to sign out.")
                    : "Sign in to sync your addons, library and Continue Watching. Press Cross.");
    set(kProfile, Kind::value, "Change profile", profile_name(active_),
        "Choose who is watching, or add a profile. Each profile has its own Stremio account, library, Continue Watching and addons. Press Cross.");
    set(kTorrents, Kind::value, "Play torrents with", prefs_.builtin_torrents ? "This app" : "Streaming server",
        prefs_.builtin_torrents ? "The app downloads torrents itself; no server needed. Press Cross to use your server."
                                : "Your Stremio streaming server downloads torrents. Press Cross to use the app's own.");
    set(kServer, Kind::value, "Streaming server", prefs_.server_url.empty() ? "Not set" : prefs_.server_url,
        "Optional: for server transcoding. Your Stremio server, for example http://192.168.1.20:11470.");
    set(kTorbox, Kind::value, "Torbox", prefs_.torbox_key.empty() ? "Not connected" : "Connected",
        debrid_confirm_ == 2 ? "Press Cross again to disconnect Torbox."
        : prefs_.torbox_key.empty()
            ? "Torrents and streams are played through your Torbox account (only the ones already cached there). Press Cross to connect with a code."
            : "Press Cross to disconnect.");
    set(kSubLangs, Kind::value, "Subtitle languages", prefs_.subtitle_langs.empty() ? "None" : prefs_.subtitle_langs,
        "Preferred order, for example \"gre, eng\".");
    set(kAutoSubs, Kind::toggle, "Subtitles on automatically", "", "Turn subtitles on automatically in your preferred language.",
        prefs_.auto_subtitles);
    set(kSubSize, Kind::value, "Subtitle size", size_label(prefs_.sub_size), "How large subtitles are drawn over the video.");
    set(kAudioLangs, Kind::value, "Audio languages", prefs_.audio_langs.empty() ? "Default" : prefs_.audio_langs,
        "Preferred audio track language, for example \"eng\". Leave empty for the default.");
    set(kAutoplay, Kind::toggle, "Play next episode automatically", "", "Start the next episode when one ends.",
        prefs_.autoplay_next);
    set(kUiSounds, Kind::toggle, "Interface sounds", "", "A soft sound when you move around, go back or switch page.", prefs_.ui_sounds);
    set(kHdr, Kind::value, "HDR output", prefs_.hdr_output ? std::string(hdr::status()) : std::string("Off"),
        "Send the picture to the TV as HDR (video as HDR10 or HLG, the interface at normal white). Tried when the app "
        "starts; the line says what the console answered. Press Cross to turn it on or off.");
    set(kVrr, Kind::toggle, "Match refresh rate to video", "",
        "With VRR on in the console's video settings and a TV that supports it, the TV refreshes at a multiple of the "
        "video's frame rate (a 24 fps film at 48 Hz, each frame shown twice) instead of a fixed 60 Hz, so motion is even.",
        prefs_.match_refresh);
    set(kAddAddon, Kind::value, "Add addon by URL", "",
        prefs_.extra_addons.empty() ? "An addon's manifest URL. Remove added addons from the Addons page."
                                    : std::to_string(prefs_.extra_addons.size()) + " added here.");
    set(kUpdates, Kind::value, "Check for updates", std::string("Version ") + kAppVersion,
        "Look for a newer version on GitHub. If there is one you can install it from here; the app closes while it updates. Press Cross.");
    set(kReload, Kind::action, "Reload addons and catalogs", "", "Fetch every addon and catalog again.");
    set(kExit, Kind::action, "Exit Stremio", "", "Close the app and go back to the home screen.");
    changed_ |= kSettings;
}

void Session::settings_activate(int row)
{
    if (row != kAccount && confirm_sign_out_)
        confirm_sign_out_ = false;
    if (row != kTorbox)
        debrid_confirm_ = 0;
    switch (row)
    {
    case kAccount:
        if (!signed_in())
            login_start();
        else if (!confirm_sign_out_)
            confirm_sign_out_ = true;
        else
        {
            confirm_sign_out_ = false;
            sign_out();
        }
        break;
    case kProfile:
        profile_picker_req_ = true;
        break;
    case kTorrents:
        prefs_.builtin_torrents = !prefs_.builtin_torrents;
        save_prefs();
        cue(hui::audio::Cue::toggle);
        break;
    case kServer:
        ask_text([this](const std::string &v) {
            // The keyboard starts empty: typing nothing keeps the address.
            std::string u = trim(v);
            if (u.empty())
                return;
            if (u.find("://") == std::string::npos)
                u = "http://" + u;
            prefs_.server_url = u;
            save_prefs();
            cue(hui::audio::Cue::saved);
            refresh_settings();
        });
        break;
    case kTorbox:
        debrid_settings_press(2);
        return;
    case kSubLangs:
        ask_text([this](const std::string &v) {
            if (trim(v).empty())
                return;
            prefs_.subtitle_langs = trim(v);
            save_prefs();
            cue(hui::audio::Cue::saved);
            refresh_settings();
        });
        break;
    case kAudioLangs:
        ask_text([this](const std::string &v) {
            if (trim(v).empty())
                return;
            prefs_.audio_langs = trim(v) == "-" ? "" : trim(v);
            save_prefs();
            cue(hui::audio::Cue::saved);
            refresh_settings();
        });
        break;
    case kAddAddon:
        ask_text([this](const std::string &v) {
            const std::string url = normalize_addon_url(v);
            if (trim(v).empty() || trim(v) == "https://")
                return;
            say("Loading addon...");
            bg<std::string>(
                [url]() {
                    ::Addon a;
                    std::string err;
                    if (!fetch_addon(url, a, err))
                        return "Could not add the addon: " + err;
                    return std::string();
                },
                [this, url](std::string &err) {
                    if (!err.empty())
                    {
                        say(err, 6.0, true);
                        return;
                    }
                    if (std::find(prefs_.extra_addons.begin(), prefs_.extra_addons.end(), url) == prefs_.extra_addons.end())
                        prefs_.extra_addons.push_back(url);
                    save_prefs();
                    say("Addon added");
                    load_addons();
                    refresh_settings();
                });
        });
        break;
    case kAutoSubs:
        prefs_.auto_subtitles = !prefs_.auto_subtitles;
        save_prefs();
        cue(hui::audio::Cue::toggle);
        break;
    case kSubSize:
    {
        static const char *sizes[] = {"sub-s", "sub-m", "sub-l"};
        const int cur = prefs_.sub_size == "sub-s" ? 0 : prefs_.sub_size == "sub-l" ? 2 : 1;
        open_dropdown("Subtitle size", {"Small", "Medium", "Large"}, cur, [this](int i) {
            prefs_.sub_size = sizes[i];
            save_prefs();
            cue(hui::audio::Cue::saved);
            refresh_settings();
        });
        break;
    }
    case kAutoplay:
        prefs_.autoplay_next = !prefs_.autoplay_next;
        save_prefs();
        cue(hui::audio::Cue::toggle);
        break;
    case kUiSounds:
        prefs_.ui_sounds = !prefs_.ui_sounds;
        save_prefs();
        cue(hui::audio::Cue::toggle);
        break;
    case kUpdates:
        update_check(true);
        break;
    case kHdr:
        prefs_.hdr_output = !prefs_.hdr_output;
        save_prefs();
        cue(hui::audio::Cue::toggle);
        break;
    case kVrr:
        prefs_.match_refresh = !prefs_.match_refresh;
        save_prefs();
        cue(hui::audio::Cue::toggle);
        break;
    case kReload:
        addons_reload();
        break;
    case kExit:
        exit_ = true;
        break;
    default:
        break;
    }
    refresh_settings();
}

// ---------------------------------------------------------------------------
// Pop-ups

void Session::say(const std::string &message, double seconds, bool error)
{
    toast.message = message;
    toast.error = error;
    toast_open = true;
    cue(hui::audio::Cue::notify);
    toast_until_ = now_seconds() + seconds;
}

void Session::open_dropdown(const std::string &title, const std::vector<std::string> &options, int active,
                            std::function<void(int)> chosen)
{
    dropdown.title = title;
    dropdown.message.clear();
    dropdown.danger = -1;
    dropdown.dialog = false;
    dropdown.poster = 0;
    dropdown.progress = -1.0f;
    dropdown.options = options;
    dropdown.active = active;
    dropdown.selected = std::max(0, std::min(active, static_cast<int>(options.size()) - 1));
    dropdown_chosen_ = std::move(chosen);
    dropdown_open = true;
}

void Session::dropdown_confirm()
{
    dropdown_open = false;
    const auto fn = dropdown_chosen_;
    dropdown_chosen_ = nullptr;
    if (fn && dropdown.selected >= 0 && dropdown.selected < static_cast<int>(dropdown.options.size()))
        fn(dropdown.selected);
}

void Session::dropdown_cancel()
{
    dropdown_open = false;
    dropdown_chosen_ = nullptr;
}

// ---------------------------------------------------------------------------
// Signing in with a code approved on another device

void Session::login_start()
{
    const int gen = ++login_gen_;
    sign_in_open = true;
    sign_in = SignInState();
    sign_in.link = "link.stremio.com";
    sign_in.status = "Loading...";
    login_code_raw_.clear();
    login_qr_url_.clear();
    login_polling_ = false;
    login_failed_ = false;
    bg<LinkCode>([]() { return link_create(); },
                 [this, gen](LinkCode &lc) {
                     if (gen != login_gen_ || !sign_in_open)
                         return;
                     if (!lc.ok)
                     {
                         sign_in.status = "Could not get a code: " + lc.error;
                         sign_in.failed = true;
                         dlog("sign-in: could not get a code: %s", lc.error.c_str());
                         login_failed_ = true;
                         login_retry_at_ = now_seconds() + 5;
                         return;
                     }
                     login_code_raw_ = lc.code;
                     sign_in.code = lc.code;
                     std::string shown = lc.link;
                     if (starts_with(shown, "https://"))
                         shown = shown.substr(8);
                     sign_in.link = shown;
                     login_qr_url_ = "https://link.stremio.com/qr?data=" + url_encode(lc.link);
                     sign_in.status = "Waiting for you to approve the sign-in on your phone or computer...";
                     login_next_poll_ = now_seconds() + 3;
                     login_expires_ = now_seconds() + 10 * 60;
                 });
}

void Session::login_poll()
{
    if (dl_active_)
    {
        debrid_poll();
        return;
    }
    sign_in.qr = login_qr_url_.empty() ? 0 : tex_.get(login_qr_url_, ArtKind::qr);
    // The sign-in screen keeps a code up however long it waits: a new one when the code runs out or could not be got.
    if (sign_in_screen && (login_failed_ ? now_seconds() >= login_retry_at_
                                         : !login_code_raw_.empty() && now_seconds() > login_expires_))
    {
        login_start();
        return;
    }
    if (login_code_raw_.empty() || login_polling_ || now_seconds() < login_next_poll_)
        return;
    if (now_seconds() > login_expires_)
    {
        sign_in.status = "The sign-in code expired. Try again from Settings.";
        login_code_raw_.clear();
        return;
    }
    login_polling_ = true;
    const int gen = login_gen_;
    const std::string code = login_code_raw_;
    bg<ApiResult>([code]() { return link_read(code); },
                  [this, gen](ApiResult &r) {
                      login_polling_ = false;
                      login_next_poll_ = now_seconds() + 3;
                      if (gen != login_gen_ || !sign_in_open)
                          return;
                      if (r.ok)
                          login_done(jstr(r.result, "authKey"));
                  });
}

void Session::sign_in_cancel()
{
    ++login_gen_;
    dl_active_ = false;
    sign_in_open = false;
    sign_in_screen = false;
    login_code_raw_.clear();
}

bool Session::board_ready_or_wait()
{
    boot_again_ = true;
    if (boot_check(1.0)) // as if a second had passed: the time limits count from when waiting starts (below)
    {
        boot_again_ = false;
        return true;
    }
    boot_done_ = false;
    boot_start_ = now_seconds();
    dlog("boot: waiting again for the Board after the sign-in screen");
    return false;
}

bool Session::start_sign_in_screen()
{
    if (signed_in())
        return false;
    login_start();
    sign_in_screen = true;
    return true;
}

void Session::login_done(const std::string &auth_key)
{
    sign_in_cancel();
    prefs_.auth_key = auth_key;
    save_prefs();
    say("Signed in");
    bg<ApiResult>([auth_key]() { return api_get_user(auth_key); },
                  [this](ApiResult &r) {
                      if (!r.ok)
                          return;
                      prefs_.user_email = jstr(r.result, "email");
                      save_prefs();
                      refresh_settings();
                      say("Signed in as " + prefs_.user_email);
                  });
    library_.clear();
    library_loaded_ = false;
    load_addons();
    load_library();
    refresh_settings();
}

void Session::sign_out()
{
    const std::string key = prefs_.auth_key;
    g_tasks.run([key]() { api_logout(key); });
    prefs_.auth_key.clear();
    prefs_.user_email.clear();
    library_.clear();
    library_loaded_ = false;
    save_prefs();
    say("Signed out");
    load_addons();
    refresh_library();
    refresh_settings();
}

// ---------------------------------------------------------------------------
// Frame

void Session::update(float dt, const Cursor &cursor)
{
    (void)dt;
    g_tasks.drain();
    const double now = now_seconds();
    // While the first screen is being got ready, more pictures go up to the GPU each frame.
    const bool arrived = tex_.pump(boot_done_ ? 3 : 12);

    if (now - clock_ > 1.0)
    {
        clock_ = now;
        clock = format_clock();
    }
    if (!boot_done_ && boot_check(now - boot_start_))
    {
        boot_done_ = true;
        boot_again_ = false;
        dlog("boot: ready after %.1f s", now - boot_start_);
    }
    if (toast_open && now > toast_until_)
        toast_open = false;
    // Played from Continue Watching on the Board: the title's page was open behind it only to play.
    // When the video is over (and no next episode is lined up), it closes.
    if (resumed_from_board_ && !watching && !resuming_ && !autoplay_pending_)
    {
        resumed_from_board_ = false;
        close_detail();
    }
    if (sign_in_open)
        login_poll();
    if (boot_done_ && !update_checked_)
    {
        update_checked_ = true;
        update_check(false); // once per start, silent unless a version is waiting
    }
    update_poll();
    poll_typing();
    if (watching)
    {
        watch_update();
        upload_video();
    }

    // More of Discover when the cursor nears the end.
    constexpr int kDiscoverCols = 4;
    if (!disc_end_ && !disc_loading_ && !disc_items_.empty() &&
        cursor.discover + 2 * kDiscoverCols >= static_cast<int>(disc_items_.size()))
        discover_load(true);

    const bool moved = cursor.board_row != last_cursor_.board_row || cursor.board_col != last_cursor_.board_col ||
                       cursor.discover != last_cursor_.discover || cursor.library != last_cursor_.library ||
                       cursor.episode != last_cursor_.episode || cursor.search_row != last_cursor_.search_row ||
                       cursor.search_col != last_cursor_.search_col;
    last_cursor_ = cursor;
    if (arrived || moved || art_dirty_ || now - last_art_ > 1.0)
    {
        art_dirty_ = false;
        last_art_ = now;
        refresh_art(cursor);
    }
    if (tex_.trim(kTextureBudget))
        refresh_art(cursor);
}

// The app is ready to be shown when the Board has its rows and the first screen's pictures are on the
// GPU: the cards in the first two rows and the picture behind the first title. It does not wait for
// ever for what does not come: a catalog that never answers, a picture that cannot be fetched.
bool Session::boot_check(double t) const
{
    if (t > 10.0)
        return true;
    // The mark has its moment. After signing in the wait lasts at least 2.5 s: the account's own catalogs come, and
    // their first pictures (and the wallpapers behind them) need time to arrive before Home is shown.
    if (t < (boot_again_ ? (signed_in() ? 2.5 : 0.6) : 1.8))
        return false;
    // The sign-in screen comes first and does not wait for the Board: it needs its code, and the wall fills in as the
    // posters arrive. (The Board is got ready behind it.)
    if (sign_in_screen && !boot_again_)
        return sign_in.qr != 0 || login_failed_;
    // Waiting again after signing in: the account's own catalogs (and its library) must have answered, not the ones the
    // sign-in screen's wall came from.
    const bool answered = !addons_loading_ && !board_loading_;
    // What is on the Board until the account's addons have answered is the previous list (the sign-in screen's own):
    // it must not count as ready. After that it is as at the first start: three rows are enough (a dead addon's catalog
    // must not hold the start up for its timeout), and the library, up to 5 s.
    if (boot_again_ && (addons_loading_ || (signed_in() && !library_loaded_ && t <= 5.0)))
        return false;
    const std::size_t rows = board.rows.size();
    if (rows == 0)
        return answered; // nothing to wait for: the page tells what is wrong
    if (!answered && rows < 3)
        return false;
    if (t > 7.0)
        return true; // pictures that have not come by now come when they come
    for (std::size_t r = 0; r < rows && r < 2; ++r)
    {
        const std::vector<Item> &items = board_[static_cast<std::size_t>(board_map_[r])].items;
        for (std::size_t c = 0; c < items.size() && c < board.rows[r].items.size() && c < 8; ++c)
            if (!items[c].poster.empty() && board.rows[r].items[c].poster == 0)
                return false;
    }
    // The wallpapers behind the first row's first cards must be in (on disk, ready to go on the GPU) too: a move
    // along the row then shows each at once. Four of the first five that have one (the rest keep coming).
    {
        const std::vector<Item> &first_row = board_[static_cast<std::size_t>(board_map_[0])].items;
        int wanted = 0, have = 0;
        for (std::size_t c = 0; c < first_row.size() && c < 5; ++c)
            if (!first_row[c].background.empty())
            {
                ++wanted;
                have += tex_.cached(first_row[c].background, ArtKind::backdrop) ? 1 : 0;
            }
        if (have < std::min(wanted, 4))
            return false;
    }
    const Item &first = board_[static_cast<std::size_t>(board_map_[0])].items.front();
    if (!first.background.empty())
        return board.rows[0].items.front().backdrop != 0;
    // No picture yet: the addons are being asked for it.
    return !(meta_asked_.count(first.type + ":" + first.id) && !meta_cache_.count(first.type + ":" + first.id));
}

// ---------------------------------------------------------------------------
// Artwork: the textures for what is near the cursor; the rest only if they are
// already on the GPU.

void Session::refresh_art(const Cursor &cursor)
{
    const auto fetch = [&](const std::string &url, ArtKind kind, bool near) {
        return near ? tex_.get(url, kind) : tex_.peek(url, kind);
    };

    // Board: cards around the cursor, and the hero's picture for the focused title and its neighbours.
    for (std::size_t r = 0; r < board.rows.size() && r < board_map_.size(); ++r)
    {
        const std::vector<Item> &items = board_[static_cast<std::size_t>(board_map_[r])].items;
        const bool row_near = std::abs(static_cast<int>(r) - cursor.board_row) <= 2;
        for (std::size_t c = 0; c < board.rows[r].items.size() && c < items.size(); ++c)
        {
            Title &t = board.rows[r].items[c];
            const int col = static_cast<int>(c);
            const bool poster_near = row_near && col < cursor.board_col + 12;
            t.poster = fetch(items[c].poster, ArtKind::poster, poster_near);
            const bool hero = static_cast<int>(r) == cursor.board_row && std::abs(col - cursor.board_col) <= 1;
            // On the GPU before the cursor gets there (two cards along, and the cards above and below), so a move
            // finds its wallpaper ready; about 15 MB each.
            const int row_gap = std::abs(static_cast<int>(r) - cursor.board_row);
            const bool resident = hero || (row_gap == 0 && std::abs(col - cursor.board_col) <= 2) ||
                                  (row_gap == 1 && col == cursor.board_col);
            t.backdrop = fetch(items[c].background, ArtKind::backdrop, resident);
            // Ready on disk ahead of time (behind everything on screen): the wallpapers of the cards around the cursor,
            // so moving to a card shows its picture at once, and while the boot animation plays, the first rows'
            // wallpapers and the first posters of every row.
            const int dr = std::abs(static_cast<int>(r) - cursor.board_row), dc = col - cursor.board_col;
            // (also while the sign-in screen is up: Continue without account then finds the Board ready)
            const bool starting = !boot_done_ || sign_in_screen;
            if (!hero && ((dr == 0 && dc >= -3 && dc <= 8) || (dr == 1 && dc >= -1 && dc <= 5) || (starting && r < 3 && c < 8)))
                tex_.warm(items[c].background, ArtKind::backdrop);
            if (!poster_near && starting && c < 10)
                tex_.warm(items[c].poster, ArtKind::poster);
            // The library lists a title without its backdrop or story: ask the addons.
            if (hero && (items[c].background.empty() || items[c].description.empty()))
                want_meta(items[c]);
        }
    }

    // The sign-in screen's wall: the first posters of the Board's first rows, each once, in copies made at the wall's
    // size. The list only grows (rows arrive in any order), so a tile never moves once it is there.
    wall.clear();
    if (sign_in_screen)
    {
        constexpr std::size_t kWallPosters = 36; // the wall's tiles (ui/overlays.cpp)
        for (std::size_t r = 0; r < board.rows.size() && r < board_map_.size() && wall_urls_.size() < kWallPosters; ++r)
        {
            const std::vector<Item> &items = board_[static_cast<std::size_t>(board_map_[r])].items;
            for (std::size_t c = 0; c < items.size() && c < 10 && wall_urls_.size() < kWallPosters; ++c)
            {
                const std::string &u = items[c].poster;
                if (!u.empty() && std::find(wall_urls_.begin(), wall_urls_.end(), u) == wall_urls_.end())
                    wall_urls_.push_back(u);
            }
        }
        for (const std::string &u : wall_urls_)
            wall.push_back(tex_.get(u, ArtKind::wall));
    }

    // Search results: the same, without a hero.
    for (std::size_t r = 0; r < search.rows.size() && r < search_map_.size(); ++r)
    {
        const std::vector<Item> &items = search_[static_cast<std::size_t>(search_map_[r])].items;
        const bool row_near = std::abs(static_cast<int>(r) - cursor.search_row) <= 2;
        for (std::size_t c = 0; c < search.rows[r].items.size() && c < items.size(); ++c)
            search.rows[r].items[c].poster =
                fetch(items[c].poster, ArtKind::poster, row_near && static_cast<int>(c) < cursor.search_col + 12);
    }

    // Starting a stream: the title's picture behind the ring and in it.
    if (launch_open)
    {
        launch.backdrop = fetch(w_item_.background, ArtKind::backdrop, true);
        launch.poster = fetch(w_item_.poster, ArtKind::poster, true);
    }

    // Discover: posters in a window, and the preview picture of the focused title.
    for (std::size_t i = 0; i < discover.items.size() && i < disc_items_.size(); ++i)
    {
        Title &t = discover.items[i];
        const int d = static_cast<int>(i) - cursor.discover;
        t.poster = fetch(disc_items_[i].poster, ArtKind::poster, d > -12 && d < 24);
        t.backdrop = fetch(disc_items_[i].background, ArtKind::backdrop, d == 0);
        if (d != 0 && d >= -2 && d <= 6)
            tex_.warm(disc_items_[i].background, ArtKind::backdrop); // the next previews, ready on disk

    }

    // Library
    for (std::size_t i = 0; i < library.items.size() && i < lib_items_.size(); ++i)
    {
        const int d = static_cast<int>(i) - cursor.library;
        library.items[i].poster = fetch(lib_items_[i].poster, ArtKind::poster, d > -14 && d < 28);
    }

    // Addons
    for (std::size_t i = 0; i < addons.items.size() && i < addon_logos_.size(); ++i)
        // Only when the Addons page is open: 26 logos from dead hosts held every download slot for 10 s each (they are
        // fetched at the same priority as the pictures of the page on show).
        addons.items[i].logo = fetch(addon_logos_[i], ArtKind::icon, page_ == Page::addons);

    // A title's page
    if (d_open_)
    {
        const std::string &background = d_meta_loaded_ && !d_meta_.background.empty() ? d_meta_.background : d_item_.background;
        detail.title.backdrop = fetch(background, ArtKind::backdrop, true);
        for (std::size_t i = 0; i < detail.episodes.size() && i < d_season_videos_.size(); ++i)
            detail.episodes[i].thumb = fetch(d_season_videos_[i]->thumbnail, ArtKind::thumb,
                                             std::abs(static_cast<int>(i) - cursor.episode) < 10);
    }
}

} // namespace sx
