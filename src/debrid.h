// Real-Debrid and Torbox, handled the way Nuvio handles them (NuvioTVSmart js/core/debrid/*, js/data/remote/api/debridApi.js):
// a stream that carries a "clientResolve" for the service, or (Torbox only) a bare torrent, is turned into a plain
// HTTPS link by the service's own API, and that link plays like any other direct link. Nothing here runs unless an
// API key is set in Settings.
//
//   Real-Debrid  client resolve only: addMagnet, torrent info, selectFiles, info (must be "downloaded"), unrestrict.
//   Torbox       client resolve and torrents: cache check, createtorrent (only if cached), mylist, requestdl.
//
// Everything blocks: call it from a worker thread.
#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "stremio.h"

namespace debrid {

struct Credentials {
	std::string realdebrid_key, torbox_key;
	bool any() const { return !realdebrid_key.empty() || !torbox_key.empty(); }
};

enum class Service { none, realdebrid, torbox };

struct Outcome {
	// success | stale | not_cached | error | missing_api_key | service_degraded | disabled
	std::string status;
	std::string detail;
	std::string url, filename;
	long long size = 0;
	bool ok() const { return status == "success"; }
};

// Nuvio's canResolveStream: the stream could be turned into a link with these keys.
bool can_resolve(const ::Stream& stream, const Credentials& keys, int season = -1, int episode = -1);

// Whether the stream belongs in the list. Streams the services can resolve are listed, torrents Torbox says are not
// cached are not (Nuvio's shouldListStream), and a stream only a service could play is hidden without one. Without a
// Torbox key, torrents are listed as they always were.
bool listed(const ::Stream& stream, const Credentials& keys);

// The service that will resolve the stream, for the "Getting the link from ..." line; none when it plays as before.
Service service_for(const ::Stream& stream, const Credentials& keys, int season = -1, int episode = -1);
const char* service_name(Service service);

// Nuvio's resolve(): the link, or why not. `season`/`episode` of the video being played, -1 when it is a film.
Outcome resolve(const ::Stream& stream, const Credentials& keys, int season, int episode,
                const std::atomic<bool>* cancel = nullptr);

// What to tell the viewer when it did not work.
std::string explain(const Outcome& outcome, Service service);

// Torbox's availability for the torrent-only streams of a list (Nuvio's annotateCachedAvailability): one request for
// all their hashes; each gets cache_state CACHED or NOT_CACHED, or UNKNOWN when Torbox could not be asked.
void annotate(std::vector<::Stream>& streams, const Credentials& keys);

// The API keys: true when the service accepts it.
bool validate_key(Service service, const std::string& key);

// Torbox sign-in with a code (Nuvio's DebridDeviceAuthService): show `user_code` and `url`, then poll redeem().
struct DeviceAuth {
	bool ok = false;
	std::string error;
	std::string device_code, user_code, verification_url, friendly_url;
	int interval_s = 5;
	std::string expires_at; // as Torbox sent it
};
DeviceAuth torbox_device_start(const std::string& app_name);
struct DeviceToken {
	enum Status { pending, authorized, expired, failed } status = failed;
	std::string access_token, message;
};
DeviceToken torbox_device_redeem(const std::string& device_code);

// File choice, exposed for the test: which of a torrent's files to play (Nuvio's selectDebridFile). Files are the
// services' JSON file objects; returns the index in `files`, or -1.
struct FileInfo {
	std::string id;       // as text, whatever the service sent
	std::string name;     // Torbox name / short_name, or Real-Debrid's path
	std::string mimetype;
	long long size = 0;
};
int select_file(const std::vector<FileInfo>& files, const std::string& filename, const std::string& torrent_name,
                bool has_file_idx, int file_idx, int season, int episode);

// For tests: point the services at another host (a mock). Empty keeps the real one.
void set_base_urls(const std::string& realdebrid, const std::string& torbox);
void clear_cache();

} // namespace debrid
