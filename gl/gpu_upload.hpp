// Pictures onto the GPU without stalling the frame. The console's GL driver lays a normal (2D) texture out in its
// tiled form on the CPU while glTexImage2D runs: about 23 ms for one 1080p video plane (PS5MC found it, and so did the
// HDR video path here), so a 2880 x 1620 wallpaper would hold the frame for about 100 ms. Rectangle textures stay
// linear: filling one is a row copy. So the picture goes into a rectangle texture and the GPU copies it, texel for
// texel, into an ordinary 2D texture made the way the toolkit makes its own (RGBA8, linear filtering, clamped, no
// mipmaps). The result is the same picture, bit for bit.
#pragma once

#include <cstdint>

namespace sx::gpu
{

// The texture (0 if the driver refused something: the caller then uploads it the usual way).
std::uint32_t texture_from_rgba(int width, int height, const unsigned char *rgba);
// Builds the copy shader now (GL thread), so the first picture does not wait for it.
void prepare();

} // namespace sx::gpu
