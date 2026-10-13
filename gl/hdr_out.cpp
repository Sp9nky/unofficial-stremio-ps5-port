#include "hdr_out.hpp"

#include "gfx/gl_program.hpp"
#include "platform/ps5/system.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef PLATFORM_PS5_NATIVE
extern "C"
{
int32_t sceVideoOutGetOutputStatus(int32_t handle, void *status);
int32_t sceVideoOutGetResolutionStatus(int32_t handle, void *status);
void sceVideoOutSetBufferAttribute2(void *attribute, std::uint64_t format, std::uint32_t tiling, std::uint32_t width,
                                    std::uint32_t height, std::uint64_t option, std::uint32_t reserved0,
                                    std::uint64_t reserved1);
int32_t sceVideoOutSubmitChangeBufferAttribute2(int32_t handle, int32_t set_index, const void *attribute, void *option);
}
#endif

namespace sx::hdr
{

namespace
{

// The scan-out buffer formats: the driver's own SDR one, and the HDR 10-bit BT.2020 PQ one (2:10:10:10).
constexpr std::uint64_t kFormatSdr = UINT64_C(0x8000000000000000);
constexpr std::uint64_t kFormatHdr = UINT64_C(0x8100070422000000);

struct State
{
    int surface_w = 0, surface_h = 0;
    std::int32_t handle = -1;
    bool hdr = false;
    int failures = 0;
    std::int64_t switched_at_us = 0;
    bool confirmed = false;
    bool want_prev = false;  // update(): a video wanting HDR was already playing last call
    bool link_sdr = false;  // the link reported SDR when that video started: stay in SDR until it ends
    std::string status = "Starting";
    std::string no_handle_text = "Not available: no video output found";
    unsigned status_version = 0;
    // GL
    GLuint ui_fbo = 0, ui_tex = 0;
    int ui_w = 0, ui_h = 0;
    GLuint program = 0, vao = 0;
    GLuint sdr_program = 0;
    bool gl_failed = false;
} g;

#ifdef PLATFORM_PS5_NATIVE
// SceVideoOutOutputStatus: resolution, dynamic range (0 unknown, 1 SDR, 2 HDR), refresh rate, flags (bit 0: HDR active).
struct OutputStatus
{
    std::uint32_t resolution;
    std::uint32_t dynamic_range;
    std::uint64_t refresh;
    std::uint64_t flags;
    std::uint64_t reserved[3];
};

bool read_status(std::int32_t handle, OutputStatus *out)
{
    std::uint64_t raw[16] = {}; // larger than the structure, zeroed: safe if the layout grew
    if (sceVideoOutGetOutputStatus(handle, raw) != 0)
        return false;
    *out = *reinterpret_cast<const OutputStatus *>(raw);
    return true;
}

// PS5MC's IsDisplayHdr: the link runs in HDR (dynamic range 2 or flag bit 0). Logs what the display reports.
bool link_is_hdr(const char *when)
{
    OutputStatus st{};
    if (!read_status(g.handle, &st))
    {
        hui::sys::log("[HDR] display (%s): status unreadable", when);
        return false;
    }
    hui::sys::log("[HDR] display (%s): dynamic range %u (%s), flags %#llx", when, st.dynamic_range,
                  st.dynamic_range == 2 ? "HDR" : st.dynamic_range == 1 ? "SDR" : "unknown",
                  static_cast<unsigned long long>(st.flags));
    return st.dynamic_range == 2 || (st.flags & 1);
}
#endif

void set_status(const std::string &text)
{
    if (text != g.status)
    {
        g.status = text;
        ++g.status_version;
        hui::sys::log("[HDR] status: %s", text.c_str());
    }
}

bool switch_scanout(bool hdr, int *rc_out)
{
#ifdef PLATFORM_PS5_NATIVE
    if (g.handle < 0)
        return false;
    glFinish(); // nothing may still be drawing in the old format
    alignas(8) unsigned char attribute[128] = {};
    sceVideoOutSetBufferAttribute2(attribute, hdr ? kFormatHdr : kFormatSdr, 0, static_cast<std::uint32_t>(g.surface_w),
                                   static_cast<std::uint32_t>(g.surface_h), 0, 0, 0);
    const std::int32_t rc = sceVideoOutSubmitChangeBufferAttribute2(g.handle, 0, attribute, nullptr);
    hui::sys::log("[HDR] scan-out to %s: %s (rc %#x)", hdr ? "HDR (BT.2020 PQ, 10-bit)" : "SDR", rc == 0 ? "in effect" : "refused",
                  static_cast<unsigned>(rc));
    *rc_out = rc;
    return rc == 0;
#else
    (void)hdr;
    *rc_out = -1;
    return false;
#endif
}

// ---- the final pass ---------------------------------------------------------------------------------------

constexpr const char *kVertex = R"(
void main()
{
    vec2 p = vec2[3](vec2(0.0, 0.0), vec2(2.0, 0.0), vec2(0.0, 2.0))[gl_VertexID];
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// Everything is floating point (no integer conversions): the packed word is built from its four bytes.
constexpr const char *kFragment = R"(
layout(location = 0) uniform sampler2D u_ui;
layout(location = 1) uniform sampler2D u_video;
layout(location = 2) uniform vec4 u_rect;  // the video's rectangle in pixels: x, y (from the top), width, height
layout(location = 3) uniform vec2 u_size;  // the surface in pixels
layout(location = 4) uniform float u_has_video;  // 0 none, 1 a 10-bit texture, 2 planes
layout(location = 5) uniform sampler2DRect u_yplane;
layout(location = 6) uniform sampler2DRect u_uvplane;
layout(location = 7) uniform vec4 u_planes;  // plane width, height, the scale from a texel to a 10-bit value, 1 for HLG
out vec4 frag_color;

// HLG video on the PQ-only HDR output (ITU-R BT.2100), as PS5MC's shader does it: HLG signal to scene light (inverse
// OETF), to display light for a 1000 cd/m2 display (OOTF, gamma 1.2), to the PQ signal normalised to 10000 cd/m2.
vec3 hlg_to_pq(vec3 e)
{
    const float a = 0.17883277;
    const float b = 0.28466892;
    const float c = 0.55991073;
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;
    e = clamp(e, 0.0, 1.0);
    vec3 low = e * e / 3.0;
    vec3 high = (exp((e - c) / a) + b) / 12.0;
    vec3 scene = mix(low, high, step(0.5, e));
    float ys = dot(scene, vec3(0.2627, 0.6780, 0.0593));
    vec3 display = scene * pow(max(ys, 1e-6), 0.2) * (1000.0 / 10000.0);
    vec3 p = pow(max(display, 0.0), vec3(m1));
    return pow((c1 + c2 * p) / (1.0 + c3 * p), vec3(m2));
}

vec3 pq_encode(vec3 l)
{
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;
    vec3 p = pow(max(l, vec3(0.0)), vec3(m1));
    return pow((c1 + c2 * p) / (1.0 + c3 * p), vec3(m2));
}

void main()
{
    vec2 fc = gl_FragCoord.xy;
    vec3 pq = vec3(0.0);
    if (u_has_video > 0.5)
    {
        vec2 uv = vec2((fc.x - u_rect.x) / u_rect.z, ((u_size.y - fc.y) - u_rect.y) / u_rect.w);
        if (uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 && uv.y <= 1.0)
        {
            if (u_has_video < 1.5)
                pq = texture(u_video, uv).rgb;
            else
            {
                // The planes at their own size (rectangle textures take texel coordinates); BT.2020 non-constant
                // luminance, limited range, 10-bit: the matrix leaves the signal PQ (or HLG) coded.
                vec2 tc = uv * u_planes.xy;
                float y10 = texture(u_yplane, tc).r * u_planes.z;
                vec2 c10 = texture(u_uvplane, tc * 0.5).rg * u_planes.z;
                float yn = (y10 - 64.0) / 876.0;
                float cb = (c10.x - 512.0) / 896.0;
                float cr = (c10.y - 512.0) / 896.0;
                vec3 e = clamp(vec3(yn + 1.4746 * cr, yn - 0.164553 * cb - 0.571353 * cr, yn + 1.8814 * cb), 0.0, 1.0);
                pq = u_planes.w > 0.5 ? hlg_to_pq(e) : e;
            }
        }
    }
    // The interface was drawn into a transparent target, so its colour is premultiplied. It is SDR (sRGB-ish,
    // BT.709): to light, to BT.2020, scaled so that white is 203 nits, to PQ.
    vec4 ui = texture(u_ui, fc / u_size);
    if (ui.a > 0.0)
    {
        vec3 c = clamp(ui.rgb / ui.a, 0.0, 1.0);
        // sRGB to linear (the piecewise curve of IEC 61966-2-1, as PS5MC's GUI shader does)
        vec3 lin = mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
        vec3 wide = vec3(dot(lin, vec3(0.6274, 0.3293, 0.0433)), dot(lin, vec3(0.0691, 0.9195, 0.0114)),
                         dot(lin, vec3(0.0164, 0.0880, 0.8956)));
        pq = mix(pq, pq_encode(wide * (203.0 / 10000.0)), ui.a);
    }
    vec3 q = floor(clamp(pq, 0.0, 1.0) * 1023.0 + 0.5);
    // The 2:10:10:10 word (R in bits 9-0, G 19-10, B 29-20, 3 in 31-30) as the framebuffer's four bytes
    float b0 = mod(q.r, 256.0);
    float b1 = floor(q.r / 256.0) + 4.0 * mod(q.g, 64.0);
    float b2 = floor(q.g / 64.0) + 16.0 * mod(q.b, 16.0);
    float b3 = floor(q.b / 16.0) + 192.0;
    // a quarter of a step in: lands on the right byte whether the hardware rounds or cuts
    frag_color = (vec4(b2, b1, b0, b3) + 0.25) / 255.0;
}
)";

// 10-bit SDR planes into an RGBA8 target (no integer conversions, as above). Row 0 of the target is the top of the picture.
constexpr const char *kSdrFragment = R"(
layout(location = 0) uniform sampler2DRect u_yplane;
layout(location = 1) uniform sampler2DRect u_uvplane;
layout(location = 2) uniform vec4 u_planes;  // plane width, height, the scale from a texel to a 10-bit value, 1 for BT.601
layout(location = 3) uniform vec2 u_out;     // the target's size in pixels
out vec4 frag_color;

void main()
{
    vec2 tc = gl_FragCoord.xy / u_out * u_planes.xy;
    float y10 = texture(u_yplane, tc).r * u_planes.z;
    vec2 c10 = texture(u_uvplane, tc * 0.5).rg * u_planes.z;
    float yn = (y10 - 64.0) / 876.0;
    float cb = (c10.x - 512.0) / 896.0;
    float cr = (c10.y - 512.0) / 896.0;
    vec3 rgb = u_planes.w > 0.5 ? vec3(yn + 1.402 * cr, yn - 0.344136 * cb - 0.714136 * cr, yn + 1.772 * cb)
                                : vec3(yn + 1.5748 * cr, yn - 0.187324 * cb - 0.468124 * cr, yn + 1.8556 * cb);
    frag_color = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
)";

bool ensure_program()
{
    if (g.program != 0)
        return true;
    g.program = hui::gfx::build_program("hdr-final", kVertex, kFragment);
    if (g.program == 0)
        return false;
    glGenVertexArrays(1, &g.vao);
    return true;
}

bool ensure_sdr_program()
{
    if (g.sdr_program != 0)
        return true;
    g.sdr_program = hui::gfx::build_program("sdr-planes", kVertex, kSdrFragment);
    if (g.sdr_program == 0)
        return false;
    if (g.vao == 0)
        glGenVertexArrays(1, &g.vao);
    return true;
}

bool ensure_target(int w, int h)
{
    if (g.ui_fbo != 0 && g.ui_w == w && g.ui_h == h)
        return true;
    if (g.ui_fbo != 0)
    {
        glDeleteFramebuffers(1, &g.ui_fbo);
        glDeleteTextures(1, &g.ui_tex);
        g.ui_fbo = g.ui_tex = 0;
    }
    glGenTextures(1, &g.ui_tex);
    glBindTexture(GL_TEXTURE_2D, g.ui_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &g.ui_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g.ui_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g.ui_tex, 0);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE)
    {
        hui::sys::log("[HDR] the interface target %dx%d is incomplete (%#x)", w, h, static_cast<unsigned>(status));
        return false;
    }
    g.ui_w = w;
    g.ui_h = h;
    return true;
}

} // namespace

void init(int surface_width, int surface_height)
{
    g.surface_w = surface_width;
    g.surface_h = surface_height;
#ifdef PLATFORM_PS5_NATIVE
    // The driver opened the video output and keeps its handle private, but it prints it to the app's log
    // ("[ps5-agc] present-open handle=XXXXXXXX"): read it from there.
    std::int32_t from_log = -1;
    if (FILE *f = std::fopen("/download0/hui/dev/app.log", "r"))
    {
        char line[512];
        while (std::fgets(line, sizeof(line), f))
        {
            const char *at = std::strstr(line, "present-open handle=");
            if (at)
                from_log = static_cast<std::int32_t>(std::strtoul(at + 20, nullptr, 16));
        }
        std::fclose(f);
    }
    std::uint64_t raw[16] = {};
    if (from_log >= 0)
    {
        const int rc = sceVideoOutGetOutputStatus(from_log, raw);
        hui::sys::log("[HDR] video out handle %d, from the driver's log (output status rc %#x)", from_log, static_cast<unsigned>(rc));
        g.handle = from_log;
        link_is_hdr("app start");
        return;
    }
    // Not in the log: ask each number until one answers.
    for (std::int32_t h = 0; h < 4096; ++h)
    {
        std::uint64_t a[16] = {}, b[16] = {};
        if (sceVideoOutGetOutputStatus(h, a) == 0 || sceVideoOutGetResolutionStatus(h, b) == 0)
        {
            g.handle = h;
            hui::sys::log("[HDR] video out handle %d, found by asking", h);
            link_is_hdr("app start");
            return;
        }
    }
    char text[160];
    std::snprintf(text, sizeof(text), "Not available: no video output found (log %s, ask: none of 0-4095)",
                  std::fopen("/download0/hui/dev/app.log", "r") ? "has no handle" : "unreadable");
    g.no_handle_text = text;
    hui::sys::log("[HDR] %s", text);
#endif
}

std::int32_t video_out_handle()
{
    return g.handle;
}

bool scanout_hdr()
{
    return g.hdr;
}

bool update(bool want)
{
    if (g.handle < 0)
    {
        set_status(g.no_handle_text);
        return false;
    }
    if (g.gl_failed)
    {
        set_status("Stopped: the HDR pass failed");
        want = false;
    }
    const std::int64_t now = hui::sys::monotonic_us();
    // PS5MC's rule: HDR only for an HDR10/HLG video AND a link that runs in HDR. Asked once, when the video starts.
#ifdef PLATFORM_PS5_NATIVE
    if (want && !g.want_prev)
        g.link_sdr = !link_is_hdr("video start");
#endif
    if (!want)
        g.link_sdr = false;
    g.want_prev = want;
    if (want && g.link_sdr)
    {
        want = false; // SDR link: the video is tone mapped as before
        set_status("Off: the display link is SDR");
    }
    if (want && !g.hdr && g.failures < 2)
    {
        int rc = 0;
        if (switch_scanout(true, &rc))
        {
            g.hdr = true;
            g.confirmed = false;
            g.switched_at_us = now;
            set_status("Switching to HDR");
        }
        else
        {
            ++g.failures;
            char text[96];
            std::snprintf(text, sizeof(text), "Refused by the console (code %#x)", static_cast<unsigned>(rc));
            set_status(text);
        }
    }
    else if (!want && g.hdr)
    {
        int rc = 0;
        if (switch_scanout(false, &rc))
        {
            g.hdr = false;
            set_status("Off");
        }
    }
    else if (!want && !g.hdr && g.failures == 0 && !g.gl_failed && !g.link_sdr)
    {
        // (not while an HDR video plays on an SDR link: "Off: the display link is SDR" stays, and the status does not change
        // every frame)
        set_status("Off");
    }
    // A moment after the switch: what does the display say?
#ifdef PLATFORM_PS5_NATIVE
    if (g.hdr && !g.confirmed && now - g.switched_at_us > 1500000)
    {
        g.confirmed = true;
        OutputStatus st{};
        if (read_status(g.handle, &st) && (st.dynamic_range == 2 || (st.flags & 1)))
            set_status("On: the TV is in HDR");
        else
        {
            char text[112];
            std::snprintf(text, sizeof(text), "Switched, but the TV reports SDR (range %u)", st.dynamic_range);
            set_status(text);
        }
    }
#endif
    return g.hdr;
}

const char *status()
{
    return g.status.c_str();
}

unsigned status_version()
{
    return g.status_version;
}

bool video_upload(std::uint32_t *texture, int *tex_w, int *tex_h, int w, int h, const void *words)
{
    if (*texture == 0 || *tex_w != w || *tex_h != h)
    {
        if (*texture != 0)
        {
            const GLuint old = *texture;
            glDeleteTextures(1, &old);
        }
        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB10_A2, w, h, 0, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, words);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        const GLenum err = glGetError();
        if (err != GL_NO_ERROR)
        {
            hui::sys::log("[HDR] the 10-bit video texture %dx%d failed (GL error %#x)", w, h, static_cast<unsigned>(err));
            glDeleteTextures(1, &tex);
            *texture = 0;
            return false;
        }
        *texture = tex;
        *tex_w = w;
        *tex_h = h;
        hui::sys::log("[HDR] 10-bit video texture %dx%d", w, h);
        return true;
    }
    glBindTexture(GL_TEXTURE_2D, *texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, words);
    return true;
}

namespace
{
bool make_rect_texture(GLuint *tex, GLenum internal_format, GLenum format, int w, int h, const void *data)
{
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_RECTANGLE, t);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_RECTANGLE, 0, internal_format, w, h, 0, format, GL_UNSIGNED_SHORT, data);
    glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    const GLenum err = glGetError();
    if (err != GL_NO_ERROR)
    {
        hui::sys::log("[HDR] a %dx%d plane texture failed (GL error %#x)", w, h, static_cast<unsigned>(err));
        glDeleteTextures(1, &t);
        return false;
    }
    *tex = t;
    return true;
}
} // namespace

bool planes_upload(VideoPlanes *planes, int w, int h, const void *y16, const void *uv16)
{
    if (planes->y == 0 || planes->w != w || planes->h != h)
    {
        if (planes->y != 0)
        {
            const GLuint old[2] = {planes->y, planes->uv};
            glDeleteTextures(2, old);
            planes->y = planes->uv = 0;
        }
        GLuint ty = 0, tuv = 0;
        if (!make_rect_texture(&ty, GL_R16, GL_RED, w, h, y16))
            return false;
        if (!make_rect_texture(&tuv, GL_RG16, GL_RG, w / 2, h / 2, uv16))
        {
            glDeleteTextures(1, &ty);
            return false;
        }
        planes->y = ty;
        planes->uv = tuv;
        planes->w = w;
        planes->h = h;
        hui::sys::log("[HDR] video planes %dx%d as rectangle textures", w, h);
        return true;
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_RECTANGLE, planes->y);
    glTexSubImage2D(GL_TEXTURE_RECTANGLE, 0, 0, 0, w, h, GL_RED, GL_UNSIGNED_SHORT, y16);
    glBindTexture(GL_TEXTURE_RECTANGLE, planes->uv);
    glTexSubImage2D(GL_TEXTURE_RECTANGLE, 0, 0, 0, w / 2, h / 2, GL_RG, GL_UNSIGNED_SHORT, uv16);
    return true;
}

void sdr_release(SdrTarget *t)
{
    if (t->fbo != 0)
        glDeleteFramebuffers(1, &t->fbo);
    if (t->texture != 0)
        glDeleteTextures(1, &t->texture);
    *t = SdrTarget();
}

bool sdr_convert(SdrTarget *t, const VideoPlanes &planes, int out_w, int out_h, bool bt601)
{
    if (planes.y == 0 || out_w <= 0 || out_h <= 0 || !ensure_sdr_program())
        return false;
    if (t->fbo == 0 || t->w != out_w || t->h != out_h)
    {
        sdr_release(t);
        glGenTextures(1, &t->texture);
        glBindTexture(GL_TEXTURE_2D, t->texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, out_w, out_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &t->fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->texture, 0);
        const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (status != GL_FRAMEBUFFER_COMPLETE)
        {
            hui::sys::log("[SDR] the video target %dx%d is incomplete (%#x)", out_w, out_h, static_cast<unsigned>(status));
            sdr_release(t);
            return false;
        }
        t->w = out_w;
        t->h = out_h;
        hui::sys::log("[SDR] 10-bit pictures are converted on the GPU: %dx%d planes to %dx%d", planes.w, planes.h, out_w, out_h);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glViewport(0, 0, out_w, out_h);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(g.sdr_program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_RECTANGLE, planes.y);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_RECTANGLE, planes.uv);
    glUniform1i(0, 0);
    glUniform1i(1, 1);
    glUniform4f(2, static_cast<float>(planes.w), static_cast<float>(planes.h), planes.high_aligned ? 65535.0f / 64.0f : 65535.0f,
                bt601 ? 1.0f : 0.0f);
    glUniform2f(3, static_cast<float>(out_w), static_cast<float>(out_h));
    glBindVertexArray(g.vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glActiveTexture(GL_TEXTURE0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
}

bool present(hui::gfx::Renderer &renderer, int width, int height, std::uint32_t video, const VideoPlanes *planes,
             const hui::gfx::Rect &video_rect, std::uint32_t target_framebuffer)
{
    if (!ensure_program() || !ensure_target(width, height))
    {
        g.gl_failed = true;
        return false;
    }
    // The interface into a transparent target (colour comes out premultiplied, alpha is the coverage)...
    glBindFramebuffer(GL_FRAMEBUFFER, g.ui_fbo);
    glViewport(0, 0, width, height);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    renderer.present(g.ui_fbo, width, height);

    // ...then the video and the interface into the scan-out as 2:10:10:10 words.
    const hui::gfx::Viewport vp = hui::gfx::fit_viewport(width, height);
    glBindFramebuffer(GL_FRAMEBUFFER, target_framebuffer);
    glViewport(0, 0, width, height);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DITHER); // the bytes must land exactly as computed
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(g.program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g.ui_tex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, video != 0 ? video : g.ui_tex);
    const bool have_planes = planes != nullptr && planes->y != 0;
    if (have_planes)
    {
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_RECTANGLE, planes->y);
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_RECTANGLE, planes->uv);
        // a texel is a 16-bit word: low-aligned 10-bit values (0-1023) read back as value / 65535; high-aligned ones
        // (value << 6) as value / 1023
        glUniform4f(7, static_cast<float>(planes->w), static_cast<float>(planes->h),
                    planes->high_aligned ? 65535.0f / 64.0f : 65535.0f, planes->hlg ? 1.0f : 0.0f);
    }
    glUniform1i(0, 0);
    glUniform1i(1, 1);
    glUniform1i(5, 2);
    glUniform1i(6, 3);
    glUniform4f(2, vp.offset_x + video_rect.x * vp.scale, vp.offset_y + video_rect.y * vp.scale, video_rect.w * vp.scale,
                video_rect.h * vp.scale);
    glUniform2f(3, static_cast<float>(width), static_cast<float>(height));
    glUniform1f(4, have_planes ? 2.0f : video != 0 ? 1.0f : 0.0f);
    glBindVertexArray(g.vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glEnable(GL_DITHER);
    return true;
}

} // namespace sx::hdr
