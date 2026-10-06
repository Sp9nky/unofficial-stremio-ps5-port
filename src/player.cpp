#include "player.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cmath>
#include <cstring>

#include "http.h"
#include "netstream.h"
#include "stremio.h"
#include "torrent/torrent_stream.h"
#include "util.h"

static const int kOutRate = 48000;
static const int kOutBytesPerSec = kOutRate * 2 * 2;  // s16 stereo
static const double kMaxQueuedAudio = 0.35;            // seconds kept in SDL's queue
// Seconds buffered before playback starts. Each time the download falls
// behind and playback has to stop, the next wait is longer (up to 30 s), so a
// stream that's barely fast enough stops a few times for longer instead of
// every few seconds (4K over a torrent, PS5 2026-10-06).
static const double kBufferTargets[] = {2, 5, 10, 20, 30};
// Packets held in memory: 30 s of a 4K remux at ~60 Mbit/s.
static const size_t kMaxQueueBytes = 256u << 20;
static const int kMaxFrames = 4;
static const int kScreenW = 1920, kScreenH = 1080;

static std::string av_err(int err) {
	char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
	av_strerror(err, buf, sizeof(buf));
	return buf;
}

// ---------------------------------------------------------------------------
// Packet queue

void Player::PacketQueue::put(AVPacket* pkt, double pts_s, double dur_s) {
	std::lock_guard<std::mutex> lock(m_);
	q_.push_back(Item{pkt, pts_s, dur_s});
	bytes_ += pkt->size;
	if (pts_s >= 0) last_pts_ = std::max(last_pts_, pts_s);
	dur_sum_ += dur_s;
	cv_.notify_one();
}

int Player::PacketQueue::get(AVPacket** pkt, int* serial, int timeout_ms) {
	std::unique_lock<std::mutex> lock(m_);
	if (q_.empty() && !eof_ && !abort_)
		cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return !q_.empty() || eof_ || abort_; });
	if (abort_) return 0;
	if (q_.empty()) return eof_ ? -1 : 0;
	Item it = q_.front();
	q_.pop_front();
	*pkt = it.pkt;
	bytes_ -= it.pkt->size;
	dur_sum_ = std::max(0.0, dur_sum_ - it.dur);
	*serial = serial_;
	if (q_.empty()) {
		last_pts_ = -1;
		dur_sum_ = 0;
	}
	return 1;
}

void Player::PacketQueue::flush() {
	std::lock_guard<std::mutex> lock(m_);
	for (auto& it : q_) av_packet_free(&it.pkt);
	q_.clear();
	bytes_ = 0;
	last_pts_ = -1;
	dur_sum_ = 0;
	eof_ = false;
	serial_++;
	cv_.notify_all();
}

void Player::PacketQueue::set_eof(bool eof) {
	std::lock_guard<std::mutex> lock(m_);
	eof_ = eof;
	cv_.notify_all();
}

void Player::PacketQueue::abort() {
	std::lock_guard<std::mutex> lock(m_);
	abort_ = true;
	cv_.notify_all();
}

void Player::PacketQueue::reset() {
	std::lock_guard<std::mutex> lock(m_);
	for (auto& it : q_) av_packet_free(&it.pkt);
	q_.clear();
	bytes_ = 0;
	last_pts_ = -1;
	dur_sum_ = 0;
	eof_ = false;
	abort_ = false;
	serial_++;
}

size_t Player::PacketQueue::count() const {
	std::lock_guard<std::mutex> lock(m_);
	return q_.size();
}

size_t Player::PacketQueue::bytes() const {
	std::lock_guard<std::mutex> lock(m_);
	return bytes_;
}

double Player::PacketQueue::duration() const {
	std::lock_guard<std::mutex> lock(m_);
	if (q_.empty()) return 0;
	double first = q_.front().pts;
	double spread = (first >= 0 && last_pts_ >= first) ? last_pts_ - first : 0;
	return std::max(spread, dur_sum_);
}

int Player::PacketQueue::serial() const {
	std::lock_guard<std::mutex> lock(m_);
	return serial_;
}

// ---------------------------------------------------------------------------
// Lifecycle

Player::~Player() { close(); }

void Player::open(const Options& opts) {
	close();
	opts_ = opts;
	abort_ = false;
	quick_ = false;
	eof_ = false;
	paused_ = false;
	buffering_ = true;
	buffer_percent_ = 0;
	dropped_ = decoded_ = shown_ = 0;
	log_at_ = 0;
	stalls_ = 0;
	stall_count_ = 0;
	start_target_ = 0;
	too_heavy_ = false;
	heavy_window_start_ = now_seconds();
	heavy_window_dropped_ = heavy_window_decoded_ = 0;
	video_drained_ = audio_drained_ = false;
	have_picture_ = false;
	seek_req_ = false;
	vdrop_ = adrop_ = opts.start > 0 ? opts.start : -1;
	audio_end_pts_ = -1;
	wall_base_pts_ = opts.start;
	wall_base_time_ = now_seconds();
	wall_running_ = false;
	duration_ = 0;
	fps_ = 0;
	start_offset_ = 0;
	{
		std::lock_guard<std::mutex> lock(err_m_);
		error_.clear();
	}
	{
		std::lock_guard<std::mutex> lock(sub_m_);
		embedded_.clear();
	}
	vq_.reset();
	aq_.reset();
	state_ = State::Opening;
	open_started_ = now_seconds();
	demux_ = std::thread([this] { demux_thread(); });
}

void Player::close() {
	abort_ = true;
	vq_.abort();
	aq_.abort();
	fq_cv_.notify_all();
	if (demux_.joinable()) demux_.join();  // joins the decoder threads too
	if (dev_) {
		SDL_CloseAudioDevice(dev_);
		dev_ = 0;
	}
	if (tex_) {
		SDL_DestroyTexture(tex_);
		tex_ = nullptr;
	}
	tex_w_ = tex_h_ = 0;
	frames_.clear();
	pool_.clear();
	state_ = State::Idle;
}

void Player::fail(const std::string& msg) {
	dlog("player: %s", msg.c_str());
	{
		std::lock_guard<std::mutex> lock(err_m_);
		error_ = msg;
	}
	state_ = State::Failed;
}

std::string Player::error() const {
	std::lock_guard<std::mutex> lock(err_m_);
	return error_;
}

int Player::interrupt_cb(void* opaque) {
	auto* p = static_cast<Player*>(opaque);
	// Only when the player is closed: the network input (NetStream) gives up
	// by itself after two minutes without data. A time limit here was what
	// cut slow torrents off ("Immediate exit requested").
	return p->abort_ ? 1 : 0;
}

double Player::to_sec(int64_t ts, double tb) const {
	if (ts == AV_NOPTS_VALUE) return -1;
	return double(ts) * tb - start_offset_;
}

bool Player::open_stream_codec(int stream, AVCodecContext** out) {
	AVStream* st = fmt_->streams[stream];
	const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
	if (!codec) {
		dlog("player: no decoder for %s", avcodec_get_name(st->codecpar->codec_id));
		return false;
	}
	AVCodecContext* ctx = avcodec_alloc_context3(codec);
	avcodec_parameters_to_context(ctx, st->codecpar);
	ctx->pkt_timebase = st->time_base;
	if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
		// The PS5 has 8 Zen 2 cores; FFmpeg can't count them here, so say it.
		ctx->thread_count = 8;
		ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
	}
	int r = avcodec_open2(ctx, codec, nullptr);
	if (r < 0) {
		dlog("player: can't open %s decoder: %s", codec->name, av_err(r).c_str());
		avcodec_free_context(&ctx);
		return false;
	}
	*out = ctx;
	return true;
}

static bool is_text_subtitle(const AVCodecParameters* par) {
	if (par->codec_type != AVMEDIA_TYPE_SUBTITLE) return false;
	const AVCodecDescriptor* d = avcodec_descriptor_get(par->codec_id);
	return d && (d->props & AV_CODEC_PROP_TEXT_SUB);
}

// ---------------------------------------------------------------------------
// Demuxer

void Player::demux_thread() {
	demux_run();
	// The input is closed by now; the network stream behind it goes last.
	if (TorrentStream::is_ours(main_pb_)) TorrentStream::close_avio(&main_pb_);
	else if (main_pb_) NetStream::close_avio(&main_pb_);
}

void Player::demux_run() {
	AVDictionary* dict = nullptr;
	av_dict_set(&dict, "user_agent", kUserAgent, 0);
	if (!opts_.headers.empty()) av_dict_set(&dict, "headers", (join(opts_.headers, "\r\n") + "\r\n").c_str(), 0);
	av_dict_set(&dict, "reconnect", "1", 0);
	av_dict_set(&dict, "reconnect_streamed", "1", 0);
	av_dict_set(&dict, "reconnect_on_network_error", "1", 0);
	av_dict_set(&dict, "reconnect_delay_max", "5", 0);
	// A torrent's first bytes can take a while (the server is still finding
	// peers); Circle still cancels at once through the interrupt callback.
	av_dict_set(&dict, "rw_timeout", "120000000", 0);
	av_dict_set(&dict, "tls_verify", "0", 0);
	av_dict_set(&dict, "seekable", "-1", 0);
	if (!http_ca_bundle().empty()) av_dict_set(&dict, "ca_file", http_ca_bundle().c_str(), 0);

	fmt_ = avformat_alloc_context();
	fmt_->interrupt_callback.callback = interrupt_cb;
	fmt_->interrupt_callback.opaque = this;
	fmt_->probesize = 5 << 20;
	fmt_->max_analyze_duration = 5 * AV_TIME_BASE;

	// HTTP(S) goes through libcurl (see netstream.h): the stream itself here,
	// and whatever the demuxer opens later (HLS) through the hooks.
	io_hooks_.headers = opts_.headers;
	io_hooks_.abort = &abort_;
	NetStream::install(fmt_, &io_hooks_);
	dlog("player: opening %s", opts_.url.c_str());
	bool torrent = TorrentStream::is_url(opts_.url);
	if (torrent || starts_with(opts_.url, "http://") || starts_with(opts_.url, "https://")) {
		std::string err;
		// A torrent from the built-in engine, or HTTP(S) through libcurl.
		main_pb_ = torrent ? TorrentStream::open_avio(opts_.url, &abort_, &err)
		                   : NetStream::open_avio(opts_.url, opts_.headers, &abort_, &err, opts_.parallel);
		if (!main_pb_) {
			avformat_free_context(fmt_);
			fmt_ = nullptr;
			av_dict_free(&dict);
			if (!abort_) fail("Could not open the stream (" + err + ")");
			return;
		}
		fmt_->pb = main_pb_;
		fmt_->flags |= AVFMT_FLAG_CUSTOM_IO;
		// Matroska keeps tags, attachments and its index at the end of the
		// file, and FFmpeg reads them while opening. For a torrent that's
		// the part the server has least of, so it could take minutes before
		// the first frame. Start from the beginning instead, without them;
		// the first seek opens the file again with the index (see seek()).
		std::string magic = torrent ? TorrentStream::of(main_pb_)->peek(4) : NetStream::of(main_pb_)->peek(4);
		if (opts_.start <= 0 && magic == "\x1A\x45\xDF\xA3") {
			main_pb_->seekable = 0;
			quick_ = true;
			// Matroska describes its tracks in the header, so a short look
			// is enough; with few seeders every MB here is a long wait.
			fmt_->probesize = 1 << 20;
			fmt_->max_analyze_duration = AV_TIME_BASE / 2;
			dlog("player: Matroska, starting without reading the end of the file");
		}
	}
	int r = avformat_open_input(&fmt_, opts_.url.c_str(), nullptr, &dict);
	av_dict_free(&dict);
	if (r < 0) {
		fmt_ = nullptr;  // freed by avformat_open_input on failure
		if (!abort_) fail("Could not open the stream (" + av_err(r) + ")");
		return;
	}
	r = avformat_find_stream_info(fmt_, nullptr);
	if (r < 0 && !abort_) dlog("player: find_stream_info: %s", av_err(r).c_str());
	if (abort_) {
		avformat_close_input(&fmt_);
		return;
	}
	if (fmt_->start_time != AV_NOPTS_VALUE) start_offset_ = double(fmt_->start_time) / AV_TIME_BASE;
	if (fmt_->duration != AV_NOPTS_VALUE && fmt_->duration > 0) duration_ = double(fmt_->duration) / AV_TIME_BASE;

	int vs = av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	int as = -1;
	// Preferred audio language first, else FFmpeg's pick.
	for (auto& want : opts_.audio_langs) {
		for (unsigned i = 0; i < fmt_->nb_streams && as < 0; i++) {
			AVStream* st = fmt_->streams[i];
			if (st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
			AVDictionaryEntry* e = av_dict_get(st->metadata, "language", nullptr, 0);
			if (e && language_to_iso639_2(e->value) == want) as = int(i);
		}
		if (as >= 0) break;
	}
	if (as < 0) as = av_find_best_stream(fmt_, AVMEDIA_TYPE_AUDIO, -1, vs, nullptr, 0);
	if (vs < 0 && as < 0) {
		avformat_close_input(&fmt_);
		fail("No video or audio in this stream");
		return;
	}

	// Colours, from the stream's tags: HDR10 (PQ) gets tone mapped; SD
	// pictures use BT.601, everything else BT.709.
	if (vs >= 0) {
		const AVCodecParameters* p = fmt_->streams[vs]->codecpar;
		bool sd_space = p->color_space == AVCOL_SPC_BT470BG || p->color_space == AVCOL_SPC_SMPTE170M;
		colors_ = p->color_trc == AVCOL_TRC_SMPTE2084 ? YuvColors::Hdr10
		          : (sd_space || (p->color_space == AVCOL_SPC_UNSPECIFIED && p->height < 720)) ? YuvColors::Bt601
		                                                                                       : YuvColors::Bt709;
		dlog("player: colours %s (transfer %d, matrix %d, primaries %d)",
		     colors_ == YuvColors::Hdr10 ? "HDR10, tone mapped" : colors_ == YuvColors::Bt601 ? "BT.601" : "BT.709",
		     int(p->color_trc), int(p->color_space), int(p->color_primaries));
	}
	// Video: the hardware decoder when it takes the stream; FFmpeg's software
	// decoder otherwise (and if the hardware one fails later).
	if (vs >= 0) {
		std::string why;
		hw_.reset(HwDecoder::open(fmt_->streams[vs]->codecpar, &why));
		hw_active_ = hw_ != nullptr;
		hw_shift_ = -1;
		if (!hw_) dlog("player: software video decoding (%s)", why.c_str());
	}
	if (vs >= 0 && !hw_ && !open_stream_codec(vs, &vctx_)) {
		AVStream* st = fmt_->streams[vs];
		std::string name = avcodec_get_name(st->codecpar->codec_id);
		avformat_close_input(&fmt_);
		fail("This video's format (" + name + ") can't be played. Pick another stream.");
		return;
	}
	if (as >= 0 && !open_stream_codec(as, &actx_)) as = -1;
	for (unsigned i = 0; i < fmt_->nb_streams; i++) {
		AVStream* st = fmt_->streams[i];
		bool keep = int(i) == vs || int(i) == as;
		if (is_text_subtitle(st->codecpar)) {
			AVCodecContext* sc = nullptr;
			if (open_stream_codec(int(i), &sc)) {
				sctx_[int(i)] = sc;
				keep = true;
			}
		}
		// Other audio tracks stay readable so they can be switched to.
		if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) keep = true;
		st->discard = keep ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
	}
	video_stream_ = vs;
	audio_stream_ = as;
	if (vs < 0) vdrop_ = -1;
	if (vs >= 0) {
		AVRational fr = av_guess_frame_rate(fmt_, fmt_->streams[vs], nullptr);
		if (fr.num > 0 && fr.den > 0) fps_ = av_q2d(fr);
	}

	if (as >= 0) {
		SDL_AudioSpec want, have;
		SDL_zero(want);
		want.freq = kOutRate;
		want.format = AUDIO_S16SYS;
		want.channels = 2;
		want.samples = 1024;
		dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
		if (!dev_) dlog("player: SDL_OpenAudioDevice: %s", SDL_GetError());
	}

	dlog("player: %s, %.0fs, video #%d %s, audio #%d %s", fmt_->iformat->name, duration_, vs,
	     vs >= 0 ? avcodec_get_name(fmt_->streams[vs]->codecpar->codec_id) : "-", as,
	     as >= 0 ? avcodec_get_name(fmt_->streams[as]->codecpar->codec_id) : "-");

	// How much to buffer before playing (and after a seek): a high-bitrate
	// file (a 4K remux) needs more, or playback starts before the download
	// is up to speed and runs dry within the first minute (PS5 2026-10-06:
	// Avengers: Endgame 4K, ~25 Mbit/s, stopped once at 45 s with 2 s buffered).
	{
		int64_t size = fmt_->pb ? avio_size(fmt_->pb) : -1;
		double mbps = fmt_->bit_rate > 0 ? fmt_->bit_rate / 1e6
		              : (size > 0 && duration_ > 0) ? size * 8.0 / duration_ / 1e6
		                                            : 0;
		start_target_ = mbps >= 40 ? 15 : mbps >= 20 ? 10 : mbps >= 10 ? 5 : 2;
		dlog("player: about %.0f Mbit/s, buffering %.0f s before playing", mbps, start_target_);
	}
	if (opts_.start > 0) do_seek(opts_.start);
	state_ = State::Playing;
	apply_run_state();

	if (vs >= 0) video_ = std::thread([this] { video_thread(); });
	if (as >= 0 && dev_) audio_ = std::thread([this] { audio_thread(); });

	AVPacket* pkt = av_packet_alloc();
	int errors = 0;
	while (!abort_) {
		double target = -1;
		{
			std::lock_guard<std::mutex> lock(seek_m_);
			if (seek_req_) {
				target = seek_target_;
				seek_req_ = false;
			}
		}
		int sw = audio_switch_.exchange(-1);
		if (sw >= 0 && sw != audio_stream_) {
			AVCodecContext* nc = nullptr;
			if (open_stream_codec(sw, &nc)) {
				std::lock_guard<std::mutex> lock(actx_m_);
				avcodec_free_context(&actx_);
				actx_ = nc;
				audio_stream_ = sw;
				// Without the index (quick_) it just carries on from here.
				if (target < 0 && !quick_) target = position();
			}
		}
		if (target >= 0) do_seek(target);

		update_buffering();

		if (vq_.bytes() + aq_.bytes() > kMaxQueueBytes ||
		    ((vs < 0 || vq_.duration() > 60) && (audio_stream_ < 0 || aq_.duration() > 60)) || eof_) {
			// Full, or at the end: wait for the decoders (or a seek).
			SDL_Delay(10);
			continue;
		}

		r = av_read_frame(fmt_, pkt);
		if (r < 0) {
			if (abort_) break;
			// The download gave up (not the end of the video): say so,
			// rather than ending playback as if the video were over.
			std::string why;
			if (NetStream* ns = main_pb_ ? NetStream::of(main_pb_) : nullptr) why = ns->failure();
			if (TorrentStream* ts = main_pb_ ? TorrentStream::of(main_pb_) : nullptr) why = ts->failure();
			if (!why.empty()) {
				fail("The stream stopped (" + why + ")");
				break;
			}
			bool at_end = r == AVERROR_EOF || (fmt_->pb && avio_feof(fmt_->pb));
			if (!at_end && ++errors < 60) {
				SDL_Delay(100);
				continue;
			}
			if (!at_end) dlog("player: read error %s, treating as end", av_err(r).c_str());
			eof_ = true;
			vq_.set_eof(true);
			aq_.set_eof(true);
			continue;
		}
		errors = 0;
		AVStream* st = fmt_->streams[pkt->stream_index];
		double tb = av_q2d(st->time_base);
		double pts = to_sec(pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts, tb);
		double dur = pkt->duration > 0 ? pkt->duration * tb : 0;
		if (pkt->stream_index == vs) {
			AVPacket* q = av_packet_alloc();
			av_packet_move_ref(q, pkt);
			vq_.put(q, pts, dur);
		} else if (pkt->stream_index == audio_stream_) {
			AVPacket* q = av_packet_alloc();
			av_packet_move_ref(q, pkt);
			aq_.put(q, pts, dur);
		} else {
			if (sctx_.count(pkt->stream_index)) handle_subtitle_packet(pkt);
			av_packet_unref(pkt);
		}
	}
	av_packet_free(&pkt);

	vq_.abort();
	aq_.abort();
	fq_cv_.notify_all();
	if (video_.joinable()) video_.join();
	if (audio_.joinable()) audio_.join();

	avcodec_free_context(&vctx_);
	hw_.reset();
	hw_active_ = false;
	if (hw_sws_) {
		sws_freeContext(hw_sws_);
		hw_sws_ = nullptr;
	}
	{
		std::lock_guard<std::mutex> lock(actx_m_);
		avcodec_free_context(&actx_);
	}
	for (auto& kv : sctx_) avcodec_free_context(&kv.second);
	sctx_.clear();
	if (sws_) {
		sws_freeContext(sws_);
		sws_ = nullptr;
	}
	if (swr_) swr_free(&swr_);
	avformat_close_input(&fmt_);
	video_stream_ = audio_stream_ = -1;
}

void Player::do_seek(double target) {
	int64_t ts = int64_t((target + start_offset_) * AV_TIME_BASE);
	int r = avformat_seek_file(fmt_, -1, INT64_MIN, ts, ts, 0);
	if (r < 0) r = av_seek_frame(fmt_, -1, ts, AVSEEK_FLAG_BACKWARD);
	if (r < 0) dlog("player: seek to %.1f failed: %s", target, av_err(r).c_str());
	vq_.flush();
	aq_.flush();
	eof_ = false;
	video_drained_ = audio_drained_ = false;
	vdrop_ = video_stream_ >= 0 ? target : -1;
	adrop_ = target;
	{
		std::lock_guard<std::mutex> lock(clock_m_);
		audio_end_pts_ = -1;
		wall_base_pts_ = target;
		wall_base_time_ = now_seconds();
		if (dev_) SDL_ClearQueuedAudio(dev_);
	}
	buffering_ = true;
	buffer_percent_ = 0;
	apply_run_state();
}

void Player::handle_subtitle_packet(AVPacket* pkt) {
	AVCodecContext* ctx = sctx_[pkt->stream_index];
	AVSubtitle sub;
	int got = 0;
	if (avcodec_decode_subtitle2(ctx, &sub, &got, pkt) < 0 || !got) return;
	double tb = av_q2d(fmt_->streams[pkt->stream_index]->time_base);
	double pts = to_sec(pkt->pts, tb);
	if (pts < 0) {
		avsubtitle_free(&sub);
		return;
	}
	double start = pts + sub.start_display_time / 1000.0;
	double end = sub.end_display_time > sub.start_display_time ? pts + sub.end_display_time / 1000.0
	             : pkt->duration > 0                         ? pts + pkt->duration * tb
	                                                         : start + 4;
	std::string text;
	for (unsigned i = 0; i < sub.num_rects; i++) {
		AVSubtitleRect* rect = sub.rects[i];
		std::string t;
		if (rect->ass) {
			// "ReadOrder,Layer,Style,Name,MarginL,MarginR,MarginV,Effect,Text"
			const char* p = rect->ass;
			for (int commas = 0; *p && commas < 8; p++)
				if (*p == ',') commas++;
			t = p;
		} else if (rect->text) {
			t = rect->text;
		}
		if (t.empty()) continue;
		if (!text.empty()) text += "\\N";
		text += t;
	}
	avsubtitle_free(&sub);
	if (text.empty()) return;
	Cue c;
	c.start = start;
	c.end = end;
	c.rml = subtitle_text_to_rml(text);
	std::lock_guard<std::mutex> lock(sub_m_);
	auto& cues = embedded_[pkt->stream_index];
	auto it = std::lower_bound(cues.begin(), cues.end(), c.start, [](const Cue& a, double v) { return a.start < v; });
	for (auto j = it; j != cues.end() && j->start == c.start; ++j)
		if (j->rml == c.rml) return;  // seen before (seeking back re-reads packets)
	cues.insert(it, c);
}

// ---------------------------------------------------------------------------
// Video

void Player::video_thread() {
	if (hw_ && video_thread_hw()) return;
	AVPacket* pkt = nullptr;
	AVFrame* frame = av_frame_alloc();
	int serial = vq_.serial();
	bool sent_eof = false;
	double tb = av_q2d(fmt_->streams[video_stream_]->time_base);

	while (!abort_) {
		int s = 0;
		int r = vq_.get(&pkt, &s, 50);
		if (abort_) break;
		if (r == 0) continue;
		if (r == 1 && s != serial) {
			avcodec_flush_buffers(vctx_);
			serial = s;
			sent_eof = false;
			video_drained_ = false;
		}
		if (r == -1) {
			if (sent_eof) {
				SDL_Delay(10);
				if (vq_.serial() != serial) {
					avcodec_flush_buffers(vctx_);
					serial = vq_.serial();
					sent_eof = false;
					video_drained_ = false;
				}
				continue;
			}
			avcodec_send_packet(vctx_, nullptr);  // drain
			sent_eof = true;
		} else {
			int sr = avcodec_send_packet(vctx_, pkt);
			if (sr < 0 && sr != AVERROR(EAGAIN)) dlog("player: video send: %s", av_err(sr).c_str());
			av_packet_free(&pkt);
		}

		while (!abort_) {
			int rr = avcodec_receive_frame(vctx_, frame);
			if (rr == AVERROR_EOF) {
				video_drained_ = true;
				break;
			}
			if (rr < 0) break;
			decoded_++;
			double pts = to_sec(frame->best_effort_timestamp, tb);
			double sar = frame->sample_aspect_ratio.num > 0 ? av_q2d(frame->sample_aspect_ratio) : 1.0;
			bool ok = emit_frame(pts, serial, [&](Frame& f) {
				// 4:2:0 HDR (and plain 4:2:0) through the same conversion as
				// hardware pictures; anything else through swscale.
				if (frame->format == AV_PIX_FMT_YUV420P10LE || frame->format == AV_PIX_FMT_YUV420P) {
					YuvPicture yp;
					yp.y = frame->data[0];
					yp.u = frame->data[1];
					yp.v = frame->data[2];
					yp.y_stride = frame->linesize[0];
					yp.c_stride = frame->linesize[1];
					yp.wide = frame->format == AV_PIX_FMT_YUV420P10LE;
					yp.width = frame->width;
					yp.height = frame->height;
					if (frame->linesize[1] == frame->linesize[2] && fast_convert(yp, f)) return;
				}
				sws_ = sws_getCachedContext(sws_, frame->width, frame->height, AVPixelFormat(frame->format), f.w, f.h,
				                            AV_PIX_FMT_BGRA,
				                            (f.w == frame->width && f.h == frame->height) ? SWS_POINT : SWS_BILINEAR,
				                            nullptr, nullptr, nullptr);
				if (!sws_) return;
				uint8_t* dst[4] = {f.pixels.data(), nullptr, nullptr, nullptr};
				int dst_stride[4] = {f.w * 4, 0, 0, 0};
				sws_scale(sws_, frame->data, frame->linesize, 0, frame->height, dst, dst_stride);
			}, frame->width, frame->height, sar);
			av_frame_unref(frame);
			if (!ok) break;
		}
	}
	if (pkt) av_packet_free(&pkt);
	av_frame_free(&frame);
}

// A decoded picture into the frame queue, scaled to the screen by `fill`.
// Skips pictures before a seek target or hopelessly late ones. False when
// playback was closed or seeked meanwhile.
bool Player::emit_frame(double pts, int serial, const std::function<void(Frame&)>& fill, int src_w, int src_h,
                        double sar) {
	double drop = vdrop_;
	if (drop >= 0) {
		if (pts >= 0 && pts < drop - 0.02) return true;
		vdrop_ = -1;
	}
	// Hopelessly late and something else is ready: skip the conversion.
	// (Never hold clock_m_ while taking fq_m_: present() nests them the other way.)
	{
		double clk;
		{
			std::lock_guard<std::mutex> cl(clock_m_);
			clk = clock_locked();
		}
		std::lock_guard<std::mutex> fl(fq_m_);
		if (!buffering_ && !frames_.empty() && pts >= 0 && pts < clk - 0.2) {
			dropped_++;
			return true;
		}
	}

	// Wait for room in the frame queue.
	std::unique_lock<std::mutex> lock(fq_m_);
	fq_cv_.wait_for(lock, std::chrono::milliseconds(500), [&] {
		return abort_ || int(frames_.size()) < kMaxFrames || vq_.serial() != serial;
	});
	while (!abort_ && int(frames_.size()) >= kMaxFrames && vq_.serial() == serial)
		fq_cv_.wait_for(lock, std::chrono::milliseconds(50));
	if (abort_ || vq_.serial() != serial) return false;
	Frame f;
	if (!pool_.empty()) {
		f = std::move(pool_.back());
		pool_.pop_back();
	}
	lock.unlock();

	// Scale to fit the screen, keeping the display aspect ratio.
	double dar = src_w * sar / src_h;
	int dw = kScreenW, dh = int(kScreenW / dar);
	if (dh > kScreenH) {
		dh = kScreenH;
		dw = int(kScreenH * dar);
	}
	f.w = dw & ~1;
	f.h = dh & ~1;
	f.pixels.resize(size_t(f.w) * f.h * 4);
	fill(f);
	f.pts = pts;
	f.serial = serial;

	lock.lock();
	frames_.push_back(std::move(f));
	return true;
}

// ---------------------------------------------------------------------------
// Hardware decoding

namespace {

// A few threads that convert one picture together: the conversion of a 4K
// picture is too much for one core at 60 frames a second.
class RowPool {
public:
	static RowPool& get() {
		static RowPool* p = new RowPool();  // never destroyed: threads outlive statics at exit
		return *p;
	}
	// fn(first_row, end_row) over [0, rows), split between the threads.
	void run(int rows, const std::function<void(int, int)>& fn) {
		const int parts = kThreads + 1;
		std::unique_lock<std::mutex> lock(m_);
		fn_ = &fn;
		rows_ = rows;
		parts_ = parts;
		pending_ = kThreads;
		gen_++;
		cv_.notify_all();
		lock.unlock();
		fn(0, rows / parts);  // this thread does the first part
		lock.lock();
		done_cv_.wait(lock, [&] { return pending_ == 0; });
		fn_ = nullptr;
	}

private:
	static const int kThreads = 3;
	RowPool() {
		for (int i = 0; i < kThreads; i++) std::thread([this, i] { loop(i + 1); }).detach();
	}
	void loop(int part) {
		int seen = 0;
		for (;;) {
			std::unique_lock<std::mutex> lock(m_);
			cv_.wait(lock, [&] { return gen_ != seen; });
			seen = gen_;
			const std::function<void(int, int)>* fn = fn_;
			int rows = rows_, parts = parts_;
			lock.unlock();
			int a = rows * part / parts, b = rows * (part + 1) / parts;
			if (fn && a < b) (*fn)(a, b);
			lock.lock();
			if (--pending_ == 0) done_cv_.notify_all();
		}
	}
	std::mutex m_;
	std::condition_variable cv_, done_cv_;
	const std::function<void(int, int)>* fn_ = nullptr;
	int rows_ = 0, parts_ = 1, pending_ = 0, gen_ = 0;
};

}  // namespace

// A picture to the BGRA frame with the right colours (yuv_convert.h), at the
// same size or exactly half (4K to the 1080p screen), on four threads.
// False for other sizes (the caller uses swscale).
bool Player::fast_convert(const YuvPicture& pic, Frame& f) {
	auto near = [](int a, int b) { return a >= b - 1 && a <= b + 1; };
	int scale = (f.w == pic.width && f.h == pic.height) ? 1
	            : (near(f.w * 2, pic.width) && near(f.h * 2, pic.height)) ? 2
	                                                                      : 0;
	if (!scale) return false;
	YuvColors colors = colors_;
	RowPool::get().run(
	    f.h, [&](int a, int b) { yuv_to_bgra(pic, colors, scale, f.pixels.data(), f.w, a, b); });
	return true;
}

// A hardware picture: two planes (Y, then interleaved UV), 8 or 16-bit samples.
void Player::hw_to_bgra(const HwDecoder::Picture& pic, Frame& f) {
	if (pic.ten_bit && hw_shift_ < 0) {
		// 10-bit samples sit either in the low or the high bits of each
		// 16-bit word; look at a few to tell.
		int max = 0;
		for (int y = 0; y < pic.height; y += 97) {
			auto* row = reinterpret_cast<const uint16_t*>(pic.y + size_t(y) * pic.pitch);
			for (int x = 0; x < pic.width; x += 31) max = std::max<int>(max, row[x]);
		}
		hw_shift_ = max > 1023 ? 6 : 0;
		dlog("player: 10-bit pictures, samples in the %s bits", hw_shift_ ? "high" : "low");
	}
	YuvPicture yp;
	yp.y = pic.y;
	yp.u = pic.uv;
	yp.y_stride = yp.c_stride = pic.pitch;
	yp.interleaved = true;
	yp.wide = pic.ten_bit;
	yp.shift = pic.ten_bit ? hw_shift_ : 0;
	yp.width = pic.width;
	yp.height = pic.height;
	if (fast_convert(yp, f)) return;
	// Other sizes: swscale from NV12 / P010.
	AVPixelFormat fmt = pic.ten_bit ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12;
	hw_sws_ = sws_getCachedContext(hw_sws_, pic.width, pic.height, fmt, f.w, f.h, AV_PIX_FMT_BGRA, SWS_BILINEAR,
	                               nullptr, nullptr, nullptr);
	if (!hw_sws_) return;
	const uint8_t* src[4] = {pic.y, pic.uv, nullptr, nullptr};
	int src_stride[4] = {pic.pitch, pic.pitch, 0, 0};
	uint8_t* dst[4] = {f.pixels.data(), nullptr, nullptr, nullptr};
	int dst_stride[4] = {f.w * 4, 0, 0, 0};
	sws_scale(hw_sws_, src, src_stride, 0, pic.height, dst, dst_stride);
}

// The video thread while the hardware decoder plays. Returns true when
// playback ended or closed; false when the decoder failed and the software
// decoder should take over from here.
bool Player::video_thread_hw() {
	AVPacket* pkt = nullptr;
	int serial = vq_.serial();
	bool sent_eof = false;
	AVStream* st = fmt_->streams[video_stream_];
	double tb = av_q2d(st->time_base);
	double sar = st->codecpar->sample_aspect_ratio.num > 0 ? av_q2d(st->codecpar->sample_aspect_ratio) : 1.0;
	bool failed = false;

	auto drain = [&]() -> bool {  // decoded pictures out; false if playback moved on
		HwDecoder::Picture pic;
		for (;;) {
			int rr = hw_->receive(&pic);
			if (rr < 0) {
				failed = true;
				return true;
			}
			if (rr == 0) return true;
			decoded_++;
			double pts = pic.pts_us == INT64_MIN ? -1 : double(pic.pts_us) / 1e6;
			if (!emit_frame(pts, serial, [&](Frame& f) { hw_to_bgra(pic, f); }, pic.width, pic.height, sar))
				return false;
		}
	};

	while (!abort_ && !failed) {
		int s = 0;
		int r = vq_.get(&pkt, &s, 50);
		if (abort_) break;
		if (r == 0) continue;
		if (r == 1 && s != serial) {
			hw_->flush();
			serial = s;
			sent_eof = false;
			video_drained_ = false;
		}
		if (r == -1) {
			if (sent_eof) {
				SDL_Delay(10);
				if (vq_.serial() != serial) {
					hw_->flush();
					serial = vq_.serial();
					sent_eof = false;
					video_drained_ = false;
				}
				continue;
			}
			hw_->send(nullptr, 0, 0);
			sent_eof = true;
			drain();
			video_drained_ = true;
			continue;
		}
		double ps = pkt->pts != AV_NOPTS_VALUE ? to_sec(pkt->pts, tb) : -1;
		int64_t pts_us = pkt->pts != AV_NOPTS_VALUE ? int64_t(ps * 1e6) : INT64_MIN;
		for (;;) {
			int sr = hw_->send(pkt->data, pkt->size, pts_us);
			if (sr == 1) {  // pictures waiting: take them first
				if (!drain()) break;
				continue;
			}
			if (sr < 0) failed = true;
			break;
		}
		av_packet_free(&pkt);
		if (!failed) drain();
	}
	if (pkt) av_packet_free(&pkt);
	if (!failed) return true;

	// The hardware decoder gave up on this stream: software from here on
	// (it picks up at the next keyframe).
	dlog("player: the hardware decoder failed; switching to software decoding");
	hw_.reset();
	hw_active_ = false;
	if (!open_stream_codec(video_stream_, &vctx_)) {
		fail("This video can't be decoded. Pick another stream.");
		return true;
	}
	return false;
}

// ---------------------------------------------------------------------------
// Audio

double Player::queued_audio_seconds() const {
	if (!dev_) return 0;
	return double(SDL_GetQueuedAudioSize(dev_)) / kOutBytesPerSec;
}

void Player::audio_thread() {
	AVPacket* pkt = nullptr;
	AVFrame* frame = av_frame_alloc();
	int serial = aq_.serial();
	bool sent_eof = false;
	std::vector<uint8_t> out;
	int swr_rate = 0, swr_fmt = -1;
	AVChannelLayout swr_layout;
	memset(&swr_layout, 0, sizeof(swr_layout));

	while (!abort_) {
		int s = 0;
		int r = aq_.get(&pkt, &s, 50);
		if (abort_) break;
		if (r == 0) continue;
		std::unique_lock<std::mutex> alock(actx_m_);
		if (!actx_) {
			if (r == 1) av_packet_free(&pkt);
			continue;
		}
		if (r == 1 && s != serial) {
			avcodec_flush_buffers(actx_);
			serial = s;
			sent_eof = false;
			audio_drained_ = false;
			if (swr_) swr_free(&swr_);
		}
		if (r == -1) {
			if (sent_eof) {
				alock.unlock();
				SDL_Delay(10);
				continue;
			}
			avcodec_send_packet(actx_, nullptr);
			sent_eof = true;
		} else {
			avcodec_send_packet(actx_, pkt);
			av_packet_free(&pkt);
		}
		double tb = av_q2d(fmt_->streams[audio_stream_]->time_base);

		while (!abort_) {
			int rr = avcodec_receive_frame(actx_, frame);
			if (rr == AVERROR_EOF) {
				audio_drained_ = true;
				break;
			}
			if (rr < 0) break;
			double pts = to_sec(frame->best_effort_timestamp, tb);
			double drop = adrop_;
			if (drop >= 0) {
				if (pts >= 0 && pts + double(frame->nb_samples) / frame->sample_rate < drop) {
					av_frame_unref(frame);
					continue;
				}
				adrop_ = -1;
			}

			AVChannelLayout in_layout;
			if (frame->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC || frame->ch_layout.nb_channels == 0)
				av_channel_layout_default(&in_layout, frame->ch_layout.nb_channels ? frame->ch_layout.nb_channels : 2);
			else
				av_channel_layout_copy(&in_layout, &frame->ch_layout);
			if (!swr_ || swr_rate != frame->sample_rate || swr_fmt != frame->format ||
			    av_channel_layout_compare(&swr_layout, &in_layout) != 0) {
				if (swr_) swr_free(&swr_);
				AVChannelLayout stereo;
				av_channel_layout_default(&stereo, 2);
				swr_alloc_set_opts2(&swr_, &stereo, AV_SAMPLE_FMT_S16, kOutRate, &in_layout,
				                    AVSampleFormat(frame->format), frame->sample_rate, 0, nullptr);
				if (!swr_ || swr_init(swr_) < 0) {
					dlog("player: can't resample audio");
					if (swr_) swr_free(&swr_);
					av_channel_layout_uninit(&in_layout);
					av_frame_unref(frame);
					continue;
				}
				swr_rate = frame->sample_rate;
				swr_fmt = frame->format;
				av_channel_layout_uninit(&swr_layout);
				av_channel_layout_copy(&swr_layout, &in_layout);
			}
			av_channel_layout_uninit(&in_layout);

			int max_out = swr_get_out_samples(swr_, frame->nb_samples) + 32;
			out.resize(size_t(max_out) * 4);
			uint8_t* outp[1] = {out.data()};
			int n = swr_convert(swr_, outp, max_out, (const uint8_t**)frame->extended_data, frame->nb_samples);
			av_frame_unref(frame);
			if (n <= 0) continue;

			// Keep SDL's queue short so the clock (and seeking) stay responsive.
			alock.unlock();
			while (!abort_ && aq_.serial() == serial && queued_audio_seconds() > kMaxQueuedAudio) SDL_Delay(5);
			alock.lock();
			if (abort_ || aq_.serial() != serial || !actx_) break;

			std::lock_guard<std::mutex> lock(clock_m_);
			if (aq_.serial() != serial) break;
			SDL_QueueAudio(dev_, out.data(), Uint32(n * 4));
			double end = (pts >= 0 ? pts : std::max(0.0, audio_end_pts_)) + double(n) / kOutRate;
			if (pts < 0 && audio_end_pts_ >= 0) end = audio_end_pts_ + double(n) / kOutRate;
			audio_end_pts_ = end;
		}
	}
	av_channel_layout_uninit(&swr_layout);
	if (pkt) av_packet_free(&pkt);
	av_frame_free(&frame);
}

// ---------------------------------------------------------------------------
// Clock, buffering, presentation

double Player::clock_locked() {
	if (audio_stream_ >= 0 && dev_) {
		if (audio_end_pts_ < 0) return wall_base_pts_;
		double latency = 1024.0 / kOutRate;
		return std::max(0.0, audio_end_pts_ - queued_audio_seconds() - latency);
	}
	if (!wall_running_) return wall_base_pts_;
	return wall_base_pts_ + (now_seconds() - wall_base_time_);
}

void Player::set_clock_running(bool running) {
	std::lock_guard<std::mutex> lock(clock_m_);
	if (running == wall_running_) return;
	if (running) {
		wall_base_time_ = now_seconds();
	} else {
		wall_base_pts_ = wall_base_pts_ + (now_seconds() - wall_base_time_);
	}
	wall_running_ = running;
}

void Player::apply_run_state() {
	bool run = !paused_ && !buffering_ && state_ == State::Playing;
	if (dev_) SDL_PauseAudioDevice(dev_, run ? 0 : 1);
	set_clock_running(run);
}

void Player::set_paused(bool p) {
	paused_ = p;
	apply_run_state();
}

bool Player::buffering(int* percent) const {
	if (percent) *percent = buffer_percent_;
	return state_ == State::Opening || (state_ == State::Playing && buffering_);
}

void Player::update_buffering() {
	if (state_ != State::Playing) return;
	bool has_v = video_stream_ >= 0, has_a = audio_stream_ >= 0 && dev_;
	if (buffering_) {
		double target = std::max(kBufferTargets[stalls_], start_target_);
		double vbuf = has_v ? vq_.duration() : 1e9, abuf = has_a ? aq_.duration() : 1e9;
		double buf = std::min(vbuf, abuf);
		buffer_percent_ = int(std::min(100.0, buf / target * 100));
		bool have_frame;
		{
			std::lock_guard<std::mutex> lock(fq_m_);
			have_frame = !has_v || !frames_.empty();
		}
		// Also go on when memory is nearly full (a very high bitrate).
		if (eof_ || (buf >= target && have_frame) || vq_.bytes() + aq_.bytes() > kMaxQueueBytes * 9 / 10) {
			if (stalls_ > 0) dlog("player: buffered %.1f s, playing again", buf);
			buffering_ = false;
			apply_run_state();
		}
		return;
	}
	if (eof_) return;
	bool starving = false;
	if (has_v) {
		std::lock_guard<std::mutex> lock(fq_m_);
		if (frames_.empty() && vq_.count() == 0 && !video_drained_) starving = true;
	}
	if (has_a && aq_.count() == 0 && queued_audio_seconds() < 0.05 && !audio_drained_) starving = true;
	if (starving) {
		// The download fell behind: wait longer this time.
		const int last = int(sizeof(kBufferTargets) / sizeof(kBufferTargets[0])) - 1;
		if (stalls_ < last) stalls_++;
		dlog("player: the stream fell behind (stop %d); buffering %.0f s before going on", ++stall_count_,
		     std::max(kBufferTargets[stalls_], start_target_));
		buffering_ = true;
		buffer_percent_ = 0;
		apply_run_state();
	}
}

double Player::position() {
	{
		std::lock_guard<std::mutex> lock(seek_m_);
		if (seek_req_) return seek_target_;
	}
	double drop = vdrop_;
	if (drop >= 0) return drop;
	std::lock_guard<std::mutex> lock(clock_m_);
	return clock_locked();
}

void Player::seek(double t) {
	if (duration_ > 0) t = std::min(t, std::max(0.0, duration_ - 2));
	t = std::max(0.0, t);
	if (quick_) {
		// Opened without the file's index (see demux_run): open again,
		// properly this time, at the new spot. Same file, same tracks.
		dlog("player: first seek, reopening at %.1fs with the index", t);
		Options o = opts_;
		o.start = t;
		open(o);
		return;
	}
	std::lock_guard<std::mutex> lock(seek_m_);
	seek_req_ = true;
	seek_target_ = t;
	have_picture_ = false;
	if (state_ == State::Ended) state_ = State::Playing;
}

void Player::present(SDL_Renderer* renderer) {
	if (state_ == State::Playing) update_buffering();
	if (state_ == State::Playing || state_ == State::Ended) {
		double clk;
		{
			std::lock_guard<std::mutex> lock(clock_m_);
			clk = clock_locked();
		}
		std::lock_guard<std::mutex> lock(fq_m_);
		int cur = vq_.serial();
		while (!frames_.empty() && frames_.front().serial != cur) {
			pool_.push_back(std::move(frames_.front()));
			frames_.pop_front();
		}
		int pick = -1;
		for (int i = 0; i < int(frames_.size()); i++) {
			if (frames_[i].pts <= clk + 0.008) pick = i;
			else break;
		}
		if (pick < 0 && !have_picture_ && !frames_.empty()) pick = 0;
		if (pick >= 0) {
			Frame& f = frames_[pick];
			if (!tex_ || tex_w_ != f.w || tex_h_ != f.h) {
				if (tex_) SDL_DestroyTexture(tex_);
				tex_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, f.w, f.h);
				tex_w_ = f.w;
				tex_h_ = f.h;
			}
			if (tex_) SDL_UpdateTexture(tex_, nullptr, f.pixels.data(), f.w * 4);
			have_picture_ = true;
			shown_++;
			// No audio: the first picture after a seek starts the clock.
			if (audio_stream_ < 0 && pick == 0 && !wall_running_) {
				std::lock_guard<std::mutex> cl(clock_m_);
				wall_base_pts_ = f.pts;
			}
			dropped_ += pick;
			heavy_window_dropped_ += pick;
			heavy_window_decoded_ += pick + 1;
			for (int i = 0; i <= pick; i++) {
				pool_.push_back(std::move(frames_.front()));
				frames_.pop_front();
			}
			fq_cv_.notify_all();
		}

		// Ended?
		if (state_ == State::Playing && eof_ && vq_.count() == 0 && aq_.count() == 0 &&
		    (video_stream_ < 0 || (video_drained_ && frames_.empty())) &&
		    (audio_stream_ < 0 || !dev_ || (audio_drained_ && queued_audio_seconds() < 0.02)))
			state_ = State::Ended;
	}

	double now = now_seconds();
	if (now - heavy_window_start_ > 8) {
		if (!paused_ && !buffering_ && heavy_window_decoded_ > 60 && heavy_window_dropped_ * 4 > heavy_window_decoded_)
			too_heavy_ = true;
		heavy_window_start_ = now;
		heavy_window_dropped_ = heavy_window_decoded_ = 0;
	}

	if (tex_ && have_picture_) {
		SDL_Rect dst = {(kScreenW - tex_w_) / 2, (kScreenH - tex_h_) / 2, tex_w_, tex_h_};
		SDL_RenderCopy(renderer, tex_, nullptr, &dst);
	}
}

// ---------------------------------------------------------------------------
// Tracks and stats

std::string Player::track_label(int stream) const {
	if (!fmt_ || stream < 0 || stream >= int(fmt_->nb_streams)) return "";
	AVStream* st = fmt_->streams[stream];
	AVDictionaryEntry* lang = av_dict_get(st->metadata, "language", nullptr, 0);
	AVDictionaryEntry* title = av_dict_get(st->metadata, "title", nullptr, 0);
	std::vector<std::string> parts;
	if (lang && strcmp(lang->value, "und") != 0) parts.push_back(language_name(lang->value));
	if (title && title->value[0]) parts.push_back(title->value);
	std::string codec = avcodec_get_name(st->codecpar->codec_id);
	for (auto& c : codec) c = char(toupper((unsigned char)c));
	if (codec == "SUBRIP") codec = "SRT";
	if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
		int ch = st->codecpar->ch_layout.nb_channels;
		std::string chs = ch == 1 ? "mono" : ch == 2 ? "stereo" : ch == 6 ? "5.1" : ch == 8 ? "7.1" : std::to_string(ch) + "ch";
		codec += " " + chs;
	}
	parts.push_back(codec);
	if (st->disposition & AV_DISPOSITION_FORCED) parts.push_back("forced");
	return join(parts, " · ");
}

std::vector<Player::Track> Player::audio_tracks() const {
	std::vector<Track> out;
	if (!fmt_ || state_ != State::Playing) return out;
	for (unsigned i = 0; i < fmt_->nb_streams; i++) {
		AVStream* st = fmt_->streams[i];
		if (st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
		AVDictionaryEntry* lang = av_dict_get(st->metadata, "language", nullptr, 0);
		out.push_back(Track{int(i), track_label(int(i)), lang ? language_to_iso639_2(lang->value) : ""});
	}
	return out;
}

std::vector<Player::Track> Player::subtitle_tracks() const {
	std::vector<Track> out;
	if (!fmt_ || state_ != State::Playing) return out;
	for (auto& kv : sctx_) {
		AVStream* st = fmt_->streams[kv.first];
		AVDictionaryEntry* lang = av_dict_get(st->metadata, "language", nullptr, 0);
		out.push_back(Track{kv.first, track_label(kv.first), lang ? language_to_iso639_2(lang->value) : ""});
	}
	return out;
}

void Player::select_audio(int stream) { audio_switch_ = stream; }

std::string Player::embedded_subtitle(int stream, double t) {
	std::lock_guard<std::mutex> lock(sub_m_);
	auto it = embedded_.find(stream);
	if (it == embedded_.end()) return "";
	return cues_at(it->second, t);
}

std::string Player::codec_summary() const {
	if (!fmt_ || state_ != State::Playing) return "";
	std::vector<std::string> parts;
	int vs = video_stream_, as = audio_stream_;
	if (vs >= 0) {
		AVCodecParameters* p = fmt_->streams[vs]->codecpar;
		std::string c = avcodec_get_name(p->codec_id);
		for (auto& ch : c) ch = char(toupper((unsigned char)ch));
		parts.push_back(c + " " + std::to_string(p->width) + "x" + std::to_string(p->height) +
		                (hw_active_ ? " (hardware)" : " (software)"));
	}
	if (as >= 0) parts.push_back(track_label(as));
	return join(parts, " · ");
}

void Player::log_stats() {
	double now = now_seconds();
	if (state_ != State::Playing) {
		log_at_ = 0;
		return;
	}
	if (log_at_ == 0) {  // start counting
		log_at_ = now;
		log_shown_ = shown_;
		log_dropped_ = dropped_;
		return;
	}
	if (now - log_at_ < 30) return;
	double secs = now - log_at_;
	int shown = shown_ - log_shown_, dropped = dropped_ - log_dropped_;
	dlog("player: %s | shown %.1f fps (video %.3g fps), dropped %d in %.0f s, buffered %.1f s%s%s", codec_summary().c_str(),
	     shown / secs, fps_, dropped, secs, std::max(vq_.duration(), aq_.duration()), paused_ ? ", paused" : "",
	     buffering_ ? ", buffering" : "");
	log_at_ = now;
	log_shown_ = shown_;
	log_dropped_ = dropped_;
}

std::string Player::stats() {
	if (state_ != State::Playing) return "";
	char buf[256];
	snprintf(buf, sizeof(buf), "%s · %.3g fps · buffered %.1fs · dropped %d of %d", codec_summary().c_str(), fps_,
	         std::max(vq_.duration(), aq_.duration()), dropped_.load(), decoded_.load());
	return buf;
}
