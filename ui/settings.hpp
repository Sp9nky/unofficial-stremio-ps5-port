// Settings: a list of choices and switches, and beside it what the focused one does.

#pragma once

#include "cards.hpp"
#include "page.hpp"

#include <vector>

namespace sx
{

class Settings : public PageBase
{
  public:
    explicit Settings(const Env &env) : PageBase(env, Page::settings)
    {
    }

    void set_content(const SettingsContent *content);
    void refresh(); // a value changed in place: the cursor stays
    void update(const hui::InputFrame &input, float dt);
    void draw(hui::gfx::DrawList &list, hui::gfx::BackdropSpec &backdrop) const;
    // The row under the cursor, or -1 while the rail has the focus.
    int cursor_row() const
    {
        return rail_.focused() ? -1 : index_;
    }

  private:
    hui::gfx::Rect row_rect(int index, bool final_position) const;
    void draw_help(hui::gfx::DrawList &list, const SettingRow &row) const;

    const SettingsContent *content_ = nullptr;
    int index_ = 0;
    hui::ui::Scroller scroll_;
    hui::ui::SpringRect ring_;
    hui::tween::Spring focus_on_;
    std::vector<hui::tween::Spring> switches_; // 0..1 per row, so switches slide
    int shown_ = -1;
    hui::tween::Timer swap_;
};

} // namespace sx
