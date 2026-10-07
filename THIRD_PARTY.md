# Third-party code, tools and assets

This project is licensed under the GNU General Public License v3.0 (see
`LICENSE`), as required by the GPL-licensed code it includes and links (the
FFmpeg build it uses is GPL-3.0-or-later). It is an unofficial port and is not
affiliated with or endorsed by Stremio.

## Code included in this repository

| Where | What | License |
| --- | --- | --- |
| `native/console_curl.c`, `native/console_curl.h` | libcurl support for PS5 native titles, from [ps5-native-app-boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate) (BlackBearReloaded; its `gmtime_r` follows Howard Hinnant's public-domain `civil_from_days`) | GPL-3.0-or-later |
| `native/ps5-app.ld` | linker layout, from ps5-native-app-boilerplate's `tooling/native/ps5-pie.ld`, with unwind-table symbols added | GPL-3.0-or-later |
| `native/heap.c`, `native/posix_fixes.c`, `native/ps5_modules.c` | approach and parts adapted from the [Kodi port for PS5](https://github.com/VivaLaVent/kodi-ps5) (Team Kodi / VivaLaVent: `shims/native-app/heap_dmem.c`, `thread_stack.c`, `pipe_fallback.c`, `PS5ImeDialog.cpp`) | GPL-2.0-or-later |
| `native/dlmalloc.c` | Doug Lea's malloc 2.8.6 | MIT-0 |
| `third_party/hui/` | [ps5-homebrew-ui](https://github.com/blackbearreloaded/ps5-homebrew-ui) (BlackBearReloaded), the OpenGL interface toolkit: renderer, controller input, text and shape drawing, glyphs, PS5 display and pad code; vendored at commit `4bd9425` (see `third_party/hui/VERSION.txt`) with a small addition (a texture-update call for video frames). Its own notices are `third_party/hui/THIRD_PARTY_NOTICES.md`; the shape shader follows Inigo Quilez's published 2D distance functions (MIT); `stb_truetype` and `stb_image_write` (Sean Barrett, public domain / MIT) are in it and are used only by host tools, not linked into the app | GPL-3.0-or-later |
| `native/hui_runtime_shims.c` | process-level runtime shims for the OpenGL runtime, from ps5-homebrew-ui's `src/runtime/runtime_shims.c` (itself adapted from ps5-opengl's `native-app/runtime_shims.c`) | GPL-3.0-or-later |
| `third_party/json.hpp` | [nlohmann/json](https://github.com/nlohmann/json) 3.11.3 (Niels Lohmann) | MIT |
| `third_party/stb_image.h` | [stb_image](https://github.com/nothings/stb) 2.30 (Sean Barrett): loads the artwork and icons | MIT / public domain |
| `src/hwdec_ps5.cpp` (hardware video decoding), `native/stubs/videodec2.c` | ported from [Nuvio PS5](https://github.com/theghostonline/Nuvio-PS5) (`app/engine/media/src/evo_vdec_native.c`, Husam Osman, itself built on an open-source GPL-3.0 PS5 media player); the libSceVideodec2 structures and call sequence come from there and from [ProsperoLight](https://github.com/blackbearreloaded/ProsperoLight) (BlackBearReloaded) and [SharpProspero](https://github.com/SvenGDK/SharpProspero) (SvenGDK) | GPL-3.0-or-later |
| `src/netstream.cpp` (several connections for big files) | the approach and sizes of Nuvio PS5's `evo_parallel_io.c` | GPL-3.0-or-later |
| `gl/session_watch.cpp` (torrent stream set-up) | follows the behaviour of `createTorrent` in [stremio-video](https://github.com/Stremio/stremio-video) (Stremio) | MIT |

## Linked into the app (downloaded at build time, not included here)

The release `eboot.bin` statically links these. All come from the
[ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) and its
[PacBrew](https://github.com/ps5-payload-dev/pacbrew-repo) packages
(both GPL-3.0 for their own recipes and code); every library keeps its own
license:

| Component | License |
| --- | --- |
| ps5-payload-sdk `libc.a`, start-up code and system-module stubs | GPL-3.0-or-later; FreeBSD-derived parts BSD |
| ps5-native-app-boilerplate start-up code (`tooling/native/app_crt.cpp`) and the clean-room `sce_module/libc.prx` loader shim shipped with the app | GPL-3.0-or-later |
| [LLVM](https://llvm.org/) libc++, libc++abi, libunwind | Apache-2.0 WITH LLVM-exception |
| [ps5-opengl](https://github.com/blackbearreloaded/ps5-opengl) SDK (BlackBearReloaded), v1.0.0: OpenGL 4.6 on the PS5, with [Mesa](https://mesa3d.org/) and OpenGNM PSBC components. Downloaded at build time (checksum-verified); its license texts and notices are copied into the app (`licenses/ps5-opengl/`) | GPL-3.0-or-later; Mesa MIT; the other parts as stated in the SDK |
| [miniupnpc](https://github.com/miniupnp/miniupnp) (Thomas Bernard): the built-in torrent engine's port mapping | BSD-3-Clause |
| [dht](https://github.com/jech/dht) (Juliusz Chroboczek): the built-in torrent engine's DHT | MIT |
| [FFmpeg](https://ffmpeg.org/) (libavformat, libavcodec, libavutil, libswresample, libswscale), built with `--enable-gpl --enable-version3` | GPL-3.0-or-later |
| [x264](https://www.videolan.org/developers/x264.html) (pulled in by that FFmpeg build) | GPL-2.0-or-later |
| [libcurl](https://curl.se/) | curl license (MIT/X derivative) |
| [OpenSSL](https://www.openssl.org/) | Apache-2.0 |
| [libpsl](https://github.com/rockdaboot/libpsl) | MIT; Public Suffix List data MPL-2.0 |
| [libwebp](https://chromium.googlesource.com/webm/libwebp) and libsharpyuv (the posters' format) | BSD-3-Clause |
| [zlib](https://zlib.net/) | zlib |
| [zstd](https://github.com/facebook/zstd) | BSD-3-Clause (dual GPL-2.0) |
| [xz / liblzma](https://tukaani.org/xz/) | 0BSD / public domain |
| [bzip2](https://sourceware.org/bzip2/) | bzip2 license (BSD-style) |
| [GNU libiconv](https://www.gnu.org/software/libiconv/) | LGPL-2.1-or-later |
| [GNU FriBidi](https://github.com/fribidi/fribidi): right-to-left subtitles (Arabic, Hebrew) | LGPL-2.1-or-later |

## Build tools (downloaded at build time, not included here)

| What | License |
| --- | --- |
| [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk): compiler wrappers, headers, sysroot | GPL-3.0-or-later (FreeBSD headers BSD) |
| [LLVM/Clang/lld](https://llvm.org/) 18 | Apache-2.0 WITH LLVM-exception |
| [ps5-native-app-boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate) (BlackBearReloaded): native-app converter, signer, asset validation; its converter and FSELF writer are derived from [SharpProspero](https://github.com/SvenGDK/SharpProspero) (SvenGDK) | GPL-3.0-or-later; GPL-3.0 |
| [MkPFS](https://github.com/PSBrew/MkPFS) (PSBrew): packs the `.ffpfsc` image | GPL-3.0 |
| [DirectXTex texconv](https://github.com/microsoft/DirectXTex) (Microsoft): converts the home-screen pictures | MIT |
| [librsvg](https://gitlab.gnome.org/GNOME/librsvg) `rsvg-convert`: renders the UI icons (`tools/make_icons.sh`) | LGPL-2.1-or-later |
| [ps5-homebrew-ui](https://github.com/blackbearreloaded/ps5-homebrew-ui): its `tools/fetch-opengl-sdk.sh` downloads the pinned ps5-opengl SDK | GPL-3.0-or-later |
| [Pillow](https://python-pillow.org/): draws the sample artwork for the screen-preview tool (`tools/preview/make_sample_art.py`) | MIT-CMU (HPND) |

## Assets in `app/`

| What | Source | License |
| --- | --- | --- |
| UI icons (`app/assets/icons_4k/`, made by `tools/make_icons.sh`) | [Stremio/stremio-icons](https://github.com/Stremio/stremio-icons) | MIT (as declared in its `package.json`) |
| App and menu logo (`app/sce_sys/icon0.png`, `app/assets/icons_4k/logo*.png`) | [homarr-labs/dashboard-icons](https://github.com/homarr-labs/dashboard-icons) | Apache-2.0 (the logo itself is Stremio's trademark) |
| Fonts (`app/fonts/`) | Noto Sans and Noto Emoji (Google) | SIL Open Font License 1.1 (`app/fonts/OFL.txt`) |
| Interface fonts (`app/fonts/Inter-*`, `app/fonts/Montserrat-Medium.ttf`) | Inter (The Inter Project Authors), Montserrat (The Montserrat Project Authors) | SIL Open Font License 1.1 (`app/fonts/Inter-LICENSE.txt`, `app/fonts/Montserrat-LICENSE.txt`) |
| Baked interface fonts (`third_party/hui/assets/fonts/*.huifont`, distance-field renderings made by the toolkit) | Inter, Montserrat, DejaVu Sans Mono (Bitstream, DejaVu), Press Start 2P, Patrick Hand | SIL Open Font License 1.1; Bitstream Vera license for DejaVu (texts beside the files) |
| Interface sounds (`app/sounds/glass/`) | the "glass" sound set of ps5-homebrew-ui (originally ProsperoEden, BlackBearReloaded), generated with ElevenLabs Sound Effects v2 for those projects and prepared by the toolkit's tools; played by the toolkit's mixer (`third_party/hui/src/audio`) | GPL-3.0-or-later, as stated in `third_party/hui/THIRD_PARTY_NOTICES.md` |
| Arabic font (`app/fonts/NotoNaskhArabicUI-*`) | Noto Naskh Arabic UI (Google), as shipped by [Nuvio PS5](https://github.com/theghostonline/Nuvio-PS5) | SIL Open Font License 1.1 (`app/fonts/NotoNaskhArabicUI-NOTICE.txt`) |
| `app/ca-bundle.crt` | Mozilla's CA certificate list, as distributed by curl | MPL-2.0 |
| Home-screen pictures (`app/sce_sys/pic0*`, `pic1*`) | this project's own artwork: the interface's violet light (rendered by `tools/preview`) with the Stremio logo | the design is ours; the Stremio logo is Stremio's trademark |

## License texts

`app/licenses/` holds the license text of every component above, with an
index (`app/licenses/README.txt`). The build copies that folder,
`LICENSE` and this file into the app, together with the notices of the toolkit and
of the OpenGL SDK, so they ship inside the `.ffpfsc`.

## Services and runtime

The app talks to Stremio's public services (`api.strem.io` for the account,
library and addon collection, `link.stremio.com` for sign-in) and to the addons
the user installed. Posters, wallpapers and descriptions come from those addons
(for example Cinemeta and its image service, `images.metahub.space`) and belong to
their owners. Streams play directly on the console (the built-in
torrent engine, or the stream's own link); a Stremio streaming server
([Stremio/server-docker](https://github.com/Stremio/server-docker), GPL-2.0)
is optional. On the console it relies on
[ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus) (drakmor),
[etaHEN](https://github.com/etaHEN/etaHEN) and
[kstuff](https://github.com/EchoStretch/kstuff); none of them is included.

"Stremio" and the Stremio logo are trademarks of their owner. They are used
here only to identify the service this unofficial client connects to.

This project is not affiliated with, endorsed by or connected to Sony
Interactive Entertainment. "PlayStation", "PS5" and "DualSense" are trademarks
of Sony Interactive Entertainment Inc., used only to say which console the
app runs on. No Sony code, SDK, keys or firmware files are included: the
system-module stubs and the `libc.prx` loader shim are independently written
(ps5-payload-sdk, ps5-native-app-boilerplate).
