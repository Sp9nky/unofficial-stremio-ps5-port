// The built-in torrent engine: plays torrents without a Stremio streaming
// server. A small BitTorrent client made for streaming:
//
//   - finds peers through the addon's trackers (HTTP and UDP), the DHT
//     (Juliusz Chroboczek's dht library) and peer exchange;
//   - fetches the torrent's metadata from the peers (BEP 9) when the addon
//     only gives the info hash;
//   - downloads the pieces just ahead of where the player reads, in order,
//     asking several peers for the next blocks when they're late;
//   - keeps pieces in a rolling cache file of a fixed size in the app's
//     storage, so a big file streams without filling the disk.
//
// It only downloads: it never accepts connections or uploads.
//
// One network thread does all the peer and DHT work; the player's demuxer
// thread reads through TorrentStream (torrent_stream.h), which blocks until
// the pieces it needs are here.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bt {

struct FileInfo {
	std::string path;     // inside the torrent, "/"-separated
	int64_t size = 0;
	int64_t offset = 0;   // in the torrent's byte stream
};

struct Stats {
	bool found = false;           // the torrent is running
	bool has_metadata = false;
	int peers = 0;                // connected
	int known_peers = 0;          // addresses found so far
	bool incoming = false;        // the router forwards our port: peers can connect to us
	double download_rate = 0;     // bytes per second
	double file_progress = 0;     // 0..1 of the selected file downloaded so far
};

struct Torrent;

class Engine {
public:
	static Engine& get();

	// Where the cache file, the DHT's node list and saved metadata go.
	// Call once before start(); cache_bytes is the rolling cache's size.
	void configure(const std::string& data_dir, int64_t cache_bytes);
	void shutdown();

	// Starts downloading the torrent (or keeps it going); any other torrent
	// stops. trackers: "udp://...", "http(s)://..." (addon sources, with or
	// without their "tracker:" prefix; "dht:" entries are ignored).
	void start(const std::string& info_hash_hex, const std::vector<std::string>& trackers);

	// Waits for the torrent's file list. False on timeout, cancel or error
	// (with *error set: "cancelled" when cancelled).
	bool wait_metadata(const std::string& info_hash_hex, std::vector<FileInfo>& files,
	                   const std::atomic<bool>* cancel, double timeout_s, std::string* error);

	// Which file will be played: its pieces come first.
	void select_file(const std::string& info_hash_hex, int file_idx);

	Stats stats(const std::string& info_hash_hex);

	// For TorrentStream. A reader holds the torrent; while any reader is
	// open the engine downloads ahead of the readers' positions.
	std::shared_ptr<Torrent> open_reader(const std::string& info_hash_hex, int file_idx, int* reader_id,
	                                     int64_t* file_size, std::string* error);
	void close_reader(const std::shared_ptr<Torrent>& t, int reader_id);
	// Reads up to n bytes of the file at pos, waiting for them to arrive.
	// Returns the count, 0 at the end of the file, -1 on abort, -2 when the
	// torrent was stopped.
	int read(const std::shared_ptr<Torrent>& t, int reader_id, int file_idx, int64_t pos, uint8_t* buf, int n,
	         const std::atomic<bool>* abort);

	// Picks the file to play: by season/episode in the name for a series,
	// else the largest video. -1 when there's no file.
	static int guess_file(const std::vector<FileInfo>& files, int season, int episode);
};

}  // namespace bt
