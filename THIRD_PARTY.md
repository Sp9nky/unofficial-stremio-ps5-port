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
| `third_party/json.hpp` | [nlohmann/json](https://github.com/nlohmann/json) 3.11.3 (Niels Lohmann) | MIT |
| `third_party/stb_image.h` | [stb_image](https://github.com/nothings/stb) 2.30 (Sean Barrett) | MIT / public domain |
| `src/hwdec_ps5.cpp` (hardware video decoding), `native/stubs/videodec2.c` | ported from [Nuvio PS5](https://github.com/theghostonline/Nuvio-PS5) (`app/engine/media/src/evo_vdec_native.c`, Husam Osman, itself built on an open-source GPL-3.0 PS5 media player); the libSceVideodec2 structures and call sequence come from there and from [ProsperoLight](https://github.com/blackbearreloaded/ProsperoLight) (BlackBearReloaded) and [SharpProspero](https://github.com/SvenGDK/SharpProspero) (SvenGDK) | GPL-3.0-or-later |
| `src/netstream.cpp` (several connections for big files) | the approach and sizes of Nuvio PS5's `evo_parallel_io.c` | GPL-3.0-or-later |
| `src/app_detail.cpp` (torrent stream set-up) | follows the behaviour of `createTorrent` in [stremio-video](https://github.com/Stremio/stremio-video) (Stremio) | MIT |

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
| [SDL2](https://www.libsdl.org/) and libsamplerate | zlib; BSD-2-Clause |
| [RmlUi](https://github.com/mikke89/RmlUi) | MIT |
| [dht](https://github.com/jech/dht) (Juliusz Chroboczek): the built-in torrent engine's DHT | MIT |
| [FFmpeg](https://ffmpeg.org/) (libavformat, libavcodec, libavutil, libswresample, libswscale), built with `--enable-gpl --enable-version3` | GPL-3.0-or-later |
| [x264](https://www.videolan.org/developers/x264.html) (pulled in by that FFmpeg build) | GPL-2.0-or-later |
| [libcurl](https://curl.se/) | curl license (MIT/X derivative) |
| [OpenSSL](https://www.openssl.org/) | Apache-2.0 |
| [libpsl](https://github.com/rockdaboot/libpsl) | MIT; Public Suffix List data MPL-2.0 |
| [FreeType](https://freetype.org/) | FreeType License (FTL) |
| [libwebp](https://chromium.googlesource.com/webm/libwebp) and libsharpyuv | BSD-3-Clause |
| [libpng](http://www.libpng.org/) | libpng License |
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
| [DirectXTex texconv](https://github.com/microsoft/DirectXTex) (Microsoft): converts the home-screen backgrounds | MIT |
| [librsvg](https://gitlab.gnome.org/GNOME/librsvg) `rsvg-convert`: renders the UI icons (`tools/make_icons.sh`) | LGPL-2.1-or-later |

## Assets in `app/`

| What | Source | License |
| --- | --- | --- |
| UI icons (`app/assets/icons/`, made by `tools/make_icons.sh`) | [Stremio/stremio-icons](https://github.com/Stremio/stremio-icons) | MIT (as declared in its `package.json`) |
| App and menu logo (`app/sce_sys/icon0.png`, `app/assets/icons/logo.png`) | [homarr-labs/dashboard-icons](https://github.com/homarr-labs/dashboard-icons) | Apache-2.0 (the logo itself is Stremio's trademark) |
| Fonts (`app/fonts/`) | Noto Sans and Noto Emoji (Google) | SIL Open Font License 1.1 (`app/fonts/OFL.txt`) |
| Arabic font (`app/fonts/NotoNaskhArabicUI-*`) | Noto Naskh Arabic UI (Google), as shipped by [Nuvio PS5](https://github.com/theghostonline/Nuvio-PS5) | SIL Open Font License 1.1 (`app/fonts/NotoNaskhArabicUI-NOTICE.txt`) |
| `app/ca-bundle.crt` | Mozilla's CA certificate list, as distributed by curl | MPL-2.0 |
| Home-screen backgrounds (`app/sce_sys/pic0*`, `pic1*`) | panels 3 (home screen) and 1 (loading screen) of `logo-presentation.png` from [Stremio/stremio-brand](https://github.com/Stremio/stremio-brand), Stremio's brand-materials repository | no license file; the repository offers its files for use. The Stremio name, logo and artwork are Stremio's |

## License texts

`app/licenses/` holds the license text of every component above, with an
index (`app/licenses/README.txt`). The build copies that folder,
`LICENSE` and this file into the app, so they ship inside the `.ffpfsc`.

## Services and runtime

The app talks to Stremio's public services (`api.strem.io` for the account,
library and addon collection, `link.stremio.com` for sign-in) and to the addons
the user installed. Streams play directly on the console (the built-in
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
