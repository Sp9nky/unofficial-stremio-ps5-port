// A title's page: its details, the seasons and episodes of a series, and the
// streams the addons offer for a film or an episode.

#include "session.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace sx
{

namespace
{

// ---- what an addon puts in a stream's name and description -------------------

// Torrentio and others decorate descriptions with emoji and symbols the fonts do
// not have; they carry the numbers (seeds, size) and are read before being dropped.
std::string plain(const std::string &s)
{
    std::string out;
    for (std::size_t i = 0; i < s.size();)
    {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        if (i + len > s.size())
            break;
        const bool emoji = len == 4 || (len == 3 && c == 0xE2 && (static_cast<unsigned char>(s[i + 1]) == 0x9A || static_cast<unsigned char>(s[i + 1]) == 0x9C)) ||
                           (len == 3 && c == 0xEF && static_cast<unsigned char>(s[i + 1]) == 0xB8);
        if (!emoji)
            out.append(s, i, len);
        i += len;
    }
    return trim(out);
}

std::string first_line(const std::string &s)
{
    const std::size_t nl = s.find('\n');
    return plain(nl == std::string::npos ? s : s.substr(0, nl));
}

const char *const kResolutionOrder[] = {"4K", "1440p", "1080p", "720p", "SD", "Other"};

std::string resolution_of(const ::Stream &st)
{
    const std::string s = lower(st.name + " " + st.description + " " + st.filename);
    const auto has = [&](const char *w) { return s.find(w) != std::string::npos; };
    if (has("2160p") || has("4k") || has("uhd"))
        return "4K";
    if (has("1440p") || has("2k"))
        return "1440p";
    if (has("1080p") || has("1080i") || has("fhd"))
        return "1080p";
    if (has("720p"))
        return "720p";
    if (has("576p") || has("480p") || has("360p") || has("dvdrip") || has("dvdscr"))
        return "SD";
    return "Other";
}

int resolution_rank(const std::string &r)
{
    for (int i = 0; i < 6; ++i)
        if (r == kResolutionOrder[i])
            return i;
    return 5;
}

// "👤 142" in a description.
int seeds_of(const std::string &description)
{
    const std::size_t p = description.find("\xF0\x9F\x91\xA4");
    if (p == std::string::npos)
        return -1;
    return std::atoi(description.c_str() + p + 4);
}

// "💾 18.4 GB" in a description, else any number followed by a size unit.
std::string size_of(const std::string &description)
{
    std::size_t p = description.find("\xF0\x9F\x92\xBE");
    if (p != std::string::npos)
    {
        p += 4;
        while (p < description.size() && description[p] == ' ')
            ++p;
        std::string size;
        while (p < description.size() && description[p] != '\n' && description[p] != ' ' && static_cast<unsigned char>(description[p]) < 0x80)
            size += description[p++];
        if (p < description.size() && description[p] == ' ')
        {
            ++p;
            std::string unit;
            while (p < description.size() && std::isalpha(static_cast<unsigned char>(description[p])))
                unit += description[p++];
            if (!unit.empty())
                size += " " + unit;
        }
        return size;
    }
    const std::string l = lower(description);
    for (const char *unit : {" gb", " mb", " tb", " gib", " mib"})
    {
        const std::size_t u = l.find(unit);
        if (u == std::string::npos || u == 0)
            continue;
        std::size_t s = u;
        while (s > 0 && (std::isdigit(static_cast<unsigned char>(l[s - 1])) || l[s - 1] == '.' || l[s - 1] == ','))
            --s;
        if (s < u)
        {
            std::string out = description.substr(s, u - s) + " ";
            std::string up = std::string(unit + 1);
            for (char &c : up)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return out + up;
        }
    }
    return "";
}

std::string tags_of(const ::Stream &st)
{
    const std::string s = lower(st.name + " " + st.description + " " + st.filename);
    static const struct
    {
        const char *needle;
        const char *label;
    } kTags[] = {{"dolby vision", "Dolby Vision"}, {" dv", "Dolby Vision"}, {"hdr10+", "HDR10+"}, {"hdr10", "HDR10"}, {"hdr", "HDR"},
                 {"atmos", "Dolby Atmos"},          {"truehd", "TrueHD"},     {"dts-hd", "DTS-HD"}, {"dts", "DTS"},
                 {"7.1", "7.1"},                    {"5.1", "5.1"},           {"remux", "Remux"},   {"bluray", "BluRay"},
                 {"web-dl", "WEB-DL"},              {"webrip", "WEBRip"},     {"hevc", "HEVC"},     {"x265", "HEVC"},
                 {"h265", "HEVC"}};
    std::vector<std::string> out;
    for (const auto &t : kTags)
        if (s.find(t.needle) != std::string::npos && std::find(out.begin(), out.end(), t.label) == out.end())
            out.push_back(t.label);
    if (out.size() > 4)
        out.resize(4);
    return join(out, "  \xC2\xB7  ");
}

Stream view_of(const ::Stream &st)
{
    Stream v;
    v.resolution = resolution_of(st);
    v.name = st.addon.empty() ? first_line(st.name) : st.addon;
    // The release name: the file's own, else the first line of the description.
    v.detail = !st.filename.empty() ? st.filename : first_line(st.description);
    if (v.detail.empty())
        v.detail = first_line(st.name);
    v.tags = tags_of(st);
    v.size = size_of(st.description);
    const int seeds = seeds_of(st.description);
    if (seeds >= 0)
        v.seeds = std::to_string(seeds) + (seeds == 1 ? " seed" : " seeds");
    else if (!st.info_hash.empty())
        v.seeds = "torrent";
    return v;
}

// "2010-09-14T00:00:00.000Z" -> "Sep 14, 2010"
std::string date_label(const std::string &released)
{
    if (released.size() < 10 || released[4] != '-' || released[7] != '-')
        return released;
    static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const int m = std::atoi(released.substr(5, 2).c_str());
    if (m < 1 || m > 12)
        return released.substr(0, 10);
    return std::string(months[m - 1]) + " " + std::to_string(std::atoi(released.substr(8, 2).c_str())) + ", " +
           released.substr(0, 4);
}

std::string episode_title(const ::Video &v)
{
    return v.title.empty() ? "Episode " + std::to_string(v.episode) : v.title;
}

} // namespace

Stream stream_view(const ::Stream &st)
{
    return view_of(st);
}

bool Session::watched(const std::string &video_id) const
{
    return watched_.count(video_id) > 0;
}

void Session::detail_toggle_watched(int index)
{
    if (index < 0 || index >= static_cast<int>(d_season_videos_.size()))
        return;
    const std::string &id = d_season_videos_[static_cast<std::size_t>(index)]->id;
    if (watched_.count(id))
    {
        watched_.erase(id);
        say("Marked as not watched");
    }
    else
    {
        watched_.insert(id);
        say("Marked as watched");
    }
    save_progress();
    detail.episodes[static_cast<std::size_t>(index)].watched = watched(id);
    changed_ |= kDetail;
}

// ---------------------------------------------------------------------------

void Session::open_title(const Title &t, const std::string &streams_video)
{
    const Item *found = nullptr;
    const auto look = [&](const std::vector<Item> &items) {
        if (found)
            return;
        for (const Item &it : items)
            if (it.id == t.id && (t.type.empty() || it.type == t.type))
            {
                found = &it;
                return;
            }
    };
    for (const BoardRow &br : board_)
        look(br.items);
    for (const BoardRow &br : search_)
        look(br.items);
    look(disc_items_);
    look(lib_items_);
    Item it;
    if (found)
    {
        it = *found;
    }
    else
    {
        it.id = t.id;
        it.type = t.type;
        it.name = t.name;
    }

    d_item_ = it;
    d_meta_ = ::Meta();
    d_meta_loaded_ = false;
    d_season_videos_.clear();
    d_seasons_.clear();
    d_streams_.clear();
    d_found_.clear();
    d_video_id_.clear();
    ++d_gen_;
    ++d_meta_gen_;
    d_open_ = true;
    want_streams_video_ = streams_video;
    resume_play_ = false;

    detail = DetailContent();
    detail_fill_title();
    detail.series = it.type == "series";
    detail.episodes_status = "Loading episodes...";
    detail.streams_status = "";
    changed_ |= kDetail;
    art_dirty_ = true;

    // The details come from the first addon that has them.
    std::vector<std::string> urls;
    for (const auto &a : addons_)
        if (a->supports("meta", it.type, it.id))
            urls.push_back(a->resource_url("meta", it.type, it.id));
    const int gen = d_meta_gen_;
    struct Res
    {
        bool ok = false;
        ::Meta meta;
        std::string error;
    };
    bg<Res>(
        [urls]() {
            Res r;
            for (const auto &u : urls)
            {
                json j;
                std::string err;
                if (!fetch_json(u, j, err))
                {
                    r.error = err;
                    continue;
                }
                const json &m = jobj(j, "meta");
                if (!m.is_object())
                    continue;
                r.meta = parse_meta(m);
                r.ok = true;
                break;
            }
            if (urls.empty())
                r.error = "no addon has details for this";
            return r;
        },
        [this, gen](Res &r) {
            if (gen != d_meta_gen_ || !d_open_)
                return;
            if (!r.ok)
            {
                detail.episodes_status = "Could not load the details: " + r.error;
                // Without details it can still be played as one video.
                detail.series = false;
                detail_load_streams(d_item_.id, "Streams");
                changed_ |= kDetail;
                if (resume_play_)
                    start_remembered_stream();
                return;
            }
            d_meta_ = r.meta;
            if (d_meta_.id.empty())
                d_meta_.id = d_item_.id;
            if (d_meta_.type.empty())
                d_meta_.type = d_item_.type;
            d_meta_loaded_ = true;
            detail_apply_meta();
            if (resume_play_)
                start_remembered_stream();
        });

    if (!detail.series)
        detail_load_streams(it.id, "Streams"); // a film: its streams right away
}

// Continue Watching, "Play now": the details are in, so the stream that was playing is played again.
void Session::start_remembered_stream()
{
    resume_play_ = false;
    ::Stream s;
    // A series' stream belongs to one episode; a film has one video, whatever it is called.
    const bool series = d_item_.type == "series";
    if (saved_stream(d_item_.id, series ? resume_video_ : std::string(), &s))
    {
        const auto p = progress_.find(d_item_.id);
        d_video_id_ = series || p == progress_.end() || p->second.stream_video_id.empty() ? resume_video_
                                                                                          : p->second.stream_video_id;
        play_stream(s, false);
        return;
    }
    // Nothing usable remembered after all: leave the launch screen, the page shows the streams.
    resuming_ = false;
    resumed_from_board_ = false;
    watch_stop(false);
}
void Session::close_detail()
{
    ++d_gen_;
    ++d_meta_gen_;
    d_open_ = false;
}

void Session::detail_fill_title()
{
    Title &t = detail.title;
    const ::Meta &m = d_meta_;
    t.id = d_item_.id;
    t.type = d_item_.type;
    t.name = d_meta_loaded_ && !m.name.empty() ? m.name : d_item_.name;
    t.year = d_meta_loaded_ ? m.release_info : d_item_.release_info;
    t.runtime = d_meta_loaded_ ? m.runtime : d_item_.runtime;
    t.genres = join(d_meta_loaded_ ? m.genres : d_item_.genres, ", ");
    t.imdb = d_meta_loaded_ ? m.imdb_rating : d_item_.imdb_rating;
    t.synopsis = d_meta_loaded_ ? m.description : d_item_.description;
    if (d_meta_loaded_)
    {
        std::vector<std::string> cast = m.cast;
        if (cast.size() > 6)
            cast.resize(6);
        detail.cast = join(cast, ", ");
        detail.directors = join(m.directors, ", ");
    }
}

void Session::detail_apply_meta()
{
    const ::Meta &m = d_meta_;
    if (d_item_.poster.empty())
        d_item_.poster = m.poster;
    if (d_item_.background.empty())
        d_item_.background = m.background;
    detail_fill_title();

    // Series-like when there are several videos (series, channels, anime).
    bool series = m.type != "movie" && m.videos.size() > 1;
    if (m.type == "series" && !m.videos.empty())
        series = true;
    if (series)
    {
        detail.series = true;
        d_seasons_.clear();
        for (const auto &v : m.videos)
            if (std::find(d_seasons_.begin(), d_seasons_.end(), v.season) == d_seasons_.end())
                d_seasons_.push_back(v.season);
        // Specials (season 0) last.
        std::stable_sort(d_seasons_.begin(), d_seasons_.end(), [](int a, int b) {
            if (a == 0)
                return false;
            if (b == 0)
                return true;
            return a < b;
        });
        detail.seasons.clear();
        for (int s : d_seasons_)
            detail.seasons.push_back(s == 0 ? "Specials" : "Season " + std::to_string(s));
        // Start on the season being watched, else the first.
        int season_idx = 0;
        std::string resume_video;
        for (const Item &cw : continue_watching())
            if (cw.id == d_item_.id)
                resume_video = cw.video_id;
        if (resume_video.empty() && progress_.count(d_item_.id))
            resume_video = progress_[d_item_.id].video_id;
        int want_episode = -1;
        for (const auto &v : m.videos)
            if (v.id == resume_video)
            {
                for (std::size_t i = 0; i < d_seasons_.size(); ++i)
                    if (d_seasons_[i] == v.season)
                        season_idx = static_cast<int>(i);
                want_episode = v.episode;
            }
        detail_set_season(season_idx);
        if (want_episode >= 0)
            for (std::size_t i = 0; i < d_season_videos_.size(); ++i)
                if (d_season_videos_[i]->episode == want_episode)
                {
                    detail.episode_hint = static_cast<int>(i);
                    ++detail.hint_serial;
                }
        // Asked for from Continue Watching: that episode's streams, without the list of episodes.
        if (!want_streams_video_.empty() && !resume_play_)
        {
            for (const auto &v : m.videos)
                if (v.id == want_streams_video_)
                {
                    std::string heading = "S" + std::to_string(v.season) + " E" + std::to_string(v.episode);
                    if (!v.title.empty())
                        heading += "  \xC2\xB7  " + v.title;
                    detail_load_streams(v.id, heading);
                    ++detail.streams_serial;
                    break;
                }
        }
        want_streams_video_.clear();
    }
    else
    {
        const bool was_series = detail.series;
        detail.series = false;
        detail.episodes.clear();
        const std::string vid = m.videos.size() == 1 ? m.videos[0].id : m.id;
        if (was_series || vid != d_video_id_)
            detail_load_streams(vid, "Streams");
    }
    changed_ |= kDetail;
    art_dirty_ = true;
}

void Session::detail_set_season(int index)
{
    if (d_seasons_.empty())
        return;
    index = std::max(0, std::min(index, static_cast<int>(d_seasons_.size()) - 1));
    d_season_idx_ = index;
    detail.season = index;
    const int season = d_seasons_[static_cast<std::size_t>(index)];
    d_season_videos_.clear();
    for (const auto &v : d_meta_.videos)
        if (v.season == season)
            d_season_videos_.push_back(&v);
    // What was left half-watched, shown on its episode.
    std::string resume_video;
    float resume_progress = -1.0f;
    for (const Item &cw : continue_watching())
        if (cw.id == d_item_.id && cw.duration > 0)
        {
            resume_video = cw.video_id;
            resume_progress = static_cast<float>(std::min(1.0, cw.offset / cw.duration));
        }
    detail.episodes.clear();
    int first_unwatched = -1;
    for (std::size_t i = 0; i < d_season_videos_.size(); ++i)
    {
        const ::Video *v = d_season_videos_[i];
        Episode e;
        e.number = v->episode;
        e.title = episode_title(*v);
        e.info = date_label(v->released);
        e.watched = watched(v->id);
        if (v->id == resume_video)
            e.progress = resume_progress;
        detail.episodes.push_back(e);
        if (!e.watched && first_unwatched < 0)
            first_unwatched = static_cast<int>(i);
    }
    detail.episode_hint = first_unwatched >= 0 ? first_unwatched : 0;
    ++detail.hint_serial;
    detail.episodes_status = detail.episodes.empty() ? "No episodes in this season." : "";
    changed_ |= kDetail;
    art_dirty_ = true;
}

void Session::detail_change_season(int delta)
{
    if (d_seasons_.empty())
        return;
    const int next = d_season_idx_ + delta;
    if (next < 0 || next >= static_cast<int>(d_seasons_.size()))
        return;
    detail_set_season(next);
}

void Session::detail_pick_episode(int index)
{
    if (index < 0 || index >= static_cast<int>(d_season_videos_.size()))
        return;
    const ::Video *v = d_season_videos_[static_cast<std::size_t>(index)];
    std::string heading = "S" + std::to_string(v->season) + " E" + std::to_string(v->episode);
    if (!v->title.empty())
        heading += "  \xC2\xB7  " + v->title;
    detail_load_streams(v->id, heading);
}

void Session::detail_load_streams(const std::string &video_id, const std::string &heading)
{
    const int gen = ++d_gen_;
    d_video_id_ = video_id;
    d_stream_heading_ = heading;
    d_found_.clear();
    d_streams_.clear();
    detail.streams.clear();
    detail.stream_heading = heading;
    d_answered_ = 0;

    std::string type = d_meta_loaded_ ? d_meta_.type : d_item_.type;
    if (type.empty())
        type = d_item_.type;
    std::vector<std::pair<std::string, std::string>> sources; // addon name, url
    for (const auto &a : addons_)
        if (a->supports("stream", type, video_id))
            sources.push_back({a->name, a->resource_url("stream", type, video_id)});
    d_pending_ = static_cast<int>(sources.size());
    if (sources.empty())
    {
        detail.streams_status = addons_loading_ ? "Addons are still loading." :
                                                  "None of your addons provide streams for this. Install a stream addon "
                                                  "(for example Torrentio) in your Stremio account.";
        changed_ |= kDetail;
        return;
    }
    detail.streams_status = "Loading streams... (0 of " + std::to_string(sources.size()) + " addons answered)";
    for (const auto &s : sources)
    {
        const std::string name = s.first, url = s.second;
        struct Res
        {
            std::vector<::Stream> streams;
            std::string error;
        };
        bg<Res>(
            [name, url]() {
                Res r;
                json j;
                if (fetch_json(url, j, r.error, 30))
                    r.streams = parse_streams(j, name);
                return r;
            },
            [this, gen, name](Res &r) {
                if (gen != d_gen_)
                    return;
                ++d_answered_;
                if (!r.error.empty())
                    dlog("streams from %s: %s", name.c_str(), r.error.c_str());
                for (auto &st : r.streams)
                    d_found_.push_back(st);
                detail_refresh_streams();
            });
    }
    changed_ |= kDetail;
}

void Session::detail_refresh_streams()
{
    // Best picture first, then the better seeded.
    std::vector<std::size_t> order(d_found_.size());
    for (std::size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        const int ra = resolution_rank(resolution_of(d_found_[a])), rb = resolution_rank(resolution_of(d_found_[b]));
        if (ra != rb)
            return ra < rb;
        return seeds_of(d_found_[a].description) > seeds_of(d_found_[b].description);
    });
    d_streams_.clear();
    detail.streams.clear();
    for (std::size_t i : order)
    {
        d_streams_.push_back(d_found_[i]);
        detail.streams.push_back(view_of(d_found_[i]));
    }
    // The next episode: a stream like the last one once the streams are in
    // (same binge group, else the same addon, else the first).
    if (autoplay_pending_)
    {
        int pick = -1;
        for (std::size_t i = 0; i < d_streams_.size() && pick < 0; ++i)
            if (!autoplay_binge_.empty() && d_streams_[i].binge_group == autoplay_binge_)
                pick = static_cast<int>(i);
        const bool all = d_answered_ >= d_pending_;
        if (pick < 0 && all)
            for (std::size_t i = 0; i < d_streams_.size() && pick < 0; ++i)
                if (d_streams_[i].addon == autoplay_addon_)
                    pick = static_cast<int>(i);
        if (pick < 0 && all && !d_streams_.empty())
            pick = 0;
        if (pick >= 0)
        {
            autoplay_pending_ = false;
            play_stream(d_streams_[static_cast<std::size_t>(pick)], false);
        }
        else if (all)
        {
            autoplay_pending_ = false;
        }
    }
    if (d_answered_ < d_pending_)
        detail.streams_status = "Loading streams... (" + std::to_string(d_answered_) + " of " + std::to_string(d_pending_) +
                                " addons answered)";
    else if (detail.streams.empty())
        detail.streams_status = "No streams found. Install a stream addon (for example Torrentio) in your Stremio account.";
    else
        detail.streams_status.clear();
    changed_ |= kDetail;
}

} // namespace sx
