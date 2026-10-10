// The "watched" field of a library item, as stremio-core keeps it (stremio-watched-bitfield): which videos of a
// series were watched, stored as
//
//     {anchor video id}:{anchor length}:{base64 of the zlib-compressed bit field}
//
// where the bits run over the series' videos sorted by season, episode and release date (bit i is byte i / 8, bit
// i % 8 from the low end), the anchor is the last watched video, and the anchor length its position + 1. Reading it
// finds the anchor in the current list of videos and shifts the bits to fit it (videos may have been added).

#pragma once

#include "stremio.h"

#include <cstdint>
#include <string>
#include <vector>

namespace sx::watched
{

// The ids of a series' videos in the order the bits run (season, then episode, then release date).
std::vector<std::string> sorted_ids(const std::vector<Video> &videos);

struct Bits
{
    std::vector<std::string> ids;
    std::vector<std::uint8_t> values;

    bool get(std::size_t i) const;
    void set(std::size_t i, bool on);
    bool get_video(const std::string &id) const;
    bool set_video(const std::string &id, bool on); // false: not one of the ids
    std::string to_string() const; // "{last watched id}:{its index + 1}:{base64}"
};

// The bits for these ids from a stored field (empty or malformed: nothing watched).
Bits construct(const std::string &field, const std::vector<std::string> &ids);

// zlib (RFC 1950) and base64, exposed for the test.
std::string inflate_zlib(const std::string &in, bool *ok);
std::string deflate_stored(const std::string &in);

} // namespace sx::watched
