// What every page of the left rail has in common: the rail and its focus,
// the entrance clock, the violet page background.

#pragma once

#include "chrome.hpp"
#include "content.hpp"
#include "core/input.hpp"
#include "gfx/backdrop_spec.hpp"

namespace sx
{

// The page background: near-black with Stremio's violet light.
void apply_page_backdrop(hui::gfx::BackdropSpec &backdrop, float time);

class PageBase
{
  public:
    virtual ~PageBase() = default;

    // The page opened: its entrance restarts, and the cursor is on its content. The rail
    // is not left holding the focus it had when the page was last left.
    void enter()
    {
        age_ = 0.0f;
        rail_.reset(page_);
    }
    bool rail_focused() const
    {
        return rail_.focused();
    }
    // The page under the rail's cursor, as an index into Page.
    int rail_cursor() const
    {
        return rail_.cursor();
    }

  protected:
    PageBase(const Env &env, Page page) : env_(env), page_(page)
    {
        rail_.set_page(page);
    }

    // While the rail holds the focus it takes the D-pad: up/down choose a
    // page, right hands the focus back. True when it used `input`.
    bool rail_input(const hui::InputFrame &input)
    {
        if (!rail_.focused())
            return false;
        if (input.nav == hui::Direction::up)
            rail_.move(-1);
        else if (input.nav == hui::Direction::down)
            rail_.move(1);
        else if (input.nav == hui::Direction::right)
            rail_.focus(false);
        return true;
    }
    void to_rail()
    {
        rail_.focus(true);
    }
    void tick(float dt)
    {
        age_ += dt;
        clock_ += dt;
        rail_.set_page(page_);
        rail_.update(dt);
    }

    const Env &env_;
    Page page_;
    Rail rail_;
    float age_ = 0.0f;
    float clock_ = 0.0f;
};

} // namespace sx
