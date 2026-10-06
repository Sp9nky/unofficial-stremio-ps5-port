#include "torrent_stream.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
}

#include <cstdlib>

#include "../util.h"
#include "engine.h"

namespace {

int read_cb(void* opaque, uint8_t* buf, int n) { return static_cast<TorrentStream*>(opaque)->read(buf, n); }
int64_t seek_cb(void* opaque, int64_t offset, int whence) {
	return static_cast<TorrentStream*>(opaque)->seek(offset, whence);
}

}  // namespace

bool TorrentStream::parse_url(const std::string& url, std::string* hash, int* idx) {
	if (!is_url(url)) return false;
	std::string rest = url.substr(10);
	size_t slash = rest.find('/');
	if (slash != 40) return false;
	*hash = rest.substr(0, 40);
	*idx = std::atoi(rest.c_str() + 41);
	return true;
}

std::string TorrentStream::make_url(const std::string& hash, int idx) {
	return "torrent://" + hash + "/" + std::to_string(idx);
}

int TorrentStream::read(uint8_t* buf, int n) {
	if (!t_) return AVERROR(EIO);
	int k = bt::Engine::get().read(t_, reader_, file_, pos_, buf, n, abort_);
	if (k > 0) {
		pos_ += k;
		return k;
	}
	if (k == 0) return AVERROR_EOF;
	if (k == -2) failure_ = "the torrent was stopped";
	return k == -1 && abort_ && abort_->load() ? AVERROR_EXIT : AVERROR(EIO);
}

int64_t TorrentStream::seek(int64_t offset, int whence) {
	if (whence & AVSEEK_SIZE) return size_;
	whence &= ~AVSEEK_FORCE;
	int64_t target;
	if (whence == SEEK_SET) target = offset;
	else if (whence == SEEK_CUR) target = pos_ + offset;
	else if (whence == SEEK_END) target = size_ + offset;
	else return AVERROR(EINVAL);
	if (target < 0) return AVERROR(EINVAL);
	pos_ = target;
	return target;
}

std::string TorrentStream::peek(size_t n) {
	std::string out(n, '\0');
	int64_t keep = pos_;
	size_t got = 0;
	while (got < n) {
		int k = bt::Engine::get().read(t_, reader_, file_, pos_ + int64_t(got),
		                               reinterpret_cast<uint8_t*>(&out[got]), int(n - got), abort_);
		if (k <= 0) break;
		got += size_t(k);
	}
	pos_ = keep;
	out.resize(got);
	return out;
}

AVIOContext* TorrentStream::open_avio(const std::string& url, const std::atomic<bool>* abort, std::string* error) {
	std::string hash;
	int idx = 0;
	if (!parse_url(url, &hash, &idx)) {
		if (error) *error = "bad torrent address";
		return nullptr;
	}
	auto* ts = new TorrentStream();
	ts->file_ = idx;
	ts->abort_ = abort;
	ts->t_ = bt::Engine::get().open_reader(hash, idx, &ts->reader_, &ts->size_, error);
	if (!ts->t_) {
		delete ts;
		return nullptr;
	}
	const int kBufSize = 256 * 1024;
	auto* buf = static_cast<uint8_t*>(av_malloc(kBufSize));
	AVIOContext* pb = buf ? avio_alloc_context(buf, kBufSize, 0, ts, read_cb, nullptr, seek_cb) : nullptr;
	if (!pb) {
		av_free(buf);
		close_avio(&pb);
		bt::Engine::get().close_reader(ts->t_, ts->reader_);
		delete ts;
		if (error) *error = "out of memory";
		return nullptr;
	}
	pb->seekable = AVIO_SEEKABLE_NORMAL;
	dlog("torrent: playing file %d of %s (%lld bytes)", idx, hash.c_str(), (long long)ts->size_);
	return pb;
}

bool TorrentStream::is_ours(AVIOContext* pb) { return pb && pb->read_packet == read_cb; }

TorrentStream* TorrentStream::of(AVIOContext* pb) {
	return is_ours(pb) ? static_cast<TorrentStream*>(pb->opaque) : nullptr;
}

void TorrentStream::close_avio(AVIOContext** pb) {
	if (!pb || !*pb) return;
	if (auto* ts = of(*pb)) {
		bt::Engine::get().close_reader(ts->t_, ts->reader_);
		delete ts;
	}
	av_freep(&(*pb)->buffer);
	avio_context_free(pb);
}
