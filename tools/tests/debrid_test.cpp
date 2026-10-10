// Host test for src/debrid.cpp against tools/tests/mock_debrid_server.py (run it with tools/tests/run_debrid_test.sh).
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "debrid.h"
#include "http.h"
#include "util.h"

using namespace debrid;

static int g_fail = 0;
#define CHECK(cond)                                                                    \
	do {                                                                               \
		if (!(cond)) {                                                                 \
			printf("FAIL line %d: %s\n", __LINE__, #cond);                             \
			g_fail++;                                                                  \
		}                                                                              \
	} while (0)

static std::string base;
static std::vector<std::string> log_now() {
	HttpResponse h = http_get(base + "/__log", 5);
	json j = json::parse(h.body, nullptr, false);
	std::vector<std::string> out;
	if (j.is_array()) for (auto& v : j) out.push_back(v.get<std::string>());
	return out;
}
static void reset() { http_post_json(base + "/__reset", "{}", 5); clear_cache(); }
static bool has(const std::vector<std::string>& l, const std::string& s) {
	for (auto& x : l) if (x.find(s) != std::string::npos) return true;
	return false;
}

static ::Stream torrent(const std::string& hash, const std::string& title = "", int file_idx = -1) {
	::Stream s;
	s.name = "Torrentio";
	s.title = title;
	s.info_hash = hash;
	s.file_idx = file_idx;
	s.sources = {"tracker:udp://tracker.example:80/announce", "dht:" + hash};
	return s;
}
static ::Stream client(const std::string& service, bool cached = true) {
	::Stream s;
	s.client_resolve.present = true;
	s.client_resolve.type = "debrid";
	s.client_resolve.service = service;
	s.client_resolve.info_hash = std::string(40, 'c');
	s.client_resolve.is_cached = cached;
	s.client_resolve.filename = "Movie.2024.2160p.mkv";
	return s;
}

int main(int argc, char** argv) {
	const std::string port = argc > 1 ? argv[1] : "18999";
	base = "http://127.0.0.1:" + port;
	http_init("");
	set_base_urls(base + "/rd/", base + "/tb/");
	const Credentials rd{"rd-test-key", ""}, tb{"", "tb-test-key"}, both{"rd-test-key", "tb-test-key"}, none{};
	const std::string cachedA(40, 'a'), cachedC(40, 'c'), dead(40, 'd');

	// --- which file (Nuvio's selectDebridFile)
	{
		std::vector<FileInfo> f = {{"1", "/Movie.2024.2160p.mkv", "", 9000}, {"2", "/Sample/sample.mkv", "", 10},
		                           {"3", "/readme.txt", "", 5}, {"4", "/Show.S01E02.1080p.mkv", "", 4000}};
		CHECK(select_file(f, "", "", false, -1, -1, -1) == 0);                        // the largest video
		CHECK(select_file(f, "", "", false, -1, 1, 2) == 3);                          // by episode
		CHECK(select_file(f, "sample.mkv", "", false, -1, -1, -1) == 1);              // by the stream's filename
		CHECK(select_file(f, "", "", true, 1, -1, -1) == 1);                          // by index
		CHECK(select_file(f, "", "", true, 2, -1, -1) == 1);                          // index 2 is a text file: the 1-based one
		CHECK(select_file(f, "", "Movie 2024 2160p.mkv", false, -1, -1, -1) == 0);    // a torrent name that is specific
		CHECK(select_file(f, "", "Some Release Name", false, -1, -1, -1) == 0);       // one that is not: ignored
		CHECK(select_file({{"1", "/a.txt", "", 5}}, "", "", false, -1, -1, -1) == -1); // nothing to play
	}

	// --- who resolves what (canResolveStream, shouldListStream)
	CHECK(!can_resolve(torrent(cachedA), none));
	CHECK(can_resolve(torrent(cachedA), tb));
	CHECK(!can_resolve(torrent(cachedA), rd));            // Real-Debrid has no torrent resolve in Nuvio
	CHECK(can_resolve(client("realdebrid"), rd));
	CHECK(can_resolve(client("realdebrid", false), rd));  // as in Nuvio: not "direct" any more, but still resolvable
	CHECK(!can_resolve(client("realdebrid"), both));      // Torbox is the active one: a Real-Debrid stream is not its
	CHECK(can_resolve(client("torbox"), both));
	CHECK(!can_resolve(client("premiumize"), both));
	CHECK(listed(torrent(cachedA), none) && listed(torrent(cachedA), rd));  // torrents list as before without Torbox
	CHECK(!listed(client("realdebrid"), none));                             // nothing can play it
	{
		::Stream s = torrent(dead);
		s.cache_state = "NOT_CACHED";
		CHECK(!listed(s, tb) && listed(s, rd));
	}

	// --- keys
	CHECK(validate_key(Service::realdebrid, "rd-test-key") && !validate_key(Service::realdebrid, "nope"));
	CHECK(validate_key(Service::torbox, "tb-test-key") && !validate_key(Service::torbox, "nope"));

	// --- Real-Debrid, the call sequence and the link
	reset();
	{
		Outcome o = resolve(client("realdebrid"), rd, -1, -1);
		CHECK(o.ok() && o.url == "https://cdn.example/d/movie.mkv" && o.size == 9000);
		auto l = log_now();
		const std::vector<std::string> want = {"POST /rd/torrents/addMagnet", "GET /rd/torrents/info/RD1", "POST /rd/torrents/selectFiles/RD1",
		                                       "GET /rd/torrents/info/RD1", "POST /rd/unrestrict/link"};
		CHECK(l == want);
		CHECK(!has(l, "DELETE"));
		resolve(client("realdebrid"), rd, -1, -1); // remembered: no new calls
		CHECK(log_now().size() == want.size());
	}
	reset(); // a torrent Real-Debrid has not got: stale, and the torrent is taken off the account again
	{
		::Stream s = client("realdebrid");
		s.client_resolve.info_hash = std::string(32, 'x') + "deadbeef";
		Outcome o = resolve(s, rd, -1, -1);
		CHECK(o.status == "stale" && has(log_now(), "DELETE /rd/torrents/delete/RD1"));
	}
	{
		Outcome o = resolve(client("realdebrid"), Credentials{"wrong", ""}, -1, -1);
		CHECK(o.status == "error");
	}

	// --- Torbox
	reset();
	{
		::Stream s = torrent(cachedA, "Show.S01.1080p");
		Outcome o = resolve(s, tb, 1, 2);
		CHECK(o.ok() && o.url == "https://tb.example/dl/7/1" && o.filename == "Show.S01E02.mkv");
		auto l = log_now();
		CHECK(has(l, "POST /tb/v1/api/torrents/checkcached") && has(l, "POST /tb/v1/api/torrents/createtorrent [multipart]") &&
		      has(l, "GET /tb/v1/api/torrents/mylist") && has(l, "GET /tb/v1/api/torrents/requestdl"));
	}
	reset(); // not cached: Torbox is asked, nothing is added to the account
	{
		Outcome o = resolve(torrent(dead), tb, -1, -1);
		CHECK(o.status == "not_cached" && !has(log_now(), "createtorrent"));
	}
	reset();
	{
		Outcome o = resolve(torrent(std::string(40, 'a'), "overloaded"), tb, -1, -1);
		CHECK(o.ok() || o.status == "service_degraded");
		Outcome w = resolve(torrent(cachedA), Credentials{"", "wrong"}, -1, -1);
		CHECK(w.status == "error" || w.status == "stale"); // the cache check says unknown, createtorrent says 401
	}

	// --- Torbox's availability for a list
	reset();
	{
		std::vector<::Stream> list = {torrent(cachedA), torrent(dead), torrent(cachedC), client("torbox")};
		annotate(list, tb);
		CHECK(list[0].cache_state == "CACHED" && list[1].cache_state == "NOT_CACHED" && list[2].cache_state == "CACHED" &&
		      list[3].cache_state.empty());
		CHECK(listed(list[0], tb) && !listed(list[1], tb) && listed(list[3], tb));
		auto l = log_now();
		int checks = 0;
		for (auto& x : l) checks += x.find("checkcached") != std::string::npos;
		CHECK(checks == 1); // one request for all of them
		std::vector<::Stream> again = {torrent(cachedA)};
		annotate(again, rd); // no Torbox key: nothing asked
		CHECK(again[0].cache_state.empty());
	}

	// --- Torbox sign-in with a code
	reset();
	{
		DeviceAuth a = torbox_device_start("Stremio for PS5");
		CHECK(a.ok && a.user_code == "ABCD-1234" && a.device_code == "dev123" && a.interval_s == 1);
		DeviceToken t1 = torbox_device_redeem(a.device_code), t2 = torbox_device_redeem(a.device_code), t3 = torbox_device_redeem(a.device_code);
		CHECK(t1.status == DeviceToken::pending && t2.status == DeviceToken::pending);
		CHECK(t3.status == DeviceToken::authorized && t3.access_token == "tb-test-key");
	}

	if (g_fail) {
		printf("%d check(s) failed\n", g_fail);
		return 1;
	}
	puts("debrid checks OK");
	return 0;
}
