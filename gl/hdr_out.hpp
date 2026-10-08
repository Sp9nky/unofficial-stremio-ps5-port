// Real HDR output. While an HDR10 or HLG video plays on a TV whose link runs in HDR, the console's scan-out is
// switched to 10-bit BT.2020 PQ and the video goes out as it is: no tone mapping. The interface is drawn at
// reference white (203 nits) on top. Otherwise nothing here is active and the SDR picture is used as before.
//
// How the console is told (the way PS5MC and BlackBearReloaded's video research did it, tested on hardware):
// the title declares HDR capability in param.json (attribute 0x62000000), and the registered scan-out buffers are
// retyped in place with sceVideoOutSubmitChangeBufferAttribute2. The buffers stay 32 bits per pixel; each pixel is
// written as a packed 2:10:10:10 word by the final pass (gl/hdr_out.cpp).

#pragma once

#include "gfx/draw_list.hpp"
#include "gfx/renderer.hpp"

#include <cstdint>

namespace sx::hdr
{

// Finds the video output (the driver keeps its handle private) and logs what the display reports.
void init(int surface_width, int surface_height);

// Called every frame before drawing: asks the console to switch the scan-out to HDR when `want` (right at start:
// the console refuses if the display, the console's HDR setting or the title's metadata do not allow it), and back
// to SDR when it is no longer wanted. Returns whether the scan-out is in HDR now.
bool update(bool want);
bool scanout_hdr();

// What happened, in words, for the screen: "Starting", "Off", "On: the TV is in HDR", "Refused by the console
// (code 0x...)" ... The version changes whenever the text does.
const char *status();
unsigned status_version();

// A video picture as a 10-bit texture (RGB10_A2) from the 32-bit words the player makes; keeps the texture across
// calls while the size stays. Returns false if the GL driver refused.
bool video_upload(std::uint32_t *texture, int *tex_w, int *tex_h, int w, int h, const void *words);

// The HDR frame: the renderer's layers into an offscreen target, then the video (if `video` is a texture, shown in
// `video_rect`, in the 1920 x 1080 layout) and the interface composed into the scan-out. False if it could not
// be done: the caller then switches back to SDR.
bool present(hui::gfx::Renderer &renderer, int width, int height, std::uint32_t video, const hui::gfx::Rect &video_rect,
             std::uint32_t target_framebuffer = 0);

} // namespace sx::hdr
