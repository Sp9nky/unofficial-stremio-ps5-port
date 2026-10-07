// A grid of posters that scrolls by rows, with the focus ring that glides
// between them. Discover and Library use it.

#pragma once

#include "chrome.hpp"
#include "content.hpp"
#include "core/input.hpp"

#include <vector>

namespace sx
{

struct GridLayout
{
    float x = theme::kLeft;      // left edge of the first column
    float y = 220.0f;            // top of the first row's posters
    float card_w = 232.0f;
    float card_h = 348.0f;
    float pitch_x = 262.0f;
    float pitch_y = 430.0f;      // a row: poster, then its name
    int cols = 6;
    float view_top = 190.0f;     // posters are clipped above this line
    float view_bottom = 1080.0f;
};

class PosterGrid
{
  public:
    enum class Edge
    {
        none,
        up,   // moved up out of the first row
        left, // moved left out of the first column
    };

    void set_items(const std::vector<Title> *items, const GridLayout &layout);
    // The items changed in place (more were added): keeps the cursor where it is.
    void sync();
    void snap();
    Edge move(hui::Direction direction);
    void update(float dt, bool has_focus);
    void draw(hui::gfx::DrawList &list, const Env &env, float age, bool show_progress, bool has_focus) const;

    int index() const
    {
        return index_;
    }
    const Title *focused() const;

  private:
    hui::gfx::Rect rect_at(int index, bool final_position) const;

    const std::vector<Title> *items_ = nullptr;
    GridLayout layout_;
    int index_ = 0;
    std::vector<hui::tween::Spring> pop_;
    hui::ui::Scroller scroll_;
    hui::ui::SpringRect ring_;
    hui::tween::Spring focus_on_;
};

} // namespace sx
