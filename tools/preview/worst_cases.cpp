#include "worst_cases.hpp"

#include "sample.hpp"

#include <cstdio>

namespace sx::preview
{

namespace
{

struct Art
{
    std::uint32_t poster[12] = {};
    std::uint32_t backdrop[12] = {};
};

bool load_art(hui::gfx::Renderer &renderer, const std::string &dir, Art *art)
{
    for (int i = 0; i < 12; ++i)
    {
        char path[512];
        std::snprintf(path, sizeof(path), "%s/poster%02d.png", dir.c_str(), i);
        art->poster[i] = load_texture(renderer, path);
        std::snprintf(path, sizeof(path), "%s/backdrop%02d.png", dir.c_str(), i);
        art->backdrop[i] = load_texture(renderer, path);
        if (!art->poster[i] || !art->backdrop[i])
            return false;
    }
    return true;
}

Title make(const Art &art, int image, const char *name, const char *year, const char *runtime, const char *genres,
           const char *imdb, const char *synopsis)
{
    Title t;
    t.id = name;
    t.name = name;
    t.year = year;
    t.runtime = runtime;
    t.genres = genres;
    t.imdb = imdb;
    t.synopsis = synopsis;
    t.poster = art.poster[image];
    t.backdrop = art.backdrop[image];
    return t;
}

const char *kLongSynopsis =
    "President Merkin Muffley is told that a rogue general has ordered a nuclear first strike on the Soviet Union. "
    "As the war room fills with advisers who each have a different idea of how to stop it, the one man who knows how "
    "the doomsday device works is the one nobody trusts. Told over a single night in three locations, with a cast "
    "that plays several roles each, it is a comedy about what happens when the people in charge are afraid of "
    "looking foolish more than they are afraid of the end of the world.";

} // namespace

bool load_worst(hui::gfx::Renderer &renderer, const std::string &art_dir, WorstCases *out)
{
    Art art;
    if (!load_art(renderer, art_dir, &art))
        return false;

    Title strangelove = make(art, 0, "Dr. Strangelove or: How I Learned to Stop Worrying and Love the Bomb", "1964", "1h 35m",
                             "Comedy, War, Satire, Political, Black Comedy", "8.4", kLongSynopsis);
    Title arabic = make(art, 1, "الأب الروحي", "1972", "2h 55m", "جريمة، دراما", "9.2",
                        "تدور القصة حول عائلة كورليوني، إحدى عائلات المافيا في نيويورك، وصراع الأجيال على السلطة.");
    Title greek = make(art, 2, "Ο Νονός Μέρος Β΄", "1974", "3h 22m", "Έγκλημα, Δράμα", "9.0",
                       "Η άνοδος του νεαρού Βίτο Κορλεόνε στη Νέα Υόρκη και η προσπάθεια του Μάικλ να κρατήσει την οικογένεια ενωμένη.");
    Title cyrillic = make(art, 3, "Властелин колец: Возвращение короля", "2003", "3h 21m", "Фэнтези, Приключения", "9.0",
                          "Последняя часть путешествия к Роковой горе, где судьба Средиземья решается вдали от главных сражений.");
    Title one = make(art, 4, "M", "1931", "1h 57m", "Crime, Thriller", "8.3", "A city hunts a killer.");
    one.poster = 0; // artwork that never arrived
    one.backdrop = 0;
    Title series = make(art, 5, "Fullmetal Alchemist: Brotherhood \xE2\x80\x93 The Complete Collection of Every Episode, OVA and Special Feature",
                        "2009\xE2\x80\x93" "2010", "24m", "Animation, Action, Adventure, Drama, Fantasy", "9.1", kLongSynopsis);
    Title bare = make(art, 6, "Se7en", "", "", "", "", "");
    Title word = make(art, 7,
                      "Supercalifragilisticexpialidocious_Documentary_Collection_Volume_12_Remastered_Extended",
                      "2021", "1h 12m", "Documentary", "7.0", "A very long name with nowhere to break.");
    Title normal = make(art, 8, "Midnight Ferry", "2022", "1h 47m", "Crime, Drama", "7.2",
                        "One night shift, one stolen ferry, and a passenger list nobody wants to read.");
    Title nearly = normal;
    nearly.progress = 0.995f;
    nearly.badge = "S12 E104";
    Title just = arabic;
    just.progress = 0.0f;
    Title mid = strangelove;
    mid.progress = 0.5f;

    // Board: a hero with a title that doesn't fit, rows of every shape.
    out->board.rows.clear();
    out->board.rows.push_back(Row{"Continue Watching", {mid, nearly, just, series, bare, cyrillic}});
    out->board.rows.push_back(Row{"Because You Watched Dr. Strangelove or: How I Learned to Stop Worrying and Love the Bomb",
                                  {strangelove, arabic, greek, cyrillic, one, series, bare, word}});
    out->board.rows.push_back(Row{"Just one", {normal}});
    out->board.rows.push_back(Row{"Nothing here yet", {}});

    // Discover and Library: long filter values, missing posters, every kind of name.
    out->discover.filters = {{"Type", "Movie"}, {"Catalog", "Top rated of all time (Official Catalogs)"},
                             {"Genre", "Science Fiction & Fantasy"}};
    out->discover.items = {strangelove, arabic, greek, cyrillic, one, series, bare, word, normal, strangelove, arabic, greek};
    out->library.filters = {{"Show", "Everything I have ever added"}, {"Sort", "Recently watched, newest first"}};
    out->library.items = {strangelove, arabic, greek, cyrillic, one, series, bare, word, normal, nearly, just, mid};
    out->library.items[1].watched = true;
    out->library.items[4].watched = true;
    out->library.count = "1 title";

    // Addons.
    auto addon = [](const char *name, const char *version, const char *types, const char *description, const char *initial,
                    bool local) {
        Addon a;
        a.name = name;
        a.version = version;
        a.types = types;
        a.description = description;
        a.initial = initial;
        a.colour = 0x7b5bf5;
        a.local = local;
        return a;
    };
    out->addons.items = {
        addon("The Ultimate All-In-One Streaming Aggregator with Subtitles, Live TV and More", "v12.104.3-beta.7+build.20261006",
              "movie series tv anime channel other", "Finds streams, subtitles, catalogues and live channels from more than two hundred sources, grouped by quality, language and size, with caching, retries and a built-in health check for every provider it talks to.",
              "T", true),
        addon("Ελληνικοί Υπότιτλοι", "v1.0", "subtitles", "Υπότιτλοι στα ελληνικά για ταινίες και σειρές.", "Ε", false),
        addon("مكتبة الأفلام العربية", "v2.1.0", "movie series", "أفلام ومسلسلات عربية مع ترجمات.", "م", false),
        addon("X", "v0", "", "", "X", false),
    };

    // Settings.
    using Kind = SettingRow::Kind;
    auto row = [](Kind kind, const char *label, const char *value, bool on, const char *help) {
        SettingRow r;
        r.kind = kind;
        r.label = label;
        r.value = value;
        r.on = on;
        r.help = help;
        return r;
    };
    out->settings.rows = {
        row(Kind::value, "Streaming server", "https://my-streaming-server.example-hosting-provider.com:11470/stremio/api/v2", false,
            "A Stremio streaming server is optional: streams play on this console without one. Set one only if you want the server to transcode video, to share a torrent between your devices or to keep a cache that outlives this console's storage. The address needs to be reachable from your network, include the port, and start with http:// or https://. If it stops answering, the app falls back to playing on this console by itself and tells you so, so a server that is switched off never stops you from watching anything at all."),
        row(Kind::toggle, "Automatically play the next episode when the current one ends", "", true,
            "When an episode ends, start the next one."),
        row(Kind::value, "Subtitle language", "Ελληνικά", false, "Language."),
        row(Kind::value, "Audio language", "العربية", false, ""),
    };

    // Detail: a series with everything long.
    out->detail = DetailContent{};
    out->detail.title = series;
    out->detail.directors = "Yasuhiro Irie, Hiromu Arakawa, Makoto Uezu, Shinichi Fukumoto, Masahiro Okuyama";
    out->detail.cast = "Romi Park, Rie Kugimiya, Shinichiro Miki, Fumiko Orikasa, Kenji Utsumi, Yuichi Nakamura, Rina Sato, Sayaka Ohara, Kazuya Nakai, Hidekatsu Shibata";
    out->detail.series = true;
    for (int s = 1; s <= 20; ++s)
        out->detail.seasons.push_back("Season " + std::to_string(s));
    out->detail.season = 19;
    const char *names[6] = {"The Day the Sky Fell Down Over the Eastern City and Nobody Could Explain Why", "Ε", "ΟΙ ΑΔΕΛΦΟΙ ΕΛΡΙΚ",
                            "الحلقة الأولى: البداية", "Episode 104", ""};
    for (int i = 0; i < 6; ++i)
    {
        Episode e;
        e.number = 100 + i;
        e.title = names[i];
        e.info = i == 1 ? "" : "Sep 14, 2010  \xC2\xB7  24m  \xC2\xB7  Rated TV-14";
        e.thumb = i % 2 == 0 ? art.backdrop[5] : 0; // some without a picture
        e.watched = i < 2;
        if (i == 2)
            e.progress = 0.995f;
        if (i == 3)
            e.progress = 0.0f;
        out->detail.episodes.push_back(e);
    }
    out->detail.stream_heading = "S19 E104  \xC2\xB7  The Day the Sky Fell Down Over the Eastern City and Nobody Could Explain Why";
    auto stream = [](const char *res, const char *name, const char *detail, const char *tags, const char *size, const char *seeds) {
        Stream s;
        s.resolution = res;
        s.name = name;
        s.detail = detail;
        s.tags = tags;
        s.size = size;
        s.seeds = seeds;
        return s;
    };
    out->detail.streams = {
        stream("4K", "The Ultimate All-In-One Streaming Aggregator", "Fullmetal.Alchemist.Brotherhood.S19E104.2160p.UHD.BluRay.REMUX.HDR10Plus.HEVC.TrueHD.Atmos.7.1.DUAL.AUDIO.MULTI.SUBS-GROUP.mkv",
               "HDR10+  \xC2\xB7  Dolby Atmos  \xC2\xB7  Dual audio  \xC2\xB7  Multi subs  \xC2\xB7  Remux", "112.48 GB", "12,402 seeds"),
        stream("1080p", "X", "a.mkv", "", "0 B", ""),
        stream("720p", "Ελληνικοί Υπότιτλοι", "Ο.Νονός.1972.720p.mkv", "", "1.6 GB", "5 seeds"),
        stream("SD", "مكتبة الأفلام", "الأب.الروحي.mkv", "", "700 MB", ""),
    };

    // Player: long names, three lines of subtitles in two scripts.
    out->player.title = series.name;
    out->player.subtitle = "S19 E104  \xC2\xB7  The Day the Sky Fell Down Over the Eastern City and Nobody Could Explain Why";
    out->player.position = 39600.0 + 754.0;
    out->player.duration = 43200.0 + 3500.0;
    out->player.buffered = 30.0;
    out->player.audio = "English  Dolby Atmos 7.1 (Director's Commentary)";
    out->player.subs = "Ελληνικά (SDH)";
    out->player.line = "هذه جملة طويلة جدا تظهر على عدة أسطر في نفس الوقت\nΑυτή είναι μια πολύ μεγάλη πρόταση που πρέπει να χωρέσει στην οθόνη\nOK";
    out->player.net = "112.8 MB/s  \xC2\xB7  1,284 peers";
    out->player.audio_tracks = {"English  Dolby Atmos 7.1 (Director's Commentary with the Whole Production Team)", "Ελληνικά  Stereo",
                                "العربية  Stereo", "日本語  5.1"};
    out->player.subtitle_tracks = {"Off", "Ελληνικά (SDH)", "العربية", "Русский", "English (Forced, Signs and Songs Only)"};
    out->player.audio_active = 0;
    out->player.subtitle_active = 1;

    // Launch.
    out->launch.title = series.name;
    out->launch.subtitle = out->detail.stream_heading;
    out->launch.source = "The Ultimate All-In-One Streaming Aggregator";
    out->launch.quality = "4K  \xC2\xB7  HDR10+  \xC2\xB7  Dolby Atmos  \xC2\xB7  112.48 GB";
    out->launch.stages = {"Finding sources", "Connecting to peers", "Filling the buffer", "Starting"};
    out->launch.stage = 2;
    out->launch.progress = 0.995f;
    out->launch.peers = "1,284";
    out->launch.speed = "112.8 MB/s";
    out->launch.done = "99%";
    out->launch.hash = "3f9a6c1be07d4a52c8d1e9b0a47f3d6215c8e2ab";
    out->launch.backdrop = art.backdrop[5];
    out->launch.poster = art.poster[5];
    return true;
}

} // namespace sx::preview
