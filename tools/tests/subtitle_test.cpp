// Host check for the subtitle text path: cues keep their logical order (the font reorders right-to-left lines once, when
// drawing), entities and drawings are handled, legacy code pages convert.
#include <cassert>
#include <cstdio>
#include <string>

#include "bidi.h"
#include "subtitles.h"
#include "util.h"

int main() {
	const std::string ar = "\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7 \xd8\xa8\xd8\xa7\xd9\x84\xd8\xb9\xd8\xa7\xd9\x84\xd9\x85";  // مرحبا بالعالم
	auto cues = parse_subtitle_file("1\n00:00:01,000 --> 00:00:02,000\n" + ar + "\n\n");
	assert(cues.size() == 1 && cues[0].rml == ar);  // untouched: bidi happens in the font
	const std::string once = bidi_visual(ar);
	assert(once != ar && bidi_visual(once) != once);  // a second pass would undo/ruin it: it must run exactly once
	// base direction: an Arabic line starting with a Latin name still reads right to left
	assert(bidi_visual("Tom " + ar) != "Tom " + ar);

	auto e = parse_subtitle_file("1\n00:00:01,000 --> 00:00:02,000\nTom&nbsp;&amp;&#233;&#xE9; Jerry\n\n");
	assert(e.size() == 1 && e[0].rml == "Tom &amp;\xc3\xa9\xc3\xa9 Jerry");

	auto a = parse_subtitle_file("[Script Info]\n[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
	                             "Dialogue: 0,0:00:01.00,0:00:02.00,Default,,0,0,0,,{\\p1}m 0 0 l 10 10{\\p0}Hello\\NWorld\n");
	assert(a.size() == 1 && a[0].rml == "Hello<br/>World");

	// Windows-1251 Russian "Привет" and Windows-1250 Polish "żółć"
	assert(to_utf8("\xcf\xf0\xe8\xe2\xe5\xf2", "rus") == "\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82");
	assert(to_utf8("\xbf\xf3\xb3\xe6", "pol") == "\xc5\xbc\xc3\xb3\xc5\x82\xc4\x87");
	puts("subtitle checks OK");
}
