#include "pcm_out.h"

#include <algorithm>
#include <cstring>
#include <time.h>

#include "util.h"

#ifdef PLATFORM_PS5_NATIVE
extern "C" {
int sceAudioOutInit(void);
int sceAudioOutOpen(int user, int type, int index, unsigned int grain_frames, unsigned int frequency,
                    unsigned int format);
int sceAudioOutOutput(int handle, const void* samples);
int sceAudioOutClose(int handle);
}
#endif

namespace {
const int kGrainFrames = 256;
#ifdef PLATFORM_PS5_NATIVE
const int kAlreadyInitialized = int(0x8026000e);
const int kSystemUser = 0xff;
const int kPortMain = 0;
const unsigned kFormatS16Stereo = 1;
#endif
}  // namespace

std::atomic<bool> AudioPort::player_wants{false};
std::atomic<bool> AudioPort::ui_holds{false};

bool PcmOut::open() {
	if (open_) return true;
#ifdef PLATFORM_PS5_NATIVE
	// The interface may have the port: say we want it, and give it up to 1.5 s to let go.
	AudioPort::player_wants = true;
	for (int i = 0; i < 300 && AudioPort::ui_holds.load(); ++i) {
		timespec ts{0, 5 * 1000 * 1000};
		nanosleep(&ts, nullptr);
	}
	int init = sceAudioOutInit();
	if (init != 0 && init != kAlreadyInitialized) {
		dlog("audio: sceAudioOutInit 0x%08x", unsigned(init));
		AudioPort::player_wants = false;
		return false;
	}
	handle_ = sceAudioOutOpen(kSystemUser, kPortMain, 0, kGrainFrames, kRate, kFormatS16Stereo);
	if (handle_ < 0) {
		dlog("audio: sceAudioOutOpen 0x%08x", unsigned(handle_));
		handle_ = -1;
		AudioPort::player_wants = false;
		return false;
	}
#endif
	{
		std::lock_guard<std::mutex> lock(m_);
		ring_.clear();
		head_ = 0;
	}
	stop_ = false;
	paused_ = true;
	// Never name the thread: pthread_setname_np hangs on the console.
	if (pthread_create(&thread_, nullptr, &PcmOut::thread_main, this) != 0) {
		dlog("audio: can't start the output thread");
#ifdef PLATFORM_PS5_NATIVE
		sceAudioOutClose(handle_);
#endif
		handle_ = -1;
		AudioPort::player_wants = false;
		return false;
	}
	open_ = true;
	return true;
}

void PcmOut::close() {
	if (!open_) return;
	stop_ = true;
	pthread_join(thread_, nullptr);
#ifdef PLATFORM_PS5_NATIVE
	if (handle_ >= 0) sceAudioOutClose(handle_);
#endif
	handle_ = -1;
	open_ = false;
	AudioPort::player_wants = false;  // the interface may have the port back
}

void PcmOut::queue(const void* data, size_t bytes) {
	std::lock_guard<std::mutex> lock(m_);
	// Drop what was played now and then, so the vector does not grow for ever.
	if (head_ > (1u << 20) && head_ * 2 > ring_.size()) {
		ring_.erase(ring_.begin(), ring_.begin() + long(head_));
		head_ = 0;
	}
	const uint8_t* p = static_cast<const uint8_t*>(data);
	ring_.insert(ring_.end(), p, p + bytes);
}

size_t PcmOut::queued_bytes() const {
	std::lock_guard<std::mutex> lock(m_);
	return ring_.size() - head_;
}

void PcmOut::clear() {
	std::lock_guard<std::mutex> lock(m_);
	ring_.clear();
	head_ = 0;
}

void* PcmOut::thread_main(void* self) {
	static_cast<PcmOut*>(self)->run();
	return nullptr;
}

void PcmOut::run() {
	alignas(64) int16_t grain[kGrainFrames * 2];
	const size_t grain_bytes = sizeof(grain);
	while (!stop_) {
		size_t got = 0;
		if (!paused_) {
			std::lock_guard<std::mutex> lock(m_);
			got = std::min(grain_bytes, ring_.size() - head_);
			if (got) {
				std::memcpy(grain, ring_.data() + head_, got);
				head_ += got;
				if (head_ == ring_.size()) {
					ring_.clear();
					head_ = 0;
				}
			}
		}
		if (got < grain_bytes) std::memset(reinterpret_cast<uint8_t*>(grain) + got, 0, grain_bytes - got);
#ifdef PLATFORM_PS5_NATIVE
		if (sceAudioOutOutput(handle_, grain) < 0) {
			struct timespec ts = {0, 5000000};
			nanosleep(&ts, nullptr);
		}
#else
		struct timespec ts = {0, long(1e9 * kGrainFrames / kRate)};
		nanosleep(&ts, nullptr);
#endif
	}
#ifdef PLATFORM_PS5_NATIVE
	sceAudioOutOutput(handle_, nullptr);  // the queued grain plays out
#endif
}
