#include "profiles.hpp"

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

constexpr std::uint32_t kSwatches[6] = {0x7b5bf5, 0x1d8cf0, 0x1fa37a, 0xe0803a, 0xc0475c, 0x2fb0b8};

Color tone(std::uint32_t rgb, float alpha = 1.0f)
{
    return Color::rgb(rgb, alpha);
}

// The colour mixed towards white (f > 0) or black (f < 0).
Color mix(std::uint32_t rgb, float f)
{
    const float r = static_cast<float>((rgb >> 16) & 0xff) / 255.0f, g = static_cast<float>((rgb >> 8) & 0xff) / 255.0f,
                b = static_cast<float>(rgb & 0xff) / 255.0f;
    const float k = f >= 0.0f ? 1.0f : 0.0f, a = std::fabs(f);
    return Color{r + (k - r) * a, g + (k - g) * a, b + (k - b) * a, 1.0f};
}

std::string initial(const std::string &name)
{
    if (name.empty())
        return "?";
    std::size_t n = 1;
    const unsigned char c = static_cast<unsigned char>(name[0]);
    if (c >= 0xf0)
        n = 4;
    else if (c >= 0xe0)
        n = 3;
    else if (c >= 0xc0)
        n = 2;
    std::string out = name.substr(0, n);
    if (n == 1 && out[0] >= 'a' && out[0] <= 'z')
        out[0] = static_cast<char>(out[0] - 'a' + 'A');
    return out;
}

void avatar(hui::gfx::DrawList &list, const Env &env, const Rect &r, std::uint32_t colour, const std::string &name)
{
    const float radius = r.w * 0.27f;
    list.shadow({r.x + r.w * 0.06f, r.y + r.h * 0.12f, r.w * 0.88f, r.h * 0.88f}, radius, r.w * 0.20f, Color::rgb(0x000000, 0.45f));
    list.gradient_rect(r, radius, mix(colour, 0.16f), mix(colour, -0.22f));
    const float size = r.h * 0.46f;
    text(list, env.fonts.display, initial(name), r.cx(), r.cy() + size * 0.36f, size, Color::rgb(0xffffff), Align::center);
}

// The ring that says "this one": white, with the violet light round it.
void focus_glow(hui::gfx::DrawList &list, const Rect &r, float radius, float glow_alpha = 0.34f)
{
    list.glow(r, radius, 26.0f, theme::accent.with_alpha(glow_alpha));
}

void focus_ring(hui::gfx::DrawList &list, const Rect &r, float radius)
{
    list.bordered_rect(r, radius, Color::rgb(0xffffff, 0.0f), 4.0f, Color::rgb(0xffffff));
}

void plus(hui::gfx::DrawList &list, float cx, float cy, float size, Color colour)
{
    list.line(cx - size * 0.5f, cy, cx + size * 0.5f, cy, 5.0f, colour);
    list.line(cx, cy - size * 0.5f, cx, cy + size * 0.5f, 5.0f, colour);
}

void tick(hui::gfx::DrawList &list, float cx, float cy, float size, Color colour)
{
    list.line(cx - size * 0.5f, cy + size * 0.02f, cx - size * 0.15f, cy + size * 0.38f, 4.0f, colour);
    list.line(cx - size * 0.15f, cy + size * 0.38f, cx + size * 0.5f, cy - size * 0.38f, 4.0f, colour);
}

void heading(hui::gfx::DrawList &list, const Env &env, const std::string &value, float x, float baseline, float size,
             Align align)
{
    text(list, env.fonts.display, value, x, baseline, size, theme::ink, align, -0.02f * size);
}

void hints(hui::gfx::DrawList &list, const Env &env, bool on_profile, bool can_leave)
{
    hui::ui::Hint h[3];
    int n = 0;
    h[n++] = {hui::ui::Button::cross, on_profile ? "Watch" : "Add"};
    if (on_profile)
        h[n++] = {hui::ui::Button::square, "Edit"};
    if (can_leave)
        h[n++] = {hui::ui::Button::circle, "Back"};
    hui::ui::HintLayout layout;
    layout.size = 36.0f;
    layout.text_size = 24.0f;
    layout.item_gap = 44.0f;
    layout.cy = 1006.0f;
    layout.font = &env.fonts.regular;
    const float w = hui::ui::measure_hints(env.fonts, h, n, layout);
    hui::ui::draw_hints(list, env.fonts, hui::ui::GlyphStyle::dark(), h, n, theme::kScreenW * 0.5f - w * 0.5f, false, layout);
}

// ---- the picker ----------------------------------------------------------------------------------------

constexpr float kBig = 320.0f, kNear = 168.0f, kFar = 124.0f, kSpace = 70.0f;

float tile_size(float d)
{
    const float a = std::fabs(d);
    if (a <= 1.0f)
        return kBig + (kNear - kBig) * a;
    return kNear + (kFar - kNear) * std::min(1.0f, a - 1.0f);
}

float tile_alpha(float d)
{
    const float a = std::fabs(d);
    if (a <= 1.0f)
        return 1.0f + (0.62f - 1.0f) * a;
    return 0.62f + (0.34f - 0.62f) * std::min(1.0f, a - 1.0f);
}

// Where a tile's centre is, left of the middle (negative d) or right of it. At whole numbers it is the sum of
// the half-sizes and gaps between the tiles; in between it slides along, so the row glides.
float tile_offset(float d)
{
    const float a = std::fabs(d);
    float at[6] = {0.0f};
    for (int k = 1; k < 6; ++k)
        at[k] = at[k - 1] + tile_size(static_cast<float>(k - 1)) * 0.5f + kSpace + tile_size(static_cast<float>(k)) * 0.5f;
    const int k = std::min(4, static_cast<int>(a));
    const float f = std::min(1.0f, a - static_cast<float>(k));
    const float v = at[k] + (at[k + 1] - at[k]) * f;
    return d < 0.0f ? -v : v;
}

Color lerp_colour(std::uint32_t a, std::uint32_t b, float f)
{
    const Color ca = Color::rgb(a), cb = Color::rgb(b);
    return Color{ca.r + (cb.r - ca.r) * f, ca.g + (cb.g - ca.g) * f, ca.b + (cb.b - ca.b) * f, 1.0f};
}

// ---- the editor -------------------------------------------------------------------------------------------

void scrim(hui::gfx::DrawList &list, float t, float strength)
{
    list.rounded_rect({0, 0, theme::kScreenW, theme::kScreenH}, 0, theme::ground.with_alpha(strength * t));
}

void panel(hui::gfx::DrawList &list, const Rect &r, float radius)
{
    list.shadow({r.x + 10, r.y + 30, r.w - 20, r.h - 20}, radius, 56.0f, Color::rgb(0x000000, 0.5f));
    list.bordered_rect(r, radius, Color::rgb(0x0d0b16, 0.995f), 1.5f, theme::hairline);
}

void row_frame(hui::gfx::DrawList &list, const Rect &r, bool on, float radius = 26.0f)
{
    if (on)
    {
        list.glow(r, radius, 22.0f, theme::accent.with_alpha(0.32f));
        list.bordered_rect(r, radius, theme::surface_hi, 3.0f, Color::rgb(0xffffff));
    }
    else
    {
        list.rounded_rect(r, radius, Color::rgb(0xffffff, 0.055f));
    }
}

// The fields, from y down; returns where they end. Plain rows, a ring that glides to the one under the cursor
// (pos is a fraction while it moves), then the words.
float editor_body(hui::gfx::DrawList &list, const Env &env, const EditorState &s, float x, float y, float w)
{
    constexpr float kRow = 76.0f, kGap = 12.0f, kColour = 104.0f;
    const ProfileEntry &p = s.profile;
    const int last = s.is_new ? 3 : 4;
    Rect rows[5];
    rows[0] = {x, y, w, kRow};
    rows[1] = {x, y + (kRow + kGap), w, kColour};
    rows[2] = {x, rows[1].y + kColour + kGap, w, kRow};
    rows[3] = {x, rows[2].y + kRow + 34.0f, w, kRow};
    rows[4] = {x, rows[3].y + kRow + kGap, w, kRow};
    const float pos = std::clamp(s.pos >= 0.0f ? s.pos : static_cast<float>(s.row), 0.0f, static_cast<float>(last));
    const int near = static_cast<int>(std::lround(pos));
    const auto weight = [&](int i) { return std::max(0.0f, 1.0f - std::fabs(pos - static_cast<float>(i))); };

    for (int i = 0; i <= last; ++i)
    {
        if (i == 3)
            list.rounded_rect(rows[i], 26.0f, theme::accent); // Done is always violet
        else
            list.rounded_rect(rows[i], 26.0f, Color::rgb(0xffffff, 0.055f + 0.075f * weight(i)));
    }
    {
        const int lo = static_cast<int>(std::floor(pos)), hi = std::min(last, lo + 1);
        const float f = pos - static_cast<float>(lo);
        const auto mixf = [&](float a, float b) { return a + (b - a) * f; };
        const Rect ring{mixf(rows[lo].x, rows[hi].x), mixf(rows[lo].y, rows[hi].y), mixf(rows[lo].w, rows[hi].w), mixf(rows[lo].h, rows[hi].h)};
        const bool remove = near == 4;
        list.glow(ring, 26.0f, 22.0f, (remove ? Color::rgb(0xff5d5d) : theme::accent).with_alpha(near == 3 ? 0.42f : 0.32f));
        list.bordered_rect(ring, 26.0f, Color::rgb(0xffffff, 0.0f), 3.0f, Color::rgb(0xffffff));
    }

    // Name
    Rect r = rows[0];
    text(list, env.fonts.regular, "Name", r.x + 32.0f, r.cy() + 9.0f, 26.0f, theme::ink_2);
    text(list, env.fonts.semibold, p.name, r.x + r.w - 32.0f, r.cy() + 10.0f, 30.0f, theme::ink, Align::right);
    // Colour
    r = rows[1];
    text(list, env.fonts.regular, "Colour", r.x + 32.0f, r.cy() + 9.0f, 26.0f, theme::ink_2);
    const float step = 62.0f;
    const float sx = r.x + r.w - 32.0f - 6.0f * step + (step - 48.0f);
    for (int i = 0; i < 6; ++i)
    {
        const float cx = sx + static_cast<float>(i) * step + 24.0f;
        list.circle(cx, r.cy(), 22.0f, tone(kSwatches[i]));
        if (i == s.colour_index)
        {
            list.bordered_rect({cx - 27.0f, r.cy() - 27.0f, 54.0f, 54.0f}, 27.0f, Color::rgb(0xffffff, 0.0f), 3.0f, Color::rgb(0xffffff));
            tick(list, cx, r.cy(), 22.0f, Color::rgb(0xffffff));
        }
    }
    // Account
    r = rows[2];
    text(list, env.fonts.regular, "Account", r.x + 32.0f, r.cy() + 9.0f, 26.0f, theme::ink_2);
    const bool in = !p.email.empty();
    const char *value = in ? p.email.c_str() : s.is_new ? "Sign in after creating" : "Sign in";
    text(list, in ? env.fonts.regular : env.fonts.semibold, value, r.x + r.w - 32.0f, r.cy() + 9.0f, 26.0f,
         in ? theme::ink_2 : theme::accent_soft, Align::right);
    // Done, Remove
    r = rows[3];
    text(list, env.fonts.semibold, s.is_new ? "Create profile" : "Done", r.cx(), r.cy() + 11.0f, 30.0f, Color::rgb(0xffffff), Align::center);
    if (!s.is_new)
    {
        r = rows[4];
        text(list, env.fonts.semibold, "Remove profile", r.cx(), r.cy() + 11.0f, 30.0f, Color::rgb(0xff8585), Align::center);
    }
    return rows[last].y + rows[last].h;
}

void editor_hints(hui::gfx::DrawList &list, const Env &env, float cx, float cy)
{
    const hui::ui::Hint h[] = {{hui::ui::Button::cross, "Select"}, {hui::ui::Button::circle, "Back"}};
    hui::ui::HintLayout layout;
    layout.size = 36.0f;
    layout.text_size = 24.0f;
    layout.item_gap = 40.0f;
    layout.cy = cy;
    layout.font = &env.fonts.regular;
    const float w = hui::ui::measure_hints(env.fonts, h, 2, layout);
    list.bordered_rect({cx - w * 0.5f - 34.0f, cy - 32.0f, w + 68.0f, 64.0f}, 32.0f, Color::rgb(0x0d0b16, 0.97f), 1.5f, theme::hairline);
    hui::ui::draw_hints(list, env.fonts, hui::ui::GlyphStyle::dark(), h, 2, cx - w * 0.5f, false, layout);
}


} // namespace

std::uint32_t profile_colour(int index)
{
    return kSwatches[std::clamp(index, 0, kProfileColours - 1)];
}

int profile_colour_index(std::uint32_t rgb)
{
    int best = 0, best_d = 1 << 30;
    for (int i = 0; i < kProfileColours; ++i)
    {
        const int dr = static_cast<int>((rgb >> 16) & 0xff) - static_cast<int>((kSwatches[i] >> 16) & 0xff);
        const int dg = static_cast<int>((rgb >> 8) & 0xff) - static_cast<int>((kSwatches[i] >> 8) & 0xff);
        const int db = static_cast<int>(rgb & 0xff) - static_cast<int>(kSwatches[i] & 0xff);
        const int d = dr * dr + dg * dg + db * db;
        if (d < best_d)
        {
            best_d = d;
            best = i;
        }
    }
    return best;
}

void draw_profile_picker(hui::gfx::DrawList &list, const Env &env, const ProfilesState &s, float t)
{
    list.push_opacity(t);
    text(list, env.fonts.regular, "Who's watching?", theme::kScreenW * 0.5f, 188.0f, 36.0f, theme::ink_2, Align::center);
    const int count = static_cast<int>(s.items.size());
    const int n = count + 1;
    const float fp = std::clamp(s.focus, 0.0f, static_cast<float>(n - 1));
    const float cx = theme::kScreenW * 0.5f;
    const int nearest = static_cast<int>(std::lround(fp));
    // The light behind the middle one takes the colour of the profile it is gliding to ("Add" is violet).
    {
        const auto colour_of = [&](int i) { return i < count ? s.items[static_cast<std::size_t>(i)].colour : 0x7b5bf5u; };
        const int lo = std::min(n - 1, static_cast<int>(std::floor(fp))), hi = std::min(n - 1, lo + 1);
        list.glow({cx - 160.0f, 330.0f, 320.0f, 320.0f}, 86.0f, 220.0f,
                  lerp_colour(colour_of(lo), colour_of(hi), fp - std::floor(fp)).with_alpha(0.30f));
    }
    // The far ones first, so the middle one is on top.
    std::vector<int> order(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        order[static_cast<std::size_t>(i)] = i;
    std::sort(order.begin(), order.end(), [&](int a, int b) { return std::fabs(a - fp) > std::fabs(b - fp); });
    for (const int i : order)
    {
        const float d = static_cast<float>(i) - fp;
        if (std::fabs(d) > 5.0f)
            continue;
        const float size = tile_size(d);
        const float x = cx + tile_offset(d);
        const Rect r{x - size * 0.5f, 340.0f + (kBig - size) * 0.5f, size, size};
        const bool add = i == count;
        const float on = std::max(0.0f, 1.0f - std::fabs(d)); // 1 on the middle one
        if (on > 0.0f)
            focus_glow(list, r, r.w * 0.27f, 0.30f * on);
        list.push_opacity(tile_alpha(d));
        if (add)
        {
            list.bordered_rect(r, r.w * 0.27f, Color::rgb(0xffffff, 0.05f), 2.0f, Color::rgb(0xffffff, 0.30f));
            plus(list, r.cx(), r.cy(), size * 0.30f, theme::ink_2);
        }
        else
        {
            const ProfileEntry &q = s.items[static_cast<std::size_t>(i)];
            avatar(list, env, r, q.colour, q.name);
        }
        list.pop_opacity();
        if (on > 0.0f)
        {
            list.push_opacity(on);
            focus_ring(list, r, r.w * 0.27f);
            list.pop_opacity();
        }
    }
    // The name and the account under the middle one; they fade as the row glides and the next ones arrive.
    const float fade = std::clamp(1.0f - std::fabs(fp - static_cast<float>(nearest)) * 3.0f, 0.0f, 1.0f);
    list.push_opacity(fade);
    if (nearest < count)
    {
        const ProfileEntry &p = s.items[static_cast<std::size_t>(nearest)];
        heading(list, env, p.name, cx, 790.0f, 64.0f, Align::center);
        text(list, env.fonts.regular, p.email.empty() ? "Not signed in" : p.email, cx, 842.0f, 26.0f,
             p.email.empty() ? theme::accent_soft : theme::ink_3, Align::center);
    }
    else
    {
        heading(list, env, "Add profile", cx, 790.0f, 64.0f, Align::center);
        text(list, env.fonts.regular, "Its own sign-in, library and Continue Watching", cx, 842.0f, 26.0f, theme::ink_3, Align::center);
    }
    list.pop_opacity();
    hints(list, env, nearest < count, s.can_leave);
    list.pop_opacity();
}

void draw_profile_editor(hui::gfx::DrawList &list, const Env &env, const EditorState &state, float t)
{
    constexpr float kW = 880.0f, kPad = 44.0f;
    scrim(list, t, 0.94f);
    const float h = state.is_new ? 658.0f : 734.0f;
    const Rect r{(theme::kScreenW - kW) * 0.5f, (theme::kScreenH - h) * 0.5f - 34.0f, kW, h};
    list.push_opacity(t);
    list.push_transform(0.96f + 0.04f * t, r.cx(), r.cy(), 0.0f, (1.0f - t) * 12.0f);
    panel(list, r, 46.0f);
    const Rect a{r.x + kPad, r.y + kPad, 132.0f, 132.0f};
    avatar(list, env, a, state.profile.colour, state.profile.name.empty() ? "?" : state.profile.name);
    heading(list, env, state.is_new ? "New profile" : "Edit profile", a.x + a.w + 36.0f, a.cy() + 14.0f, 44.0f, Align::left);
    editor_body(list, env, state, r.x + kPad, r.y + kPad + 132.0f + 36.0f, kW - 2.0f * kPad);
    editor_hints(list, env, r.cx(), r.y + r.h + 62.0f);
    list.pop_transform();
    list.pop_opacity();
}

} // namespace sx
