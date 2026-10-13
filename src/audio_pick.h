// The audio track a file starts with, chosen the way Nuvio does it (nuvio_player.cpp, nuvio_pick_audio_stream).
#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <algorithm>
#include <string>
#include <vector>

#include "stremio.h"
#include "util.h"


// How good an audio stream is for the default pick: lossless before lossy, then more channels.
inline int audio_rank(const AVStream* st) {
	const AVCodecParameters* p = st->codecpar;
	int r;
	switch (p->codec_id) {
	case AV_CODEC_ID_TRUEHD: case AV_CODEC_ID_MLP: r = 90; break;
	case AV_CODEC_ID_FLAC: case AV_CODEC_ID_ALAC: r = 85; break;
	case AV_CODEC_ID_DTS: r = p->profile == AV_PROFILE_DTS_HD_MA ? 88 : 60; break;
	case AV_CODEC_ID_EAC3: r = 55; break;
	case AV_CODEC_ID_AC3: r = 45; break;
	case AV_CODEC_ID_OPUS: r = 40; break;
	case AV_CODEC_ID_AAC: r = 35; break;
	default:
		r = (p->codec_id >= AV_CODEC_ID_PCM_S16LE && p->codec_id < AV_CODEC_ID_ADPCM_IMA_QT) ? 86 : 20;
	}
	return r * 10 + std::min(8, p->ch_layout.nb_channels);
}

inline bool is_commentary(const AVStream* st) {
	if (st->disposition & (AV_DISPOSITION_COMMENT | AV_DISPOSITION_VISUAL_IMPAIRED)) return true;
	AVDictionaryEntry* t = av_dict_get(st->metadata, "title", nullptr, 0);
	if (!t || !t->value) return false;
	const std::string title = lower(t->value);
	return title.find("commentary") != std::string::npos || title.find("description") != std::string::npos;
}

inline std::string stream_language(const AVStream* st) {
	AVDictionaryEntry* l = av_dict_get(st->metadata, "language", nullptr, 0);
	return l && l->value ? language_to_iso639_2(l->value) : std::string();
}

// The first of the preferred languages the file has, and within a language its best track, never a commentary. With no
// match: the best track in the language of the track FFmpeg would have picked. (`fallback` is that track, or -1.)
inline int pick_audio_stream(AVFormatContext* fmt, const std::vector<std::string>& wanted, int fallback) {
	auto best_in = [&](const std::string& lang) {
		int best = -1, best_rank = -1;
		for (unsigned i = 0; i < fmt->nb_streams; i++) {
			const AVStream* st = fmt->streams[i];
			if (st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO || !avcodec_find_decoder(st->codecpar->codec_id)) continue;
			if (is_commentary(st)) continue;
			if (!lang.empty() && stream_language(st) != lang) continue;
			int rank = audio_rank(st);
			if (st->disposition & AV_DISPOSITION_DEFAULT) rank += 1;
			if (rank > best_rank) { best_rank = rank; best = int(i); }
		}
		return best;
	};
	for (const std::string& want : wanted) {
		int pick = best_in(want);
		if (pick >= 0) return pick;
	}
	if (fallback >= 0 && fallback < int(fmt->nb_streams)) {
		int pick = best_in(stream_language(fmt->streams[fallback]));
		if (pick >= 0) return pick;
	}
	return fallback;
}

