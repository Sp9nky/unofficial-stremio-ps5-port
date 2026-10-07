#include "addons.hpp"

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

constexpr int kCols = 3;
constexpr float kTileW = 540.0f;
constexpr float kTileH = 248.0f;
constexpr float kGapX = 25.0f;
constexpr float kGapY = 24.0f;
constexpr float kTop = 214.0f;

} // namespace

Rect Addons::tile_rect(int index, bool final_position) const
{
    const int row = index / kCols;
    const int col = index % kCols;
    const float scroll = final_position ? scroll_.position.target : scroll_.offset();
    return {theme::kLeft + static_cast<float>(col) * (kTileW + kGapX),
            kTop + static_cast<float>(row) * (kTileH + kGapY) - scroll, kTileW, kTileH};
}

void Addons::set_content(const AddonsContent *content)
{
    content_ = content;
    index_ = 0;
    scroll_.position.snap(0.0f);
    ring_.snap(tile_rect(0, true));
    focus_on_.snap(1.0f);
}

void Addons::update(const hui::InputFrame &input, float dt)
{
    tick(dt);
    if (!content_ || content_->items.empty())
        return;
    const int count = static_cast<int>(content_->items.size());
    if (!rail_input(input))
    {
        const int col = index_ % kCols;
        switch (input.nav)
        {
        case Direction::left:
            if (col == 0)
                to_rail();
            else
                --index_;
            break;
        case Direction::right:
            if (col + 1 < kCols && index_ + 1 < count)
                ++index_;
            break;
        case Direction::up:
            if (index_ >= kCols)
                index_ -= kCols;
            break;
        case Direction::down:
            if (index_ + kCols < count)
                index_ += kCols;
            else if ((count - 1) / kCols > index_ / kCols)
                index_ = count - 1;
            break;
        default:
            break;
        }
    }
    const float top = static_cast<float>(index_ / kCols) * (kTileH + kGapY);
    scroll_.reveal(top, top + kTileH, 1080.0f - kTop - 30.0f, 12.0f);
    scroll_.update(dt, 18.0f);
    ring_.target(tile_rect(index_, true));
    ring_.update(dt, 20.0f);
    focus_on_.target = rail_.focused() ? 0.0f : 1.0f;
    focus_on_.update(dt, 16.0f);
}

void Addons::draw_tile(hui::gfx::DrawList &list, const Addon &a, const Rect &r, float enter, bool focused) const
{
    list.push_opacity(enter);
    list.push_transform(1.0f, 0.0f, 0.0f, 0.0f, 16.0f * (1.0f - enter));
    list.rounded_rect(r, 28.0f, focused ? theme::surface_hi : theme::surface);
    // The addon's logo, or its initial on its own colour.
    const Rect icon{r.x + 30.0f, r.y + 30.0f, 88.0f, 88.0f};
    if (a.logo != 0)
    {
        list.image(a.logo, icon, hui::gfx::kFullUv, Color::rgb(0xffffff), 20.0f);
    }
    else
    {
        list.gradient_rect(icon, 20.0f, hui::gfx::mix(Color::rgb(a.colour), Color::rgb(0xffffff), 0.18f),
                           hui::gfx::mix(Color::rgb(a.colour), Color::rgb(0x000000), 0.35f));
        text(list, env_.fonts.semibold, a.initial, icon.cx(), icon.cy() + 15.0f, 44.0f, Color::rgb(0xffffff, 0.95f),
             Align::center);
    }
    const float x = icon.x + icon.w + 24.0f;
    const float w = r.x + r.w - x - 28.0f;
    text(list, env_.fonts.semibold, env_.fonts.semibold.font->fit(a.name, 30.0f, w), x, r.y + 66.0f, 30.0f, theme::ink);
    // The version gives way to the "Local" tag, which keeps its place.
    const float local_w = a.local ? env_.fonts.semibold.measure("Local", 20.0f) + 28.0f + 16.0f : 0.0f;
    const std::string version = env_.fonts.regular.font->fit(a.version, 22.0f, w - local_w);
    text(list, env_.fonts.regular, version, x, r.y + 102.0f, 22.0f, theme::ink_3);
    if (a.local)
        draw_tag(list, env_, "Local", x + env_.fonts.regular.measure(version, 22.0f) + 16.0f, r.y + 94.0f,
                 theme::accent.with_alpha(0.28f), theme::accent_soft);
    // What it provides. Tags that would run past the tile are counted instead: "+2".
    std::vector<std::string> kinds;
    for (std::size_t start = 0; start < a.types.size();)
    {
        std::size_t end = a.types.find(' ', start);
        if (end == std::string::npos)
            end = a.types.size();
        if (end > start)
            kinds.push_back(a.types.substr(start, end - start));
        start = end + 1;
    }
    const float tag_limit = r.x + r.w - 30.0f;
    float tx = r.x + 30.0f;
    for (std::size_t i = 0; i < kinds.size(); ++i)
    {
        const std::size_t left = kinds.size() - i;
        const auto width = [&](const std::string &s) { return env_.fonts.semibold.measure(s, 20.0f) + 28.0f; };
        const std::string more = "+" + std::to_string(left);
        // Room for this tag, and for "+n" after it if others remain.
        const float need = width(kinds[i]) + (left > 1 ? 10.0f + width("+" + std::to_string(left - 1)) : 0.0f);
        if (tx + need > tag_limit && tx + width(more) <= tag_limit)
        {
            draw_tag(list, env_, more, tx, r.y + 146.0f, Color::rgb(0xffffff, 0.09f), theme::ink_3);
            break;
        }
        tx += draw_tag(list, env_, kinds[i], tx, r.y + 146.0f, Color::rgb(0xffffff, 0.09f), theme::ink_2) + 10.0f;
    }
    hui::ui::paragraph(list, env_.fonts.regular, a.description, r.x + 30.0f, r.y + 196.0f, 22.0f, r.w - 60.0f, 29.0f,
                       theme::ink_2, 2);
    list.pop_transform();
    list.pop_opacity();
}

void Addons::draw(hui::gfx::DrawList &list, hui::gfx::BackdropSpec &backdrop) const
{
    apply_page_backdrop(backdrop, clock_);
    if (!content_)
        return;
    const float in = tween::stagger(age_, 0, 0.0f, 0.45f);
    list.push_opacity(in);
    char count[48];
    std::snprintf(count, sizeof(count), "%zu addons installed", content_->items.size());
    text(list, env_.fonts.semibold, count, theme::kLeft, 158.0f, theme::kSubtitle, theme::ink_2);
    list.pop_opacity();

    list.push_clip({theme::kLeft - 44.0f, 192.0f, theme::kScreenW, 1080.0f - 192.0f});
    const Rect ring = ring_.value();
    if (focus_on_.value > 0.01f)
    {
        list.push_opacity(focus_on_.value);
        list.glow(ring.inset(-3.0f), 30.0f, 30.0f, theme::accent.with_alpha(0.38f));
        list.pop_opacity();
    }
    for (int i = 0; i < static_cast<int>(content_->items.size()); ++i)
    {
        const Rect r = tile_rect(i, false);
        if (r.y > 1080.0f || r.y + r.h < 180.0f)
            continue;
        const float enter = tween::stagger(age_, (i / kCols) * 2 + i % kCols, 0.045f, 0.45f);
        draw_tile(list, content_->items[i], r, enter, i == index_ && !rail_.focused());
    }
    if (focus_on_.value > 0.01f)
    {
        list.push_opacity(focus_on_.value);
        list.bordered_rect(ring.inset(-3.0f), 30.0f, Color::rgb(0xffffff, 0.0f), 4.0f, Color::rgb(0xffffff));
        list.pop_opacity();
    }
    list.pop_clip();

    fade_down(list, theme::kRailWidth, 0.0f, theme::kScreenW, 118.0f, 0.62f);
    fade_up(list, theme::kRailWidth, 1000.0f, theme::kScreenW, 80.0f, 0.94f);
    draw_topbar(list, env_, false, age_);
    // The only action here, where it can be seen.
    draw_chip(list, env_, hui::ui::Button::square, "Reload addons", theme::kRight - 252.0f, 148.0f);
    rail_.draw(list, env_, age_);
}

} // namespace sx
