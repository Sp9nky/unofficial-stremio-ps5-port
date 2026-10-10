#include "bidi.h"

#include <cstdint>
#include <vector>

#ifdef HAVE_FRIBIDI
#include <fribidi/fribidi.h>
#endif

namespace {

std::vector<uint32_t> utf8_decode(const std::string& s) {
	std::vector<uint32_t> out;
	for (size_t i = 0; i < s.size();) {
		unsigned char c = static_cast<unsigned char>(s[i]);
		uint32_t cp;
		int n;
		if (c < 0x80) cp = c, n = 1;
		else if ((c >> 5) == 6) cp = c & 0x1f, n = 2;
		else if ((c >> 4) == 14) cp = c & 0x0f, n = 3;
		else if ((c >> 3) == 30) cp = c & 0x07, n = 4;
		else {
			i++;
			continue;
		}
		if (i + n > s.size()) break;
		for (int k = 1; k < n; k++) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3f);
		out.push_back(cp);
		i += size_t(n);
	}
	return out;
}

void utf8_put(std::string& out, uint32_t cp) {
	if (cp < 0x80) {
		out += char(cp);
	} else if (cp < 0x800) {
		out += char(0xc0 | (cp >> 6));
		out += char(0x80 | (cp & 0x3f));
	} else if (cp < 0x10000) {
		out += char(0xe0 | (cp >> 12));
		out += char(0x80 | ((cp >> 6) & 0x3f));
		out += char(0x80 | (cp & 0x3f));
	} else {
		out += char(0xf0 | (cp >> 18));
		out += char(0x80 | ((cp >> 12) & 0x3f));
		out += char(0x80 | ((cp >> 6) & 0x3f));
		out += char(0x80 | (cp & 0x3f));
	}
}

bool is_rtl(uint32_t cp) {
	return (cp >= 0x0590 && cp <= 0x08ff) || (cp >= 0xfb1d && cp <= 0xfdff) || (cp >= 0xfe70 && cp <= 0xfeff);
}

}  // namespace

bool has_rtl(const std::string& utf8) {
	for (size_t i = 0; i + 1 < utf8.size(); i++) {
		unsigned char c = static_cast<unsigned char>(utf8[i]);
		// U+0590..U+08FF start with 0xD6..0xE0; presentation forms with 0xEF 0xAC..0xBB.
		if ((c >= 0xd6 && c <= 0xdf) || (c == 0xe0 && static_cast<unsigned char>(utf8[i + 1]) <= 0xa3)) return true;
		if (c == 0xef && i + 1 < utf8.size()) {
			unsigned char d = static_cast<unsigned char>(utf8[i + 1]);
			if (d >= 0xac && d <= 0xbb) return true;
		}
	}
	return false;
}

std::string bidi_visual(const std::string& utf8) {
#ifdef HAVE_FRIBIDI
	if (!has_rtl(utf8)) return utf8;
	std::vector<uint32_t> in = utf8_decode(utf8);
	if (in.empty()) return utf8;
	std::vector<FriBidiChar> logical(in.begin(), in.end()), visual(in.size() + 1);
	// The line's direction: right-to-left when most of its letters are (a line that starts with a name in Latin
	// letters but is Arabic or Hebrew all through still reads right to left); else from its first letter.
	size_t rtl = 0, latin = 0;
	for (uint32_t cp : in) {
		if (is_rtl(cp)) rtl++;
		else if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || (cp >= 0xc0 && cp <= 0x24f)) latin++;
	}
	FriBidiParType base = rtl > 0 && rtl >= latin ? FRIBIDI_PAR_RTL : FRIBIDI_PAR_ON;
	// log2vis: bidi reordering plus Arabic shaping (presentation forms,
	// lam-alef ligatures).
	if (!fribidi_log2vis(logical.data(), FriBidiStrIndex(logical.size()), &base, visual.data(), nullptr, nullptr,
	                     nullptr))
		return utf8;
	std::string out;
	for (size_t i = 0; i < logical.size(); i++) {
		uint32_t cp = visual[i];
		if (cp == 0xfeff || cp == 0x200c || cp == 0x200d || cp == 0x200e || cp == 0x200f) continue;  // invisible marks
		utf8_put(out, cp);
	}
	return out;
#else
	return utf8;
#endif
}
