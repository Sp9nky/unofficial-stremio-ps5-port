cmake_minimum_required(VERSION 3.16)

project(stremio-ps5 C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# PS5_NATIVE: Stremio as the title's own eboot.bin (one app). CMake compiles
# it into libstremio.a; native/build.sh links that the native way.
option(PS5_NATIVE "build the native PS5 app (linked by native/build.sh)" OFF)

find_package(SDL2 REQUIRED)
find_package(RmlUi REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

pkg_check_modules(FREETYPE2 REQUIRED IMPORTED_TARGET freetype2)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
                  libavformat libavcodec libavutil libswresample libswscale)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(WEBP REQUIRED IMPORTED_TARGET libwebp)

set(STREMIO_SOURCES
    src/main.cpp
    src/app.cpp
    src/app_board.cpp
    src/app_browse.cpp
    src/app_detail.cpp
    src/app_settings.cpp
    src/app_watch.cpp
    src/artcache.cpp
    src/http.cpp
    src/hwdec_ps5.cpp
    src/player.cpp
    src/netstream.cpp
    src/render_sdl.cpp
    src/stremio.cpp
    src/subtitles.cpp
    src/tasks.cpp
    src/util.cpp
    src/yuv_convert.cpp
    src/torrent/bencode.cpp
    src/torrent/engine.cpp
    src/torrent/torrent_stream.cpp
)

# PS5 is set by the payload SDK's toolchain file (toolchain/prospero.cmake).
if(PS5 AND PS5_NATIVE)
    add_library(stremio STATIC ${STREMIO_SOURCES} src/pad_ps5.cpp)
    # The torrent engine's DHT and router port mapping: PacBrew's libdht and
    # libminiupnpc (linked by native/build.sh).
    target_compile_definitions(stremio PRIVATE PLATFORM_PS5=1 PLATFORM_PS5_NATIVE=1 HAVE_DHT=1 HAVE_UPNP=1
                               MINIUPNP_STATICLIB)
elseif(PS5)
    message(FATAL_ERROR "the PS5 app is built by native/build.sh (PS5_NATIVE=ON)")
else()
    add_executable(stremio ${STREMIO_SOURCES})
endif()

target_include_directories(stremio PRIVATE src third_party)
target_compile_options(stremio PRIVATE -Wall -Wno-unused-function)

target_link_libraries(stremio PRIVATE
    RmlUi::Core
    SDL2::SDL2
    PkgConfig::FREETYPE2
    PkgConfig::FFMPEG
    PkgConfig::CURL
    PkgConfig::WEBP
    Threads::Threads
    m
)
if(TARGET SDL2::SDL2main AND NOT PS5_NATIVE)
    target_link_libraries(stremio PRIVATE SDL2::SDL2main)
endif()
if(NOT PS5_NATIVE)
    # The torrent engine hashes with OpenSSL; it uses the DHT when libdht is there.
    find_package(OpenSSL REQUIRED)
    target_link_libraries(stremio PRIVATE OpenSSL::Crypto)
    find_library(DHT_LIBRARY dht)
    if(DHT_LIBRARY)
        target_compile_definitions(stremio PRIVATE HAVE_DHT=1)
        target_link_libraries(stremio PRIVATE ${DHT_LIBRARY})
    endif()
endif()
