// The frame every page shares: the left rail, the top bar and small pieces
// (hint chips, rating badges).

#pragma once

#include "content.hpp"
#include "gfx/draw_list.hpp"
#include "theme.hpp"
#include "ui/fonts.hpp"
#include "ui/glyphs.hpp"
#include "ui/motion.hpp"

#include <cstdint>
#include <string>

namespace sx
{

// Pictures the interface draws, uploaded once by whoever owns the GL context.
struct Icons
{
    std::uint32_t nav_solid[5] = {}; // the page you are on
    std::uint32_t nav_line[5] = {};  // the others
    std::uint32_t search = 0;
    std::uint32_t logo = 0;
    std::uint32_t name = 0;    // "Stremio" as the boot screen shows it, a picture (it is drawn before the fonts exist)
    std::uint32_t logo_xl = 0; // the boot screen's mark, 144 virtual px at its own size
};

// What every screen needs to draw.
struct Env
{
    const hui::ui::Fonts &fonts;
    const Icons &icons;
    std::string clock = "21:45";
};

// The left navigation: five pages. When it holds the focus a
// violet pill glides to the item under the cursor, as Stremio's menu does.
class Rail
{
  public:
    Rail();

    void set_page(Page page);
    void focus(bool on);
    bool focused() const
    {
        return focused_;
    }
    int cursor() const
    {
        return cursor_;
    }
    void move(int delta); // up/down while focused
    // A page opened: the rail lets go of the focus and rests on that page, with no glide.
    void reset(Page page);
    void update(float dt);
    void draw(hui::gfx::DrawList &list, const Env &env, float age) const;

  private:
    static hui::gfx::Rect item_rect(int index);

    int page_ = 0;
    int cursor_ = 0;
    bool focused_ = false;
    hui::ui::SpringRect pill_;
    hui::tween::Spring glow_; // 1 while focused, eases out
};

// The search pill and the clock. search_focused lights the pill.
// With `query`, the pill shows what has been typed (and a blinking caret) instead of the hint.
// The Stremio logo at the top left (draw_topbar has it; screens without a top bar call this).
void draw_logo(hui::gfx::DrawList &list, const Env &env);

void draw_topbar(hui::gfx::DrawList &list, const Env &env, bool search_focused, float age,
                 const std::string *query = nullptr, float clock = 0.0f);

// A controller glyph with its label in a soft pill: "(X) Open".
// Returns the width, so chips can be placed in a row. max_text (0 = none)
// shortens a long label with an ellipsis.
float draw_chip(hui::gfx::DrawList &list, const Env &env, hui::ui::Button button, const char *label,
                float x, float cy, bool strong = false, float max_text = 0.0f);

// The yellow IMDb badge followed by the rating. Returns the width.
float draw_imdb(hui::gfx::DrawList &list, const Env &env, const std::string &rating, float x,
                float baseline);

// A fade from the page colour, for the places content scrolls under chrome.
// `solid` pixels at the top are covered completely (at `strength`) before the fade starts.
void fade_down(hui::gfx::DrawList &list, float x, float y, float w, float h, float strength, float solid = 0.0f);
void fade_up(hui::gfx::DrawList &list, float x, float y, float w, float h, float strength);

} // namespace sx
