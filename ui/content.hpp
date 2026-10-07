// What the screens show: plain data, no graphics code. The console fills it
// from the user's addons; the preview tool fills it with invented samples.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sx
{

struct Title
{
    std::string id;
    std::string type;      // "movie", "series": what the addons call it
    std::string name;
    std::string year;
    std::string runtime;   // "2h 08m"
    std::string genres;    // "Drama, Mystery"
    std::string imdb;      // "7.8", empty when unknown
    std::string synopsis;
    std::string badge;     // "S2 E5" on a series being watched
    float progress = -1.0f; // 0..1 when it can be resumed, negative when not
    bool watched = false;   // finished: a green check on its poster
    std::uint32_t poster = 0;   // GL texture, 0 until the artwork has arrived
    std::uint32_t backdrop = 0; // GL texture, 0 until the artwork has arrived
};

struct Row
{
    std::string title; // "Continue Watching", "Popular - Movie"
    std::vector<Title> items;
    std::string key;   // which catalog it is, so a row keeps the cursor when others appear above it
};

struct BoardContent
{
    std::vector<Row> rows;
    std::string status; // what to say while there are no rows: "Loading your catalogs..."
};

// A drop-down choice at the top of Discover and Library: "Type  Movie".
struct Filter
{
    std::string label; // "Type", "Catalog", "Genre"
    std::string value; // "Movie", "Popular", "All genres"
};

struct DiscoverContent
{
    std::vector<Filter> filters;
    std::vector<Title> items;
    std::string status; // while there are no items: "Loading...", "Nothing here."
};

struct LibraryContent
{
    std::vector<Filter> filters;
    std::vector<Title> items;
    std::string count; // "128 titles"
    std::string status; // while there are no items
};

struct Addon
{
    std::string name;
    std::string version;     // "v2.4.1"
    std::string types;       // "movie  series  tv"
    std::string description;
    std::string initial;     // shown when the addon has no logo
    std::uint32_t logo = 0;  // GL texture, 0 until it has arrived
    std::uint32_t colour = 0x7b5bf5; // the tile's colour while there is no logo
    bool local = false;      // runs on this console, not on a server
};

struct AddonsContent
{
    std::vector<Addon> items;
};

struct SettingRow
{
    enum class Kind
    {
        value,  // opens a list of choices
        toggle, // on or off
        action, // does something: sign out, reload
    };
    Kind kind = Kind::value;
    std::string label;
    std::string value; // shown on the right
    bool on = false;   // for toggles
    std::string help;  // what it does, shown beside the list
};

struct SettingsContent
{
    std::string account; // "alex@example.com"
    std::vector<SettingRow> rows;
};

struct Episode
{
    int number = 0;
    std::string title;
    std::string info;         // "Oct 4  ·  52m"
    bool watched = false;
    float progress = -1.0f;   // 0..1 when it can be resumed
    std::uint32_t thumb = 0;  // GL texture, 0 until it has arrived
};

// One way to watch: a file from an addon.
struct Stream
{
    std::string resolution; // "4K", "1080p", "720p", "SD"
    std::string name;       // the addon or release: "Stream Finder"
    std::string detail;     // the file name
    std::string tags;       // "HDR10  ·  Dolby Atmos"
    std::string size;       // "18.4 GB"
    std::string seeds;      // "142 seeds", empty for direct links
};

struct DetailContent
{
    Title title;
    std::string directors;
    std::string cast;
    bool series = false;
    std::vector<std::string> seasons; // "Season 1", ...
    int season = 0;
    std::vector<Episode> episodes;
    std::vector<Stream> streams;
    std::string stream_heading; // "Streams", or the episode they are for
    // What the lists say while they are empty: "Loading streams...", "No streams found".
    std::string episodes_status;
    std::string streams_status;
    // Asks the page to put its episode cursor on `episode_hint` when this changes.
    int episode_hint = 0;
    int hint_serial = 0;
    // Asks the page to show the streams of the episode that was just loaded, when this changes.
    int streams_serial = 0;
};

struct PlayerState
{
    std::string title;
    std::string subtitle;   // "S2 E5  ·  The Long Night"
    double position = 0.0;  // seconds
    double duration = 0.0;
    double buffered = 0.0;  // seconds, how far ahead is ready
    bool paused = false;
    int buffering = -1;     // percent while stalled, negative when playing
    std::string net;        // "8.4 MB/s  ·  32 peers"
    std::string audio;      // "English  5.1"
    std::string subs;       // "Greek", "Off"
    std::string line;       // the subtitle on screen
    bool controls = true;   // shown on a button press, hidden after a few seconds
    bool menu = false;      // the track menu
    std::vector<std::string> audio_tracks;
    std::vector<std::string> subtitle_tracks;
    int audio_active = 0;
    int subtitle_active = 0;
    int menu_column = 0;
    int menu_row = 0;
    std::string stats;           // "H264 1920x800 (hardware) · 24 fps · buffered 12.0s · dropped 0 of 450"
    int sub_size = 1;            // 0 small, 1 medium, 2 large
    std::string subtitle_delay;  // "+0.25 s"
};

// Starting a stream: what is being started, how far it has got, and what the
// torrent engine is doing.
struct LaunchState
{
    std::string title;
    std::string subtitle; // "S2 E5  Â·  The Long Night" for an episode
    std::string source;   // the addon: "Stream Finder"
    std::string quality;  // "4K  Â·  HDR10  Â·  21.3 GB"
    std::vector<std::string> stages; // "Finding sources", "Connecting to peers", ...
    int stage = 0;                   // the one in progress; earlier ones are done
    float progress = -1.0f;          // how full the buffer is, negative when not known yet
    bool torrent = true;
    std::string peers; // "32"
    std::string speed; // "8.4 MB/s"
    std::string done;  // "12%"
    std::string hash;  // the torrent's info hash
    std::uint32_t backdrop = 0;
    std::uint32_t poster = 0;
};
struct SearchContent
{
    std::string query;
    BoardContent results; // rows: "Movies", "Series"
};

// The five pages of the left rail, in Stremio's order.
enum class Page
{
    board,
    discover,
    library,
    addons,
    settings,
};

} // namespace sx
