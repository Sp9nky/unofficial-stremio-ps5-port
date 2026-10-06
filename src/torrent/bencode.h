// Bencoding, BitTorrent's data format (BEP 3): integers, byte strings, lists
// and dictionaries. Used for torrent metadata, tracker answers and the
// extension messages peers exchange.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct BValue {
	enum Type { None, Int, Str, List, Dict } type = None;
	int64_t i = 0;
	std::string s;
	std::vector<BValue> l;
	std::map<std::string, BValue> d;

	BValue() = default;
	explicit BValue(int64_t v) : type(Int), i(v) {}
	explicit BValue(const std::string& v) : type(Str), s(v) {}
	static BValue list() { BValue v; v.type = List; return v; }
	static BValue dict() { BValue v; v.type = Dict; return v; }

	bool is_int() const { return type == Int; }
	bool is_str() const { return type == Str; }
	bool is_list() const { return type == List; }
	bool is_dict() const { return type == Dict; }

	// Dictionary lookup: nullptr when this isn't a dictionary or has no key.
	const BValue* get(const std::string& key) const;
	int64_t get_int(const std::string& key, int64_t def = 0) const;
	std::string get_str(const std::string& key) const;
};

// Decodes one value from data[0..n). On success returns true and sets
// *used to the number of bytes it took (the rest may be other data, as in a
// metadata message). Depth and sizes are bounded: input comes from the net.
bool bdecode(const char* data, size_t n, BValue& out, size_t* used = nullptr);
inline bool bdecode(const std::string& s, BValue& out, size_t* used = nullptr) {
	return bdecode(s.data(), s.size(), out, used);
}

std::string bencode(const BValue& v);
