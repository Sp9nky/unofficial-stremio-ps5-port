#include "cards.hpp"

#include <algorithm>
#include <cmath>

namespace sx
{

using hui::gfx::Align;
using hui::gfx::Color;
using hui::gfx::kFullUv;
using hui::gfx::Rect;
using hui::ui::text;

void draw_poster_card(hui::gfx::DrawList &list, const Env &env, const Title &t, const Rect &r, float scale,
                      float enter, bool focused, bool show_progress)
{
    list.push_opacity(enter);
    // Cards arrive from a little below.
    const float lift = 18.0f * (1.0f - enter);
    list.push_transform(scale, r.cx(), r.cy(), 0.0f, lift);
    list.shadow({r.x + 8, r.y + 18, r.w - 16, r.h - 14}, 22, 36, Color::rgb(0x000000, focused ? 0.55f : 0.38f));
    if (t.poster != 0)
    {
        list.image(t.poster, r, kFullUv, Color::rgb(0xffffff), 18.0f);
    }
    else
    {
        list.gradient_rect(r, 18.0f, Color::rgb(0x2a2540), Color::rgb(0x14111f));
        hui::ui::paragraph(list, env.fonts.semibold, t.name, r.x + 20, r.y + 60, 26.0f, r.w - 40, 34.0f, theme::ink_2,
                           5);
    }
    // A strip at the foot of the poster of whatever can be resumed.
    if (show_progress && t.progress >= 0.0f)
    {
        list.gradient_rect({r.x, r.y + r.h - 70.0f, r.w, 70.0f}, 0.0f, Color::rgb(0x000000, 0.0f),
                           Color::rgb(0x000000, 0.62f));
        const Rect track{r.x + 18.0f, r.y + r.h - 24.0f, r.w - 36.0f, 7.0f};
        list.rounded_rect(track, 3.5f, Color::rgb(0xffffff, 0.30f));
        list.rounded_rect({track.x, track.y, track.w * std::clamp(t.progress, 0.0f, 1.0f), track.h}, 3.5f,
                          theme::accent_soft);
    }
    if (!t.badge.empty())
    {
        const float bw = env.fonts.semibold.measure(t.badge, 20.0f) + 22.0f;
        list.rounded_rect({r.x + 12.0f, r.y + 12.0f, bw, 34.0f}, 10.0f, Color::rgb(0x0c0b11, 0.78f));
        text(list, env.fonts.semibold, t.badge, r.x + 12.0f + bw * 0.5f, r.y + 36.0f, 20.0f, theme::ink, Align::center);
    }
    if (t.watched)
    {
        // A green disc with a check, top right.
        const float cx = r.x + r.w - 32.0f, cy = r.y + 32.0f;
        list.circle(cx, cy, 18.0f, theme::watched);
        list.line(cx - 7.0f, cy + 1.0f, cx - 2.0f, cy + 6.5f, 3.4f, Color::rgb(0xffffff));
        list.line(cx - 2.0f, cy + 6.5f, cx + 8.0f, cy - 6.0f, 3.4f, Color::rgb(0xffffff));
    }
    list.pop_transform();
    // The name under the poster.
    const hui::ui::FontRef &face = focused ? env.fonts.semibold : env.fonts.regular;
    text(list, face, face.font->fit(t.name, 24.0f, r.w), r.x, r.y + r.h + 54.0f, 24.0f,
         focused ? theme::ink : theme::ink_2);
    list.pop_opacity();
}

void draw_chevron(hui::gfx::DrawList &list, float cx, float cy, float size, int direction, Color colour)
{
    // Two strokes meeting at a point: direction 0 down, 1 right, 2 left.
    const float h = size * 0.5f;
    float ax, ay, bx, by, px, py;
    switch (direction)
    {
    case 1:
        ax = cx - h * 0.6f, ay = cy - h, px = cx + h * 0.6f, py = cy, bx = cx - h * 0.6f, by = cy + h;
        break;
    case 2:
        ax = cx + h * 0.6f, ay = cy - h, px = cx - h * 0.6f, py = cy, bx = cx + h * 0.6f, by = cy + h;
        break;
    default:
        ax = cx - h, ay = cy - h * 0.6f, px = cx, py = cy + h * 0.6f, bx = cx + h, by = cy - h * 0.6f;
        break;
    }
    list.line(ax, ay, px, py, 3.2f, colour);
    list.line(px, py, bx, by, 3.2f, colour);
}

namespace
{
constexpr float kChipPad = 26.0f + 14.0f + 16.0f + 18.0f + 24.0f; // everything but the two texts
}

std::vector<float> filter_value_caps(const Env &env, const std::vector<Filter> &filters, float avail)
{
    std::vector<float> natural, caps(filters.size(), 0.0f);
    float fixed = 16.0f * static_cast<float>(filters.size() > 0 ? filters.size() - 1 : 0), total = 0.0f;
    for (const Filter &f : filters)
    {
        natural.push_back(env.fonts.semibold.measure(f.value, 24.0f));
        fixed += kChipPad + env.fonts.regular.measure(f.label, 22.0f);
        total += natural.back();
    }
    const float budget = avail - fixed;
    if (total <= budget)
        return caps;
    float lo = 40.0f, hi = 0.0f;
    for (float v : natural)
        hi = std::max(hi, v);
    for (int i = 0; i < 24; ++i)
    {
        const float mid = (lo + hi) * 0.5f;
        float sum = 0.0f;
        for (float v : natural)
            sum += std::min(v, mid);
        (sum > budget ? hi : lo) = mid;
    }
    for (std::size_t i = 0; i < caps.size(); ++i)
        if (natural[i] > lo)
            caps[i] = lo;
    return caps;
}

float draw_filter_chip(hui::gfx::DrawList &list, const Env &env, const Filter &f, float x, float cy, bool focused,
                       float max_value)
{
    const float h = 58.0f;
    const float label_w = env.fonts.regular.measure(f.label, 22.0f);
    const std::string value = max_value > 0.0f ? env.fonts.semibold.font->fit(f.value, 24.0f, max_value) : f.value;
    const float value_w = env.fonts.semibold.measure(value, 24.0f);
    const float w = 26.0f + label_w + 14.0f + value_w + 16.0f + 18.0f + 24.0f;
    const Rect r{x, cy - h * 0.5f, w, h};
    if (focused)
    {
        list.glow(r, h * 0.5f, 22.0f, theme::accent.with_alpha(0.42f));
        list.bordered_rect(r, h * 0.5f, theme::surface_hi, 3.0f, Color::rgb(0xffffff));
    }
    else
    {
        list.rounded_rect(r, h * 0.5f, theme::surface);
    }
    text(list, env.fonts.regular, f.label, x + 26.0f, cy + 8.0f, 22.0f, theme::ink_3);
    text(list, env.fonts.semibold, value, x + 26.0f + label_w + 14.0f, cy + 8.5f, 24.0f, theme::ink);
    draw_chevron(list, x + w - 30.0f, cy, 14.0f, 0, focused ? theme::ink : theme::ink_2);
    return w;
}

void draw_status(hui::gfx::DrawList &list, const Env &env, const std::string &message, float baseline)
{
    if (message.empty())
        return;
    hui::ui::paragraph(list, env.fonts.regular, message, theme::kLeft, baseline, 32.0f, 1000.0f, 46.0f, theme::ink_2, 3);
}

float draw_tag(hui::gfx::DrawList &list, const Env &env, const std::string &label, float x, float cy, Color fill,
               Color ink)
{
    const float w = env.fonts.semibold.measure(label, 20.0f) + 28.0f;
    list.rounded_rect({x, cy - 17.0f, w, 34.0f}, 17.0f, fill);
    text(list, env.fonts.semibold, label, x + w * 0.5f, cy + 7.0f, 20.0f, ink, Align::center);
    return w;
}

void draw_switch(hui::gfx::DrawList &list, float x, float cy, float on)
{
    const Rect track{x - 34.0f, cy - 19.0f, 68.0f, 38.0f};
    list.rounded_rect(track, 19.0f, hui::gfx::mix(Color::rgb(0xffffff, 0.18f), theme::accent, on));
    const float knob = track.x + 19.0f + 30.0f * on;
    list.shadow({knob - 14.0f, cy - 10.0f, 28.0f, 28.0f}, 14.0f, 8.0f, Color::rgb(0x000000, 0.35f));
    list.circle(knob, cy, 15.0f, Color::rgb(0xffffff));
}

} // namespace sx
