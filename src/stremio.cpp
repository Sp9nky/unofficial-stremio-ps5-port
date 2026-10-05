#include "stremio.h"

#include <algorithm>

#include "http.h"

const char* kCinemetaUrl = "https://v3-cinemeta.strem.io/manifest.json";
const char* kOpenSubtitlesUrl = "https://opensubtitles-v3.strem.io/manifest.json";

static const char* kApiBase = "https://api.strem.io/api/";
static const char* kLinkBase = "https://link.stremio.com/api/v2/";

static std::vector<std::string> str_array(const json& j) {
	std::vector<std::string> out;
	if (j.is_array()) {
		for (auto& v : j)
			if (v.is_string()) out.push_back(v.get<std::string>());
	} else if (j.is_string()) {
		out.push_back(j.get<std::string>());
	}
	return out;
}

// ---------------------------------------------------------------------------
// Addons

bool Catalog::has_extra(const std::string& n) const { return extra(n) != nullptr; }

const CatalogExtra* Catalog::extra(const std::string& n) const {
	for (auto& e : extras)
		if (e.name == n) return &e;
	return nullptr;
}

bool Catalog::requires_other_than(const std::string& allowed) const {
	for (auto& e : extras)
		if (e.required && e.name != allowed) return true;
	return false;
}

std::string normalize_addon_url(std::string url) {
	url = trim(url);
	if (starts_with(url, "stremio://")) url = "https://" + url.substr(10);
	if (!starts_with(url, "http://") && !starts_with(url, "https://")) url = "https://" + url;
	if (!ends_with(url, "/manifest.json")) {
		while (!url.empty() && url.back() == '/') url.pop_back();
		url += "/manifest.json";
	}
	return url;
}

bool parse_addon(const json& m, const std::string& transport_url, Addon& a) {
	if (!m.is_object() || jstr(m, "id").empty()) return false;
	a.manifest = m;
	a.transport_url = transport_url;
	a.base = transport_url;
	if (ends_with(a.base, "/manifest.json")) a.base = a.base.substr(0, a.base.size() - 14);
	a.id = jstr(m, "id");
	a.name = jstr(m, "name", a.id);
	a.version = jstr(m, "version");
	a.description = jstr(m, "description");
	a.types = str_array(jobj(m, "types"));
	a.id_prefixes = str_array(jobj(m, "idPrefixes"));

	const json& res = jobj(m, "resources");
	if (res.is_array()) {
		for (auto& r : res) {
			AddonResource ar;
			if (r.is_string()) {
				ar.name = r.get<std::string>();
			} else if (r.is_object()) {
				ar.name = jstr(r, "name");
				ar.types = str_array(jobj(r, "types"));
				ar.id_prefixes = str_array(jobj(r, "idPrefixes"));
			}
			if (!ar.name.empty()) a.resources.push_back(ar);
		}
	}

	const json& cats = jobj(m, "catalogs");
	if (cats.is_array()) {
		for (auto& c : cats) {
			Catalog cat;
			cat.type = jstr(c, "type");
			cat.id = jstr(c, "id");
			cat.name = jstr(c, "name");
			if (cat.type.empty() || cat.id.empty()) continue;
			const json& extra = jobj(c, "extra");
			if (extra.is_array()) {
				for (auto& e : extra) {
					CatalogExtra ce;
					ce.name = jstr(e, "name");
					ce.required = jbool(e, "isRequired");
					ce.options = str_array(jobj(e, "options"));
					if (!ce.name.empty()) cat.extras.push_back(ce);
				}
			}
			// Older manifests: extraSupported / extraRequired.
			for (auto& n : str_array(jobj(c, "extraSupported")))
				if (!cat.has_extra(n)) cat.extras.push_back(CatalogExtra{n, false, {}});
			for (auto& n : str_array(jobj(c, "extraRequired"))) {
				bool found = false;
				for (auto& e : cat.extras)
					if (e.name == n) e.required = found = true;
				if (!found) cat.extras.push_back(CatalogExtra{n, true, {}});
			}
			if (const CatalogExtra* g = cat.extra("genre")) {
				if (g->options.empty()) {
					// Some addons list genres at the top level.
					for (auto& e : cat.extras)
						if (e.name == "genre") e.options = str_array(jobj(c, "genres"));
				}
			}
			a.catalogs.push_back(cat);
		}
	}
	return true;
}

bool Addon::supports(const std::string& resource, const std::string& type, const std::string& id) const {
	for (auto& r : resources) {
		if (r.name != resource) continue;
		const auto& types_ = r.types.empty() ? types : r.types;
		if (!types_.empty() && std::find(types_.begin(), types_.end(), type) == types_.end()) continue;
		const auto& prefixes = r.id_prefixes.empty() ? id_prefixes : r.id_prefixes;
		if (prefixes.empty() || id.empty()) return true;
		for (auto& p : prefixes)
			if (starts_with(id, p)) return true;
	}
	return false;
}

std::string Addon::resource_url(const std::string& resource, const std::string& type, const std::string& id,
                                const std::string& extra) const {
	std::string u = base + "/" + resource + "/" + url_encode(type) + "/" + url_encode(id);
	if (!extra.empty()) u += "/" + extra;
	return u + ".json";
}

bool fetch_json(const std::string& url, json& out, std::string& err, long timeout_s,
                const std::atomic<bool>* cancel) {
	HttpResponse r = http_get(url, timeout_s, cancel);
	if (!r.ok()) {
		err = r.describe();
		return false;
	}
	out = json::parse(r.body, nullptr, false);
	if (out.is_discarded()) {
		err = "invalid JSON";
		return false;
	}
	return true;
}

bool fetch_addon(const std::string& url, Addon& out, std::string& err, const std::atomic<bool>* cancel) {
	json m;
	if (!fetch_json(url, m, err, 15, cancel)) return false;
	if (!parse_addon(m, url, out)) {
		err = "not an addon manifest";
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// Metadata

Meta parse_meta(const json& j) {
	Meta m;
	m.id = jstr(j, "id", jstr(j, "imdb_id"));
	m.type = jstr(j, "type");
	m.name = jstr(j, "name");
	m.poster = jstr(j, "poster");
	m.background = jstr(j, "background");
	m.logo = jstr(j, "logo");
	m.description = jstr(j, "description");
	m.release_info = jstr(j, "releaseInfo", jstr(j, "year"));
	m.year = m.release_info;
	m.runtime = jstr(j, "runtime");
	m.imdb_rating = jstr(j, "imdbRating");
	m.genres = str_array(jobj(j, "genres"));
	if (m.genres.empty()) m.genres = str_array(jobj(j, "genre"));
	m.cast = str_array(jobj(j, "cast"));
	m.directors = str_array(jobj(j, "director"));
	// Newer addons use "links" with categories instead.
	const json& links = jobj(j, "links");
	if (links.is_array()) {
		bool want_genres = m.genres.empty(), want_cast = m.cast.empty(), want_dir = m.directors.empty();
		for (auto& l : links) {
			std::string cat = jstr(l, "category"), name = jstr(l, "name");
			if (name.empty()) continue;
			if (want_genres && cat == "Genres") m.genres.push_back(name);
			else if (want_cast && cat == "Cast") m.cast.push_back(name);
			else if (want_dir && (cat == "Directors" || cat == "Director")) m.directors.push_back(name);
		}
	}
	const json& vids = jobj(j, "videos");
	if (vids.is_array()) {
		for (auto& v : vids) {
			Video vid;
			vid.id = jstr(v, "id");
			if (vid.id.empty()) continue;
			vid.title = jstr(v, "title", jstr(v, "name"));
			vid.thumbnail = jstr(v, "thumbnail");
			vid.overview = jstr(v, "overview", jstr(v, "description"));
			vid.released = jstr(v, "released", jstr(v, "firstAired"));
			vid.season = int(jnum(v, "season", 0));
			vid.episode = int(jnum(v, "episode", jnum(v, "number", 0)));
			m.videos.push_back(vid);
		}
		std::stable_sort(m.videos.begin(), m.videos.end(), [](const Video& a, const Video& b) {
			if (a.season != b.season) return a.season < b.season;
			return a.episode < b.episode;
		});
	}
	return m;
}

std::vector<Stream> parse_streams(const json& j, const std::string& addon_name) {
	std::vector<Stream> out;
	const json& arr = jobj(j, "streams");
	if (!arr.is_array()) return out;
	for (auto& s : arr) {
		Stream st;
		st.addon = addon_name;
		st.name = jstr(s, "name", addon_name);
		st.description = jstr(s, "description", jstr(s, "title"));
		st.url = jstr(s, "url");
		st.info_hash = lower(jstr(s, "infoHash"));
		st.file_idx = int(jnum(s, "fileIdx", -1));
		st.sources = str_array(jobj(s, "sources"));
		const json& bh = jobj(s, "behaviorHints");
		st.filename = jstr(bh, "filename");
		st.binge_group = jstr(bh, "bingeGroup");
		st.not_web_ready = jbool(bh, "notWebReady");
		const json& req = jobj(jobj(bh, "proxyHeaders"), "request");
		if (req.is_object()) {
			for (auto it = req.begin(); it != req.end(); ++it)
				if (it.value().is_string()) st.request_headers.push_back(it.key() + ": " + it.value().get<std::string>());
		}
		// YouTube and external links can't be played here.
		if (!st.playable()) continue;
		if (starts_with(st.url, "magnet:")) {
			// magnet:?xt=urn:btih:<hash>
			size_t p = st.url.find("btih:");
			if (p != std::string::npos) {
				st.info_hash = lower(st.url.substr(p + 5, 40));
				st.url.clear();
			} else continue;
		}
		out.push_back(st);
	}
	return out;
}

std::vector<SubtitleTrack> parse_subtitles(const json& j, const std::string& addon_name) {
	std::vector<SubtitleTrack> out;
	const json& arr = jobj(j, "subtitles");
	if (!arr.is_array()) return out;
	for (auto& s : arr) {
		SubtitleTrack t;
		t.id = jstr(s, "id");
		t.url = jstr(s, "url");
		t.lang = jstr(s, "lang");
		t.addon = addon_name;
		if (t.url.empty()) continue;
		t.label = language_name(t.lang);
		out.push_back(t);
	}
	return out;
}

// ---------------------------------------------------------------------------
// Account API

ApiResult api_call(const std::string& method, json body) {
	ApiResult r;
	HttpResponse h = http_post_json(kApiBase + method, body.dump(), 25);
	if (!h.error.empty()) {
		r.error = h.error;
		return r;
	}
	json j = json::parse(h.body, nullptr, false);
	if (j.is_discarded()) {
		r.error = h.status ? "HTTP " + std::to_string(h.status) : "invalid response";
		return r;
	}
	const json& err = jobj(j, "error");
	if (!err.is_null()) {
		r.error = err.is_object() ? jstr(err, "message", "error") : (err.is_string() ? err.get<std::string>() : "error");
		return r;
	}
	if (!j.contains("result")) {
		r.error = "HTTP " + std::to_string(h.status);
		return r;
	}
	r.ok = true;
	r.result = j["result"];
	return r;
}

ApiResult api_login(const std::string& email, const std::string& password) {
	return api_call("login", json{{"type", "Login"}, {"email", email}, {"password", password}, {"facebook", false}});
}

ApiResult api_get_user(const std::string& auth_key) {
	return api_call("getUser", json{{"type", "GetUser"}, {"authKey", auth_key}});
}

ApiResult api_addon_collection(const std::string& auth_key) {
	return api_call("addonCollectionGet", json{{"type", "AddonCollectionGet"}, {"authKey", auth_key}, {"update", true}});
}

ApiResult api_library_get(const std::string& auth_key) {
	return api_call("datastoreGet",
	                json{{"authKey", auth_key}, {"collection", "libraryItem"}, {"ids", json::array()}, {"all", true}});
}

ApiResult api_library_put(const std::string& auth_key, const json& items) {
	return api_call("datastorePut", json{{"authKey", auth_key}, {"collection", "libraryItem"}, {"changes", items}});
}

ApiResult api_logout(const std::string& auth_key) {
	return api_call("logout", json{{"type", "Logout"}, {"authKey", auth_key}});
}

LinkCode link_create() {
	LinkCode lc;
	HttpResponse h = http_get(std::string(kLinkBase) + "create?type=Create", 20);
	if (!h.ok()) {
		lc.error = h.describe();
		return lc;
	}
	json j = json::parse(h.body, nullptr, false);
	const json& res = jobj(j, "result");
	lc.code = jstr(res, "code");
	lc.link = jstr(res, "link", "https://link.stremio.com");
	if (lc.code.empty()) {
		lc.error = jstr(jobj(j, "error"), "message", "no code in response");
		return lc;
	}
	lc.ok = true;
	return lc;
}

ApiResult link_read(const std::string& code) {
	ApiResult r;
	HttpResponse h = http_get(std::string(kLinkBase) + "read?type=Read&code=" + url_encode(code), 20);
	if (!h.error.empty()) {
		r.error = h.error;
		return r;
	}
	json j = json::parse(h.body, nullptr, false);
	const json& res = jobj(j, "result");
	std::string key = jstr(res, "authKey");
	if (!key.empty()) {
		r.ok = true;
		r.result = res;
		return r;
	}
	// Not approved yet. The server answers "Invalid or expired token" (code
	// 101) until then, so that isn't an expiry: the caller times out itself.
	r.error = jstr(jobj(j, "error"), "message", "pending");
	return r;
}

// ---------------------------------------------------------------------------
// Languages

struct Lang {
	const char* code2;
	const char* code3;
	const char* alt3;
	const char* name;
};

static const Lang kLangs[] = {
	{"en", "eng", "", "English"},     {"el", "gre", "ell", "Greek"},     {"es", "spa", "", "Spanish"},
	{"fr", "fre", "fra", "French"},   {"de", "ger", "deu", "German"},    {"it", "ita", "", "Italian"},
	{"pt", "por", "", "Portuguese"},  {"ro", "rum", "ron", "Romanian"},  {"nl", "dut", "nld", "Dutch"},
	{"pl", "pol", "", "Polish"},      {"ru", "rus", "", "Russian"},      {"tr", "tur", "", "Turkish"},
	{"ar", "ara", "", "Arabic"},      {"he", "heb", "", "Hebrew"},       {"bg", "bul", "", "Bulgarian"},
	{"hr", "hrv", "", "Croatian"},    {"sr", "srp", "scc", "Serbian"},   {"cs", "cze", "ces", "Czech"},
	{"sk", "slo", "slk", "Slovak"},   {"sl", "slv", "", "Slovenian"},    {"hu", "hun", "", "Hungarian"},
	{"sv", "swe", "", "Swedish"},     {"no", "nor", "nob", "Norwegian"}, {"da", "dan", "", "Danish"},
	{"fi", "fin", "", "Finnish"},     {"et", "est", "", "Estonian"},     {"lv", "lav", "", "Latvian"},
	{"lt", "lit", "", "Lithuanian"},  {"uk", "ukr", "", "Ukrainian"},    {"zh", "chi", "zho", "Chinese"},
	{"ja", "jpn", "", "Japanese"},    {"ko", "kor", "", "Korean"},       {"hi", "hin", "", "Hindi"},
	{"th", "tha", "", "Thai"},        {"vi", "vie", "", "Vietnamese"},   {"id", "ind", "", "Indonesian"},
	{"ms", "may", "msa", "Malay"},    {"fa", "per", "fas", "Persian"},   {"sq", "alb", "sqi", "Albanian"},
	{"mk", "mac", "mkd", "Macedonian"}, {"bs", "bos", "", "Bosnian"},    {"ca", "cat", "", "Catalan"},
	{"pb", "pob", "", "Portuguese (BR)"},
};

static const Lang* find_lang(const std::string& code) {
	std::string c = lower(trim(code));
	if (c.empty()) return nullptr;
	for (auto& l : kLangs) {
		if (c == l.code2 || c == l.code3 || (l.alt3[0] && c == l.alt3) || c == lower(l.name)) return &l;
	}
	return nullptr;
}

std::string language_name(const std::string& code) {
	const Lang* l = find_lang(code);
	if (l) return l->name;
	return code.empty() ? "Unknown" : code;
}

std::string language_to_iso639_2(const std::string& code) {
	const Lang* l = find_lang(code);
	return l ? l->code3 : lower(trim(code));
}
