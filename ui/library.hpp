// Library: everything the user has saved or watched, as a grid of posters.

#pragma once

#include "cards.hpp"
#include "grid.hpp"
#include "page.hpp"

namespace sx
{

class Library : public PageBase
{
  public:
    explicit Library(const Env &env) : PageBase(env, Page::library)
    {
    }

    void set_content(const LibraryContent *content);
    void refresh()
    {
        grid_.sync();
    }
    int cursor_index() const
    {
        return grid_.index();
    }
    int filter_cursor() const
    {
        return in_filters_ && !rail_.focused() ? filter_ : -1;
    }
    void update(const hui::InputFrame &input, float dt);
    void draw(hui::gfx::DrawList &list, hui::gfx::BackdropSpec &backdrop) const;
    const Title *focused_title() const
    {
        return in_filters_ || rail_.focused() ? nullptr : grid_.focused();
    }

  private:
    const LibraryContent *content_ = nullptr;
    PosterGrid grid_;
    bool in_filters_ = false;
    int filter_ = 0;
};

} // namespace sx
