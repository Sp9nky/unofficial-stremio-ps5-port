#include "netstream.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
}

#include <curl/curl.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <functional>

#include "http.h"
#include "util.h"

namespace {

const size_t kMaxBuffered = 16u << 20;   // read-ahead
const int64_t kSkipWindow = 1 << 20;    // forward seeks shorter than this read on
const int kRetries = 20;  // slow torrents: keep trying, as the PC app does (Circle stops)

// Parallel mode (Nuvio PS5's numbers): files from 96 MB, 4 MB chunks, six
// connections. The chunks ahead of the reader go to a file in the app's
// storage, up to 6 GB (about half an hour of 4K at 25 Mbit/s), so a slow
// patch in the download doesn't stop playback; in memory (128 MB) when the
// file can't be used.
const int64_t kParallelMin = 96ll << 20;
const int64_t kChunk = 4ll << 20;
const int kWindowRam = 32;
const int kWindowDisk = 1536;
const int kWorkers = 6;
// A streaming server may still be fetching that part of the torrent: be patient.
const int kChunkAttempts = 20;
const double kSpeedLogEvery = 30;

std::string g_cache_dir;
std::atomic<int> g_cache_serial{0};

int read_cb(void* opaque, uint8_t* buf, int n) { return static_cast<NetStream*>(opaque)->read(buf, n); }
int64_t seek_cb(void* opaque, int64_t offset, int whence) {
	return static_cast<NetStream*>(opaque)->seek(offset, whence);
}

bool is_http(const char* url) { return !strncmp(url, "http://", 7) || !strncmp(url, "https://", 8); }

int io_open_cb(AVFormatContext* s, AVIOContext** pb, const char* url, int flags, AVDictionary** options) {
	auto* hooks = static_cast<NetStream::IoHooks*>(s->opaque);
	if ((flags & AVIO_FLAG_WRITE) || !is_http(url)) return hooks->default_open(s, pb, url, flags, options);
	std::string err;
	*pb = NetStream::open_avio(url, hooks->headers, hooks->abort, &err);
	if (!*pb) {
		dlog("net: %s: %s", url, err.c_str());
		return err == "cancelled" ? AVERROR_EXIT : AVERROR(EIO);
	}
	return 0;
}

int io_close_cb(AVFormatContext* s, AVIOContext* pb) {
	if (NetStream::is_ours(pb)) {
		NetStream::close_avio(&pb);
		return 0;
	}
	auto* hooks = static_cast<NetStream::IoHooks*>(s->opaque);
	return hooks->default_close(s, pb);
}

}  // namespace

NetStream::NetStream(const std::string& url, const std::vector<std::string>& headers, const std::atomic<bool>* abort,
                     bool parallel)
    : url_(url), headers_(headers), abort_(abort), parallel_wanted_(parallel) {}

NetStream::~NetStream() {
	{
		std::lock_guard<std::mutex> lock(m_);
		stop_ = true;
		gen_++;
	}
	cv_.notify_all();
	if (thread_.joinable()) thread_.join();
	for (auto& w : workers_)
		if (w.joinable()) w.join();
	if (cache_) {
		fclose(cache_);
		remove(cache_path_.c_str());
	}
}

void NetStream::set_cache_dir(const std::string& dir) {
	g_cache_dir = dir;
	for (int i = 0; i < 16; i++) remove((dir + "/stream-cache-" + std::to_string(i) + ".bin").c_str());  // left by a crash
}

// The download speed now and then, for the log (not shown in the app).
void NetStream::log_speed(int64_t bytes) {  // m_ held
	speed_bytes_ += bytes;
	double now = now_seconds();
	if (speed_at_ == 0) speed_at_ = now;
	if (now - speed_at_ < kSpeedLogEvery) return;
	double rate = speed_bytes_ / (now - speed_at_) / (1 << 20);
	if (parallel_) {
		int64_t idx = pos_ / kChunk, ahead = 0;  // ready chunks from the reader on
		while (ahead < window_) {
			const Chunk& s = chunks_[size_t((idx + ahead) % window_)];
			if (s.index != idx + ahead || s.state != 2) break;
			ahead++;
		}
		dlog("net: %.2f MB/s over %d connections, %lld MB downloaded ahead of the player%s", rate, kWorkers,
		     (long long)(ahead * (kChunk >> 20)), cache_ ? "" : " (in memory)");
	} else {
		dlog("net: %.2f MB/s over one connection", rate);
	}
	speed_at_ = now;
	speed_bytes_ = 0;
}

bool NetStream::stopped() const { return stop_ || (abort_ && abort_->load()); }

bool NetStream::start() {
	std::unique_lock<std::mutex> lock(m_);
	req_pos_ = 0;
	gen_++;
	thread_ = std::thread([this] { run(); });
	while (!headers_done_ && error_.empty() && !eof_) {
		if (abort_ && abort_->load()) {
			error_ = "cancelled";
			break;
		}
		cv_.wait_for(lock, std::chrono::milliseconds(100));
	}
	// A big file from a server that takes byte ranges: several connections.
	if (error_.empty() && parallel_wanted_ && size_ >= kParallelMin && status_ == 206) start_parallel();
	return error_.empty();
}

// ---------------------------------------------------------------------------
// Parallel mode

void NetStream::start_parallel() {  // m_ held
	// Stop the single download (it has served its purpose: the size, the
	// final URL and that ranges work) and drop what it brought.
	gen_++;
	started_gen_ = gen_;
	buf_.clear();
	head_ = 0;
	discard_ = 0;
	eof_ = false;
	parallel_ = true;
	window_ = kWindowRam;
	if (!g_cache_dir.empty()) {
		cache_path_ = g_cache_dir + "/stream-cache-" + std::to_string(g_cache_serial++ % 16) + ".bin";
		cache_ = fopen(cache_path_.c_str(), "w+b");
		if (cache_) window_ = kWindowDisk;
		else dlog("net: can't create %s (errno %d); read-ahead in memory", cache_path_.c_str(), errno);
	}
	chunks_.assign(size_t(window_), Chunk());
	window_base_ = pos_ / kChunk;
	par_started_ = now_seconds();
	par_bytes_ = 0;
	if (final_url_.empty()) final_url_ = url_;
	for (int i = 0; i < kWorkers; i++) workers_.emplace_back([this] { worker(); });
	dlog("net: %lld MB file, downloading with %d connections, up to %d MB ahead %s", (long long)(size_ >> 20),
	     kWorkers, int(window_ * (kChunk >> 20)), cache_ ? "on storage" : "in memory");
}

// Storage failed (full, or refused): carry on with the read-ahead in memory.
void NetStream::drop_disk_cache(const char* why) {  // m_ held
	dlog("net: stream cache %s (errno %d); read-ahead in memory from now on", why, errno);
	fclose(cache_);
	remove(cache_path_.c_str());
	cache_ = nullptr;
	window_ = kWindowRam;
	chunks_.assign(size_t(window_), Chunk());
	cv_.notify_all();
}

namespace {
struct ChunkFetch {
	std::vector<uint8_t>* data;
	size_t expect;
	std::function<bool()> keep_going;
};
size_t chunk_data(char* p, size_t size, size_t n, void* ud) {
	auto* f = static_cast<ChunkFetch*>(ud);
	size_t len = size * n;
	if (f->data->size() + len > f->expect) return 0;  // more than asked for: not a range answer
	f->data->insert(f->data->end(), p, p + len);
	return len;
}
int chunk_progress(void* ud, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
	return static_cast<ChunkFetch*>(ud)->keep_going() ? 0 : 1;
}
}  // namespace

void NetStream::worker() {
	CURL* c = curl_easy_init();  // kept: the connection is reused for the next chunk
	std::unique_lock<std::mutex> lock(m_);
	while (c && !stopped()) {
		// The first chunk of the window that nobody has or is fetching.
		double now = now_seconds();
		int64_t last = (size_ - 1) / kChunk;
		int64_t pick = -1;
		for (int64_t i = window_base_; i < window_base_ + window_ && i <= last; i++) {
			Chunk& s = chunks_[size_t(i % window_)];
			if (s.state == 1) continue;  // busy (this chunk, or an old one still finishing)
			if (s.index == i && s.state == 2) continue;
			if (s.index == i && now < s.retry_at) continue;
			pick = i;
			break;
		}
		if (pick < 0) {
			cv_.wait_for(lock, std::chrono::milliseconds(100));
			continue;
		}
		const int my_window = window_;
		const size_t si = size_t(pick % window_);
		{
			Chunk& slot = chunks_[si];
			if (slot.index != pick) slot.attempts = 0;
			slot.index = pick;
			slot.state = 1;
			slot.data.clear();
		}
		int64_t from = pick * kChunk, to = std::min(size_, from + kChunk) - 1;
		std::string url = final_url_;
		lock.unlock();

		std::vector<uint8_t> data;
		data.reserve(size_t(to - from + 1));
		ChunkFetch f{&data, size_t(to - from + 1), [this, pick, my_window] {
			            std::lock_guard<std::mutex> l(m_);
			            // Give up when closed, or when a seek left this chunk behind.
			            return !stopped() && window_ == my_window && pick >= window_base_ &&
			                   pick < window_base_ + window_;
		            }};
		char errbuf[CURL_ERROR_SIZE] = {0};
		struct curl_slist* hdrs = nullptr;
		for (auto& h : headers_) hdrs = curl_slist_append(hdrs, h.c_str());
		std::string range = std::to_string(from) + "-" + std::to_string(to);
		curl_easy_reset(c);
		curl_easy_setopt(c, CURLOPT_URL, url.c_str());
		curl_easy_setopt(c, CURLOPT_RANGE, range.c_str());
		curl_easy_setopt(c, CURLOPT_USERAGENT, kUserAgent);
		curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
		curl_easy_setopt(c, CURLOPT_MAXREDIRS, 8L);
		curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
		curl_easy_setopt(c, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
		curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
		curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
		curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 120L);
		curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errbuf);
		curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, chunk_data);
		curl_easy_setopt(c, CURLOPT_WRITEDATA, &f);
		curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
		curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, chunk_progress);
		curl_easy_setopt(c, CURLOPT_XFERINFODATA, &f);
		if (!http_ca_bundle().empty() && file_exists(http_ca_bundle()))
			curl_easy_setopt(c, CURLOPT_CAINFO, http_ca_bundle().c_str());
		http_setup_handle(c);
		if (hdrs) curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
		CURLcode rc = curl_easy_perform(c);
		long status = 0;
		curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
		if (hdrs) curl_slist_free_all(hdrs);

		lock.lock();
		if (window_ != my_window || chunks_[si].index != pick) continue;  // not ours any more
		Chunk& slot = chunks_[si];
		if (rc == CURLE_OK && status == 206 && data.size() == size_t(to - from + 1)) {
			if (cache_) {  // to storage, in this slot's place in the file
				if (fseeko(cache_, off_t(si) * kChunk, SEEK_SET) != 0 ||
				    fwrite(data.data(), 1, data.size(), cache_) != data.size() || fflush(cache_) != 0) {
					drop_disk_cache("write failed");
					continue;
				}
				slot.len = int64_t(data.size());
			} else {
				slot.data = std::move(data);
				slot.len = int64_t(slot.data.size());
			}
			slot.state = 2;
			par_bytes_ += to - from + 1;
			log_speed(to - from + 1);
		} else {
			slot.state = 0;
			if (!stopped() && pick >= window_base_ && pick < window_base_ + window_) {
				slot.attempts++;
				slot.retry_at = now_seconds() + std::min(8, slot.attempts);
				std::string why = status >= 400 ? "HTTP " + std::to_string(status)
				                  : errbuf[0]    ? std::string(errbuf)
				                                 : curl_easy_strerror(rc);
				dlog("net: chunk %lld: %s (attempt %d)", (long long)pick, why.c_str(), slot.attempts);
				if (slot.attempts >= kChunkAttempts || (status >= 400 && status < 500)) error_ = why;
			}
		}
		cv_.notify_all();
	}
	lock.unlock();
	if (c) curl_easy_cleanup(c);
}

int NetStream::read_parallel(std::unique_lock<std::mutex>& lock, uint8_t* buf, int n) {
	for (;;) {
		if (abort_ && abort_->load()) return AVERROR_EXIT;
		if (pos_ >= size_) return AVERROR_EOF;
		int64_t idx = pos_ / kChunk;
		// The window follows the reader, one chunk kept behind it for short
		// steps back; a jump outside it starts a new one there.
		int64_t base = idx < window_base_ || idx >= window_base_ + window_ ? idx : std::max(window_base_, idx - 1);
		if (base != window_base_) {
			window_base_ = base;
			cv_.notify_all();
		}
		size_t si = size_t(idx % window_);
		Chunk& s = chunks_[si];
		if (s.index == idx && s.state == 2) {
			size_t off = size_t(pos_ - idx * kChunk);
			size_t k = std::min(size_t(n), size_t(s.len) - off);
			if (cache_) {
				if (fseeko(cache_, off_t(si) * kChunk + off_t(off), SEEK_SET) != 0 || fread(buf, 1, k, cache_) != k) {
					drop_disk_cache("read failed");
					continue;  // fetched again, into memory
				}
			} else {
				memcpy(buf, s.data.data() + off, k);
			}
			pos_ += int64_t(k);
			return int(k);
		}
		if (!error_.empty()) return AVERROR(EIO);
		cv_.wait_for(lock, std::chrono::milliseconds(100));
	}
}

// ---------------------------------------------------------------------------
// Download thread

void NetStream::run() {
	std::unique_lock<std::mutex> lock(m_);
	while (!stop_) {
		cv_.wait(lock, [this] { return stop_ || gen_ != started_gen_; });
		if (stop_) break;
		int gen = gen_;
		started_gen_ = gen;
		int64_t from = req_pos_;
		lock.unlock();
		transfer(from, gen);
		lock.lock();
	}
}

bool NetStream::transfer(int64_t from, int gen) {
	for (int attempt = 0;; attempt++) {
		CURL* c = curl_easy_init();
		if (!c) {
			std::lock_guard<std::mutex> lock(m_);
			error_ = "curl_easy_init failed";
			cv_.notify_all();
			return false;
		}
		{
			std::lock_guard<std::mutex> lock(m_);
			cur_gen_ = gen;
			header_ok_ = false;
			status_ = 0;
		}
		char errbuf[CURL_ERROR_SIZE] = {0};
		struct curl_slist* hdrs = nullptr;
		for (auto& h : headers_) hdrs = curl_slist_append(hdrs, h.c_str());
		std::string range = std::to_string(from) + "-";
		curl_easy_setopt(c, CURLOPT_URL, url_.c_str());
		curl_easy_setopt(c, CURLOPT_RANGE, range.c_str());
		curl_easy_setopt(c, CURLOPT_USERAGENT, kUserAgent);
		curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
		curl_easy_setopt(c, CURLOPT_MAXREDIRS, 8L);
		curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
		curl_easy_setopt(c, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
		curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
		// A torrent with few seeders can go quiet for minutes; reconnect
		// only after three minutes without any data.
		curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
		curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 180L);
		curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errbuf);
		curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, on_header);
		curl_easy_setopt(c, CURLOPT_HEADERDATA, this);
		curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
		curl_easy_setopt(c, CURLOPT_WRITEDATA, this);
		curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
		curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, on_progress);
		curl_easy_setopt(c, CURLOPT_XFERINFODATA, this);
		if (!http_ca_bundle().empty() && file_exists(http_ca_bundle()))
			curl_easy_setopt(c, CURLOPT_CAINFO, http_ca_bundle().c_str());
		http_setup_handle(c);
		if (hdrs) curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
		{
			std::lock_guard<std::mutex> lock(m_);
			discard_ = 0;
			range_from_ = from;
			cur_curl_ = c;
		}
		CURLcode rc = curl_easy_perform(c);
		{
			std::lock_guard<std::mutex> lock(m_);
			cur_curl_ = nullptr;
		}
		long status = 0;
		curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
		if (hdrs) curl_slist_free_all(hdrs);
		curl_easy_cleanup(c);

		std::unique_lock<std::mutex> lock(m_);
		if (stop_ || gen != gen_) return true;  // closed or seeked meanwhile
		if (rc == CURLE_OK && status >= 200 && status < 300) {
			eof_ = true;
			cv_.notify_all();
			return true;
		}
		if (status >= 400 && status < 500) {
			error_ = "HTTP " + std::to_string(status);
			dlog("net: %s -> %s", url_.c_str(), error_.c_str());
			cv_.notify_all();
			return false;
		}
		// 502/503/504: the proxy in front of the streaming server gave up
		// waiting (a torrent still looking for its first pieces). Not ready
		// yet, so ask again, as the PC app's player does.
		std::string why = status >= 500 ? "HTTP " + std::to_string(status) + " (server still preparing)"
		                                : (errbuf[0] ? errbuf : curl_easy_strerror(rc));
		if (attempt >= kRetries || (abort_ && abort_->load())) {
			error_ = why;
			dlog("net: %s -> %s (gave up)", url_.c_str(), why.c_str());
			cv_.notify_all();
			return false;
		}
		// Dropped connection: carry on from where the data ends.
		from = pos_ + int64_t(buf_.size() - head_) + discard_;
		dlog("net: %s -> %s; reconnecting at %lld", url_.c_str(), why.c_str(), (long long)from);
		cv_.wait_for(lock, std::chrono::seconds(1), [&] { return stop_ || gen != gen_; });
		if (stop_ || gen != gen_) return true;
	}
}

size_t NetStream::on_header(char* data, size_t size, size_t n, void* self) {
	auto* s = static_cast<NetStream*>(self);
	std::string line(data, size * n);
	std::lock_guard<std::mutex> lock(s->m_);
	if (starts_with(line, "HTTP/")) {
		size_t sp = line.find(' ');
		s->status_ = sp == std::string::npos ? 0 : std::atol(line.c_str() + sp + 1);
		return size * n;
	}
	std::string low = lower(line);
	if (starts_with(low, "content-range:")) {
		size_t slash = low.find('/');
		if (slash != std::string::npos && low[slash + 1] != '*') s->size_ = std::atoll(low.c_str() + slash + 1);
	} else if (starts_with(low, "content-length:") && s->status_ == 200 && s->size_ < 0) {
		s->size_ = std::atoll(low.c_str() + 15);
	}
	if (line == "\r\n" || line == "\n") {
		// End of one response's headers; redirects come before the real one.
		if (s->status_ >= 200 && s->status_ < 300) {
			s->header_ok_ = true;
			s->headers_done_ = true;
			char* eff = nullptr;
			if (s->cur_curl_ && curl_easy_getinfo(static_cast<CURL*>(s->cur_curl_), CURLINFO_EFFECTIVE_URL, &eff) == CURLE_OK &&
			    eff)
				s->final_url_ = eff;
			// 200 instead of 206: the server ignored Range and sends the
			// whole file, so drop everything before where we asked to start.
			if (s->status_ == 200 && s->range_from_ > 0) s->discard_ = s->range_from_;
			s->cv_.notify_all();
		}
	}
	return size * n;
}

size_t NetStream::on_data(char* data, size_t size, size_t n, void* self) {
	auto* s = static_cast<NetStream*>(self);
	size_t len = size * n;
	std::unique_lock<std::mutex> lock(s->m_);
	if (s->stop_ || s->cur_gen_ != s->gen_) return 0;  // closed or seeked: abort this transfer
	if (!s->header_ok_) return size * n;               // a redirect's body
	if (s->discard_ > 0) {
		size_t drop = size_t(std::min<int64_t>(s->discard_, int64_t(len)));
		s->discard_ -= int64_t(drop);
		data += drop;
		len -= drop;
		if (len == 0) return size * n;
	}
	// Read-ahead full: wait for the player to catch up.
	while (s->buf_.size() - s->head_ >= kMaxBuffered) {
		if (s->stop_ || s->cur_gen_ != s->gen_ || (s->abort_ && s->abort_->load())) return 0;
		s->cv_.wait_for(lock, std::chrono::milliseconds(100));
	}
	if (s->cur_gen_ != s->gen_) return 0;
	if (s->head_ > (4u << 20) && s->head_ * 2 > s->buf_.size()) {
		s->buf_.erase(s->buf_.begin(), s->buf_.begin() + s->head_);
		s->head_ = 0;
	}
	s->buf_.insert(s->buf_.end(), data, data + len);
	s->log_speed(int64_t(len));
	s->cv_.notify_all();
	return size * n;
}

int NetStream::on_progress(void* self, int64_t, int64_t, int64_t, int64_t) {
	auto* s = static_cast<NetStream*>(self);
	std::lock_guard<std::mutex> lock(s->m_);
	return (s->stop_ || s->cur_gen_ != s->gen_ || (s->abort_ && s->abort_->load())) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Reading and seeking (the demuxer's thread)

int NetStream::read(uint8_t* buf, int n) {
	std::unique_lock<std::mutex> lock(m_);
	if (parallel_) return read_parallel(lock, buf, n);
	for (;;) {
		if (abort_ && abort_->load()) return AVERROR_EXIT;
		size_t avail = buf_.size() - head_;
		if (avail > 0) {
			size_t k = std::min(avail, size_t(n));
			memcpy(buf, buf_.data() + head_, k);
			head_ += k;
			pos_ += int64_t(k);
			if (head_ == buf_.size()) buf_.clear(), head_ = 0;
			cv_.notify_all();
			return int(k);
		}
		if (!error_.empty()) return AVERROR(EIO);
		if (eof_) return AVERROR_EOF;
		cv_.wait_for(lock, std::chrono::milliseconds(100));
	}
}

std::string NetStream::peek(size_t n) {
	std::unique_lock<std::mutex> lock(m_);
	if (parallel_) {
		std::string out(n, '\0');
		int64_t keep = pos_;
		size_t got = 0;
		while (got < n) {
			int k = read_parallel(lock, reinterpret_cast<uint8_t*>(&out[got]), int(n - got));
			if (k <= 0) break;
			got += size_t(k);
		}
		pos_ = keep;
		out.resize(got);
		return out;
	}
	while (buf_.size() - head_ < n && error_.empty() && !eof_ && !(abort_ && abort_->load()))
		cv_.wait_for(lock, std::chrono::milliseconds(100));
	size_t k = std::min(n, buf_.size() - head_);
	return std::string(reinterpret_cast<const char*>(buf_.data() + head_), k);
}

std::string NetStream::failure() {
	std::lock_guard<std::mutex> lock(m_);
	return error_;
}

NetStream* NetStream::of(AVIOContext* pb) { return is_ours(pb) ? static_cast<NetStream*>(pb->opaque) : nullptr; }

int64_t NetStream::seek(int64_t offset, int whence) {
	std::unique_lock<std::mutex> lock(m_);
	if (whence & AVSEEK_SIZE) return size_ >= 0 ? size_ : AVERROR(ENOSYS);
	whence &= ~AVSEEK_FORCE;
	int64_t target;
	if (whence == SEEK_SET) target = offset;
	else if (whence == SEEK_CUR) target = pos_ + offset;
	else if (whence == SEEK_END && size_ >= 0) target = size_ + offset;
	else return AVERROR(EINVAL);
	if (target < 0) return AVERROR(EINVAL);
	if (parallel_) {  // the next read moves the window there
		pos_ = target;
		cv_.notify_all();
		return target;
	}

	int64_t avail = int64_t(buf_.size() - head_);
	int64_t end = pos_ + avail;  // first byte not yet downloaded
	if (target >= pos_ && target <= end) {  // already here
		head_ += size_t(target - pos_);
		pos_ = target;
		cv_.notify_all();
		return target;
	}
	if (target > end && target - end < kSkipWindow && error_.empty() && !eof_) {
		// A little further on: keep the connection and drop what's between.
		buf_.clear();
		head_ = 0;
		discard_ += target - end;
		pos_ = target;
		cv_.notify_all();
		return target;
	}
	// Anywhere else: a new request from there.
	dlog("net: jump to %lld of %lld (new request)", (long long)target, (long long)size_);
	buf_.clear();
	head_ = 0;
	discard_ = 0;
	pos_ = target;
	error_.clear();
	gen_++;
	if (size_ >= 0 && target >= size_) {
		eof_ = true;
		started_gen_ = gen_;  // nothing to download
	} else {
		eof_ = false;
		req_pos_ = target;
	}
	cv_.notify_all();
	return target;
}

// ---------------------------------------------------------------------------
// FFmpeg glue

AVIOContext* NetStream::open_avio(const std::string& url, const std::vector<std::string>& headers,
                                  const std::atomic<bool>* abort, std::string* error, bool parallel) {
	auto* ns = new NetStream(url, headers, abort, parallel);
	if (!ns->start()) {
		if (error) *error = ns->error();
		delete ns;
		return nullptr;
	}
	const int kBufSize = 256 * 1024;
	auto* buf = static_cast<uint8_t*>(av_malloc(kBufSize));
	AVIOContext* pb = buf ? avio_alloc_context(buf, kBufSize, 0, ns, read_cb, nullptr, seek_cb) : nullptr;
	if (!pb) {
		av_free(buf);
		delete ns;
		if (error) *error = "out of memory";
		return nullptr;
	}
	pb->seekable = ns->size() > 0 ? AVIO_SEEKABLE_NORMAL : 0;
	dlog("net: opened %s (%lld bytes)", url.c_str(), (long long)ns->size());
	return pb;
}

bool NetStream::is_ours(AVIOContext* pb) { return pb && pb->read_packet == read_cb; }

void NetStream::close_avio(AVIOContext** pb) {
	if (!pb || !*pb) return;
	delete static_cast<NetStream*>((*pb)->opaque);
	av_freep(&(*pb)->buffer);
	avio_context_free(pb);
}

void NetStream::install(AVFormatContext* fmt, IoHooks* hooks) {
	hooks->default_open = fmt->io_open;
	hooks->default_close = fmt->io_close2;
	fmt->opaque = hooks;
	fmt->io_open = io_open_cb;
	fmt->io_close2 = io_close_cb;
}
