#include "watched.hpp"

#include <algorithm>
#include <cstring>

namespace sx::watched
{

namespace
{

// ---- base64 ------------------------------------------------------------------------------------------------

const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string b64_encode(const std::string &in)
{
    std::string out;
    std::size_t i = 0;
    for (; i + 2 < in.size(); i += 3)
    {
        const unsigned v = (static_cast<unsigned char>(in[i]) << 16) | (static_cast<unsigned char>(in[i + 1]) << 8) |
                           static_cast<unsigned char>(in[i + 2]);
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += kB64[(v >> 6) & 63];
        out += kB64[v & 63];
    }
    if (i + 1 == in.size())
    {
        const unsigned v = static_cast<unsigned char>(in[i]) << 16;
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += "==";
    }
    else if (i + 2 == in.size())
    {
        const unsigned v = (static_cast<unsigned char>(in[i]) << 16) | (static_cast<unsigned char>(in[i + 1]) << 8);
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += kB64[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

bool b64_decode(const std::string &in, std::string *out)
{
    out->clear();
    unsigned acc = 0;
    int bits = 0;
    for (const char c : in)
    {
        if (c == '=')
            break;
        const char *p = std::strchr(kB64, c);
        if (!p || c == '\0')
            return false;
        acc = (acc << 6) | static_cast<unsigned>(p - kB64);
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            *out += static_cast<char>((acc >> bits) & 0xff);
        }
    }
    return true;
}

// ---- inflate (RFC 1951) --------------------------------------------------------------------------------------

struct Huffman
{
    std::uint16_t count[16] = {};
    std::uint16_t symbol[320] = {};
};

struct Stream
{
    const unsigned char *in;
    std::size_t size, pos = 0;
    unsigned bit_buf = 0;
    int bit_count = 0;
    std::string out;
    bool bad = false;

    int bits(int need)
    {
        while (bit_count < need)
        {
            if (pos >= size)
            {
                bad = true;
                return 0;
            }
            bit_buf |= static_cast<unsigned>(in[pos++]) << bit_count;
            bit_count += 8;
        }
        const int v = static_cast<int>(bit_buf & ((1u << need) - 1));
        bit_buf >>= need;
        bit_count -= need;
        return v;
    }
};

void build(Huffman *h, const unsigned char *lengths, int n)
{
    std::memset(h->count, 0, sizeof(h->count));
    for (int i = 0; i < n; i++)
        h->count[lengths[i]]++;
    h->count[0] = 0;
    std::uint16_t offs[16];
    offs[1] = 0;
    for (int i = 1; i < 15; i++)
        offs[i + 1] = static_cast<std::uint16_t>(offs[i] + h->count[i]);
    for (int i = 0; i < n; i++)
        if (lengths[i])
            h->symbol[offs[lengths[i]]++] = static_cast<std::uint16_t>(i);
}

int decode(Stream *s, const Huffman *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++)
    {
        code |= s->bits(1);
        if (s->bad)
            return -1;
        const int count = h->count[len];
        if (code - count < first)
            return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

const std::uint16_t kLenBase[] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const std::uint16_t kLenExtra[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const std::uint16_t kDistBase[] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const std::uint16_t kDistExtra[] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool codes(Stream *s, const Huffman *lit, const Huffman *dist)
{
    for (;;)
    {
        const int sym = decode(s, lit);
        if (sym < 0)
            return false;
        if (sym < 256)
            s->out += static_cast<char>(sym);
        else if (sym == 256)
            return true;
        else
        {
            const int l = sym - 257;
            if (l >= 29)
                return false;
            const int len = kLenBase[l] + s->bits(kLenExtra[l]);
            const int d = decode(s, dist);
            if (d < 0 || d >= 30)
                return false;
            const std::size_t distance = static_cast<std::size_t>(kDistBase[d] + s->bits(kDistExtra[d]));
            if (s->bad || distance > s->out.size())
                return false;
            for (int i = 0; i < len; i++)
                s->out += s->out[s->out.size() - distance];
        }
        if (s->out.size() > (64u << 20))
            return false;
    }
}

} // namespace

std::string inflate_zlib(const std::string &data, bool *ok)
{
    *ok = false;
    if (data.size() < 6 || (static_cast<unsigned char>(data[0]) & 0x0f) != 8)
        return std::string();
    Stream s{reinterpret_cast<const unsigned char *>(data.data()) + 2, data.size() - 2};
    for (int last = 0; !last;)
    {
        last = s.bits(1);
        const int type = s.bits(2);
        if (s.bad)
            return std::string();
        if (type == 0)
        {
            s.bit_buf = 0;
            s.bit_count = 0;
            if (s.pos + 4 > s.size)
                return std::string();
            const unsigned len = s.in[s.pos] | (s.in[s.pos + 1] << 8);
            s.pos += 4;
            if (s.pos + len > s.size)
                return std::string();
            s.out.append(reinterpret_cast<const char *>(s.in + s.pos), len);
            s.pos += len;
        }
        else if (type == 1)
        {
            unsigned char lengths[320];
            int i = 0;
            for (; i < 144; i++)
                lengths[i] = 8;
            for (; i < 256; i++)
                lengths[i] = 9;
            for (; i < 280; i++)
                lengths[i] = 7;
            for (; i < 288; i++)
                lengths[i] = 8;
            Huffman lit, dist;
            build(&lit, lengths, 288);
            for (i = 0; i < 30; i++)
                lengths[i] = 5;
            build(&dist, lengths, 30);
            if (!codes(&s, &lit, &dist))
                return std::string();
        }
        else if (type == 2)
        {
            const int nlen = s.bits(5) + 257, ndist = s.bits(5) + 1, ncode = s.bits(4) + 4;
            if (s.bad || nlen > 286 || ndist > 30)
                return std::string();
            static const unsigned char order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            unsigned char lengths[320] = {};
            for (int i = 0; i < ncode; i++)
                lengths[order[i]] = static_cast<unsigned char>(s.bits(3));
            Huffman lencode;
            build(&lencode, lengths, 19);
            int idx = 0;
            unsigned char all[320] = {};
            while (idx < nlen + ndist)
            {
                int sym = decode(&s, &lencode);
                if (sym < 0)
                    return std::string();
                if (sym < 16)
                    all[idx++] = static_cast<unsigned char>(sym);
                else
                {
                    int len = 0, rep;
                    if (sym == 16)
                    {
                        if (idx == 0)
                            return std::string();
                        len = all[idx - 1];
                        rep = 3 + s.bits(2);
                    }
                    else if (sym == 17)
                        rep = 3 + s.bits(3);
                    else
                        rep = 11 + s.bits(7);
                    if (idx + rep > nlen + ndist)
                        return std::string();
                    while (rep--)
                        all[idx++] = static_cast<unsigned char>(len);
                }
            }
            Huffman lit, dist;
            build(&lit, all, nlen);
            build(&dist, all + nlen, ndist);
            if (!codes(&s, &lit, &dist))
                return std::string();
        }
        else
            return std::string();
        if (s.bad)
            return std::string();
    }
    *ok = true;
    return s.out;
}

// A valid zlib stream of stored blocks: the bit field is a few dozen bytes, so compression gains nothing.
std::string deflate_stored(const std::string &in)
{
    std::string out;
    out += static_cast<char>(0x78);
    out += static_cast<char>(0x01);
    std::size_t pos = 0;
    do
    {
        const std::size_t n = std::min<std::size_t>(in.size() - pos, 65535);
        const bool last = pos + n >= in.size();
        out += static_cast<char>(last ? 1 : 0);
        out += static_cast<char>(n & 0xff);
        out += static_cast<char>((n >> 8) & 0xff);
        out += static_cast<char>(~n & 0xff);
        out += static_cast<char>((~n >> 8) & 0xff);
        out.append(in, pos, n);
        pos += n;
    } while (pos < in.size());
    unsigned a = 1, b = 0;
    for (const char c : in)
    {
        a = (a + static_cast<unsigned char>(c)) % 65521;
        b = (b + a) % 65521;
    }
    const unsigned adler = (b << 16) | a;
    for (int shift = 24; shift >= 0; shift -= 8)
        out += static_cast<char>((adler >> shift) & 0xff);
    return out;
}

// ---- the field ------------------------------------------------------------------------------------------------

std::vector<std::string> sorted_ids(const std::vector<Video> &videos)
{
    // Season, then episode, then release date; a video with none sorts first (as stremio-core's sort does).
    std::vector<const Video *> v;
    for (const Video &x : videos)
        v.push_back(&x);
    std::stable_sort(v.begin(), v.end(), [](const Video *a, const Video *b) {
        if (a->season != b->season)
            return a->season < b->season;
        if (a->episode != b->episode)
            return a->episode < b->episode;
        return a->released < b->released; // ISO dates: text order is date order
    });
    std::vector<std::string> ids;
    for (const Video *x : v)
        ids.push_back(x->id);
    return ids;
}

bool Bits::get(std::size_t i) const
{
    const std::size_t byte = i / 8;
    return byte < values.size() && ((values[byte] >> (i % 8)) & 1) != 0;
}

void Bits::set(std::size_t i, bool on)
{
    const std::size_t byte = i / 8;
    if (byte >= values.size())
        values.resize(byte + 1, 0);
    if (on)
        values[byte] = static_cast<std::uint8_t>(values[byte] | (1u << (i % 8)));
    else
        values[byte] = static_cast<std::uint8_t>(values[byte] & ~(1u << (i % 8)));
}

bool Bits::get_video(const std::string &id) const
{
    const auto it = std::find(ids.begin(), ids.end(), id);
    return it != ids.end() && get(static_cast<std::size_t>(it - ids.begin()));
}

bool Bits::set_video(const std::string &id, bool on)
{
    const auto it = std::find(ids.begin(), ids.end(), id);
    if (it == ids.end())
        return false;
    set(static_cast<std::size_t>(it - ids.begin()), on);
    return true;
}

std::string Bits::to_string() const
{
    // the last watched video is the anchor; none watched: index 0 (the first id, or "undefined")
    std::size_t last = 0;
    for (std::size_t i = ids.size() * 8 > values.size() * 8 ? values.size() * 8 : ids.size(); i-- > 0;)
        if (get(i))
        {
            last = i;
            break;
        }
    const std::string anchor = last < ids.size() ? ids[last] : "undefined";
    std::string bytes(values.begin(), values.end());
    return anchor + ":" + std::to_string(last + 1) + ":" + b64_encode(deflate_stored(bytes));
}

Bits construct(const std::string &field, const std::vector<std::string> &ids)
{
    Bits out;
    out.ids = ids;
    out.values.assign((ids.size() + 7) / 8, 0);
    // {anchor id (may hold ':')}:{anchor length}:{base64}
    const std::size_t p2 = field.rfind(':');
    if (p2 == std::string::npos || p2 == 0)
        return out;
    const std::size_t p1 = field.rfind(':', p2 - 1);
    if (p1 == std::string::npos)
        return out;
    const std::string anchor = field.substr(0, p1);
    const long anchor_length = std::strtol(field.substr(p1 + 1, p2 - p1 - 1).c_str(), nullptr, 10);
    std::string raw, bytes;
    bool ok = false;
    if (!b64_decode(field.substr(p2 + 1), &raw))
        return out;
    bytes = inflate_zlib(raw, &ok);
    if (!ok)
        return out;
    const auto it = std::find(ids.begin(), ids.end(), anchor);
    if (it == ids.end())
        return out; // the anchor video is gone: nothing can be told
    const long offset = anchor_length - static_cast<long>(it - ids.begin()) - 1;
    Bits stored;
    stored.values.assign(bytes.begin(), bytes.end());
    stored.values.resize(std::max(stored.values.size(), (ids.size() + 7) / 8), 0);
    const long length = static_cast<long>(ids.size());
    for (long i = 0; i < length; i++)
    {
        const long prev = i + offset;
        if (prev >= 0 && prev < length)
            out.set(static_cast<std::size_t>(i), stored.get(static_cast<std::size_t>(prev)));
    }
    return out;
}

} // namespace sx::watched
