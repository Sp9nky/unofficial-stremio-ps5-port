// Hardware video decoding, see hwdec_ps5.h. Ported from Nuvio PS5's
// evo_vdec_native.c (GPL-3.0-or-later, Copyright (C) 2026 Husam Osman); the
// libSceVideodec2 structures and call sequence come from there, which took
// them from blackbearreloaded/ProsperoLight and SvenGDK/SharpProspero.
#include "hwdec_ps5.h"

#include "util.h"

#ifndef PLATFORM_PS5_NATIVE

void HwDecoder::load_module() {}
HwDecoder* HwDecoder::open(const AVCodecParameters*, std::string* why) {
	if (why) *why = "no hardware decoder on this platform";
	return nullptr;
}
HwDecoder::~HwDecoder() {}
int HwDecoder::send(const uint8_t*, int, int64_t) { return -1; }
int HwDecoder::receive(Picture*) { return -1; }
void HwDecoder::flush() {}
const char* HwDecoder::name() const { return ""; }

#else

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
}

#include <algorithm>
#include <cstring>
#include <mutex>

// ---------------------------------------------------------------------------
// libSceVideodec2 (imports through native/stubs/videodec2.c)

extern "C" {
struct SceVideodec2DecoderConfigInfo {
	uint64_t size;
	uint32_t resource_type;
	uint32_t codec_type;
	uint32_t profile;
	uint32_t max_level;
	int32_t max_width;
	int32_t max_height;
	int32_t max_dpb_frames;
	uint32_t pipeline_depth;
	uint64_t compute_queue;
	uint64_t cpu_affinity;
	int32_t cpu_priority;
	uint32_t optimize_progressive;
	uint32_t check_memory_type;
	uint32_t reserved;
};
struct SceVideodec2DecoderMemoryInfo {
	uint64_t size;
	uint64_t cpu_size;
	void* cpu;
	uint64_t gpu_size;
	void* gpu;
	uint64_t cpu_gpu_size;
	void* cpu_gpu;
	uint64_t max_frame_size;
	uint32_t frame_alignment;
	uint32_t reserved;
};
struct SceVideodec2ComputeConfigInfo {
	uint64_t size;
	uint16_t pipe_id;
	uint16_t queue_id;
	uint8_t check_memory_type;
	uint8_t reserved0;
	uint16_t reserved1;
};
struct SceVideodec2ComputeMemoryInfo {
	uint64_t size;
	uint64_t cpu_gpu_size;
	void* cpu_gpu;
};
struct SceVideodec2InputData {
	uint64_t size;
	void* au;
	uint64_t au_size;
	uint64_t pts;
	uint64_t dts;
	uint64_t attached;
};
struct SceVideodec2FrameBuffer {
	uint64_t size;
	void* buffer;
	uint64_t buffer_size;
	uint32_t accepted;
	uint32_t reserved;
};
struct SceVideodec2OutputInfo {
	uint64_t size;
	uint8_t valid;
	uint8_t error;
	uint8_t picture_count;
	uint8_t padding;
	uint32_t codec;
	uint32_t width;
	uint32_t pitch;
	uint32_t height;
	uint32_t reserved;
	void* buffer;
	uint64_t buffer_size;
	uint32_t frame_format;
	uint32_t pitch_bytes;
};

int32_t sceVideodec2QueryComputeMemoryInfo(SceVideodec2ComputeMemoryInfo* memory);
int32_t sceVideodec2AllocateComputeQueue(const SceVideodec2ComputeConfigInfo* config,
                                         const SceVideodec2ComputeMemoryInfo* memory, void** compute_queue);
int32_t sceVideodec2ReleaseComputeQueue(void* compute_queue);
int32_t sceVideodec2QueryDecoderMemoryInfo(const SceVideodec2DecoderConfigInfo* config,
                                           SceVideodec2DecoderMemoryInfo* memory);
int32_t sceVideodec2CreateDecoder(const SceVideodec2DecoderConfigInfo* config,
                                  const SceVideodec2DecoderMemoryInfo* memory, void** decoder);
int32_t sceVideodec2DeleteDecoder(void* decoder);
int32_t sceVideodec2Reset(void* decoder);
int32_t sceVideodec2Decode(void* decoder, SceVideodec2InputData* input, SceVideodec2FrameBuffer* frame,
                           SceVideodec2OutputInfo* output);
int32_t sceVideodec2Flush(void* decoder, SceVideodec2FrameBuffer* frame, SceVideodec2OutputInfo* output);

int sceSysmoduleLoadModule(uint16_t id);
int64_t sceKernelGetDirectMemorySize(void);
int sceKernelAllocateDirectMemory(int64_t, int64_t, size_t, size_t, int, int64_t*);
int sceKernelMapDirectMemory(void**, size_t, int, int, int64_t, size_t);
int sceKernelReleaseDirectMemory(int64_t, size_t);
int sceKernelAvailableDirectMemorySize(int64_t, int64_t, size_t, int64_t*, size_t*);
int sceKernelMunmap(void*, size_t);
int sceKernelMapNamedFlexibleMemory(void**, size_t, int, int, const char*);
int sceKernelReleaseFlexibleMemory(void*, size_t);
int sceKernelAvailableFlexibleMemorySize(size_t*);
}

namespace {

const uint16_t kSysmoduleVideodec2 = 207;
const uint32_t kCodecAvc = 1, kCodecHevc = 974921, kCodecVp9 = 2382845;
const uint32_t kResourceCompute = 1;
const size_t kInputSlotBytes = 8u << 20;  // ~50x the largest 4K keyframe seen
const unsigned kInputSlots = 4;
const unsigned kFrameSlots = 8;
const int kReorderDepth = 4;
const int kReorderSlots = kReorderDepth + 1;
const int kPtsPool = kReorderDepth + int(kInputSlots) + 8;
const int kPostFlushErrors = 32;  // refusals tolerated before the first picture after a seek
const int kMaxW = 3840, kMaxH = 2176;

bool g_module_loaded = false;

enum Kind { H264, HEVC, VP9, HEVC10, KIND_COUNT };

struct CodecDesc {
	Kind kind;
	uint32_t codec_type, profile;
	int level_1080, level_4k;
	const char* bsf;
	bool vp9;
	const char* tag;
	unsigned depth;
};

// max_level: AVC level x10, HEVC general_level_idc (x30), VP9 x10.
const CodecDesc kCodecs[KIND_COUNT] = {
    {H264, kCodecAvc, 100, 51, 52, "h264_mp4toannexb", false, "H.264", 4},
    {HEVC, kCodecHevc, 1, 123, 153, "hevc_mp4toannexb", false, "HEVC", 4},
    {VP9, kCodecVp9, 0, 41, 51, "vp9_superframe_split", true, "VP9", 4},
    {HEVC10, kCodecHevc, 2, 123, 153, "hevc_mp4toannexb", false, "HEVC 10-bit", 4},
};

size_t align16k(size_t v) { return (v + 0x3fffu) & ~size_t(0x3fff); }

int alloc_direct(size_t size, int prot, int64_t* start, void** addr) {
	int rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), size, 0x4000, 12, start);
	if (rc == 0) rc = sceKernelMapDirectMemory(addr, size, prot, 0, *start, 0x4000);
	return rc;
}
void free_direct(void* addr, int64_t start, size_t size) {
	if (addr) sceKernelMunmap(addr, size);
	if (start >= 0) sceKernelReleaseDirectMemory(start, size);
}

// One decoder per codec, kept for the next video of the same kind (creating
// one maps a few hundred MB; doing that per video would fragment memory).
struct Slot {
	bool ready = false, owned = false;
	void* compute_queue = nullptr;
	void* compute_mem = nullptr;
	int64_t compute_start = -1;
	size_t compute_size = 0;
	void* decoder = nullptr;
	void* cpu_mem = nullptr;
	size_t cpu_map = 0;
	bool cpu_direct = false;  // workspace in direct memory (flexible memory was short)
	int64_t cpu_start = -1;
	void* gpu_mem = nullptr;
	int64_t gpu_start = -1;
	size_t gpu_size = 0;
	void* cpu_gpu_mem = nullptr;
	int64_t cpu_gpu_start = -1;
	size_t cpu_gpu_size = 0;
	void* input_mem = nullptr;
	int64_t input_start = -1;
	size_t input_pool = 0;
	void* frame_mem = nullptr;
	int64_t frame_start = -1;
	size_t frame_pool = 0, frame_size = 0;
	int max_w = 0, max_h = 0;
	SceVideodec2DecoderConfigInfo cfg{};
	SceVideodec2DecoderMemoryInfo mem{};
};

std::mutex g_slots_m;
Slot g_slots[KIND_COUNT];

void slot_teardown(Slot& s) {
	if (s.decoder) sceVideodec2DeleteDecoder(s.decoder);
	free_direct(s.frame_mem, s.frame_start, s.frame_pool);
	free_direct(s.input_mem, s.input_start, s.input_pool);
	free_direct(s.cpu_gpu_mem, s.cpu_gpu_start, s.cpu_gpu_size);
	free_direct(s.gpu_mem, s.gpu_start, s.gpu_size);
	if (s.cpu_mem) {
		if (s.cpu_direct) {
			free_direct(s.cpu_mem, s.cpu_start, s.cpu_map);
		} else {
			sceKernelReleaseFlexibleMemory(s.cpu_mem, s.cpu_map);
			sceKernelMunmap(s.cpu_mem, s.cpu_map);
		}
	}
	if (s.compute_queue) sceVideodec2ReleaseComputeQueue(s.compute_queue);
	free_direct(s.compute_mem, s.compute_start, s.compute_size);
	s = Slot();
}

// Sony calls can fault instead of failing: each step is logged before it
// runs, so the last line in the log names the one that did.
#define STAGE(name)                                                    \
	do {                                                               \
		stage = (name);                                                \
		dlog("hwdec: %s %dx%d [%s]", d.tag, w, h, stage);              \
	} while (0)

int slot_bringup(Slot& s, const CodecDesc& d, int w, int h) {
	const char* stage = "?";
	int rc;
	SceVideodec2ComputeMemoryInfo cm{};
	SceVideodec2ComputeConfigInfo cc{};
	cm.size = sizeof cm;
	STAGE("QueryComputeMemoryInfo");
	if ((rc = sceVideodec2QueryComputeMemoryInfo(&cm)) != 0) goto fail;
	s.compute_size = align16k(size_t(cm.cpu_gpu_size));
	STAGE("alloc(compute)");
	if ((rc = alloc_direct(s.compute_size, 0x33, &s.compute_start, &s.compute_mem)) != 0) goto fail;
	cm.cpu_gpu = s.compute_mem;
	cm.cpu_gpu_size = s.compute_size;
	cc.size = sizeof cc;
	STAGE("AllocateComputeQueue");
	if ((rc = sceVideodec2AllocateComputeQueue(&cc, &cm, &s.compute_queue)) != 0) goto fail;
	if (!s.compute_queue) {
		rc = -1;
		goto fail;
	}
	{
		SceVideodec2DecoderConfigInfo config{};
		SceVideodec2DecoderMemoryInfo mem{};
		config.size = sizeof config;
		config.resource_type = kResourceCompute;
		config.codec_type = d.codec_type;
		config.profile = d.profile;
		config.max_level = uint32_t((w > 1920 || h > 1088) ? d.level_4k : d.level_1080);
		config.max_width = w;
		config.max_height = h;
		config.max_dpb_frames = -1;  // the decoder sizes it
		config.pipeline_depth = d.depth;
		config.compute_queue = uint64_t(s.compute_queue);
		config.cpu_affinity = 0x3f;
		config.cpu_priority = 700;
		config.optimize_progressive = 1;
		mem.size = sizeof mem;
		STAGE("QueryDecoderMemoryInfo");
		if ((rc = sceVideodec2QueryDecoderMemoryInfo(&config, &mem)) != 0) goto fail;
		{
			int64_t st = 0;
			size_t avail = 0;
			sceKernelAvailableDirectMemorySize(0, sceKernelGetDirectMemorySize(), 0x4000, &st, &avail);
			dlog("hwdec: needs workspace %llu KB, gpu %llu KB, cpu_gpu %llu KB, frames %u x %llu KB, input %zu KB; "
			     "%zu MB direct memory free",
			     (unsigned long long)(mem.cpu_size >> 10), (unsigned long long)(mem.gpu_size >> 10),
			     (unsigned long long)(mem.cpu_gpu_size >> 10), kFrameSlots,
			     (unsigned long long)(mem.max_frame_size >> 10), (kInputSlotBytes * kInputSlots) >> 10, avail >> 20);
		}

		// The decoder's CPU workspace: flexible memory, as the decoder
		// expects. This app has little of it, so if that fails, try
		// direct memory mapped for the CPU.
		s.cpu_map = align16k(size_t(mem.cpu_size));
		size_t flex = 0;
		sceKernelAvailableFlexibleMemorySize(&flex);
		STAGE("MapNamedFlexibleMemory");
		rc = sceKernelMapNamedFlexibleMemory(&mem.cpu, s.cpu_map, 0x03, 0, "StremioVdec");
		if (rc != 0 || !mem.cpu) {
			dlog("hwdec: workspace of %zu KB not in flexible memory (rc 0x%08x, %zu KB free); using direct memory",
			     s.cpu_map >> 10, unsigned(rc), flex >> 10);
			mem.cpu = nullptr;
			STAGE("alloc(workspace)");
			if ((rc = alloc_direct(s.cpu_map, 0x33, &s.cpu_start, &mem.cpu)) != 0) goto fail;
			s.cpu_direct = true;
		}
		s.cpu_mem = mem.cpu;

		s.gpu_size = align16k(size_t(mem.gpu_size));
		s.cpu_gpu_size = align16k(size_t(mem.cpu_gpu_size));
		s.frame_size = align16k(size_t(mem.max_frame_size));
		s.input_pool = kInputSlotBytes * kInputSlots;
		s.frame_pool = s.frame_size * kFrameSlots;
		if (!s.frame_size) {
			rc = -1;
			goto fail;
		}
		mem.gpu_size = s.gpu_size;
		if (s.cpu_gpu_size) mem.cpu_gpu_size = s.cpu_gpu_size;
		STAGE("alloc(gpu)");
		if ((rc = alloc_direct(s.gpu_size, 0x33, &s.gpu_start, &s.gpu_mem)) != 0) goto fail;
		mem.gpu = s.gpu_mem;
		if (s.cpu_gpu_size) {
			STAGE("alloc(cpu_gpu)");
			if ((rc = alloc_direct(s.cpu_gpu_size, 0x33, &s.cpu_gpu_start, &s.cpu_gpu_mem)) != 0) goto fail;
			mem.cpu_gpu = s.cpu_gpu_mem;
		}
		STAGE("alloc(input)");
		if ((rc = alloc_direct(s.input_pool, 0x33, &s.input_start, &s.input_mem)) != 0) goto fail;
		STAGE("alloc(frames)");
		if ((rc = alloc_direct(s.frame_pool, 0x33, &s.frame_start, &s.frame_mem)) != 0) goto fail;
		STAGE("CreateDecoder");
		if ((rc = sceVideodec2CreateDecoder(&config, &mem, &s.decoder)) != 0 || !s.decoder) {
			if (!rc) rc = -1;
			goto fail;
		}
		s.cfg = config;
		s.mem = mem;
		STAGE("Reset");
		if ((rc = sceVideodec2Reset(s.decoder)) != 0) goto fail;
		s.max_w = w;
		s.max_h = h;
		s.ready = true;
		dlog("hwdec: %s decoder ready for %dx%d (%zu MB)", d.tag, w, h,
		     (s.compute_size + s.gpu_size + s.cpu_gpu_size + s.input_pool + s.frame_pool + s.cpu_map) >> 20);
		return 0;
	}
fail:
	dlog("hwdec: %s %dx%d failed at [%s]: rc 0x%08x", d.tag, w, h, stage, unsigned(rc));
	slot_teardown(s);
	return rc ? rc : -1;
}
#undef STAGE

// A new decoder object on the slot's memory: what a seek needs. Reset alone
// left some 4K streams unable to restart (Nuvio PS5, hardware 2026-10-01).
int slot_renew(Slot& s) {
	if (s.decoder) sceVideodec2DeleteDecoder(s.decoder);
	s.decoder = nullptr;
	SceVideodec2DecoderMemoryInfo mem = s.mem;
	int rc = sceVideodec2CreateDecoder(&s.cfg, &mem, &s.decoder);
	if (rc != 0 || !s.decoder) {
		s.decoder = nullptr;
		return rc ? rc : -1;
	}
	return sceVideodec2Reset(s.decoder);
}

// ---------------------------------------------------------------------------
// HEVC access-unit handling

const int kNalTrailHi = 5, kNalRaslN = 8, kNalRaslR = 9, kNalIrapHi = 23;

// First VCL NAL type in an Annex-B access unit, or -1.
int hevc_au_nal_type(const uint8_t* au, int size) {
	for (int i = 0; i + 4 < size; i++) {
		if (au[i] != 0 || au[i + 1] != 0) continue;
		int payload;
		if (au[i + 2] == 1) payload = i + 3;
		else if (au[i + 2] == 0 && au[i + 3] == 1) payload = i + 4;
		else continue;
		if (payload >= size) break;
		int type = (au[payload] >> 1) & 0x3f;
		if (type <= kNalIrapHi) return type;
		i = payload;
	}
	return -1;
}

struct Bits {
	const uint8_t* p;
	int n, byte, bit, zeros;
	int get() {
		if (byte >= n) return 0;
		if (bit == 0 && zeros >= 2 && p[byte] == 3) {  // emulation prevention
			byte++;
			zeros = 0;
			if (byte >= n) return 0;
		}
		int v = (p[byte] >> (7 - bit)) & 1;
		if (++bit == 8) {
			zeros = p[byte] == 0 ? zeros + 1 : 0;
			bit = 0;
			byte++;
		}
		return v;
	}
	unsigned u(int k) {
		unsigned v = 0;
		while (k-- > 0) v = (v << 1) | unsigned(get());
		return v;
	}
	unsigned ue() {
		int lz = 0;
		while (lz < 31 && !get()) lz++;
		return ((1u << lz) - 1) + u(lz);
	}
};

// The ID a VPS (32), SPS (33) or PPS (34) defines.
unsigned hevc_ps_id(int type, const uint8_t* payload, int len) {
	Bits b{payload, len, 0, 0, 0};
	if (type == 32) return b.u(4);
	if (type == 34) return b.ue();
	b.u(4);
	unsigned subs = b.u(3);
	b.u(1);
	b.u(32), b.u(32), b.u(32);
	unsigned prof[8] = {0}, lvl[8] = {0};
	for (unsigned i = 0; i < subs && i < 8; i++) prof[i] = b.u(1), lvl[i] = b.u(1);
	if (subs > 0)
		for (unsigned i = subs; i < 8; i++) b.u(2);
	for (unsigned i = 0; i < subs && i < 8; i++) {
		if (prof[i]) b.u(32), b.u(32), b.u(24);
		if (lvl[i]) b.u(8);
	}
	return b.ue();
}

// Copies an HEVC access unit without what the decoder can't take: NAL types
// 48-63 (Dolby Vision RPU / enhancement layer; the base layer plays) and a
// parameter set that a later one in the same AU replaces (UHD Blu-ray remuxes
// repeat theirs in-band; two different PPS 0 in one AU make the decoder
// refuse the keyframe).
int hevc_copy_base_layer(uint8_t* dst, const uint8_t* au, int size) {
	const int kMaxNals = 512;
	static thread_local int sc_at[kMaxNals], nal_at[kMaxNals], end_at[kMaxNals];
	static thread_local uint32_t key[kMaxNals];
	int count = 0, sc = -1, nal = -1;
	for (int i = 0; i + 3 <= size; i++)
		if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1) {
			sc = (i > 0 && au[i - 1] == 0) ? i - 1 : i;
			nal = i + 3;
			break;
		}
	if (nal < 0) {
		memcpy(dst, au, size_t(size));
		return size;
	}
	while (nal < size && count < kMaxNals) {
		int next_sc = size, next_nal = -1;
		for (int j = nal; j + 3 <= size; j++)
			if (au[j] == 0 && au[j + 1] == 0 && au[j + 2] == 1) {
				next_sc = (j > nal && au[j - 1] == 0) ? j - 1 : j;
				next_nal = j + 3;
				break;
			}
		int type = (au[nal] >> 1) & 0x3f;
		sc_at[count] = sc;
		nal_at[count] = nal;
		end_at[count] = next_sc;
		key[count] = 0;
		if (type >= 32 && type <= 34 && nal + 2 < next_sc) {
			unsigned layer = ((au[nal] & 1u) << 5) | (au[nal + 1] >> 3);
			unsigned id = hevc_ps_id(type, au + nal + 2, next_sc - nal - 2);
			key[count] = 0x80000000u | (uint32_t(type) << 24) | (layer << 16) | (id & 0xffffu);
		}
		count++;
		if (next_nal < 0) break;
		sc = next_sc;
		nal = next_nal;
	}
	if (count == kMaxNals && nal < size) {
		memcpy(dst, au, size_t(size));
		return size;
	}
	int o = 0;
	for (int k = 0; k < count; k++) {
		int type = (au[nal_at[k]] >> 1) & 0x3f;
		if (type >= 48) continue;
		if (key[k]) {
			bool replaced = false;
			for (int m = k + 1; m < count && !replaced; m++) replaced = key[m] == key[k];
			if (replaced) continue;
		}
		memcpy(dst + o, au + sc_at[k], size_t(end_at[k] - sc_at[k]));
		o += end_at[k] - sc_at[k];
	}
	return o;
}

// tiles_enabled_flag of an HEVC PPS: the hardware decoder doesn't do tiles.
bool hevc_pps_tiles(const uint8_t* payload, int len) {
	Bits b{payload, len, 0, 0, 0};
	b.ue(), b.ue();
	b.u(1), b.u(1);
	b.u(3);
	b.u(1), b.u(1);
	b.ue(), b.ue();
	b.ue();
	b.u(1), b.u(1);
	if (b.u(1)) b.ue();
	b.ue(), b.ue();
	b.u(1), b.u(1), b.u(1);
	b.u(1);
	return b.u(1) != 0;
}

bool hevc_extradata_tiles(const uint8_t* x, int n) {
	if (!x || n < 4) return false;
	if (x[0] == 0 && x[1] == 0 && (x[2] == 1 || (x[2] == 0 && x[3] == 1))) {
		for (int i = 0; i + 4 < n; i++) {
			if (x[i] || x[i + 1] || x[i + 2] != 1) continue;
			int nal = i + 3;
			if (((x[nal] >> 1) & 0x3f) == 34 && hevc_pps_tiles(x + nal + 2, n - nal - 2)) return true;
		}
		return false;
	}
	if (n < 23) return false;
	int pos = 23, arrays = x[22];
	for (int a = 0; a < arrays && pos + 3 <= n; a++) {
		int type = x[pos] & 0x3f;
		int count = (x[pos + 1] << 8) | x[pos + 2];
		pos += 3;
		for (int k = 0; k < count && pos + 2 <= n; k++) {
			int len = (x[pos] << 8) | x[pos + 1];
			pos += 2;
			if (pos + len > n) return false;
			if (type == 34 && len > 2 && hevc_pps_tiles(x + pos + 2, len - 2)) return true;
			pos += len;
		}
	}
	return false;
}

// VP9: is this coded frame shown? Hidden alt-ref frames are decoded for
// reference but never presented.
bool vp9_frame_is_shown(const uint8_t* b, int size) {
	int p = 0;
	auto bit = [&]() { int i = p++; return (i >> 3) < size ? (b[i >> 3] >> (7 - (i & 7))) & 1 : 0; };
	if (bit() != 1 || bit() != 0) return true;
	int prof = bit();
	prof |= bit() << 1;
	if (prof == 3) bit();
	if (bit()) return true;  // show_existing_frame
	bit();                   // frame_type
	return bit() != 0;       // show_frame
}

}  // namespace

// ---------------------------------------------------------------------------

struct HwDecoder::Impl {
	const CodecDesc* desc = nullptr;
	Slot* slot = nullptr;
	int disp_w = 0, disp_h = 0;
	unsigned au_ring = 0, calls = 0, fails = 0, frames = 0;
	bool flushing = false, fatal = false;
	bool drop_leading = true, need_irap = false;
	int post_flush_errs = 0, since_flush_out = 0;
	bool logged_err = false, logged_strip = false;

	AVBSFContext* bsf = nullptr;
	AVCodecParameters* bsf_par = nullptr;
	std::string extradata;  // Annex-B parameter sets, sent first when there's no bsf
	AVPacket* in_pkt = nullptr;
	AVPacket* out_pkt = nullptr;

	int64_t pts_pool[kPtsPool];
	int pts_n = 0;

	struct Out {
		bool used = false;
		Picture pic;
	} ro[kReorderSlots];
	int ro_count = 0;

	void pts_push(int64_t pts) {
		if (pts_n >= kPtsPool) {
			memmove(pts_pool, pts_pool + 1, (kPtsPool - 1) * sizeof(int64_t));
			pts_n = kPtsPool - 1;
		}
		pts_pool[pts_n++] = pts;
	}
	// Pictures come out in display order without a timestamp: pair each
	// with the smallest pending one.
	int64_t pts_take() {
		if (pts_n <= 0) return INT64_MIN;
		int mi = 0;
		for (int i = 1; i < pts_n; i++) {
			if (pts_pool[i] == INT64_MIN) continue;
			if (pts_pool[mi] == INT64_MIN || pts_pool[i] < pts_pool[mi]) mi = i;
		}
		int64_t v = pts_pool[mi];
		pts_pool[mi] = pts_pool[--pts_n];
		return v;
	}

	// Where the chroma plane starts, in luma rows: right after the height the
	// decoder reports for the picture, as Nuvio PS5 does (ro_harvest: planes[1]
	// = data + pitch x out->height, used as is by pp_playback.c).
	uint32_t chroma_rows = 0;
	uint32_t find_chroma(const SceVideodec2OutputInfo& out, uint32_t pitch, bool ten) {
		(void)ten;
		dlog("hwdec: picture %ux%u pitch %u, buffer %llu bytes; chroma at row %u", out.width, out.height, pitch,
		     (unsigned long long)out.buffer_size, out.height);
		return out.height;
	}
	void harvest(const SceVideodec2OutputInfo& out) {
		Out* s = nullptr;
		for (auto& r : ro)
			if (!r.used) {
				s = &r;
				break;
			}
		if (!s || !out.buffer) return;
		bool ten = out.pitch_bytes ? out.pitch_bytes >= out.pitch * 2u : desc->kind == HEVC10;
		uint32_t pitch = out.pitch_bytes ? out.pitch_bytes : out.pitch;
		if (ten && pitch < out.pitch * 2u) pitch = out.pitch * 2u;
		// The frame stays in the decoder's pool until it reuses the slot,
		// several pictures later: hand out pointers into it, no copy.
		s->pic.y = static_cast<const uint8_t*>(out.buffer);
		if (chroma_rows == 0) chroma_rows = find_chroma(out, pitch, ten);
		s->pic.uv = s->pic.y + size_t(pitch) * chroma_rows;  // after the padded luma rows
		s->pic.pitch = int(pitch);
		s->pic.width = int(disp_w && uint32_t(disp_w) < out.width ? uint32_t(disp_w) : out.width);
		s->pic.height = int(disp_h && uint32_t(disp_h) < out.height ? uint32_t(disp_h) : out.height);
		s->pic.ten_bit = ten;
		s->pic.pts_us = pts_take();
		s->used = true;
		ro_count++;
	}

	// One access unit to the decoder. present=false: decode for reference only.
	int decode_one(const uint8_t* au, int size, int64_t pts, bool present) {
		if (size <= 0 || size_t(size) > kInputSlotBytes) return -1;
		// Leading pictures after a random access point (open GOP) reference
		// what was never decoded: drop them before the decoder sees them.
		if (desc->codec_type == kCodecHevc && drop_leading) {
			int type = hevc_au_nal_type(au, size);
			if (type == kNalRaslN || type == kNalRaslR) return 0;
			if (type >= 0 && type <= kNalTrailHi) drop_leading = false;
		}
		unsigned islot = au_ring % kInputSlots, fslot = au_ring % kFrameSlots;
		au_ring++;
		uint8_t* in_mem = static_cast<uint8_t*>(slot->input_mem) + size_t(islot) * kInputSlotBytes;
		bool irap = false;
		if (desc->codec_type == kCodecHevc) {
			int n = hevc_copy_base_layer(in_mem, au, size);
			if (n != size && !logged_strip) {
				logged_strip = true;
				dlog("hwdec: left out Dolby Vision layers / repeated parameter sets");
			}
			size = n;
			if (size <= 0) return 0;
			int vcl = hevc_au_nal_type(in_mem, size);
			irap = vcl >= 16 && vcl <= kNalIrapHi;
			if (need_irap && vcl >= 0 && !irap) return 0;  // depends on a refused keyframe
		} else {
			memcpy(in_mem, au, size_t(size));
		}

		SceVideodec2InputData in{};
		SceVideodec2FrameBuffer fb{};
		SceVideodec2OutputInfo out{};
		in.size = sizeof in;
		in.au = in_mem;
		in.au_size = uint64_t(size);
		in.pts = uint64_t(pts);
		in.dts = UINT64_MAX;
		fb.size = sizeof fb;
		fb.buffer = static_cast<uint8_t*>(slot->frame_mem) + size_t(fslot) * slot->frame_size;
		fb.buffer_size = slot->frame_size;
		out.size = sizeof out;
		int rc = sceVideodec2Decode(slot->decoder, &in, &fb, &out);
		calls++;
		if (calls <= 2 || (out.valid && frames == 0))
			dlog("hwdec: decode #%u rc 0x%08x valid %u %ux%u pitch %u", calls, unsigned(rc), out.valid, out.width,
			     out.height, out.pitch_bytes ? out.pitch_bytes : out.pitch);
		if (irap) need_irap = rc != 0;
		if (rc != 0) {
			fails++;
			if (!logged_err) {
				logged_err = true;
				dlog("hwdec: decode #%u failed rc 0x%08x (%d bytes)", calls, unsigned(rc), size);
			}
			// Before the first picture after a seek, errors mean we started
			// mid-stream, not that the stream can't be decoded.
			if (since_flush_out == 0 && ++post_flush_errs <= kPostFlushErrors) return 0;
			return -1;
		}
		if (present) pts_push(pts);
		if (out.valid && out.picture_count) {
			since_flush_out++;
			if (present) {
				frames++;
				harvest(out);
			}
		}
		return 0;
	}

	void drain() {
		for (int guard = 0; guard < 64; guard++) {
			unsigned fslot = au_ring % kFrameSlots;
			au_ring++;
			SceVideodec2FrameBuffer fb{};
			SceVideodec2OutputInfo out{};
			fb.size = sizeof fb;
			fb.buffer = static_cast<uint8_t*>(slot->frame_mem) + size_t(fslot) * slot->frame_size;
			fb.buffer_size = slot->frame_size;
			out.size = sizeof out;
			if (sceVideodec2Flush(slot->decoder, &fb, &out) != 0) break;
			if (!(out.valid && out.picture_count)) break;
			harvest(out);
			if (ro_count >= kReorderSlots - 1) break;
		}
	}

	// (Re)made at open and on every seek: a fresh h264_mp4toannexb puts the
	// parameter sets back in front of the first keyframe; a flushed one doesn't.
	bool bsf_build() {
		const AVBitStreamFilter* bf = av_bsf_get_by_name(desc->bsf);
		if (!bf) return false;
		if (bsf) av_bsf_free(&bsf);
		if (av_bsf_alloc(bf, &bsf) < 0) return bsf = nullptr, false;
		if (avcodec_parameters_copy(bsf->par_in, bsf_par) < 0) return av_bsf_free(&bsf), false;
		bsf->time_base_in = AVRational{1, 1000000};
		if (av_bsf_init(bsf) < 0) return av_bsf_free(&bsf), false;
		return true;
	}

	bool present_of(const AVPacket* p) const { return !desc->vp9 || vp9_frame_is_shown(p->data, p->size); }

	int feed_filtered() {
		while (av_bsf_receive_packet(bsf, out_pkt) == 0) {
			int64_t fp = out_pkt->pts == AV_NOPTS_VALUE ? INT64_MIN : out_pkt->pts;
			int r = decode_one(out_pkt->data, out_pkt->size, fp, present_of(out_pkt));
			av_packet_unref(out_pkt);
			if (r < 0) return -1;
		}
		return 0;
	}
};

void HwDecoder::load_module() {
	int r = sceSysmoduleLoadModule(kSysmoduleVideodec2);
	g_module_loaded = r == 0;
	dlog("hwdec: video decoder module %s (0x%x)", g_module_loaded ? "loaded" : "NOT loaded", unsigned(r));
}

HwDecoder* HwDecoder::open(const AVCodecParameters* par, std::string* why) {
	auto refuse = [&](const std::string& w) {
		if (why) *why = w;
		return nullptr;
	};
	if (!g_module_loaded) return refuse("no decoder module");
	int depth = par->bits_per_raw_sample > 8 ? par->bits_per_raw_sample : 8;
	if (par->format == AV_PIX_FMT_YUV420P10LE || par->format == AV_PIX_FMT_YUV420P10BE ||
	    (par->codec_id == AV_CODEC_ID_HEVC && par->profile == AV_PROFILE_HEVC_MAIN_10) ||
	    (par->codec_id == AV_CODEC_ID_VP9 && par->profile == AV_PROFILE_VP9_2))
		depth = 10;
	if (par->format >= 0 && par->format != AV_PIX_FMT_YUV420P && par->format != AV_PIX_FMT_YUVJ420P &&
	    par->format != AV_PIX_FMT_YUV420P10LE && par->format != AV_PIX_FMT_YUV420P10BE &&
	    par->format != AV_PIX_FMT_NV12)
		return refuse("pixel format isn't 4:2:0");

	const CodecDesc* d = nullptr;
	switch (par->codec_id) {
	case AV_CODEC_ID_H264:
		if (depth > 8) return refuse("10-bit H.264");
		// Baseline, Main, High (and constrained); not High 10/4:2:2/4:4:4.
		if (par->profile != AV_PROFILE_UNKNOWN && par->profile > 100 && par->profile != 578)
			return refuse("H.264 profile " + std::to_string(par->profile));
		d = &kCodecs[H264];
		break;
	case AV_CODEC_ID_HEVC:
		if (depth > 10) return refuse("12-bit HEVC");
		if (par->profile != AV_PROFILE_UNKNOWN && par->profile != AV_PROFILE_HEVC_MAIN &&
		    par->profile != AV_PROFILE_HEVC_MAIN_10)
			return refuse("HEVC profile " + std::to_string(par->profile));
		if (hevc_extradata_tiles(par->extradata, par->extradata_size)) return refuse("HEVC coded in tiles");
		d = &kCodecs[depth == 10 ? HEVC10 : HEVC];
		break;
	case AV_CODEC_ID_VP9:
		if (depth > 8) return refuse("10-bit VP9");
		d = &kCodecs[VP9];
		break;
	default:
		return refuse(std::string(avcodec_get_name(par->codec_id)) + " has no hardware decoder");
	}
	int w = par->width, h = par->height;
	if (w < 16 || h < 16) return refuse("unknown picture size");
	// Size for the coded picture: encoders pad to their block size (up to 64).
	w = std::min((w + 63) & ~63, kMaxW);
	h = std::min((h + 63) & ~63, kMaxH);
	if (par->width > kMaxW || par->height > kMaxH) return refuse("bigger than 4K");
	w = std::max(w, 1920);
	h = std::max(h, 1088);

	std::lock_guard<std::mutex> lock(g_slots_m);
	Slot& slot = g_slots[d->kind];
	if (slot.owned) return refuse("decoder busy");
	if (slot.ready && (slot.max_w < w || slot.max_h < h)) slot_teardown(slot);
	if (!slot.ready && slot_bringup(slot, *d, w, h) != 0) return refuse("decoder set-up failed");

	auto* impl = new Impl();
	impl->desc = d;
	impl->slot = &slot;
	impl->disp_w = par->width;
	impl->disp_h = par->height;
	bool need_bsf = d->vp9 || (par->extradata && par->extradata_size >= 4 && par->extradata[0] == 1);
	if (need_bsf) {
		impl->bsf_par = avcodec_parameters_alloc();
		if (!impl->bsf_par || avcodec_parameters_copy(impl->bsf_par, par) < 0 || !impl->bsf_build()) {
			if (impl->bsf_par) avcodec_parameters_free(&impl->bsf_par);
			delete impl;
			return refuse(std::string("bitstream filter ") + d->bsf + " failed");
		}
	} else if (par->extradata && par->extradata_size > 0) {
		impl->extradata.assign(reinterpret_cast<const char*>(par->extradata), size_t(par->extradata_size));
	}
	impl->in_pkt = av_packet_alloc();
	impl->out_pkt = av_packet_alloc();
	slot.owned = true;
	sceVideodec2Reset(slot.decoder);
	if (!impl->extradata.empty())
		impl->decode_one(reinterpret_cast<const uint8_t*>(impl->extradata.data()), int(impl->extradata.size()),
		                 INT64_MIN, false);
	impl->drop_leading = true;

	auto* hd = new HwDecoder();
	hd->d_ = impl;
	dlog("hwdec: playing %dx%d %s on the hardware decoder", par->width, par->height, d->tag);
	return hd;
}

HwDecoder::~HwDecoder() {
	if (!d_) return;
	dlog("hwdec: closed after %u decodes, %u pictures, %u errors", d_->calls, d_->frames, d_->fails);
	{
		std::lock_guard<std::mutex> lock(g_slots_m);
		if (d_->slot->decoder) sceVideodec2Reset(d_->slot->decoder);
		d_->slot->owned = false;
	}
	if (d_->bsf) av_bsf_free(&d_->bsf);
	if (d_->bsf_par) avcodec_parameters_free(&d_->bsf_par);
	av_packet_free(&d_->in_pkt);
	av_packet_free(&d_->out_pkt);
	delete d_;
}

int HwDecoder::send(const uint8_t* data, int size, int64_t pts_us) {
	Impl* n = d_;
	if (n->fatal || !n->slot->decoder) return -1;
	if (!data || size <= 0) {  // end of stream: everything out
		if (n->bsf) {
			av_bsf_send_packet(n->bsf, nullptr);
			n->feed_filtered();
		}
		n->drain();
		n->flushing = true;
		return 0;
	}
	if (n->ro_count > kReorderDepth) return 1;
	int r;
	if (n->bsf) {
		av_packet_unref(n->in_pkt);
		if (av_new_packet(n->in_pkt, size) < 0) return -1;
		memcpy(n->in_pkt->data, data, size_t(size));
		n->in_pkt->pts = pts_us == INT64_MIN ? AV_NOPTS_VALUE : pts_us;
		n->in_pkt->dts = AV_NOPTS_VALUE;
		if (av_bsf_send_packet(n->bsf, n->in_pkt) < 0) return -1;
		r = n->feed_filtered();
	} else {
		r = n->decode_one(data, size, pts_us, true);
	}
	if (r < 0) n->fatal = true;
	return r < 0 ? -1 : 0;
}

int HwDecoder::receive(Picture* out) {
	Impl* n = d_;
	if (n->fatal) return -1;
	if (n->ro_count <= (n->flushing ? 0 : kReorderDepth)) return 0;
	Impl::Out* best = nullptr;
	for (auto& r : n->ro) {
		if (!r.used) continue;
		if (!best || best->pic.pts_us == INT64_MIN || (r.pic.pts_us != INT64_MIN && r.pic.pts_us < best->pic.pts_us))
			best = &r;
	}
	if (!best) return 0;
	*out = best->pic;
	best->used = false;
	n->ro_count--;
	return 1;
}

void HwDecoder::flush() {
	Impl* n = d_;
	for (auto& r : n->ro) r.used = false;
	n->ro_count = 0;
	n->pts_n = 0;
	n->au_ring = 0;
	n->flushing = false;
	n->fatal = false;
	n->drop_leading = true;
	n->need_irap = false;
	n->post_flush_errs = 0;
	n->since_flush_out = 0;
	n->logged_err = false;
	if (n->bsf_par && !n->bsf_build()) {
		dlog("hwdec: bitstream filter rebuild failed after a seek");
		n->fatal = true;
		return;
	}
	{
		std::lock_guard<std::mutex> lock(g_slots_m);
		int rc = slot_renew(*n->slot);
		if (rc != 0) {
			dlog("hwdec: new decoder after a seek failed: rc 0x%08x", unsigned(rc));
			n->fatal = true;
			n->slot->ready = false;  // the next open builds it again
			return;
		}
	}
	if (!n->extradata.empty())
		n->decode_one(reinterpret_cast<const uint8_t*>(n->extradata.data()), int(n->extradata.size()), INT64_MIN,
		              false);
}

const char* HwDecoder::name() const { return d_->desc->tag; }

#endif  // PLATFORM_PS5_NATIVE
