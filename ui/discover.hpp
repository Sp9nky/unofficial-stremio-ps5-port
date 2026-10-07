// Discover: pick a type, a catalogue and a genre, browse the posters, see the
// focused title on the right.

#pragma once

#include "cards.hpp"
#include "grid.hpp"
#include "page.hpp"

namespace sx
{

class Discover : public PageBase
{
  public:
    explicit Discover(const Env &env) : PageBase(env, Page::discover)
    {
    }

    void set_content(const DiscoverContent *content);
    void refresh() // more items were added: the cursor stays
    {
        grid_.sync();
    }
    int cursor_index() const
    {
        return grid_.index();
    }
    // The filter under the cursor, or -1 while it is on the posters or the rail.
    int filter_cursor() const
    {
        return in_filters_ && !rail_.focused() ? filter_ : -1;
    }
    void update(const hui::InputFrame &input, float dt);
    void draw(hui::gfx::DrawList &list, hui::gfx::BackdropSpec &backdrop) const;
    // The poster under the cursor, null while the cursor is on a filter or the rail.
    const Title *focused_title() const
    {
        return in_filters_ || rail_.focused() ? nullptr : grid_.focused();
    }

  private:
    void draw_preview(hui::gfx::DrawList &list, const Title &title) const;

    const DiscoverContent *content_ = nullptr;
    PosterGrid grid_;
    bool in_filters_ = false;
    int filter_ = 0;
    int shown_ = -1;
    hui::tween::Timer swap_;
};

} // namespace sx
