// Watching: the player's overlays, tracks, subtitles and progress.

#include <algorithm>
#include <cmath>

#include "app.h"
#include "http.h"

static const double kInfoSeconds = 5;
static const double kSaveEvery = 30;

void App::watch_start(const std::string& url, const std::vector<std::string>& headers, double start,
                      const std::string& title, const std::string& subtitle) {
	Player::Options o;
	o.url = url;
	o.headers = headers;
	o.start = start;
	o.audio_langs = pref_audio_langs();
	player_.open(o);

	watching_ = true;
	launch_visible = true;
	launch_status = "Opening stream...";
	w_title = title;
	w_subtitle = subtitle;
	w_sub_size = settings_.sub_size;
	w_start_ = start;
	w_started_ = false;
	w_heavy_warned_ = false;
	w_menu_built_ = false;
	w_auto_sub_done_ = false;
	w_subs_.clear();
	w_sub_active_ = -1;
	w_sub_delay_ = 0;
	w_sub_rml.clear();
	w_sub_shown_.clear();
	w_last_save_ = now_seconds();
	w_last_saved_pos_ = start;
	menu_visible = false;
	w_paused = false;
	info_visible = true;
	w_info_until_ = now_seconds() + kInfoSeconds;
	w_progress = "0%";
	w_time = format_time(start);
	w_duration.clear();
	w_tracks.clear();
	w_stats.clear();
	watch_load_addon_subtitles();
	dirty_all();
}

// ---------------------------------------------------------------------------
// Torrent statistics (peers, speed, how much of the file is there)

void App::torrent_stats_start(const std::string& url, const std::string& info_hash) {
	t_gen_++;
	t_stats_url_ = url;
	t_hash = info_hash;
	t_peers = "0";
	t_speed = "0.00 MB/s";
	t_progress = "0.00 %";
	t_visible = true;
	t_next_poll_ = 0;
	t_polling_ = false;
	dirty_all();
}

void App::torrent_stats_poll() {
	// Only while the stream starts or buffers, as the panel is shown then.
	if (!launch_visible && !w_buffering) return;
	if (t_polling_ || now_seconds() < t_next_poll_) return;
	t_polling_ = true;
	int gen = t_gen_;
	std::string url = t_stats_url_;
	struct Stats {
		bool ok = false;
		double peers = 0, speed = 0, progress = -1;
	};
	g_tasks.run<Stats>(
	    [url]() {
		    Stats s;
		    HttpResponse h = http_get(url, 5);
		    if (!h.ok()) return s;
		    json j = json::parse(h.body, nullptr, false);
		    if (!j.is_object()) return s;
		    s.ok = true;
		    s.peers = jnum(j, "peers");
		    s.speed = jnum(j, "downloadSpeed");
		    if (j.contains("streamProgress")) s.progress = jnum(j, "streamProgress");
		    return s;
	    },
	    [this, gen](Stats& s) {
		    t_polling_ = false;
		    t_next_poll_ = now_seconds() + 1;
		    if (gen != t_gen_ || !s.ok) return;
		    char buf[32];
		    t_peers = std::to_string(int(s.peers));
		    snprintf(buf, sizeof(buf), "%.2f MB/s", s.speed / (1024 * 1024));
		    t_speed = buf;
		    if (s.progress >= 0) {
			    snprintf(buf, sizeof(buf), "%.2f %%", s.progress * 100);
			    t_progress = buf;
		    }
		    dirty("t_peers");
		    dirty("t_speed");
		    dirty("t_progress");
	    });
}

void App::watch_show_info() {
	w_info_until_ = now_seconds() + kInfoSeconds;
	if (!info_visible) {
		info_visible = true;
		dirty("info_visible");
	}
}

void App::watch_update() {
	double now = now_seconds();
	Player::State st = player_.state();

	if (st == Player::State::Failed) {
		std::string err = player_.error();
		watch_stop(false);
		show_toast("Playback failed: " + err +
		               (w_stream_.info_hash.empty() ? "" : ". The torrent may have no seeders; try another stream."),
		           8);
		return;
	}
	if (st == Player::State::Ended) {
		watch_save_progress(true);
		bool next = settings_.autoplay_next && d_series;
		watch_stop(true);
		if (next) watch_next_episode();
		return;
	}

	int pct = 0;
	bool buffering = player_.buffering(&pct);
	if (launch_visible) {
		std::string s;
		if (st == Player::State::Opening) s = "Opening stream...";
		else if (buffering) s = "Buffering " + std::to_string(pct) + "%";
		if (!s.empty() && s != launch_status) {
			launch_status = s;
			dirty("launch_status");
		}
		if (st == Player::State::Playing && !buffering) {
			launch_visible = false;
			w_started_ = true;
			dirty_all();
		}
	}

	if (st == Player::State::Playing && !w_menu_built_) {
		// The file is open: its own subtitle tracks go first.
		w_menu_built_ = true;
		std::vector<WatchSub> embedded;
		for (auto& t : player_.subtitle_tracks()) {
			WatchSub s;
			s.label = t.label + " (in file)";
			s.lang = t.lang;
			s.embedded = t.stream;
			embedded.push_back(s);
		}
		w_subs_.insert(w_subs_.begin(), embedded.begin(), embedded.end());
		if (w_sub_active_ >= 0) w_sub_active_ += int(embedded.size());
		watch_auto_subtitles();
	}

	double pos = player_.position();
	double dur = player_.duration();

	// Subtitles, every frame.
	std::string sub;
	if (w_sub_active_ >= 0 && w_sub_active_ < int(w_subs_.size())) {
		WatchSub& ws = w_subs_[w_sub_active_];
		double t = pos - w_sub_delay_;
		if (ws.embedded >= 0) sub = player_.embedded_subtitle(ws.embedded, t);
		else if (!ws.cues.empty()) sub = cues_at(ws.cues, t);
	}
	if (sub != w_sub_shown_) {
		w_sub_shown_ = sub;
		w_sub_rml = sub;
		dirty("w_sub_rml");
	}

	if (now - w_last_ui_ >= 0.25) {
		w_last_ui_ = now;
		char pbuf[32];
		snprintf(pbuf, sizeof(pbuf), "%.2f%%", dur > 0 ? std::min(100.0, pos * 100 / dur) : 0.0);
		w_progress = pbuf;
		w_time = format_time(pos);
		w_duration = dur > 0 ? format_time(dur) : "";
		std::string bt = buffering && !launch_visible ? "Buffering " + std::to_string(pct) + "%" : "";
		bool show_buf = !bt.empty() && w_started_;
		if (show_buf != w_buffering || bt != w_buffer_text) {
			w_buffering = show_buf;
			w_buffer_text = bt;
			dirty("w_buffering");
			dirty("w_buffer_text");
		}
		if (player_.paused() != w_paused) {
			w_paused = player_.paused();
			dirty("w_paused");
		}
		if (info_visible) {
			std::string audio, subs = "Off";
			for (auto& t : player_.audio_tracks())
				if (t.stream == player_.audio_stream()) audio = t.label;
			if (w_sub_active_ >= 0 && w_sub_active_ < int(w_subs_.size())) subs = w_subs_[w_sub_active_].label;
			w_tracks = "Audio: " + (audio.empty() ? "-" : audio) + "     Subtitles: " + subs;
			if (w_sub_delay_ != 0) {
				char d[32];
				snprintf(d, sizeof(d), " (%+.2fs)", w_sub_delay_);
				w_tracks += d;
			}
			w_stats = player_.stats();
			if (!w_stream_.name.empty()) w_stats = replace_all(w_stream_.name, "\n", " ") + " · " + w_stats;
		}
		dirty("w_progress");
		dirty("w_time");
		dirty("w_duration");
		dirty("w_tracks");
		dirty("w_stats");
	}

	if (info_visible && !menu_visible && !player_.paused() && now > w_info_until_) {
		info_visible = false;
		dirty("info_visible");
	}

	if (w_started_ && now - w_last_save_ > kSaveEvery) {
		w_last_save_ = now;
		watch_save_progress(false);
	}

	if (player_.too_heavy() && !w_heavy_warned_) {
		w_heavy_warned_ = true;
		show_toast("This video is too heavy to decode smoothly. Pick a 1080p stream, or press Square on a stream to "
		           "transcode it on the server.",
		           10);
	}
}

void App::watch_stop(bool ended) {
	if (w_started_ && !ended) watch_save_progress(true);
	if (ended && !w_video_id_.empty()) {
		watched_.insert(w_video_id_);
		save_progress();
	}
	w_gen_++;
	player_.close();
	watching_ = false;
	launch_visible = false;
	t_visible = false;
	menu_visible = false;
	w_sub_rml.clear();
	w_sub_shown_.clear();
	w_buffering = false;
	w_paused = false;
	// Back on the detail page: refresh what changed.
	if (view == "detail") {
		detail_update_resume();
		for (size_t i = 0; i < d_episodes.size() && i < d_season_videos_.size(); i++)
			d_episodes[i].watched = watched(d_season_videos_[i]->id);
	}
	dirty_all();
}

void App::watch_save_progress(bool final) {
	double pos = player_.position(), dur = player_.duration();
	if (dur <= 0 || w_item_.id.empty()) return;  // live streams
	bool finished = pos > dur * 0.9;
	if (!final && pos < 1) return;

	Progress& p = progress_[w_item_.id];
	p.type = w_item_.type;
	p.name = w_item_.name;
	p.poster = w_item_.poster;
	p.video_id = w_video_id_;
	p.time = finished ? 0 : pos;
	p.duration = dur;
	p.updated = now_epoch_ms();
	if (finished) watched_.insert(w_video_id_);
	save_progress();

	double delta = w_last_saved_pos_ >= 0 ? std::max(0.0, std::min(pos - w_last_saved_pos_, kSaveEvery * 2)) : 0;
	w_last_saved_pos_ = pos;

	if (!signed_in()) return;
	std::string now = iso8601_now();
	json item;
	auto it = library_.find(w_item_.id);
	if (it != library_.end()) {
		item = it->second;
	} else {
		item = json{{"_id", w_item_.id},
		            {"name", w_item_.name},
		            {"type", w_item_.type},
		            {"poster", w_item_.poster},
		            {"posterShape", "poster"},
		            {"removed", true},
		            {"temp", true},
		            {"_ctime", now},
		            {"state", json::object()},
		            {"behaviorHints", {{"defaultVideoId", nullptr}, {"featuredVideoId", nullptr}, {"hasScheduledVideos", false}}}};
	}
	json& s = item["state"];
	if (!s.is_object()) s = json::object();
	s["lastWatched"] = now;
	s["timeOffset"] = int64_t(finished ? 0 : pos * 1000);
	s["duration"] = int64_t(dur * 1000);
	s["video_id"] = w_video_id_;
	s["timeWatched"] = int64_t(jnum(s, "timeWatched") + delta * 1000);
	s["overallTimeWatched"] = int64_t(jnum(s, "overallTimeWatched") + delta * 1000);
	if (!s.contains("timesWatched")) s["timesWatched"] = 0;
	if (!s.contains("flaggedWatched")) s["flaggedWatched"] = 0;
	if (!s.contains("noNotif")) s["noNotif"] = false;
	if (!s.contains("watched")) s["watched"] = nullptr;
	if (finished && final) {
		s["timesWatched"] = int(jnum(s, "timesWatched")) + 1;
		if (w_item_.type == "movie") s["flaggedWatched"] = 1;
	}
	item["_mtime"] = now;
	library_[w_item_.id] = item;
	library_loaded_ = true;

	std::string key = settings_.auth_key;
	json changes = json::array({item});
	g_tasks.run<ApiResult>([key, changes]() { return api_library_put(key, changes); },
	                       [](ApiResult& r) {
		                       if (!r.ok) dlog("library put: %s", r.error.c_str());
	                       });
}

// ---------------------------------------------------------------------------
// Input

void App::watch_button(Btn b) {
	if (launch_visible) {
		if (b == Btn::Circle) watch_stop(false);
		return;
	}
	watch_show_info();
	switch (b) {
	case Btn::Cross:
	case Btn::Touchpad: player_.set_paused(!player_.paused()); break;
	case Btn::Up:  // audio and subtitles menu
		watch_build_menu();
		menu_visible = true;
		dirty_all();
		break;
	case Btn::L3: player_.seek(0); break;  // from the start
	case Btn::Left: player_.seek(player_.position() - 10); break;
	case Btn::Right: player_.seek(player_.position() + 10); break;
	case Btn::L1: player_.seek(player_.position() - 60); break;
	case Btn::R1: player_.seek(player_.position() + 60); break;
	case Btn::Square: {
		// Cycle: off -> each subtitle -> off
		int n = int(w_subs_.size());
		if (n == 0) {
			show_toast("No subtitles for this video");
			break;
		}
		int next = w_sub_active_ + 1;
		if (next >= n) next = -1;
		watch_select_sub(next);
		break;
	}
	case Btn::Triangle: {
		auto tracks = player_.audio_tracks();
		if (tracks.size() < 2) {
			show_toast(tracks.empty() ? "No audio" : "Only one audio track");
			break;
		}
		size_t cur = 0;
		for (size_t i = 0; i < tracks.size(); i++)
			if (tracks[i].stream == player_.audio_stream()) cur = i;
		auto& t = tracks[(cur + 1) % tracks.size()];
		player_.select_audio(t.stream);
		show_toast("Audio: " + t.label);
		break;
	}
	case Btn::Options:
		watch_build_menu();
		menu_visible = true;
		dirty_all();
		break;
	case Btn::L2:
		w_sub_delay_ -= 0.25;
		show_toast("Subtitle delay " + std::to_string(int(std::lround(w_sub_delay_ * 1000))) + " ms");
		break;
	case Btn::R2:
		w_sub_delay_ += 0.25;
		show_toast("Subtitle delay " + std::to_string(int(std::lround(w_sub_delay_ * 1000))) + " ms");
		break;
	case Btn::Circle: watch_stop(false); break;
	default: break;
	}
	dirty("w_paused");
}

void App::watch_build_menu() {
	m_audio.clear();
	int audio_sel = 0;
	auto tracks = player_.audio_tracks();
	for (size_t i = 0; i < tracks.size(); i++) {
		bool active = tracks[i].stream == player_.audio_stream();
		if (active) audio_sel = int(i);
		m_audio.push_back({tracks[i].label, active});
	}
	m_subs.clear();
	m_subs.push_back({"Off", w_sub_active_ < 0});
	for (size_t i = 0; i < w_subs_.size(); i++) {
		std::string label = w_subs_[i].label;
		if (w_subs_[i].loading) label += "  (loading)";
		if (w_subs_[i].failed) label += "  (failed)";
		m_subs.push_back({label, int(i) == w_sub_active_});
	}
	if (!menu_visible) {
		m_audio_sel = audio_sel;
		m_sub_sel = w_sub_active_ + 1;
		m_col = tracks.size() > 1 ? 0 : 1;
	}
	m_audio_sel = std::min(m_audio_sel, std::max(0, int(m_audio.size()) - 1));
	m_sub_sel = std::min(m_sub_sel, int(m_subs.size()) - 1);
	char d[64];
	snprintf(d, sizeof(d), "Subtitle delay %+.2fs", w_sub_delay_);
	m_delay = d;
}

void App::watch_menu_button(Btn b) {
	switch (b) {
	case Btn::Left: m_col = 0; break;
	case Btn::Right: m_col = 1; break;
	case Btn::Up:
		if (m_col == 0 && m_audio_sel > 0) m_audio_sel--;
		if (m_col == 1 && m_sub_sel > 0) m_sub_sel--;
		break;
	case Btn::Down:
		if (m_col == 0 && m_audio_sel + 1 < int(m_audio.size())) m_audio_sel++;
		if (m_col == 1 && m_sub_sel + 1 < int(m_subs.size())) m_sub_sel++;
		break;
	case Btn::Cross:
		if (m_col == 0) {
			auto tracks = player_.audio_tracks();
			if (m_audio_sel < int(tracks.size())) player_.select_audio(tracks[m_audio_sel].stream);
			for (size_t i = 0; i < m_audio.size(); i++) m_audio[i].active = int(i) == m_audio_sel;
		} else {
			watch_select_sub(m_sub_sel - 1);
		}
		break;
	case Btn::L2: w_sub_delay_ -= 0.25; break;
	case Btn::R2: w_sub_delay_ += 0.25; break;
	case Btn::Circle:
	case Btn::Options:
	case Btn::Touchpad:
		menu_visible = false;
		watch_show_info();
		break;
	default: break;
	}
	if (menu_visible) watch_build_menu();
	dirty_all();
}

// ---------------------------------------------------------------------------
// Subtitles

void App::watch_select_sub(int idx) {
	if (idx < 0 || idx >= int(w_subs_.size())) {
		w_sub_active_ = -1;
		show_toast("Subtitles off");
		if (menu_visible) watch_build_menu();
		dirty_all();
		return;
	}
	w_sub_active_ = idx;
	WatchSub& ws = w_subs_[idx];
	show_toast("Subtitles: " + ws.label);
	if (ws.embedded < 0 && ws.cues.empty() && !ws.loading) {
		ws.loading = true;
		ws.failed = false;
		std::string url = ws.url, lang = ws.lang;
		int gen = w_gen_;
		struct Res {
			std::vector<Cue> cues;
			std::string error;
		};
		bg<Res>(
		    [url, lang]() {
			    Res r;
			    HttpResponse h = http_get(url, 30);
			    if (!h.ok()) {
				    r.error = h.describe();
				    return r;
			    }
			    r.cues = parse_subtitle_file(to_utf8(h.body, lang));
			    if (r.cues.empty()) r.error = "That subtitle file is empty or unreadable";
			    return r;
		    },
		    [this, gen, url](Res& r) {
			    if (gen != w_gen_) return;
			    for (auto& s : w_subs_) {
				    if (s.url != url) continue;
				    s.loading = false;
				    s.cues = std::move(r.cues);
				    s.failed = !r.error.empty();
			    }
			    if (!r.error.empty()) show_toast("Could not download the subtitles: " + r.error);
			    if (menu_visible) watch_build_menu();
			    dirty_all();
		    });
	}
	if (menu_visible) watch_build_menu();
	dirty_all();
}

void App::watch_load_addon_subtitles() {
	std::string type = w_item_.type, vid = w_video_id_;
	std::vector<std::pair<std::string, std::string>> sources;
	for (auto& a : addons_)
		if (a->supports("subtitles", type, vid)) sources.push_back({a->name, a->resource_url("subtitles", type, vid)});
	int gen = w_gen_;
	for (auto& src : sources) {
		std::string name = src.first, url = src.second;
		bg<std::vector<SubtitleTrack>>(
		    [name, url]() {
			    json j;
			    std::string err;
			    if (!fetch_json(url, j, err, 25)) return std::vector<SubtitleTrack>();
			    return parse_subtitles(j, name);
		    },
		    [this, gen](std::vector<SubtitleTrack>& tracks) {
			    if (gen != w_gen_) return;
			    // Preferred languages first, then the rest, at most 60.
			    auto prefs = pref_sub_langs();
			    auto rank = [&](const std::string& lang) {
				    std::string l = language_to_iso639_2(lang);
				    for (size_t i = 0; i < prefs.size(); i++)
					    if (prefs[i] == l) return int(i);
				    return int(prefs.size());
			    };
			    std::stable_sort(tracks.begin(), tracks.end(), [&](const SubtitleTrack& a, const SubtitleTrack& b) {
				    return rank(a.lang) < rank(b.lang);
			    });
			    std::map<std::string, int> per_lang;
			    for (auto& t : tracks) {
				    if (w_subs_.size() >= 80) break;
				    int& n = per_lang[t.lang];
				    n++;
				    WatchSub s;
				    s.label = t.label + " " + std::to_string(n) + " · " + t.addon;
				    s.lang = language_to_iso639_2(t.lang);
				    s.url = t.url;
				    w_subs_.push_back(s);
			    }
			    watch_auto_subtitles();
			    if (menu_visible) watch_build_menu();
			    dirty_all();
		    });
	}
}

void App::watch_auto_subtitles() {
	if (!settings_.auto_subtitles || w_auto_sub_done_ || w_sub_active_ >= 0) return;
	for (auto& lang : pref_sub_langs()) {
		// Don't auto-pick subtitles in the language being heard.
		bool heard = false;
		for (auto& t : player_.audio_tracks())
			if (t.stream == player_.audio_stream() && t.lang == lang) heard = true;
		if (heard) {
			w_auto_sub_done_ = true;
			return;
		}
		for (size_t i = 0; i < w_subs_.size(); i++) {
			if (w_subs_[i].lang != lang) continue;
			w_auto_sub_done_ = true;
			watch_select_sub(int(i));
			return;
		}
	}
}

// ---------------------------------------------------------------------------
// Next episode

void App::watch_next_episode() {
	const auto& vids = d_meta_.videos;
	size_t cur = vids.size();
	for (size_t i = 0; i < vids.size(); i++)
		if (vids[i].id == w_video_id_) cur = i;
	if (cur + 1 >= vids.size()) return;
	const Video& next = vids[cur + 1];
	if (next.season == 0 && vids[cur].season != 0) return;  // don't roll into specials

	for (size_t i = 0; i < d_seasons_.size(); i++)
		if (d_seasons_[i] == next.season && int(i) != d_season_idx_) detail_set_season(int(i));
	for (size_t i = 0; i < d_season_videos_.size(); i++)
		if (d_season_videos_[i]->id == next.id) d_episode_sel = int(i);

	autoplay_pending_ = true;
	autoplay_binge_ = w_stream_.binge_group;
	autoplay_addon_ = w_stream_.addon;
	d_zone = "streams";
	detail_load_streams(next.id);
	show_toast("Next: S" + std::to_string(next.season) + "E" + std::to_string(next.episode) +
	           (next.title.empty() ? "" : " · " + next.title));
	dirty_all();
}
