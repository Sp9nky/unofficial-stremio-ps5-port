# Unofficial Stremio PS5 Port

[![Support me on Ko-fi](https://img.shields.io/badge/Ko--fi-Support_this_project-FF5E5B?style=for-the-badge&logo=ko-fi&logoColor=white)](https://ko-fi.com/sp9nky)
[![Join the beta on Discord](https://img.shields.io/badge/Discord-Join_the_beta-5865F2?style=for-the-badge&logo=discord&logoColor=white)](https://discord.gg/fJzFmvFqVf)

> [!IMPORTANT]
> **We need beta testers!** The next version is being tested right now, and I need
> people with a jailbroken PS5 to try the test builds on different consoles, TVs,
> networks and streams, and tell me what works and what doesn't. Join the beta
> Discord server to get the test builds and report what you find:
> **https://discord.gg/fJzFmvFqVf**

> [!IMPORTANT]
> **Version 1.2 fixes a crash on HLS (`.m3u8`) streams**, such as live streams from addons.
> See [What's new in 1.2](#whats-new-in-12).
>
> **Version 1.1 fixes HDR, Real-Debrid and Torbox**, adds a sign-in screen, profiles and
> in-app updates. See [What's new in 1.1](#whats-new-in-11).
>
> **Version 1.0 was a full redesign of the app.** Every screen was rebuilt from
> scratch. See [What's new in 1.0](#whats-new-in-10).
>
> **Stremio now lives in the Games section of the PlayStation home screen.** From
> 1.0 it is a game-type title (before, it was under Media). If you have an older
> version, remove it first: see [Install](#install).
>
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

## Screenshots

<table>
  <tr>
    <td width="50%"><a href="docs/screenshots/home.jpg"><img src="docs/screenshots/home.jpg" alt="The home screen"></a><br><sub><b>Home</b>: the wallpaper of the title you are on, and your rows</sub></td>
    <td width="50%"><a href="docs/screenshots/continue-watching.jpg"><img src="docs/screenshots/continue-watching.jpg" alt="Continue Watching"></a><br><sub><b>Continue Watching</b>: play now, or choose another episode or stream</sub></td>
  </tr>
  <tr>
    <td width="50%"><a href="docs/screenshots/discover.jpg"><img src="docs/screenshots/discover.jpg" alt="Discover"></a><br><sub><b>Discover</b>: browse any catalog, with a preview</sub></td>
    <td width="50%"><a href="docs/screenshots/library.jpg"><img src="docs/screenshots/library.jpg" alt="Library"></a><br><sub><b>Library</b>: everything you saved or watched</sub></td>
  </tr>
  <tr>
    <td width="50%"><a href="docs/screenshots/episodes.jpg"><img src="docs/screenshots/episodes.jpg" alt="Episodes"></a><br><sub><b>Episodes</b>: seasons, what you have watched, where you stopped</sub></td>
    <td width="50%"><a href="docs/screenshots/streams.jpg"><img src="docs/screenshots/streams.jpg" alt="Streams"></a><br><sub><b>Streams</b>: grouped by resolution, with size and seeds</sub></td>
  </tr>
  <tr>
    <td width="50%"><a href="docs/screenshots/player.jpg"><img src="docs/screenshots/player.jpg" alt="The player"></a><br><sub><b>Player</b>: subtitles, audio tracks, seeking</sub></td>
    <td width="50%"><a href="docs/screenshots/search.jpg"><img src="docs/screenshots/search.jpg" alt="Search"></a><br><sub><b>Search</b>: with the PS5's own keyboard</sub></td>
  </tr>
</table>

The titles and artwork in these pictures are invented (made by `tools/preview`).

## What's new in 1.2

- **A crash on HLS streams is fixed.** Streams that arrive as an HLS playlist (`.m3u8`), such
  as live broadcasts and recordings from some addons (a Kick addon, for example), made the app
  close a moment after they started. They play now, live ones and recorded ones, and you can
  seek in a recording. Reported in [issue #7](https://github.com/Sp9nky/unofficial-stremio-ps5-port/issues/7).

## What's new in 1.1

- **Real HDR, fixed, and automatic.** While an HDR10 or HLG video plays on a screen that
  reports HDR, it goes out as real HDR instead of being squeezed into SDR, and the interface
  stays at normal white on top. When the video ends, the screen goes back to normal. On a
  screen that doesn't report HDR (an SDR monitor, for example) nothing changes: its colours stay
  as they are, and HDR video is converted to normal colours. There is no HDR setting to turn
  on or off. It needs an HDR screen and the PS5's own HDR setting at **On When Supported**
  (Settings, Screen and Video, Video Output). The screen may blink for a moment when an HDR
  video starts and when it ends. If you update from an older version, delete the old image,
  wait a few seconds, then copy the new one so the console reads the app's HDR flags. Dolby
  Vision profile 8 plays its HDR10 picture; profile 5 files (no HDR10 picture inside) are
  flagged on screen and can show wrong colours. The green picture some 10-bit files had is
  gone too.
- **Real-Debrid and Torbox, fixed.** Streams from debrid addons (for example Torrentio with
  your Real-Debrid key) now play straight from the debrid service and show as
  **[RD+] Torrentio** in the list. For Torbox, connect it in **Settings, Torbox** (you get a
  code to approve on your phone) and it resolves torrents for you. Without a debrid key
  nothing changes. Playback no longer stops when a piece of a stream fails, it just tries again.
- **Sign-in screen.** If nobody is signed in, the app opens on a sign-in screen (QR code, link
  and code) instead of an empty Home. **Continue without account** skips it.
- **Profiles.** One Stremio account per person on the same console, each with its own library,
  Continue Watching, addons and settings. With more than one profile, **Who's watching?**
  opens first; **Settings, Change profile** opens it any time.
- **Updates from inside the app.** Stremio checks GitHub when it starts (and in **Settings,
  Check for updates**), downloads the new version with a progress bar, checks it and replaces
  its own image. It needs a payload loader on port 9021 and the image in `/data/homebrew`; the
  old image is kept as `PPSA77711.ffpfsc.old` and the steps go to `/data/stremio-update.log`.
- **Audio and subtitles.** Videos start with the English audio track (the best English one,
  never a commentary); change the order in **Settings, Audio languages**, or type `-` to use
  the file's own default. Subtitles in your language turn on by themselves (**Settings,
  Subtitle languages**; **Subtitles on automatically** switches it off), and the message that
  names them now shows at the top of the loading screen. Arabic no longer shows backwards, and
  Hebrew, Thai, Japanese, Chinese and Korean subtitles now have fonts.
- **Pictures load faster and the wallpaper changes smoothly** from title to title.
  Posters and wallpapers start downloading during the boot animation, and the app waits for
  the first ones before showing Home.
- **Smoother dialogs**, refresh-rate matching for video (**Settings, Match refresh rate to
  video**), and your watched episodes and progress go back to your Stremio account.

## What's new in 1.0

**A full redesign.** The whole interface is new: it keeps Stremio's look and is
drawn on the console's GPU, at 4K when your TV supports it (1080p otherwise).

And more besides the new look:

- **Now in the Games section.** Stremio is a game-type title, so it shows up under
  Games on the PlayStation home screen. It also gets all of the console's direct
  memory (about 11 GB instead of about 2 GB), which the artwork and the video decoder
  use.
- **Hardware decoding reworked** for the new interface: the video goes from the
  console's decoder to the GPU for display, and the player shows whether hardware
  or software decoding is in use.
- **Continue Watching picks up where you left off**: it plays the same episode with
  the same stream, at the point where you stopped, and after a finished episode it
  goes on to the next one.
- **○ always takes you back**, all the way to the Board.
- **The video pauses by itself** when you press the PS button.
- **Faster, sharper pictures**: the app loads the posters and wallpapers before it
  shows the home screen, and uses higher-resolution wallpapers where they exist.
- **Soft interface sounds** when you move around, switch page or go back (turn them
  off in Settings, **Interface sounds**).
- **Search** opens the PS5's own keyboard, at the top right of the screen.
- Everything of 0.3.x is still here: the built-in torrent engine, direct links,
  subtitles (including Arabic and Hebrew) and sign-in with a code.

## What's new in 0.3.1

- **Arabic subtitles fixed**: they showed as white boxes; they now show in
  an Arabic font, right to left, with joined letters.

## What's new in 0.3.0

Fewer buffering stops:

- **Long read-ahead on the console's storage**: the app now uses up to
  16 GB of the PS5's storage. Built-in torrents keep a 6 GB cache and
  download up to 3 GB (about a quarter of an hour of 4K) ahead of what
  you're watching; streams from a streaming server or a direct link keep up
  to 6 GB ahead.
- **Six connections for streaming-server streams** too (before, only direct
  links): one connection from a server measured 3-6 MB/s, six about 8 MB/s.
- If a source is slower than the video itself (a torrent with few seeders),
  it can still stop: pick a stream with more seeders or a smaller file.

## What's new in 0.2.0

- **Streams directly on the console**: a built-in torrent engine (trackers,
  DHT, peer exchange) downloads just ahead of what you're watching, into a
  rolling cache of up to 6 GB, and bans peers that send bad data.
- **Hardware video decoding**: H.264, HEVC (8 and 10-bit) and VP9 up to 4K
  decode on the PS5's own video hardware; other formats fall back to
  software decoding.
- **Real HDR**: HDR10 and HLG titles go out as HDR on a screen that reports HDR
  (and are converted to normal colours on any other screen).
- **Faster direct links**: big files are downloaded over several
  connections at once.
- **Smarter buffering**: the buffer adapts to the video's bitrate and to how
  well the download keeps up, so playback stops less.

## Features

- **Sign in** with your Stremio account (QR code / link code); your addons
  and library sync from your account.
- **Board, Discover, Library, Addons, Settings** pages in Stremio's colours,
  with poster art, wallpapers and smooth scrolling.
- **Search** with the PS5's on-screen keyboard.
- **Title pages** with seasons and episodes; streams grouped by resolution
  (4K, 1080p, 720p, ...) before you pick one.
- **Player** (FFmpeg, hardware decoding) with subtitles (from addons), audio
  track selection, seeking, and resume (Continue Watching).
- **Torrent statistics** (peers, speed, progress) while a torrent starts.
- **Profiles**: one Stremio account per person, chosen on a Who's watching? screen.
- **Updates inside the app**: it finds, downloads, verifies and installs new versions.
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
3. ShadowMountPlus registers it: start **Stremio** from the **Games** section of
   the home screen.
4. In **Settings**, sign in to your Stremio account.

From 1.1 you can update inside the app (**Settings, Check for updates**, or the message at
start-up). To update by hand (also from 0.x or 1.0), close Stremio first, delete the old
`PPSA77711.ffpfsc`, wait a few seconds, then copy the new one. (If the image is
replaced while the app is running, the console keeps the old icon and
backgrounds.)

> [!NOTE]
> Version 1.0 keeps the title ID `PPSA77711`, but it is a game-type title: it
> appears in the **Games** section, where 0.x was under Media.

The app keeps its settings, artwork cache, read-ahead caches and log in its
own storage (`/download0/stremio`, up to 16 GB), the same place as before, so
your sign-in should carry over; if it doesn't, sign in again.

## Controls

| Where | Button | Action |
| --- | --- | --- |
| Everywhere | ✕ / ○ | select / back (○ from any page goes to the Board) |
| | D-pad, left stick | move |
| | △ or Options | search |
| Board | □ | See all (not on Continue Watching) |
| | ✕ on a Continue Watching card | play now / choose another episode / choose another stream |
| | Options, on a Continue Watching card | remove it (asks first) |
| Addons | □ | reload the addons |
| | ✕ on an addon you added by URL | remove it |
| Title page, episodes | ✕ | choose the episode |
| | L1 / R1 | previous / next season |
| | △ | mark the episode watched / not watched |
| Title page, streams | ✕ | play |
| | □ | play with the streaming server |
| Player | ✕ or touchpad | pause / play |
| | D-pad left / right | seek 10 s |
| | L1 / R1 | seek 1 min |
| | L3 | back to the start |
| | □ / △ | next subtitle / next audio track |
| | L2 / R2 | subtitle delay -/+ 0.25 s |
| | D-pad up or Options | track menu |
| | ○ | stop |

## Building

Builds run on Linux or WSL (Ubuntu 24.04 tested):

```bash
./build.sh
```

The script installs what it needs (clang 18, the
[ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) with its prebuilt
libraries,
[ps5-native-app-boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate)
for the PS5 tooling, and, through
[ps5-homebrew-ui](https://github.com/blackbearreloaded/ps5-homebrew-ui)'s own
script, the [ps5-opengl](https://github.com/blackbearreloaded/ps5-opengl) SDK,
a pinned release that is checked against its SHA-256), then writes:

- `dist/PPSA77711.ffpfsc`: the app as one image file, for `/data/homebrew/`
- `dist/PPSA77711/` (and `.zip`): the same as a folder (if you copy the folder
  instead, make sure `eboot.bin` keeps its execute permission)

`tools/preview/preview.sh` renders the screens on a PC (Linux or WSL with
Mesa's software OpenGL) as pictures, with invented sample art, for working on
the design without a console.

### How it works

Stremio is compiled with the payload SDK's compiler, linked with the full C++
runtime and the OpenGL SDK, and turned into a signed native `eboot.bin` by the
boilerplate's converter (`gl/build.sh`). The screens are drawn by the
ps5-homebrew-ui toolkit (vendored in `third_party/hui`) with OpenGL 4.6 on the
console's GPU. A native title runs in a sandbox with a minimal C runtime, so
`native/` adds what the libraries need there:

- `heap.c`: the malloc heap in direct memory (a title's flexible memory is
  only a few hundred MB), 32-byte aligned, leaving room for the video decoder;
- `posix_fixes.c`: 8 MB thread stacks, `pipe()` as a socket pair;
- `console_curl.c`: name lookup through the console's resolver, socket fixes;
- `kernel_mem.c`: memory mapping through the console's own library;
- `ps5_modules.c`: loads the on-screen keyboard module before first use;
- `hui_runtime_shims.c`: the process-level shims the OpenGL runtime needs;
- `stubs/`: link stubs for system modules the SDK has none for (the video
  decoder).

`native/check_imports.py` lists any system function that would resolve to a
module the console doesn't load into an app.

### Project layout

```
gl/           the app's logic and entry point: accounts, catalogs, library, search,
              playback, the artwork cache, the PS5 keyboard dialog, the main loop
ui/           the screens and their look: Board, Discover, Library, Addons, Settings,
              title page, player controls, pop-ups, the boot screen
src/          what is underneath: the Stremio API, HTTP, the FFmpeg player and hardware
              decoder, subtitles, audio output
src/torrent/  the built-in torrent engine and the player's torrent input
native/       native PS5 build: link script, runtime fixes, stubs
third_party/  hui (ps5-homebrew-ui, vendored), nlohmann/json, stb_image
app/          what the console reads: icons, fonts, certificates, the home-screen
              icon and pictures (sce_sys), licenses/ (license texts shipped inside the app)
tools/        preview/ (renders the screens on a PC), make_icons.sh (icons from
              stremio-icons), make_boot_art.py and make_dds.ps1 (the boot screen's name and
              the console's start-up pictures)
```

## Status and known limitations

- The PS5 doesn't let the app accept incoming connections, so torrents only
  use peers the app can connect to; torrents with few reachable seeders start
  slowly or stop to buffer.
- Some formats (AV1, 4K HEVC coded in tiles) decode in software and can
  stutter at 4K.
- Dolby Vision plays its HDR10 layer; profile 5 files have no HDR10 layer and can show
  wrong colours.
- Hebrew, Thai, Chinese, Japanese and Korean text has fonts, but Indian scripts (Hindi,
  Tamil...) don't, and the interface isn't mirrored for right-to-left languages.
  Picture-based subtitles (PGS, VobSub) aren't supported.
- Some wallpapers are only available in 1280x720 at the image service Stremio
  uses; they are enlarged and sharpened, but cannot look as sharp as full HD ones.
- A Stremio streaming server is optional: set one in Settings only if you
  want server transcoding or prefer it for torrents.

## Credits and licenses

Licensed under the [GNU GPL v3.0](LICENSE). This app stands on the work of many
developers; thank you to all of them:

- **BlackBearReloaded**: [ps5-homebrew-ui](https://github.com/blackbearreloaded/ps5-homebrew-ui),
  the OpenGL interface toolkit this version is built on (renderer, controller
  input, text and shapes, runtime shims; vendored in `third_party/hui`), and its
  sound mixer and the *Glass* interface sounds (from ProsperoEden, generated with
  ElevenLabs Sound Effects);
  [ps5-opengl](https://github.com/blackbearreloaded/ps5-opengl), which makes
  OpenGL 4.6 run on the PS5, together with [Mesa](https://mesa3d.org/) and
  OpenGNM PSBC; and
  [ps5-native-app-boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate),
  the native-app foundation and tooling (with parts derived from
  [SharpProspero](https://github.com/SvenGDK/SharpProspero) by SvenGDK). The
  self-update notes of its [ProsperoStore](https://github.com/blackbearreloaded/ProsperoStore)
  inspired how this app updates itself (a helper started through the payload loader);
  no code was taken from it: the helper and the update code were written for this app.
- The [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) and its
  [PacBrew](https://github.com/ps5-payload-dev/pacbrew-repo) libraries
  (FFmpeg, libcurl, OpenSSL, libwebp, GNU FriBidi, miniupnpc and more).
- The Kodi ports for PS5, [PS5MC](https://github.com/VivaLaVent/PS5MC) and
  [kodi-ps5](https://github.com/VivaLaVent/kodi-ps5) (Team Kodi, VivaLaVent):
  techniques for the heap, thread stacks and the keyboard dialog, and, in 1.1, how HDR
  is switched on (only for HDR10 and HLG video, and only when the display link reports
  HDR), the HLG-to-PQ and interface-compositing maths, refresh-rate matching on VRR
  links, and sending video to the GPU as rectangle textures.
- [Nuvio PS5](https://github.com/theghostonline/Nuvio-PS5) (Husam Osman): the
  hardware video decoding and parallel downloads were ported from it; in 1.1 also the
  choice of the audio track, the automatic subtitles, the 10-bit SDR video path, the
  read-ahead in memory and how a failed download piece is tried again. Its Nuvio TV web
  app, [NuvioTVSmart](https://github.com/NuvioMedia/NuvioTVSmart) (NuvioMedia), is where
  the Real-Debrid and Torbox handling was ported from.
- [EVO Player](https://github.com/sainsaji/EVO-PLAYER-PS5) (sainsaji): the in-place HDR
  switch of the console's screen, which Nuvio PS5 and PS5MC build on.
- [dht](https://github.com/jech/dht) (Juliusz Chroboczek) and
  [miniupnpc](https://github.com/miniupnp/miniupnp) (Thomas Bernard): the
  torrent engine's peer discovery and port mapping.
- [stb](https://github.com/nothings/stb) (Sean Barrett): image loading, and the
  font baking behind the interface's text;
  [nlohmann/json](https://github.com/nlohmann/json) (Niels Lohmann);
  [libwebp](https://chromium.googlesource.com/webm/libwebp) (Google): the
  posters' format; Inigo Quilez's published distance functions, behind the
  shapes the toolkit draws.
- Fonts: [Inter](https://github.com/rsms/inter) (The Inter Project Authors),
  [Montserrat](https://github.com/JulietaUla/Montserrat) (The Montserrat Project
  Authors) and [DejaVu](https://dejavu-fonts.github.io/) for the interface (the
  toolkit also ships Press Start 2P and Patrick Hand); Noto Sans, Noto Emoji and
  Noto Naskh Arabic UI, Noto Sans Hebrew and Thai, and Noto Sans CJK / Source Han Sans
  (Google, Adobe; subsets for Japanese, Chinese and Korean subtitles) for other scripts.
- Build tools: [MkPFS](https://github.com/PSBrew/MkPFS) (PSBrew) for packaging,
  Microsoft's [DirectXTex](https://github.com/microsoft/DirectXTex) for the
  console's start-up pictures, [librsvg](https://gitlab.gnome.org/GNOME/librsvg)
  for the icons and [Pillow](https://python-pillow.org/) for the pictures and
  the screen previews.
- On the console, the app relies on
  [ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus) (drakmor),
  [etaHEN](https://github.com/etaHEN/etaHEN) and
  [kstuff](https://github.com/EchoStretch/kstuff); none of them is included.
- [Stremio](https://github.com/Stremio): [stremio-icons](https://github.com/Stremio/stremio-icons)
  and [stremio-video](https://github.com/Stremio/stremio-video), and the logo
  from [dashboard-icons](https://github.com/homarr-labs/dashboard-icons)
  (homarr-labs). [stremio-core](https://github.com/Stremio/stremio-core) is the
  reference for how the account's library, Continue Watching, watched episodes and the
  torrent settings behave (no code was taken from it). The app uses Stremio's public
  services for your account.
- [Real-Debrid](https://real-debrid.com/) and [Torbox](https://torbox.app/) are services
  of their own companies: the app only uses their public interfaces, with your own key or
  account. [Torrentio](https://github.com/TheBeastLT/torrentio-scraper) and the other
  addons you install are yours and are not part of this app.
- Posters, wallpapers and descriptions come from the addons you use (for example
  Cinemeta and its image service) and belong to their owners.

See [THIRD_PARTY.md](THIRD_PARTY.md) for every component, where it is used and
its license.

## Note

This project is vibe coded: it was built with the help of AI.
