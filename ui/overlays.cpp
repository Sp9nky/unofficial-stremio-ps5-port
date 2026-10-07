#include "overlays.hpp"

#include "cards.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace sx
{

using hui::gfx::Align;
using hui::gfx::Color;
using hui::gfx::kFullUv;
using hui::gfx::Rect;
using hui::ui::text;
namespace tween = hui::tween;

namespace
{

void scrim(hui::gfx::DrawList &list, float t, float strength)
{
    list.rounded_rect({0, 0, theme::kScreenW, theme::kScreenH}, 0, theme::ground.with_alpha(strength * t));
}

// A panel that arrives from a slightly smaller size.
void panel(hui::gfx::DrawList &list, const Rect &r, float radius, float fill_alpha)
{
    list.shadow({r.x + 10, r.y + 30, r.w - 20, r.h - 20}, radius, 56.0f, Color::rgb(0x000000, 0.5f));
    list.bordered_rect(r, radius, Color::rgb(0x0d0b16, std::max(fill_alpha, 0.995f)), 1.5f, theme::hairline);
}

void tick(hui::gfx::DrawList &list, float x, float cy, float size, Color colour)
{
    list.line(x, cy + size * 0.05f, x + size * 0.33f, cy + size * 0.40f, 3.6f, colour);
    list.line(x + size * 0.33f, cy + size * 0.40f, x + size, cy - size * 0.38f, 3.6f, colour);
}

} // namespace

// ---------------------------------------------------------------------------

namespace
{

constexpr float kTwoPi = 6.2831853f;

float chip_width(const Env &env, const char *label)
{
    return 12.0f + 36.0f + 12.0f + env.fonts.semibold.measure(label, theme::kBody) + 22.0f;
}

std::string stats_line(const LaunchState &s)
{
    std::string out;
    if (!s.peers.empty())
        out += s.peers + (s.peers == "1" ? " peer" : " peers");
    if (!s.speed.empty())
        out += (out.empty() ? "" : "   \xC2\xB7   ") + s.speed;
    if (!s.done.empty())
        out += (out.empty() ? "" : "   \xC2\xB7   ") + s.done + " downloaded";
    return out;
}

// A step of starting a stream: done (a violet check), in progress (a ring that
// turns) or still to come (an empty ring).
void stage_mark(hui::gfx::DrawList &list, float cx, float cy, int state, float clock, float r = 17.0f)
{
    if (state == 0)
    {
        list.circle(cx, cy, r, theme::accent);
        list.line(cx - r * 0.42f, cy + r * 0.04f, cx - r * 0.1f, cy + r * 0.38f, 3.2f, Color::rgb(0xffffff));
        list.line(cx - r * 0.1f, cy + r * 0.38f, cx + r * 0.46f, cy - r * 0.32f, 3.2f, Color::rgb(0xffffff));
    }
    else if (state == 1)
    {
        list.ring(cx, cy, r - 1.0f, 4.0f, Color::rgb(0xffffff, 0.16f));
        list.arc(cx, cy, r, 4.5f, clock * 5.0f, 1.8f, theme::accent_soft);
    }
    else
    {
        list.ring(cx, cy, r - 1.5f, 3.0f, Color::rgb(0xffffff, 0.22f));
    }
}

int stage_state(const LaunchState &s, int i)
{
    return i < s.stage ? 0 : i == s.stage ? 1 : 2;
}

std::string current_stage(const LaunchState &s)
{
    return s.stage >= 0 && s.stage < static_cast<int>(s.stages.size()) ? s.stages[s.stage] : std::string();
}

// Draws up to two lines ending on last_baseline; a second line pushes the pair
// down so the first never starts above first_min. Returns the last baseline.
float title_lines(hui::gfx::DrawList &list, const Env &env, const std::string &title, float x, float last_baseline,
                  float first_min, float size, float line_height, float width, Align align, float tracking)
{
    const hui::ui::FontRef &display = env.fonts.display;
    std::vector<std::string> lines = display.font->wrap(title, size, width);
    if (lines.size() > 2)
    {
        lines.resize(2);
        lines[1] = display.font->fit(lines[1] + " \xE2\x80\xA6", size, width);
    }
    float baseline = std::max(last_baseline - line_height * static_cast<float>(lines.size() - 1), first_min);
    for (const std::string &line : lines)
    {
        text(list, display, display.font->fit(line, size, width), x, baseline, size, theme::ink, align, tracking);
        baseline += line_height;
    }
    return baseline - line_height;
}

// Starting a stream: one ring around the artwork that fills with the buffer, the
// title and what is happening in the centre, the steps along a line below.
void launch_screen(hui::gfx::DrawList &list, const Env &env, const LaunchState &s, float clock)
{
    const Rect full{0, 0, theme::kScreenW, theme::kScreenH};
    list.rounded_rect(full, 0, theme::ground);
    if (s.backdrop != 0)
        list.image(s.backdrop, full, kFullUv, Color::rgb(0xffffff, 0.30f));
    list.gradient_rect(full, 0, theme::ground.with_alpha(0.70f), theme::ground.with_alpha(0.95f));

    const float cx = theme::kScreenW * 0.5f, cy = 372.0f, radius = 150.0f;
    list.glow({cx - radius, cy - radius, radius * 2, radius * 2}, radius, 46.0f, theme::accent.with_alpha(0.22f));
    list.ring(cx, cy, radius, 10.0f, Color::rgb(0xffffff, 0.14f));
    if (s.progress >= 0.0f)
        list.arc(cx, cy, radius + 5.0f, 10.0f, 0.0f, kTwoPi * std::clamp(s.progress, 0.01f, 1.0f), theme::accent_soft);
    else
        list.arc(cx, cy, radius + 5.0f, 10.0f, clock * 3.0f, 1.5f, theme::accent_soft);
    const Rect art{cx - 122.0f, cy - 122.0f, 244.0f, 244.0f};
    if (s.poster != 0)
        list.image(s.poster, art, {0.0f, 0.12f, 1.0f, 0.6667f}, Color::rgb(0xffffff), 122.0f);
    else
        list.circle(cx, cy, 122.0f, Color::rgb(0x2a2540));

    const float last = title_lines(list, env, s.title, cx, 622.0f, 600.0f, 56.0f, 64.0f, 1300.0f, Align::center, -1.0f);
    float y = last + 54.0f;
    if (!s.subtitle.empty())
    {
        text(list, env.fonts.regular, env.fonts.regular.font->fit(s.subtitle, 30.0f, 1500.0f), cx, y, 30.0f, theme::ink_2,
             Align::center);
        y += 58.0f;
    }
    std::string status = current_stage(s);
    if (s.progress >= 0.0f)
    {
        char pct[16];
        std::snprintf(pct, sizeof(pct), "  %d%%", static_cast<int>(s.progress * 100.0f + 0.5f));
        status += pct;
    }
    text(list, env.fonts.semibold, status, cx, y + 12.0f, 36.0f, theme::ink, Align::center);
    text(list, env.fonts.regular, stats_line(s), cx, y + 62.0f, 26.0f, theme::ink_2, Align::center);

    // The steps along a line, the current one named.
    const int n = static_cast<int>(s.stages.size());
    const float spread = 300.0f, line_y = 902.0f;
    const float first = cx - spread * static_cast<float>(n - 1) * 0.5f;
    for (int i = 0; i + 1 < n; ++i)
        list.rounded_rect({first + static_cast<float>(i) * spread + 22.0f, line_y - 1.5f, spread - 44.0f, 3.0f}, 1.5f,
                          i < s.stage ? theme::accent : Color::rgb(0xffffff, 0.14f));
    for (int i = 0; i < n; ++i)
    {
        const float px = first + static_cast<float>(i) * spread;
        const int state = stage_state(s, i);
        stage_mark(list, px, line_y, state, clock, 16.0f);
        text(list, state == 1 ? env.fonts.semibold : env.fonts.regular, s.stages[i], px, line_y + 52.0f, 22.0f,
             state == 1 ? theme::ink : state == 0 ? theme::ink_2 : theme::ink_3, Align::center);
    }
    draw_chip(list, env, hui::ui::Button::circle, "Cancel", cx - chip_width(env, "Cancel") * 0.5f, 1034.0f);
}

} // namespace

void draw_launch(hui::gfx::DrawList &list, const Env &env, const LaunchState &s, float clock, float t)
{
    list.push_opacity(t);
    launch_screen(list, env, s, clock);
    list.pop_opacity();
}
// ---------------------------------------------------------------------------

// A question about a title: a dialog in the middle of the screen. The poster and the title say what it is
// about, the choices are large buttons (the first one, the likely one, is where the cursor starts), and the
// buttons to press are shown under it. It arrives from a little smaller, and leaves the same way.
void draw_dialog(hui::gfx::DrawList &list, const Env &env, const DropdownState &s, float t)
{
    constexpr float kW = 800.0f, kPad = 40.0f, kPosterW = 124.0f, kPosterH = 186.0f, kRow = 78.0f, kGap = 12.0f;
    scrim(list, t, 0.80f);
    const int n = static_cast<int>(s.options.size());
    const float choices_h = static_cast<float>(n) * kRow + static_cast<float>(std::max(0, n - 1)) * kGap;
    const float h = kPad + kPosterH + 38.0f + choices_h + kPad;
    const Rect r{(theme::kScreenW - kW) * 0.5f, (theme::kScreenH - h) * 0.5f - 34.0f, kW, h};
    list.push_opacity(t);
    list.push_transform(0.95f + 0.05f * t, r.cx(), r.cy(), 0.0f, 0.0f);
    panel(list, r, 46.0f, 1.0f);

    // What it is about: the poster, then the name, the line under it and how far it is watched.
    const Rect poster{r.x + kPad, r.y + kPad, kPosterW, kPosterH};
    if (s.poster != 0)
        list.image(s.poster, poster, kFullUv, Color::rgb(0xffffff), 18.0f);
    else
        list.gradient_rect(poster, 18.0f, Color::rgb(0x2a2540), Color::rgb(0x14111f));
    list.bordered_rect(poster, 18.0f, Color::rgb(0xffffff, 0.0f), 1.5f, theme::hairline);
    const float tx = poster.x + poster.w + 32.0f, tw = r.x + r.w - kPad - tx;
    std::vector<std::string> head = env.fonts.semibold.font->wrap(s.title, 36.0f, tw);
    if (head.size() > 2)
    {
        head.resize(2);
        head[1] = env.fonts.semibold.font->fit(head[1] + " \xE2\x80\xA6", 36.0f, tw);
    }
    std::vector<std::string> sub;
    if (!s.message.empty())
    {
        sub = env.fonts.regular.font->wrap(s.message, 26.0f, tw);
        if (sub.size() > 2)
        {
            sub.resize(2);
            sub[1] = env.fonts.regular.font->fit(sub[1] + " \xE2\x80\xA6", 26.0f, tw);
        }
    }
    const bool bar = s.progress >= 0.0f;
    const float block = static_cast<float>(head.size()) * 44.0f + (sub.empty() ? 0.0f : 10.0f + static_cast<float>(sub.size()) * 34.0f) +
                        (bar ? 30.0f : 0.0f);
    float y = poster.cy() - block * 0.5f;
    for (const std::string &line : head)
    {
        text(list, env.fonts.semibold, line, tx, y + 34.0f, 36.0f, theme::ink, Align::left, -0.4f);
        y += 44.0f;
    }
    if (!sub.empty())
    {
        y += 10.0f;
        for (const std::string &line : sub)
        {
            text(list, env.fonts.regular, line, tx, y + 26.0f, 26.0f, theme::ink_2);
            y += 34.0f;
        }
    }
    if (bar)
    {
        const float bw = std::min(tw, 340.0f);
        list.rounded_rect({tx, y + 12.0f, bw, 8.0f}, 4.0f, Color::rgb(0xffffff, 0.14f));
        list.rounded_rect({tx, y + 12.0f, std::max(8.0f, bw * std::min(1.0f, s.progress)), 8.0f}, 4.0f, theme::accent_soft);
    }

    // The choices.
    float cy = poster.y + kPosterH + 38.0f;
    for (int i = 0; i < n; ++i)
    {
        const Rect row{r.x + kPad, cy, kW - 2.0f * kPad, kRow};
        const bool selected = i == s.selected, danger = i == s.danger;
        if (selected)
        {
            list.glow(row, 28.0f, 24.0f, (danger ? Color::rgb(0xff5d5d) : theme::accent).with_alpha(0.34f));
            list.bordered_rect(row, 28.0f, danger ? Color::rgb(0x3a1820) : theme::surface_hi, 3.0f, Color::rgb(0xffffff));
        }
        else
        {
            list.rounded_rect(row, 28.0f, Color::rgb(0xffffff, 0.055f));
        }
        const hui::ui::FontRef &face = selected ? env.fonts.semibold : env.fonts.regular;
        const Color ink = danger ? Color::rgb(0xff8585) : selected ? theme::ink : theme::ink_2;
        text(list, face, face.font->fit(s.options[static_cast<std::size_t>(i)], 30.0f, row.w - 150.0f), row.x + 36.0f, row.cy() + 10.0f,
             30.0f, ink);
        if (selected)
            hui::ui::draw_button(list, env.fonts, hui::ui::GlyphStyle::dark(), hui::ui::Button::cross, row.x + row.w - 36.0f - 38.0f,
                                 row.cy(), 38.0f);
        cy += kRow + kGap;
    }

    // What to press.
    const hui::ui::Hint hints[] = {{hui::ui::Button::cross, "Select"}, {hui::ui::Button::circle, "Back"}};
    hui::ui::HintLayout layout;
    layout.size = 36.0f;
    layout.text_size = 24.0f;
    layout.item_gap = 40.0f;
    layout.cy = r.y + r.h + 62.0f;
    layout.font = &env.fonts.regular;
    const float hw = hui::ui::measure_hints(env.fonts, hints, 2, layout);
    // On a plate of its own: the page behind it is still there.
    list.bordered_rect({r.cx() - hw * 0.5f - 34.0f, layout.cy - 32.0f, hw + 68.0f, 64.0f}, 32.0f, Color::rgb(0x0d0b16, 0.97f), 1.5f,
                       theme::hairline);
    hui::ui::draw_hints(list, env.fonts, hui::ui::GlyphStyle::dark(), hints, 2, r.cx() - hw * 0.5f, false, layout);
    list.pop_transform();
    list.pop_opacity();
}
void draw_dropdown(hui::gfx::DrawList &list, const Env &env, const DropdownState &s, float t)
{
    if (s.dialog)
    {
        draw_dialog(list, env, s, t);
        return;
    }
    scrim(list, t, 0.72f);
    const int visible = static_cast<int>(std::min<std::size_t>(s.options.size(), 9));
    // A confirmation has a line under its title (what it is about).
    const float extra = s.message.empty() ? 0.0f : 46.0f;
    const Rect r{theme::kScreenW * 0.5f - 310.0f, 130.0f, 620.0f, 150.0f + extra + static_cast<float>(visible) * 76.0f};
    list.push_opacity(t);
    list.push_transform(0.96f + 0.04f * t, r.cx(), r.y, 0.0f, 0.0f);
    panel(list, r, 34.0f, 0.97f);
    text(list, env.fonts.semibold, env.fonts.semibold.font->fit(s.title, 34.0f, r.w - 88.0f), r.x + 44.0f, r.y + 82.0f,
         34.0f, theme::ink);
    if (!s.message.empty())
        text(list, env.fonts.regular, env.fonts.regular.font->fit(s.message, 26.0f, r.w - 88.0f), r.x + 44.0f, r.y + 128.0f,
             26.0f, theme::ink_2);
    list.push_clip({r.x, r.y + 110.0f + extra, r.w, r.h - 126.0f - extra});
    for (int i = 0; i < static_cast<int>(s.options.size()); ++i)
    {
        const Rect row{r.x + 28.0f, r.y + 118.0f + extra + static_cast<float>(i) * 76.0f, r.w - 56.0f, 66.0f};
        const bool selected = i == s.selected;
        const bool danger = i == s.danger;
        if (selected)
        {
            list.glow(row, 20.0f, 20.0f, (danger ? Color::rgb(0xff5d5d) : theme::accent).with_alpha(0.32f));
            list.bordered_rect(row, 20.0f, theme::surface_hi, 3.0f, Color::rgb(0xffffff));
        }
        else
        {
            list.rounded_rect(row, 20.0f, Color::rgb(0xffffff, 0.05f));
        }
        const bool active = i == s.active;
        if (active)
            tick(list, row.x + 26.0f, row.cy(), 24.0f, theme::accent_soft);
        const hui::ui::FontRef &face = active ? env.fonts.semibold : env.fonts.regular;
        text(list, face, face.font->fit(s.options[i], 27.0f, row.w - 110.0f), row.x + 70.0f, row.cy() + 9.0f, 27.0f,
             danger ? Color::rgb(0xff8585) : active || selected ? theme::ink : theme::ink_2);
    }
    list.pop_clip();
    list.pop_transform();
    list.pop_opacity();
}

// ---------------------------------------------------------------------------

void draw_boot_mark(hui::gfx::DrawList &list, const Icons &icons, float glow, float scale)
{
    const float cx = theme::kScreenW * 0.5f, cy = 470.0f;
    if (glow > 0.001f)
        list.glow({cx - 110.0f, cy - 110.0f, 220.0f, 220.0f}, 110.0f, 150.0f, theme::accent.with_alpha(glow));
    const std::uint32_t mark = icons.logo_xl != 0 ? icons.logo_xl : icons.logo;
    if (mark == 0)
        return;
    list.push_transform(scale, cx, cy, 0.0f, 0.0f);
    list.image(mark, {cx - 72.0f, cy - 72.0f, 144.0f, 144.0f}, kFullUv, Color::rgb(0xffffff));
    // The name under it (a picture of 700 x 130 px, 350 x 65 here; its baseline is 45 down): the console's
    // start-up pictures have it too, so it is there from the first frame.
    if (icons.name != 0)
        list.image(icons.name, {cx - 175.0f, cy + 156.0f - 45.0f, 350.0f, 65.0f}, kFullUv, Color::rgb(0xffffff));
    list.pop_transform();
}

void draw_boot(hui::gfx::DrawList &list, const Env &env, float t, float out)
{
    const float W = theme::kScreenW;
    const float leave = hui::tween::cubic_out(hui::tween::clamp01(out));
    // No ground of its own: it is drawn over the page's light (the backdrop; see apply_page_backdrop), the
    // same light the Board has, so the console's start-up picture, this screen and the Board are all one
    // background. The mark and the name are already in the start-up picture, so they are there from the
    // first frame; the glow under them and the bar come in after.
    const float in = hui::tween::cubic_out(hui::tween::clamp01((t - 0.35f) / 0.7f));
    const float breathe = 0.5f + 0.5f * std::sin(t * 2.6f);
    const float cx = W * 0.5f, cy = 470.0f;
    list.push_opacity(1.0f - leave);
    draw_boot_mark(list, env.icons, in * (0.12f + 0.10f * breathe), 1.0f + 0.05f * leave);
    {
        list.push_opacity(in);
        // A short bar that travels back and forth: the app is working, and it is not stuck.
        const Rect track{cx - 90.0f, cy + 200.0f, 180.0f, 4.0f};
        list.rounded_rect(track, 2.0f, Color::rgb(0xffffff, 0.10f));
        const float pos = 0.5f - 0.5f * std::cos(t * 2.4f);
        list.rounded_rect({track.x + (track.w - 64.0f) * pos, track.y, 64.0f, track.h}, 2.0f, theme::accent_soft);
        list.pop_opacity();
    }
    list.pop_opacity();
}
// ---------------------------------------------------------------------------

void draw_sign_in(hui::gfx::DrawList &list, const Env &env, const SignInState &s, float clock, float t)
{
    scrim(list, t, 0.94f);
    const Rect r{260.0f, 140.0f, 1400.0f, 800.0f};
    list.push_opacity(t);
    list.push_transform(0.97f + 0.03f * t, r.cx(), r.cy(), 0.0f, 0.0f);
    panel(list, r, 40.0f, 0.98f);
    if (env.icons.logo != 0)
        list.image(env.icons.logo, {r.x + 64.0f, r.y + 52.0f, 76.0f, 76.0f}, kFullUv, Color::rgb(0xffffff));
    text(list, env.fonts.display, "Sign in to Stremio", r.x + 164.0f, r.y + 112.0f, 46.0f, theme::ink, Align::left, -0.8f);

    // The QR code, on white so any phone can read it.
    const Rect qr{r.x + 64.0f, r.y + 190.0f, 440.0f, 440.0f};
    list.rounded_rect(qr, 26.0f, Color::rgb(0xffffff));
    if (s.qr != 0)
        list.image(s.qr, qr.inset(22.0f), kFullUv, Color::rgb(0xffffff));

    const float x = qr.x + qr.w + 70.0f;
    auto step = [&](int n, const char *line, float y) {
        list.circle(x + 20.0f, y - 10.0f, 20.0f, theme::accent);
        char digit[4];
        std::snprintf(digit, sizeof(digit), "%d", n);
        text(list, env.fonts.semibold, digit, x + 20.0f, y - 1.0f, 24.0f, Color::rgb(0xffffff), Align::center);
        text(list, env.fonts.regular, line, x + 58.0f, y, 28.0f, theme::ink_2);
    };
    step(1, "Scan the code with your phone", r.y + 218.0f);
    step(2, "or open this address on any device", r.y + 282.0f);
    text(list, env.fonts.semibold, s.link, x + 58.0f, r.y + 346.0f, 38.0f, theme::accent_soft);
    step(3, "and enter this code", r.y + 428.0f);
    const Rect code{x + 58.0f, r.y + 462.0f, 520.0f, 124.0f};
    list.rounded_rect(code, 26.0f, theme::surface);
    text(list, env.fonts.display, s.code, code.cx(), code.cy() + 34.0f, 92.0f, theme::ink, Align::center, 14.0f);
    // Waiting, plainly.
    const float pulse = 0.5f + 0.5f * std::sin(clock * 4.0f);
    list.circle(x + 70.0f, r.y + 640.0f, 8.0f, theme::accent_soft.with_alpha(0.4f + 0.6f * pulse));
    text(list, env.fonts.regular, s.status, x + 96.0f, r.y + 648.0f, 26.0f, theme::ink_3);
    draw_chip(list, env, hui::ui::Button::circle, "Cancel", r.x + 64.0f, r.y + r.h - 52.0f);
    list.pop_transform();
    list.pop_opacity();
}

// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------

void draw_toast(hui::gfx::DrawList &list, const Env &env, const ToastState &s, float t)
{
    const float text_w = env.fonts.semibold.measure(s.message, 27.0f);
    const float w = std::min(1500.0f, 150.0f + text_w);
    const Rect r{theme::kScreenW * 0.5f - w * 0.5f, 936.0f + 22.0f * (1.0f - t), w, 84.0f};
    const Color edge = s.error ? Color::rgb(0xe5566a) : theme::accent;
    list.push_opacity(t);
    list.shadow({r.x + 6, r.y + 18, r.w - 12, r.h - 8}, 42.0f, 36.0f, Color::rgb(0x000000, 0.5f));
    list.glow(r, 42.0f, 20.0f, edge.with_alpha(0.22f));
    list.bordered_rect(r, 42.0f, Color::rgb(0x100d1d, 0.97f), 2.5f, edge);
    list.circle(r.x + 46.0f, r.cy(), 17.0f, edge);
    text(list, env.fonts.semibold, s.error ? "!" : "i", r.x + 46.0f, r.cy() + 9.0f, 24.0f, Color::rgb(0xffffff), Align::center);
    text(list, env.fonts.semibold, env.fonts.semibold.font->fit(s.message, 27.0f, w - 130.0f), r.x + 84.0f, r.cy() + 10.0f,
         27.0f, theme::ink);
    list.pop_opacity();
}

} // namespace sx
