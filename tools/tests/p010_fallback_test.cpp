// The swscale fallback for 10-bit hardware pictures: low-aligned samples (what the console's decoder gives) read as P010
// came out dark and green; moved up by 6 bits they match the CPU conversion.
#include <cstdio>
#include <cstdlib>
#include <vector>
extern "C" {
#include <libswscale/swscale.h>
}
#include "yuv_convert.h"

int main() {
	const int w = 64, h = 32;
	std::vector<uint16_t> y(w * h), uv(w * h / 2);
	for (int i = 0; i < w * h; i++) y[i] = uint16_t(200 + (i % w) * 10);          // a luma ramp, low-aligned
	for (size_t i = 0; i < uv.size(); i += 2) { uv[i] = 400; uv[i + 1] = 600; }   // some colour
	std::vector<uint8_t> cpu(w * h * 4);
	YuvPicture p;
	p.y = reinterpret_cast<uint8_t*>(y.data());
	p.u = reinterpret_cast<uint8_t*>(uv.data());
	p.y_stride = p.c_stride = w * 2;
	p.interleaved = p.wide = true;
	p.width = w; p.height = h;
	yuv_to_rgba(p, YuvColors::Bt709, 1, cpu.data(), w, 0, h);
	auto run = [&](bool shift) {
		std::vector<uint16_t> ys(y), uvs(uv);
		if (shift) { for (auto& v : ys) v <<= 6; for (auto& v : uvs) v <<= 6; }
		SwsContext* c = sws_getContext(w, h, AV_PIX_FMT_P010LE, w, h, AV_PIX_FMT_RGBA, SWS_POINT, nullptr, nullptr, nullptr);
		sws_setColorspaceDetails(c, sws_getCoefficients(SWS_CS_ITU709), 0, sws_getCoefficients(SWS_CS_ITU709), 0, 0, 1 << 16, 1 << 16);
		const uint8_t* src[4] = {reinterpret_cast<uint8_t*>(ys.data()), reinterpret_cast<uint8_t*>(uvs.data()), nullptr, nullptr};
		int ss[4] = {w * 2, w * 2, 0, 0};
		std::vector<uint8_t> out(w * h * 4);
		uint8_t* dst[4] = {out.data(), nullptr, nullptr, nullptr};
		int ds[4] = {w * 4, 0, 0, 0};
		sws_scale(c, src, ss, 0, h, dst, ds);
		sws_freeContext(c);
		int worst = 0;
		for (size_t i = 0; i < out.size(); i++) if (i % 4 != 3) worst = std::max(worst, std::abs(int(out[i]) - int(cpu[i])));
		return worst;
	};
	const int raw = run(false), fixed = run(true);
	printf("low-aligned read as P010: largest difference from the CPU picture %d; moved up 6 bits: %d\n", raw, fixed);
	puts(fixed < 12 && raw > 40 ? "p010 fallback check OK" : "p010 fallback check FAILED");
	return fixed < 12 && raw > 40 ? 0 : 1;
}
