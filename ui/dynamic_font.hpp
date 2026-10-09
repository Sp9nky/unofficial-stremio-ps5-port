// The interface's fonts: signed-distance-field glyphs made from real font
// files the first time a text needs them, instead of the toolkit's baked
// ASCII-only atlases. Titles, names and subtitles come from addons in any
// language, so a face is a chain: the first file that has a character draws it.
//
//   regular   Inter Regular   -> Noto Sans -> Noto Sans Thai -> Noto Naskh Arabic UI
//   semibold  Inter SemiBold  -> Noto Sans Bold -> Noto Sans Thai Bold -> Noto Naskh Arabic UI Bold
//   display   Montserrat      -> Inter SemiBold -> Noto Sans Bold -> Noto Sans Thai Bold -> Naskh Bold
//
// Right-to-left text (Arabic, Hebrew) is reordered and its letters joined by
// FriBidi (src/bidi.cpp) before it is measured or drawn.

#pragma once

#include "gfx/font.hpp"
#include "gfx/renderer.hpp"
#include "ui/fonts.hpp"

#include <memory>
#include <string>
#include <vector>

#include "stb_truetype.h"

namespace sx
{

struct FaceSpec
{
    std::string file;      // under the font folder
    float size_scale = 1.0f; // makes a fallback face match the primary's size
};

class TtfChain : public hui::gfx::GlyphSource
{
  public:
    TtfChain(float pixel_size = 56.0f, float sdf_range = 8.0f) : pixel_size_(pixel_size), range_(sdf_range)
    {
    }

    // Loads the faces in priority order; false if the first one can't be read.
    bool load(const std::string &font_dir, const std::vector<FaceSpec> &faces);
    bool glyph(std::uint32_t codepoint, hui::gfx::GlyphBitmap &out) override;
    // MODIFIED for Stremio for PS5: rasterize a glyph by index from the Thai
    // face (for .small/.narrow variant substitution).
    bool glyph_by_index(int glyph_index, hui::gfx::GlyphBitmap &out) override;

    float pixel_size() const
    {
        return pixel_size_;
    }
    float sdf_range() const
    {
        return range_;
    }
    // Vertical metrics of the first face at pixel_size().
    void metrics(float *ascent, float *descent, float *line_gap) const;

  private:
    struct Face
    {
        std::string data;
        stbtt_fontinfo info{};
        float size_scale = 1.0f;
    };
    std::vector<std::unique_ptr<Face>> faces_;
    // MODIFIED for Stremio for PS5: index of the Thai face in faces_ (-1 if
    // none). Variant glyphs (.small/.narrow) are rasterized from this face.
    int thai_face_ = -1;
    float pixel_size_;
    float range_;
};

// The three faces the screens use, with the textures they are drawn from.
struct UiFonts
{
    TtfChain regular_chain, semibold_chain, display_chain;
    hui::gfx::Font regular, semibold, display;
    // Pre-baked faces for what is only ever ASCII.
    hui::gfx::Font mono, pixel, hand;
};

// Loads the chains from `font_dir` (the app's fonts folder) and uploads their
// atlases. `baked_dir` holds the toolkit's .huifont files for mono, pixel and
// hand. Fills `fonts` for the screens.
bool load_ui_fonts(hui::gfx::Renderer &renderer, const std::string &font_dir, const std::string &baked_dir,
                   UiFonts *out, hui::ui::Fonts *fonts);

// Copies glyphs made since the last call to the GPU. Call every frame, after
// the screens have recorded their text and before the frame is presented.
void sync_fonts(hui::gfx::Renderer &renderer, const UiFonts &fonts, const hui::ui::Fonts &refs);

} // namespace sx
