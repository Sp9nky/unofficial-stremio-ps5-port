// The Board, Discover and the Library.

#include "session.hpp"
#include "stream_json.hpp"

#include <algorithm>
#include <cstdlib>

namespace sx
{

namespace
{

constexpr std::size_t kMaxRowCards = 50;

std::string row_title(const Catalog &c)
{
    const std::string type = capitalize(c.type);
    return c.name.empty() ? type : c.name + " - " + type;
}

// "tt0903747:2:5" -> "S2 E5"
std::string episode_badge(const std::string &video_id)
{
    const auto parts = split(video_id, ':');
    if (parts.size() >= 3)
    {
        const int s = std::atoi(parts[parts.size() - 2].c_str()), e = std::atoi(parts.back().c_str());
        if (s > 0 || e > 0)
            return "S" + std::to_string(s) + " E" + std::to_string(e);
    }
    return "";
}

std::vector<Item> parse_catalog(const json &j)
{
    std::vector<Item> items;
    const json &metas = jobj(j, "metas");
    if (!metas.is_array())
        return items;
    for (const auto &m : metas)
    {
        Item it = item_from_meta_json(m);
        if (!it.id.empty() && !it.name.empty())
            items.push_back(it);
    }
    return items;
}

std::string catalog_label(const ::Addon *a, const Catalog *c)
{
    return c->name.empty() ? a->name : c->name;
}

} // namespace

Title Session::make_title(const Item &it, bool with_progress) const
{
    Title t;
    t.id = it.id;
    t.type = it.type;
    t.name = it.name;
    t.year = it.release_info;
    t.runtime = it.runtime;
    t.genres = join(it.genres, ", ");
    t.imdb = it.imdb_rating;
    t.synopsis = it.description;
    if (with_progress)
    {
        if (it.type == "series")
            t.badge = episode_badge(it.video_id);
        if (it.duration > 0 && it.offset > 0)
            t.progress = static_cast<float>(std::max(0.02, std::min(1.0, it.offset / it.duration)));
        t.watched = it.times_watched > 0 && it.offset <= 0;
    }
    return t;
}

// ---------------------------------------------------------------------------
// Details the library does not carry

void Session::apply_meta(Item &it) const
{
    const auto m = meta_cache_.find(it.type + ":" + it.id);
    if (m == meta_cache_.end())
        return;
    const Item &c = m->second;
    if (it.background.empty())
        it.background = c.background;
    if (it.logo.empty())
        it.logo = c.logo;
    if (it.poster.empty())
        it.poster = c.poster;
    if (it.description.empty())
        it.description = c.description;
    if (it.release_info.empty())
        it.release_info = c.release_info;
    if (it.runtime.empty())
        it.runtime = c.runtime;
    if (it.imdb_rating.empty())
        it.imdb_rating = c.imdb_rating;
    if (it.genres.empty())
        it.genres = c.genres;
}

void Session::want_meta(const Item &it)
{
    const std::string key = it.type + ":" + it.id;
    if (meta_cache_.count(key) || meta_asked_.count(key))
        return;
    std::vector<std::string> urls;
    for (const auto &a : addons_)
        if (a->supports("meta", it.type, it.id))
            urls.push_back(a->resource_url("meta", it.type, it.id));
    if (urls.empty())
        return;
    meta_asked_.insert(key);
    struct Res
    {
        bool ok = false;
        ::Meta meta;
    };
    bg<Res>(
        [urls]() {
            Res r;
            for (const auto &u : urls)
            {
                json j;
                std::string err;
                if (!fetch_json(u, j, err))
                    continue;
                const json &m = jobj(j, "meta");
                if (!m.is_object())
                    continue;
                r.meta = parse_meta(m);
                r.ok = true;
                break;
            }
            return r;
        },
        [this, key](Res &r) {
            if (!r.ok)
                return;
            Item c;
            c.background = r.meta.background;
            c.logo = r.meta.logo;
            c.poster = r.meta.poster;
            c.description = r.meta.description;
            c.release_info = r.meta.release_info;
            c.runtime = r.meta.runtime;
            c.imdb_rating = r.meta.imdb_rating;
            c.genres = r.meta.genres;
            meta_cache_[key] = c;
            refresh_board();
            refresh_library();
        });
}

// ---------------------------------------------------------------------------
// Continue watching

std::vector<Item> Session::continue_watching() const
{
    std::vector<Item> out;
    if (signed_in() && library_loaded_)
    {
        for (const auto &kv : library_)
        {
            const json &li = kv.second;
            const json &st = jobj(li, "state");
            const double offset = jnum(st, "timeOffset") / 1000.0;
            const bool removed = jbool(li, "removed"), temp = jbool(li, "temp");
            if (offset <= 0 || (removed && !temp))
                continue;
            Item it;
            it.id = kv.first;
            it.type = jstr(li, "type");
            it.name = jstr(li, "name");
            it.poster = jstr(li, "poster");
            it.background = jstr(li, "background");
            it.logo = jstr(li, "logo");
            it.video_id = jstr(st, "video_id");
            it.offset = offset;
            it.duration = jnum(st, "duration") / 1000.0;
            it.last_watched = iso8601_to_ms(jstr(st, "lastWatched"));
            apply_meta(it);
            out.push_back(it);
        }
    }
    else
    {
        for (const auto &kv : progress_)
        {
            if (kv.second.time <= 0)
                continue;
            Item it;
            it.id = kv.first;
            it.type = kv.second.type;
            it.name = kv.second.name;
            it.poster = kv.second.poster;
            it.video_id = kv.second.video_id;
            it.offset = kv.second.time;
            it.duration = kv.second.duration;
            it.last_watched = kv.second.updated;
            apply_meta(it);
            out.push_back(it);
        }
    }
    // The episode after one that was just finished: nothing of it is watched yet, so there is no
    // timeOffset to find, only what was remembered here.
    for (const auto &kv : progress_)
    {
        if (kv.second.up_next.empty())
            continue;
        bool listed = false;
        for (const Item &o : out)
            listed = listed || o.id == kv.first;
        if (listed)
            continue;
        Item it;
        it.id = kv.first;
        it.type = kv.second.type;
        it.name = kv.second.name;
        it.poster = kv.second.poster;
        const auto li = library_.find(kv.first);
        if (li != library_.end())
        {
            it.poster = jstr(li->second, "poster");
            it.background = jstr(li->second, "background");
            it.logo = jstr(li->second, "logo");
        }
        it.video_id = kv.second.up_next;
        it.up_next = true;
        it.last_watched = kv.second.updated;
        apply_meta(it);
        out.push_back(it);
    }
    std::sort(out.begin(), out.end(), [](const Item &a, const Item &b) { return a.last_watched > b.last_watched; });
    if (out.size() > kMaxRowCards)
        out.resize(kMaxRowCards);
    return out;
}

// ---------------------------------------------------------------------------
// Board

void Session::build_board()
{
    const int gen = ++home_gen_;
    board_.clear();
    BoardRow cw;
    cw.key = "continue";
    cw.title = "Continue Watching";
    cw.continue_watching = true;
    cw.loaded = true;
    board_.push_back(cw);

    for (const auto &a : addons_)
        for (const auto &c : a->catalogs)
        {
            if (c.requires_other_than(""))
                continue; // needs a search or a genre
            BoardRow row;
            row.key = a->transport_url + "|" + c.type + "|" + c.id;
            row.title = row_title(c);
            row.addon_url = a->transport_url;
            row.catalog = c;
            board_.push_back(row);
        }

    for (std::size_t i = 1; i < board_.size(); ++i)
    {
        const ::Addon *a = find_addon(board_[i].addon_url);
        if (!a)
            continue;
        const std::string url = a->resource_url("catalog", board_[i].catalog.type, board_[i].catalog.id);
        struct Res
        {
            std::vector<Item> items;
            std::string error;
        };
        bg<Res>(
            [url]() {
                Res r;
                json j;
                if (fetch_json(url, j, r.error))
                    r.items = parse_catalog(j);
                return r;
            },
            [this, gen, i](Res &r) {
                if (gen != home_gen_ || i >= board_.size())
                    return;
                board_[i].loaded = true;
                board_[i].items = std::move(r.items);
                if (board_[i].items.size() > kMaxRowCards)
                    board_[i].items.resize(kMaxRowCards);
                if (!r.error.empty())
                    dlog("catalog %s: %s", board_[i].title.c_str(), r.error.c_str());
                refresh_board();
            });
    }
    refresh_board();
}

void Session::refresh_board()
{
    if (!board_.empty())
        board_[0].items = continue_watching();
    board.rows.clear();
    board_map_.clear();
    bool any_loading = false;
    for (std::size_t i = 0; i < board_.size(); ++i)
    {
        const BoardRow &br = board_[i];
        if (!br.loaded)
        {
            any_loading = true;
            continue;
        }
        if (br.items.empty())
            continue;
        Row row;
        row.key = br.key;
        row.title = br.title;
        for (const Item &it : br.items)
            row.items.push_back(make_title(it, br.continue_watching));
        board.rows.push_back(std::move(row));
        board_map_.push_back(static_cast<int>(i));
    }
    board_loading_ = any_loading;
    if (!board.rows.empty())
        board.status.clear();
    else if (addons_loading_ || any_loading)
        board.status = "Loading your catalogs...";
    else if (addons_.empty())
        board.status = "No addon could be reached. Check the network connection, then reload the addons in Settings.";
    else
        board.status = "None of your addons offer catalogs.";
    changed_ |= kBoard;
    art_dirty_ = true;
}

void Session::continue_remove(const Item &it)
{
    const auto p = progress_.find(it.id);
    if (p != progress_.end())
    {
        p->second.time = 0;
        p->second.up_next.clear();
        save_progress();
    }
    const auto li = library_.find(it.id);
    if (signed_in() && li != library_.end())
    {
        json &item = li->second;
        item["state"]["timeOffset"] = 0;
        item["_mtime"] = iso8601_now();
        const std::string key = prefs_.auth_key;
        const json changes = json::array({item});
        bg<ApiResult>([key, changes]() { return api_library_put(key, changes); },
                      [this](ApiResult &r) {
                          if (!r.ok)
                              say("Could not update your library: " + r.error, 5.0, true);
                      });
    }
    say("Removed from Continue Watching");
    refresh_board();
    refresh_library();
}

bool Session::board_options(int row, int col)
{
    if (row < 0 || row >= static_cast<int>(board_map_.size()))
        return false;
    const BoardRow &br = board_[static_cast<std::size_t>(board_map_[static_cast<std::size_t>(row)])];
    if (!br.continue_watching || col < 0 || col >= static_cast<int>(br.items.size()))
        return false;
    const Item it = br.items[static_cast<std::size_t>(col)];
    // The poster and how far it is watched, in the dialog.
    std::uint32_t poster = 0;
    float progress = -1.0f;
    if (row < static_cast<int>(board.rows.size()) && col < static_cast<int>(board.rows[static_cast<std::size_t>(row)].items.size()))
    {
        const Title &t = board.rows[static_cast<std::size_t>(row)].items[static_cast<std::size_t>(col)];
        poster = t.poster;
        progress = t.progress;
    }
    const std::string where = it.type == "series" ? episode_badge(it.video_id) : std::string();
    open_dropdown(it.name, {"Remove from Continue Watching", "Cancel"}, -1, [this, it, poster, progress, where](int i) {
        if (i != 0)
            return;
        // Taking something away asks once more, and the cursor starts on the safe answer.
        open_dropdown(it.name, {"Remove", "Keep"}, -1, [this, it](int j) {
            if (j == 0)
                continue_remove(it);
        });
        dropdown.message = "Remove it from Continue Watching?";
        dropdown.danger = 0;
        dropdown.selected = 1;
        dropdown.dialog = true;
        dropdown.poster = poster;
        dropdown.progress = progress;
    });
    dropdown.message = where;
    dropdown.selected = 0;
    dropdown.dialog = true;
    dropdown.poster = poster;
    dropdown.progress = progress;
    return true;
}

bool Session::board_is_continue(int row) const
{
    return row >= 0 && row < static_cast<int>(board_map_.size()) &&
           board_[static_cast<std::size_t>(board_map_[static_cast<std::size_t>(row)])].continue_watching;
}

bool Session::saved_stream(const std::string &item_id, const std::string &video_id, ::Stream *out) const
{
    const auto p = progress_.find(item_id);
    if (p == progress_.end() || p->second.stream_json.empty())
        return false;
    // The stream is for one episode: it is no use for another.
    if (!video_id.empty() && p->second.stream_video_id != video_id)
        return false;
    try
    {
        const ::Stream s = stream_from_json(json::parse(p->second.stream_json));
        if (!s.playable())
            return false;
        *out = s;
        return true;
    }
    catch (...)
    {
        return false;
    }
}

void Session::continue_press(int row, int col)
{
    if (!board_is_continue(row))
        return;
    const BoardRow &br = board_[static_cast<std::size_t>(board_map_[static_cast<std::size_t>(row)])];
    if (col < 0 || col >= static_cast<int>(br.items.size()) || row >= static_cast<int>(board.rows.size()) ||
        col >= static_cast<int>(board.rows[static_cast<std::size_t>(row)].items.size()))
        return;
    const Item it = br.items[static_cast<std::size_t>(col)];
    const Title title = board.rows[static_cast<std::size_t>(row)].items[static_cast<std::size_t>(col)];
    ::Stream saved;
    // Half watched, with the stream that was playing remembered: ask what to do. Otherwise (the next
    // episode of one that was finished, or nothing remembered) the streams are shown at once.
    const bool can_play_now =
        !it.up_next && it.offset > 0 && saved_stream(it.id, it.type == "series" ? it.video_id : std::string(), &saved);
    if (!can_play_now)
    {
        open_req_ = std::make_pair(title, it.video_id);
        return;
    }
    const bool series = it.type == "series";
    std::vector<std::string> options{"Play now"};
    if (series)
        options.push_back("Choose another episode");
    options.push_back("Choose another stream");
    open_dropdown(it.name, options, -1, [this, title, it, series](int i) {
        if (i == 0)
            resume_play(title, it);
        else if (series && i == 1)
            open_req_ = std::make_pair(title, std::string()); // the episodes
        else
            open_req_ = std::make_pair(title, it.video_id); // the streams
    });
    std::string where = series ? episode_badge(it.video_id) : std::string();
    if (it.duration > 0)
    {
        if (!where.empty())
            where += "  \xC2\xB7  ";
        where += std::to_string(static_cast<int>(std::min(1.0, it.offset / it.duration) * 100.0 + 0.5)) + "% watched";
    }
    dropdown.message = where;
    dropdown.selected = 0;
    dropdown.active = -1;
    dropdown.dialog = true;
    dropdown.poster = title.poster;
    dropdown.progress = title.progress;
}

void Session::resume_play(const Title &title, const Item &it)
{
    if (watching)
        return;
    open_title(title); // loads the details (the film's streams too: they are not used)
    resume_play_ = true;
    resumed_from_board_ = true;
    resume_video_ = it.video_id.empty() ? it.id : it.video_id;
    // The launch screen is up at once, while the details come in.
    w_item_ = it;
    launch = LaunchState();
    launch.title = it.name;
    launch.subtitle = it.type == "series" ? episode_badge(it.video_id) : std::string();
    launch.stages = {"Finding sources", "Connecting to peers", "Filling the buffer", "Starting"};
    launch.stage = 0;
    launch.progress = -1.0f;
    launch_open = true;
    watching = true;
    resuming_ = true;
    playing = PlayerState();
    playing.title = it.name;
    playing.subtitle = launch.subtitle;
    playing.controls = true;
    art_dirty_ = true;
}

std::string Session::next_episode_id() const
{
    if (!d_open_ || !d_meta_loaded_ || !detail.series || w_item_.id != d_item_.id)
        return std::string();
    const auto &vids = d_meta_.videos;
    std::size_t cur = vids.size();
    for (std::size_t i = 0; i < vids.size(); ++i)
        if (vids[i].id == w_video_id_)
            cur = i;
    if (cur + 1 >= vids.size())
        return std::string();
    if (vids[cur + 1].season == 0 && vids[cur].season != 0)
        return std::string(); // do not roll into the specials
    return vids[cur + 1].id;
}
void Session::board_square(int row, int col)
{
    (void)col;
    if (row < 0 || row >= static_cast<int>(board_map_.size()))
        return;
    const int bi = board_map_[static_cast<std::size_t>(row)];
    BoardRow &br = board_[static_cast<std::size_t>(bi)];
    if (br.continue_watching)
        return; // everything in it can be reached by scrolling: it has no "See all"    // See all: Discover opens on this catalog.
    see_all_addon_ = br.addon_url;
    see_all_catalog_ = br.catalog.id;
    see_all_type_ = br.catalog.type;
    disc_items_.clear();
    discover.items.clear();
    changed_ |= kDiscover | kDiscoverNew;
    goto_ = Page::discover;
}

// ---------------------------------------------------------------------------
// Discover

void Session::page_opened(Page page)
{
    page_ = page;
    if (page == Page::discover)
        enter_discover();
    else if (page == Page::library && signed_in() && !library_loaded_)
        load_library();
}

void Session::enter_discover()
{
    // Types offered by catalogs that can be browsed without a search.
    disc_types_.clear();
    for (const auto &a : addons_)
        for (const auto &c : a->catalogs)
        {
            if (c.requires_other_than("genre"))
                continue;
            if (std::find(disc_types_.begin(), disc_types_.end(), c.type) == disc_types_.end())
                disc_types_.push_back(c.type);
        }
    const auto rank = [](const std::string &t) { return t == "movie" ? 0 : t == "series" ? 1 : 2; };
    std::stable_sort(disc_types_.begin(), disc_types_.end(),
                     [&](const std::string &a, const std::string &b) { return rank(a) < rank(b); });
    if (disc_types_.empty())
    {
        discover.filters.clear();
        discover.items.clear();
        disc_items_.clear();
        discover.status = addons_loading_ ? "Addons are still loading..." : "None of your addons have catalogs to browse.";
        changed_ |= kDiscover;
        return;
    }
    bool restart = false;
    if (!see_all_type_.empty())
    {
        disc_type_ = see_all_type_;
        disc_genre_.clear();
        disc_catalog_ = 0;
        restart = true;
    }
    else if (std::find(disc_types_.begin(), disc_types_.end(), disc_type_) == disc_types_.end())
    {
        disc_type_ = disc_types_[0];
        disc_catalog_ = 0;
        disc_genre_.clear();
        restart = true;
    }
    discover_build_chips();
    if (!see_all_type_.empty())
    {
        for (std::size_t i = 0; i < disc_catalogs_.size(); ++i)
            if (disc_catalogs_[i].addon->transport_url == see_all_addon_ && disc_catalogs_[i].catalog->id == see_all_catalog_)
                disc_catalog_ = static_cast<int>(i);
        discover_build_chips();
        see_all_type_.clear();
    }
    if (restart || disc_items_.empty())
        discover_load(false);
    else
        changed_ |= kDiscover;
}

void Session::discover_build_chips()
{
    disc_catalogs_.clear();
    for (const auto &a : addons_)
        for (const auto &c : a->catalogs)
            if (c.type == disc_type_ && !c.requires_other_than("genre"))
                disc_catalogs_.push_back({a.get(), &c});
    if (disc_catalog_ >= static_cast<int>(disc_catalogs_.size()))
        disc_catalog_ = 0;

    discover.filters.clear();
    discover.filters.push_back({"Type", capitalize(disc_type_)});
    if (!disc_catalogs_.empty())
    {
        const DiscCatalog &dc = disc_catalogs_[static_cast<std::size_t>(disc_catalog_)];
        discover.filters.push_back({"Catalog", catalog_label(dc.addon, dc.catalog)});
        const CatalogExtra *g = dc.catalog->extra("genre");
        if (g && !g->options.empty())
        {
            if (g->required && disc_genre_.empty())
                disc_genre_ = g->options[0];
            discover.filters.push_back({"Genre", disc_genre_.empty() ? "All genres" : disc_genre_});
        }
        else
        {
            disc_genre_.clear();
        }
    }
    changed_ |= kDiscover;
}

void Session::discover_load(bool more)
{
    if (disc_catalogs_.empty())
        return;
    if (!more)
    {
        ++disc_gen_;
        disc_items_.clear();
        discover.items.clear();
        disc_end_ = false;
        changed_ |= kDiscover | kDiscoverNew;
    }
    if (disc_loading_ && more)
        return;
    const DiscCatalog &dc = disc_catalogs_[static_cast<std::size_t>(disc_catalog_)];
    std::vector<std::string> extra;
    if (!disc_genre_.empty())
        extra.push_back("genre=" + url_encode(disc_genre_));
    if (more && !disc_items_.empty())
        extra.push_back("skip=" + std::to_string(disc_items_.size()));
    const std::string url = dc.addon->resource_url("catalog", dc.catalog->type, dc.catalog->id, join(extra, "&"));
    const int gen = disc_gen_;
    disc_loading_ = true;
    if (disc_items_.empty())
        discover.status = "Loading...";
    struct Res
    {
        std::vector<Item> items;
        std::string error;
    };
    bg<Res>(
        [url]() {
            Res r;
            json j;
            if (fetch_json(url, j, r.error))
                r.items = parse_catalog(j);
            return r;
        },
        [this, gen](Res &r) {
            if (gen != disc_gen_)
                return;
            disc_loading_ = false;
            // Skip duplicates (some addons ignore skip and send page one again).
            std::size_t added = 0;
            for (const Item &it : r.items)
            {
                const bool dup = std::any_of(disc_items_.begin(), disc_items_.end(), [&](const Item &e) { return e.id == it.id; });
                if (dup)
                    continue;
                disc_items_.push_back(it);
                discover.items.push_back(make_title(it, false));
                ++added;
            }
            if (added == 0)
                disc_end_ = true;
            if (!r.error.empty() && disc_items_.empty())
                discover.status = "Could not load this catalog (" + r.error + ")";
            else if (disc_items_.empty())
                discover.status = "Nothing here.";
            else
                discover.status.clear();
            changed_ |= kDiscover;
            art_dirty_ = true;
        });
}

// ---------------------------------------------------------------------------
// The two filter rows

void Session::filter_activate(Page page, int filter)
{
    if (page == Page::discover)
    {
        if (filter == 0)
        {
            std::vector<std::string> opts;
            int active = 0;
            for (std::size_t i = 0; i < disc_types_.size(); ++i)
            {
                opts.push_back(capitalize(disc_types_[i]));
                if (disc_types_[i] == disc_type_)
                    active = static_cast<int>(i);
            }
            open_dropdown("Type", opts, active, [this](int i) {
                if (disc_types_[static_cast<std::size_t>(i)] == disc_type_)
                    return;
                disc_type_ = disc_types_[static_cast<std::size_t>(i)];
                disc_catalog_ = 0;
                disc_genre_.clear();
                discover_build_chips();
                discover_load(false);
            });
        }
        else if (filter == 1)
        {
            std::vector<std::string> opts;
            for (const auto &dc : disc_catalogs_)
            {
                std::string label = catalog_label(dc.addon, dc.catalog);
                if (label != dc.addon->name)
                    label += " (" + dc.addon->name + ")";
                opts.push_back(label);
            }
            open_dropdown("Catalog", opts, disc_catalog_, [this](int i) {
                if (i == disc_catalog_)
                    return;
                disc_catalog_ = i;
                disc_genre_.clear();
                discover_build_chips();
                discover_load(false);
            });
        }
        else if (filter == 2 && !disc_catalogs_.empty())
        {
            const CatalogExtra *g = disc_catalogs_[static_cast<std::size_t>(disc_catalog_)].catalog->extra("genre");
            if (!g)
                return;
            std::vector<std::string> opts;
            int active = 0;
            if (!g->required)
                opts.push_back("All genres");
            for (const auto &o : g->options)
            {
                if (o == disc_genre_)
                    active = static_cast<int>(opts.size());
                opts.push_back(o);
            }
            const bool required = g->required;
            open_dropdown("Genre", opts, active, [this, opts, required](int i) {
                const std::string genre = (!required && i == 0) ? "" : opts[static_cast<std::size_t>(i)];
                if (genre == disc_genre_)
                    return;
                disc_genre_ = genre;
                discover_build_chips();
                discover_load(false);
            });
        }
    }
    else if (page == Page::library)
    {
        if (filter == 0)
        {
            std::vector<std::string> types;
            for (const auto &kv : library_)
            {
                const std::string t = jstr(kv.second, "type");
                if (!t.empty() && std::find(types.begin(), types.end(), t) == types.end())
                    types.push_back(t);
            }
            for (const auto &kv : progress_)
                if (!kv.second.type.empty() && std::find(types.begin(), types.end(), kv.second.type) == types.end())
                    types.push_back(kv.second.type);
            std::sort(types.begin(), types.end());
            std::vector<std::string> opts = {"All types"};
            int active = 0;
            for (const auto &t : types)
            {
                if (t == lib_type_)
                    active = static_cast<int>(opts.size());
                opts.push_back(capitalize(t));
            }
            open_dropdown("Type", opts, active, [this, types](int i) {
                lib_type_ = i == 0 ? "" : types[static_cast<std::size_t>(i - 1)];
                refresh_library();
                changed_ |= kLibraryNew;
            });
        }
        else
        {
            open_dropdown("Sort by", {"Last watched", "A-Z", "Most watched"}, lib_sort_, [this](int i) {
                lib_sort_ = i;
                refresh_library();
                changed_ |= kLibraryNew;
            });
        }
    }
}

// ---------------------------------------------------------------------------
// Library

void Session::refresh_library()
{
    std::vector<Item> all;
    if (signed_in())
    {
        for (const auto &kv : library_)
        {
            const json &li = kv.second;
            if (jbool(li, "removed") || jbool(li, "temp"))
                continue;
            const json &st = jobj(li, "state");
            Item it;
            it.id = kv.first;
            it.type = jstr(li, "type");
            it.name = jstr(li, "name");
            it.poster = jstr(li, "poster");
            it.background = jstr(li, "background");
            it.logo = jstr(li, "logo");
            it.video_id = jstr(st, "video_id");
            it.offset = jnum(st, "timeOffset") / 1000.0;
            it.duration = jnum(st, "duration") / 1000.0;
            it.times_watched = static_cast<int>(jnum(st, "timesWatched"));
            it.last_watched = iso8601_to_ms(jstr(st, "lastWatched"));
            if (!it.last_watched)
                it.last_watched = iso8601_to_ms(jstr(li, "_mtime"));
            apply_meta(it);
            all.push_back(it);
        }
    }
    else
    {
        for (const auto &kv : progress_)
        {
            Item it;
            it.id = kv.first;
            it.type = kv.second.type;
            it.name = kv.second.name;
            it.poster = kv.second.poster;
            it.video_id = kv.second.video_id;
            it.offset = kv.second.time;
            it.duration = kv.second.duration;
            it.last_watched = kv.second.updated;
            all.push_back(it);
        }
    }

    std::vector<std::string> types;
    for (const Item &it : all)
        if (!it.type.empty() && std::find(types.begin(), types.end(), it.type) == types.end())
            types.push_back(it.type);
    if (!lib_type_.empty() && std::find(types.begin(), types.end(), lib_type_) == types.end())
        lib_type_.clear();

    static const char *sorts[] = {"Last watched", "A-Z", "Most watched"};
    library.filters.clear();
    library.filters.push_back({"Show", lib_type_.empty() ? "All types" : capitalize(lib_type_)});
    library.filters.push_back({"Sort", sorts[lib_sort_]});

    lib_items_.clear();
    for (const Item &it : all)
        if (lib_type_.empty() || it.type == lib_type_)
            lib_items_.push_back(it);
    if (lib_sort_ == 0)
        std::sort(lib_items_.begin(), lib_items_.end(), [](const Item &a, const Item &b) { return a.last_watched > b.last_watched; });
    else if (lib_sort_ == 1)
        std::sort(lib_items_.begin(), lib_items_.end(), [](const Item &a, const Item &b) { return lower(a.name) < lower(b.name); });
    else
        std::sort(lib_items_.begin(), lib_items_.end(), [](const Item &a, const Item &b) { return a.times_watched > b.times_watched; });

    library.items.clear();
    for (const Item &it : lib_items_)
        library.items.push_back(make_title(it, true));
    const std::size_t n = library.items.size();
    library.count = std::to_string(n) + (n == 1 ? " title" : " titles");

    if (n > 0)
        library.status.clear();
    else if (signed_in() && !library_loaded_)
        library.status = "Loading your library...";
    else if (!signed_in() && all.empty())
        library.status = "Sign in to your Stremio account in Settings to see your library here.";
    else
        library.status = "Nothing in your library matches this filter.";
    changed_ |= kLibrary;
    art_dirty_ = true;
}

} // namespace sx
