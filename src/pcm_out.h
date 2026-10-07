// Sound out: 48 kHz stereo 16-bit samples handed over as they are decoded.
//
// On the console it is sceAudioOut, one 256-frame grain at a time from a thread
// of its own (the call blocks for each grain, which paces the thread). Elsewhere
// a thread takes the samples at the same speed and drops them, so the player's
// clock, which is the audio queue, runs the same.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <pthread.h>
#include <vector>

// The console gives the app one audio port at a time. The interface sounds (gl/ui_sound.cpp) hold it
// while no video plays; the player says it wants it, and they let go.
struct AudioPort {
	static std::atomic<bool> player_wants;
	static std::atomic<bool> ui_holds;
};

class PcmOut {
public:
	static const int kRate = 48000;
	static const int kBytesPerFrame = 4;  // two channels of s16

	PcmOut() = default;
	PcmOut(const PcmOut&) = delete;
	PcmOut& operator=(const PcmOut&) = delete;
	~PcmOut() { close(); }

	bool open();
	void close();
	bool is_open() const { return open_; }

	// Appends samples; the queue grows as needed (the caller keeps it short).
	void queue(const void* data, size_t bytes);
	size_t queued_bytes() const;
	// Paused: silence goes out and the queue stays where it is.
	void pause(bool p) { paused_ = p; }
	void clear();

private:
	static void* thread_main(void* self);
	void run();

	bool open_ = false;
	int handle_ = -1;
	pthread_t thread_{};
	std::atomic<bool> stop_{false};
	std::atomic<bool> paused_{true};
	mutable std::mutex m_;
	std::vector<uint8_t> ring_;  // what is queued, oldest first
	size_t head_ = 0;            // bytes of ring_ already played
};
