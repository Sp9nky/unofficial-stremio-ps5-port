#include "chrome.hpp"

#include "core/tween.hpp"

#include <algorithm>
#include <cmath>

namespace sx
{

using hui::gfx::Align;
using hui::gfx::Color;
using hui::gfx::kFullUv;
using hui::gfx::Rect;
using hui::ui::text;

namespace
{

constexpr const char *kPageNames[5] = {"Board", "Discover", "Library", "Addons", "Settings"};
constexpr float kItemW = 100.0f;
constexpr float kItemH = 108.0f;
constexpr float kItemGap = 12.0f;

} // namespace

Rail::Rail()
{
    pill_.snap(item_rect(0));
}

Rect Rail::item_rect(int index)
{
    const float total = 5.0f * kItemH + 4.0f * kItemGap;
    const float top = (theme::kScreenH - total) * 0.5f;
    return {(theme::kRailWidth - kItemW) * 0.5f, top + static_cast<float>(index) * (kItemH + kItemGap),
            kItemW, kItemH};
}

void Rail::set_page(Page page)
{
    page_ = static_cast<int>(page);
    if (!focused_)
        cursor_ = page_;
}

void Rail::focus(bool on)
{
    focused_ = on;
    if (on)
        cursor_ = page_;
}

void Rail::move(int delta)
{
    cursor_ = std::clamp(cursor_ + delta, 0, 4);
}

void Rail::reset(Page page)
{
    page_ = static_cast<int>(page);
    focused_ = false;
    cursor_ = page_;
    pill_.snap(item_rect(cursor_));
    glow_.snap(0.0f);
}

void Rail::update(float dt)
{
    pill_.target(item_rect(cursor_));
    pill_.update(dt, 22.0f);
    glow_.target = focused_ ? 1.0f : 0.0f;
    glow_.update(dt, 16.0f);
}

void Rail::draw(hui::gfx::DrawList &list, const Env &env, float age) const
{
    // The pill under the cursor: violet with a white edge, only while the rail has the focus.
    if (glow_.value > 0.01f)
    {
        const Rect p = pill_.value();
        list.push_opacity(glow_.value);
        list.glow(p, 26, 26, theme::accent.with_alpha(0.45f));
        list.bordered_rect(p, 26, theme::accent, 3.0f, Color::rgb(0xffffff));
        list.pop_opacity();
    }
    for (int i = 0; i < 5; ++i)
    {
        const Rect r = item_rect(i);
        const bool active = i == page_;
        const bool under_cursor = focused_ && i == cursor_;
        // Items arrive one after the other when a page opens.
        const float in = hui::tween::stagger(age, i, 0.05f, 0.35f);
        list.push_opacity(in);
        if (active && !under_cursor)
            list.rounded_rect(r, 26, theme::surface);
        const Color tint = (active || under_cursor) ? Color::rgb(0xffffff) : theme::ink_3;
        const std::uint32_t icon = (active || under_cursor) ? env.icons.nav_solid[i] : env.icons.nav_line[i];
        if (icon != 0)
            list.image(icon, {r.cx() - 24.0f, r.y + 14.0f + 6.0f * (1.0f - in), 48.0f, 48.0f}, kFullUv, tint);
        text(list, env.fonts.regular, kPageNames[i], r.cx(), r.y + 90.0f, theme::kLabel, tint, Align::center);
        list.pop_opacity();
    }
}

void draw_logo(hui::gfx::DrawList &list, const Env &env)
{
    // Top left, level with the search pill, over the rail's column.
    if (env.icons.logo != 0)
        list.image(env.icons.logo, {theme::kRailWidth * 0.5f - 30.0f, 34.0f, 60.0f, 60.0f}, kFullUv,
                   Color::rgb(0xffffff, 0.92f));
}

void draw_topbar(hui::gfx::DrawList &list, const Env &env, bool search_focused, float age, const std::string *query,
                 float clock)
{
    const float in = hui::tween::stagger(age, 0, 0.0f, 0.4f);
    list.push_opacity(in);
    draw_logo(list, env);
    // The search pill; its Triangle glyph says which button opens it.
    const Rect pill{theme::kLeft, 34.0f, 620.0f, 60.0f};
    if (search_focused)
    {
        list.glow(pill, 30, 22, theme::accent.with_alpha(0.4f));
        list.bordered_rect(pill, 30, theme::surface_hi, 3.0f, Color::rgb(0xffffff));
    }
    else
    {
        list.rounded_rect(pill, 30, theme::surface);
    }
    if (env.icons.search != 0)
        list.image(env.icons.search, {pill.x + 24.0f, pill.cy() - 14.0f, 28.0f, 28.0f}, kFullUv,
                   search_focused ? Color::rgb(0xffffff) : theme::ink_3);
    if (query != nullptr && !query->empty())
    {
        const float w = text(list, env.fonts.semibold, env.fonts.semibold.font->fit(*query, theme::kBody + 2.0f, 440.0f),
                             pill.x + 70.0f, pill.cy() + 9.0f, theme::kBody + 2.0f, theme::ink);
        if (std::fmod(clock, 1.0f) < 0.6f)
            list.rounded_rect({pill.x + 74.0f + w, pill.cy() - 15.0f, 3.0f, 30.0f}, 1.5f, theme::accent_soft);
    }
    else
    {
        text(list, env.fonts.regular, "Search movies, series and more", pill.x + 70.0f, pill.cy() + 8.0f, theme::kBody,
             search_focused ? theme::ink_2 : theme::ink_3);
    }
    hui::ui::draw_button(list, env.fonts, hui::ui::GlyphStyle::dark(), hui::ui::Button::triangle,
                         pill.x + pill.w - 54.0f, pill.cy(), 34.0f);

    // The clock, at the right edge of the page (Stremio has no profile picture to show).
    text(list, env.fonts.semibold, env.clock, theme::kRight, 74.0f, 30.0f, theme::ink_2, Align::right);
    list.pop_opacity();
}

float draw_chip(hui::gfx::DrawList &list, const Env &env, hui::ui::Button button, const char *label, float x,
                float cy, bool strong, float max_text)
{
    const float h = 52.0f;
    const float glyph = 36.0f;
    std::string shown = label;
    if (max_text > 0.0f)
        shown = env.fonts.semibold.font->fit(shown, theme::kBody, max_text);
    label = shown.c_str();
    const float text_w = env.fonts.semibold.measure(label, theme::kBody);
    const float w = 12.0f + glyph + 12.0f + text_w + 22.0f;
    const Rect r{x, cy - h * 0.5f, w, h};
    if (strong)
        list.rounded_rect(r, h * 0.5f, Color::rgb(0xffffff, 0.94f));
    else
        list.rounded_rect(r, h * 0.5f, theme::surface_hi);
    hui::ui::draw_button(list, env.fonts, hui::ui::GlyphStyle::dark(), button, x + 12.0f, cy, glyph);
    text(list, env.fonts.semibold, label, x + 12.0f + glyph + 12.0f, cy + 8.0f, theme::kBody,
         strong ? Color::rgb(0x0c0b11) : theme::ink);
    return w;
}

float draw_imdb(hui::gfx::DrawList &list, const Env &env, const std::string &rating, float x, float baseline)
{
    if (rating.empty())
        return 0.0f;
    const Rect badge{x, baseline - 25.0f, 62.0f, 32.0f};
    list.rounded_rect(badge, 7, theme::imdb);
    text(list, env.fonts.semibold, "IMDb", badge.cx(), baseline - 1.0f, 19.0f, Color::rgb(0x000000), Align::center);
    const float w = text(list, env.fonts.semibold, rating, x + 74.0f, baseline, theme::kBody + 2.0f, theme::ink);
    return 74.0f + w;
}

void fade_down(hui::gfx::DrawList &list, float x, float y, float w, float h, float strength, float solid)
{
    if (solid > 0.5f)
        list.rounded_rect({x, y, w, solid}, 0, theme::ground.with_alpha(strength));
    list.gradient_rect({x, y + solid, w, h - solid}, 0, theme::ground.with_alpha(strength),
                       theme::ground.with_alpha(0.0f));
}

void fade_up(hui::gfx::DrawList &list, float x, float y, float w, float h, float strength)
{
    list.gradient_rect({x, y, w, h}, 0, theme::ground.with_alpha(0.0f), theme::ground.with_alpha(strength));
}

} // namespace sx
