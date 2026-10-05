#include "netstream.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
}

#include <curl/curl.h>

#include <algorithm>
#include <chrono>
#include <cstring>

#include "http.h"
#include "util.h"

namespace {

const size_t kMaxBuffered = 16u << 20;   // read-ahead
const int64_t kSkipWindow = 1 << 20;    // forward seeks shorter than this read on
const int kRetries = 20;  // slow torrents: keep trying, as the PC app does (Circle stops)

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

NetStream::NetStream(const std::string& url, const std::vector<std::string>& headers, const std::atomic<bool>* abort)
    : url_(url), headers_(headers), abort_(abort) {}

NetStream::~NetStream() {
	{
		std::lock_guard<std::mutex> lock(m_);
		stop_ = true;
		gen_++;
	}
	cv_.notify_all();
	if (thread_.joinable()) thread_.join();
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
	return error_.empty();
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
		}
		CURLcode rc = curl_easy_perform(c);
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
                                  const std::atomic<bool>* abort, std::string* error) {
	auto* ns = new NetStream(url, headers, abort);
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
