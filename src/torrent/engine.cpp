#include "engine.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <openssl/rand.h>
#include <openssl/sha.h>

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "../http.h"
#include "../util.h"
#include "bencode.h"

#ifdef HAVE_DHT
#include <dht/dht.h>
#endif
#ifdef HAVE_UPNP
#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#include <miniupnpc/upnperrors.h>
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#ifdef PLATFORM_PS5_NATIVE
// native/console_curl.c: the console's own non-blocking socket option.
extern "C" int console_curl_nonblocking(int socket);
#endif

namespace bt {

namespace {

const int kBlock = 16 * 1024;
const int kMaxPeers = 80;            // connected at once
// Most addresses from trackers and the DHT never answer (gone, or behind a
// router): try many at once and give up on each soon, so the few that work
// are found in seconds rather than minutes (on the PS5: 5 of 1000 answered,
// 30 attempts of 7 s each took minutes to get through the list).
const int kMaxConnecting = 80;       // connection attempts at once
const double kConnectTimeout = 4;
const double kHandshakeTimeout = 15;
const double kRequestTimeout = 15;   // a block not here by then is asked from someone else
const double kChokedTimeout = 60;    // a peer that keeps us choked this long makes room for another
const uint32_t kMaxMessage = 2u << 20;
// How far ahead of the player to download: half the 1 GB cache, about three
// minutes of a 4K remux, to ride out a slow patch in the swarm.
const int64_t kMaxReadahead = 512ll << 20;
const int64_t kMaxActiveBytes = 128ll << 20;  // pieces being assembled in memory
const int64_t kMaxRamCache = 384ll << 20;     // when the cache file can't be used
const double kPauseAfter = 45;       // no reader for this long: stop downloading
const int kTrackerThreads = 2;
const int kExtMetadata = 1, kExtPex = 2;  // our extension message ids (BEP 10)

const char* kDefaultTrackers[] = {
    "udp://tracker.opentrackr.org:1337/announce",
    "udp://open.stealth.si:80/announce",
    "udp://tracker.torrent.eu.org:451/announce",
    "udp://exodus.desync.com:6969/announce",
    "udp://open.demonii.com:1337/announce",
    "udp://explodie.org:6969/announce",
    "udp://tracker.openbittorrent.com:6969/announce",
    "udp://tracker.dler.org:6969/announce",
    "udp://p4p.arenabg.com:1337/announce",
    "http://tracker.opentrackr.org:1337/announce",
};

const char* kDhtBootstrap[][2] = {
    {"router.bittorrent.com", "6881"},
    {"dht.transmissionbt.com", "6881"},
    {"router.utorrent.com", "6881"},
    {"dht.libtorrent.org", "25401"},
};

// ---------------------------------------------------------------------------
// Small helpers

uint32_t be32(const char* p) {
	const auto* u = reinterpret_cast<const uint8_t*>(p);
	return (uint32_t(u[0]) << 24) | (uint32_t(u[1]) << 16) | (uint32_t(u[2]) << 8) | u[3];
}
void put32(std::string& s, uint32_t v) {
	char b[4] = {char(v >> 24), char(v >> 16), char(v >> 8), char(v)};
	s.append(b, 4);
}
void put64(std::string& s, uint64_t v) {
	put32(s, uint32_t(v >> 32));
	put32(s, uint32_t(v));
}

bool hex_to_bytes(const std::string& hex, uint8_t out[20]) {
	if (hex.size() != 40) return false;
	for (int i = 0; i < 20; i++) {
		unsigned v;
		if (sscanf(hex.c_str() + i * 2, "%2x", &v) != 1) return false;
		out[i] = uint8_t(v);
	}
	return true;
}

std::string pct_encode(const std::string& raw) {
	static const char* hx = "0123456789ABCDEF";
	std::string out;
	for (unsigned char c : raw) {
		if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += char(c);
		else out += '%', out += hx[c >> 4], out += hx[c & 15];
	}
	return out;
}

uint64_t addr_key(const sockaddr_in& a) { return (uint64_t(ntohl(a.sin_addr.s_addr)) << 16) | ntohs(a.sin_port); }

std::string addr_str(const sockaddr_in& a) {
	char ip[INET_ADDRSTRLEN] = "?";
	inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
	return std::string(ip) + ":" + std::to_string(ntohs(a.sin_port));
}

bool usable(const sockaddr_in& a) {
	uint32_t ip = ntohl(a.sin_addr.s_addr);
	return ip != 0 && (ip >> 24) != 127 && (ip >> 24) != 0 && ntohs(a.sin_port) != 0 && (ip >> 28) < 14;
}

// Compact peers: 4 bytes IPv4 + 2 bytes port each.
void parse_compact(const std::string& s, std::vector<sockaddr_in>& out) {
	for (size_t i = 0; i + 6 <= s.size(); i += 6) {
		sockaddr_in a{};
		a.sin_family = AF_INET;
		memcpy(&a.sin_addr.s_addr, s.data() + i, 4);
		memcpy(&a.sin_port, s.data() + i + 4, 2);
		if (usable(a)) out.push_back(a);
	}
}

// In a PS5 title fcntl(O_NONBLOCK) doesn't make a socket non-blocking: the
// console has its own socket option for it (as curl's sockets use).
void set_nonblocking(int fd) {
#ifdef PLATFORM_PS5_NATIVE
	static bool logged = false;
	int r = console_curl_nonblocking(fd);
	if (!logged) {
		logged = true;
		dlog("torrent: non-blocking sockets: %s", r == 0 ? "ok" : "FAILED");
	}
	if (r == 0) return;
#endif
	int fl = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, (fl < 0 ? 0 : fl) | O_NONBLOCK);
}

// Whether a read would return at once. Reads in a loop check this first, so
// a socket that stayed blocking can't stall the network thread.
bool readable_now(int fd) {
	pollfd pf{fd, POLLIN, 0};
	return poll(&pf, 1, 0) > 0 && (pf.revents & (POLLIN | POLLERR | POLLHUP));
}

void random_bytes(void* buf, size_t n) {
	if (RAND_bytes(static_cast<unsigned char*>(buf), int(n)) != 1) {
		auto* p = static_cast<uint8_t*>(buf);
		for (size_t i = 0; i < n; i++) p[i] = uint8_t(rand());
	}
}

bool is_video_name(const std::string& path) {
	static const char* exts[] = {".mkv", ".mp4", ".avi", ".m4v", ".mov", ".ts", ".m2ts", ".webm", ".wmv", ".mpg", ".mpeg", ".flv"};
	std::string p = lower(path);
	for (auto* e : exts)
		if (ends_with(p, e)) return true;
	return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// State

struct Peer {
	int fd = -1;
	sockaddr_in addr{};
	uint64_t key = 0;
	enum State { Connecting, Handshaking, Active } state = Connecting;
	double since = 0, last_data = 0, last_send = 0, choked_since = 0;
	bool got_any = false;  // sent us at least one block
	bool incoming = false;  // it connected to us: our handshake goes after theirs
	std::string in;
	size_t in_off = 0;
	std::string out;
	bool ext = false;
	bool peer_choking = true, am_interested = false;
	int ut_metadata = 0, ut_pex = 0;
	int64_t metadata_size = 0;
	int reqq = 250;
	std::vector<uint8_t> has;
	int has_count = 0;
	bool has_all_early = false;
	std::string early_bitfield;
	std::vector<uint32_t> early_haves;
	struct Req {
		uint32_t piece, begin, len;
		double at;
	};
	std::vector<Req> reqs;
	int max_reqs = 16;
	int64_t bytes_window = 0;
	double rate = 0;
	bool dead = false;
};

struct Piece {  // being downloaded: blocks collect in memory until verified
	std::string data;
	std::vector<uint8_t> got;
	std::vector<uint8_t> asked;  // outstanding requests per block
	std::vector<uint64_t> from;  // which peer sent each block (for smart ban)
	int got_count = 0;
};

// A block of a piece that failed its check: who sent it and a hash of what
// they sent. When the piece later checks out, whoever sent a different block
// sent the bad data.
struct SuspectBlock {
	int block;
	uint64_t peer;
	uint64_t hash;
};

struct Known {
	double retry_at = 0;
	int fails = 0;
	bool connected = false;
};

struct Torrent {
	std::string hex;
	uint8_t ih[20] = {};
	std::atomic<bool> stopped{false};
	double started = 0;

	std::mutex trk_m;  // trackers and tracker_peers (tracker threads)
	std::vector<std::string> trackers;
	std::vector<sockaddr_in> tracker_peers;
	std::atomic<bool> want_peers{true};
	std::atomic<bool> idle{false};
	std::vector<std::thread> tracker_threads;

	// Metadata
	bool has_meta = false;
	std::string meta;
	int64_t meta_size = 0;
	std::vector<uint8_t> meta_got;
	std::vector<double> meta_asked;
	int64_t plen = 0, total = 0;
	int npieces = 0;
	std::string hashes;
	std::vector<FileInfo> files;

	// Pieces
	std::vector<uint8_t> have, ever;
	std::map<int, Piece> active;
	int file = -1;

	// Rolling cache: slots of one piece each, in a file (or memory).
	FILE* cache = nullptr;
	std::vector<std::string> ram;  // slots in memory when the file can't be used
	int nslots = 0, slots_used = 0;
	std::vector<int> slot_of, piece_in_slot;
	std::vector<double> slot_touch;

	// Readers: id -> byte position in the torrent
	std::map<int, int64_t> readers;
	int next_reader = 1;
	double last_reader = 0;

	// Download order, refreshed when readers move
	std::vector<int> wanted;
	bool wanted_dirty = true;

	// Peers
	std::vector<std::unique_ptr<Peer>> peers;
	std::deque<sockaddr_in> candidates;
	std::map<uint64_t, Known> known;
	double next_refill = 0;
	// Smart ban: peers caught sending bad data (by IP), and the blocks of
	// failed pieces still to be judged.
	std::set<uint32_t> banned;
	std::map<int, std::vector<SuspectBlock>> suspects;

	// Stats
	int64_t bytes_window = 0;
	double rate = 0, rate_at = 0;
	double next_dht = 0, next_log = 0;

	int64_t piece_size(int p) const { return std::min<int64_t>(plen, total - int64_t(p) * plen); }
	int blocks(int p) const { return int((piece_size(p) + kBlock - 1) / kBlock); }
	~Torrent() {
		if (cache) fclose(cache);
	}
};

// ---------------------------------------------------------------------------
// The engine

namespace {

struct Impl {
	std::mutex mu;
	std::condition_variable cv;  // pieces, metadata, stops
	std::thread net;
	bool running = false, quit = false;
	std::vector<std::shared_ptr<Torrent>> torrents;
	std::string data_dir;
	int64_t cache_bytes = 1ll << 30;
	std::string peer_id;

	// DHT
	int dht_fd = -1;
	bool dht_ok = false;
	double dht_next = 0, dht_saved = 0;
	uint8_t dht_id[20] = {};
	std::mutex boot_m;
	std::vector<sockaddr_in> boot;
	std::atomic<bool> boot_started{false};

	// Incoming connections: peers behind a router can only reach us here,
	// once the router forwards the port (UPnP, in a tracker thread).
	int listen_fd = -1;
	int listen_port = 0;
	std::mutex upnp_m;
	std::string upnp_control, upnp_service;  // to remove the mapping at exit
	std::atomic<int> mapped_port{0};
	std::atomic<bool> map_started{false};
};

Impl& impl() {
	static Impl* i = new Impl();  // never destroyed: threads may outlive statics at exit
	return *i;
}

void tracker_worker(std::shared_ptr<Torrent> t, int worker);
void net_loop();

std::shared_ptr<Torrent> find_locked(const std::string& hex) {
	for (auto& t : impl().torrents)
		if (t->hex == hex && !t->stopped) return t;
	return nullptr;
}

// ---------------------------------------------------------------------------
// Messages to peers

void send_msg(Peer* p, uint8_t id, const std::string& payload = "") {
	put32(p->out, uint32_t(payload.size() + 1));
	p->out += char(id);
	p->out += payload;
}

void send_ext(Peer* p, int ext_id, const std::string& payload) {
	std::string m;
	m += char(ext_id);
	m += payload;
	send_msg(p, 20, m);
}

void send_request(Peer* p, uint32_t piece, uint32_t begin, uint32_t len, double now) {
	std::string m;
	put32(m, piece);
	put32(m, begin);
	put32(m, len);
	send_msg(p, 6, m);
	p->reqs.push_back({piece, begin, len, now});
}

void send_cancel(Peer* p, uint32_t piece, uint32_t begin, uint32_t len) {
	std::string m;
	put32(m, piece);
	put32(m, begin);
	put32(m, len);
	send_msg(p, 8, m);
}

void send_handshake(Torrent* t, Peer* p) {
	std::string h;
	h += char(19);
	h += "BitTorrent protocol";
	char reserved[8] = {0, 0, 0, 0, 0, 0x10, 0, 0};  // extension protocol
	h.append(reserved, 8);
	h.append(reinterpret_cast<const char*>(t->ih), 20);
	h += impl().peer_id;
	p->out += h;
}

void send_ext_handshake(Peer* p) {
	BValue d = BValue::dict();
	BValue m = BValue::dict();
	m.d["ut_metadata"] = BValue(int64_t(kExtMetadata));
	m.d["ut_pex"] = BValue(int64_t(kExtPex));
	d.d["m"] = m;
	d.d["v"] = BValue(std::string("Stremio PS5"));
	d.d["reqq"] = BValue(int64_t(500));
	send_ext(p, 0, bencode(d));
}

// ---------------------------------------------------------------------------
// Requests

void release_request(Torrent* t, const Peer::Req& r) {
	auto it = t->active.find(int(r.piece));
	if (it == t->active.end()) return;
	size_t b = r.begin / kBlock;
	if (b < it->second.asked.size() && it->second.asked[b] > 0) it->second.asked[b]--;
}

void release_all(Torrent* t, Peer* p) {
	for (auto& r : p->reqs) release_request(t, r);
	p->reqs.clear();
}

void drop_peer(Torrent* t, Peer* p, const char* why) {
	if (p->dead) return;
	p->dead = true;
	release_all(t, p);
	(void)why;
}

// ---------------------------------------------------------------------------
// Download order

int64_t readahead_bytes(Torrent* t) {
	int64_t cache = int64_t(t->nslots) * t->plen;
	return std::max<int64_t>(std::min<int64_t>(cache / 2, kMaxReadahead), 4 * t->plen);
}

void refresh_wanted(Torrent* t) {
	t->wanted_dirty = false;
	t->wanted.clear();
	if (!t->has_meta || t->file < 0) return;
	const FileInfo& f = t->files[size_t(t->file)];
	if (f.size <= 0) return;
	int last = int((f.offset + f.size - 1) / t->plen);
	std::vector<int64_t> starts;
	for (auto& r : t->readers) starts.push_back(r.second);
	if (starts.empty()) starts.push_back(f.offset);  // about to play from the start
	std::sort(starts.begin(), starts.end());
	int64_t ahead = readahead_bytes(t);
	std::set<int> seen;
	for (int64_t s : starts) {
		int first = int(std::max(s, f.offset) / t->plen);
		int end = int(std::min(s + ahead, f.offset + f.size - 1) / t->plen);
		end = std::min(end, last);
		for (int p = first; p <= end; p++)
			if (!t->have[size_t(p)] && seen.insert(p).second) t->wanted.push_back(p);
	}

	// After a jump the peers are still busy with blocks for the old spot:
	// call those off, so the new spot comes first, and drop the half-done
	// pieces nobody needs now (they'd hold memory and request slots).
	for (auto& up : t->peers) {
		Peer* p = up.get();
		if (p->dead) continue;
		for (size_t i = 0; i < p->reqs.size();) {
			if (seen.count(int(p->reqs[i].piece))) {
				i++;
				continue;
			}
			send_cancel(p, p->reqs[i].piece, p->reqs[i].begin, p->reqs[i].len);
			release_request(t, p->reqs[i]);
			p->reqs.erase(p->reqs.begin() + long(i));
		}
	}
	for (auto it = t->active.begin(); it != t->active.end();) {
		if (seen.count(it->first)) ++it;
		else it = t->active.erase(it);
	}
}

bool peer_has(Peer* p, int piece) { return size_t(piece) < p->has.size() && p->has[size_t(piece)]; }

void update_interest(Torrent* t, Peer* p) {
	if (!t->has_meta || p->state != Peer::Active) return;
	bool want = false;
	for (int w : t->wanted)
		if (peer_has(p, w)) {
			want = true;
			break;
		}
	// Keep interest in a peer that has anything we lack, so it may unchoke
	// us before the reader gets to its pieces.
	if (!want && p->has_count > 0) {
		for (int i = 0; i < t->npieces && !want; i++)
			if (p->has[size_t(i)] && !t->have[size_t(i)]) want = true;
	}
	if (want != p->am_interested) {
		p->am_interested = want;
		send_msg(p, want ? 2 : 3);
	}
}

void pick(Torrent* t, Peer* p, double now) {
	if (!t->has_meta || p->peer_choking || p->state != Peer::Active || p->dead) return;
	if (t->wanted_dirty) refresh_wanted(t);
	int room = std::min(p->max_reqs, p->reqq) - int(p->reqs.size());
	if (room <= 0) return;
	size_t max_active = size_t(std::max<int64_t>(4, kMaxActiveBytes / t->plen));
	for (size_t wi = 0; wi < t->wanted.size() && room > 0; wi++) {
		int piece = t->wanted[wi];
		if (t->have[size_t(piece)] || !peer_has(p, piece)) continue;
		auto it = t->active.find(piece);
		if (it == t->active.end()) {
			if (t->active.size() >= max_active && wi >= 2) continue;
			Piece pc;
			pc.data.resize(size_t(t->piece_size(piece)));
			pc.got.assign(size_t(t->blocks(piece)), 0);
			pc.asked.assign(size_t(t->blocks(piece)), 0);
			it = t->active.emplace(piece, std::move(pc)).first;
		}
		Piece& pc = it->second;
		int nb = t->blocks(piece);
		for (int b = 0; b < nb && room > 0; b++) {
			if (pc.got[size_t(b)] || pc.asked[size_t(b)]) continue;
			uint32_t begin = uint32_t(b) * kBlock;
			uint32_t len = uint32_t(std::min<int64_t>(kBlock, t->piece_size(piece) - begin));
			send_request(p, uint32_t(piece), begin, len, now);
			pc.asked[size_t(b)]++;
			room--;
		}
	}
	// The next pieces the reader needs: when every block is already asked
	// for, ask this peer too (a slow peer shouldn't hold up playback).
	for (size_t wi = 0; wi < t->wanted.size() && wi < 3 && room > 0; wi++) {
		int piece = t->wanted[wi];
		if (t->have[size_t(piece)] || !peer_has(p, piece)) continue;
		auto it = t->active.find(piece);
		if (it == t->active.end()) continue;
		Piece& pc = it->second;
		int nb = t->blocks(piece);
		for (int b = 0; b < nb && room > 0; b++) {
			if (pc.got[size_t(b)] || pc.asked[size_t(b)] != 1) continue;
			uint32_t begin = uint32_t(b) * kBlock;
			bool mine = false;
			for (auto& r : p->reqs)
				if (r.piece == uint32_t(piece) && r.begin == begin) mine = true;
			if (mine) continue;
			uint32_t len = uint32_t(std::min<int64_t>(kBlock, t->piece_size(piece) - begin));
			send_request(p, uint32_t(piece), begin, len, now);
			pc.asked[size_t(b)]++;
			room--;
		}
	}
}

// ---------------------------------------------------------------------------
// Cache

bool slot_write(Torrent* t, int slot, const std::string& data) {
	if (t->cache) {
		if (fseeko(t->cache, off_t(slot) * t->plen, SEEK_SET) == 0 &&
		    fwrite(data.data(), 1, data.size(), t->cache) == data.size() && fflush(t->cache) == 0)
			return true;
		dlog("torrent: cache file write failed (errno %d); keeping pieces in memory", errno);
		fclose(t->cache);
		t->cache = nullptr;
		// Start over in memory: what was in the file is gone.
		for (int s = 0; s < t->slots_used; s++) {
			int p = t->piece_in_slot[size_t(s)];
			if (p >= 0) t->have[size_t(p)] = 0, t->slot_of[size_t(p)] = -1;
			t->piece_in_slot[size_t(s)] = -1;
		}
		t->nslots = int(std::max<int64_t>(8, std::min<int64_t>(t->nslots, kMaxRamCache / t->plen)));
		t->slots_used = 0;
		t->piece_in_slot.assign(size_t(t->nslots), -1);
		t->slot_touch.assign(size_t(t->nslots), 0);
		t->ram.assign(size_t(t->nslots), std::string());
		t->wanted_dirty = true;
		return false;
	}
	t->ram[size_t(slot)] = data;
	return true;
}

bool slot_read(Torrent* t, int slot, int64_t off, uint8_t* buf, int n) {
	if (t->cache) {
		return fseeko(t->cache, off_t(slot) * t->plen + off, SEEK_SET) == 0 &&
		       fread(buf, 1, size_t(n), t->cache) == size_t(n);
	}
	const std::string& s = t->ram[size_t(slot)];
	if (off + n > int64_t(s.size())) return false;
	memcpy(buf, s.data() + off, size_t(n));
	return true;
}

// A free slot, or the one whose piece is least likely to be read again:
// not ahead of a reader, and used longest ago.
int take_slot(Torrent* t, double now) {
	if (t->slots_used < t->nslots) return t->slots_used++;
	int64_t ahead = readahead_bytes(t);
	int best = -1;
	double best_touch = 1e300;
	for (int s = 0; s < t->nslots; s++) {
		int p = t->piece_in_slot[size_t(s)];
		bool keep = false;
		int64_t ps = int64_t(p) * t->plen;
		for (auto& r : t->readers)
			if (ps + t->plen > r.second && ps <= r.second + ahead) keep = true;
		if (keep) continue;
		if (t->slot_touch[size_t(s)] < best_touch) best_touch = t->slot_touch[size_t(s)], best = s;
	}
	if (best < 0) {  // everything is ahead of a reader: the oldest anyway
		for (int s = 0; s < t->nslots; s++)
			if (t->slot_touch[size_t(s)] < best_touch) best_touch = t->slot_touch[size_t(s)], best = s;
	}
	int old = t->piece_in_slot[size_t(best)];
	if (old >= 0) t->have[size_t(old)] = 0, t->slot_of[size_t(old)] = -1;
	(void)now;
	return best;
}

void store_piece(Torrent* t, int piece, const std::string& data, double now) {
	int slot = take_slot(t, now);
	if (!slot_write(t, slot, data)) {
		slot = take_slot(t, now);
		slot_write(t, slot, data);
	}
	t->piece_in_slot[size_t(slot)] = piece;
	t->slot_of[size_t(piece)] = slot;
	t->slot_touch[size_t(slot)] = now;
	t->have[size_t(piece)] = 1;
	t->ever[size_t(piece)] = 1;
}

// ---------------------------------------------------------------------------
// Metadata

bool parse_info(Torrent* t, const std::string& raw) {
	BValue info;
	if (!bdecode(raw, info) || !info.is_dict()) return false;
	int64_t plen = info.get_int("piece length");
	std::string hashes = info.get_str("pieces");
	if (plen <= 0 || plen > (64ll << 20) || hashes.empty() || hashes.size() % 20) return false;
	std::string name = info.get_str("name.utf-8");
	if (name.empty()) name = info.get_str("name");
	std::vector<FileInfo> files;
	int64_t total = 0;
	if (const BValue* fl = info.get("files")) {
		if (!fl->is_list()) return false;
		for (auto& f : fl->l) {
			FileInfo fi;
			fi.size = f.get_int("length", -1);
			if (fi.size < 0) return false;
			const BValue* path = f.get("path.utf-8");
			if (!path || !path->is_list()) path = f.get("path");
			std::string p = name;
			if (path && path->is_list())
				for (auto& part : path->l)
					if (part.is_str()) p += "/" + part.s;
			fi.path = p;
			fi.offset = total;
			total += fi.size;
			files.push_back(fi);
		}
	} else {
		FileInfo fi;
		fi.size = info.get_int("length", -1);
		if (fi.size < 0) return false;
		fi.path = name;
		total = fi.size;
		files.push_back(fi);
	}
	int64_t np = (total + plen - 1) / plen;
	if (total <= 0 || np * 20 != int64_t(hashes.size())) return false;

	t->plen = plen;
	t->total = total;
	t->npieces = int(np);
	t->hashes = hashes;
	t->files = files;
	t->have.assign(size_t(np), 0);
	t->ever.assign(size_t(np), 0);
	t->slot_of.assign(size_t(np), -1);

	// The cache: as many slots as fit in the cache size (at least 8).
	Impl& I = impl();
	int64_t slots = std::max<int64_t>(8, std::min<int64_t>(np, I.cache_bytes / plen));
	t->nslots = int(slots);
	std::string path = I.data_dir + "/torrent-cache.bin";
	t->cache = fopen(path.c_str(), "w+b");
	if (!t->cache) {
		dlog("torrent: can't create %s (errno %d); keeping pieces in memory", path.c_str(), errno);
		t->nslots = int(std::max<int64_t>(8, std::min<int64_t>(slots, kMaxRamCache / plen)));
		t->ram.assign(size_t(t->nslots), std::string());
	}
	t->piece_in_slot.assign(size_t(t->nslots), -1);
	t->slot_touch.assign(size_t(t->nslots), 0);
	t->has_meta = true;
	t->wanted_dirty = true;
	dlog("torrent %s: \"%s\", %zu files, %lld bytes, %d pieces of %lld KB, cache %d pieces", t->hex.c_str(),
	     name.c_str(), files.size(), (long long)total, t->npieces, (long long)(plen / 1024), t->nslots);
	return true;
}

void apply_bitfields(Torrent* t, Peer* p) {
	p->has.assign(size_t(t->npieces), 0);
	p->has_count = 0;
	if (p->has_all_early) {
		std::fill(p->has.begin(), p->has.end(), 1);
		p->has_count = t->npieces;
	} else {
		for (int i = 0; i < t->npieces && size_t(i / 8) < p->early_bitfield.size(); i++)
			if (uint8_t(p->early_bitfield[size_t(i / 8)]) & (0x80 >> (i % 8))) p->has[size_t(i)] = 1, p->has_count++;
		for (uint32_t h : p->early_haves)
			if (h < uint32_t(t->npieces) && !p->has[h]) p->has[h] = 1, p->has_count++;
	}
	p->early_bitfield.clear();
	p->early_haves.clear();
}

void metadata_done(Torrent* t) {
	unsigned char digest[20];
	SHA1(reinterpret_cast<const unsigned char*>(t->meta.data()), t->meta.size(), digest);
	if (memcmp(digest, t->ih, 20) != 0 || !parse_info(t, t->meta)) {
		dlog("torrent %s: metadata from peers didn't check out, fetching again", t->hex.c_str());
		std::fill(t->meta_got.begin(), t->meta_got.end(), 0);
		std::fill(t->meta_asked.begin(), t->meta_asked.end(), 0);
		return;
	}
	make_dirs(impl().data_dir + "/torrents");
	write_file(impl().data_dir + "/torrents/" + t->hex + ".info", t->meta);
	for (auto& p : t->peers)
		if (p->state == Peer::Active) apply_bitfields(t, p.get());
	impl().cv.notify_all();
}

void request_metadata(Torrent* t, Peer* p, double now) {
	if (t->has_meta || !p->ut_metadata || p->metadata_size <= 0 || p->metadata_size > (32 << 20)) return;
	if (t->meta_size == 0) {
		t->meta_size = p->metadata_size;
		t->meta.assign(size_t(t->meta_size), '\0');
		size_t n = size_t((t->meta_size + kBlock - 1) / kBlock);
		t->meta_got.assign(n, 0);
		t->meta_asked.assign(n, 0);
	}
	if (p->metadata_size != t->meta_size) return;
	for (size_t i = 0; i < t->meta_got.size(); i++) {
		if (t->meta_got[i] || now - t->meta_asked[i] < 8) continue;
		BValue d = BValue::dict();
		d.d["msg_type"] = BValue(int64_t(0));
		d.d["piece"] = BValue(int64_t(i));
		send_ext(p, p->ut_metadata, bencode(d));
		t->meta_asked[i] = now;
	}
}

void on_metadata_msg(Torrent* t, Peer* p, const char* data, size_t n, double now) {
	BValue d;
	size_t used = 0;
	if (!bdecode(data, n, d, &used) || !d.is_dict()) return;
	int64_t type = d.get_int("msg_type", -1);
	int64_t piece = d.get_int("piece", -1);
	if (type == 1 && !t->has_meta && piece >= 0 && size_t(piece) < t->meta_got.size()) {
		size_t off = size_t(piece) * kBlock;
		size_t want = size_t(std::min<int64_t>(kBlock, t->meta_size - int64_t(off)));
		if (n - used != want || t->meta_got[size_t(piece)]) return;
		memcpy(&t->meta[off], data + used, want);
		t->meta_got[size_t(piece)] = 1;
		if (std::all_of(t->meta_got.begin(), t->meta_got.end(), [](uint8_t g) { return g != 0; })) metadata_done(t);
	} else if (type == 2 && piece >= 0 && size_t(piece) < t->meta_asked.size()) {
		t->meta_asked[size_t(piece)] = 0;  // rejected: ask someone else
	} else if (type == 0) {
		// We don't serve metadata.
		BValue r = BValue::dict();
		r.d["msg_type"] = BValue(int64_t(2));
		r.d["piece"] = BValue(piece);
		if (p->ut_metadata) send_ext(p, p->ut_metadata, bencode(r));
	}
	(void)now;
}

// ---------------------------------------------------------------------------
// Peer messages

uint32_t key_ip(uint64_t key) { return uint32_t(key >> 16); }

uint64_t block_hash(const char* p, size_t n) {
	uint64_t h = 1469598103934665603ull;
	for (size_t i = 0; i < n; i++) h = (h ^ uint8_t(p[i])) * 1099511628211ull;
	return h;
}

// Never again: the peer sent data that failed the check.
void ban_peer(Torrent* t, uint64_t key, const char* why) {
	if (!key || !t->banned.insert(key_ip(key)).second) return;
	sockaddr_in a{};
	a.sin_addr.s_addr = htonl(key_ip(key));
	a.sin_port = htons(uint16_t(key & 0xffff));
	dlog("torrent %s: banned %s (%s)", t->hex.c_str(), addr_str(a).c_str(), why);
	for (auto& p : t->peers)
		if (key_ip(p->key) == key_ip(key)) drop_peer(t, p.get(), "banned");
}

void add_candidates(Torrent* t, const std::vector<sockaddr_in>& addrs) {
	for (auto& a : addrs) {
		uint64_t k = addr_key(a);
		if (t->known.count(k) || t->banned.count(key_ip(k))) continue;
		t->known[k] = Known();
		t->candidates.push_back(a);
	}
}

void on_block(Torrent* t, Peer* p, uint32_t piece, uint32_t begin, const char* data, size_t len, double now) {
	for (size_t i = 0; i < p->reqs.size(); i++)
		if (p->reqs[i].piece == piece && p->reqs[i].begin == begin) {
			p->reqs.erase(p->reqs.begin() + long(i));
			break;
		}
	p->got_any = true;
	p->bytes_window += int64_t(len);
	t->bytes_window += int64_t(len);
	if (!t->has_meta || piece >= uint32_t(t->npieces) || begin % kBlock) return;
	auto it = t->active.find(int(piece));
	if (it == t->active.end()) return;
	Piece& pc = it->second;
	size_t b = begin / kBlock;
	if (b >= pc.got.size() || pc.got[b]) return;
	if (int64_t(begin) + int64_t(len) > int64_t(pc.data.size()) ||
	    int64_t(len) != std::min<int64_t>(kBlock, t->piece_size(int(piece)) - begin))
		return;
	memcpy(&pc.data[begin], data, len);
	pc.got[b] = 1;
	pc.got_count++;
	pc.asked[b] = 0;
	if (pc.from.size() != pc.got.size()) pc.from.assign(pc.got.size(), 0);
	pc.from[b] = p->key;
	// Others asked for the same block (see pick): call that off.
	for (auto& q : t->peers) {
		if (q.get() == p || q->dead) continue;
		for (size_t i = 0; i < q->reqs.size(); i++)
			if (q->reqs[i].piece == piece && q->reqs[i].begin == begin) {
				send_cancel(q.get(), piece, begin, q->reqs[i].len);
				q->reqs.erase(q->reqs.begin() + long(i));
				break;
			}
	}
	if (pc.got_count < int(pc.got.size())) return;

	unsigned char digest[20];
	SHA1(reinterpret_cast<const unsigned char*>(pc.data.data()), pc.data.size(), digest);
	if (memcmp(digest, t->hashes.data() + size_t(piece) * 20, 20) != 0) {
		dlog("torrent %s: piece %u failed its check, downloading it again", t->hex.c_str(), piece);
		// Who sent what, to judge once a good copy arrives. All of it from
		// one peer: that peer, right away.
		std::vector<SuspectBlock>& sus = t->suspects[int(piece)];
		sus.clear();
		bool one_sender = true;
		for (size_t i = 0; i < pc.from.size(); i++) {
			size_t off = i * kBlock, n = std::min<size_t>(kBlock, pc.data.size() - off);
			sus.push_back({int(i), pc.from[i], block_hash(pc.data.data() + off, n)});
			if (pc.from[i] != pc.from[0]) one_sender = false;
		}
		if (one_sender && !pc.from.empty()) ban_peer(t, pc.from[0], "a whole piece of bad data");
		std::fill(pc.got.begin(), pc.got.end(), 0);
		std::fill(pc.asked.begin(), pc.asked.end(), 0);
		std::fill(pc.from.begin(), pc.from.end(), 0);
		pc.got_count = 0;
		return;
	}
	// Checked out: anyone whose earlier copy of a block differs sent bad data.
	auto sus = t->suspects.find(int(piece));
	if (sus != t->suspects.end()) {
		for (auto& s : sus->second) {
			size_t off = size_t(s.block) * kBlock, n = std::min<size_t>(kBlock, pc.data.size() - off);
			if (off < pc.data.size() && block_hash(pc.data.data() + off, n) != s.hash)
				ban_peer(t, s.peer, "bad block in a failed piece");
		}
		t->suspects.erase(sus);
	}
	std::string done = std::move(pc.data);
	t->active.erase(it);
	store_piece(t, int(piece), done, now);
	t->wanted_dirty = true;
	impl().cv.notify_all();
}

void on_ext_handshake(Torrent* t, Peer* p, const char* data, size_t n, double now) {
	BValue d;
	if (!bdecode(data, n, d) || !d.is_dict()) return;
	if (const BValue* m = d.get("m")) {
		p->ut_metadata = int(m->get_int("ut_metadata", 0));
		p->ut_pex = int(m->get_int("ut_pex", 0));
	}
	p->metadata_size = d.get_int("metadata_size", 0);
	p->reqq = int(std::max<int64_t>(4, std::min<int64_t>(d.get_int("reqq", 250), 1000)));
	request_metadata(t, p, now);
}

void on_pex(Torrent* t, const char* data, size_t n) {
	BValue d;
	if (!bdecode(data, n, d) || !d.is_dict()) return;
	std::vector<sockaddr_in> v;
	parse_compact(d.get_str("added"), v);
	add_candidates(t, v);
}

void on_message(Torrent* t, Peer* p, uint8_t id, const char* pl, size_t n, double now) {
	switch (id) {
	case 0:  // choke
		if (!p->peer_choking) p->choked_since = now;
		p->peer_choking = true;
		release_all(t, p);
		break;
	case 1:  // unchoke
		p->peer_choking = false;
		pick(t, p, now);
		break;
	case 4:  // have
		if (n >= 4) {
			uint32_t idx = be32(pl);
			if (t->has_meta) {
				if (idx < uint32_t(t->npieces) && !p->has[idx]) {
					p->has[idx] = 1;
					p->has_count++;
					if (!p->am_interested) update_interest(t, p);
					if (!p->peer_choking) pick(t, p, now);
				}
			} else if (p->early_haves.size() < 1u << 20) {
				p->early_haves.push_back(idx);
			}
		}
		break;
	case 5:  // bitfield
		p->early_bitfield.assign(pl, n);
		if (t->has_meta) {
			apply_bitfields(t, p);
			update_interest(t, p);
		}
		break;
	case 7:  // piece
		if (n >= 8) on_block(t, p, be32(pl), be32(pl + 4), pl + 8, n - 8, now);
		if (!p->dead) pick(t, p, now);
		break;
	case 14:  // have all (fast extension)
		p->has_all_early = true;
		if (t->has_meta) {
			apply_bitfields(t, p);
			update_interest(t, p);
		}
		break;
	case 20:  // extended
		if (n < 1) break;
		if (pl[0] == 0) on_ext_handshake(t, p, pl + 1, n - 1, now);
		else if (uint8_t(pl[0]) == kExtMetadata) on_metadata_msg(t, p, pl + 1, n - 1, now);
		else if (uint8_t(pl[0]) == kExtPex) on_pex(t, pl + 1, n - 1);
		break;
	default:  // interested, not interested, request, cancel, port, ...: we don't upload
		break;
	}
}

void process_input(Torrent* t, Peer* p, double now) {
	if (p->state == Peer::Handshaking) {
		if (p->in.size() - p->in_off < 68) return;
		const char* h = p->in.data() + p->in_off;
		if (h[0] != 19 || memcmp(h + 1, "BitTorrent protocol", 19) != 0 || memcmp(h + 28, t->ih, 20) != 0) {
			drop_peer(t, p, "bad handshake");
			return;
		}
		p->ext = (uint8_t(h[25]) & 0x10) != 0;
		p->in_off += 68;
		p->state = Peer::Active;
		p->last_data = now;
		p->choked_since = now;
		if (p->incoming) send_handshake(t, p);
		if (p->ext) send_ext_handshake(p);
		if (t->has_meta) apply_bitfields(t, p);
	}
	while (!p->dead) {
		size_t avail = p->in.size() - p->in_off;
		if (avail < 4) break;
		uint32_t len = be32(p->in.data() + p->in_off);
		if (len > kMaxMessage) {
			drop_peer(t, p, "message too big");
			return;
		}
		if (avail < 4 + size_t(len)) break;
		const char* m = p->in.data() + p->in_off + 4;
		p->in_off += 4 + len;
		p->last_data = now;
		if (len > 0) on_message(t, p, uint8_t(m[0]), m + 1, len - 1, now);
	}
	if (p->in_off == p->in.size()) {
		p->in.clear();
		p->in_off = 0;
	} else if (p->in_off > (256u << 10)) {
		p->in.erase(0, p->in_off);
		p->in_off = 0;
	}
}

// ---------------------------------------------------------------------------
// Connections

void connect_peer(Torrent* t, const sockaddr_in& a, double now) {
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return;
	set_nonblocking(fd);
#ifdef SO_NOSIGPIPE
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
	int rcv = 512 * 1024;
	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
	auto p = std::make_unique<Peer>();
	p->fd = fd;
	p->addr = a;
	p->key = addr_key(a);
	p->since = now;
	t->known[p->key].connected = true;
	int r = connect(fd, reinterpret_cast<const sockaddr*>(&a), sizeof(a));
	if (r == 0) {
		p->state = Peer::Handshaking;
		send_handshake(t, p.get());
	} else if (errno == EINPROGRESS || errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR) {
		p->state = Peer::Connecting;
	} else {
		static int logged = 0;
		if (logged++ < 3) dlog("torrent: connect to %s failed at once (errno %d)", addr_str(a).c_str(), errno);
		close(fd);
		Known& k = t->known[p->key];
		k.connected = false;
		k.fails++;
		k.retry_at = now + 60;
		return;
	}
	t->peers.push_back(std::move(p));
}

// The port peers can reach us on (told to trackers and the DHT).
int announce_port() {
	int p = impl().listen_port;
	return p > 0 ? p : 6881;
}

void open_listener() {
	Impl& I = impl();
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		dlog("torrent: no listening socket (errno %d)", errno);
		return;
	}
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	sockaddr_in a{};
	a.sin_family = AF_INET;
	int port = 0;
	for (int p = 6881; p <= 6889 && !port; p++) {
		a.sin_port = htons(uint16_t(p));
		if (bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) port = p;
	}
	if (!port || listen(fd, 32) != 0) {
		dlog("torrent: can't accept connections (errno %d)", errno);
		close(fd);
		return;
	}
	set_nonblocking(fd);
	I.listen_fd = fd;
	I.listen_port = port;
	dlog("torrent: accepting peers on port %d", port);
}

// Peers that connected to us join the torrent being played (one at a time).
void accept_peers(double now) {
	Impl& I = impl();
	for (int i = 0; i < 16; i++) {
		if (i > 0 && !readable_now(I.listen_fd)) break;
		sockaddr_in a{};
		socklen_t len = sizeof(a);
		int fd = accept(I.listen_fd, reinterpret_cast<sockaddr*>(&a), &len);
		if (fd < 0) break;
		Torrent* t = nullptr;
		for (auto& x : I.torrents)
			if (!x->stopped && !x->idle) t = x.get();
		uint64_t key = addr_key(a);
		if (!t || int(t->peers.size()) >= kMaxPeers + 20 || t->known[key].connected) {
			close(fd);
			continue;
		}
		set_nonblocking(fd);
		auto p = std::make_unique<Peer>();
		p->fd = fd;
		p->addr = a;
		p->key = key;
		p->since = now;
		p->incoming = true;
		p->state = Peer::Handshaking;
		t->known[key].connected = true;
		static int logged = 0;
		if (logged++ < 3) dlog("torrent: %s connected to us", addr_str(a).c_str());
		t->peers.push_back(std::move(p));
	}
}

void on_writable(Torrent* t, Peer* p, double now) {
	if (p->state == Peer::Connecting) {
		int err = 0;
		socklen_t len = sizeof(err);
		getsockopt(p->fd, SOL_SOCKET, SO_ERROR, &err, &len);
		if (err) {
			static int logged = 0;
			if (logged++ < 3) dlog("torrent: connect to %s failed (error %d)", addr_str(p->addr).c_str(), err);
			drop_peer(t, p, "connect failed");
			return;
		}
		p->state = Peer::Handshaking;
		p->since = now;
		send_handshake(t, p);
	}
	for (int i = 0; !p->out.empty(); i++) {
		if (i > 0) {  // as readable_now: never wait for room
			pollfd pf{p->fd, POLLOUT, 0};
			if (poll(&pf, 1, 0) <= 0 || !(pf.revents & POLLOUT)) break;
		}
		ssize_t n = send(p->fd, p->out.data(), p->out.size(), MSG_NOSIGNAL);
		if (n > 0) {
			p->out.erase(0, size_t(n));
			p->last_send = now;
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) break;
		drop_peer(t, p, "send failed");
		return;
	}
}

void on_readable(Torrent* t, Peer* p, double now) {
	char buf[64 * 1024];
	for (int i = 0; i < 16 && !p->dead; i++) {
		if (i > 0 && !readable_now(p->fd)) break;
		ssize_t n = recv(p->fd, buf, sizeof(buf), 0);
		if (n > 0) {
			p->in.append(buf, size_t(n));
			if (n < ssize_t(sizeof(buf))) break;
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) break;
		drop_peer(t, p, n == 0 ? "closed" : "recv failed");
		return;
	}
	process_input(t, p, now);
}

void close_peer(Torrent* t, Peer* p, double now) {
	if (p->fd >= 0) close(p->fd);
	p->fd = -1;
	Known& k = t->known[p->key];
	k.connected = false;
	if (p->state == Peer::Active && p->got_any) {
		k.fails = 0;
		k.retry_at = now + 30;
	} else {
		k.fails++;
		k.retry_at = now + std::min(600.0, 30.0 * k.fails);
	}
}

// ---------------------------------------------------------------------------
// Upkeep, a few times a second

void upkeep(Torrent* t, double now) {
	// Peers from the trackers
	{
		std::lock_guard<std::mutex> lock(t->trk_m);
		if (!t->tracker_peers.empty()) {
			add_candidates(t, t->tracker_peers);
			t->tracker_peers.clear();
		}
	}

	if (t->wanted_dirty) refresh_wanted(t);
	bool reading = !t->readers.empty();
	if (reading) t->last_reader = now;
	bool idle = t->has_meta && !reading && now - std::max(t->last_reader, t->started) > kPauseAfter;
	if (idle != t->idle) {
		t->idle = idle;
		dlog("torrent %s: %s", t->hex.c_str(), idle ? "nobody is watching, pausing" : "resuming");
	}

	int active = 0, connecting = 0;
	for (auto& up : t->peers) {
		Peer* p = up.get();
		if (p->dead) continue;
		if (idle) {
			drop_peer(t, p, "paused");
			continue;
		}
		if (p->state == Peer::Connecting) {
			connecting++;
			if (now - p->since > kConnectTimeout) drop_peer(t, p, "connect timeout");
			continue;
		}
		if (p->state == Peer::Handshaking) {
			if (now - p->since > kHandshakeTimeout) drop_peer(t, p, "handshake timeout");
			continue;
		}
		active++;
		// Late blocks: ask someone else, and expect less from this peer.
		bool late = false;
		for (size_t i = 0; i < p->reqs.size();) {
			if (now - p->reqs[i].at > kRequestTimeout) {
				release_request(t, p->reqs[i]);
				p->reqs.erase(p->reqs.begin() + long(i));
				late = true;
			} else {
				i++;
			}
		}
		if (late) p->max_reqs = std::max(4, p->max_reqs / 2);
		// Peers that keep us waiting make room for others, when there are others.
		bool others = !t->candidates.empty();
		if (others && p->peer_choking && p->am_interested && now - p->choked_since > kChokedTimeout) {
			drop_peer(t, p, "kept us choked");
			continue;
		}
		if (others && !t->has_meta && !p->ut_metadata && now - p->since > 20) {
			drop_peer(t, p, "can't send metadata");
			continue;
		}
		if (now - p->last_data > 180) {
			drop_peer(t, p, "silent");
			continue;
		}
		if (!t->has_meta) request_metadata(t, p, now);
		else {
			update_interest(t, p);
			pick(t, p, now);
		}
		if (p->out.empty() && now - p->last_send > 90) put32(p->out, 0);  // keep-alive
	}

	// More peers
	if (!idle) {
		if (t->candidates.empty() && now >= t->next_refill) {
			t->next_refill = now + 5;
			for (auto& k : t->known) {
				if (k.second.connected || k.second.retry_at > now) continue;
				sockaddr_in a{};
				a.sin_family = AF_INET;
				a.sin_addr.s_addr = htonl(uint32_t(k.first >> 16));
				a.sin_port = htons(uint16_t(k.first & 0xffff));
				t->candidates.push_back(a);
				if (t->candidates.size() >= 200) break;
			}
		}
		while (active + connecting < kMaxPeers && connecting < kMaxConnecting && !t->candidates.empty()) {
			sockaddr_in a = t->candidates.front();
			t->candidates.pop_front();
			Known& k = t->known[addr_key(a)];
			if (k.connected || k.retry_at > now || t->banned.count(key_ip(addr_key(a)))) continue;
			connect_peer(t, a, now);
			connecting++;
		}
	}
	t->want_peers = !idle && active < kMaxPeers / 2;

	// Speeds, once a second
	if (now - t->rate_at >= 1) {
		double dt = now - t->rate_at;
		t->rate = t->rate_at > 0 ? t->bytes_window / dt : 0;
		t->bytes_window = 0;
		t->rate_at = now;
		for (auto& up : t->peers) {
			Peer* p = up.get();
			p->rate = p->bytes_window / dt;
			p->bytes_window = 0;
			// Enough requests in flight for about two seconds of its speed.
			int want = int(p->rate * 2 / kBlock) + 4;
			p->max_reqs = std::max(4, std::min({want, 250, p->reqq}));
		}
		if (now >= t->next_log) {
			t->next_log = now + 10;
			int have = 0;
			for (uint8_t h : t->have) have += h;
			dlog("torrent %s: %d peers (%d connecting, %zu known), %.2f MB/s, %d pieces cached, %zu active",
			     t->hex.c_str(), active, connecting, t->known.size(), t->rate / (1024 * 1024), have, t->active.size());
		}
	}
}

// ---------------------------------------------------------------------------
// DHT

#ifdef HAVE_DHT
void dht_event(void* closure, int event, const unsigned char* info_hash, const void* data, size_t len) {
	(void)closure;
	if (event != DHT_EVENT_VALUES) return;
	std::vector<sockaddr_in> v;
	parse_compact(std::string(static_cast<const char*>(data), len), v);
	for (auto& t : impl().torrents)
		if (!t->stopped && memcmp(t->ih, info_hash, 20) == 0) add_candidates(t.get(), v);
}

void dht_start() {
	Impl& I = impl();
	I.dht_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (I.dht_fd < 0) {
		dlog("dht: no UDP socket (errno %d)", errno);
		return;
	}
	sockaddr_in any{};
	any.sin_family = AF_INET;
	// The same port number as the peer listener, so one router mapping
	// covers both; any port if that's taken.
	any.sin_port = htons(uint16_t(I.listen_port));
	bool bound = I.listen_port && bind(I.dht_fd, reinterpret_cast<sockaddr*>(&any), sizeof(any)) == 0;
	if (!bound) {
		any.sin_port = 0;
		bound = bind(I.dht_fd, reinterpret_cast<sockaddr*>(&any), sizeof(any)) == 0;
	}
	if (!bound) {
		dlog("dht: bind failed (errno %d)", errno);
		close(I.dht_fd);
		I.dht_fd = -1;
		return;
	}
	set_nonblocking(I.dht_fd);
	std::string saved;
	std::string path = I.data_dir + "/dht.dat";
	if (read_file(path, saved) && saved.size() >= 20) memcpy(I.dht_id, saved.data(), 20);
	else random_bytes(I.dht_id, 20);
	const unsigned char v[4] = {'S', 'P', 0, 1};
	if (dht_init(I.dht_fd, -1, I.dht_id, v) < 0) {
		dlog("dht: init failed");
		close(I.dht_fd);
		I.dht_fd = -1;
		return;
	}
	I.dht_ok = true;
	std::vector<sockaddr_in> nodes;
	if (saved.size() > 20) parse_compact(saved.substr(20), nodes);
	for (auto& a : nodes) dht_ping_node(reinterpret_cast<sockaddr*>(&a), sizeof(a));
	dlog("dht: started, %zu saved nodes", nodes.size());
	I.dht_saved = now_seconds();
}

void dht_save() {
	Impl& I = impl();
	if (!I.dht_ok) return;
	sockaddr_in sins[300];
	int num = 300, num6 = 0;
	dht_get_nodes(sins, &num, nullptr, &num6);
	std::string out(reinterpret_cast<const char*>(I.dht_id), 20);
	for (int i = 0; i < num; i++) {
		out.append(reinterpret_cast<const char*>(&sins[i].sin_addr.s_addr), 4);
		out.append(reinterpret_cast<const char*>(&sins[i].sin_port), 2);
	}
	write_file(I.data_dir + "/dht.dat", out);
}

void dht_poll(bool readable, double now) {
	Impl& I = impl();
	if (!I.dht_ok) return;
	{
		std::lock_guard<std::mutex> lock(I.boot_m);
		for (auto& a : I.boot) dht_ping_node(reinterpret_cast<sockaddr*>(&a), sizeof(a));
		I.boot.clear();
	}
	time_t tosleep = 1;
	if (readable) {
		char buf[4096];
		for (int i = 0; i < 64; i++) {
			if (i > 0 && !readable_now(I.dht_fd)) break;
			sockaddr_storage from{};
			socklen_t fl = sizeof(from);
			ssize_t n = recvfrom(I.dht_fd, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&from), &fl);
			if (n <= 0) break;
			buf[n] = 0;  // dht_periodic wants it terminated
			dht_periodic(buf, size_t(n), reinterpret_cast<sockaddr*>(&from), int(fl), &tosleep, dht_event, nullptr);
		}
		I.dht_next = now + double(tosleep);
	}
	if (now >= I.dht_next) {
		dht_periodic(nullptr, 0, nullptr, 0, &tosleep, dht_event, nullptr);
		I.dht_next = now + double(tosleep);
	}
	for (auto& t : I.torrents) {
		if (t->stopped || t->idle || now < t->next_dht) continue;
		int good = 0, dubious = 0, cached = 0, incoming = 0;
		dht_nodes(AF_INET, &good, &dubious, &cached, &incoming);
		if (good + dubious < 4) {
			t->next_dht = now + 3;  // still bootstrapping
			continue;
		}
		// With the port open on the router, also tell the DHT we have it,
		// so peers that can't be reached come to us.
		if (dht_search(t->ih, I.mapped_port.load(), AF_INET, dht_event, nullptr) >= 0) {
			t->next_dht = now + (t->want_peers ? 45 : 300);
		} else {
			t->next_dht = now + 10;
		}
	}
	if (now - I.dht_saved > 600) {
		I.dht_saved = now;
		dht_save();
	}
}
#endif

void resolve_bootstrap() {
	std::vector<sockaddr_in> found;
	for (auto& h : kDhtBootstrap) {
		addrinfo hints{}, *res = nullptr;
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_DGRAM;
		if (getaddrinfo(h[0], h[1], &hints, &res) != 0 || !res) continue;
		for (addrinfo* r = res; r; r = r->ai_next)
			if (r->ai_family == AF_INET) found.push_back(*reinterpret_cast<sockaddr_in*>(r->ai_addr));
		freeaddrinfo(res);
	}
	std::lock_guard<std::mutex> lock(impl().boot_m);
	impl().boot.insert(impl().boot.end(), found.begin(), found.end());
}

// ---------------------------------------------------------------------------
// Network thread

void net_loop() {
	Impl& I = impl();
	{
		std::lock_guard<std::mutex> lock(I.mu);
#ifdef HAVE_DHT
		dht_start();
#endif
	}
	std::vector<pollfd> fds;
	std::vector<std::pair<Torrent*, Peer*>> who;
	double next_upkeep = 0;
	for (;;) {
		fds.clear();
		who.clear();
		{
			std::lock_guard<std::mutex> lock(I.mu);
			if (I.quit) break;
			if (I.dht_fd >= 0) fds.push_back({I.dht_fd, POLLIN, 0});
			if (I.listen_fd >= 0) fds.push_back({I.listen_fd, POLLIN, 0});
			for (auto& t : I.torrents)
				for (auto& p : t->peers) {
					if (p->dead || p->fd < 0) continue;
					short ev = 0;
					if (p->state == Peer::Connecting) ev = POLLOUT;
					else ev = short(POLLIN | (p->out.empty() ? 0 : POLLOUT));
					fds.push_back({p->fd, ev, 0});
					who.push_back({t.get(), p.get()});
				}
		}
		int n = poll(fds.data(), nfds_t(fds.size()), 50);
		if (n < 0 && errno != EINTR) usleep(20000);

		std::lock_guard<std::mutex> lock(I.mu);
		if (I.quit) break;
		double now = now_seconds();
		size_t base = 0;
		bool dht_in = false, listen_in = false;
		if (I.dht_fd >= 0) dht_in = n > 0 && (fds[base++].revents & POLLIN);
		if (I.listen_fd >= 0) listen_in = n > 0 && (fds[base++].revents & POLLIN);
#ifdef HAVE_DHT
		dht_poll(dht_in, now);
#else
		(void)dht_in;
#endif
		if (listen_in) accept_peers(now);
		for (size_t i = 0; i < who.size(); i++) {
			short re = fds[base + i].revents;
			Torrent* t = who[i].first;
			Peer* p = who[i].second;
			if (!re || p->dead) continue;
			if (re & POLLOUT) on_writable(t, p, now);
			if (!p->dead && (re & POLLIN)) on_readable(t, p, now);
			if (!p->dead && (re & (POLLERR | POLLHUP | POLLNVAL)) && !(re & POLLIN)) drop_peer(t, p, "socket error");
			if (!p->dead && !p->out.empty()) on_writable(t, p, now);
		}
		if (now >= next_upkeep) {
			next_upkeep = now + 0.25;
			for (auto& t : I.torrents)
				if (!t->stopped) upkeep(t.get(), now);
		}
		// Flush what upkeep and the handlers queued
		for (auto& t : I.torrents)
			for (auto& p : t->peers)
				if (!p->dead && p->state != Peer::Connecting && !p->out.empty()) on_writable(t.get(), p.get(), now);
		// Remove closed peers and stopped torrents
		for (auto& t : I.torrents) {
			auto& v = t->peers;
			for (auto& p : v)
				if ((p->dead || t->stopped) && p->fd >= 0) {
					if (!p->dead) drop_peer(t.get(), p.get(), "stopped");
					close_peer(t.get(), p.get(), now);
				}
			v.erase(std::remove_if(v.begin(), v.end(), [](const std::unique_ptr<Peer>& p) { return p->fd < 0; }),
			        v.end());
		}
		for (auto it = I.torrents.begin(); it != I.torrents.end();) {
			if ((*it)->stopped) {
				for (auto& th : (*it)->tracker_threads)
					if (th.joinable()) th.join();
				dlog("torrent %s: stopped", (*it)->hex.c_str());
				it = I.torrents.erase(it);
			} else {
				++it;
			}
		}
	}
	// Quitting
	std::lock_guard<std::mutex> lock(I.mu);
	for (auto& t : I.torrents) {
		t->stopped = true;
		for (auto& p : t->peers)
			if (p->fd >= 0) close(p->fd), p->fd = -1;
		for (auto& th : t->tracker_threads)
			if (th.joinable()) th.join();
	}
	I.torrents.clear();
#ifdef HAVE_DHT
	if (I.dht_ok) {
		dht_save();
		dht_uninit();
		I.dht_ok = false;
	}
#endif
	if (I.dht_fd >= 0) close(I.dht_fd), I.dht_fd = -1;
	I.cv.notify_all();
}

// ---------------------------------------------------------------------------
// Trackers (their own threads: HTTP and DNS block)

bool wait_stoppable(Torrent* t, double seconds) {
	double until = now_seconds() + seconds;
	while (now_seconds() < until) {
		if (t->stopped) return false;
		usleep(250 * 1000);
	}
	return !t->stopped;
}

int64_t bytes_left(Torrent* t) { return t->has_meta ? t->total : 1ll << 30; }

bool announce_http(Torrent* t, const std::string& url, bool first, std::vector<sockaddr_in>& peers, int* interval) {
	std::string u = url + (url.find('?') == std::string::npos ? "?" : "&");
	u += "info_hash=" + pct_encode(std::string(reinterpret_cast<const char*>(t->ih), 20));
	u += "&peer_id=" + pct_encode(impl().peer_id);
	u += "&port=" + std::to_string(announce_port()) + "&uploaded=0&downloaded=0&left=" + std::to_string(bytes_left(t));
	u += "&compact=1&numwant=200";
	if (first) u += "&event=started";
	HttpResponse r = http_get(u, 15, &t->stopped);
	if (!r.ok()) return false;
	BValue d;
	if (!bdecode(r.body, d) || !d.is_dict() || d.get("failure reason")) return false;
	if (const BValue* p = d.get("peers")) {
		if (p->is_str()) {
			parse_compact(p->s, peers);
		} else if (p->is_list()) {
			for (auto& e : p->l) {
				sockaddr_in a{};
				a.sin_family = AF_INET;
				if (inet_pton(AF_INET, e.get_str("ip").c_str(), &a.sin_addr) != 1) continue;
				a.sin_port = htons(uint16_t(e.get_int("port")));
				if (usable(a)) peers.push_back(a);
			}
		}
	}
	*interval = int(d.get_int("interval", 0));
	return true;
}

bool udp_exchange(Torrent* t, int fd, const std::string& req, std::string& resp, uint32_t tid) {
	for (int attempt = 0; attempt < 2; attempt++) {
		if (send(fd, req.data(), req.size(), 0) < 0) return false;
		double until = now_seconds() + 4 + attempt * 4;
		while (now_seconds() < until) {
			if (t->stopped) return false;
			pollfd pf{fd, POLLIN, 0};
			if (poll(&pf, 1, 250) <= 0) continue;
			char buf[4096];
			ssize_t n = recv(fd, buf, sizeof(buf), 0);
			if (n < 8) continue;
			if (be32(buf + 4) != tid) continue;
			resp.assign(buf, size_t(n));
			return true;
		}
	}
	return false;
}

bool announce_udp(Torrent* t, const std::string& url, bool first, std::vector<sockaddr_in>& peers, int* interval) {
	// udp://host:port[/announce]
	std::string rest = url.substr(6);
	size_t slash = rest.find('/');
	if (slash != std::string::npos) rest = rest.substr(0, slash);
	size_t colon = rest.rfind(':');
	if (colon == std::string::npos) return false;
	std::string host = rest.substr(0, colon), port = rest.substr(colon + 1);
	addrinfo hints{}, *res = nullptr;
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) return false;
	sockaddr_in to = *reinterpret_cast<sockaddr_in*>(res->ai_addr);
	freeaddrinfo(res);
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) return false;
	bool ok = false;
	if (connect(fd, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == 0) {
		uint32_t tid;
		random_bytes(&tid, 4);
		std::string req;
		put64(req, 0x41727101980ull);
		put32(req, 0);  // connect
		put32(req, tid);
		std::string resp;
		if (udp_exchange(t, fd, req, resp, tid) && resp.size() >= 16 && be32(resp.data()) == 0) {
			std::string conn = resp.substr(8, 8);
			random_bytes(&tid, 4);
			uint32_t key;
			random_bytes(&key, 4);
			req.clear();
			req += conn;
			put32(req, 1);  // announce
			put32(req, tid);
			req.append(reinterpret_cast<const char*>(t->ih), 20);
			req += impl().peer_id;
			put64(req, 0);                       // downloaded
			put64(req, uint64_t(bytes_left(t)));  // left
			put64(req, 0);                       // uploaded
			put32(req, first ? 2 : 0);           // event: started / none
			put32(req, 0);                       // ip
			put32(req, key);
			put32(req, 200);  // num want
			int port = announce_port();
			req += char(port >> 8), req += char(port & 0xff);
			if (udp_exchange(t, fd, req, resp, tid) && resp.size() >= 20 && be32(resp.data()) == 1) {
				*interval = int(be32(resp.data() + 8));
				parse_compact(resp.substr(20), peers);
				ok = true;
			}
		}
	}
	close(fd);
	return ok;
}

// Asks the router (UPnP) to forward our port to the PS5, once per run, so
// peers behind their own routers can connect to us.
void map_port() {
#ifdef HAVE_UPNP
	Impl& I = impl();
	int port = I.listen_port;
	if (!port) return;
	int err = 0;
	UPNPDev* devs = upnpDiscover(2000, nullptr, nullptr, 0, 0, 2, &err);
	if (!devs) {
		dlog("upnp: no router answered (error %d); only outgoing connections", err);
		return;
	}
	UPNPUrls urls;
	IGDdatas data;
	char lan[64] = "";
	int r = UPNP_GetValidIGD(devs, &urls, &data, lan, sizeof(lan));
	freeUPNPDevlist(devs);
	if (r != 1) {
		dlog("upnp: no usable router (%d); only outgoing connections", r);
		if (r) FreeUPNPUrls(&urls);
		return;
	}
	std::string ps = std::to_string(port);
	int tcp = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype, ps.c_str(), ps.c_str(), lan,
	                              "Stremio PS5", "TCP", nullptr, "0");
	int udp = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype, ps.c_str(), ps.c_str(), lan,
	                              "Stremio PS5", "UDP", nullptr, "0");
	dlog("upnp: port %d forwarded to %s: TCP %s, UDP %s", port, lan, tcp == 0 ? "ok" : strupnperror(tcp),
	     udp == 0 ? "ok" : strupnperror(udp));
	if (tcp == 0) {
		std::lock_guard<std::mutex> lock(I.upnp_m);
		I.upnp_control = urls.controlURL;
		I.upnp_service = data.first.servicetype;
		I.mapped_port = port;
	}
	FreeUPNPUrls(&urls);
#endif
}

void unmap_port() {
#ifdef HAVE_UPNP
	Impl& I = impl();
	std::lock_guard<std::mutex> lock(I.upnp_m);
	int port = I.mapped_port.exchange(0);
	if (!port) return;
	std::string ps = std::to_string(port);
	UPNP_DeletePortMapping(I.upnp_control.c_str(), I.upnp_service.c_str(), ps.c_str(), "TCP", nullptr);
	UPNP_DeletePortMapping(I.upnp_control.c_str(), I.upnp_service.c_str(), ps.c_str(), "UDP", nullptr);
#endif
}

void tracker_worker(std::shared_ptr<Torrent> t, int worker) {
	Torrent* tp = t.get();
	// One-time set-up, beside the first announces: the router mapping first
	// (so the trackers get a port that works), the DHT's first nodes.
	if (worker == 0 && !impl().map_started.exchange(true)) map_port();
	if (worker == kTrackerThreads - 1 && !impl().boot_started.exchange(true)) resolve_bootstrap();
	int round = 0;
	while (!tp->stopped) {
		std::vector<std::string> list;
		{
			std::lock_guard<std::mutex> lock(tp->trk_m);
			list = tp->trackers;
		}
		int min_interval = 0;
		if (!tp->idle) {
			for (size_t i = size_t(worker); i < list.size() && !tp->stopped; i += kTrackerThreads) {
				std::vector<sockaddr_in> peers;
				int interval = 0;
				bool ok = starts_with(list[i], "udp://") ? announce_udp(tp, list[i], round == 0, peers, &interval)
				                                         : announce_http(tp, list[i], round == 0, peers, &interval);
				if (ok) {
					std::lock_guard<std::mutex> lock(tp->trk_m);
					tp->tracker_peers.insert(tp->tracker_peers.end(), peers.begin(), peers.end());
					if (interval > 0 && (min_interval == 0 || interval < min_interval)) min_interval = interval;
				}
				if (round == 0)
					dlog("tracker %s: %s, %zu peers", list[i].c_str(), ok ? "ok" : "no answer", peers.size());
			}
			round++;
		}
		// Next round: in a minute while peers are short, else as the tracker
		// asks (5 to 30 minutes).
		double start = now_seconds();
		while (!tp->stopped) {
			double waited = now_seconds() - start;
			double wait = tp->want_peers ? 60 : std::max(300, std::min(min_interval, 1800));
			if (waited >= wait) break;
			if (!wait_stoppable(tp, 1)) break;
		}
	}
}

void ensure_running() {
	Impl& I = impl();
	if (I.running) return;
	I.running = true;
	I.quit = false;
	char id[21];
	snprintf(id, sizeof(id), "-SP0100-");
	static const char* al = "0123456789abcdefghijklmnopqrstuvwxyz";
	uint8_t r[12];
	random_bytes(r, 12);
	std::string pid(id);
	for (int i = 0; i < 12; i++) pid += al[r[i] % 36];
	I.peer_id = pid;
	if (I.listen_fd < 0) open_listener();
	I.net = std::thread(net_loop);
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API

Engine& Engine::get() {
	static Engine e;
	return e;
}

void Engine::configure(const std::string& data_dir, int64_t cache_bytes) {
	std::lock_guard<std::mutex> lock(impl().mu);
	impl().data_dir = data_dir;
	impl().cache_bytes = cache_bytes;
}

void Engine::shutdown() {
	Impl& I = impl();
	{
		std::lock_guard<std::mutex> lock(I.mu);
		if (!I.running) return;
		I.quit = true;
		for (auto& t : I.torrents) t->stopped = true;
	}
	I.cv.notify_all();
	if (I.net.joinable()) I.net.join();
	unmap_port();
	std::lock_guard<std::mutex> lock(I.mu);
	I.running = false;
}

void Engine::start(const std::string& hex_in, const std::vector<std::string>& trackers_in) {
	Impl& I = impl();
	std::string hex = lower(hex_in);
	std::vector<std::string> trackers;
	for (auto x : trackers_in) {
		if (starts_with(x, "tracker:")) x = x.substr(8);
		if (starts_with(x, "dht:")) continue;
		if ((starts_with(x, "udp://") || starts_with(x, "http://") || starts_with(x, "https://")) &&
		    std::find(trackers.begin(), trackers.end(), x) == trackers.end())
			trackers.push_back(x);
	}
	for (auto* d : kDefaultTrackers)
		if (std::find(trackers.begin(), trackers.end(), d) == trackers.end()) trackers.push_back(d);

	std::lock_guard<std::mutex> lock(I.mu);
	ensure_running();
	if (auto t = find_locked(hex)) {
		std::lock_guard<std::mutex> tl(t->trk_m);
		for (auto& x : trackers)
			if (std::find(t->trackers.begin(), t->trackers.end(), x) == t->trackers.end()) t->trackers.push_back(x);
		t->started = now_seconds();  // fresh grace period before pausing
		return;
	}
	for (auto& t : I.torrents) t->stopped = true;  // one torrent at a time
	I.cv.notify_all();

	auto t = std::make_shared<Torrent>();
	t->hex = hex;
	if (!hex_to_bytes(hex, t->ih)) {
		dlog("torrent: bad info hash %s", hex.c_str());
		t->stopped = true;
		return;
	}
	t->started = now_seconds();
	t->trackers = trackers;
	// Metadata saved from an earlier time: no need to ask the peers.
	std::string saved;
	if (read_file(I.data_dir + "/torrents/" + hex + ".info", saved)) {
		unsigned char digest[20];
		SHA1(reinterpret_cast<const unsigned char*>(saved.data()), saved.size(), digest);
		if (memcmp(digest, t->ih, 20) == 0 && parse_info(t.get(), saved)) dlog("torrent %s: metadata from the cache", hex.c_str());
	}
	dlog("torrent %s: starting with %zu trackers", hex.c_str(), trackers.size());
	I.torrents.push_back(t);
	for (int w = 0; w < kTrackerThreads; w++) t->tracker_threads.emplace_back(tracker_worker, t, w);
}

bool Engine::wait_metadata(const std::string& hex, std::vector<FileInfo>& files, const std::atomic<bool>* cancel,
                           double timeout_s, std::string* error) {
	Impl& I = impl();
	double until = now_seconds() + timeout_s;
	std::unique_lock<std::mutex> lock(I.mu);
	for (;;) {
		auto t = find_locked(lower(hex));
		if (!t) {
			if (error) *error = "the torrent was stopped";
			return false;
		}
		if (t->has_meta) {
			files = t->files;
			return true;
		}
		if (cancel && cancel->load()) {
			if (error) *error = "cancelled";
			return false;
		}
		if (now_seconds() >= until) {
			if (error) *error = "timed out";
			return false;
		}
		I.cv.wait_for(lock, std::chrono::milliseconds(200));
	}
}

void Engine::select_file(const std::string& hex, int file_idx) {
	std::lock_guard<std::mutex> lock(impl().mu);
	if (auto t = find_locked(lower(hex))) {
		if (file_idx >= 0 && size_t(file_idx) < t->files.size()) t->file = file_idx;
		t->wanted_dirty = true;
	}
}

Stats Engine::stats(const std::string& hex) {
	Stats s;
	std::lock_guard<std::mutex> lock(impl().mu);
	auto t = find_locked(lower(hex));
	if (!t) return s;
	s.found = true;
	s.has_metadata = t->has_meta;
	for (auto& p : t->peers)
		if (!p->dead && p->state == Peer::Active) s.peers++;
	s.known_peers = int(t->known.size());
	s.incoming = impl().mapped_port.load() > 0;
	s.download_rate = t->rate;
	if (t->has_meta && t->file >= 0) {
		const FileInfo& f = t->files[size_t(t->file)];
		if (f.size > 0) {
			int first = int(f.offset / t->plen), last = int((f.offset + f.size - 1) / t->plen);
			int got = 0;
			for (int p = first; p <= last; p++) got += t->ever[size_t(p)];
			s.file_progress = double(got) / double(last - first + 1);
		}
	}
	return s;
}

std::shared_ptr<Torrent> Engine::open_reader(const std::string& hex, int file_idx, int* reader_id, int64_t* file_size,
                                             std::string* error) {
	std::lock_guard<std::mutex> lock(impl().mu);
	auto t = find_locked(lower(hex));
	if (!t || !t->has_meta) {
		if (error) *error = t ? "no metadata yet" : "the torrent isn't running";
		return nullptr;
	}
	if (file_idx < 0 || size_t(file_idx) >= t->files.size()) {
		if (error) *error = "no such file in the torrent";
		return nullptr;
	}
	t->file = file_idx;
	int id = t->next_reader++;
	t->readers[id] = t->files[size_t(file_idx)].offset;
	t->wanted_dirty = true;
	t->idle = false;
	*reader_id = id;
	*file_size = t->files[size_t(file_idx)].size;
	return t;
}

void Engine::close_reader(const std::shared_ptr<Torrent>& t, int reader_id) {
	std::lock_guard<std::mutex> lock(impl().mu);
	t->readers.erase(reader_id);
	t->last_reader = now_seconds();
	t->wanted_dirty = true;
}

int Engine::read(const std::shared_ptr<Torrent>& t, int reader_id, int file_idx, int64_t pos, uint8_t* buf, int n,
                 const std::atomic<bool>* abort) {
	Impl& I = impl();
	std::unique_lock<std::mutex> lock(I.mu);
	const FileInfo& f = t->files[size_t(file_idx)];
	if (pos >= f.size || n <= 0) return 0;
	int64_t abs = f.offset + pos;
	auto r = t->readers.find(reader_id);
	if (r != t->readers.end() && r->second != abs) {
		// Into another piece (reading on, or a jump): the download order
		// follows the reader.
		if (abs / t->plen != r->second / t->plen) t->wanted_dirty = true;
		r->second = abs;
	}
	int piece = int(abs / t->plen);
	while (!t->have[size_t(piece)]) {
		if (t->stopped) return -2;
		if (abort && abort->load()) return -1;
		I.cv.wait_for(lock, std::chrono::milliseconds(100));
	}
	int64_t in_piece = abs - int64_t(piece) * t->plen;
	int64_t k = std::min<int64_t>({int64_t(n), t->piece_size(piece) - in_piece, f.size - pos});
	int slot = t->slot_of[size_t(piece)];
	if (slot < 0 || !slot_read(t.get(), slot, in_piece, buf, int(k))) {
		dlog("torrent %s: cache read failed for piece %d", t->hex.c_str(), piece);
		t->have[size_t(piece)] = 0;  // download it again
		t->wanted_dirty = true;
		return -1;
	}
	t->slot_touch[size_t(slot)] = now_seconds();
	return int(k);
}

int Engine::guess_file(const std::vector<FileInfo>& files, int season, int episode) {
	int best = -1;
	if (season >= 0 && episode >= 0) {
		char pats[4][32];
		snprintf(pats[0], 32, "s%02de%02d", season, episode);
		snprintf(pats[1], 32, "s%de%02d", season, episode);
		snprintf(pats[2], 32, "%dx%02d", season, episode);
		snprintf(pats[3], 32, "s%02d.e%02d", season, episode);
		for (size_t i = 0; i < files.size(); i++) {
			if (!is_video_name(files[i].path)) continue;
			std::string name = lower(files[i].path);
			size_t sl = name.rfind('/');
			if (sl != std::string::npos) name = name.substr(sl + 1);
			for (auto& p : pats) {
				size_t at = name.find(p);
				// "s01e01" must not match "s01e010"
				if (at != std::string::npos && !isdigit(uint8_t(name[std::min(name.size() - 1, at + strlen(p))]))) {
					if (best < 0 || files[i].size > files[size_t(best)].size) best = int(i);
					break;
				}
			}
		}
		if (best >= 0) return best;
	}
	for (size_t i = 0; i < files.size(); i++)
		if (is_video_name(files[i].path) && (best < 0 || files[i].size > files[size_t(best)].size)) best = int(i);
	if (best >= 0) return best;
	for (size_t i = 0; i < files.size(); i++)
		if (best < 0 || files[i].size > files[size_t(best)].size) best = int(i);
	return best;
}

}  // namespace bt

#ifdef HAVE_DHT
// What the dht library asks of its user.
extern "C" {
int dht_sendto(int sockfd, const void* buf, int len, int flags, const struct sockaddr* to, int tolen) {
	return int(sendto(sockfd, buf, size_t(len), flags, to, socklen_t(tolen)));
}
int dht_blacklisted(const struct sockaddr* sa, int salen) {
	(void)sa;
	(void)salen;
	return 0;
}
void dht_hash(void* hash_return, int hash_size, const void* v1, int len1, const void* v2, int len2, const void* v3,
              int len3) {
	std::string all;
	if (v1) all.append(static_cast<const char*>(v1), size_t(len1));
	if (v2) all.append(static_cast<const char*>(v2), size_t(len2));
	if (v3) all.append(static_cast<const char*>(v3), size_t(len3));
	unsigned char d[20];
	SHA1(reinterpret_cast<const unsigned char*>(all.data()), all.size(), d);
	memset(hash_return, 0, size_t(hash_size));
	memcpy(hash_return, d, size_t(std::min(hash_size, 20)));
}
int dht_random_bytes(void* buf, size_t size) {
	bt::random_bytes(buf, size);
	return int(size);
}
}
#endif
