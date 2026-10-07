// Pieces several pages share: the poster card, filter chips, switches, tags.

#pragma once

#include "chrome.hpp"
#include "content.hpp"

#include <string>

namespace sx
{

// A poster with its name beneath. `rect` is the poster; `scale` is the focus
// pop (1 = resting), `enter` the entrance (0..1). show_progress draws the
// resume bar and episode badge.
void draw_poster_card(hui::gfx::DrawList &list, const Env &env, const Title &title, const hui::gfx::Rect &rect,
                      float scale, float enter, bool focused, bool show_progress);

// "Type  Movie v": a drop-down choice. Returns its width.
// max_value (0 = none) cuts a long value short with an ellipsis.
float draw_filter_chip(hui::gfx::DrawList &list, const Env &env, const Filter &filter, float x, float cy,
                       bool focused, float max_value = 0.0f);

// How wide each chip's value may be so a row of chips fits in `avail`: short
// values keep their full text, the longest ones give way equally.
std::vector<float> filter_value_caps(const Env &env, const std::vector<Filter> &filters, float avail);

// A message where a list would be: "Loading...", "Nothing here." (up to three lines).
void draw_status(hui::gfx::DrawList &list, const Env &env, const std::string &message, float baseline);

// A small rounded label ("movie", "Local"). Returns its width.
float draw_tag(hui::gfx::DrawList &list, const Env &env, const std::string &text, float x, float cy,
               hui::gfx::Color fill, hui::gfx::Color ink);

// An on/off switch whose centre is (x, cy). `on` is 0..1 so it can slide.
void draw_switch(hui::gfx::DrawList &list, float x, float cy, float on);

// A chevron pointing down, right or left, centred at (cx, cy).
void draw_chevron(hui::gfx::DrawList &list, float cx, float cy, float size, int direction,
                  hui::gfx::Color colour);

} // namespace sx
