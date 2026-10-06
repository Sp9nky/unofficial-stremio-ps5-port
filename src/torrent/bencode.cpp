#include "bencode.h"

namespace {

const int kMaxDepth = 64;

struct Parser {
	const char* p;
	const char* end;

	bool number(int64_t& out, char stop) {
		bool neg = false;
		if (p < end && *p == '-') neg = true, p++;
		if (p >= end || *p < '0' || *p > '9') return false;
		int64_t v = 0;
		int digits = 0;
		while (p < end && *p >= '0' && *p <= '9') {
			if (++digits > 18) return false;
			v = v * 10 + (*p++ - '0');
		}
		if (p >= end || *p != stop) return false;
		p++;
		out = neg ? -v : v;
		return true;
	}

	bool string(std::string& out) {
		int64_t len;
		if (!number(len, ':') || len < 0 || len > end - p) return false;
		out.assign(p, size_t(len));
		p += len;
		return true;
	}

	bool value(BValue& v, int depth) {
		if (depth > kMaxDepth || p >= end) return false;
		char c = *p;
		if (c == 'i') {
			p++;
			v.type = BValue::Int;
			return number(v.i, 'e');
		}
		if (c == 'l') {
			p++;
			v.type = BValue::List;
			while (p < end && *p != 'e') {
				v.l.emplace_back();
				if (!value(v.l.back(), depth + 1)) return false;
			}
			if (p >= end) return false;
			p++;
			return true;
		}
		if (c == 'd') {
			p++;
			v.type = BValue::Dict;
			while (p < end && *p != 'e') {
				std::string key;
				if (!string(key)) return false;
				if (!value(v.d[key], depth + 1)) return false;
			}
			if (p >= end) return false;
			p++;
			return true;
		}
		if (c >= '0' && c <= '9') {
			v.type = BValue::Str;
			return string(v.s);
		}
		return false;
	}
};

void encode(const BValue& v, std::string& out) {
	switch (v.type) {
	case BValue::Int:
		out += 'i';
		out += std::to_string(v.i);
		out += 'e';
		break;
	case BValue::Str:
		out += std::to_string(v.s.size());
		out += ':';
		out += v.s;
		break;
	case BValue::List:
		out += 'l';
		for (auto& x : v.l) encode(x, out);
		out += 'e';
		break;
	case BValue::Dict:
		out += 'd';
		for (auto& kv : v.d) {  // std::map keeps keys sorted, as bencoding requires
			out += std::to_string(kv.first.size());
			out += ':';
			out += kv.first;
			encode(kv.second, out);
		}
		out += 'e';
		break;
	case BValue::None:
		break;
	}
}

}  // namespace

const BValue* BValue::get(const std::string& key) const {
	if (type != Dict) return nullptr;
	auto it = d.find(key);
	return it == d.end() ? nullptr : &it->second;
}

int64_t BValue::get_int(const std::string& key, int64_t def) const {
	const BValue* v = get(key);
	return v && v->is_int() ? v->i : def;
}

std::string BValue::get_str(const std::string& key) const {
	const BValue* v = get(key);
	return v && v->is_str() ? v->s : std::string();
}

bool bdecode(const char* data, size_t n, BValue& out, size_t* used) {
	Parser ps{data, data + n};
	out = BValue();
	if (!ps.value(out, 0)) return false;
	if (used) *used = size_t(ps.p - data);
	return true;
}

std::string bencode(const BValue& v) {
	std::string out;
	encode(v, out);
	return out;
}
