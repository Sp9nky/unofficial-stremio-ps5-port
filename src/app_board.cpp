// Board (home) and search results: rows of posters, one per catalog.

#include <algorithm>
#include <cstdlib>

#include "app.h"

static const size_t kMaxRowCards = 50;

static std::string row_title(const Catalog& c) {
	std::string type = capitalize(c.type);
	return c.name.empty() ? type : c.name + " - " + type;
}

// "tt0903747:2:5" -> "S2E5"
static std::string episode_badge(const std::string& video_id) {
	auto parts = split(video_id, ':');
	if (parts.size() >= 3) {
		int s = atoi(parts[parts.size() - 2].c_str()), e = atoi(parts.back().c_str());
		if (s > 0 || e > 0) return "S" + std::to_string(s) + "E" + std::to_string(e);
	}
	return "";
}

UiCard App::make_card(const Item& it, bool show_progress) {
	UiCard c;
	c.title = it.name;
	c.initials = initials(it.name);
	if (show_progress) {
		if (it.type == "series") c.badge = episode_badge(it.video_id);
		if (it.duration > 0 && it.offset > 0) {
			int pct = std::max(2, std::min(100, int(it.offset * 100 / it.duration)));
			c.progress = std::to_string(pct) + "%";
		}
	}
	return c;
}

static std::vector<Item> parse_catalog(const json& j) {
	std::vector<Item> items;
	const json& metas = jobj(j, "metas");
	if (!metas.is_array()) return items;
	for (auto& m : metas) {
		Item it = item_from_meta_json(m);
		if (!it.id.empty() && !it.name.empty()) items.push_back(it);
	}
	return items;
}

// ---------------------------------------------------------------------------
// Continue watching

std::vector<Item> App::continue_watching() const {
	std::vector<Item> out;
	if (signed_in() && library_loaded_) {
		for (auto& kv : library_) {
			const json& li = kv.second;
			const json& st = jobj(li, "state");
			double offset = jnum(st, "timeOffset") / 1000.0;
			bool removed = jbool(li, "removed"), temp = jbool(li, "temp");
			if (offset <= 0 || (removed && !temp)) continue;
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
			out.push_back(it);
		}
	} else {
		for (auto& kv : progress_) {
			if (kv.second.time <= 0) continue;
			Item it;
			it.id = kv.first;
			it.type = kv.second.type;
			it.name = kv.second.name;
			it.poster = kv.second.poster;
			it.video_id = kv.second.video_id;
			it.offset = kv.second.time;
			it.duration = kv.second.duration;
			it.last_watched = kv.second.updated;
			out.push_back(it);
		}
	}
	std::sort(out.begin(), out.end(), [](const Item& a, const Item& b) { return a.last_watched > b.last_watched; });
	if (out.size() > kMaxRowCards) out.resize(kMaxRowCards);
	return out;
}

void App::remove_from_continue_watching(const Item& it) {
	auto p = progress_.find(it.id);
	if (p != progress_.end()) {
		p->second.time = 0;
		save_progress();
	}
	auto li = library_.find(it.id);
	if (signed_in() && li != library_.end()) {
		json& item = li->second;
		item["state"]["timeOffset"] = 0;
		item["_mtime"] = iso8601_now();
		std::string key = settings_.auth_key;
		json changes = json::array({item});
		bg<ApiResult>([key, changes]() { return api_library_put(key, changes); },
		              [this](ApiResult& r) {
			              if (!r.ok) show_toast("Could not update your library: " + r.error);
		              });
	}
	show_toast("Removed from Continue Watching");
	refresh_home_cards();
}

// ---------------------------------------------------------------------------
// Board

void App::build_home() {
	int gen = ++home_gen_;
	board_.clear();
	BoardRow cw;
	cw.title = "Continue Watching";
	cw.continue_watching = true;
	cw.loaded = true;
	board_.push_back(cw);

	for (auto& a : addons_) {
		for (auto& c : a->catalogs) {
			if (c.requires_other_than("")) continue;  // needs a search or a genre
			BoardRow row;
			row.title = row_title(c);
			row.addon_url = a->transport_url;
			row.catalog = c;
			board_.push_back(row);
		}
	}

	for (size_t i = 1; i < board_.size(); i++) {
		const Addon* a = find_addon(board_[i].addon_url);
		if (!a) continue;
		std::string url = a->resource_url("catalog", board_[i].catalog.type, board_[i].catalog.id);
		struct Res {
			std::vector<Item> items;
			std::string error;
		};
		bg<Res>(
		    [url]() {
			    Res r;
			    json j;
			    if (fetch_json(url, j, r.error)) r.items = parse_catalog(j);
			    return r;
		    },
		    [this, gen, i](Res& r) {
			    if (gen != home_gen_ || i >= board_.size()) return;
			    board_[i].loaded = true;
			    board_[i].items = std::move(r.items);
			    if (board_[i].items.size() > kMaxRowCards) board_[i].items.resize(kMaxRowCards);
			    if (!r.error.empty()) dlog("catalog %s: %s", board_[i].title.c_str(), r.error.c_str());
			    refresh_home_cards();
		    });
	}
	refresh_home_cards();
}

void App::refresh_home_cards() {
	if (!board_.empty()) board_[0].items = continue_watching();

	// Keep the selection on the same catalog when rows appear above it.
	int selected_board = (home_row < int(home_map_.size())) ? home_map_[home_row] : -1;

	home_map_.clear();
	std::vector<UiRow> rows;
	bool any_loading = false;
	for (size_t i = 0; i < board_.size(); i++) {
		BoardRow& br = board_[i];
		if (!br.loaded) {
			any_loading = true;
			continue;
		}
		if (br.items.empty()) continue;
		UiRow r;
		r.title = br.title;
		r.dom_id = "home-strip-" + std::to_string(i);
		r.see_all = !br.continue_watching;
		home_map_.push_back(int(i));
		rows.push_back(std::move(r));
	}
	// Rows still loading show up later; say so when there's nothing yet.
	if (rows.empty() && !any_loading && !addons_loading_ && !addons_.empty()) {
		UiRow r;
		r.title = "Catalogs";
		r.dom_id = "home-strip-none";
		r.message = "None of your addons offer catalogs";
		home_map_.push_back(-1);
		rows.push_back(r);
	}

	if (selected_board >= 0) {
		for (size_t r = 0; r < home_map_.size(); r++)
			if (home_map_[r] == selected_board) home_row = int(r);
	}
	if (home_row >= int(rows.size())) home_row = std::max(0, int(rows.size()) - 1);

	for (size_t r = 0; r < rows.size(); r++) {
		int bi = home_map_[r];
		if (bi < 0) continue;
		auto& items = board_[bi].items;
		for (size_t c = 0; c < items.size(); c++) {
			UiCard card = make_card(items[c], board_[bi].continue_watching);
			bool near = std::abs(int(r) - home_row) <= 2 && int(c) < home_col + 10;
			card.image = art(items[c].poster, ArtKind::PosterLarge, near);
			rows[r].cards.push_back(std::move(card));
		}
	}
	if (home_row < int(rows.size())) home_col = std::min(home_col, std::max(0, int(rows[home_row].cards.size()) - 1));
	home_rows = std::move(rows);
	dirty_all();
}

void App::open_item(const Item& it) {
	return_view_ = view;
	return_zone_ = zone;
	detail_open(it);
}

static void move_in_rows(const std::vector<UiRow>& rows, int& row, int& col, Btn b, std::string& zone, bool& moved) {
	moved = false;
	if (rows.empty()) {
		if (b == Btn::Left) zone = "nav";
		if (b == Btn::Up) zone = "searchbox";
		return;
	}
	int ncards = int(rows[row].cards.size());
	switch (b) {
	case Btn::Up:
		if (row > 0) {
			row--;
			col = std::min(col, std::max(0, int(rows[row].cards.size()) - 1));
			moved = true;
		} else {
			zone = "searchbox";
		}
		break;
	case Btn::Down:
		if (row + 1 < int(rows.size())) {
			row++;
			col = std::min(col, std::max(0, int(rows[row].cards.size()) - 1));
			moved = true;
		}
		break;
	case Btn::Left:
		if (col > 0) {
			col--;
			moved = true;
		} else {
			zone = "nav";
		}
		break;
	case Btn::Right:
		if (col + 1 < ncards) {
			col++;
			moved = true;
		}
		break;
	default: break;
	}
}

void App::board_button(Btn b) {
	bool moved = false;
	switch (b) {
	case Btn::Up:
	case Btn::Down:
	case Btn::Left:
	case Btn::Right:
		move_in_rows(home_rows, home_row, home_col, b, zone, moved);
		if (moved) refresh_home_cards();
		break;
	case Btn::Cross:
		if (home_row < int(home_map_.size()) && home_map_[home_row] >= 0) {
			auto& items = board_[home_map_[home_row]].items;
			if (home_col < int(items.size())) open_item(items[home_col]);
		}
		break;
	case Btn::Square:  // see all, or on Continue Watching: remove (Triangle is search)
		if (home_row < int(home_map_.size()) && home_map_[home_row] > 0) discover_see_all(board_[home_map_[home_row]]);
		else if (home_row < int(home_map_.size()) && home_map_[home_row] == 0) {
			auto& items = board_[0].items;
			if (home_col < int(items.size())) remove_from_continue_watching(items[home_col]);
		}
		break;
	case Btn::Circle: zone = "nav"; break;
	default: break;
	}
	dirty_all();
}

// ---------------------------------------------------------------------------
// Search

void App::start_search(const std::string& q) {
	int gen = ++search_gen_;
	search_query = q;
	view = "search";
	zone = "content";
	search_row = search_col = 0;
	search_.clear();
	for (auto& a : addons_) {
		for (auto& c : a->catalogs) {
			if (!c.has_extra("search") || c.requires_other_than("search")) continue;
			BoardRow row;
			row.title = row_title(c);
			row.addon_url = a->transport_url;
			row.catalog = c;
			search_.push_back(row);
		}
	}
	if (search_.empty()) {
		search_status = addons_loading_ ? "Addons are still loading..." : "None of your addons can search";
		refresh_search_cards();
		return;
	}
	search_status = "Searching...";
	search_pending_ = int(search_.size());
	for (size_t i = 0; i < search_.size(); i++) {
		const Addon* a = find_addon(search_[i].addon_url);
		std::string url = a ? a->resource_url("catalog", search_[i].catalog.type, search_[i].catalog.id,
		                                      "search=" + url_encode(q))
		                    : "";
		struct Res {
			std::vector<Item> items;
			std::string error;
		};
		bg<Res>(
		    [url]() {
			    Res r;
			    json j;
			    if (!url.empty() && fetch_json(url, j, r.error)) r.items = parse_catalog(j);
			    return r;
		    },
		    [this, gen, i, q](Res& r) {
			    if (gen != search_gen_ || i >= search_.size()) return;
			    search_[i].loaded = true;
			    search_[i].items = std::move(r.items);
			    if (search_[i].items.size() > kMaxRowCards) search_[i].items.resize(kMaxRowCards);
			    search_pending_--;
			    size_t total = 0;
			    for (auto& s : search_) total += s.items.size();
			    if (search_pending_ > 0) search_status = "Searching...";
			    else if (total == 0) search_status = "No results for \"" + q + "\"";
			    else search_status = "";
			    refresh_search_cards();
		    });
	}
	refresh_search_cards();
}

void App::refresh_search_cards() {
	int selected = (search_row < int(search_map_.size())) ? search_map_[search_row] : -1;
	search_map_.clear();
	std::vector<UiRow> rows;
	for (size_t i = 0; i < search_.size(); i++) {
		if (!search_[i].loaded || search_[i].items.empty()) continue;
		UiRow r;
		r.title = search_[i].title;
		r.dom_id = "search-strip-" + std::to_string(i);
		search_map_.push_back(int(i));
		rows.push_back(std::move(r));
	}
	if (selected >= 0)
		for (size_t r = 0; r < search_map_.size(); r++)
			if (search_map_[r] == selected) search_row = int(r);
	if (search_row >= int(rows.size())) search_row = std::max(0, int(rows.size()) - 1);
	for (size_t r = 0; r < rows.size(); r++) {
		auto& items = search_[search_map_[r]].items;
		for (size_t c = 0; c < items.size(); c++) {
			UiCard card = make_card(items[c], false);
			bool near = std::abs(int(r) - search_row) <= 2 && int(c) < search_col + 10;
			card.image = art(items[c].poster, ArtKind::PosterLarge, near);
			rows[r].cards.push_back(std::move(card));
		}
	}
	if (search_row < int(rows.size()))
		search_col = std::min(search_col, std::max(0, int(rows[search_row].cards.size()) - 1));
	search_rows = std::move(rows);
	dirty_all();
}

void App::search_button(Btn b) {
	bool moved = false;
	switch (b) {
	case Btn::Up:
	case Btn::Down:
	case Btn::Left:
	case Btn::Right:
		move_in_rows(search_rows, search_row, search_col, b, zone, moved);
		if (moved) refresh_search_cards();
		break;
	case Btn::Cross:
		if (search_row < int(search_map_.size())) {
			auto& items = search_[search_map_[search_row]].items;
			if (search_col < int(items.size())) open_item(items[search_col]);
		}
		break;
	case Btn::Circle:
		search_gen_++;
		set_view("home");
		zone = "content";
		break;
	default: break;
	}
	dirty_all();
}
