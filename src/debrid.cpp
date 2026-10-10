#include "debrid.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <set>

#include "http.h"
#include "util.h"

namespace debrid {

namespace {

std::string g_rd_base = "https://api.real-debrid.com/rest/1.0/";
std::string g_tb_base = "https://api.torbox.app/";

constexpr long kCacheTtlMs = 15 * 60 * 1000; // RESOLVE_CACHE_TTL_MS
constexpr size_t kCacheMax = 100;            // RESOLVE_CACHE_MAX_ENTRIES

std::string base_join(const std::string& base, const std::string& path) {
	std::string b = base;
	while (!b.empty() && b.back() == '/') b.pop_back();
	size_t i = 0;
	while (i < path.size() && path[i] == '/') i++;
	return b + "/" + path.substr(i);
}

// ---------------------------------------------------------------------------
// The services' JSON, the way debridApi.js asks for it

struct Reply {
	bool ok = false;
	long status = 0;
	json data;        // the parsed body, or the text when it is not JSON
	std::string text;
};

Reply call(const std::string& base, const std::string& path, const std::string& method, const std::string& key,
           const std::atomic<bool>* cancel, const std::string& body = "", const std::string& content_type = "",
           const std::vector<std::pair<std::string, std::string>>& form = {}) {
	HttpRequest q;
	q.method = method;
	q.url = base_join(base, path);
	if (!key.empty()) q.headers.push_back("Authorization: Bearer " + key);
	q.body = body;
	q.content_type = content_type;
	q.form = form;
	q.cancel = cancel;
	HttpResponse h = http_request(q);
	Reply r;
	r.status = h.status;
	r.text = h.body;
	r.ok = h.ok();
	if (!h.error.empty()) dlog("debrid: %s %s -> %s", method.c_str(), path.substr(0, path.find('?')).c_str(), h.error.c_str());
	std::string t = trim(h.body);
	if (!t.empty()) {
		json j = json::parse(t, nullptr, false);
		r.data = j.is_discarded() ? json(h.body) : j;
	}
	return r;
}

std::string form_body(const std::vector<std::pair<std::string, std::string>>& fields) {
	std::string out;
	for (auto& f : fields) {
		if (!out.empty()) out += "&";
		out += url_encode(f.first) + "=" + url_encode(f.second);
	}
	return out;
}

const json& at(const json& j, const char* key) {
	static const json null;
	if (!j.is_object()) return null;
	auto it = j.find(key);
	return it == j.end() ? null : *it;
}

bool truthy(const json& j) {
	if (j.is_null()) return false;
	if (j.is_boolean()) return j.get<bool>();
	if (j.is_number()) return j.get<double>() != 0;
	if (j.is_string()) return !j.get<std::string>().empty();
	return true;
}

std::string text_of(const json& j) {
	if (j.is_string()) return j.get<std::string>();
	if (j.is_number_integer()) return std::to_string(j.get<long long>());
	if (j.is_number()) return std::to_string(long(j.get<double>()));
	return "";
}

bool degraded(const Reply& r) { return r.status == 502 || r.status == 503 || r.status == 504; }

Outcome failure(const char* status, const std::string& detail = "") {
	Outcome o;
	o.status = status;
	o.detail = detail;
	return o;
}

Outcome degraded_failure(const Reply& r, const char* name) {
	return failure("service_degraded", std::string(name) + " service returned HTTP " + std::to_string(r.status) + ".");
}

Outcome success(const std::string& url, const std::string& filename, long long size) {
	Outcome o;
	o.status = "success";
	o.url = url;
	o.filename = filename;
	o.size = size;
	return o;
}

// ---------------------------------------------------------------------------
// Providers (debridProviders.js)

Service by_id(const std::string& id) {
	const std::string n = lower(trim(id));
	if (n == "realdebrid" || n == "real-debrid" || n == "real_debrid" || n == "rd") return Service::realdebrid;
	if (n == "torbox" || n == "tb") return Service::torbox;
	return Service::none; // Premiumize is Nuvio's, not ours
}

const std::string& key_for(const Credentials& k, Service s) {
	static const std::string empty;
	return s == Service::realdebrid ? k.realdebrid_key : s == Service::torbox ? k.torbox_key : empty;
}

// preferredResolverService: Real-Debrid is hidden in Nuvio's settings, so only Torbox is ever the active service.
Service preferred(const Credentials& k) { return k.torbox_key.empty() ? Service::none : Service::torbox; }

bool local_capable(Service s) { return s == Service::torbox; } // LOCAL_TORRENT_RESOLVE
bool cache_check_capable(Service s) { return s == Service::torbox; }

// ---------------------------------------------------------------------------
// The resolve object (directDebridResolver.js)

struct Resolve {
	std::string type, service, info_hash, magnet_uri, torrent_name, filename;
	bool has_file_idx = false;
	int file_idx = -1;
	std::vector<std::string> sources;
	int season = -1, episode = -1;
	bool is_cached = false;
};

bool is_direct(const ::Stream& s) {
	const ClientResolve& c = s.client_resolve;
	return c.present && lower(trim(c.type)) == "debrid" && by_id(c.service) != Service::none && c.is_cached;
}

bool needs_local(const ::Stream& s) { return !is_direct(s) && s.url.empty() && !s.info_hash.empty(); }

std::string tracker_url(const std::string& source) {
	std::string v = trim(source);
	if (v.empty() || starts_with(lower(v), "dht:")) return "";
	if (starts_with(lower(v), "tracker:")) v = trim(v.substr(8));
	return v;
}

std::string build_magnet(const Resolve& r) {
	if (!trim(r.magnet_uri).empty()) return trim(r.magnet_uri);
	const std::string hash = trim(r.info_hash);
	if (hash.empty()) return "";
	std::string dn = trim(!r.filename.empty() ? r.filename : r.torrent_name);
	std::string out = "magnet:?xt=urn:btih:" + url_encode(hash);
	if (!dn.empty()) out += "&dn=" + url_encode(dn);
	std::set<std::string> seen;
	for (const auto& src : r.sources) {
		const std::string t = tracker_url(src);
		if (t.empty() || !seen.insert(t).second) continue;
		out += "&tr=" + url_encode(t);
	}
	return out;
}

bool get_resolve(const ::Stream& s, const Credentials& keys, int season, int episode, Resolve* out) {
	if (s.client_resolve.present) {
		const ClientResolve& c = s.client_resolve;
		out->type = c.type;
		out->service = c.service;
		out->info_hash = c.info_hash;
		out->magnet_uri = c.magnet_uri;
		out->torrent_name = c.torrent_name;
		out->filename = c.filename;
		out->has_file_idx = c.has_file_idx;
		out->file_idx = c.file_idx;
		out->sources = c.sources;
		out->season = c.season;
		out->episode = c.episode;
		out->is_cached = c.is_cached;
		return true;
	}
	if (!needs_local(s)) return false;
	const Service p = preferred(keys);
	if (p == Service::none || !local_capable(p)) return false;
	Resolve r;
	Resolve m; // the magnet from the hash and the addon's sources only
	m.info_hash = s.info_hash;
	m.sources = s.sources;
	r.magnet_uri = build_magnet(m);
	if (r.magnet_uri.empty()) return false;
	r.type = "torrent";
	r.info_hash = s.info_hash;
	r.has_file_idx = s.file_idx >= 0;
	r.file_idx = s.file_idx;
	r.sources = s.sources;
	r.torrent_name = !s.title.empty() ? s.title : s.name;
	r.filename = s.filename;
	r.season = season;
	r.episode = episode;
	r.service = p == Service::torbox ? "torbox" : "realdebrid";
	r.is_cached = s.cache_state == "CACHED";
	*out = r;
	return true;
}

// ---------------------------------------------------------------------------
// Which file of the torrent (debridFileSelection.js)

const char* kVideoExt[] = {".mp4", ".mkv", ".webm", ".avi", ".mov", ".m4v", ".ts", ".m2ts", ".wmv", ".flv"};

bool has_video_ext(const std::string& value) {
	const std::string n = lower(trim(value));
	for (const char* e : kVideoExt)
		if (ends_with(n, e)) return true;
	return false;
}

std::string last_part(const std::string& value) {
	std::string last;
	std::string cur;
	for (char c : value) {
		if (c == '/' || c == '\\') {
			if (!cur.empty()) last = cur;
			cur.clear();
		} else {
			cur += c;
		}
	}
	if (!cur.empty()) last = cur;
	return last;
}

std::string display_name(const FileInfo& f) { return last_part(f.name); }

std::string normalized_name(const std::string& value) {
	std::string n = last_part(value);
	const size_t dot = n.rfind('.');
	if (dot != std::string::npos && n.find('.', dot + 1) == std::string::npos && dot + 1 < n.size()) n = n.substr(0, dot);
	std::string out;
	bool gap = false;
	for (unsigned char c : lower(n)) {
		if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
			if (gap && !out.empty()) out += ' ';
			gap = false;
			out += char(c);
		} else {
			gap = true;
		}
	}
	return out;
}

std::vector<std::string> episode_patterns(int season, int episode) {
	if (season <= 0 || episode <= 0) return {};
	char a[32], b[32], c[32];
	snprintf(a, sizeof a, "s%02de%02d", season, episode);
	snprintf(b, sizeof b, "%dx%02d", season, episode);
	snprintf(c, sizeof c, "%dx%d", season, episode);
	return {a, b, c};
}

bool looks_specific(const std::string& value, const std::vector<std::string>& patterns) {
	const std::string l = lower(value);
	if (has_video_ext(l)) return true;
	for (auto& p : patterns)
		if (l.find(p) != std::string::npos) return true;
	return false;
}

bool is_playable(const FileInfo& f) {
	if (starts_with(lower(f.mimetype), "video/")) return true;
	return has_video_ext(display_name(f));
}

} // namespace

int select_file(const std::vector<FileInfo>& files, const std::string& filename, const std::string& torrent_name,
                bool has_file_idx, int file_idx, int season, int episode) {
	std::vector<int> playable;
	for (size_t i = 0; i < files.size(); i++)
		if (is_playable(files[i])) playable.push_back(int(i));
	if (playable.empty()) return -1;

	const auto patterns = episode_patterns(season, episode);
	std::vector<std::string> names;
	for (const std::string& n : {filename, looks_specific(torrent_name, patterns) ? torrent_name : std::string()}) {
		const std::string v = normalized_name(n);
		if (!v.empty() && std::find(names.begin(), names.end(), v) == names.end()) names.push_back(v);
	}
	if (!names.empty()) {
		for (int i : playable) {
			const std::string n = normalized_name(display_name(files[size_t(i)]));
			for (auto& name : names)
				if (n.find(name) != std::string::npos || name.find(n) != std::string::npos) return i;
		}
	}
	if (!patterns.empty()) {
		for (int i : playable) {
			const std::string n = lower(display_name(files[size_t(i)]));
			for (auto& p : patterns)
				if (n.find(p) != std::string::npos) return i;
		}
	}
	if (has_file_idx && file_idx >= 0) {
		if (size_t(file_idx) < files.size() && is_playable(files[size_t(file_idx)])) return file_idx;
		if (file_idx > 0 && size_t(file_idx - 1) < files.size() && is_playable(files[size_t(file_idx - 1)])) return file_idx - 1;
		for (int i : playable)
			if (files[size_t(i)].id == std::to_string(file_idx)) return i;
	}
	int best = playable[0];
	for (int i : playable)
		if (files[size_t(i)].size > files[size_t(best)].size) best = i;
	return best;
}

namespace {

std::vector<FileInfo> files_from_json(const json& arr) {
	std::vector<FileInfo> out;
	if (!arr.is_array()) return out;
	for (const json& f : arr) {
		FileInfo fi;
		fi.id = text_of(at(f, "id"));
		for (const char* k : {"name", "short_name", "shortName", "path", "absolute_path", "absolutePath"}) {
			const std::string v = text_of(at(f, k));
			if (!v.empty()) {
				fi.name = v;
				break;
			}
		}
		for (const char* k : {"mimetype", "mimeType", "mime_type"}) {
			const std::string v = text_of(at(f, k));
			if (!v.empty()) {
				fi.mimetype = v;
				break;
			}
		}
		fi.size = static_cast<long long>(jnum(f, "size", jnum(f, "bytes", 0)));
		out.push_back(fi);
	}
	return out;
}

int pick(const json& file_array, const Resolve& r, int season, int episode, std::vector<FileInfo>* files) {
	*files = files_from_json(file_array);
	return select_file(*files, r.filename, r.torrent_name, r.has_file_idx, r.file_idx, season, episode);
}

// ---------------------------------------------------------------------------
// Torbox

Outcome resolve_torbox(const Resolve& r, const std::string& key, int season, int episode, const std::atomic<bool>* cancel) {
	const std::string magnet = build_magnet(r);
	if (magnet.empty()) return failure("stale");
	Reply create = call(g_tb_base, "v1/api/torrents/createtorrent", "POST", key, cancel, "", "",
	                    {{"magnet", magnet}, {"add_only_if_cached", "true"}, {"allow_zip", "false"}});
	const json& cdata = at(at(create.data, "data"), "torrent_id");
	std::string torrent_id = text_of(cdata);
	if (torrent_id.empty()) torrent_id = text_of(at(at(create.data, "data"), "id"));
	if (!create.ok || torrent_id.empty()) {
		if (degraded(create)) return degraded_failure(create, "Torbox");
		return create.status == 409 ? failure("not_cached")
		                            : failure(create.status == 401 || create.status == 403 ? "error" : "stale");
	}
	Reply torrent = call(g_tb_base, "v1/api/torrents/mylist?id=" + url_encode(torrent_id) + "&bypass_cache=true", "GET", key, cancel);
	const json& farr = at(at(torrent.data, "data"), "files");
	if (!torrent.ok || !farr.is_array()) {
		if (degraded(torrent)) return degraded_failure(torrent, "Torbox");
		return failure("stale");
	}
	std::vector<FileInfo> files;
	const int idx = pick(farr, r, season, episode, &files);
	if (idx < 0) return failure("stale");
	const FileInfo& file = files[size_t(idx)];
	std::string q = "v1/api/torrents/requestdl?token=" + url_encode(trim(key)) + "&torrent_id=" + url_encode(torrent_id) +
	                "&zip_link=false&redirect=false&append_name=false";
	if (!file.id.empty()) q += "&file_id=" + url_encode(file.id);
	Reply link = call(g_tb_base, q, "GET", key, cancel);
	const json& d = at(link.data, "data");
	const std::string url = d.is_string() ? d.get<std::string>() : "";
	if (!link.ok || url.empty()) {
		if (degraded(link)) return degraded_failure(link, "Torbox");
		return failure("stale");
	}
	return success(url, display_name(file), file.size);
}

// ---------------------------------------------------------------------------
// Real-Debrid

Outcome resolve_realdebrid(const Resolve& r, const std::string& key, int season, int episode, const std::atomic<bool>* cancel) {
	const std::string magnet = build_magnet(r);
	if (magnet.empty()) return failure("stale");
	Reply add = call(g_rd_base, "torrents/addMagnet", "POST", key, cancel, form_body({{"magnet", magnet}}),
	                 "application/x-www-form-urlencoded");
	const std::string torrent_id = text_of(at(add.data, "id"));
	if (!add.ok || torrent_id.empty()) return failure(add.status == 401 || add.status == 403 ? "error" : "stale");
	bool resolved = false;
	Outcome out;
	do {
		Reply before = call(g_rd_base, "torrents/info/" + url_encode(torrent_id), "GET", key, cancel);
		const json& farr = at(before.data, "files");
		if (!before.ok || !farr.is_array()) {
			out = failure("stale");
			break;
		}
		std::vector<FileInfo> files;
		const int idx = pick(farr, r, season, episode, &files);
		if (idx < 0 || files[size_t(idx)].id.empty()) {
			out = failure("stale");
			break;
		}
		const FileInfo& file = files[size_t(idx)];
		Reply select = call(g_rd_base, "torrents/selectFiles/" + url_encode(torrent_id), "POST", key, cancel,
		                    form_body({{"files", file.id}}), "application/x-www-form-urlencoded");
		if (!select.ok && select.status != 202) {
			out = failure("stale");
			break;
		}
		Reply after = call(g_rd_base, "torrents/info/" + url_encode(torrent_id), "GET", key, cancel);
		if (!after.ok || lower(text_of(at(after.data, "status"))) != "downloaded") {
			out = failure("stale");
			break;
		}
		std::string link;
		const json& links = at(after.data, "links");
		if (links.is_array())
			for (const json& l : links)
				if (truthy(l)) {
					link = text_of(l);
					break;
				}
		if (link.empty()) {
			out = failure("stale");
			break;
		}
		Reply un = call(g_rd_base, "unrestrict/link", "POST", key, cancel, form_body({{"link", link}}),
		                "application/x-www-form-urlencoded");
		const std::string url = text_of(at(un.data, "download"));
		if (!un.ok || url.empty()) {
			out = failure("stale");
			break;
		}
		resolved = true;
		const std::string fn = text_of(at(un.data, "filename"));
		const long long fs = static_cast<long long>(jnum(un.data, "filesize", 0));
		out = success(url, fn.empty() ? display_name(file) : fn, fs > 0 ? fs : file.size);
	} while (false);
	if (!resolved) call(g_rd_base, "torrents/delete/" + url_encode(torrent_id), "DELETE", key, cancel);
	return out;
}

// ---------------------------------------------------------------------------
// Torbox's cache check

struct CacheStatus {
	std::string status; // success | service_degraded | unknown
	bool cached = false;
	Outcome failure_outcome;
};

Reply torbox_check_cached(const std::string& key, const std::vector<std::string>& hashes, const std::atomic<bool>* cancel) {
	json body;
	body["hashes"] = json::array();
	for (auto& h : hashes) {
		const std::string n = lower(trim(h));
		if (!n.empty()) body["hashes"].push_back(n);
	}
	return call(g_tb_base, "v1/api/torrents/checkcached?format=object", "POST", key, cancel, body.dump(), "application/json");
}

CacheStatus local_cache_status(Service s, const std::string& key, const std::string& hash, const std::atomic<bool>* cancel) {
	CacheStatus st;
	const std::string h = lower(trim(hash));
	st.status = "unknown";
	if (h.empty() || s != Service::torbox) return st;
	Reply r = torbox_check_cached(key, {h}, cancel);
	if (degraded(r)) {
		st.status = "service_degraded";
		st.failure_outcome = degraded_failure(r, "Torbox");
		return st;
	}
	if (!r.ok || at(r.data, "success") == json(false)) return st;
	st.status = "success";
	st.cached = truthy(at(at(r.data, "data"), h.c_str()));
	return st;
}

// ---------------------------------------------------------------------------
// The resolved links, remembered for 15 minutes; the same resolve asked twice at once is done once

struct CacheEntry {
	Outcome outcome;
	std::chrono::steady_clock::time_point at;
	long long order;
};
std::mutex g_mu;
std::map<std::string, CacheEntry> g_cache;
std::map<std::string, std::shared_future<Outcome>> g_inflight;
long long g_order = 0;

std::string fingerprint(const std::string& s) { return std::to_string(std::hash<std::string>()(s)); }

std::string cache_key(const Resolve& r, Service s, const std::string& api_key, const ::Stream& st, int season, int episode) {
	std::string identity = !r.info_hash.empty() ? r.info_hash : !r.magnet_uri.empty() ? r.magnet_uri : !r.torrent_name.empty() ? r.torrent_name : r.filename;
	if (trim(identity).empty()) return "";
	const std::string fn = !r.filename.empty() ? r.filename : st.filename;
	return std::string(s == Service::torbox ? "torbox" : "realdebrid") + "|" + fingerprint(api_key) + "|" + lower(trim(identity)) + "|" +
	       (r.has_file_idx ? std::to_string(r.file_idx) : "") + "|" + lower(trim(fn)) + "|" +
	       std::to_string(season >= 0 ? season : r.season) + "|" + std::to_string(episode >= 0 ? episode : r.episode);
}

} // namespace

// ---------------------------------------------------------------------------
// Public

bool can_resolve(const ::Stream& stream, const Credentials& keys, int season, int episode) {
	if (!keys.any()) return false; // "enabled"
	Resolve r;
	if (!get_resolve(stream, keys, season, episode, &r)) return false;
	const Service provider = by_id(r.service);
	if (provider == Service::none) return false;
	const Service active = preferred(keys);
	if (is_direct(stream) && active != Service::none && provider != active) return false;
	if (needs_local(stream) && !local_capable(provider)) return false;
	if (needs_local(stream) && stream.cache_state == "NOT_CACHED") return false;
	return !key_for(keys, provider).empty();
}

bool listed(const ::Stream& stream, const Credentials& keys) {
	if (!stream.url.empty() || can_resolve(stream, keys)) return true;
	// A torrent plays with the app's engine or the server, unless Torbox is the one resolving torrents.
	if (!stream.info_hash.empty()) return keys.torbox_key.empty();
	return false; // a stream only a service could play
}

Service service_for(const ::Stream& stream, const Credentials& keys, int season, int episode) {
	if (!stream.url.empty() || !can_resolve(stream, keys, season, episode)) return Service::none;
	Resolve r;
	if (!get_resolve(stream, keys, season, episode, &r)) return Service::none;
	return by_id(r.service);
}

const char* service_name(Service s) { return s == Service::realdebrid ? "Real-Debrid" : s == Service::torbox ? "Torbox" : "Debrid"; }

Outcome resolve(const ::Stream& stream, const Credentials& keys, int season, int episode, const std::atomic<bool>* cancel) {
	if (!stream.url.empty()) return success(stream.url, stream.filename, 0);
	if (!keys.any()) return failure("disabled");
	Resolve r;
	if (!get_resolve(stream, keys, season, episode, &r)) return failure("stale");
	const Service provider = by_id(r.service);
	const std::string api_key = provider == Service::none ? "" : key_for(keys, provider);
	if (provider == Service::none || api_key.empty()) return failure("missing_api_key");
	const Service active = preferred(keys);
	if (is_direct(stream) && active != Service::none && provider != active) return failure("stale");
	if (needs_local(stream) && stream.cache_state == "NOT_CACHED") return failure("not_cached");
	if (needs_local(stream) && !stream.info_hash.empty() && stream.cache_state != "CACHED" && cache_check_capable(provider)) {
		const CacheStatus cs = local_cache_status(provider, api_key, stream.info_hash, cancel);
		if (cs.status == "service_degraded") return cs.failure_outcome;
		if (cs.status == "success" && !cs.cached) return failure("not_cached");
	}

	const std::string key = cache_key(r, provider, api_key, stream, season, episode);
	std::promise<Outcome> promise;
	std::shared_future<Outcome> waiting;
	bool owner = true;
	if (!key.empty()) {
		std::unique_lock<std::mutex> lock(g_mu);
		auto c = g_cache.find(key);
		if (c != g_cache.end()) {
			const long age = long(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - c->second.at).count());
			if (age <= kCacheTtlMs) return c->second.outcome;
			g_cache.erase(c);
		}
		auto f = g_inflight.find(key);
		if (f != g_inflight.end()) {
			waiting = f->second;
			owner = false;
		} else {
			g_inflight[key] = promise.get_future().share();
		}
	}
	if (!owner) return waiting.get();

	const int s = season >= 0 ? season : r.season, e = episode >= 0 ? episode : r.episode;
	Outcome out = provider == Service::torbox ? resolve_torbox(r, api_key, s, e, cancel) : resolve_realdebrid(r, api_key, s, e, cancel);
	if (!key.empty()) {
		std::lock_guard<std::mutex> lock(g_mu);
		if (out.ok()) {
			g_cache[key] = {out, std::chrono::steady_clock::now(), ++g_order};
			while (g_cache.size() > kCacheMax) {
				auto oldest = g_cache.begin();
				for (auto it = g_cache.begin(); it != g_cache.end(); ++it)
					if (it->second.order < oldest->second.order) oldest = it;
				g_cache.erase(oldest);
			}
		}
		promise.set_value(out);
		g_inflight.erase(key);
	}
	return out;
}

std::string explain(const Outcome& o, Service service) {
	const std::string name = service_name(service);
	if (o.status == "not_cached") return "This one isn't cached on " + name + " yet. Pick another stream.";
	if (o.status == "missing_api_key") return "Add your " + name + " key in Settings first.";
	if (o.status == "error") return name + " refused the request. Check the key in Settings.";
	if (o.status == "service_degraded") return o.detail.empty() ? name + " isn't answering right now. Try again in a minute." : o.detail + " Try again in a minute.";
	if (o.status == "disabled") return "Connect " + name + " in Settings first.";
	return name + " has no link for this one any more. Pick another stream.";
}

void annotate(std::vector<::Stream>& streams, const Credentials& keys) {
	const Service p = preferred(keys);
	if (p == Service::none || !cache_check_capable(p)) return;
	std::vector<std::string> hashes;
	std::set<std::string> seen;
	for (const ::Stream& s : streams) {
		if (s.info_hash.empty() || !s.url.empty() || s.client_resolve.present) continue;
		if (s.cache_state == "CACHED" || s.cache_state == "NOT_CACHED") continue;
		const std::string h = lower(trim(s.info_hash));
		if (!h.empty() && seen.insert(h).second) hashes.push_back(h);
	}
	if (hashes.empty()) return;
	Reply r = torbox_check_cached(keys.torbox_key, hashes, nullptr);
	const bool good = r.ok && at(r.data, "success") != json(false);
	const json& d = at(r.data, "data");
	std::set<std::string> cached;
	if (good && d.is_object())
		for (auto it = d.begin(); it != d.end(); ++it) cached.insert(lower(it.key()));
	for (::Stream& s : streams) {
		if (s.info_hash.empty() || !s.url.empty() || s.client_resolve.present) continue;
		if (s.cache_state == "CACHED" || s.cache_state == "NOT_CACHED") continue;
		s.cache_state = !good ? "UNKNOWN" : cached.count(lower(trim(s.info_hash))) ? "CACHED" : "NOT_CACHED";
	}
	dlog("debrid: Torbox cache check, %zu torrents, %zu cached%s", hashes.size(), cached.size(), good ? "" : " (it could not be asked)");
}

bool validate_key(Service service, const std::string& key) {
	if (service == Service::realdebrid) return call(g_rd_base, "user", "GET", trim(key), nullptr).ok;
	if (service == Service::torbox) return call(g_tb_base, "v1/api/user/me", "GET", trim(key), nullptr).ok;
	return false;
}

DeviceAuth torbox_device_start(const std::string& app_name) {
	DeviceAuth a;
	Reply r = call(g_tb_base, "v1/api/user/auth/device/start?app=" + url_encode(app_name), "GET", "", nullptr);
	const json& d = at(r.data, "data");
	if (!r.ok || at(r.data, "success") == json(false) || !d.is_object()) {
		a.error = r.status ? "Torbox answered HTTP " + std::to_string(r.status) : "Torbox could not be reached";
		return a;
	}
	a.device_code = trim(text_of(at(d, "device_code")));
	a.user_code = trim(text_of(at(d, "code")));
	a.verification_url = trim(text_of(at(d, "verification_url")));
	a.friendly_url = trim(text_of(at(d, "friendly_verification_url")));
	if (a.friendly_url.empty()) a.friendly_url = a.verification_url;
	const double interval = jnum(d, "interval", 5);
	a.interval_s = interval > 0 ? std::max(1, int(interval)) : 5;
	a.expires_at = text_of(at(d, "expires_at"));
	if (a.device_code.empty() || a.user_code.empty() || a.verification_url.empty()) {
		a.error = "Torbox's answer was incomplete.";
		return a;
	}
	a.ok = true;
	return a;
}

DeviceToken torbox_device_redeem(const std::string& device_code) {
	DeviceToken t;
	json body{{"device_code", trim(device_code)}};
	Reply r = call(g_tb_base, "v1/api/user/auth/device/token", "POST", "", nullptr, body.dump(), "application/json");
	const json& env = r.data.is_object() ? r.data : json::object();
	const std::string token = trim(text_of(at(at(env, "data"), "access_token")));
	if (r.ok && at(env, "success") != json(false) && !token.empty()) {
		t.status = DeviceToken::authorized;
		t.access_token = token;
		return t;
	}
	std::string message = lower(trim(text_of(at(env, "error")) + " " + text_of(at(env, "detail")) + " " +
	                                 text_of(at(env, "error_description")) + " " + text_of(at(env, "message"))));
	if (message.find("pending") != std::string::npos || message.find("not authorized") != std::string::npos ||
	    message.find("not been used") != std::string::npos || message.find("not used yet") != std::string::npos ||
	    message.find("scan the code") != std::string::npos || r.status == 404 || r.status == 409 || r.status == 425) {
		t.status = DeviceToken::pending;
		return t;
	}
	if (message.find("expired") != std::string::npos || r.status == 410) {
		t.status = DeviceToken::expired;
		return t;
	}
	t.status = DeviceToken::failed;
	t.message = trim(message);
	return t;
}

void set_base_urls(const std::string& realdebrid, const std::string& torbox) {
	g_rd_base = realdebrid.empty() ? "https://api.real-debrid.com/rest/1.0/" : realdebrid;
	g_tb_base = torbox.empty() ? "https://api.torbox.app/" : torbox;
}

void clear_cache() {
	std::lock_guard<std::mutex> lock(g_mu);
	g_cache.clear();
}

} // namespace debrid
