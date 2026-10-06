#pragma once

#include <SDL.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "hwdec_ps5.h"
#include "netstream.h"
#include "subtitles.h"
#include "yuv_convert.h"

struct AVFormatContext;
struct AVCodecContext;
struct AVPacket;
struct SwsContext;
struct SwrContext;

// Plays one URL with FFmpeg; video on the PS5's hardware decoder when it
// takes the stream (hwdec_ps5.h), else FFmpeg's software decoder. Threads:
// demux, video decode (+ scale to screen size), audio decode (+ resample
// into SDL's queue). The audio queue is the master clock; without audio, the wall clock.
// The UI thread calls present() every frame.
class Player {
public:
	struct Track {
		int stream = -1;
		std::string label, lang;
	};
	struct Options {
		std::string url;
		std::vector<std::string> headers;
		double start = 0;
		std::vector<std::string> audio_langs;  // preferred, ISO 639-2
		// A direct link (not the streaming server): a big file may be
		// downloaded over several connections (netstream.h).
		bool parallel = false;
	};
	enum class State { Idle, Opening, Playing, Ended, Failed };

	~Player();

	void open(const Options& opts);
	void close();

	State state() const { return state_.load(); }
	std::string error() const;

	bool paused() const { return paused_; }
	void set_paused(bool p);
	bool buffering(int* percent = nullptr) const;

	double position();  // seconds; the seek target while a seek is pending
	double duration() const { return duration_; }
	void seek(double t);

	std::vector<Track> audio_tracks() const;
	int audio_stream() const { return audio_stream_.load(); }
	void select_audio(int stream);

	// Text subtitles inside the file (SRT/ASS/WebVTT/mov_text).
	std::vector<Track> subtitle_tracks() const;
	std::string embedded_subtitle(int stream, double t);

	// UI thread: picks the frame due now, uploads and draws it.
	void present(SDL_Renderer* renderer);

	std::string stats();
	std::string codec_summary() const;
	// Every 30 s while playing: frame rate shown, drops, buffer, decoder (to the log).
	void log_stats();
	bool too_heavy() const { return too_heavy_; }

private:
	class PacketQueue {
	public:
		void put(AVPacket* pkt, double pts_s, double dur_s);
		// 1: packet, 0: timed out/empty, -1: end of input reached and empty
		int get(AVPacket** pkt, int* serial, int timeout_ms);
		void flush();
		void set_eof(bool eof);
		void abort();
		size_t count() const;
		size_t bytes() const;
		double duration() const;
		int serial() const;
		void reset();

	private:
		struct Item {
			AVPacket* pkt;
			double pts, dur;
		};
		mutable std::mutex m_;
		std::condition_variable cv_;
		std::deque<Item> q_;
		size_t bytes_ = 0;
		double last_pts_ = -1, dur_sum_ = 0;
		int serial_ = 0;
		bool eof_ = false, abort_ = false;
	};
	struct Frame {
		std::vector<uint8_t> pixels;  // BGRA
		int w = 0, h = 0;
		double pts = 0;
		int serial = 0;
	};

	static int interrupt_cb(void* opaque);
	void demux_thread();
	void demux_run();
	void video_thread();
	bool video_thread_hw();  // false: the hardware decoder gave up, carry on in software
	bool emit_frame(double pts, int serial, const std::function<void(Frame&)>& fill, int src_w, int src_h,
	                double sar);
	void hw_to_bgra(const HwDecoder::Picture& pic, Frame& f);
	bool fast_convert(const YuvPicture& pic, Frame& f);
	YuvColors colors_ = YuvColors::Bt709;  // of the video being played
	void audio_thread();
	void fail(const std::string& msg);
	bool open_stream_codec(int stream, AVCodecContext** out);
	void handle_subtitle_packet(AVPacket* pkt);
	void update_buffering();
	double clock_locked();
	void set_clock_running(bool running);
	std::string track_label(int stream) const;

	Options opts_;
	std::atomic<State> state_{State::Idle};
	mutable std::mutex err_m_;
	std::string error_;

	std::thread demux_, video_, audio_;
	std::atomic<bool> abort_{false};
	double open_started_ = 0;

	AVFormatContext* fmt_ = nullptr;
	AVIOContext* main_pb_ = nullptr;  // our network input (NetStream)
	std::atomic<bool> quick_{false};  // opened without the index: seeking reopens
	NetStream::IoHooks io_hooks_;
	std::atomic<int> video_stream_{-1}, audio_stream_{-1};
	AVCodecContext* vctx_ = nullptr;
	std::unique_ptr<HwDecoder> hw_;      // the hardware decoder, while it decodes
	std::atomic<bool> hw_active_{false};  // for the stats line
	int hw_shift_ = -1;                   // 10-bit samples: shift to 8 bits (2 or 8, found on the first picture)
	SwsContext* hw_sws_ = nullptr;        // hardware pictures at sizes the fast path doesn't do
	AVCodecContext* actx_ = nullptr;
	std::mutex actx_m_;
	std::map<int, AVCodecContext*> sctx_;
	double duration_ = 0;

	PacketQueue vq_, aq_;
	std::atomic<bool> eof_{false};

	// Seeking
	std::mutex seek_m_;
	bool seek_req_ = false;
	double seek_target_ = 0;
	// Accurate seeking: each decoder discards output before its target.
	std::atomic<double> vdrop_{-1}, adrop_{-1};
	std::atomic<int> audio_switch_{-1};
	double start_offset_ = 0;  // container start time; all times shown are relative to it
	double to_sec(int64_t ts, double tb) const;
	void do_seek(double target);
	void apply_run_state();
	double queued_audio_seconds() const;

	// Video frames
	std::mutex fq_m_;
	std::condition_variable fq_cv_;
	std::deque<Frame> frames_;
	std::vector<Frame> pool_;
	std::atomic<bool> video_drained_{false};
	SDL_Texture* tex_ = nullptr;
	int tex_w_ = 0, tex_h_ = 0;
	bool have_picture_ = false;
	SwsContext* sws_ = nullptr;

	// Audio
	SDL_AudioDeviceID dev_ = 0;
	SwrContext* swr_ = nullptr;
	std::mutex clock_m_;
	double audio_end_pts_ = -1;  // pts at the end of what's queued in SDL
	std::atomic<bool> audio_drained_{false};

	// Wall clock (no audio)
	double wall_base_pts_ = 0, wall_base_time_ = 0;
	bool wall_running_ = false;

	std::atomic<bool> paused_{false};
	std::atomic<bool> buffering_{true};
	std::atomic<int> buffer_percent_{0};
	int stalls_ = 0;       // index into kBufferTargets: grows each time the stream falls behind
	int stall_count_ = 0;  // for the log
	double start_target_ = 0;  // seconds to buffer at least, from the file's bitrate

	// Subtitles inside the file
	mutable std::mutex sub_m_;
	std::map<int, std::vector<Cue>> embedded_;

	// Stats
	std::atomic<int> dropped_{0}, decoded_{0}, shown_{0};
	// log_stats(): counts at the previous call
	double log_at_ = 0;
	int log_shown_ = 0, log_dropped_ = 0;
	std::atomic<bool> too_heavy_{false};
	double heavy_window_start_ = 0;
	int heavy_window_dropped_ = 0, heavy_window_decoded_ = 0;
	double fps_ = 0;
};
