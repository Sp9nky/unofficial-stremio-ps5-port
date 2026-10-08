#include "yuv_convert.h"

#include <cmath>
#include <mutex>

namespace {

inline uint8_t clamp8(int v) { return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v); }

// A 10-bit sample (8-bit samples are scaled up).
inline int sample(const YuvPicture& p, const uint8_t* row, int x) {
	return p.wide ? (reinterpret_cast<const uint16_t*>(row)[x] >> p.shift) & 1023 : row[x] << 2;
}

// ---------------------------------------------------------------------------
// SDR: integer studio-range matrices, x256, on 8-bit values

struct Matrix {
	int rv, gu, gv, bu;
};
const Matrix kBt709{459, 55, 136, 541}, kBt601{409, 100, 208, 516};

inline void put_sdr(uint8_t* o, int y10, int u10, int v10, const Matrix& m) {
	int c = 298 * ((y10 >> 2) - 16) + 128;
	int u = (u10 >> 2) - 128, v = (v10 >> 2) - 128;
	o[0] = clamp8((c + m.rv * v) >> 8);
	o[1] = clamp8((c - m.gu * u - m.gv * v) >> 8);
	o[2] = clamp8((c + m.bu * u) >> 8);
	o[3] = 255;
}

// ---------------------------------------------------------------------------
// HDR10

// Brightest light, in units of 100 nits: 1000 nits, what HDR10 films and
// series are mastered for (it matched FFmpeg's tonemap reading the stream's
// mastering metadata, on "What If" S02E03).
const float kPeak = 10.0f;
// Light scale before tone mapping. 1.0 came out about half as bright as
// FFmpeg's tonemap; 2.5 matched its brightness on test frames ("What If"
// S02E03, measured), with natural colours.
const float kGain = 2.5f;
const int kOutLutSize = 4096;

float pq_eotf(float e) {  // PQ code value (0..1) to light, units of 100 nits
	const float m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
	float p = std::pow(std::max(e, 0.0f), 1.0f / m2);
	float l = std::pow(std::max(p - c1, 0.0f) / (c2 - c3 * p), 1.0f / m1);
	return l * 10000.0f / 100.0f;
}

float hable(float x) {
	const float a = 0.15f, b = 0.50f, c = 0.10f, d = 0.20f, e = 0.02f, f = 0.30f;
	return (x * (a * x + c * b) + d * e) / (x * (a * x + b) + d * f) - e / f;
}

float bt709_oetf(float l) {
	l = std::min(std::max(l, 0.0f), 1.0f);
	return l < 0.018f ? 4.5f * l : 1.099f * std::pow(l, 0.45f) - 0.099f;
}

// HLG (ARIB STD-B67): signal to scene light 0..1 (the inverse OETF).
float hlg_inverse_oetf(float e) {
	const float a = 0.17883277f, b = 0.28466892f, c = 0.55991073f;
	e = std::min(std::max(e, 0.0f), 1.0f);
	return e <= 0.5f ? e * e / 3.0f : (std::exp((e - c) / a) + b) / 12.0f;
}

// PQ inverse EOTF: light as a fraction of 10000 nits to the PQ signal.
float pq_oetf(float l) {
	const float m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
	float p = std::pow(std::min(std::max(l, 0.0f), 1.0f), m1);
	return std::pow((c1 + c2 * p) / (1.0f + c3 * p), m2);
}

struct HdrTables {
	float pq[1024];            // PQ code (10-bit) to light
	float hlg[1024];           // HLG code (10-bit) to scene light 0..1
	float hlg_pq[4096];        // HLG display light (0..0.1 of 10000 nits, indexed by sqrt) to the PQ signal
	float hlg_ootf[1024];      // HLG system gamma: scene luminance (index/1023) to a light scale, units of 100 nits
	uint8_t out[kOutLutSize];  // SDR light 0..1, indexed by sqrt(light), to 8-bit BT.709
	float hable_norm;
	HdrTables() {
		for (int i = 0; i < 1024; i++) pq[i] = pq_eotf(i / 1023.0f);
		for (int i = 0; i < 1024; i++) hlg[i] = hlg_inverse_oetf(i / 1023.0f);
		// OOTF for a 1000-nit display: light = Ys^0.2 * scene * 1000 nits (BT.2100); units of 100 nits
		for (int i = 0; i < 1024; i++) hlg_ootf[i] = std::pow(i / 1023.0f, 0.2f) * 10.0f;
		for (int i = 0; i < kOutLutSize; i++) {
			float s = i / float(kOutLutSize - 1);
			out[i] = uint8_t(std::lround(bt709_oetf(s * s) * 255.0f));
		}
		for (int i = 0; i < 4096; i++) {
			float s = i / 4095.0f;
			hlg_pq[i] = pq_oetf(s * s * 0.1f);
		}
		hable_norm = 1.0f / hable(kPeak);
	}
};

const HdrTables& hdr_tables() {
	static HdrTables* t = new HdrTables();  // built once, never freed
	return *t;
}

inline uint8_t hdr_out(const HdrTables& t, float l) {  // l: SDR light, 0..1
	if (l <= 0) return t.out[0];
	int i = int(std::sqrt(l) * (kOutLutSize - 1) + 0.5f);
	return t.out[i < kOutLutSize ? i : kOutLutSize - 1];
}

// R'G'B' (0..1, BT.2020) from 10-bit studio-range BT.2020 non-constant luminance YUV
inline void bt2020_rgb(int y10, int u10, int v10, float& rp, float& gp, float& bp) {
	float y = (y10 - 64) * (1.0f / 876.0f);
	float cb = (u10 - 512) * (1.0f / 896.0f), cr = (v10 - 512) * (1.0f / 896.0f);
	rp = y + 1.4746f * cr;
	gp = y - 0.16455f * cb - 0.57135f * cr;
	bp = y + 1.8814f * cb;
}

inline int code10(float e) {
	int i = int(e * 1023.0f + 0.5f);
	return i < 0 ? 0 : i > 1023 ? 1023 : i;
}

// r, g, b: BT.2020 light in units of 100 nits (already scaled by kGain)
inline void tonemap_out(const HdrTables& t, uint8_t* o, float r, float g, float b) {
	// BT.2020 to BT.709 primaries, in light
	float r7 = 1.6605f * r - 0.5876f * g - 0.0728f * b;
	float g7 = -0.1246f * r + 1.1329f * g - 0.0083f * b;
	float b7 = -0.0182f * r - 0.1006f * g + 1.1187f * b;
	// Tone mapping on the brightest channel, the same factor for all three
	// (as FFmpeg's tonemap filter): keeps hue and saturation.
	float sig = std::max(std::max(r7, g7), std::max(b7, 1e-6f));
	float k = hable(sig) * t.hable_norm / sig;
	r7 *= k;
	g7 *= k;
	b7 *= k;
	o[0] = hdr_out(t, r7);
	o[1] = hdr_out(t, g7);
	o[2] = hdr_out(t, b7);
	o[3] = 255;
}

inline void put_hdr(const HdrTables& t, uint8_t* o, int y10, int u10, int v10) {
	float rp, gp, bp;
	bt2020_rgb(y10, u10, v10, rp, gp, bp);
	tonemap_out(t, o, t.pq[code10(rp)] * kGain, t.pq[code10(gp)] * kGain, t.pq[code10(bp)] * kGain);
}

inline void put_hlg(const HdrTables& t, uint8_t* o, int y10, int u10, int v10) {
	float rp, gp, bp;
	bt2020_rgb(y10, u10, v10, rp, gp, bp);
	float r = t.hlg[code10(rp)], g = t.hlg[code10(gp)], b = t.hlg[code10(bp)];
	// the HLG system gamma works on the scene luminance
	float ys = 0.2627f * r + 0.6780f * g + 0.0593f * b;
	float k = t.hlg_ootf[code10(ys)] * kGain;
	tonemap_out(t, o, r * k, g * k, b * k);
}

}  // namespace

void yuv_to_rgba(const YuvPicture& p, YuvColors colors, int scale, uint8_t* out, int out_w, int row_begin,
                 int row_end) {
	const bool hlg = colors == YuvColors::Hlg;
	const HdrTables* t = (colors == YuvColors::Hdr10 || hlg) ? &hdr_tables() : nullptr;
	const Matrix& m = colors == YuvColors::Bt601 ? kBt601 : kBt709;
	for (int oy = row_begin; oy < row_end; oy++) {
		uint8_t* o = out + size_t(oy) * out_w * 4;
		int sy = scale == 2 ? oy * 2 : oy;
		const uint8_t* y0 = p.y + size_t(sy) * p.y_stride;
		const uint8_t* y1 = scale == 2 ? y0 + p.y_stride : y0;
		const uint8_t* cu = p.u + size_t(sy / 2) * p.c_stride;
		const uint8_t* cv = p.interleaved ? cu : p.v + size_t(sy / 2) * p.c_stride;
		for (int ox = 0; ox < out_w; ox++, o += 4) {
			int x = scale == 2 ? ox * 2 : ox;
			int yv = scale == 2
			             ? (sample(p, y0, x) + sample(p, y0, x + 1) + sample(p, y1, x) + sample(p, y1, x + 1) + 2) >> 2
			             : sample(p, y0, x);
			int cx = x / 2;
			int uv = p.interleaved ? sample(p, cu, cx * 2) : sample(p, cu, cx);
			int vv = p.interleaved ? sample(p, cv, cx * 2 + 1) : sample(p, cv, cx);
			if (t) hlg ? put_hlg(*t, o, yv, uv, vv) : put_hdr(*t, o, yv, uv, vv);
			else put_sdr(o, yv, uv, vv, m);
		}
	}
}

void yuv_to_rgb10(const YuvPicture& p, YuvColors colors, int scale, uint32_t* out, int out_w, int row_begin,
                  int row_end) {
	const HdrTables& t = hdr_tables();
	const bool hlg = colors == YuvColors::Hlg;
	for (int oy = row_begin; oy < row_end; oy++) {
		uint32_t* o = out + size_t(oy) * out_w;
		int sy = scale == 2 ? oy * 2 : oy;
		const uint8_t* y0 = p.y + size_t(sy) * p.y_stride;
		const uint8_t* y1 = scale == 2 ? y0 + p.y_stride : y0;
		const uint8_t* cu = p.u + size_t(sy / 2) * p.c_stride;
		const uint8_t* cv = p.interleaved ? cu : p.v + size_t(sy / 2) * p.c_stride;
		for (int ox = 0; ox < out_w; ox++) {
			int x = scale == 2 ? ox * 2 : ox;
			int yv = scale == 2
			             ? (sample(p, y0, x) + sample(p, y0, x + 1) + sample(p, y1, x) + sample(p, y1, x + 1) + 2) >> 2
			             : sample(p, y0, x);
			int cx = x / 2;
			int uv = p.interleaved ? sample(p, cu, cx * 2) : sample(p, cu, cx);
			int vv = p.interleaved ? sample(p, cv, cx * 2 + 1) : sample(p, cv, cx);
			float rp, gp, bp;
			bt2020_rgb(yv, uv, vv, rp, gp, bp);
			int r, g, b;
			if (!hlg) {  // HDR10: the signal is PQ already
				r = code10(rp);
				g = code10(gp);
				b = code10(bp);
			} else {
				float er = t.hlg[code10(rp)], eg = t.hlg[code10(gp)], eb = t.hlg[code10(bp)];
				float ys = 0.2627f * er + 0.6780f * eg + 0.0593f * eb;
				float k = t.hlg_ootf[code10(ys)] * 0.01f;  // system gamma, for a 1000-nit display, in 10000-nit units
				auto enc = [&](float e) {
					float d = std::min(e * k, 0.1f);
					int i = int(std::sqrt(d * 10.0f) * 4095.0f + 0.5f);
					return code10(t.hlg_pq[i < 4096 ? i : 4095]);
				};
				r = enc(er);
				g = enc(eg);
				b = enc(eb);
			}
			o[ox] = uint32_t(r) | (uint32_t(g) << 10) | (uint32_t(b) << 20) | (3u << 30);
		}
	}
}
