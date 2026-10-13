// Which audio track a file starts with: a file with five audio tracks (Czech first and marked default, then English
// commentary, English 5.1, English stereo, French) and the picker run on it.
#include "audio_pick.h"

#include <cstdio>
#include <cstdlib>

namespace
{
int failed = 0;

void check(bool ok, const char *what)
{
    std::printf("%s   %s\n", ok ? "ok" : "FAIL", what);
    failed += ok ? 0 : 1;
}

void add_audio(AVFormatContext *out, AVCodecID id, int channels, const char *lang, const char *title, bool is_default)
{
    AVStream *st = avformat_new_stream(out, nullptr);
    st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    st->codecpar->codec_id = id;
    st->codecpar->sample_rate = 48000;
    av_channel_layout_default(&st->codecpar->ch_layout, channels);
    st->time_base = {1, 1000};
    av_dict_set(&st->metadata, "language", lang, 0);
    if (title)
        av_dict_set(&st->metadata, "title", title, 0);
    if (is_default)
        st->disposition |= AV_DISPOSITION_DEFAULT;
}
} // namespace

int main()
{
    // The picker only reads the file's list of tracks, so one is made by hand.
    AVFormatContext *fmt = avformat_alloc_context();
    add_audio(fmt, AV_CODEC_ID_EAC3, 6, "cze", nullptr, true);              // 0
    add_audio(fmt, AV_CODEC_ID_AAC, 2, "eng", "Director's Commentary", false); // 1
    add_audio(fmt, AV_CODEC_ID_EAC3, 6, "eng", nullptr, false);             // 2
    add_audio(fmt, AV_CODEC_ID_AAC, 2, "eng", nullptr, false);              // 3
    add_audio(fmt, AV_CODEC_ID_AC3, 6, "fre", nullptr, false);              // 4
    // FFmpeg's own pick would be the file's default track: the Czech one.
    const int ffmpeg_pick = 0;
    check(pick_audio_stream(fmt, {"eng"}, ffmpeg_pick) == 2, "English preferred: the English 5.1 track, not the Czech default, the commentary or the stereo one");
    check(pick_audio_stream(fmt, {"fre", "eng"}, ffmpeg_pick) == 4, "French first, English second: French");
    check(pick_audio_stream(fmt, {"ger", "eng"}, ffmpeg_pick) == 2, "German is not there: the next preference, English");
    check(pick_audio_stream(fmt, {"ger"}, ffmpeg_pick) == 0, "none of the preferences are there: the file's own default (Czech)");
    check(pick_audio_stream(fmt, {}, ffmpeg_pick) == 0, "no preference: the file's own default (Czech)");
    avformat_free_context(fmt);
    std::printf(failed ? "audio_pick: %d check(s) failed\n" : "audio_pick OK\n", failed);
    return failed ? 1 : 0;
}
