// The player's input for a torrent played by the built-in engine: an
// AVIOContext over one file of the torrent, at "torrent://<info hash>/<file>".
// Reads wait for the pieces they need; seeks just move the reader (the
// engine then downloads from there first).
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

struct AVIOContext;

namespace bt {
struct Torrent;
}

class TorrentStream {
public:
	static bool is_url(const std::string& url) { return url.compare(0, 10, "torrent://") == 0; }
	// "torrent://<hash>/<idx>" -> hash, idx. False if it isn't one.
	static bool parse_url(const std::string& url, std::string* hash, int* idx);
	static std::string make_url(const std::string& hash, int idx);

	static AVIOContext* open_avio(const std::string& url, const std::atomic<bool>* abort, std::string* error);
	static bool is_ours(AVIOContext* pb);
	static TorrentStream* of(AVIOContext* pb);
	static void close_avio(AVIOContext** pb);

	// The first bytes of the file, without moving (waits for them).
	std::string peek(size_t n);
	std::string failure() const { return failure_; }
	int64_t size() const { return size_; }

	int read(uint8_t* buf, int n);
	int64_t seek(int64_t offset, int whence);

private:
	TorrentStream() = default;
	std::shared_ptr<bt::Torrent> t_;
	int reader_ = 0, file_ = 0;
	int64_t size_ = 0, pos_ = 0;
	const std::atomic<bool>* abort_ = nullptr;
	std::string failure_;
};
