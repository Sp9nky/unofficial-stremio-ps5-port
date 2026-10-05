#pragma once

#include <string>
#include <vector>

struct Cue {
	double start = 0, end = 0;
	std::string rml;  // escaped, with <em>/<strong> and <br/>
};

// Parses SubRip, WebVTT, SubStation Alpha and MicroDVD text (UTF-8 already).
// Returns cues sorted by start time.
std::vector<Cue> parse_subtitle_file(const std::string& text);

// Turns one cue's text (SRT/VTT-style markup or ASS override tags, "\N"
// line breaks) into RML.
std::string subtitle_text_to_rml(const std::string& text);

// Text of the cues active at `t` joined with line breaks, "" when none.
std::string cues_at(const std::vector<Cue>& cues, double t);
