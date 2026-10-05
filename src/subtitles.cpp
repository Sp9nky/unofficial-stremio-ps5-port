#include "subtitles.h"

#include <algorithm>
#include <cstdlib>

#include "util.h"

// "00:01:02,345" / "01:02.345" / "0:01:02.34" -> seconds; -1 if not a time.
static double parse_timestamp(const std::string& s) {
	std::vector<double> parts;
	std::string cur;
	for (char c : s) {
		if (c == ':') {
			if (cur.empty()) return -1;
			parts.push_back(atof(cur.c_str()));
			cur.clear();
		} else if (isdigit((unsigned char)c) || c == '.' || c == ',') {
			cur += (c == ',') ? '.' : c;
		} else {
			return -1;
		}
	}
	if (cur.empty()) return -1;
	parts.push_back(atof(cur.c_str()));
	if (parts.size() < 2 || parts.size() > 3) return -1;
	double t = 0;
	for (double p : parts) t = t * 60 + p;
	return t;
}

std::string subtitle_text_to_rml(const std::string& in) {
	std::string out;
	bool italic = false, bold = false;
	size_t i = 0;
	while (i < in.size()) {
		char c = in[i];
		if (c == '{' && i + 1 < in.size() && in[i + 1] == '\\') {
			// ASS override block: honour italics/bold, drop the rest.
			size_t end = in.find('}', i);
			if (end == std::string::npos) break;
			std::string tags = in.substr(i, end - i);
			if (tags.find("\\i1") != std::string::npos && !italic) out += "<em>", italic = true;
			if (tags.find("\\i0") != std::string::npos && italic) out += "</em>", italic = false;
			if (tags.find("\\b1") != std::string::npos && !bold) out += "<strong>", bold = true;
			if (tags.find("\\b0") != std::string::npos && bold) out += "</strong>", bold = false;
			i = end + 1;
			continue;
		}
		if (c == '{' && i + 1 < in.size() && in[i + 1] == 'y' && in.find('}', i) != std::string::npos) {
			// MicroDVD {y:i}
			size_t end = in.find('}', i);
			if (in.compare(i, 4, "{y:i") == 0 && !italic) out += "<em>", italic = true;
			i = end + 1;
			continue;
		}
		if (c == '\\' && i + 1 < in.size() && (in[i + 1] == 'N' || in[i + 1] == 'n')) {
			out += "<br/>";
			i += 2;
			continue;
		}
		if (c == '\\' && i + 1 < in.size() && in[i + 1] == 'h') {
			out += ' ';
			i += 2;
			continue;
		}
		if (c == '<') {
			size_t end = in.find('>', i);
			if (end != std::string::npos) {
				std::string tag = lower(in.substr(i + 1, end - i - 1));
				if (tag == "i" && !italic) out += "<em>", italic = true;
				else if (tag == "/i" && italic) out += "</em>", italic = false;
				else if (tag == "b" && !bold) out += "<strong>", bold = true;
				else if (tag == "/b" && bold) out += "</strong>", bold = false;
				else if (tag == "br" || tag == "br/" || tag == "br /") out += "<br/>";
				// <font>, <u>, <c.yellow>, <v Name>, VTT timestamps: dropped.
				i = end + 1;
				continue;
			}
		}
		if (c == '\r') {
			i++;
			continue;
		}
		if (c == '\n') {
			out += "<br/>";
			i++;
			continue;
		}
		switch (c) {
		case '&': out += "&amp;"; break;
		case '<': out += "&lt;"; break;
		case '>': out += "&gt;"; break;
		case '{': out += "&#123;"; break;
		case '}': out += "&#125;"; break;
		default: out += c;
		}
		i++;
	}
	if (bold) out += "</strong>";
	if (italic) out += "</em>";
	// Collapse leading/trailing breaks.
	while (starts_with(out, "<br/>")) out = out.substr(5);
	while (ends_with(out, "<br/>")) out = out.substr(0, out.size() - 5);
	return out;
}

static std::vector<std::string> lines_of(const std::string& text) {
	std::vector<std::string> lines;
	std::string cur;
	for (char c : text) {
		if (c == '\n') {
			if (!cur.empty() && cur.back() == '\r') cur.pop_back();
			lines.push_back(cur);
			cur.clear();
		} else {
			cur += c;
		}
	}
	if (!cur.empty()) lines.push_back(cur);
	return lines;
}

static std::vector<Cue> parse_srt_vtt(const std::vector<std::string>& lines) {
	std::vector<Cue> cues;
	for (size_t i = 0; i < lines.size(); i++) {
		size_t arrow = lines[i].find("-->");
		if (arrow == std::string::npos) continue;
		std::string a = trim(lines[i].substr(0, arrow));
		std::string b = trim(lines[i].substr(arrow + 3));
		size_t sp = b.find_first_of(" \t");  // VTT cue settings
		if (sp != std::string::npos) b = b.substr(0, sp);
		double start = parse_timestamp(a), end = parse_timestamp(b);
		if (start < 0 || end < 0) continue;
		std::string text;
		size_t j = i + 1;
		for (; j < lines.size(); j++) {
			if (trim(lines[j]).empty()) break;
			if (lines[j].find("-->") != std::string::npos) break;  // missing blank line
			if (!text.empty()) text += "\n";
			text += lines[j];
		}
		// A cue number right before the next timing line belongs to it, not to us.
		if (j < lines.size() && lines[j].find("-->") != std::string::npos && !text.empty()) {
			size_t nl = text.find_last_of('\n');
			std::string last = nl == std::string::npos ? text : text.substr(nl + 1);
			if (!last.empty() && last.find_first_not_of("0123456789") == std::string::npos)
				text = nl == std::string::npos ? "" : text.substr(0, nl);
		}
		i = j - 1;
		Cue c;
		c.start = start;
		c.end = end;
		c.rml = subtitle_text_to_rml(text);
		if (!c.rml.empty()) cues.push_back(c);
	}
	return cues;
}

static std::vector<Cue> parse_ass(const std::vector<std::string>& lines) {
	std::vector<Cue> cues;
	int text_field = 9, start_field = 1, end_field = 2;
	for (auto& line : lines) {
		if (starts_with(line, "Format:") && !cues.size()) {
			auto fields = split(line.substr(7), ',');
			for (size_t k = 0; k < fields.size(); k++) {
				std::string f = lower(trim(fields[k]));
				if (f == "start") start_field = int(k);
				else if (f == "end") end_field = int(k);
				else if (f == "text") text_field = int(k);
			}
			continue;
		}
		if (!starts_with(line, "Dialogue:")) continue;
		std::string rest = line.substr(9);
		std::vector<std::string> fields;
		size_t pos = 0;
		for (int k = 0; k < text_field; k++) {
			size_t comma = rest.find(',', pos);
			if (comma == std::string::npos) break;
			fields.push_back(rest.substr(pos, comma - pos));
			pos = comma + 1;
		}
		if (int(fields.size()) < text_field) continue;
		Cue c;
		c.start = parse_timestamp(trim(fields[start_field]));
		c.end = parse_timestamp(trim(fields[end_field]));
		if (c.start < 0 || c.end < 0) continue;
		c.rml = subtitle_text_to_rml(rest.substr(pos));
		if (!c.rml.empty()) cues.push_back(c);
	}
	return cues;
}

static std::vector<Cue> parse_microdvd(const std::vector<std::string>& lines) {
	// {start}{end}text with frame numbers; assume 23.976 fps unless {1}{1}fps.
	std::vector<Cue> cues;
	double fps = 23.976;
	for (auto& line : lines) {
		if (line.size() < 6 || line[0] != '{') continue;
		size_t a = line.find('}');
		if (a == std::string::npos || a + 1 >= line.size() || line[a + 1] != '{') continue;
		size_t b = line.find('}', a + 1);
		if (b == std::string::npos) continue;
		long f0 = atol(line.substr(1, a - 1).c_str());
		long f1 = atol(line.substr(a + 2, b - a - 2).c_str());
		std::string text = line.substr(b + 1);
		if (f0 == 1 && f1 == 1) {
			double v = atof(text.c_str());
			if (v > 1) fps = v;
			continue;
		}
		Cue c;
		c.start = f0 / fps;
		c.end = f1 / fps;
		c.rml = subtitle_text_to_rml(replace_all(text, "|", "\n"));
		if (!c.rml.empty()) cues.push_back(c);
	}
	return cues;
}

std::vector<Cue> parse_subtitle_file(const std::string& text) {
	auto lines = lines_of(text);
	std::vector<Cue> cues;
	bool ass = text.find("[Script Info]") != std::string::npos || text.find("\nDialogue:") != std::string::npos;
	if (ass) cues = parse_ass(lines);
	if (cues.empty()) cues = parse_srt_vtt(lines);
	if (cues.empty()) cues = parse_microdvd(lines);
	std::stable_sort(cues.begin(), cues.end(), [](const Cue& a, const Cue& b) { return a.start < b.start; });
	return cues;
}

std::string cues_at(const std::vector<Cue>& cues, double t) {
	// Cues are sorted by start; find the last one that started, then look
	// back for overlapping ones (rarely more than two).
	auto it = std::upper_bound(cues.begin(), cues.end(), t, [](double v, const Cue& c) { return v < c.start; });
	std::string out;
	int looked = 0;
	while (it != cues.begin() && looked < 12) {
		--it;
		looked++;
		if (it->end > t) out = out.empty() ? it->rml : it->rml + "<br/>" + out;
	}
	return out;
}
