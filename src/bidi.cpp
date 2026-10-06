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

// RML entities used by subtitles (subtitles.cpp) back to text, and the text
// escaped again afterwards.
std::string unescape(const std::string& s) {
	std::string out;
	for (size_t i = 0; i < s.size(); i++) {
		if (s[i] == '&') {
			size_t semi = s.find(';', i);
			if (semi != std::string::npos && semi - i <= 6) {
				std::string e = s.substr(i, semi - i + 1);
				const char* r = e == "&amp;"    ? "&"
				                : e == "&lt;"   ? "<"
				                : e == "&gt;"   ? ">"
				                : e == "&quot;" ? "\""
				                : e == "&apos;" ? "'"
				                : e == "&#39;"  ? "'"
				                : e == "&#123;" ? "{"
				                : e == "&#125;" ? "}"
				                                : nullptr;
				if (r) {
					out += r;
					i = semi;
					continue;
				}
			}
		}
		out += s[i];
	}
	return out;
}

std::string escape(const std::string& s) {
	std::string out;
	for (char c : s) {
		switch (c) {
		case '&': out += "&amp;"; break;
		case '<': out += "&lt;"; break;
		case '>': out += "&gt;"; break;
		case '{': out += "&#123;"; break;
		case '}': out += "&#125;"; break;
		default: out += c;
		}
	}
	return out;
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
	FriBidiParType base = FRIBIDI_PAR_ON;  // from the text itself
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

std::string bidi_visual_rml(const std::string& rml) {
	if (!has_rtl(rml)) return rml;
	std::string out;
	size_t start = 0;
	for (;;) {
		size_t br = rml.find("<br/>", start);
		std::string line = rml.substr(start, br == std::string::npos ? std::string::npos : br - start);
		if (has_rtl(line)) {
			// Text without its tags, reordered; italics/bold for the whole line.
			bool em = line.find("<em>") != std::string::npos, strong = line.find("<strong>") != std::string::npos;
			std::string text;
			for (size_t i = 0; i < line.size(); i++) {
				if (line[i] == '<') {
					size_t end = line.find('>', i);
					if (end != std::string::npos) {
						i = end;
						continue;
					}
				}
				text += line[i];
			}
			line = escape(bidi_visual(unescape(text)));
			if (strong) line = "<strong>" + line + "</strong>";
			if (em) line = "<em>" + line + "</em>";
		}
		out += line;
		if (br == std::string::npos) break;
		out += "<br/>";
		start = br + 5;
	}
	return out;
}
