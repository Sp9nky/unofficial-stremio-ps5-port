#define STB_TRUETYPE_IMPLEMENTATION
#include "dynamic_font.hpp"

// MODIFIED for Stremio for PS5: Thai mark positioning tables. Included at
// global scope so ::thai_anchors matches the forward declaration in
// third_party/hui/src/gfx/font.hpp (an include inside namespace sx would
// create a distinct sx::thai_anchors type and break set_thai_anchors).
#include "thai_anchors.hpp"

#include "bidi.h"
#include "core/save_file.hpp"

#include <algorithm>
#include <cstdio>

namespace sx
{

bool TtfChain::load(const std::string &font_dir, const std::vector<FaceSpec> &specs)
{
    faces_.clear();
    thai_face_ = -1;
    for (const FaceSpec &spec : specs)
    {
        auto face = std::make_unique<Face>();
        if (!hui::save::read_file(font_dir + "/" + spec.file, &face->data))
        {
            std::fprintf(stderr, "font: cannot read %s\n", spec.file.c_str());
            if (faces_.empty())
                return false;
            continue;
        }
        const unsigned char *bytes = reinterpret_cast<const unsigned char *>(face->data.data());
        if (!stbtt_InitFont(&face->info, bytes, stbtt_GetFontOffsetForIndex(bytes, 0)))
        {
            std::fprintf(stderr, "font: cannot parse %s\n", spec.file.c_str());
            if (faces_.empty())
                return false;
            continue;
        }
        face->size_scale = spec.size_scale;
        // MODIFIED for Stremio for PS5: remember which face is Thai, so
        // glyph_by_index() rasterizes variants from the right face.
        if (thai_face_ < 0 && spec.file.find("Thai") != std::string::npos)
            thai_face_ = static_cast<int>(faces_.size());
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

// MODIFIED for Stremio for PS5: rasterizes from the Thai face (identified at
// load time), where the .small/.narrow variant glyphs live.
bool TtfChain::glyph_by_index(int glyph_index, hui::gfx::GlyphBitmap &out)
{
    if (thai_face_ < 0 || thai_face_ >= static_cast<int>(faces_.size()) || glyph_index <= 0)
        return false;
    const std::unique_ptr<Face> &face = faces_[static_cast<std::size_t>(thai_face_)];
    const float scale = stbtt_ScaleForMappingEmToPixels(&face->info, pixel_size_ * face->size_scale);
    int advance = 0, bearing = 0;
    stbtt_GetGlyphHMetrics(&face->info, glyph_index, &advance, &bearing);
    out = {};
    out.advance = 0.0f; // variants are combining marks: no advance
    int w = 0, h = 0, xoff = 0, yoff = 0;
    unsigned char *bitmap = stbtt_GetGlyphSDF(&face->info, scale, glyph_index, static_cast<int>(range_), 128,
                                              128.0f / range_, &w, &h, &xoff, &yoff);
    if (bitmap == nullptr)
        return false;
    out.pixels.assign(bitmap, bitmap + static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    out.w = w;
    out.h = h;
    out.offset_x = static_cast<float>(xoff);
    out.offset_y = static_cast<float>(yoff);
    stbtt_FreeSDF(bitmap, nullptr);
    return true;
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
    for (std::uint32_t c = 0xe00; c <= 0xe7f; ++c)
        font.has_glyph(c);
}

bool make(hui::gfx::Renderer &renderer, hui::gfx::Font *font, TtfChain *chain, const std::string &dir,
          const std::vector<FaceSpec> &faces, hui::ui::FontRef *ref)
{
    if (!chain->load(dir, faces))
        return false;
    float ascent = 0, descent = 0, gap = 0;
    chain->metrics(&ascent, &descent, &gap);
    font->init_dynamic(chain, chain->pixel_size(), chain->sdf_range(), ascent, descent, gap, 2048);
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

namespace
{

// Attaches the baked positioning tables and registers .small/.narrow variant
// glyphs under private-use codepoints (U+E000.. for .small tones, U+E010.. for
// .narrow marks, in table order) so Font::layout() can find() them.
void register_thai_variants(hui::gfx::Font *font, const thai_anchors::ThaiAnchorSet *set)
{
    font->set_thai_anchors(set);
    // .small tones: one per tone codepoint (0x0E48..0x0E4B).
    for (std::uint32_t tone = 0x0E48; tone <= 0x0E4B; ++tone)
    {
        int glyph = -1;
        for (int i = 0; i < set->n_stacked; ++i)
            if (set->stacked[i].tone_cp == tone)
            {
                glyph = set->stacked[i].small_glyph;
                break;
            }
        if (glyph > 0)
            font->add_thai_variant(0xE000u + (tone - 0x0E48u), glyph);
    }
    // .narrow marks: in table order.
    for (int i = 0; i < set->n_narrow; ++i)
        font->add_thai_variant(0xE010u + static_cast<std::uint32_t>(i), set->narrow[i].glyph);
}

} // namespace

bool load_ui_fonts(hui::gfx::Renderer &renderer, const std::string &font_dir, const std::string &baked_dir,
                   UiFonts *out, hui::ui::Fonts *fonts)
{
    // Noto Sans and Naskh are drawn a little larger than Inter's em so the
    // three scripts sit at the same visual size in a line. Noto Sans Thai
    // matches Noto Sans metrics, so it needs no rescale. The Thai faces must
    // exist in the fonts folder (app/fonts) or Thai text falls back to '?'.
    const bool ok =
        make(renderer, &out->regular, &out->regular_chain, font_dir,
             {{"Inter-Regular.ttf"},
              {"NotoSans-Regular.ttf"},
              {"NotoSansThai-Regular.ttf"},
              {"NotoNaskhArabicUI-Regular.ttf", 1.12f}},
             &fonts->regular) &&
        make(renderer, &out->semibold, &out->semibold_chain, font_dir,
             {{"Inter-SemiBold.ttf"},
              {"NotoSans-Bold.ttf"},
              {"NotoSansThai-Bold.ttf"},
              {"NotoNaskhArabicUI-Bold.ttf", 1.12f}},
             &fonts->semibold) &&
        make(renderer, &out->display, &out->display_chain, font_dir,
             {{"Montserrat-Medium.ttf"},
              {"Inter-SemiBold.ttf"},
              {"NotoSans-Bold.ttf"},
              {"NotoSansThai-Bold.ttf"},
              {"NotoNaskhArabicUI-Bold.ttf", 1.12f}},
             &fonts->display);
    if (!ok)
        return false;
    // MODIFIED for Stremio for PS5: Thai mark positioning. Attach the baked
    // GPOS/HarfBuzz tables and register the .small/.narrow variant glyphs
    // under private-use codepoints so layout() can find() them.
    register_thai_variants(&out->regular, &thai_anchors::kRegular);
    register_thai_variants(&out->semibold, &thai_anchors::kBold);
    register_thai_variants(&out->display, &thai_anchors::kBold);
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
