#define STB_TRUETYPE_IMPLEMENTATION
#include "dynamic_font.hpp"

#include "bidi.h"
#include "core/save_file.hpp"

#include <algorithm>
#include <cstdio>
#include <map>

namespace sx
{

namespace
{

// A font file is read once, however many chains use it (the CJK faces are several megabytes).
std::shared_ptr<const std::string> read_font_file(const std::string &path)
{
    static std::map<std::string, std::weak_ptr<const std::string>> cache;
    if (const auto found = cache[path].lock())
        return found;
    auto data = std::make_shared<std::string>();
    if (!hui::save::read_file(path, data.get()))
        return nullptr;
    cache[path] = data;
    return data;
}

} // namespace

bool TtfChain::load(const std::string &font_dir, const std::vector<FaceSpec> &specs)
{
    faces_.clear();
    for (const FaceSpec &spec : specs)
    {
        auto face = std::make_unique<Face>();
        face->data = read_font_file(font_dir + "/" + spec.file);
        if (face->data == nullptr)
        {
            std::fprintf(stderr, "font: cannot read %s\n", spec.file.c_str());
            if (faces_.empty())
                return false;
            continue;
        }
        const unsigned char *bytes = reinterpret_cast<const unsigned char *>(face->data->data());
        if (!stbtt_InitFont(&face->info, bytes, stbtt_GetFontOffsetForIndex(bytes, 0)))
        {
            std::fprintf(stderr, "font: cannot parse %s\n", spec.file.c_str());
            if (faces_.empty())
                return false;
            continue;
        }
        face->size_scale = spec.size_scale;
        faces_.push_back(std::move(face));
    }
    return !faces_.empty();
}

void TtfChain::metrics(float *ascent, float *descent, float *line_gap) const
{
    int a = 0, d = 0, g = 0;
    const Face &face = *faces_.front();
    stbtt_GetFontVMetrics(&face.info, &a, &d, &g);
    const float scale = stbtt_ScaleForMappingEmToPixels(&face.info, pixel_size_ * face.size_scale);
    *ascent = static_cast<float>(a) * scale;
    *descent = static_cast<float>(d) * scale;
    *line_gap = static_cast<float>(g) * scale;
}

bool TtfChain::glyph(std::uint32_t codepoint, hui::gfx::GlyphBitmap &out)
{
    // Marks that take no room: joiners, direction marks, variation selectors.
    if ((codepoint >= 0x200b && codepoint <= 0x200f) || (codepoint >= 0x202a && codepoint <= 0x202e) ||
        (codepoint >= 0x2060 && codepoint <= 0x2064) || (codepoint >= 0xfe00 && codepoint <= 0xfe0f) ||
        codepoint == 0xfeff)
    {
        out = {};
        return true;
    }
    if (codepoint < 32)
        return false;
    for (const std::unique_ptr<Face> &face : faces_)
    {
        const int index = stbtt_FindGlyphIndex(&face->info, static_cast<int>(codepoint));
        if (index == 0 && codepoint != ' ' && codepoint != 0xa0)
            continue;
        const float scale = stbtt_ScaleForMappingEmToPixels(&face->info, pixel_size_ * face->size_scale);
        int advance = 0;
        int bearing = 0;
        stbtt_GetGlyphHMetrics(&face->info, index, &advance, &bearing);
        out = {};
        out.advance = static_cast<float>(advance) * scale;
        int w = 0, h = 0, xoff = 0, yoff = 0;
        unsigned char *bitmap = stbtt_GetGlyphSDF(&face->info, scale, index, static_cast<int>(range_), 128,
                                                  128.0f / range_, &w, &h, &xoff, &yoff);
        if (bitmap != nullptr)
        {
            out.pixels.assign(bitmap, bitmap + static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
            out.w = w;
            out.h = h;
            out.offset_x = static_cast<float>(xoff);
            out.offset_y = static_cast<float>(yoff);
            stbtt_FreeSDF(bitmap, nullptr);
        }
        return true;
    }
    return false;
}

namespace
{

std::string shape_text(std::string_view text)
{
    return bidi_visual(std::string(text));
}

// Draws the glyphs most text uses now, so the first screen doesn't stall.
void prewarm(const hui::gfx::Font &font)
{
    for (std::uint32_t c = 0x20; c <= 0x17f; ++c)
        font.has_glyph(c);
    for (std::uint32_t c = 0x2010; c <= 0x2026; ++c)
        font.has_glyph(c);
    for (std::uint32_t c = 0x386; c <= 0x3ce; ++c)
        font.has_glyph(c);
    for (std::uint32_t c = 0x400; c <= 0x45f; ++c)
        font.has_glyph(c);
}

bool make(hui::gfx::Renderer &renderer, hui::gfx::Font *font, TtfChain *chain, const std::string &dir,
          const std::vector<FaceSpec> &faces, hui::ui::FontRef *ref, int atlas_size)
{
    if (!chain->load(dir, faces))
        return false;
    float ascent = 0, descent = 0, gap = 0;
    chain->metrics(&ascent, &descent, &gap);
    font->init_dynamic(chain, chain->pixel_size(), chain->sdf_range(), ascent, descent, gap, atlas_size);
    font->set_text_transform(&shape_text);
    prewarm(*font);
    ref->font = font;
    ref->texture = renderer.batch().create_font_texture(*font);
    int first = 0, last = 0;
    font->take_dirty(&first, &last); // the whole atlas was just uploaded
    return true;
}

bool load_baked(hui::gfx::Renderer &renderer, const std::string &path, hui::gfx::Font *font, hui::ui::FontRef *ref)
{
    std::string data;
    if (!hui::save::read_file(path, &data) || !font->load(data))
        return false;
    ref->font = font;
    ref->texture = renderer.batch().create_font_texture(*font);
    return true;
}

} // namespace

bool load_ui_fonts(hui::gfx::Renderer &renderer, const std::string &font_dir, const std::string &baked_dir,
                   UiFonts *out, hui::ui::Fonts *fonts)
{
    // Noto Sans, Naskh and the others are drawn a little larger or smaller than Inter's em so the scripts sit at the
    // same visual size in a line. Hebrew, Thai and Chinese/Japanese/Korean come last: Inter and Noto Sans answer
    // for their letters first. The CJK faces are one weight (Medium) for all three chains; where Japanese and
    // Chinese share a character, the Japanese form is drawn.
    const auto extra = [](bool bold) {
        return std::vector<FaceSpec>{{"NotoNaskhArabicUI-" + std::string(bold ? "Bold" : "Regular") + ".ttf", 1.12f},
                                     {"NotoSansHebrew-" + std::string(bold ? "Bold" : "Regular") + ".ttf"},
                                     {"NotoSansThai-" + std::string(bold ? "Bold" : "Regular") + ".ttf", 1.08f},
                                     {"NotoSansJP-Subset.ttf", 0.94f},
                                     {"NotoSansSC-Subset.ttf", 0.94f},
                                     {"NotoSansTC-Subset.ttf", 0.94f},
                                     {"NotoSansKR-Subset.ttf", 0.94f}};
    };
    const auto chain = [&](std::vector<FaceSpec> head, bool bold) {
        const std::vector<FaceSpec> tail = extra(bold);
        head.insert(head.end(), tail.begin(), tail.end());
        return head;
    };
    // The atlases start empty and fill with the glyphs a screen needs; a film in Japanese uses thousands, so the
    // semibold one (subtitles) is the largest. A full atlas starts over (hui font.cpp).
    const bool ok =
        make(renderer, &out->regular, &out->regular_chain, font_dir,
             chain({{"Inter-Regular.ttf"}, {"NotoSans-Regular.ttf"}}, false), &fonts->regular, 2048) &&
        make(renderer, &out->semibold, &out->semibold_chain, font_dir,
             chain({{"Inter-SemiBold.ttf"}, {"NotoSans-Bold.ttf"}}, true), &fonts->semibold, 4096) &&
        make(renderer, &out->display, &out->display_chain, font_dir,
             chain({{"Montserrat-Medium.ttf"}, {"Inter-SemiBold.ttf"}, {"NotoSans-Bold.ttf"}}, true), &fonts->display,
             2048);
    if (!ok)
        return false;
    // Only ASCII is ever drawn with these.
    if (!load_baked(renderer, baked_dir + "/dejavu-sans-mono.huifont", &out->mono, &fonts->mono))
        fonts->mono = fonts->semibold;
    fonts->pixel = fonts->semibold;
    fonts->hand = fonts->semibold;
    return true;
}

void sync_fonts(hui::gfx::Renderer &renderer, const UiFonts &fonts, const hui::ui::Fonts &refs)
{
    renderer.batch().update_font_texture(refs.regular.texture, fonts.regular);
    renderer.batch().update_font_texture(refs.semibold.texture, fonts.semibold);
    renderer.batch().update_font_texture(refs.display.texture, fonts.display);
}

} // namespace sx
