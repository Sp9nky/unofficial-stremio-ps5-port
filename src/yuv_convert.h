// YUV 4:2:0 pictures to the BGRA frames the player shows, at the same size
// or exactly half (4K to the 1080p screen), with the right colours:
//
//   SDR: studio-range YUV with the BT.601 (SD) or BT.709 (HD) matrix;
//   HDR10 (PQ, BT.2020): the BT.2020 matrix, the PQ curve to light, tone
//   mapping (Hable, as FFmpeg's tonemap filter), BT.2020 to BT.709
//   primaries, then the BT.709 curve. Shown as plain SDR, HDR pictures
//   looked pale and purple (PS5 2026-10-06).
//
// Pure C++ with no FFmpeg or SDL, so it can be checked on a PC against
// FFmpeg's own conversion.
#pragma once

#include <cstdint>

struct YuvPicture {
	const uint8_t* y = nullptr;
	const uint8_t* u = nullptr;  // interleaved: the UV plane
	const uint8_t* v = nullptr;  // planar only
	int y_stride = 0, c_stride = 0;  // bytes
	bool interleaved = false;        // NV12-style UV plane
	bool wide = false;               // 16-bit samples
	int shift = 0;                   // wide: right shift to 10-bit values (0 low-aligned, 6 high-aligned)
	int width = 0, height = 0;
};

enum class YuvColors { Bt601, Bt709, Hdr10 };

// Rows [row_begin, row_end) of the out_w x out_h BGRA output; out_w/out_h is
// the picture's size (scale 1) or half of it (scale 2). Safe to call from
// several threads on different rows.
void yuv_to_bgra(const YuvPicture& pic, YuvColors colors, int scale, uint8_t* out, int out_w, int row_begin,
                 int row_end);
