#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "json.hpp"

using json = nlohmann::json;

// Logging goes to stdout and to <data dir>/log.txt (opened by log_open).
void log_open(const std::string& path);
void dlog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Strings
std::string trim(const std::string& s);
std::string lower(std::string s);
std::vector<std::string> split(const std::string& s, char sep);
std::string join(const std::vector<std::string>& v, const std::string& sep);
bool starts_with(const std::string& s, const std::string& prefix);
bool ends_with(const std::string& s, const std::string& suffix);
std::string replace_all(std::string s, const std::string& from, const std::string& to);
std::string url_encode(const std::string& s);
std::string initials(const std::string& title);
std::string capitalize(const std::string& s);

// Escapes text for an RML text node (data-rml / {{ }} are fine for plain
// values; this is for strings fed through data-rml).
std::string rml_escape(const std::string& s);

// Text encodings: subtitle files are often not UTF-8.
bool is_valid_utf8(const std::string& s);
std::string cp1253_to_utf8(const std::string& s);   // Greek Windows code page
std::string cp1252_to_utf8(const std::string& s);   // Western Windows code page
std::string to_utf8(const std::string& s, const std::string& lang_hint);

// Time
double now_seconds();                  // monotonic
int64_t now_epoch_ms();                // wall clock
std::string iso8601_now();
int64_t iso8601_to_ms(const std::string& s);  // 0 if unparseable
std::string format_clock();            // "21:45"
std::string format_time(double secs);  // "1:02:03" or "2:03"

// Files
bool file_exists(const std::string& path);
bool make_dirs(const std::string& path);
bool read_file(const std::string& path, std::string& out);
bool write_file(const std::string& path, const std::string& data);  // atomic
bool load_json(const std::string& path, json& out, std::string* err = nullptr);
bool save_json(const std::string& path, const json& j);
std::string path_dir(const std::string& path);
uint64_t fnv1a(const std::string& s);

// JSON helpers that never throw.
std::string jstr(const json& j, const char* key, const std::string& def = "");
double jnum(const json& j, const char* key, double def = 0);
bool jbool(const json& j, const char* key, bool def = false);
const json& jobj(const json& j, const char* key);
