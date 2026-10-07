// Addons: the sources of catalogues, streams and subtitles, as tiles.

#pragma once

#include "cards.hpp"
#include "page.hpp"

#include <algorithm>

namespace sx
{

class Addons : public PageBase
{
  public:
    explicit Addons(const Env &env) : PageBase(env, Page::addons)
    {
    }

    void set_content(const AddonsContent *content);
    void refresh() // addons arrived or changed: the cursor stays
    {
        if (content_)
            index_ = index_ < static_cast<int>(content_->items.size()) ? index_ : std::max(0, static_cast<int>(content_->items.size()) - 1);
    }
    int cursor_index() const
    {
        return index_;
    }
    void update(const hui::InputFrame &input, float dt);
    void draw(hui::gfx::DrawList &list, hui::gfx::BackdropSpec &backdrop) const;

  private:
    hui::gfx::Rect tile_rect(int index, bool final_position) const;
    void draw_tile(hui::gfx::DrawList &list, const Addon &addon, const hui::gfx::Rect &rect, float enter,
                   bool focused) const;

    const AddonsContent *content_ = nullptr;
    int index_ = 0;
    hui::ui::Scroller scroll_;
    hui::ui::SpringRect ring_;
    hui::tween::Spring focus_on_;
};

} // namespace sx
