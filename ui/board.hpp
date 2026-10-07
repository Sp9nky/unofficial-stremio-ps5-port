// The Board: Stremio's home page. A hero shows the title under the cursor,
// rows of posters below it (Continue Watching first), the left rail beside.

#pragma once

#include "chrome.hpp"
#include "content.hpp"
#include "core/input.hpp"
#include "gfx/backdrop_spec.hpp"

#include <vector>

namespace sx
{

class Board
{
  public:
    explicit Board(const Env &env) : env_(env)
    {
    }

    void set_content(const BoardContent *content);
    // The content changed in place (rows arrived, loaded more): keeps the cursor
    // on the same row, found again by its key, and the rows' scroll positions.
    void refresh();
    void enter(); // the page opened: restart the entrance
    void update(const hui::InputFrame &input, float dt);
    void draw(hui::gfx::DrawList &scene, hui::gfx::BackdropSpec &backdrop) const;

    // Search results: the same rows without the hero, with the query in the search pill.
    void set_search(const std::string *query);
    const Title *focused_title() const;
    bool rail_focused() const
    {
        return rail_.focused();
    }
    int rail_cursor() const
    {
        return rail_.cursor();
    }
    // The cursor, for loading the artwork around it.
    int cursor_row() const
    {
        return row_;
    }
    int cursor_col() const
    {
        return row_ < static_cast<int>(col_.size()) ? col_[row_] : 0;
    }

  private:
    struct Key
    {
        int row = -1;
        int col = -1;
        bool operator==(const Key &other) const
        {
            return row == other.row && col == other.col;
        }
    };

    hui::gfx::Rect card_rect(int row, int col, bool final_position) const;
    float row_top(int row, bool final_position) const;
    void draw_hero(hui::gfx::DrawList &list, float offset) const;
    void draw_row(hui::gfx::DrawList &list, int row, float top) const;

    const Env &env_;
    const BoardContent *content_ = nullptr;
    const std::string *query_ = nullptr; // set while this shows search results
    bool hero_ = true;
    Rail rail_;

    int row_ = 0;
    bool moved_ = false; // the player has used the cursor since the rows were set
    std::vector<std::string> keys_; // the rows' keys when the state below was made
    std::vector<int> col_;
    std::vector<std::vector<hui::tween::Spring>> scale_; // per card: the pop of the focused one
    std::vector<hui::ui::Scroller> across_;              // per row: horizontal scroll
    hui::ui::Scroller down_;                             // the rows beneath the hero
    hui::ui::SpringRect ring_;                           // the white ring around the focused card
    hui::tween::Spring focus_on_;                        // 1 while the content holds the focus

    Key shown_;            // what the hero shows
    Key before_;           // what it showed before, fading out
    hui::tween::Timer swap_; // the crossfade between the two
    float age_ = 0.0f;       // since enter(): the entrance
    float clock_ = 0.0f;
};

} // namespace sx
