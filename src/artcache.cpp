#include "artcache.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "http.h"
#include "tasks.h"
#include "util.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#include "stb_image.h"
#include <webp/decode.h>

ArtCache g_art;

// PNG/JPEG via stb_image, WebP (what metahub serves posters as) via libwebp.
static bool decode_image(const std::string& data, std::vector<unsigned char>& px, int& w, int& h, const char** why) {
	const unsigned char* bytes = (const unsigned char*)data.data();
	if (data.size() >= 12 && memcmp(bytes, "RIFF", 4) == 0 && memcmp(bytes + 8, "WEBP", 4) == 0) {
		uint8_t* out = WebPDecodeRGBA(bytes, data.size(), &w, &h);
		if (!out) {
			if (why) *why = "bad WebP";
			return false;
		}
		px.assign(out, out + size_t(w) * h * 4);
		WebPFree(out);
		return true;
	}
	int comp;
	unsigned char* out = stbi_load_from_memory(bytes, int(data.size()), &w, &h, &comp, 4);
	if (!out) {
		if (why) *why = stbi_failure_reason();
		return false;
	}
	px.assign(out, out + size_t(w) * h * 4);
	stbi_image_free(out);
	return true;
}

static const char* kind_suffix(ArtKind k) {
	switch (k) {
	case ArtKind::Poster: return "p2";  // "2": sized for kUiScale (older files were 182x277)
	case ArtKind::PosterLarge: return "pl2";
	case ArtKind::Background: return "b";
	case ArtKind::Backdrop: return "bd";
	case ArtKind::Still: return "s";
	case ArtKind::LogoBox: return "lb";
	case ArtKind::Logo: return "l";
	case ArtKind::Thumb: return "t2";
	case ArtKind::Qr: return "q";
	case ArtKind::Icon: return "i2";
	}
	return "x";
}

void ArtCache::start(const std::string& dir, int workers) {
	dir_ = dir;
	make_dirs(dir_);
	for (int i = 0; i < workers; i++) threads_.emplace_back([this] { worker(); });
}

void ArtCache::stop() {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stopping_ = true;
		queue_.clear();
	}
	cv_.notify_all();
	for (auto& t : threads_) t.join();
	threads_.clear();
}

std::string ArtCache::path_for(const std::string& url, ArtKind kind) const {
	char name[64];
	snprintf(name, sizeof(name), "/%016llx%s.rgba", (unsigned long long)fnv1a(url), kind_suffix(kind));
	return dir_ + name;
}

std::string ArtCache::get(const std::string& url, ArtKind kind, std::function<void(const std::string&)> ready) {
	if (url.empty() || (!starts_with(url, "http://") && !starts_with(url, "https://"))) return "";
	std::string path = path_for(url, kind);
	std::lock_guard<std::mutex> lock(mutex_);
	auto k = known_.find(path);
	if (k != known_.end()) return k->second ? path : "";
	if (file_exists(path)) {
		known_[path] = true;
		return path;
	}
	auto& waiters = waiting_[path];
	bool queued = !waiters.empty();
	if (ready) waiters.push_back(ready);
	else waiters.push_back([](const std::string&) {});
	if (queued) {
		// Move it to the front: it's wanted again now.
		for (size_t i = 0; i < queue_.size(); i++) {
			if (queue_[i].path == path) {
				Job j = queue_[i];
				queue_.erase(queue_.begin() + i);
				queue_.push_back(j);
				break;
			}
		}
		return "";
	}
	queue_.push_back(Job{url, path, kind});
	cv_.notify_one();
	return "";
}

std::string ArtCache::peek(const std::string& url, ArtKind kind) {
	if (url.empty()) return "";
	std::string path = path_for(url, kind);
	std::lock_guard<std::mutex> lock(mutex_);
	auto k = known_.find(path);
	if (k != known_.end()) return k->second ? path : "";
	if (file_exists(path)) {
		known_[path] = true;
		return path;
	}
	return "";
}

void ArtCache::clear_queue() {
	std::lock_guard<std::mutex> lock(mutex_);
	for (auto& j : queue_) waiting_.erase(j.path);
	queue_.clear();
}

void ArtCache::worker() {
	while (true) {
		Job job;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
			if (stopping_) return;
			job = queue_.back();
			queue_.pop_back();
		}
		bool ok = fetch(job);
		std::vector<std::function<void(const std::string&)>> waiters;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			known_[job.path] = ok;
			auto it = waiting_.find(job.path);
			if (it != waiting_.end()) {
				waiters.swap(it->second);
				waiting_.erase(it);
			}
		}
		std::string path = ok ? job.path : "";
		if (!waiters.empty())
			g_tasks.post([waiters, path]() {
				for (auto& w : waiters) w(path);
			});
	}
}

// ---------------------------------------------------------------------------
// Scaling

// Area-averaging resample of the source rectangle (sx, sy, sw, sh) into dw x dh.
static void resample(const unsigned char* src, int src_w, float sx, float sy, float sw, float sh,
                     unsigned char* dst, int dw, int dh, bool nearest) {
	float fx = sw / dw, fy = sh / dh;
	for (int y = 0; y < dh; y++) {
		for (int x = 0; x < dw; x++) {
			unsigned char* d = dst + (size_t(y) * dw + x) * 4;
			if (nearest || (fx <= 1.0f && fy <= 1.0f)) {
				// Upscaling (or QR codes): bilinear would blur, nearest is fine at these sizes.
				int ix = int(sx + (x + 0.5f) * fx), iy = int(sy + (y + 0.5f) * fy);
				const unsigned char* s = src + (size_t(iy) * src_w + ix) * 4;
				memcpy(d, s, 4);
				continue;
			}
			int x0 = int(sx + x * fx), x1 = std::max(x0 + 1, int(sx + (x + 1) * fx));
			int y0 = int(sy + y * fy), y1 = std::max(y0 + 1, int(sy + (y + 1) * fy));
			uint32_t acc[4] = {0, 0, 0, 0}, n = 0;
			for (int yy = y0; yy < y1; yy++) {
				const unsigned char* s = src + (size_t(yy) * src_w + x0) * 4;
				for (int xx = x0; xx < x1; xx++, s += 4) {
					// Weight colour by alpha so transparent pixels don't darken edges (logos).
					uint32_t a = s[3];
					acc[0] += s[0] * a;
					acc[1] += s[1] * a;
					acc[2] += s[2] * a;
					acc[3] += a;
					n++;
				}
			}
			if (acc[3]) {
				d[0] = (unsigned char)(acc[0] / acc[3]);
				d[1] = (unsigned char)(acc[1] / acc[3]);
				d[2] = (unsigned char)(acc[2] / acc[3]);
			} else {
				d[0] = d[1] = d[2] = 0;
			}
			d[3] = (unsigned char)(acc[3] / n);
		}
	}
}

static void round_corners(unsigned char* px, int w, int h, float r) {
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			float cx = -1, cy = -1;
			if (x < r && y < r) cx = r, cy = r;
			else if (x >= w - r && y < r) cx = w - r, cy = r;
			else if (x < r && y >= h - r) cx = r, cy = h - r;
			else if (x >= w - r && y >= h - r) cx = w - r, cy = h - r;
			if (cx < 0) continue;
			float dx = (x + 0.5f) - cx, dy = (y + 0.5f) - cy;
			float dist = std::sqrt(dx * dx + dy * dy);
			float cover = r - dist + 0.5f;  // 1px anti-aliased edge
			if (cover >= 1) continue;
			unsigned char* a = px + (size_t(y) * w + x) * 4 + 3;
			*a = cover <= 0 ? 0 : (unsigned char)(*a * cover);
		}
	}
}

// Soft blur: three box passes each way (close to a gaussian), alpha ignored.
static void blur(unsigned char* px, int w, int h, int r) {
	std::vector<unsigned char> line(size_t(std::max(w, h)) * 4);
	auto pass = [&](unsigned char* base, int n, size_t step) {
		for (int i = 0; i < n; i++) memcpy(&line[size_t(i) * 4], base + i * step, 4);
		int win = 2 * r + 1;
		for (int c = 0; c < 3; c++) {
			int sum = 0;
			for (int k = -r; k <= r; k++) sum += line[size_t(std::min(std::max(k, 0), n - 1)) * 4 + c];
			for (int i = 0; i < n; i++) {
				base[i * step + c] = (unsigned char)(sum / win);
				int out = std::max(i - r, 0), in = std::min(i + r + 1, n - 1);
				sum += line[size_t(in) * 4 + c] - line[size_t(out) * 4 + c];
			}
		}
	};
	for (int it = 0; it < 3; it++) {
		for (int y = 0; y < h; y++) pass(px + size_t(y) * w * 4, w, 4);
		for (int x = 0; x < w; x++) pass(px + size_t(x) * 4, h, size_t(w) * 4);
	}
}

static bool write_rgba(const std::string& path, const std::vector<unsigned char>& px, int w, int h) {
	std::string data;
	data.reserve(12 + px.size());
	data.append("RGBA", 4);
	uint32_t dims[2] = {uint32_t(w), uint32_t(h)};
	data.append((const char*)dims, 8);
	data.append((const char*)px.data(), px.size());
	return write_file(path, data);
}

bool ArtCache::fetch(const Job& job) {
	HttpResponse r = http_get(job.url, 30);
	if (!r.ok()) {
		dlog("art: %s: %s", job.url.c_str(), r.describe().c_str());
		return false;
	}
	int w, h;
	std::vector<unsigned char> decoded;
	const char* why = "";
	if (!decode_image(r.body, decoded, w, h, &why)) {
		dlog("art: can't decode %s (%s)", job.url.c_str(), why);
		return false;
	}
	const unsigned char* src = decoded.data();

	int dw, dh;
	bool crop = true, nearest = false;
	float radius = 0;
	switch (job.kind) {
	// Posters and thumbnails at exactly their on-screen size (dp * kUiScale),
	// so they're drawn unscaled.
	case ArtKind::Poster:
		dw = int(std::lround(182 * kUiScale)), dh = int(std::lround(277 * kUiScale));
		radius = 12 * kUiScale;
		break;
	case ArtKind::PosterLarge:  // .card .poster-img in board.rcss
		dw = int(std::lround(278 * kUiScale)), dh = int(std::lround(422 * kUiScale));
		radius = 14 * kUiScale;
		break;
	case ArtKind::Background: dw = 1280, dh = 720; break;
	case ArtKind::Backdrop: dw = 1920, dh = 1080; break;  // full screen, blurred below
	case ArtKind::Thumb: dw = int(std::lround(160 * kUiScale)), dh = int(std::lround(90 * kUiScale)); break;
	case ArtKind::Qr: dw = 400, dh = 400, nearest = true; break;
	case ArtKind::Icon:  // .addon-logo in browse.rcss
		dw = dh = int(std::lround(96 * kUiScale));
		radius = 18 * kUiScale;
		break;
	case ArtKind::Still:  // .preview-still in browse.rcss
		dw = int(std::lround(652 * kUiScale)), dh = int(std::lround(367 * kUiScale));
		radius = 16 * kUiScale;
		break;
	case ArtKind::LogoBox:  // .preview-logo in browse.rcss
		dw = int(std::lround(560 * kUiScale)), dh = int(std::lround(120 * kUiScale));
		break;
	case ArtKind::Logo:
	default:
		crop = false;
		dh = std::min(150, h);
		dw = std::max(1, int(float(w) * dh / h));
		if (dw > 900) {
			dw = 900;
			dh = std::max(1, int(float(h) * dw / w));
		}
		break;
	}

	// Logos are often wide wordmarks: fit the whole picture inside the box on
	// transparency (addon icons centred, title logos left-aligned); only
	// square addon icons get rounded corners.
	if (job.kind == ArtKind::Icon || job.kind == ArtKind::LogoBox) {
		float aspect = float(w) / h;
		int fw = dw, fh = dh;
		if (aspect > float(dw) / dh) fh = std::max(1, int(std::lround(dw / aspect)));
		else fw = std::max(1, int(std::lround(dh * aspect)));
		std::vector<unsigned char> fit(size_t(fw) * fh * 4);
		resample(src, w, 0, 0, float(w), float(h), fit.data(), fw, fh, false);
		if (job.kind == ArtKind::Icon && aspect > 0.9f && aspect < 1.1f) round_corners(fit.data(), fw, fh, radius);
		std::vector<unsigned char> px(size_t(dw) * dh * 4, 0);
		int ox = job.kind == ArtKind::LogoBox ? 0 : (dw - fw) / 2, oy = (dh - fh) / 2;
		for (int y = 0; y < fh; y++)
			memcpy(&px[(size_t(y + oy) * dw + ox) * 4], &fit[size_t(y) * fw * 4], size_t(fw) * 4);
		return write_rgba(job.path, px, dw, dh);
	}

	float sx = 0, sy = 0, sw = float(w), sh = float(h);
	if (crop) {
		float target = float(dw) / dh, aspect = float(w) / h;
		if (aspect > target) {
			sw = h * target;
			sx = (w - sw) / 2;
		} else {
			sh = w / target;
			sy = (h - sh) / 2;
		}
	}
	std::vector<unsigned char> px(size_t(dw) * dh * 4);
	resample(src, w, sx, sy, sw, sh, px.data(), dw, dh, nearest);
	if (radius > 0) round_corners(px.data(), dw, dh, radius);
	if (job.kind == ArtKind::Backdrop) blur(px.data(), dw, dh, 4);
	return write_rgba(job.path, px, dw, dh);
}

bool load_image_rgba(const std::string& path, std::vector<unsigned char>& pixels, int& w, int& h) {
	std::string data;
	if (!read_file(path, data)) return false;
	if (data.size() >= 12 && memcmp(data.data(), "RGBA", 4) == 0) {
		uint32_t dims[2];
		memcpy(dims, data.data() + 4, 8);
		w = int(dims[0]), h = int(dims[1]);
		size_t need = size_t(w) * h * 4;
		if (w <= 0 || h <= 0 || data.size() < 12 + need) return false;
		pixels.assign(data.begin() + 12, data.begin() + 12 + need);
		return true;
	}
	return decode_image(data, pixels, w, h, nullptr);
}
