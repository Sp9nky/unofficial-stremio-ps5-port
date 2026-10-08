// YUV 4:2:0 pictures to the RGBA frames the player shows, at the same size
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

enum class YuvColors { Bt601, Bt709, Hdr10, Hlg };  // Hlg: BT.2020 with the HLG curve, tone mapped like HDR10

// The same for the HDR output (colors must be Hdr10 or Hlg): no tone mapping. The picture stays BT.2020 PQ (HLG
// is converted to PQ, as BT.2100 describes) and each pixel is one 32-bit word, 10 bits per colour: R in bits 9-0,
// G in 19-10, B in 29-20, 3 in 31-30. That is what the HDR scanout and a GL_RGB10_A2 texture read.
void yuv_to_rgb10(const YuvPicture& pic, YuvColors colors, int scale, uint32_t* out, int out_w, int row_begin,
                  int row_end);

// Rows [row_begin, row_end) of the out_w x out_h RGBA output; out_w/out_h is
// the picture's size (scale 1) or half of it (scale 2). Safe to call from
// several threads on different rows.
void yuv_to_rgba(const YuvPicture& pic, YuvColors colors, int scale, uint8_t* out, int out_w, int row_begin,
                 int row_end);
