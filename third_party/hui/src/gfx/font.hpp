// ps5-homebrew-ui - Baked SDF font: loading, measuring and glyph layout.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "gfx/font_format.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace hui::gfx
{

// MODIFIED for Stremio for PS5 (the rest of this file is upstream): a font can
// also be filled at run time. A GlyphSource draws one glyph as a signed
// distance field the first time a text needs it, and the font packs it into
// its atlas; the GPU copy is then updated with take_dirty().
struct GlyphBitmap
{
    std::vector<std::uint8_t> pixels; // w * h, 128 on the glyph edge (see font_format.hpp)
    int w = 0;
    int h = 0;
    float offset_x = 0.0f; // quad top-left relative to the pen on the baseline, in atlas pixels
    float offset_y = 0.0f;
    float advance = 0.0f;
};

class GlyphSource
{
  public:
    virtual ~GlyphSource() = default;
    // False if no face has the character.
    virtual bool glyph(std::uint32_t codepoint, GlyphBitmap &out) = 0;
};

// One positioned glyph quad in output pixels, with its atlas UV rectangle.
struct GlyphQuad
{
    float x0, y0, x1, y1;
    float u0, v0, u1, v1;
};

enum class Align : std::uint8_t
{
    left,
    center,
    right,
};

class Font
{
  public:
    // Parses a .huifont blob; returns false (and keeps an error) if invalid.
    bool load(std::string_view data);
    const std::string &error() const
    {
        return error_;
    }

    // Width in pixels of one line of UTF-8 text at the given pixel size.
    // tracking is extra space between glyphs, in pixels.
    float measure(std::string_view text, float size, float tracking = 0.0f) const;
    // text, or its longest prefix plus an ellipsis that fits max_width.
    std::string fit(std::string_view text, float size, float max_width,
                    float tracking = 0.0f) const;
    float ascent(float size) const
    {
        return header_.ascent * size / header_.pixel_size;
    }
    float descent(float size) const
    {
        return -header_.descent * size / header_.pixel_size;
    }
    float line_height(float size) const
    {
        return (header_.ascent - header_.descent + header_.line_gap) * size / header_.pixel_size;
    }
    // Distance-field spread in output pixels at a size (for shader anti-aliasing).
    float sdf_range(float size) const
    {
        return header_.sdf_range * size / header_.pixel_size;
    }

    // Lays out one line with its baseline at y. x is the left edge, centre or
    // right edge depending on align. Appends to quads; returns the advance.
    float layout(std::string_view text, float x, float y, float size, Align align,
                 std::vector<GlyphQuad> &quads, float tracking = 0.0f) const;

    // Breaks text into lines no wider than max_width (at word boundaries).
    std::vector<std::string> wrap(std::string_view text, float size, float max_width) const;

    std::uint16_t atlas_width() const
    {
        return header_.atlas_width;
    }
    std::uint16_t atlas_height() const
    {
        return header_.atlas_height;
    }
    const std::vector<std::uint8_t> &atlas() const
    {
        return atlas_;
    }
    bool has_glyph(std::uint32_t codepoint) const
    {
        return find(codepoint) != nullptr;
    }

    // ---- MODIFIED for Stremio for PS5: glyphs made at run time ----
    // Starts an empty font of atlas_size x atlas_size that asks `source` for
    // glyphs as they are used. The metrics are those of the primary face, at
    // pixel_size.
    void init_dynamic(GlyphSource *source, float pixel_size, float sdf_range, float ascent, float descent,
                      float line_gap, int atlas_size);
    // Text goes through this before it is measured or laid out (right-to-left
    // reordering and Arabic joining); null leaves it alone.
    void set_text_transform(std::string (*transform)(std::string_view))
    {
        transform_ = transform;
    }
    // The atlas rows that changed since the last call, for updating the GPU
    // copy. False when nothing did.
    bool take_dirty(int *first_row, int *last_row) const;

  private:
    const font_format::Glyph *find(std::uint32_t codepoint) const;
    bool add_glyph(std::uint32_t codepoint) const;
    float kern(std::uint32_t first, std::uint32_t second) const;
    float measure_raw(std::string_view text, float size, float tracking) const;

    font_format::Header header_{};
    mutable std::vector<font_format::Glyph> glyphs_;
    std::vector<font_format::Kern> kerns_;
    mutable std::vector<std::uint8_t> atlas_;
    std::string error_;
    // Run-time glyphs.
    GlyphSource *source_ = nullptr;
    std::string (*transform_)(std::string_view) = nullptr;
    mutable std::unordered_set<std::uint32_t> missing_;
    mutable int pen_x_ = 1;
    mutable int pen_y_ = 1;
    mutable int shelf_ = 0;
    mutable int dirty_first_ = -1;
    mutable int dirty_last_ = -1;
};

// Decodes the next UTF-8 codepoint from text at *index (advancing it);
// invalid bytes decode as U+FFFD.
std::uint32_t next_codepoint(std::string_view text, std::size_t *index);

} // namespace hui::gfx
