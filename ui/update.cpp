#include "update.hpp"

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

void panel(hui::gfx::DrawList &list, const Rect &r, float radius)
{
    list.shadow({r.x + 10, r.y + 30, r.w - 20, r.h - 20}, radius, 56.0f, Color::rgb(0x000000, 0.5f));
    list.bordered_rect(r, radius, Color::rgb(0x0d0b16, 0.995f), 1.5f, theme::hairline);
}

// A button: the primary one is violet, the others are plain; the one under the cursor has the white ring.
// A button's body; `weight` (0..1) is how close the cursor's ring is. The ring itself is drawn once, gliding.
void button(hui::gfx::DrawList &list, const Env &env, const Rect &r, const char *label, bool primary, float weight, Color ink)
{
    list.rounded_rect(r, 26.0f, primary ? theme::accent : Color::rgb(0xffffff, 0.055f + 0.075f * weight));
    text(list, env.fonts.semibold, label, r.cx(), r.cy() + 11.0f, 30.0f, ink, Align::center);
}

void ring(hui::gfx::DrawList &list, const Rect &r, bool primary)
{
    list.glow(r, 26.0f, 22.0f, theme::accent.with_alpha(primary ? 0.42f : 0.30f));
    list.bordered_rect(r, 26.0f, Color::rgb(0xffffff, 0.0f), 3.0f, Color::rgb(0xffffff));
}

void bar(hui::gfx::DrawList &list, const Rect &r, float progress, float clock, bool indeterminate)
{
    list.rounded_rect(r, r.h * 0.5f, Color::rgb(0xffffff, 0.12f));
    if (indeterminate)
    {
        // A short light that travels back and forth: it is working, and not stuck.
        const float w = r.w * 0.28f, pos = 0.5f - 0.5f * std::cos(clock * 2.2f);
        list.rounded_rect({r.x + (r.w - w) * pos, r.y, w, r.h}, r.h * 0.5f, theme::accent_soft);
        return;
    }
    const float w = std::max(r.h, r.w * std::clamp(progress, 0.0f, 1.0f));
    list.glow({r.x, r.y, w, r.h}, r.h * 0.5f, 14.0f, theme::accent.with_alpha(0.35f));
    list.rounded_rect({r.x, r.y, w, r.h}, r.h * 0.5f, theme::accent_soft);
}

void hint(hui::gfx::DrawList &list, const Env &env, float cx, float cy, bool select, bool back, const char *back_label)
{
    hui::ui::Hint h[2];
    int n = 0;
    if (select)
        h[n++] = {hui::ui::Button::cross, "Select"};
    if (back)
        h[n++] = {hui::ui::Button::circle, back_label};
    if (n == 0)
        return;
    hui::ui::HintLayout layout;
    layout.size = 36.0f;
    layout.text_size = 24.0f;
    layout.item_gap = 40.0f;
    layout.cy = cy;
    layout.font = &env.fonts.regular;
    const float w = hui::ui::measure_hints(env.fonts, h, n, layout);
    list.bordered_rect({cx - w * 0.5f - 34.0f, cy - 32.0f, w + 68.0f, 64.0f}, 32.0f, Color::rgb(0x0d0b16, 0.97f), 1.5f, theme::hairline);
    hui::ui::draw_hints(list, env.fonts, hui::ui::GlyphStyle::dark(), h, n, cx - w * 0.5f, false, layout);
}

} // namespace

void draw_update(hui::gfx::DrawList &list, const Env &env, const UpdateState &s, float clock, float t)
{
    using Phase = UpdateState::Phase;
    constexpr float kW = 880.0f, kPad = 48.0f, kRow = 76.0f, kGap = 12.0f;
    list.rounded_rect({0, 0, theme::kScreenW, theme::kScreenH}, 0, theme::ground.with_alpha(0.94f * t));

    const char *title = s.phase == Phase::offer          ? "Update available"
                        : s.phase == Phase::downloading  ? "Downloading the update"
                        : s.phase == Phase::installing   ? "Getting it ready"
                        : s.phase == Phase::closing      ? "Updating"
                        : s.phase == Phase::failed       ? "The update did not finish"
                                                         : "You are up to date";
    // Which pieces this state has decides the height.
    std::vector<std::string> notes = s.notes;
    if (notes.size() > 5)
        notes.resize(5);
    float body = 0.0f;
    switch (s.phase)
    {
    case Phase::offer:
        body = 44.0f + (notes.empty() ? 0.0f : 20.0f + static_cast<float>(notes.size()) * 36.0f) + (s.test_build ? 56.0f : 0.0f) + 40.0f +
               2.0f * kRow + kGap;
        break;
    case Phase::downloading:
    case Phase::installing:
        body = 40.0f + 12.0f + 44.0f;
        break;
    case Phase::closing:
        body = 40.0f + 36.0f * 3.0f;
        break;
    case Phase::failed:
    case Phase::current:
        body = 40.0f + 36.0f * 2.0f + 40.0f + kRow;
        break;
    }
    const float h = kPad + 92.0f + 36.0f + body + kPad;
    const Rect r{(theme::kScreenW - kW) * 0.5f, (theme::kScreenH - h) * 0.5f - 34.0f, kW, h};
    list.push_opacity(t);
    list.push_transform(0.96f + 0.04f * t, r.cx(), r.cy(), 0.0f, (1.0f - t) * 12.0f);
    panel(list, r, 46.0f);

    // The mark and the title.
    if (env.icons.logo != 0)
        list.image(env.icons.logo, {r.x + kPad, r.y + kPad, 92.0f, 92.0f}, kFullUv, Color::rgb(0xffffff));
    text(list, env.fonts.display, title, r.x + kPad + 92.0f + 32.0f, r.y + kPad + 62.0f, 42.0f, theme::ink, Align::left, -0.8f);

    const float x = r.x + kPad, w = kW - 2.0f * kPad;
    float y = r.y + kPad + 92.0f + 36.0f;
    switch (s.phase)
    {
    case Phase::offer:
    {
        text(list, env.fonts.semibold, "Version " + s.version, x, y + 34.0f, 34.0f, theme::ink);
        text(list, env.fonts.regular, s.size, x + w, y + 34.0f, 26.0f, theme::ink_3, Align::right);
        y += 44.0f;
        if (!notes.empty())
        {
            y += 20.0f;
            for (const std::string &line : notes)
            {
                text(list, env.fonts.regular, env.fonts.regular.font->fit(line, 26.0f, w), x, y + 26.0f, 26.0f, theme::ink_2);
                y += 36.0f;
            }
        }
        if (s.test_build)
        {
            y += 20.0f;
            text(list, env.fonts.regular, env.fonts.regular.font->fit("Test build: this installs the published release over it.", 24.0f, w), x,
                 y + 24.0f, 24.0f, theme::accent_soft);
            y += 36.0f;
        }
        y += 40.0f;
        const float pos = std::clamp(s.pos >= 0.0f ? s.pos : static_cast<float>(s.selected), 0.0f, 1.0f);
        button(list, env, {x, y, w, kRow}, "Update now", true, 1.0f - pos, Color::rgb(0xffffff));
        button(list, env, {x, y + kRow + kGap, w, kRow}, "Later", false, pos, theme::ink_2);
        ring(list, {x, y + pos * (kRow + kGap), w, kRow}, pos < 0.5f);
        hint(list, env, r.cx(), r.y + r.h + 62.0f, true, false, "");
        break;
    }
    case Phase::downloading:
    case Phase::installing:
    {
        const bool download = s.phase == Phase::downloading;
        bar(list, {x, y + 40.0f, w, 12.0f}, s.progress, clock, !download);
        const std::string left = download ? s.detail : std::string("Checking it, then putting it in place");
        text(list, env.fonts.regular, left, x, y + 40.0f + 12.0f + 38.0f, 26.0f, theme::ink_2);
        if (download && !s.left.empty())
            text(list, env.fonts.regular, s.left, x + w, y + 40.0f + 12.0f + 38.0f, 26.0f, theme::ink_3, Align::right);
        hint(list, env, r.cx(), r.y + r.h + 62.0f, false, download, "Cancel");
        break;
    }
    case Phase::closing:
    {
        text(list, env.fonts.regular, "The app closes now.", x, y + 40.0f + 26.0f, 28.0f, theme::ink);
        text(list, env.fonts.regular, "A message on the console says when the new version", x, y + 40.0f + 70.0f, 26.0f, theme::ink_2);
        text(list, env.fonts.regular, "is in place. Then open Stremio again.", x, y + 40.0f + 106.0f, 26.0f, theme::ink_2);
        break;
    }
    case Phase::failed:
    case Phase::current:
    {
        const bool failed = s.phase == Phase::failed;
        std::vector<std::string> lines = env.fonts.regular.font->wrap(
            failed ? s.message + " Nothing was changed." : "Version " + s.current + " is the newest one.", 26.0f, w);
        if (lines.size() > 2)
            lines.resize(2);
        float ly = y + 40.0f + 26.0f;
        for (const std::string &line : lines)
        {
            text(list, env.fonts.regular, line, x, ly, 26.0f, failed ? Color::rgb(0xff8585) : theme::ink_2);
            ly += 36.0f;
        }
        button(list, env, {x, r.y + r.h - kPad - kRow, w, kRow}, "Close", false, 1.0f, theme::ink);
        ring(list, {x, r.y + r.h - kPad - kRow, w, kRow}, false);
        hint(list, env, r.cx(), r.y + r.h + 62.0f, true, false, "");
        break;
    }
    }
    list.pop_transform();
    list.pop_opacity();
}

} // namespace sx
