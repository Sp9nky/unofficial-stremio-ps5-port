// Detail page: metadata on the left, seasons/episodes or streams on the right.

#include <algorithm>
#include <cstdlib>

#include "app.h"
#include "http.h"
#include "torrent/engine.h"
#include "torrent/torrent_stream.h"

static std::string random_id() {
	static const char* hex = "0123456789abcdef";
	std::string s;
	uint64_t x = uint64_t(now_epoch_ms()) * 6364136223846793005ULL + uint64_t(rand());
	for (int i = 0; i < 16; i++) {
		s += hex[x & 15];
		x = x * 6364136223846793005ULL + 1442695040888963407ULL;
	}
	return s;
}

std::string App::episode_label(const Video& v) const {
	std::string t = v.title.empty() ? "Episode " + std::to_string(v.episode) : v.title;
	if (v.episode > 0) return std::to_string(v.episode) + ". " + t;
	return t;
}

bool App::watched(const std::string& video_id) const { return watched_.count(video_id) > 0; }

void App::toggle_watched(const std::string& video_id) {
	if (watched_.count(video_id)) {
		watched_.erase(video_id);
		show_toast("Marked as not watched");
	} else {
		watched_.insert(video_id);
		show_toast("Marked as watched");
	}
	save_progress();
}

// ---------------------------------------------------------------------------

void App::detail_open(const Item& it) {
	g_art.clear_queue();
	d_item_ = it;
	d_meta_ = Meta();
	d_meta_loaded_ = false;
	d_gen_++;
	d_meta_gen_++;
	d_video_id_.clear();
	view = "detail";

	d_name = it.name;
	d_runtime = it.runtime;
	d_year = it.release_info;
	d_imdb = it.imdb_rating;
	d_genres = join(it.genres, ", ");
	d_cast.clear();
	d_directors.clear();
	d_description = it.description;
	d_background = art(it.background, ArtKind::Backdrop);
	d_logo = art(it.logo, ArtKind::Logo);
	d_series = it.type == "series";
	d_zone = d_series ? "episodes" : "streams";
	d_episodes.clear();
	d_season_videos_.clear();
	d_seasons_.clear();
	d_episode_sel = 0;
	d_season_label.clear();
	d_streams.clear();
	d_stream_list_.clear();
	d_stream_sel = 0;
	d_streams_title = "Streams";
	d_streams_status.clear();
	detail_update_resume();

	// Metadata from the first addon that has it.
	std::vector<std::string> urls;
	for (auto& a : addons_)
		if (a->supports("meta", it.type, it.id)) urls.push_back(a->resource_url("meta", it.type, it.id));
	int gen = d_meta_gen_;
	struct Res {
		bool ok = false;
		Meta meta;
		std::string error;
	};
	bg<Res>(
	    [urls]() {
		    Res r;
		    for (auto& u : urls) {
			    json j;
			    std::string err;
			    if (!fetch_json(u, j, err)) {
				    r.error = err;
				    continue;
			    }
			    const json& m = jobj(j, "meta");
			    if (!m.is_object()) continue;
			    r.meta = parse_meta(m);
			    r.ok = true;
			    break;
		    }
		    if (urls.empty()) r.error = "no addon has details for this";
		    return r;
	    },
	    [this, gen](Res& r) {
		    if (gen != d_meta_gen_ || view != "detail") return;
		    if (!r.ok) {
			    if (d_series) d_streams_status = "Could not load details: " + r.error;
			    else show_toast("Could not load details: " + r.error);
			    if (d_series) {
				    d_series = false;
				    d_zone = "streams";
				    detail_load_streams(d_item_.id);
			    }
			    dirty_all();
			    return;
		    }
		    d_meta_ = r.meta;
		    if (d_meta_.id.empty()) d_meta_.id = d_item_.id;
		    if (d_meta_.type.empty()) d_meta_.type = d_item_.type;
		    d_meta_loaded_ = true;
		    detail_apply_meta();
	    });

	if (!d_series) {
		// Movies: streams right away, for the item id.
		detail_load_streams(it.id);
	}
	dirty_all();
}

void App::detail_apply_meta() {
	const Meta& m = d_meta_;
	if (!m.name.empty()) d_name = m.name;
	d_runtime = m.runtime;
	d_year = m.release_info;
	d_imdb = m.imdb_rating;
	d_genres = join(m.genres, ", ");
	std::vector<std::string> cast = m.cast;
	if (cast.size() > 6) cast.resize(6);
	d_cast = join(cast, ", ");
	d_directors = join(m.directors, ", ");
	d_description = m.description;
	if (d_item_.poster.empty()) d_item_.poster = m.poster;
	if (d_item_.background.empty()) d_item_.background = m.background;
	if (d_item_.logo.empty()) d_item_.logo = m.logo;
	if (d_item_.name.empty()) d_item_.name = m.name;
	d_background = art(m.background.empty() ? d_item_.background : m.background, ArtKind::Backdrop);
	d_logo = art(m.logo.empty() ? d_item_.logo : m.logo, ArtKind::Logo);

	// Series-like when there are several videos (series, channels, anime).
	bool series = m.type != "movie" && m.videos.size() > 1;
	if (m.type == "series" && !m.videos.empty()) series = true;
	if (series) {
		d_series = true;
		d_seasons_.clear();
		for (auto& v : m.videos)
			if (std::find(d_seasons_.begin(), d_seasons_.end(), v.season) == d_seasons_.end()) d_seasons_.push_back(v.season);
		// Specials (season 0) last.
		std::stable_sort(d_seasons_.begin(), d_seasons_.end(), [](int a, int b) {
			if (a == 0) return false;
			if (b == 0) return true;
			return a < b;
		});
		// Start on the season being watched, else the first.
		int season_idx = 0;
		std::string resume_vid;
		for (auto& cw : continue_watching())
			if (cw.id == d_item_.id) resume_vid = cw.video_id;
		if (resume_vid.empty() && progress_.count(d_item_.id)) resume_vid = progress_[d_item_.id].video_id;
		int want_episode = -1;
		for (auto& v : m.videos)
			if (v.id == resume_vid) {
				for (size_t i = 0; i < d_seasons_.size(); i++)
					if (d_seasons_[i] == v.season) season_idx = int(i);
				want_episode = v.episode;
			}
		if (d_zone != "streams") d_zone = "episodes";
		detail_set_season(season_idx);
		if (want_episode >= 0)
			for (size_t i = 0; i < d_season_videos_.size(); i++)
				if (d_season_videos_[i]->episode == want_episode) d_episode_sel = int(i);
	} else {
		bool was_series = d_series;
		d_series = false;
		d_zone = "streams";
		std::string vid = m.videos.size() == 1 ? m.videos[0].id : m.id;
		if (was_series || vid != d_video_id_) detail_load_streams(vid);
	}
	detail_update_resume();
	refresh_images();
	dirty_all();
}

void App::detail_set_season(int idx) {
	if (d_seasons_.empty()) return;
	idx = std::max(0, std::min(idx, int(d_seasons_.size()) - 1));
	d_season_idx_ = idx;
	int season = d_seasons_[idx];
	d_season_label = season == 0 ? "Specials" : "Season " + std::to_string(season);
	d_season_videos_.clear();
	for (auto& v : d_meta_.videos)
		if (v.season == season) d_season_videos_.push_back(&v);
	d_episodes.clear();
	int first_unwatched = -1;
	for (size_t i = 0; i < d_season_videos_.size(); i++) {
		const Video* v = d_season_videos_[i];
		UiEpisode e;
		e.title = episode_label(*v);
		std::string released = v->released.size() >= 10 ? v->released.substr(0, 10) : v->released;
		e.sub = released;
		e.watched = watched(v->id);
		e.thumb = art(v->thumbnail, ArtKind::Thumb, i < 10);
		if (!e.watched && first_unwatched < 0) first_unwatched = int(i);
		d_episodes.push_back(e);
	}
	d_episode_sel = first_unwatched >= 0 ? first_unwatched : 0;
	dirty_all();
}

void App::detail_update_resume() {
	d_resume.clear();
	for (auto& cw : continue_watching()) {
		if (cw.id != d_item_.id || cw.offset <= 0) continue;
		if (cw.type == "series" || split(cw.video_id, ':').size() >= 3) {
			auto parts = split(cw.video_id, ':');
			if (parts.size() >= 3)
				d_resume = "Resume S" + parts[parts.size() - 2] + "E" + parts.back() + " at " + format_time(cw.offset);
			else d_resume = "Resume at " + format_time(cw.offset);
		} else {
			d_resume = "Resume at " + format_time(cw.offset);
		}
	}
}

void App::detail_load_streams(const std::string& video_id) {
	int gen = ++d_gen_;
	d_video_id_ = video_id;
	d_stream_list_.clear();
	d_streams.clear();
	d_stream_sel = 0;
	d_answered_ = 0;
	d_pick_res = true;
	d_res_.clear();
	d_res_sel_ = 0;

	std::string type = d_meta_loaded_ ? d_meta_.type : d_item_.type;
	if (type.empty()) type = d_item_.type;
	d_streams_base_title_ = "Streams";
	if (d_series) {
		for (auto* v : d_season_videos_)
			if (v->id == video_id)
				d_streams_base_title_ = "S" + std::to_string(v->season) + "E" + std::to_string(v->episode) +
				                        (v->title.empty() ? "" : " · " + v->title);
	}
	d_streams_title = d_streams_base_title_;

	std::vector<std::pair<std::string, std::string>> sources;  // addon name, url
	for (auto& a : addons_)
		if (a->supports("stream", type, video_id)) sources.push_back({a->name, a->resource_url("stream", type, video_id)});
	d_pending_ = int(sources.size());
	if (sources.empty()) {
		autoplay_pending_ = false;
		d_streams_status = addons_loading_
		                       ? "Addons are still loading"
		                       : "None of your addons provide streams for this. Install a stream addon (for example "
		                         "Torrentio) in your Stremio account, or add it under Settings.";
		dirty_all();
		return;
	}
	d_streams_status = "Loading streams... (0 of " + std::to_string(sources.size()) + " addons answered)";
	for (auto& s : sources) {
		std::string name = s.first, url = s.second;
		struct Res {
			std::vector<Stream> streams;
			std::string error;
		};
		bg<Res>(
		    [name, url]() {
			    Res r;
			    json j;
			    if (fetch_json(url, j, r.error, 30)) r.streams = parse_streams(j, name);
			    return r;
		    },
		    [this, gen, name](Res& r) {
			    if (gen != d_gen_) return;
			    d_answered_++;
			    if (!r.error.empty()) dlog("streams from %s: %s", name.c_str(), r.error.c_str());
			    for (auto& st : r.streams) d_stream_list_.push_back(st);
			    detail_refresh_streams();
		    });
	}
	dirty_all();
}

// A stream's resolution, from what addons put in its name and title
// (Torrentio: "Torrentio\n4k DV | HDR", "...2160p...", "1080p", ...).
static const char* const kResOrder[] = {"4K", "1440p", "1080p", "720p", "SD", "Other"};

static std::string stream_resolution(const Stream& st) {
	std::string s = lower(st.name + " " + st.description + " " + st.filename);
	auto has = [&](const char* w) { return s.find(w) != std::string::npos; };
	if (has("2160p") || has("4k") || has("uhd")) return "4K";
	if (has("1440p") || has("2k")) return "1440p";
	if (has("1080p") || has("1080i") || has("fhd")) return "1080p";
	if (has("720p")) return "720p";
	if (has("576p") || has("480p") || has("360p") || has("dvdrip") || has("dvdscr")) return "SD";
	return "Other";
}

void App::detail_refresh_streams() {
	d_streams.clear();
	// Group by resolution, in kResOrder order.
	std::map<std::string, std::vector<int>> groups;
	for (size_t i = 0; i < d_stream_list_.size(); i++) groups[stream_resolution(d_stream_list_[i])].push_back(int(i));

	if (d_pick_res) {
		d_res_keys_.clear();
		d_view_.clear();
		for (const char* key : kResOrder) {
			auto g = groups.find(key);
			if (g == groups.end()) continue;
			std::vector<std::string> addons;
			for (int i : g->second) {
				const std::string& a = d_stream_list_[i].addon;
				if (std::find(addons.begin(), addons.end(), a) == addons.end()) addons.push_back(a);
			}
			UiStream u;
			u.name = key;
			u.addon = std::to_string(g->second.size()) + (g->second.size() == 1 ? " stream" : " streams");
			u.desc = "From " + join(addons, ", ");
			d_streams.push_back(u);
			d_res_keys_.push_back(key);
		}
		d_streams_title = d_streams_base_title_ + (d_streams.empty() ? "" : " · Choose a quality");
	} else {
		d_view_ = groups[d_res_];
		for (int i : d_view_) {
			const Stream& st = d_stream_list_[i];
			UiStream u;
			u.name = st.name;
			u.addon = st.addon;
			u.desc = st.description.empty() ? st.filename : st.description;
			d_streams.push_back(u);
		}
		d_streams_title = d_streams_base_title_ + " · " + d_res_;
	}
	if (d_stream_sel >= int(d_streams.size())) d_stream_sel = std::max(0, int(d_streams.size()) - 1);

	if (autoplay_pending_) {
		// Next episode: same binge group, else same addon; first stream once all answered.
		int pick = -1;
		for (size_t i = 0; i < d_stream_list_.size() && pick < 0; i++)
			if (!autoplay_binge_.empty() && d_stream_list_[i].binge_group == autoplay_binge_) pick = int(i);
		bool all = d_answered_ >= d_pending_;
		if (pick < 0 && all)
			for (size_t i = 0; i < d_stream_list_.size() && pick < 0; i++)
				if (d_stream_list_[i].addon == autoplay_addon_) pick = int(i);
		if (pick < 0 && all && !d_stream_list_.empty()) pick = 0;
		if (pick >= 0) {
			autoplay_pending_ = false;
			d_stream_sel = pick;
			play_stream(d_stream_list_[pick], false);
		} else if (all) {
			autoplay_pending_ = false;
		}
	}

	if (d_answered_ < d_pending_)
		d_streams_status = "Loading streams... (" + std::to_string(d_answered_) + " of " + std::to_string(d_pending_) +
		                   " addons answered)";
	else if (d_streams.empty())
		d_streams_status = "No streams found. Install a stream addon (for example Torrentio) in your Stremio account, "
		                   "or add it under Settings.";
	else d_streams_status = "";
	dirty_all();
}

void App::detail_close() {
	d_gen_++;
	d_meta_gen_++;
	view = return_view_;
	zone = return_zone_;
	if (view == "home") refresh_home_cards();
	if (view == "library") library_refresh();
	if (view == "discover") refresh_images();
	if (view == "search") refresh_search_cards();
	dirty_all();
}

void App::detail_button(Btn b) {
	autoplay_pending_ = false;  // any button cancels playing the next episode
	if (d_zone == "seasons") {
		switch (b) {
		case Btn::Left:
		case Btn::L1: detail_set_season(d_season_idx_ - 1); break;
		case Btn::Right:
		case Btn::R1: detail_set_season(d_season_idx_ + 1); break;
		case Btn::Down:
		case Btn::Cross: d_zone = "episodes"; break;
		case Btn::Circle: detail_close(); return;
		default: break;
		}
		dirty_all();
		return;
	}
	if (d_zone == "episodes") {
		switch (b) {
		case Btn::Up:
			if (d_episode_sel > 0) d_episode_sel--;
			else d_zone = "seasons";
			refresh_images();
			break;
		case Btn::Down:
			if (d_episode_sel + 1 < int(d_episodes.size())) d_episode_sel++;
			refresh_images();
			break;
		case Btn::L1: detail_set_season(d_season_idx_ - 1); break;
		case Btn::R1: detail_set_season(d_season_idx_ + 1); break;
		case Btn::Cross:
		case Btn::Right:
			if (d_episode_sel < int(d_season_videos_.size())) {
				d_zone = "streams";
				detail_load_streams(d_season_videos_[d_episode_sel]->id);
			}
			break;
		case Btn::Triangle:
			if (d_episode_sel < int(d_season_videos_.size())) {
				toggle_watched(d_season_videos_[d_episode_sel]->id);
				d_episodes[d_episode_sel].watched = watched(d_season_videos_[d_episode_sel]->id);
			}
			break;
		case Btn::Circle: detail_close(); return;
		default: break;
		}
		dirty_all();
		return;
	}
	// Streams: first a resolution, then a stream of that resolution.
	switch (b) {
	case Btn::Up:
		if (d_stream_sel > 0) d_stream_sel--;
		break;
	case Btn::Down:
		if (d_stream_sel + 1 < int(d_streams.size())) d_stream_sel++;
		break;
	case Btn::Cross:
	case Btn::Square:
		if (d_pick_res) {
			if (b == Btn::Cross && d_stream_sel < int(d_res_keys_.size())) {
				d_res_ = d_res_keys_[d_stream_sel];
				d_res_sel_ = d_stream_sel;
				d_pick_res = false;
				d_stream_sel = 0;
				detail_refresh_streams();
			}
		} else if (d_stream_sel < int(d_view_.size())) {
			play_stream(d_stream_list_[d_view_[d_stream_sel]], b == Btn::Square);
		}
		break;
	case Btn::Left:
	case Btn::Circle:
		if (!d_pick_res) {  // back to the resolutions
			d_pick_res = true;
			d_stream_sel = d_res_sel_;
			detail_refresh_streams();
			break;
		}
		if (b == Btn::Left) {
			if (d_series) d_zone = "episodes", d_gen_++;
			break;
		}
		if (d_series) {
			d_gen_++;
			d_zone = "episodes";
		} else {
			detail_close();
			return;
		}
		break;
	default: break;
	}
	dirty_all();
}

// ---------------------------------------------------------------------------
// Playing

void App::play_stream(const Stream& st, bool transcode) {
	std::string srv = server();
	bool torrent = st.url.empty() && !st.info_hash.empty();
	// Torrents play with the app's own engine, unless Settings says to use
	// the server; transcoding is always the server's.
	bool builtin = torrent && !transcode && (settings_.builtin_torrents || srv.empty());
	if (torrent && !builtin && srv.empty()) {
		show_toast("Torrent streams need your Stremio streaming server: set it in Settings.", 6);
		return;
	}
	if (transcode && srv.empty()) {
		show_toast("Transcoding needs your Stremio streaming server: set it in Settings.", 6);
		return;
	}

	// What's being played, for progress and the player's titles.
	w_item_ = d_item_;
	if (d_meta_loaded_) {
		w_item_.name = d_meta_.name.empty() ? d_item_.name : d_meta_.name;
		w_item_.type = d_meta_.type.empty() ? d_item_.type : d_meta_.type;
		if (!d_meta_.poster.empty()) w_item_.poster = d_meta_.poster;
		if (!d_meta_.background.empty()) w_item_.background = d_meta_.background;
	}
	w_video_id_ = d_video_id_;
	w_stream_ = st;
	w_transcode_ = transcode;

	std::string title = w_item_.name, subtitle;
	if (d_series) {
		for (auto& v : d_meta_.videos)
			if (v.id == w_video_id_)
				subtitle = "S" + std::to_string(v.season) + "E" + std::to_string(v.episode) +
				           (v.title.empty() ? "" : " · " + v.title);
	} else {
		subtitle = d_year;
	}

	// Resume where it was left.
	double start = 0;
	for (auto& cw : continue_watching()) {
		if (cw.id != w_item_.id) continue;
		bool same_video = cw.video_id.empty() || cw.video_id == w_video_id_ || !d_series;
		if (same_video && cw.offset > 5 && (cw.duration <= 0 || cw.offset < cw.duration * 0.95)) start = cw.offset;
	}

	int gen = ++w_gen_;
	launch_visible = true;
	launch_title = title;
	launch_image = art(w_item_.background, ArtKind::Background);
	launch_status = builtin   ? "Finding peers..."
	                : torrent ? "Preparing torrent..."
	                : transcode ? "Starting transcoding on the server..."
	                            : "Opening stream...";
	if (start > 0) launch_status += "  Resume at " + format_time(start);
	dirty_all();

	struct Res {
		std::string url, error;
		std::vector<std::string> headers;
		int file_idx = -1;
	};
	int season = -1, episode = -1;
	if (d_series)
		for (auto& v : d_meta_.videos)
			if (v.id == w_video_id_) season = v.season, episode = v.episode;
	if (builtin) torrent_stats_start(TorrentStream::make_url(st.info_hash, -1), st.info_hash);
	else if (torrent) torrent_stats_start(srv + "/" + st.info_hash + "/stats.json", st.info_hash);
	else t_visible = false;
	auto cancel = std::make_shared<std::atomic<bool>>(false);
	launch_cancel_ = cancel;
	Stream s = st;
	bg<Res>(
	    [s, srv, torrent, builtin, transcode, season, episode, cancel]() {
		    Res r;
		    std::string media = s.url;
		    if (!s.url.empty()) r.headers = s.request_headers;
		    if (builtin) {
			    // The app's own engine: start it, wait for the file list (the
			    // peers send it when the addon gave only the hash), pick the
			    // file and play it from the engine.
			    bt::Engine& eng = bt::Engine::get();
			    eng.start(s.info_hash, s.sources);
			    std::vector<bt::FileInfo> files;
			    std::string err;
			    if (!eng.wait_metadata(s.info_hash, files, cancel.get(), 900, &err)) {
				    if (err == "cancelled") r.error = "cancelled";
				    else if (err == "timed out")
					    r.error = "No peers sent this torrent's file list in 15 minutes. Try another stream.";
				    else r.error = "The torrent couldn't start (" + err + ").";
				    return r;
			    }
			    int idx = s.file_idx;
			    if (idx < 0 || size_t(idx) >= files.size()) idx = bt::Engine::guess_file(files, season, episode);
			    if (idx < 0) {
				    r.error = "This torrent has no video file.";
				    return r;
			    }
			    dlog("torrent %s: playing file %d, %s", s.info_hash.c_str(), idx, files[size_t(idx)].path.c_str());
			    eng.select_file(s.info_hash, idx);
			    r.url = TorrentStream::make_url(s.info_hash, idx);
			    r.file_idx = idx;
			    return r;
		    }
		    if (torrent) {
			    // As Stremio's own player does (stremio-video createTorrent):
			    // with no trackers from the addon and a known file, play the
			    // file URL straight away and let the server find peers its
			    // own way (its default trackers and DHT). Only otherwise ask
			    // it to /create the torrent first.
			    int idx = s.file_idx;
			    std::vector<std::string> sources;
			    for (auto& x : s.sources) {
				    std::string src = starts_with(x, "tracker:") || starts_with(x, "dht:") ? x : "tracker:" + x;
				    if (std::find(sources.begin(), sources.end(), src) == sources.end()) sources.push_back(src);
			    }
			    if (!sources.empty() || idx < 0) {
				    json body;
				    body["torrent"] = {{"infoHash", s.info_hash}};
				    if (!sources.empty()) {
					    std::vector<std::string> all = {"dht:" + s.info_hash};
					    for (auto& x : sources)
						    if (x != all[0]) all.push_back(x);
					    sources = all;
					    body["peerSearch"] = {{"sources", sources}, {"min", 40}, {"max", 200}};
				    }
				    if (idx < 0) {
					    json guess = json::object();
					    if (season >= 0) guess["season"] = season;
					    if (episode >= 0) guess["episode"] = episode;
					    body["guessFileIdx"] = guess;
				    } else {
					    body["guessFileIdx"] = false;
				    }
				    // With few seeders the server may need minutes to get the
				    // torrent's metadata; the PC app waits as long as it takes,
				    // so wait 15 minutes (Circle cancels). The proxy in front
				    // of the server answers 502/503/504 when it tires of
				    // waiting: that means "not yet", so ask again.
				    const double deadline = now_seconds() + 900;
				    HttpResponse h;
				    for (;;) {
					    long left = long(deadline - now_seconds());
					    h = http_post_json(srv + "/" + s.info_hash + "/create", body.dump(), std::max(left, 1L),
					                       cancel.get());
					    bool not_yet = h.error.empty() && h.status >= 502 && h.status <= 504;
					    if (!not_yet || cancel->load() || now_seconds() + 3 >= deadline) break;
					    dlog("torrent: server answered HTTP %ld to /create, asking again", h.status);
					    for (int i = 0; i < 20 && !cancel->load(); i++) SDL_Delay(100);
				    }
				    if (cancel->load()) h.error = "cancelled";
				    if (!h.error.empty() || !h.ok()) {
					    if (h.error == "cancelled") r.error = "cancelled";
					    else if (h.error.find("timed out") != std::string::npos)
						    r.error = "Your streaming server found no peers for this torrent in 15 minutes. Try another "
						              "stream.";
					    else r.error = "Your streaming server: " + h.describe();
					    return r;
				    }
				    if (idx < 0) {
					    json j = json::parse(h.body, nullptr, false);
					    idx = int(jnum(j, "guessedFileIdx", 0));
				    }
			    }
			    media = srv + "/" + s.info_hash + "/" + std::to_string(idx);
			    std::string query;
			    for (auto& x : sources) query += (query.empty() ? "?tr=" : "&tr=") + url_encode(x);
			    media += query;
			    r.file_idx = idx;
			    r.headers.clear();
		    }
		    if (transcode) {
			    r.url = srv + "/hlsv2/" + random_id() + "/master.m3u8?mediaURL=" + url_encode(media) +
			            "&videoCodecs=h264&audioCodecs=aac&audioCodecs=mp3&maxAudioChannels=2";
			    r.headers.clear();
		    } else {
			    r.url = media;
		    }
		    return r;
	    },
	    [this, gen, start, title, subtitle, srv, s, builtin](Res& r) {
		    if (gen != w_gen_ || !launch_visible) return;
		    if (!r.error.empty()) {
			    launch_visible = false;
			    t_visible = false;
			    if (r.error != "cancelled") show_toast(r.error, 8);
			    dirty_all();
			    return;
		    }
		    if (builtin) t_stats_url_ = r.url;
		    else if (r.file_idx >= 0)
			    t_stats_url_ = srv + "/" + s.info_hash + "/" + std::to_string(r.file_idx) + "/stats.json";
		    watch_start(r.url, r.headers, start, title, subtitle, !s.url.empty() && r.url == s.url);
	    });
}
