// Discover (one catalog as a grid, with a preview), Library, Addons, and
// the dropdown used by their filters.

#include <algorithm>

#include "app.h"

static const int kLibCols = 7;

static std::string catalog_label(const Addon* a, const Catalog* c) {
	if (!c->name.empty()) return c->name;
	return a->name;
}

// Moves a selection in a grid of `cols` columns; returns false when the move
// leaves the grid (up from the first row / left from the first column).
static bool grid_move(int& sel, int n, int cols, Btn b, bool& left_edge, bool& top_edge) {
	left_edge = top_edge = false;
	if (n == 0) {
		left_edge = b == Btn::Left;
		top_edge = b == Btn::Up;
		return false;
	}
	switch (b) {
	case Btn::Up:
		if (sel >= cols) sel -= cols;
		else top_edge = true;
		return !top_edge;
	case Btn::Down:
		if (sel + cols < n) sel += cols;
		else if (sel / cols < (n - 1) / cols) sel = n - 1;
		else return false;
		return true;
	case Btn::Left:
		if (sel % cols == 0) left_edge = true;
		else sel--;
		return !left_edge;
	case Btn::Right:
		if (sel % cols < cols - 1 && sel + 1 < n) {
			sel++;
			return true;
		}
		return false;
	default: return false;
	}
}

// ---------------------------------------------------------------------------
// Discover

void App::enter_discover() {
	// Types offered by catalogs that can be browsed without a search.
	disc_types_.clear();
	for (auto& a : addons_)
		for (auto& c : a->catalogs) {
			if (c.requires_other_than("genre")) continue;
			if (std::find(disc_types_.begin(), disc_types_.end(), c.type) == disc_types_.end())
				disc_types_.push_back(c.type);
		}
	auto rank = [](const std::string& t) { return t == "movie" ? 0 : t == "series" ? 1 : 2; };
	std::stable_sort(disc_types_.begin(), disc_types_.end(),
	                 [&](const std::string& a, const std::string& b) { return rank(a) < rank(b); });
	if (disc_types_.empty()) {
		disc_chips.clear();
		disc_cards.clear();
		disc_items_.clear();
		dp_name.clear();
		disc_status = addons_loading_ ? "Addons are still loading..." : "None of your addons have catalogs to browse.";
		dirty_all();
		return;
	}
	if (std::find(disc_types_.begin(), disc_types_.end(), disc_type_) == disc_types_.end()) {
		disc_type_ = disc_types_[0];
		disc_catalog_ = 0;
		disc_genre_.clear();
	}
	discover_build_chips();
	if (disc_items_.empty()) discover_load(false);
	dirty_all();
}

void App::discover_build_chips() {
	disc_catalogs_.clear();
	for (auto& a : addons_)
		for (auto& c : a->catalogs)
			if (c.type == disc_type_ && !c.requires_other_than("genre")) disc_catalogs_.push_back({a.get(), &c});
	if (disc_catalog_ >= int(disc_catalogs_.size())) disc_catalog_ = 0;

	disc_chips.clear();
	disc_chips.push_back({capitalize(disc_type_)});
	if (!disc_catalogs_.empty()) {
		auto& dc = disc_catalogs_[disc_catalog_];
		disc_chips.push_back({catalog_label(dc.addon, dc.catalog)});
		const CatalogExtra* g = dc.catalog->extra("genre");
		if (g && !g->options.empty()) {
			if (g->required && disc_genre_.empty()) disc_genre_ = g->options[0];
			disc_chips.push_back({disc_genre_.empty() ? "All genres" : disc_genre_});
		} else {
			disc_genre_.clear();
		}
	}
	if (disc_chip >= int(disc_chips.size())) disc_chip = int(disc_chips.size()) - 1;
}

void App::discover_load(bool more) {
	if (disc_catalogs_.empty()) return;
	if (!more) {
		disc_gen_++;
		disc_items_.clear();
		disc_cards.clear();
		disc_sel = 0;
		disc_end_ = false;
		dp_name.clear();
	}
	if (disc_loading_ && more) return;
	auto& dc = disc_catalogs_[disc_catalog_];
	std::vector<std::string> extra;
	if (!disc_genre_.empty()) extra.push_back("genre=" + url_encode(disc_genre_));
	if (more && !disc_items_.empty()) extra.push_back("skip=" + std::to_string(disc_items_.size()));
	std::string url = dc.addon->resource_url("catalog", dc.catalog->type, dc.catalog->id, join(extra, "&"));
	int gen = disc_gen_;
	disc_loading_ = true;
	if (disc_items_.empty()) disc_status = "Loading...";
	struct Res {
		std::vector<Item> items;
		std::string error;
	};
	bg<Res>(
	    [url]() {
		    Res r;
		    json j;
		    if (fetch_json(url, j, r.error)) {
			    const json& metas = jobj(j, "metas");
			    if (metas.is_array())
				    for (auto& m : metas) {
					    Item it = item_from_meta_json(m);
					    if (!it.id.empty() && !it.name.empty()) r.items.push_back(it);
				    }
		    }
		    return r;
	    },
	    [this, gen](Res& r) {
		    if (gen != disc_gen_) return;
		    disc_loading_ = false;
		    // Skip duplicates (some addons ignore skip and send page one again).
		    size_t added = 0;
		    for (auto& it : r.items) {
			    bool dup = false;
			    for (auto& e : disc_items_)
				    if (e.id == it.id) {
					    dup = true;
					    break;
				    }
			    if (dup) continue;
			    disc_items_.push_back(it);
			    UiCard c;
			    c.title = it.name;
			    c.initials = initials(it.name);
			    disc_cards.push_back(c);
			    added++;
		    }
		    if (added == 0) disc_end_ = true;
		    if (!r.error.empty() && disc_items_.empty()) disc_status = "Could not load this catalog (" + r.error + ")";
		    else if (disc_items_.empty()) disc_status = "Nothing here.";
		    else disc_status = "";
		    refresh_images();
		    discover_preview();
		    dirty_all();
	    });
	dirty_all();
}

void App::discover_preview() {
	if (disc_sel >= int(disc_items_.size())) {
		dp_name.clear();
		return;
	}
	const Item& it = disc_items_[disc_sel];
	dp_name = it.name;
	dp_logo = art(it.logo, ArtKind::LogoBox);
	dp_still = art(it.background, ArtKind::Still);
	dp_runtime = it.runtime;
	dp_year = it.release_info;
	dp_imdb = it.imdb_rating;
	dp_genres = join(it.genres, " · ");
	dp_desc = it.description;
}

void App::discover_button(Btn b) {
	if (zone == "filters") {
		switch (b) {
		case Btn::Left:
			if (disc_chip > 0) disc_chip--;
			else zone = "nav";
			break;
		case Btn::Right:
			if (disc_chip + 1 < int(disc_chips.size())) disc_chip++;
			break;
		case Btn::Up: zone = "searchbox"; break;
		case Btn::Down:
			if (!disc_cards.empty()) zone = "content";
			break;
		case Btn::Circle: zone = "nav"; break;
		case Btn::Cross:
			if (disc_chip == 0) {
				std::vector<std::string> opts;
				int active = 0;
				for (size_t i = 0; i < disc_types_.size(); i++) {
					opts.push_back(capitalize(disc_types_[i]));
					if (disc_types_[i] == disc_type_) active = int(i);
				}
				open_dropdown("Type", opts, active, [this](int i) {
					if (disc_types_[i] == disc_type_) return;
					disc_type_ = disc_types_[i];
					disc_catalog_ = 0;
					disc_genre_.clear();
					discover_build_chips();
					discover_load(false);
				});
			} else if (disc_chip == 1) {
				std::vector<std::string> opts;
				for (auto& dc : disc_catalogs_) {
					std::string label = catalog_label(dc.addon, dc.catalog);
					if (label != dc.addon->name) label += " (" + dc.addon->name + ")";
					opts.push_back(label);
				}
				open_dropdown("Catalog", opts, disc_catalog_, [this](int i) {
					if (i == disc_catalog_) return;
					disc_catalog_ = i;
					disc_genre_.clear();
					discover_build_chips();
					discover_load(false);
				});
			} else if (disc_chip == 2 && !disc_catalogs_.empty()) {
				const CatalogExtra* g = disc_catalogs_[disc_catalog_].catalog->extra("genre");
				if (!g) break;
				std::vector<std::string> opts;
				int active = 0;
				if (!g->required) opts.push_back("All genres");
				for (auto& o : g->options) {
					if (o == disc_genre_) active = int(opts.size());
					opts.push_back(o);
				}
				bool required = g->required;
				open_dropdown("Genre", opts, active, [this, opts, required](int i) {
					std::string genre = (!required && i == 0) ? "" : opts[i];
					if (genre == disc_genre_) return;
					disc_genre_ = genre;
					discover_build_chips();
					discover_load(false);
				});
			}
			break;
		default: break;
		}
		dirty_all();
		return;
	}

	bool left_edge, top_edge;
	switch (b) {
	case Btn::Up:
	case Btn::Down:
	case Btn::Left:
	case Btn::Right:
		if (grid_move(disc_sel, int(disc_cards.size()), kDiscCols, b, left_edge, top_edge)) {
			discover_preview();
			refresh_images();
			if (!disc_end_ && !disc_loading_ && disc_sel + 2 * kDiscCols >= int(disc_items_.size())) discover_load(true);
		}
		if (left_edge) zone = "nav";
		if (top_edge) zone = "filters";
		break;
	case Btn::Cross:
		if (disc_sel < int(disc_items_.size())) open_item(disc_items_[disc_sel]);
		break;
	case Btn::Circle: zone = "filters"; break;
	default: break;
	}
	dirty_all();
}

void App::discover_see_all(const BoardRow& row) {
	disc_type_ = row.catalog.type;
	disc_genre_.clear();
	disc_catalog_ = 0;
	disc_items_.clear();
	set_view("discover");  // builds types/catalogs for disc_type_
	for (size_t i = 0; i < disc_catalogs_.size(); i++) {
		if (disc_catalogs_[i].addon->transport_url == row.addon_url && disc_catalogs_[i].catalog->id == row.catalog.id) {
			disc_catalog_ = int(i);
			break;
		}
	}
	discover_build_chips();
	discover_load(false);
	zone = "content";
	dirty_all();
}

// ---------------------------------------------------------------------------
// Library

void App::enter_library() {
	if (lib_chips.empty()) lib_chip = 0;
	if (signed_in() && !library_loaded_) load_library();
	library_refresh();
}

void App::library_refresh() {
	std::vector<Item> all;
	if (signed_in()) {
		for (auto& kv : library_) {
			const json& li = kv.second;
			if (jbool(li, "removed") || jbool(li, "temp")) continue;
			const json& st = jobj(li, "state");
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
			it.times_watched = int(jnum(st, "timesWatched"));
			it.last_watched = iso8601_to_ms(jstr(st, "lastWatched"));
			if (!it.last_watched) it.last_watched = iso8601_to_ms(jstr(li, "_mtime"));
			all.push_back(it);
		}
	} else {
		for (auto& kv : progress_) {
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
	for (auto& it : all)
		if (!it.type.empty() && std::find(types.begin(), types.end(), it.type) == types.end()) types.push_back(it.type);
	std::sort(types.begin(), types.end());
	if (!lib_type_.empty() && std::find(types.begin(), types.end(), lib_type_) == types.end()) lib_type_.clear();

	static const char* sorts[] = {"Last watched", "A-Z", "Most watched"};
	lib_chips.clear();
	lib_chips.push_back({lib_type_.empty() ? "All types" : capitalize(lib_type_)});
	lib_chips.push_back({sorts[lib_sort_]});

	lib_items_.clear();
	for (auto& it : all)
		if (lib_type_.empty() || it.type == lib_type_) lib_items_.push_back(it);
	if (lib_sort_ == 0)
		std::sort(lib_items_.begin(), lib_items_.end(),
		          [](const Item& a, const Item& b) { return a.last_watched > b.last_watched; });
	else if (lib_sort_ == 1)
		std::sort(lib_items_.begin(), lib_items_.end(),
		          [](const Item& a, const Item& b) { return lower(a.name) < lower(b.name); });
	else
		std::sort(lib_items_.begin(), lib_items_.end(),
		          [](const Item& a, const Item& b) { return a.times_watched > b.times_watched; });

	lib_cards.clear();
	for (auto& it : lib_items_) lib_cards.push_back(make_card(it, true));
	if (lib_sel >= int(lib_cards.size())) lib_sel = std::max(0, int(lib_cards.size()) - 1);

	if (!lib_items_.empty()) lib_status = "";
	else if (signed_in() && !library_loaded_) lib_status = "Loading your library...";
	else if (!signed_in() && all.empty()) lib_status = "Sign in to your Stremio account (Settings) to see your library here.";
	else lib_status = "Nothing in your library matches this filter.";

	if (view == "library") refresh_images();
	dirty_all();
}

void App::library_button(Btn b) {
	if (zone == "filters") {
		switch (b) {
		case Btn::Left:
			if (lib_chip > 0) lib_chip--;
			else zone = "nav";
			break;
		case Btn::Right:
			if (lib_chip + 1 < int(lib_chips.size())) lib_chip++;
			break;
		case Btn::Up: zone = "searchbox"; break;
		case Btn::Down:
			if (!lib_cards.empty()) zone = "content";
			break;
		case Btn::Circle: zone = "nav"; break;
		case Btn::Cross:
			if (lib_chip == 0) {
				std::vector<std::string> types;
				for (auto& kv : library_) {
					std::string t = jstr(kv.second, "type");
					if (!t.empty() && std::find(types.begin(), types.end(), t) == types.end()) types.push_back(t);
				}
				for (auto& kv : progress_)
					if (!kv.second.type.empty() && std::find(types.begin(), types.end(), kv.second.type) == types.end())
						types.push_back(kv.second.type);
				std::sort(types.begin(), types.end());
				std::vector<std::string> opts = {"All types"};
				int active = 0;
				for (auto& t : types) {
					if (t == lib_type_) active = int(opts.size());
					opts.push_back(capitalize(t));
				}
				open_dropdown("Type", opts, active, [this, types](int i) {
					lib_type_ = i == 0 ? "" : types[i - 1];
					lib_sel = 0;
					library_refresh();
				});
			} else {
				open_dropdown("Sort by", {"Last watched", "A-Z", "Most watched"}, lib_sort_, [this](int i) {
					lib_sort_ = i;
					lib_sel = 0;
					library_refresh();
				});
			}
			break;
		default: break;
		}
		dirty_all();
		return;
	}
	bool left_edge, top_edge;
	switch (b) {
	case Btn::Up:
	case Btn::Down:
	case Btn::Left:
	case Btn::Right:
		if (grid_move(lib_sel, int(lib_cards.size()), kLibCols, b, left_edge, top_edge)) refresh_images();
		if (left_edge) zone = "nav";
		if (top_edge) zone = "filters";
		break;
	case Btn::Cross:
		if (lib_sel < int(lib_items_.size())) open_item(lib_items_[lib_sel]);
		break;
	case Btn::Circle: zone = "filters"; break;
	default: break;
	}
	dirty_all();
}

// ---------------------------------------------------------------------------
// Addons

void App::enter_addons() {
	addon_rows.clear();
	for (auto& a : addons_) {
		UiAddon r;
		r.name = a->name;
		r.version = a->version.empty() ? "" : "v" + a->version;
		r.desc = a->description;
		std::vector<std::string> types;
		for (auto& t : a->types) types.push_back(capitalize(t));
		r.types = join(types, " · ");
		r.local = !a->from_account;
		// No logo: the first letter of the name (one UTF-8 character).
		size_t len = a->name.empty() ? 0 : 1;
		while (len < a->name.size() && (a->name[len] & 0xC0) == 0x80) len++;
		r.initials = a->name.substr(0, len);
		r.logo = art(jstr(a->manifest, "logo"), ArtKind::Icon);
		addon_rows.push_back(r);
	}
	if (addon_sel >= int(addon_rows.size())) addon_sel = std::max(0, int(addon_rows.size()) - 1);
	dirty_all();
}

void App::addons_button(Btn b) {
	switch (b) {
	// Tiles, kAddonCols to a row.
	case Btn::Up:
		if (addon_sel >= kAddonCols) addon_sel -= kAddonCols;
		else zone = "searchbox";
		break;
	case Btn::Down: {
		int n = int(addon_rows.size());
		if (addon_sel / kAddonCols < (n - 1) / kAddonCols) addon_sel = std::min(addon_sel + kAddonCols, n - 1);
		break;
	}
	case Btn::Left:
		if (addon_sel % kAddonCols > 0) addon_sel--;
		else zone = "nav";
		break;
	case Btn::Right:
		if (addon_sel % kAddonCols < kAddonCols - 1 && addon_sel + 1 < int(addon_rows.size())) addon_sel++;
		break;
	case Btn::Circle: zone = "nav"; break;
	case Btn::Square:
		show_toast("Reloading addons...");
		load_addons();
		if (signed_in()) load_library();
		break;
	case Btn::Cross: {
		if (addon_sel >= int(addons_.size())) break;
		std::string url = addons_[addon_sel]->transport_url;
		auto it = std::find(settings_.extra_addons.begin(), settings_.extra_addons.end(), url);
		if (it == settings_.extra_addons.end()) {
			show_toast("Install and remove addons in the Stremio app on your phone or computer; they sync here. "
			           "Press Square to reload them now.",
			           7);
			break;
		}
		open_dropdown(addons_[addon_sel]->name, {"Remove addon", "Keep"}, 1, [this, url](int i) {
			if (i != 0) return;
			auto& ex = settings_.extra_addons;
			ex.erase(std::remove(ex.begin(), ex.end(), url), ex.end());
			save_settings();
			load_addons();
		});
		break;
	}
	default: break;
	}
	dirty_all();
}

// ---------------------------------------------------------------------------
// Dropdown

void App::open_dropdown(const std::string& title, const std::vector<std::string>& options, int active,
                        std::function<void(int)> chosen) {
	dd_title = title;
	dd_options.clear();
	for (size_t i = 0; i < options.size(); i++) dd_options.push_back({options[i], int(i) == active});
	dd_sel = std::max(0, std::min(active, int(options.size()) - 1));
	dd_chosen_ = chosen;
	dd_visible = true;
	dirty_all();
}

void App::dropdown_button(Btn b) {
	switch (b) {
	case Btn::Up:
		if (dd_sel > 0) dd_sel--;
		break;
	case Btn::Down:
		if (dd_sel + 1 < int(dd_options.size())) dd_sel++;
		break;
	case Btn::Cross: {
		dd_visible = false;
		auto fn = dd_chosen_;
		int sel = dd_sel;
		dd_chosen_ = nullptr;
		if (fn && sel < int(dd_options.size())) fn(sel);
		break;
	}
	case Btn::Circle:
		dd_visible = false;
		dd_chosen_ = nullptr;
		break;
	default: break;
	}
	dirty_all();
}
