// Network input for the player: FFmpeg reads HTTP(S) through libcurl.
//
// FFmpeg's own HTTP client never got a byte from the streaming server on the
// PS5 (it opened, waited and gave up), while libcurl talks to the same server
// fine. So every http(s) URL the player opens -- the stream itself and, for
// HLS, the playlists and segments -- goes through NetStream instead.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct AVIOContext;
struct AVFormatContext;
struct AVDictionary;

class NetStream {
public:
	NetStream(const std::string& url, const std::vector<std::string>& headers, const std::atomic<bool>* abort);
	~NetStream();

	// Starts downloading and waits for the response headers. False on error
	// (see error()).
	bool start();
	const std::string& error() const { return error_; }  // after start(), same thread
	std::string failure();  // any time: why downloading stopped, if it did
	int64_t size() const { return size_; }  // -1 when unknown

	// The first bytes at the current position, without consuming them
	// (fewer at the end of the file or on error).
	std::string peek(size_t n);
	static NetStream* of(AVIOContext* pb);

	// AVIOContext callbacks: FFmpeg error codes on failure.
	int read(uint8_t* buf, int n);
	int64_t seek(int64_t offset, int whence);

	// An AVIOContext reading this stream; free with close_avio().
	static AVIOContext* open_avio(const std::string& url, const std::vector<std::string>& headers,
	                              const std::atomic<bool>* abort, std::string* error);
	static bool is_ours(AVIOContext* pb);
	static void close_avio(AVIOContext** pb);

	// AVFormatContext.io_open / io_close2 for the files a demuxer opens
	// itself (HLS playlists and segments). opaque must point at the
	// player's IoHooks.
	struct IoHooks {
		std::vector<std::string> headers;
		const std::atomic<bool>* abort = nullptr;
		int (*default_open)(AVFormatContext*, AVIOContext**, const char*, int, AVDictionary**) = nullptr;
		int (*default_close)(AVFormatContext*, AVIOContext*) = nullptr;
	};
	static void install(AVFormatContext* fmt, IoHooks* hooks);

private:
	void run();
	bool transfer(int64_t from, int gen);
	bool stopped() const;

	static size_t on_header(char* data, size_t size, size_t n, void* self);
	static size_t on_data(char* data, size_t size, size_t n, void* self);
	static int on_progress(void* self, int64_t, int64_t, int64_t, int64_t);

	std::string url_;
	std::vector<std::string> headers_;
	const std::atomic<bool>* abort_;

	std::mutex m_;
	std::condition_variable cv_;
	std::thread thread_;
	bool stop_ = false;

	// Requests to the download thread: (re)start at req_pos_ when gen_ moves.
	int gen_ = 0, started_gen_ = -1;
	int64_t req_pos_ = 0;

	// Downloaded, not yet read: buf_[head_..] holds bytes from pos_ on.
	std::vector<uint8_t> buf_;
	size_t head_ = 0;
	int64_t pos_ = 0;
	bool eof_ = false;
	bool headers_done_ = false;
	long status_ = 0;
	int64_t size_ = -1;
	std::string error_;
	int64_t discard_ = 0;  // bytes still to drop before buf_ (short forward seeks, or a server that ignored Range)
	bool header_ok_ = false;
	int64_t range_from_ = 0;  // where the running transfer was asked to start
	int cur_gen_ = 0;      // generation of the running transfer (for the callbacks)
};
