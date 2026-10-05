#pragma once

#include <atomic>
#include <map>
#include <string>
#include <vector>

#include "util.h"

// ---------------------------------------------------------------------------
// Addons (https://github.com/Stremio/stremio-addon-sdk/blob/master/docs/protocol.md)

struct CatalogExtra {
	std::string name;
	bool required = false;
	std::vector<std::string> options;
};

struct Catalog {
	std::string type, id, name;
	std::vector<CatalogExtra> extras;
	bool has_extra(const std::string& n) const;
	bool requires_other_than(const std::string& allowed) const;  // a required extra other than `allowed`
	const CatalogExtra* extra(const std::string& n) const;
};

struct AddonResource {
	std::string name;
	std::vector<std::string> types;        // empty: the manifest's types
	std::vector<std::string> id_prefixes;  // empty: the manifest's idPrefixes (or any)
};

struct Addon {
	std::string transport_url;  // .../manifest.json
	std::string base;           // transport_url without /manifest.json
	std::string id, name, version, description;
	std::vector<std::string> types;
	std::vector<std::string> id_prefixes;
	std::vector<AddonResource> resources;
	std::vector<Catalog> catalogs;
	bool from_account = false;
	json manifest;

	bool supports(const std::string& resource, const std::string& type, const std::string& id) const;
	std::string resource_url(const std::string& resource, const std::string& type, const std::string& id,
	                         const std::string& extra = "") const;
};

bool parse_addon(const json& manifest, const std::string& transport_url, Addon& out);
std::string normalize_addon_url(std::string url);

// ---------------------------------------------------------------------------
// Metadata

struct Video {
	std::string id, title, thumbnail, overview, released;
	int season = 0, episode = 0;
};

struct Meta {
	std::string id, type, name;
	std::string poster, background, logo;
	std::string description, release_info, runtime, imdb_rating, year;
	std::vector<std::string> genres, cast, directors;
	std::vector<Video> videos;
};

Meta parse_meta(const json& j);

struct Stream {
	std::string name, description;  // display
	std::string addon;              // addon name
	std::string url;                // direct HTTP(S) URL
	std::string info_hash;          // torrent
	int file_idx = -1;
	std::vector<std::string> sources;
	std::string filename;
	std::string binge_group;
	bool not_web_ready = false;
	std::vector<std::string> request_headers;  // behaviorHints.proxyHeaders.request
	bool playable() const { return !url.empty() || !info_hash.empty(); }
};

std::vector<Stream> parse_streams(const json& j, const std::string& addon_name);

struct SubtitleTrack {
	std::string id, url, lang, label, addon;
};

std::vector<SubtitleTrack> parse_subtitles(const json& j, const std::string& addon_name);

// ---------------------------------------------------------------------------
// Account API (api.strem.io). All blocking; call from worker threads.

struct ApiResult {
	bool ok = false;
	json result;
	std::string error;
};

ApiResult api_call(const std::string& method, json body);
ApiResult api_login(const std::string& email, const std::string& password);
ApiResult api_get_user(const std::string& auth_key);
ApiResult api_addon_collection(const std::string& auth_key);
ApiResult api_library_get(const std::string& auth_key);
ApiResult api_library_put(const std::string& auth_key, const json& items);
ApiResult api_logout(const std::string& auth_key);

// Sign-in with a code approved on another device (link.stremio.com).
struct LinkCode {
	bool ok = false;
	std::string code, link, error;
};
LinkCode link_create();
// Returns ok with result.authKey once approved; ok=false, error="" while pending.
ApiResult link_read(const std::string& code);

// Fetches a manifest and parses it.
bool fetch_addon(const std::string& url, Addon& out, std::string& err, const std::atomic<bool>* cancel = nullptr);

// GET an addon resource as JSON.
bool fetch_json(const std::string& url, json& out, std::string& err, long timeout_s = 20,
                const std::atomic<bool>* cancel = nullptr);

extern const char* kCinemetaUrl;
extern const char* kOpenSubtitlesUrl;

// Language codes: "eng" -> "English", "en" -> "eng".
std::string language_name(const std::string& code);
std::string language_to_iso639_2(const std::string& code);
