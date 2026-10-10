// Opens each URL given on stdin (one per line) the way the app does (NetStream, parallel mode, 128 MB in memory), checks the
// file starts like a video, reads a few hundred MB and reports speed, the host and any trouble. Then opens the first one twice
// at once (what a seek's reopen does). The URLs may contain keys: only the host is printed.
#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "http.h"
#include "netstream.h"
#include "util.h"

static std::string host_of(const std::string& u) {
	size_t a = u.find("://");
	a = a == std::string::npos ? 0 : a + 3;
	return u.substr(a, u.find('/', a) - a);
}

static bool probe(const std::string& url, long long want_mb, const char* label) {
	const auto t0 = std::chrono::steady_clock::now();
	NetStream ns(url, {}, nullptr, true);
	if (!ns.start()) {
		printf("%s: FAIL to open: %s\n", label, ns.error().c_str());
		return false;
	}
	const double open_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	std::string head = ns.peek(4);
	const bool mkv = head.size() == 4 && (unsigned char)head[0] == 0x1A && (unsigned char)head[1] == 0x45 && (unsigned char)head[2] == 0xDF && (unsigned char)head[3] == 0xA3;
	const bool mp4 = false;
	(void)mp4;
	std::vector<uint8_t> buf(1 << 20);
	long long got = 0;
	const auto t1 = std::chrono::steady_clock::now();
	while (got < want_mb * 1048576ll) {
		int n = ns.read(buf.data(), int(buf.size()));
		if (n <= 0) {
			printf("%s: FAIL read returned %d after %lld MB (%s)\n", label, n, got >> 20, ns.failure().c_str());
			return false;
		}
		got += n;
	}
	const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
	printf("%s: ok  host %s  file %lld MB  opened in %.1f s  starts %s  read %lld MB at %.1f MB/s (%.0f Mbit/s)\n", label, host_of(url).c_str(),
	       static_cast<long long>(ns.size() >> 20), open_s, mkv ? "as Matroska" : "with other bytes", got >> 20, got / 1048576.0 / secs,
	       got * 8 / 1e6 / secs);
	return true;
}

int main(int argc, char** argv) {
	http_init("/etc/ssl/certs/ca-certificates.crt");
	const long long mb = argc > 1 ? atoll(argv[1]) : 300;
	std::vector<std::string> urls;
	for (std::string l; std::getline(std::cin, l);)
		if (!trim(l).empty()) urls.push_back(trim(l));
	int bad = 0, i = 0;
	for (auto& u : urls) {
		char label[32];
		snprintf(label, sizeof label, "stream %d", ++i);
		if (!probe(u, mb, label)) bad++;
	}
	if (!urls.empty()) {
		printf("-- the first one opened twice at once --\n");
		NetStream a(urls[0], {}, nullptr, true), b(urls[0], {}, nullptr, true);
		const bool ok = a.start() && b.start();
		printf("both open: %s\n", ok ? "ok" : "FAIL");
		if (!ok) bad++;
	}
	printf(bad ? "%d problem(s)\n" : "all fine\n", bad);
	return bad ? 1 : 0;
}
