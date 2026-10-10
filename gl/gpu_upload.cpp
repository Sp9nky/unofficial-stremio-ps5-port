#include "gpu_upload.hpp"

#include "gfx/gl_program.hpp"
#include "platform/ps5/system.hpp"

namespace sx::gpu
{

namespace
{

constexpr const char *kVertex = R"(
void main()
{
    vec2 p = vec2[3](vec2(0.0, 0.0), vec2(2.0, 0.0), vec2(0.0, 2.0))[gl_VertexID];
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// Texel for texel: the fragment's centre is the source texel's centre (rectangle textures take texel coordinates),
// sampled without filtering. Row 0 of the target is row 0 of the source.
constexpr const char *kFragment = R"(
layout(location = 0) uniform sampler2DRect u_src;
out vec4 frag_color;

void main()
{
    frag_color = texture(u_src, gl_FragCoord.xy);
}
)";

struct State
{
    GLuint program = 0, vao = 0, fbo = 0;
    bool failed = false; // the driver refused once: the usual uploads from then on
} g;

bool ensure()
{
    if (g.failed)
        return false;
    if (g.program != 0)
        return true;
    g.program = hui::gfx::build_program("picture-copy", kVertex, kFragment);
    if (g.program == 0)
    {
        g.failed = true;
        return false;
    }
    glGenVertexArrays(1, &g.vao);
    glGenFramebuffers(1, &g.fbo);
    return true;
}

} // namespace

void prepare()
{
    ensure();
}

std::uint32_t texture_from_rgba(int width, int height, const unsigned char *rgba)
{
    if (width <= 0 || height <= 0 || rgba == nullptr || !ensure())
        return 0;
    while (glGetError() != GL_NO_ERROR)
    {
    }
    // The picture, linear
    GLuint src = 0;
    glGenTextures(1, &src);
    glBindTexture(GL_TEXTURE_RECTANGLE, src);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_RECTANGLE, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // The texture the interface draws, as the toolkit makes them (GlBatch::create_texture), filled by the GPU
    GLuint dst = 0;
    glGenTextures(1, &dst);
    glBindTexture(GL_TEXTURE_2D, dst);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_FRAMEBUFFER, g.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE || glGetError() != GL_NO_ERROR)
    {
        hui::sys::log("[ART] the GPU copy of a %dx%d picture was refused: uploading pictures directly", width, height);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteTextures(1, &src);
        glDeleteTextures(1, &dst);
        g.failed = true;
        return 0;
    }
    glViewport(0, 0, width, height);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(g.program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_RECTANGLE, src);
    glUniform1i(0, 0);
    glBindVertexArray(g.vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteTextures(1, &src); // the GL keeps it until the copy has run
    return dst;
}

} // namespace sx::gpu
