#include "settings.hpp"

#include <algorithm>

namespace sx
{

using hui::Direction;
using hui::gfx::Align;
using hui::gfx::Color;
using hui::gfx::Rect;
using hui::ui::text;
namespace tween = hui::tween;

namespace
{

constexpr float kListX = theme::kLeft;
constexpr float kListW = 1040.0f;
constexpr float kRowH = 90.0f;
constexpr float kPitch = 100.0f;
constexpr float kTop = 196.0f;
constexpr Rect kHelp{1290.0f, 196.0f, 570.0f, 640.0f};

} // namespace

Rect Settings::row_rect(int index, bool final_position) const
{
    const float scroll = final_position ? scroll_.position.target : scroll_.offset();
    return {kListX, kTop + static_cast<float>(index) * kPitch - scroll, kListW, kRowH};
}

void Settings::set_content(const SettingsContent *content)
{
    content_ = content;
    index_ = 0;
    shown_ = -1;
    switches_.assign(content ? content->rows.size() : 0, {});
    if (content)
        for (std::size_t i = 0; i < content->rows.size(); ++i)
            switches_[i].snap(content->rows[i].on ? 1.0f : 0.0f);
    scroll_.position.snap(0.0f);
    ring_.snap(row_rect(0, true));
    focus_on_.snap(1.0f);
}

void Settings::refresh()
{
    if (!content_)
        return;
    const std::size_t rows = content_->rows.size();
    const std::size_t before = switches_.size();
    switches_.resize(rows);
    for (std::size_t i = before; i < rows; ++i)
        switches_[i].snap(content_->rows[i].on ? 1.0f : 0.0f);
    index_ = std::min(index_, std::max(0, static_cast<int>(rows) - 1));
}

void Settings::update(const hui::InputFrame &input, float dt)
{
    tick(dt);
    if (!content_ || content_->rows.empty())
        return;
    const int count = static_cast<int>(content_->rows.size());
    if (!rail_input(input))
    {
        if (input.nav == Direction::up)
            index_ = std::max(0, index_ - 1);
        else if (input.nav == Direction::down)
            index_ = std::min(count - 1, index_ + 1);
        else if (input.nav == Direction::left)
            to_rail();
    }
    const float top = static_cast<float>(index_) * kPitch;
    scroll_.reveal(top, top + kRowH, 1080.0f - kTop - 60.0f, 14.0f);
    scroll_.update(dt, 18.0f);
    ring_.target(row_rect(index_, true));
    ring_.update(dt, 22.0f);
    focus_on_.target = rail_.focused() ? 0.0f : 1.0f;
    focus_on_.update(dt, 16.0f);
    for (std::size_t i = 0; i < switches_.size(); ++i)
    {
        switches_[i].target = content_->rows[i].on ? 1.0f : 0.0f;
        switches_[i].update(dt, 22.0f);
    }
    if (index_ != shown_)
    {
        shown_ = index_;
        swap_.start(0.28f);
    }
    swap_.update(dt);
}

void Settings::draw_help(hui::gfx::DrawList &list, const SettingRow &row) const
{
    const float in = tween::smoothstep(swap_.progress());
    const float panel_in = tween::stagger(age_, 2, 0.05f, 0.5f);
    list.push_opacity(panel_in);
    list.bordered_rect(kHelp, 32.0f, Color::rgb(0xffffff, 0.05f), 1.5f, theme::hairline);
    const float x = kHelp.x + 38.0f;
    const float w = kHelp.w - 76.0f;
    list.push_opacity(in);
    const float rise = 8.0f * (1.0f - in);
    const hui::ui::FontRef &display = env_.fonts.display;
    std::vector<std::string> lines = display.font->wrap(row.label, 38.0f, w);
    if (lines.size() > 2)
        lines.resize(2);
    float baseline = kHelp.y + 86.0f + rise;
    for (const std::string &line : lines)
    {
        text(list, display, line, x, baseline, 38.0f, theme::ink, Align::left, -0.8f);
        baseline += 46.0f;
    }
    // The current value, in the page's violet.
    std::string value = row.value;
    if (row.kind == SettingRow::Kind::toggle)
        value = row.on ? "On" : "Off";
    text(list, env_.fonts.semibold, env_.fonts.semibold.font->fit(value, 28.0f, w), x, baseline + 6.0f, 28.0f,
         theme::accent_soft);
    hui::ui::paragraph(list, env_.fonts.regular, row.help, x, baseline + 64.0f, 24.0f, w, 36.0f, theme::ink_2, 9);
    const char *verb = row.kind == SettingRow::Kind::toggle ? "Switch" : row.kind == SettingRow::Kind::action ? "Open" : "Change";
    draw_chip(list, env_, hui::ui::Button::cross, verb, x, kHelp.y + kHelp.h - 62.0f, true);
    list.pop_opacity();
    list.pop_opacity();
}

void Settings::draw(hui::gfx::DrawList &list, hui::gfx::BackdropSpec &backdrop) const
{
    apply_page_backdrop(backdrop, clock_);
    if (!content_)
        return;
    const float in = tween::stagger(age_, 0, 0.0f, 0.45f);
    list.push_opacity(in);
    text(list, env_.fonts.semibold, "Settings", theme::kLeft, 158.0f, theme::kSubtitle + 4.0f, theme::ink_2);
    list.pop_opacity();

    list.push_clip({theme::kLeft - 44.0f, 188.0f, 1130.0f, 1080.0f - 188.0f});
    const Rect ring = ring_.value();
    if (focus_on_.value > 0.01f)
    {
        list.push_opacity(focus_on_.value);
        list.glow(ring, 24.0f, 26.0f, theme::accent.with_alpha(0.34f));
        list.pop_opacity();
    }
    for (int i = 0; i < static_cast<int>(content_->rows.size()); ++i)
    {
        const Rect r = row_rect(i, false);
        if (r.y > 1080.0f || r.y + r.h < 180.0f)
            continue;
        const SettingRow &row = content_->rows[i];
        const bool focused = i == index_ && !rail_.focused();
        const float enter = tween::stagger(age_, i, 0.04f, 0.4f);
        list.push_opacity(enter);
        list.push_transform(1.0f, 0.0f, 0.0f, 0.0f, 14.0f * (1.0f - enter));
        list.rounded_rect(r, 24.0f, focused ? theme::surface_hi : theme::surface);
        text(list, env_.fonts.semibold, row.label, r.x + 34.0f, r.cy() + 10.0f, 28.0f, focused ? theme::ink : theme::ink_2);
        const float right = r.x + r.w - 34.0f;
        if (row.kind == SettingRow::Kind::toggle)
        {
            draw_switch(list, right - 34.0f, r.cy(), switches_[i].value);
        }
        else
        {
            draw_chevron(list, right - 8.0f, r.cy(), 16.0f, 1, focused ? theme::ink : theme::ink_3);
            text(list, env_.fonts.regular, env_.fonts.regular.font->fit(row.value, 26.0f, 420.0f), right - 40.0f, r.cy() + 9.0f,
                 26.0f, focused ? theme::accent_soft : theme::ink_3, Align::right);
        }
        list.pop_transform();
        list.pop_opacity();
    }
    if (focus_on_.value > 0.01f)
    {
        list.push_opacity(focus_on_.value);
        list.bordered_rect(ring, 24.0f, Color::rgb(0xffffff, 0.0f), 4.0f, Color::rgb(0xffffff));
        list.pop_opacity();
    }
    list.pop_clip();

    if (index_ >= 0 && index_ < static_cast<int>(content_->rows.size()))
        draw_help(list, content_->rows[index_]);

    fade_down(list, theme::kRailWidth, 0.0f, theme::kScreenW, 118.0f, 0.62f);
    fade_up(list, theme::kRailWidth, 1000.0f, theme::kScreenW, 80.0f, 0.94f);
    draw_topbar(list, env_, false, age_);
    rail_.draw(list, env_, age_);
}

} // namespace sx
