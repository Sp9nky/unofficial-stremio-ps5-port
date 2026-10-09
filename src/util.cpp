#include "util.h"

#include <sys/stat.h>
#include <sys/time.h>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>

#ifdef PLATFORM_PS5
extern "C" int sceKernelDebugOutText(int channel, const char* text);
#endif

static FILE* g_log = nullptr;
static std::mutex g_log_mutex;

void log_open(const std::string& path) {
	std::lock_guard<std::mutex> lock(g_log_mutex);
	if (g_log) fclose(g_log);
	g_log = fopen(path.c_str(), "w");
}

void dlog(const char* fmt, ...) {
	char buf[2048];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	std::lock_guard<std::mutex> lock(g_log_mutex);
#ifdef PLATFORM_PS5
	// The kernel log (klogsrv, port 3232).
	char line[2100];
	snprintf(line, sizeof(line), "[stremio] %s\n", buf);
	sceKernelDebugOutText(0, line);
#else
	printf("[stremio] %s\n", buf);
	fflush(stdout);
#endif
	if (g_log) {
		fprintf(g_log, "%s\n", buf);
		fflush(g_log);
	}
}

// ---------------------------------------------------------------------------
// Strings

std::string trim(const std::string& s) {
	size_t a = s.find_first_not_of(" \t\r\n");
	if (a == std::string::npos) return "";
	size_t b = s.find_last_not_of(" \t\r\n");
	return s.substr(a, b - a + 1);
}

std::string lower(std::string s) {
	for (auto& c : s)
		if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
	return s;
}

std::vector<std::string> split(const std::string& s, char sep) {
	std::vector<std::string> out;
	size_t start = 0;
	while (true) {
		size_t p = s.find(sep, start);
		if (p == std::string::npos) {
			out.push_back(s.substr(start));
			break;
		}
		out.push_back(s.substr(start, p - start));
		start = p + 1;
	}
	return out;
}

std::string join(const std::vector<std::string>& v, const std::string& sep) {
	std::string out;
	for (size_t i = 0; i < v.size(); i++) {
		if (i) out += sep;
		out += v[i];
	}
	return out;
}

bool starts_with(const std::string& s, const std::string& prefix) {
	return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(const std::string& s, const std::string& suffix) {
	return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string replace_all(std::string s, const std::string& from, const std::string& to) {
	if (from.empty()) return s;
	size_t p = 0;
	while ((p = s.find(from, p)) != std::string::npos) {
		s.replace(p, from.size(), to);
		p += to.size();
	}
	return s;
}

std::string url_encode(const std::string& s) {
	static const char* hex = "0123456789ABCDEF";
	std::string out;
	for (unsigned char c : s) {
		if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
			out += char(c);
		} else {
			out += '%';
			out += hex[c >> 4];
			out += hex[c & 15];
		}
	}
	return out;
}

std::string initials(const std::string& title) {
	// The placeholder poster shows the whole title; it wraps inside the box.
	return title;
}

std::string capitalize(const std::string& s) {
	if (s.empty()) return s;
	std::string out = s;
	if (out[0] >= 'a' && out[0] <= 'z') out[0] = char(out[0] - 'a' + 'A');
	return out;
}

std::string rml_escape(const std::string& s) {
	std::string out;
	out.reserve(s.size() + 16);
	for (char c : s) {
		switch (c) {
		case '&': out += "&amp;"; break;
		case '<': out += "&lt;"; break;
		case '>': out += "&gt;"; break;
		case '"': out += "&quot;"; break;
		case '{': out += "&#123;"; break;
		case '}': out += "&#125;"; break;
		default: out += c;
		}
	}
	return out;
}

// ---------------------------------------------------------------------------
// Encodings

bool is_valid_utf8(const std::string& s) {
	const unsigned char* p = (const unsigned char*)s.data();
	size_t n = s.size(), i = 0;
	while (i < n) {
		unsigned char c = p[i];
		int len;
		if (c < 0x80) len = 1;
		else if ((c & 0xE0) == 0xC0) len = 2;
		else if ((c & 0xF0) == 0xE0) len = 3;
		else if ((c & 0xF8) == 0xF0) len = 4;
		else return false;
		if (i + len > n) return false;
		for (int k = 1; k < len; k++)
			if ((p[i + k] & 0xC0) != 0x80) return false;
		i += len;
	}
	return true;
}

static void put_utf8(std::string& out, uint32_t cp) {
	if (cp < 0x80) {
		out += char(cp);
	} else if (cp < 0x800) {
		out += char(0xC0 | (cp >> 6));
		out += char(0x80 | (cp & 0x3F));
	} else {
		out += char(0xE0 | (cp >> 12));
		out += char(0x80 | ((cp >> 6) & 0x3F));
		out += char(0x80 | (cp & 0x3F));
	}
}

static const uint16_t kCp1252High[32] = {
	0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
	0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD,
	0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
	0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178,
};

static const uint16_t kCp1253A0[32] = {
	0x00A0, 0x0385, 0x0386, 0x00A3, 0x00A4, 0x00A5, 0x00A6, 0x00A7,
	0x00A8, 0x00A9, 0xFFFD, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x2015,
	0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x0384, 0x00B5, 0x00B6, 0x00B7,
	0x0388, 0x0389, 0x038A, 0x00BB, 0x038C, 0x00BD, 0x038E, 0x038F,
};

std::string cp1252_to_utf8(const std::string& s) {
	std::string out;
	out.reserve(s.size() * 2);
	for (unsigned char c : s) {
		if (c < 0x80) out += char(c);
		else if (c < 0xA0) put_utf8(out, kCp1252High[c - 0x80]);
		else put_utf8(out, c);
	}
	return out;
}

std::string cp1253_to_utf8(const std::string& s) {
	std::string out;
	out.reserve(s.size() * 2);
	for (unsigned char c : s) {
		if (c < 0x80) out += char(c);
		else if (c < 0xA0) {
			// Shared punctuation with 1252 except a few that 1253 leaves undefined.
			uint16_t cp = kCp1252High[c - 0x80];
			if (c == 0x88 || c == 0x8A || c == 0x8C || c == 0x8E || c == 0x98 || c == 0x9A ||
			    c == 0x9C || c == 0x9E || c == 0x9F)
				cp = 0xFFFD;
			put_utf8(out, cp);
		} else if (c < 0xC0) put_utf8(out, kCp1253A0[c - 0xA0]);
		else if (c == 0xD2 || c == 0xFF) put_utf8(out, 0xFFFD);
		else put_utf8(out, 0x0390 + (c - 0xC0));
	}
	return out;
}

// Windows-1256 (Arabic, Persian, Urdu), 0x80-0xFF: generated from Python's cp1256 codec.
static const uint16_t kCp1256High[128] = {
	0x20AC, 0x067E, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
	0x02C6, 0x2030, 0x0679, 0x2039, 0x0152, 0x0686, 0x0698, 0x0688,
	0x06AF, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
	0x06A9, 0x2122, 0x0691, 0x203A, 0x0153, 0x200C, 0x200D, 0x06BA,
	0x00A0, 0x060C, 0x00A2, 0x00A3, 0x00A4, 0x00A5, 0x00A6, 0x00A7,
	0x00A8, 0x00A9, 0x06BE, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x00AF,
	0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x00B4, 0x00B5, 0x00B6, 0x00B7,
	0x00B8, 0x00B9, 0x061B, 0x00BB, 0x00BC, 0x00BD, 0x00BE, 0x061F,
	0x06C1, 0x0621, 0x0622, 0x0623, 0x0624, 0x0625, 0x0626, 0x0627,
	0x0628, 0x0629, 0x062A, 0x062B, 0x062C, 0x062D, 0x062E, 0x062F,
	0x0630, 0x0631, 0x0632, 0x0633, 0x0634, 0x0635, 0x0636, 0x00D7,
	0x0637, 0x0638, 0x0639, 0x063A, 0x0640, 0x0641, 0x0642, 0x0643,
	0x00E0, 0x0644, 0x00E2, 0x0645, 0x0646, 0x0647, 0x0648, 0x00E7,
	0x00E8, 0x00E9, 0x00EA, 0x00EB, 0x0649, 0x064A, 0x00EE, 0x00EF,
	0x064B, 0x064C, 0x064D, 0x064E, 0x00F4, 0x064F, 0x0650, 0x00F7,
	0x0651, 0x00F9, 0x0652, 0x00FB, 0x00FC, 0x200E, 0x200F, 0x06D2,
};

static std::string cp1256_to_utf8(const std::string& s) {
	std::string out;
	out.reserve(s.size() * 2);
	for (unsigned char c : s) {
		if (c < 0x80) out += char(c);
		else put_utf8(out, kCp1256High[c - 0x80]);
	}
	return out;
}

// Windows-874 (Thai; TIS-620 plus punctuation in 0x80-0x9F), 0x80-0xFF:
// generated from Python's cp874 codec.
static const uint16_t kCp874High[128] = {
	0x20AC, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0x2026, 0xFFFD, 0xFFFD,
	0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD,
	0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
	0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD,
	0x00A0, 0x0E01, 0x0E02, 0x0E03, 0x0E04, 0x0E05, 0x0E06, 0x0E07,
	0x0E08, 0x0E09, 0x0E0A, 0x0E0B, 0x0E0C, 0x0E0D, 0x0E0E, 0x0E0F,
	0x0E10, 0x0E11, 0x0E12, 0x0E13, 0x0E14, 0x0E15, 0x0E16, 0x0E17,
	0x0E18, 0x0E19, 0x0E1A, 0x0E1B, 0x0E1C, 0x0E1D, 0x0E1E, 0x0E1F,
	0x0E20, 0x0E21, 0x0E22, 0x0E23, 0x0E24, 0x0E25, 0x0E26, 0x0E27,
	0x0E28, 0x0E29, 0x0E2A, 0x0E2B, 0x0E2C, 0x0E2D, 0x0E2E, 0x0E2F,
	0x0E30, 0x0E31, 0x0E32, 0x0E33, 0x0E34, 0x0E35, 0x0E36, 0x0E37,
	0x0E38, 0x0E39, 0x0E3A, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0x0E3F,
	0x0E40, 0x0E41, 0x0E42, 0x0E43, 0x0E44, 0x0E45, 0x0E46, 0x0E47,
	0x0E48, 0x0E49, 0x0E4A, 0x0E4B, 0x0E4C, 0x0E4D, 0x0E4E, 0x0E4F,
	0x0E50, 0x0E51, 0x0E52, 0x0E53, 0x0E54, 0x0E55, 0x0E56, 0x0E57,
	0x0E58, 0x0E59, 0x0E5A, 0x0E5B, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD,
};

std::string cp874_to_utf8(const std::string& s) {
	std::string out;
	out.reserve(s.size() * 2);
	for (unsigned char c : s) {
		if (c < 0x80) out += char(c);
		else put_utf8(out, kCp874High[c - 0x80]);
	}
	return out;
}

std::string to_utf8(const std::string& in, const std::string& lang_hint) {
	std::string s = in;
	// Strip a UTF-8 byte order mark.
	if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
		s = s.substr(3);
	// UTF-16 LE/BE with BOM.
	if (s.size() >= 2 && (((unsigned char)s[0] == 0xFF && (unsigned char)s[1] == 0xFE) ||
	                      ((unsigned char)s[0] == 0xFE && (unsigned char)s[1] == 0xFF))) {
		bool le = (unsigned char)s[0] == 0xFF;
		std::string out;
		for (size_t i = 2; i + 1 < s.size(); i += 2) {
			uint32_t cp = le ? ((unsigned char)s[i] | ((unsigned char)s[i + 1] << 8))
			                 : (((unsigned char)s[i] << 8) | (unsigned char)s[i + 1]);
			if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;  // no surrogate pairs in subtitles worth keeping
			put_utf8(out, cp);
		}
		return out;
	}
	if (is_valid_utf8(s)) return s;
	std::string l = lower(lang_hint);
	if (l == "gre" || l == "ell" || l == "el" || l == "greek") return cp1253_to_utf8(s);
	if (l == "ara" || l == "ar" || l == "arabic" || l == "per" || l == "fas" || l == "fa" || l == "urd" || l == "ur")
		return cp1256_to_utf8(s);
	if (l == "tha" || l == "th" || l == "thai") return cp874_to_utf8(s);
	// Guess Greek when the high bytes are mostly in the Greek letter range.
	size_t high = 0, greek = 0, thai = 0;
	for (unsigned char c : s) {
		if (c >= 0x80) {
			high++;
			if (c >= 0xC1 && c <= 0xFE) greek++;
			if (c >= 0xA1 && c <= 0xFB) thai++;
		}
	}
	if (high > 20 && greek * 10 > high * 8 && l.empty()) return cp1253_to_utf8(s);
	// Guess Thai when the high bytes are mostly in the Thai letter range.
	if (high > 20 && thai * 10 > high * 8 && l.empty()) return cp874_to_utf8(s);
	return cp1252_to_utf8(s);
}

// ---------------------------------------------------------------------------
// Time

double now_seconds() {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return double(ts.tv_sec) + double(ts.tv_nsec) / 1e9;
}

int64_t now_epoch_ms() {
	struct timeval tv;
	gettimeofday(&tv, nullptr);
	return int64_t(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
}

std::string iso8601_now() {
	int64_t ms = now_epoch_ms();
	time_t t = time_t(ms / 1000);
	struct tm tm;
	gmtime_r(&t, &tm);
	char buf[64];
	snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1,
	         tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, int(ms % 1000));
	return buf;
}

int64_t iso8601_to_ms(const std::string& s) {
	int y, mo, d, h = 0, mi = 0;
	double sec = 0;
	if (sscanf(s.c_str(), "%d-%d-%dT%d:%d:%lf", &y, &mo, &d, &h, &mi, &sec) < 3) return 0;
	struct tm tm;
	memset(&tm, 0, sizeof(tm));
	tm.tm_year = y - 1900;
	tm.tm_mon = mo - 1;
	tm.tm_mday = d;
	tm.tm_hour = h;
	tm.tm_min = mi;
	tm.tm_sec = int(sec);
	time_t t = timegm(&tm);
	return int64_t(t) * 1000 + int64_t((sec - int(sec)) * 1000);
}

std::string format_clock() {
	time_t t = time(nullptr);
	struct tm tm;
	localtime_r(&t, &tm);
	char buf[16];
	snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
	return buf;
}

std::string format_time(double secs) {
	if (secs < 0) secs = 0;
	long t = long(secs);
	char buf[32];
	if (t >= 3600) snprintf(buf, sizeof(buf), "%ld:%02ld:%02ld", t / 3600, (t / 60) % 60, t % 60);
	else snprintf(buf, sizeof(buf), "%ld:%02ld", t / 60, t % 60);
	return buf;
}

// ---------------------------------------------------------------------------
// Files

bool file_exists(const std::string& path) {
	struct stat st;
	return stat(path.c_str(), &st) == 0;
}

bool make_dirs(const std::string& path) {
	if (path.empty()) return false;
	std::string cur;
	for (size_t i = 0; i < path.size(); i++) {
		cur += path[i];
		if ((path[i] == '/' && i > 0) || i == path.size() - 1) {
			if (mkdir(cur.c_str(), 0777) != 0 && errno != EEXIST) {
				struct stat st;
				if (stat(cur.c_str(), &st) != 0) return false;
			}
		}
	}
	return true;
}

bool read_file(const std::string& path, std::string& out) {
	FILE* f = fopen(path.c_str(), "rb");
	if (!f) return false;
	out.clear();
	char buf[65536];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
	fclose(f);
	return true;
}

bool write_file(const std::string& path, const std::string& data) {
	std::string tmp = path + ".tmp";
	FILE* f = fopen(tmp.c_str(), "wb");
	if (!f) return false;
	bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
	ok = (fclose(f) == 0) && ok;
	if (!ok) {
		remove(tmp.c_str());
		return false;
	}
	if (rename(tmp.c_str(), path.c_str()) != 0) {
		// Some file systems refuse to rename over an existing file.
		remove(path.c_str());
		if (rename(tmp.c_str(), path.c_str()) != 0) return false;
	}
	return true;
}

bool load_json(const std::string& path, json& out, std::string* err) {
	std::string data;
	if (!read_file(path, data)) {
		if (err) *err = "not found";
		return false;
	}
	out = json::parse(data, nullptr, false);
	if (out.is_discarded()) {
		if (err) *err = "invalid JSON";
		out = json();
		return false;
	}
	return true;
}

bool save_json(const std::string& path, const json& j) {
	return write_file(path, j.dump(1, '\t'));
}

std::string path_dir(const std::string& path) {
	size_t p = path.find_last_of('/');
	if (p == std::string::npos) return ".";
	if (p == 0) return "/";
	return path.substr(0, p);
}

uint64_t fnv1a(const std::string& s) {
	uint64_t h = 1469598103934665603ULL;
	for (unsigned char c : s) {
		h ^= c;
		h *= 1099511628211ULL;
	}
	return h;
}

// ---------------------------------------------------------------------------
// JSON

std::string jstr(const json& j, const char* key, const std::string& def) {
	if (!j.is_object()) return def;
	auto it = j.find(key);
	if (it == j.end()) return def;
	if (it->is_string()) return it->get<std::string>();
	if (it->is_number_integer()) return std::to_string(it->get<long long>());
	if (it->is_number()) {
		char buf[64];
		snprintf(buf, sizeof(buf), "%g", it->get<double>());
		return buf;
	}
	return def;
}

double jnum(const json& j, const char* key, double def) {
	if (!j.is_object()) return def;
	auto it = j.find(key);
	if (it == j.end()) return def;
	if (it->is_number()) return it->get<double>();
	if (it->is_string()) {
		const std::string& s = it->get_ref<const std::string&>();
		char* end = nullptr;
		double v = strtod(s.c_str(), &end);
		if (end != s.c_str()) return v;
	}
	return def;
}

bool jbool(const json& j, const char* key, bool def) {
	if (!j.is_object()) return def;
	auto it = j.find(key);
	if (it == j.end() || !it->is_boolean()) return def;
	return it->get<bool>();
}

const json& jobj(const json& j, const char* key) {
	static const json null_json;
	if (!j.is_object()) return null_json;
	auto it = j.find(key);
	if (it == j.end()) return null_json;
	return *it;
}
