// What the app knows about, apart from how it is drawn: something that can be
// opened, what was watched, the user's settings.

#pragma once

#include "stremio.h"
#include "util.h"

#include <cstdint>
#include <string>
#include <vector>

namespace sx
{

// Something that can be opened: a catalog entry, a library item.
struct Item
{
    std::string id, type, name, poster, background, logo;
    std::string description, release_info, runtime, imdb_rating;
    std::vector<std::string> genres;
    // Continue watching
    std::string video_id;
    double offset = 0, duration = 0; // seconds
    std::int64_t last_watched = 0;   // epoch ms
    int times_watched = 0;
    bool up_next = false; // video_id is the episode after one that was finished (nothing watched of it yet)
};

Item item_from_meta_json(const json &j);

// Where a title was left, for people who are not signed in.
struct Progress
{
    std::string type, name, poster, video_id;
    double time = 0, duration = 0;
    std::int64_t updated = 0;
    // The stream (as JSON) that was playing and the episode it was for: Continue Watching plays it again.
    std::string stream_json, stream_video_id;
    // The episode after the one that was just finished, until something of it is watched.
    std::string up_next;
};

struct Prefs
{
    std::string server_url;
    bool builtin_torrents = true; // the app's own torrent engine, no server needed
    std::string subtitle_langs = "eng";
    bool auto_subtitles = true;
    std::string sub_size = "sub-m";
    std::string audio_langs;
    bool autoplay_next = true;
    bool ui_sounds = true; // the interface's soft sounds
    std::vector<std::string> extra_addons;
    std::string auth_key, user_email;
};

// One catalog of one addon, shown as a row on the Board.
struct BoardRow
{
    std::string key; // addon url | type | catalog id
    std::string title;
    std::string addon_url;
    Catalog catalog;
    std::vector<Item> items;
    bool continue_watching = false;
    bool loaded = false;
};

} // namespace sx
