#pragma once

#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Downloads artwork, scales it to the size the UI draws it at and stores it
// as raw RGBA (".rgba": "RGBA", u32 width, u32 height, pixels) so the
// renderer can load it without decoding. Kinds decide the size:
//
//   poster      182x277 dp, cropped to fill, rounded corners (grid cards)
//   poster_l    278x422 dp, the same (board and search rows)
//   background  1280x720, cropped to fill (detail page, launch screen)
//   logo        height 150, aspect kept (detail page, discover preview)
//   thumb       160x90 dp, cropped to fill (episodes)
//   qr          400x400, nearest neighbour (sign-in)
//   icon        96x96 dp, fitted, transparent around (addon tiles)
//   backdrop    1920x1080, cropped to fill, softly blurred (detail page)
//   still       652x367 dp, cropped to fill, rounded corners (discover preview)
//   logo_box    560x120 dp, fitted, left-aligned on transparency (discover preview)
enum class ArtKind { Poster, PosterLarge, Background, Backdrop, Still, Logo, LogoBox, Thumb, Qr, Icon };

class ArtCache {
public:
	void start(const std::string& dir, int workers);
	void stop();

	// Returns the local path when cached; otherwise queues a download and
	// returns "", calling `ready(path)` on the UI thread when done (path is
	// "" on failure). Newer requests are served first.
	std::string get(const std::string& url, ArtKind kind, std::function<void(const std::string&)> ready);

	// Returns the local path when cached, without downloading.
	std::string peek(const std::string& url, ArtKind kind);

	// Drops queued (not yet started) downloads, e.g. when leaving a page.
	void clear_queue();

private:
	struct Job {
		std::string url, path;
		ArtKind kind;
	};
	std::string path_for(const std::string& url, ArtKind kind) const;
	void worker();
	bool fetch(const Job& job);

	std::string dir_;
	std::mutex mutex_;
	std::condition_variable cv_;
	std::vector<Job> queue_;  // LIFO
	std::map<std::string, std::vector<std::function<void(const std::string&)>>> waiting_;  // by path
	std::map<std::string, bool> known_;  // path -> exists (true) / failed (false)
	std::vector<std::thread> threads_;
	bool stopping_ = false;
};

extern ArtCache g_art;

// Loads an .rgba file (or any image stb_image can decode). Pixels are RGBA,
// straight alpha.
bool load_image_rgba(const std::string& path, std::vector<unsigned char>& pixels, int& w, int& h);
