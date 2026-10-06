# Unofficial Stremio PS5 Port

> [!IMPORTANT]
> **Since version 0.2.0, no Stremio streaming server is needed.**
> <ins>**The app streams movies and TV shows directly on the console**</ins>:
> torrents are downloaded by the app's own built-in torrent engine, and
> direct links (e.g. debrid services) play straight from their source.

A Stremio client for jailbroken PlayStation 5 consoles, running as a native
app on the home screen: browse your catalogs and library, pick a stream, and
watch it, all with the DualSense.

> **Unofficial.** This project is not affiliated with, endorsed by or
> supported by Stremio, or by Sony Interactive Entertainment. It has no
> connection with either. "Stremio" and its logo are trademarks of their
> owner; "PlayStation", "PS5" and "DualSense" are trademarks of Sony
> Interactive Entertainment Inc. No Sony code, SDK, keys or firmware files
> are included.

## What's new in 0.2.0

- **Streams directly on the console**: a built-in torrent engine (trackers,
  DHT, peer exchange) downloads just ahead of what you're watching, into a
  rolling 1 GB cache, and bans peers that send bad data.
- **Hardware video decoding**: H.264, HEVC (8 and 10-bit) and VP9 up to 4K
  decode on the PS5's own video hardware; other formats fall back to
  software decoding.
- **HDR10 tone mapping**: 4K HDR titles are converted to the TV's normal
  range with correct colours.
- **Faster direct links**: big files are downloaded over several
  connections at once.
- **Smarter buffering**: the buffer adapts to the video's bitrate and to how
  well the download keeps up, so playback stops less.

## Features

- **Sign in** with your Stremio account (QR code / link code); your addons
  and library sync from your account.
- **Board, Discover, Library, Addons, Settings** pages styled after Stremio 5,
  with poster art, previews and smooth scrolling.
- **Search** with the PS5's on-screen keyboard.
- **Detail pages** with seasons and episodes; streams grouped by resolution
  (4K, 1080p, 720p, ...) before you pick one.
- **Player** (FFmpeg, hardware decoding) with subtitles (from addons), audio
  track selection, seeking, and resume (Continue Watching).
- **Torrent statistics** (peers, speed, progress) while a torrent starts.
- **One native app**: no launcher, no payload sender and no server needed.

## Requirements

- A jailbroken PS5 that can run homebrew titles, with
  [ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus) (or another
  tool that mounts `.ffpfsc` images) and fake-signed app support (e.g.
  [kstuff](https://github.com/EchoStretch/kstuff) /
  [etaHEN](https://github.com/etaHEN/etaHEN)).
  Developed and tested on firmware **11.60**.

## Install

1. Download `PPSA77711.ffpfsc` from the [Releases](../../releases) page.
2. Copy it to `/data/homebrew/` on the console (FTP, PS5 Upload, ...).
3. ShadowMountPlus registers it: start **Stremio** from the home screen.
4. In **Settings**, sign in to your Stremio account.

To update, close Stremio first, delete the old `PPSA77711.ffpfsc`, wait a few
seconds, then copy the new one. (If the image is replaced while the app is
running, the console keeps the old icon and backgrounds.)

The app keeps its settings, artwork cache, torrent cache and log in its own
storage (`/download0/stremio`).

## Controls

| Where | Button | Action |
| --- | --- | --- |
| Everywhere | ✕ / ○ | select / back |
| | △, R3, Options | search |
| | L1 / R1 | previous / next page |
| | L2 / R2 | scroll a page up / down |
| | Touchpad | open / close the menu |
| | L3 | back to the Board |
| Player | ✕ | pause / play |
| | D-pad left / right | seek 10 s |
| | L1 / R1 | seek 1 min |
| | □ / △ | subtitles / audio |
| | Options | track menu |
| | ○ | stop |

## Building

Builds run on Linux or WSL (Ubuntu 24.04 tested):

```bash
./build.sh
```

The script installs what it needs (clang 18, the
[ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) with its prebuilt
libraries, and
[ps5-native-app-boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate)
for the PS5 tooling), then writes:

- `dist/PPSA77711.ffpfsc`: the app as one image file, for `/data/homebrew/`
- `dist/PPSA77711/`: the same as a folder (if you copy the folder instead,
  make sure `eboot.bin` keeps its execute permission)

`./build.sh desktop` builds a Linux version of the UI for development.

### How it works

Stremio is compiled with the payload SDK's compiler into a static library,
linked with the full C++ runtime, and turned into a signed native
`eboot.bin` by the boilerplate's converter (`native/build.sh`). A native
title runs in a sandbox with a minimal C runtime, so `native/` adds what the
libraries need there:

- `heap.c`: the malloc heap in direct memory (a title's flexible memory is
  only a few hundred MB), 32-byte aligned, leaving room for the video decoder;
- `posix_fixes.c`: 8 MB thread stacks, `pipe()` as a socket pair;
- `console_curl.c`: name lookup through the console's resolver, socket fixes;
- `ps5_modules.c`: loads the on-screen keyboard module before first use;
- `stubs/`: link stubs for system modules the SDK has none for (the video
  decoder).

`native/check_imports.py` lists any system function that would resolve to a
module the console doesn't load into an app.

### Project layout

```
src/          the app (C++17, SDL2, RmlUi, FFmpeg, libcurl)
src/torrent/  the built-in torrent engine and the player's torrent input
native/       native PS5 build: link script, runtime fixes, build and pack scripts
app/          what the console reads: UI (RmlUi documents and styles), icons,
              fonts, certificates, home-screen icon and backgrounds,
              licenses/ (license texts shipped inside the app)
tools/        make_icons.sh (UI icons from stremio-icons), make_dds.ps1 (backgrounds)
```

## Status and known limitations

- The PS5 doesn't let the app accept incoming connections, so torrents only
  use peers the app can connect to; torrents with few reachable seeders start
  slowly or stop to buffer.
- Some formats (AV1, 4K HEVC coded in tiles) decode in software and can
  stutter at 4K.
- HDR is shown tone mapped, not as HDR; Dolby Vision plays its HDR10 layer.
- A Stremio streaming server is optional: set one in Settings only if you
  want server transcoding or prefer it for torrents.

## Credits and licenses

Licensed under the [GNU GPL v3.0](LICENSE). This port builds on
[ps5-native-app-boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate)
(BlackBearReloaded, with tooling derived from
[SharpProspero](https://github.com/SvenGDK/SharpProspero) by SvenGDK),
the [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) and its
[PacBrew](https://github.com/ps5-payload-dev/pacbrew-repo) libraries (SDL2,
RmlUi, FFmpeg, libcurl, OpenSSL and more), techniques from the
[Kodi port for PS5](https://github.com/VivaLaVent/kodi-ps5), hardware video
decoding ported from [Nuvio PS5](https://github.com/theghostonline/Nuvio-PS5)
(Husam Osman), the [dht](https://github.com/jech/dht) library (Juliusz
Chroboczek), [MkPFS](https://github.com/PSBrew/MkPFS) for packaging,
[Stremio's icons](https://github.com/Stremio/stremio-icons) and
[stremio-video](https://github.com/Stremio/stremio-video), and the logo from
[dashboard-icons](https://github.com/homarr-labs/dashboard-icons). See
[THIRD_PARTY.md](THIRD_PARTY.md) for every component and its license.

## Note

This project is vibe coded: it was built with the help of AI.
