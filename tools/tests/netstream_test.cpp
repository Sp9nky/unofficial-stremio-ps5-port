// Host test: a piece that fails (HTTP 429 from the host) does not stop the stream. Reads a 130 MB file through NetStream's
// parallel mode while the host refuses some pieces a few times, and checks every byte arrives and nothing "stops".
// Run with tools/tests/run_netstream_test.sh.
#include <cstdio>
#include <cstdlib>
#include <string>

extern "C" {
#include <libavformat/avio.h>
}

#include "http.h"
#include "netstream.h"
#include "util.h"

int main(int argc, char** argv) {
	const std::string port = argc > 1 ? argv[1] : "18998";
	const std::string base = "http://127.0.0.1:" + port;
	http_init("");
	NetStream::set_cache_dir("/tmp");
	const long long size = 130ll * 1024 * 1024;
	int bad = 0;

	NetStream ns(base + "/file?fail=7", {}, nullptr, true); // each piece from 40 MB on is refused seven times first (three rounds)
	if (!ns.start()) {
		printf("FAIL: start: %s\n", ns.error().c_str());
		return 1;
	}
	printf("size %lld, parallel\n", static_cast<long long>(ns.size()));
	std::string chunk(1 << 20, 0);
	long long pos = 0;
	while (pos < size) {
		int n = ns.read(reinterpret_cast<uint8_t*>(&chunk[0]), int(chunk.size()));
		if (n <= 0) {
			printf("FAIL: read returned %d at %lld (%s)\n", n, pos, ns.failure().c_str());
			return 1;
		}
		for (int k = 0; k < n; k += 4099) {
			const long long i = pos + k;
			if (static_cast<uint8_t>(chunk[size_t(k)]) != static_cast<uint8_t>((i * 31 + (i >> 16)) & 0xFF)) {
				if (++bad < 5) printf("FAIL: wrong byte at %lld\n", i);
			}
		}
		pos += n;
	}
	const std::string failure = ns.failure();
	HttpResponse h = http_get(base + "/__hits", 5);
	printf("read %lld bytes, %s 429 answers were sent, failure(): \"%s\"\n", pos, h.body.c_str(), failure.c_str());
	if (bad || !failure.empty() || atoi(h.body.c_str()) < 7) {
		puts("FAIL");
		return 1;
	}
	puts("netstream checks OK: pieces refused with 429 were fetched again and the stream never stopped");
	return 0;
}
