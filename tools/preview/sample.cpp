#include "sample.hpp"

#include <cstdio>
#include <cstring>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "stb_image.h"

namespace sx::preview
{

std::uint32_t load_texture(hui::gfx::Renderer &renderer, const std::string &path, int *w, int *h)
{
    int width = 0;
    int height = 0;
    int comp = 0;
    unsigned char *pixels = stbi_load(path.c_str(), &width, &height, &comp, 4);
    if (!pixels)
    {
        std::fprintf(stderr, "cannot load %s\n", path.c_str());
        return 0;
    }
    const std::uint32_t texture = renderer.batch().create_texture(width, height, pixels);
    stbi_image_free(pixels);
    if (w)
        *w = width;
    if (h)
        *h = height;
    return texture;
}

namespace
{

struct Sample
{
    const char *name;
    const char *year;
    const char *runtime;
    const char *genres;
    const char *imdb;
    const char *synopsis;
};

const Sample kSamples[12] = {
    {"The Lantern Keepers", "2024", "2h 08m", "Drama, Mystery", "7.8",
     "On a fogbound island, the last lighthouse keepers guard a secret that the sea keeps trying to return."},
    {"Salt and Static", "2023", "52m", "Sci-Fi, Thriller", "8.1",
     "A radio engineer starts hearing broadcasts from a town that was flooded forty years ago."},
    {"Midnight Ferry", "2022", "1h 47m", "Crime, Drama", "7.2",
     "One night shift, one stolen ferry, and a passenger list nobody wants to read."},
    {"Orchard Street", "2021", "45m", "Comedy, Drama", "7.9",
     "Five neighbours, one shared garden and a feud that has outlived the people who started it."},
    {"Northbound", "2024", "2h 21m", "Adventure", "7.5",
     "Two strangers cross a frozen country by rail, each with a reason they keep to themselves."},
    {"Paper Moons", "2020", "58m", "Fantasy, Family", "8.4",
     "A girl who folds paper into moons discovers that each one lights a different night."},
    {"The Quiet Meridian", "2025", "1h 58m", "Sci-Fi", "7.0",
     "A survey team on the line between two time zones loses an hour and cannot find it."},
    {"Glasshouse", "2023", "50m", "Thriller", "8.0",
     "In a botanical garden under glass, every visitor leaves with someone else's secret."},
    {"Ember Lines", "2022", "2h 03m", "Action", "6.9",
     "A fire inspector races a blaze that follows the old rail routes across three counties."},
    {"Harbor Lights", "2019", "1h 36m", "Romance", "7.3",
     "Two night-shift workers at opposite ends of a port send each other messages in lamplight."},
    {"A Slow Country", "2024", "1h 52m", "Documentary", "8.2", "A year on the roads of a country that refuses to hurry."},
    {"Velvet Hours", "2021", "1h 44m", "Drama", "7.1",
     "A theatre prepares its final show while the audience slowly takes over the stage."},
};

bool load_titles(hui::gfx::Renderer &renderer, const std::string &art_dir, Title (&titles)[12])
{
    for (int i = 0; i < 12; ++i)
    {
        char path[512];
        std::snprintf(path, sizeof(path), "%s/poster%02d.png", art_dir.c_str(), i);
        titles[i].poster = load_texture(renderer, path);
        std::snprintf(path, sizeof(path), "%s/backdrop%02d.png", art_dir.c_str(), i);
        titles[i].backdrop = load_texture(renderer, path);
        if (!titles[i].poster || !titles[i].backdrop)
            return false;
        titles[i].id = "sample" + std::to_string(i);
        titles[i].name = kSamples[i].name;
        titles[i].year = kSamples[i].year;
        titles[i].runtime = kSamples[i].runtime;
        titles[i].genres = kSamples[i].genres;
        titles[i].imdb = kSamples[i].imdb;
        titles[i].synopsis = kSamples[i].synopsis;
    }
    return true;
}

std::vector<Title> pick(const Title (&titles)[12], std::initializer_list<int> indexes)
{
    std::vector<Title> items;
    for (int i : indexes)
        items.push_back(titles[i]);
    return items;
}

} // namespace

bool load_board(hui::gfx::Renderer &renderer, const std::string &art_dir, BoardContent *out)
{
    Title titles[12];
    if (!load_titles(renderer, art_dir, titles))
        return false;
    out->rows.clear();
    Row resume{"Continue Watching", pick(titles, {0, 2, 1, 3, 7, 5})};
    resume.items[0].progress = 0.42f;
    resume.items[1].progress = 0.78f;
    resume.items[2].progress = 0.15f;
    resume.items[2].badge = "S2 E5";
    resume.items[3].progress = 0.60f;
    resume.items[3].badge = "S1 E3";
    resume.items[4].progress = 0.90f;
    resume.items[4].badge = "S1 E8";
    resume.items[5].progress = 0.05f;
    resume.items[5].badge = "S3 E1";
    out->rows.push_back(resume);
    out->rows.push_back(Row{"Popular - Movie", pick(titles, {4, 6, 8, 9, 10, 11, 0, 2})});
    out->rows.push_back(Row{"Popular - Series", pick(titles, {1, 3, 5, 7, 2, 4, 6, 8})});
    return true;
}

bool load_discover(hui::gfx::Renderer &renderer, const std::string &art_dir, DiscoverContent *out)
{
    Title titles[12];
    if (!load_titles(renderer, art_dir, titles))
        return false;
    out->filters = {{"Type", "Movie"}, {"Catalog", "Popular"}, {"Genre", "All genres"}};
    out->items = pick(titles, {4, 6, 8, 9, 10, 11, 0, 2, 1, 3, 5, 7, 6, 4, 9, 10});
    return true;
}

bool load_library(hui::gfx::Renderer &renderer, const std::string &art_dir, LibraryContent *out)
{
    Title titles[12];
    if (!load_titles(renderer, art_dir, titles))
        return false;
    out->filters = {{"Show", "All titles"}, {"Sort", "Recently watched"}};
    out->items = pick(titles, {0, 2, 1, 3, 7, 5, 4, 6, 8, 9, 10, 11, 0, 3});
    out->items[0].progress = 0.42f;
    out->items[1].watched = true;
    out->items[2].progress = 0.15f;
    out->items[2].badge = "S2 E5";
    out->items[3].watched = true;
    out->items[4].progress = 0.90f;
    out->items[4].badge = "S1 E8";
    out->items[6].watched = true;
    out->items[8].progress = 0.30f;
    out->items[9].watched = true;
    out->count = "126 titles";
    return true;
}

void load_addons(AddonsContent *out)
{
    auto make = [](const char *name, const char *version, const char *types, const char *description,
                   const char *initial, std::uint32_t colour, bool local) {
        Addon a;
        a.name = name;
        a.version = version;
        a.types = types;
        a.description = description;
        a.initial = initial;
        a.colour = colour;
        a.local = local;
        return a;
    };
    out->items = {
        make("Official Catalogs", "v3.0.1", "movie series", "Popular and featured movies and series, with posters, ratings and cast.", "O", 0x7b5bf5, false),
        make("Stream Finder", "v2.4.0", "movie series", "Finds streams for the title you open, grouped by quality and size.", "S", 0x1d8cf0, false),
        make("Subtitle Hub", "v1.9.3", "subtitles", "Subtitles in dozens of languages, matched to the file you play.", "H", 0x1fa37a, false),
        make("Live TV Guide", "v1.2.0", "tv", "Channel lists and what is on now, from public playlists.", "L", 0xe0803a, false),
        make("Debrid Streams", "v4.1.2", "movie series", "Cached streams from your debrid account, ready to play at once.", "D", 0xc0475c, false),
        make("This Console", "v0.4.0", "movie series", "Files and shares on your own network, played straight from the source.", "C", 0x4b5cc4, true),
        make("Kids Picks", "v1.0.6", "movie series", "Family-friendly titles, filtered by age rating.", "K", 0x2fb0b8, false),
    };
}

void load_settings(SettingsContent *out)
{
    using Kind = SettingRow::Kind;
    out->account = "alex@example.com";
    auto row = [](Kind kind, const char *label, const char *value, bool on, const char *help) {
        SettingRow r;
        r.kind = kind;
        r.label = label;
        r.value = value;
        r.on = on;
        r.help = help;
        return r;
    };
    out->rows = {
        row(Kind::action, "Account", "alex@example.com", false,
            "You are signed in to your Stremio account. Your library, addons and where you stopped watching sync from it. Select to sign out."),
        row(Kind::value, "Streaming server", "Not used", false,
            "A Stremio streaming server is optional: streams play on this console without one. Set one only if you want the server to transcode."),
        row(Kind::value, "Play torrents with", "Built-in engine", false,
            "The built-in engine downloads torrents directly on this console, ahead of what you are watching, and keeps up to 16 GB on its storage."),
        row(Kind::toggle, "Hardware decoding", "", true,
            "Uses the console's video hardware for H.264, HEVC and VP9 up to 4K. Switch it off only to test a stream that fails."),
        row(Kind::value, "Default quality", "Ask each time", false,
            "Choose a resolution to start every stream with, or be asked each time you pick one."),
        row(Kind::value, "Subtitle language", "Greek", false,
            "The language subtitles start in when a stream has them. You can change it while playing."),
        row(Kind::value, "Subtitle size", "Medium", false, "How large subtitles are drawn on the screen."),
        row(Kind::value, "Audio language", "Original", false, "The audio track to start with, when a stream offers several."),
        row(Kind::toggle, "Play next episode", "", true,
            "When an episode ends, start the next one automatically after a short countdown."),
        row(Kind::action, "About", "Version 0.4.0", false,
            "An unofficial Stremio client for PS5. Not affiliated with, endorsed by or connected to Stremio or Sony Interactive Entertainment."),
    };
}

bool load_detail(hui::gfx::Renderer &renderer, const std::string &art_dir, DetailContent *series, DetailContent *movie)
{
    Title titles[12];
    if (!load_titles(renderer, art_dir, titles))
        return false;
    auto stream = [](const char *res, const char *name, const char *detail, const char *tags, const char *size,
                     const char *seeds) {
        Stream s;
        s.resolution = res;
        s.name = name;
        s.detail = detail;
        s.tags = tags;
        s.size = size;
        s.seeds = seeds;
        return s;
    };

    *movie = DetailContent{};
    movie->title = titles[4];
    movie->directors = "Mara Okonkwo";
    movie->cast = "Idris Vale, Noor Haddad, Tomas Lindqvist, Wen Zhao, Elena Marchetti";
    movie->stream_heading = "Streams";
    movie->streams = {
        stream("4K", "Stream Finder", "Northbound.2024.2160p.WEB-DL.DDP5.1.HDR.H265-GRP.mkv", "HDR10  \xC2\xB7  Dolby Atmos",
               "21.3 GB", "212 seeds"),
        stream("4K", "Debrid Streams", "Northbound 2024 2160p BluRay REMUX HEVC DV", "Dolby Vision  \xC2\xB7  TrueHD", "58.9 GB", ""),
        stream("1080p", "Stream Finder", "Northbound.2024.1080p.WEB-DL.DDP5.1.H264-GRP.mkv", "", "5.4 GB", "431 seeds"),
        stream("1080p", "Debrid Streams", "Northbound.2024.1080p.BluRay.x264-GRP", "", "9.8 GB", ""),
        stream("1080p", "This Console", "/media/movies/Northbound (2024)/Northbound (2024).mkv", "Local file", "7.1 GB", ""),
        stream("720p", "Stream Finder", "Northbound.2024.720p.WEBRip.x264-GRP.mp4", "", "2.1 GB", "598 seeds"),
        stream("720p", "Debrid Streams", "Northbound.2024.720p.WEB.h264", "", "1.8 GB", ""),
        stream("SD", "Stream Finder", "Northbound.2024.480p.WEBRip.x264.mp4", "", "0.9 GB", "177 seeds"),
    };

    *series = DetailContent{};
    series->title = titles[1];
    series->directors = "Priya Natarajan, Jonas Weil";
    series->cast = "Camille Durand, Ibrahim Sesay, Lotte Hansen, Rafael Ortega, Mina Kobayashi";
    series->series = true;
    series->seasons = {"Season 1", "Season 2", "Season 3"};
    series->season = 1;
    const char *names[8] = {"Carrier Wave", "Dead Air", "Standing Waves", "The Flooded Road", "The Long Night", "Signal to Noise",
                            "Low Tide", "What the Water Kept"};
    const char *info[8] = {"Oct 4  \xC2\xB7  52m", "Oct 11  \xC2\xB7  49m", "Oct 18  \xC2\xB7  51m", "Oct 25  \xC2\xB7  55m",
                           "Nov 1  \xC2\xB7  58m", "Nov 8  \xC2\xB7  47m", "Nov 15  \xC2\xB7  50m", "Nov 22  \xC2\xB7  62m"};
    const int art[8] = {1, 3, 5, 7, 2, 6, 8, 9};
    for (int i = 0; i < 8; ++i)
    {
        Episode e;
        e.number = i + 1;
        e.title = names[i];
        e.info = info[i];
        e.thumb = titles[art[i]].backdrop;
        e.watched = i < 4;
        if (i == 4)
            e.progress = 0.42f;
        series->episodes.push_back(e);
    }
    series->stream_heading = "S2 E5  \xC2\xB7  The Long Night";
    series->streams = movie->streams;
    for (Stream &s : series->streams)
    {
        auto swap = [&](const char *from, const char *to) {
            const std::size_t at = s.detail.find(from);
            if (at != std::string::npos)
                s.detail.replace(at, std::strlen(from), to);
        };
        swap("/media/movies/Northbound (2024)/Northbound (2024).mkv", "/media/series/Salt and Static/S02/S02E05.mkv");
        swap("Northbound.2024", "Salt.and.Static.S02E05");
        swap("Northbound 2024", "Salt and Static S02E05");
    }
    return true;
}

bool load_search(hui::gfx::Renderer &renderer, const std::string &art_dir, SearchContent *found, SearchContent *none)
{
    Title titles[12];
    if (!load_titles(renderer, art_dir, titles))
        return false;
    found->query = "lantern";
    found->results.rows.clear();
    found->results.rows.push_back(Row{"Movies  \xC2\xB7  3 results", pick(titles, {0, 11, 9})});
    found->results.rows.push_back(Row{"Series  \xC2\xB7  2 results", pick(titles, {3, 1})});
    none->query = "lanterns of mars";
    none->results.rows.clear();
    return true;
}

std::uint32_t load_frame(hui::gfx::Renderer &renderer, const std::string &art_dir, int index)
{
    char path[512];
    std::snprintf(path, sizeof(path), "%s/backdrop%02d.png", art_dir.c_str(), index);
    return load_texture(renderer, path);
}

PlayerState sample_player()
{
    PlayerState p;
    p.title = "Salt and Static";
    p.subtitle = "S2 E5  \xC2\xB7  The Long Night";
    p.position = 2538.0;
    p.duration = 6944.0;
    p.buffered = 140.0;
    p.audio = "English  5.1";
    p.subs = "Greek";
    p.line = "Some of us were never meant to be found.\nThat is why we kept the signal on.";
    p.net = "8.4 MB/s  \xC2\xB7  32 peers";
    p.audio_tracks = {"English  5.1  (original)", "Greek  Stereo", "Spanish  5.1", "Director's commentary"};
    p.subtitle_tracks = {"Off", "Greek", "English", "English (SDH)", "Spanish", "French", "German"};
    p.audio_active = 0;
    p.subtitle_active = 1;
    return p;
}

std::uint32_t load_poster(hui::gfx::Renderer &renderer, const std::string &art_dir, int index)
{
    char path[512];
    std::snprintf(path, sizeof(path), "%s/poster%02d.png", art_dir.c_str(), index);
    return load_texture(renderer, path);
}

LaunchState sample_launch(std::uint32_t backdrop, std::uint32_t poster)
{
    LaunchState s;
    s.title = "Salt and Static";
    s.subtitle = "S2 E5  \xC2\xB7  The Long Night";
    s.source = "Stream Finder";
    s.quality = "4K  \xC2\xB7  HDR10  \xC2\xB7  21.3 GB";
    s.stages = {"Finding sources", "Connecting to peers", "Filling the buffer", "Starting"};
    s.stage = 2;
    s.progress = 0.12f;
    s.peers = "32";
    s.speed = "8.4 MB/s";
    s.done = "12%";
    s.hash = "3f9a6c1be07d4a52c8d1e9b0a47f3d6215c8e2ab";
    s.backdrop = backdrop;
    s.poster = poster;
    return s;
}
} // namespace sx::preview
