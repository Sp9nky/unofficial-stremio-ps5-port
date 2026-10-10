// Tries Real-Debrid or Torbox with a real key, from a computer: asks the service for the link of a torrent exactly as
// the app does, then reads 8 MB of the link to see that it plays and how fast this connection gets it.
//
//   DEBRID_KEY=<api key> debrid_live realdebrid <info hash> [season episode]
//   DEBRID_KEY=<api key> debrid_live torbox <info hash> [season episode]
//
// Real-Debrid is tried the way Nuvio does it: with a debrid-aware stream (clientResolve) saying the torrent is cached.
// The key is read from the environment so it does not end up in the shell history.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "debrid.h"
#include "http.h"
#include "util.h"

int main(int argc, char** argv) {
	if (argc < 3 || !getenv("DEBRID_KEY")) {
		puts("usage: DEBRID_KEY=<api key> debrid_live <realdebrid|torbox> <info hash> [season episode]");
		return 2;
	}
	const std::string service = argv[1], hash = lower(argv[2]), key = getenv("DEBRID_KEY");
	const int season = argc > 4 ? atoi(argv[3]) : -1, episode = argc > 4 ? atoi(argv[4]) : -1;
	http_init("/etc/ssl/certs/ca-certificates.crt");
	debrid::Credentials keys;
	::Stream s;
	s.info_hash = hash;
	if (service == "realdebrid") {
		keys.realdebrid_key = key;
		s.info_hash.clear();
		s.client_resolve = {true, "debrid", "realdebrid", hash, "", "", "", -1, false, {}, season, episode, true};
	} else if (service == "torbox") {
		keys.torbox_key = key;
		std::vector<::Stream> list = {s};
		debrid::annotate(list, keys);
		s = list[0];
		printf("Torbox cache: %s\n", s.cache_state.c_str());
	} else {
		puts("service must be realdebrid or torbox");
		return 2;
	}
	printf("key accepted: %s\n", debrid::validate_key(service == "torbox" ? debrid::Service::torbox : debrid::Service::realdebrid, key) ? "yes" : "NO");
	const debrid::Service who = debrid::service_for(s, keys, season, episode);
	if (who == debrid::Service::none) {
		puts("the app would not use this service for that stream");
		return 1;
	}
	const auto t0 = std::chrono::steady_clock::now();
	debrid::Outcome o = debrid::resolve(s, keys, season, episode);
	printf("resolve: %s %s (%.1f s)\n", o.status.c_str(), o.detail.c_str(),
	       std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
	if (!o.ok()) {
		printf("the app would say: %s\n", debrid::explain(o, who).c_str());
		return 1;
	}
	const size_t slash = o.url.find('/', 8);
	printf("file: %s, %lld MB, link on %s\n", o.filename.c_str(), o.size >> 20, o.url.substr(0, slash).c_str());
	const auto t1 = std::chrono::steady_clock::now();
	HttpResponse h = http_get(o.url, 30, nullptr, {"Range: bytes=0-8388607"});
	const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
	printf("range read: HTTP %ld, %zu bytes in %.1f s = %.1f MB/s one connection\n", h.status, h.body.size(), secs,
	       double(h.body.size()) / 1048576.0 / std::max(secs, 0.001));
	return h.status == 206 || h.status == 200 ? 0 : 1;
}
