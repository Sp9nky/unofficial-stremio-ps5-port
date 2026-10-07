// Hardware video decoding on the PS5 (libSceVideodec2): H.264, HEVC (8 and
// 10-bit) and VP9 up to 3840x2176, decoded by the console's video block
// instead of the CPU. The player falls back to FFmpeg's software decoder for
// anything this refuses (other codecs, tiled HEVC, a decoder error).
//
// Ported from Nuvio PS5 (app/engine/media/src/evo_vdec_native.c,
// https://github.com/theghostonline/Nuvio-PS5, GPL-3.0-or-later,
// Copyright (C) 2026 Husam Osman, building on an open-source GPL-3.0 PS5
// media player), which found and fixed each detail below on hardware.
#pragma once

#include <cstdint>
#include <string>

struct AVCodecParameters;

class HwDecoder {
public:
	// A decoded picture: NV12 (8-bit) or two-plane 4:2:0 with 16-bit samples
	// (10-bit). Valid until the next send/receive/flush.
	struct Picture {
		const uint8_t* y = nullptr;
		const uint8_t* uv = nullptr;
		int pitch = 0;           // bytes per row, both planes
		int width = 0, height = 0;
		bool ten_bit = false;
		int64_t pts_us = INT64_MIN;
	};

	// Loads the decoder module; call once at start-up (before the first frame is drawn).
	static void load_module();

	// A decoder for this stream, or nullptr (why says so) when the hardware
	// can't take it.
	static HwDecoder* open(const AVCodecParameters* par, std::string* why);
	~HwDecoder();

	// One packet (pts in microseconds, INT64_MIN unknown); data=nullptr at
	// the end of the stream. 0 taken, 1 receive() first, -1 failed.
	int send(const uint8_t* data, int size, int64_t pts_us);
	// 1 a picture, 0 none yet, -1 failed.
	int receive(Picture* out);
	// After a seek.
	void flush();

	const char* name() const;

	struct Impl;

private:
	HwDecoder() = default;
	Impl* d_ = nullptr;
};
