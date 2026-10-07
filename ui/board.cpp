#include "board.hpp"

#include "cards.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace sx
{

using hui::Direction;
using hui::gfx::Align;
using hui::gfx::Color;
using hui::gfx::kFullUv;
using hui::gfx::Rect;
using hui::ui::text;
namespace tween = hui::tween;

namespace
{

// The rows (virtual pixels).
constexpr float kCardW = 232.0f;
constexpr float kCardH = 348.0f;
constexpr float kPitch = 260.0f;
constexpr float kRowH = 500.0f;
constexpr float kRowsTop = 500.0f; // top of the first row's header, under the hero
constexpr float kRowsTopSearch = 176.0f; // and with no hero
constexpr float kPop = 1.07f;      // the focused card's size

std::string join_meta(const Title &t)
{
    std::string out;
    for (const std::string *part : {&t.year, &t.runtime, &t.genres})
    {
        if (part->empty())
            continue;
        if (!out.empty())
            out += "   \xC2\xB7   ";
        out += *part;
    }
    return out;
}

} // namespace

namespace
{

// The wallpaper is laid under the page with long, eased ramps: no box, no band.
float eased(float t)
{
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

// Strips snapped to whole pixels and not overlapping: a seam shows wherever two translucent
// edges meet at a fraction of a pixel or overlap.
float cut(float from, float len, int i, int n)
{
    return std::floor(from + len * static_cast<float>(i) / static_cast<float>(n));
}

void ramp_v(hui::gfx::DrawList &l, float x, float y, float w, float h, float a0, float a1, Color c, int n = 28)
{
    for (int i = 0; i < n; ++i)
    {
        const float y0 = cut(y, h, i, n), y1 = cut(y, h, i + 1, n);
        const float t0 = static_cast<float>(i) / n, t1 = static_cast<float>(i + 1) / n;
        l.gradient_rect({x, y0, w, y1 - y0}, 0, c.with_alpha(a0 + (a1 - a0) * eased(t0)),
                        c.with_alpha(a0 + (a1 - a0) * eased(t1)));
    }
}

void ramp_h(hui::gfx::DrawList &l, float x, float y, float w, float h, float a0, float a1, Color c, int n = 28)
{
    for (int i = 0; i < n; ++i)
    {
        const float x0 = cut(x, w, i, n), x1 = cut(x, w, i + 1, n);
        const float t0 = static_cast<float>(i) / n, t1 = static_cast<float>(i + 1) / n;
        l.gradient_rect_h({x0, y, x1 - x0, h}, 0, c.with_alpha(a0 + (a1 - a0) * eased(t0)),
                          c.with_alpha(a0 + (a1 - a0) * eased(t1)));
    }
}

} // namespace
void Board::set_content(const BoardContent *content)
{
    content_ = content;
    const std::size_t rows = content ? content->rows.size() : 0;
    keys_.clear();
    for (std::size_t r = 0; r < rows; ++r)
        keys_.push_back(content->rows[r].key);
    col_.assign(rows, 0);
    across_.assign(rows, {});
    scale_.assign(rows, {});
    for (std::size_t r = 0; r < rows; ++r)
        scale_[r].assign(content->rows[r].items.size(), tween::Spring{1.0f, 0.0f, 1.0f});
    row_ = 0;
    moved_ = false;
    while (row_ + 1 < static_cast<int>(rows) && content->rows[row_].items.empty())
        ++row_;
    down_.position.snap(0.0f);
    shown_ = before_ = Key{};
    ring_.snap(card_rect(row_, 0, true));
    focus_on_.snap(1.0f);
}

void Board::refresh()
{
    if (!content_)
        return;
    const std::size_t rows = content_->rows.size();
    if (keys_.empty())
    {
        // The first rows have arrived: start on the first one that has cards.
        set_content(content_);
        return;
    }
    std::vector<int> col(rows, 0);
    std::vector<hui::ui::Scroller> across(rows);
    std::vector<std::vector<hui::tween::Spring>> scale(rows);
    std::string focused_key = row_ < static_cast<int>(keys_.size()) ? keys_[row_] : std::string();
    int new_row = -1;
    for (std::size_t r = 0; r < rows; ++r)
    {
        const Row &row = content_->rows[r];
        const auto it = std::find(keys_.begin(), keys_.end(), row.key);
        if (it != keys_.end())
        {
            const std::size_t old = static_cast<std::size_t>(it - keys_.begin());
            col[r] = col_[old];
            across[r] = across_[old];
            scale[r] = std::move(scale_[old]);
            if (row.key == focused_key && new_row < 0)
                new_row = static_cast<int>(r);
        }
        scale[r].resize(row.items.size(), tween::Spring{1.0f, 0.0f, 1.0f});
        col[r] = std::min(col[r], std::max(0, static_cast<int>(row.items.size()) - 1));
    }
    keys_.clear();
    for (const Row &row : content_->rows)
        keys_.push_back(row.key);
    col_ = std::move(col);
    across_ = std::move(across);
    scale_ = std::move(scale);
    row_ = new_row >= 0 ? new_row : std::min(row_, std::max(0, static_cast<int>(rows) - 1));
    // Rows come in as the addons answer, in any order. Until the player has moved, the
    // first row is where they are, whatever turned up above the one that was first before.
    if (!moved_)
        row_ = 0;
    // The cursor is never left on a row that has nothing in it.
    while (row_ + 1 < static_cast<int>(rows) && content_->rows[row_].items.empty())
        ++row_;
}

void Board::enter()
{
    age_ = 0.0f;
    rail_.reset(Page::board);
    focus_on_.snap(1.0f);
}

void Board::set_search(const std::string *query)
{
    query_ = query;
    hero_ = query == nullptr;
}

const Title *Board::focused_title() const
{
    if (!content_ || row_ >= static_cast<int>(content_->rows.size()))
        return nullptr;
    const Row &row = content_->rows[row_];
    if (row.items.empty())
        return nullptr;
    return &row.items[std::min<std::size_t>(col_[row_], row.items.size() - 1)];
}

float Board::row_top(int row, bool final_position) const
{
    const float offset = final_position ? down_.position.target : down_.offset();
    return (hero_ ? kRowsTop : kRowsTopSearch) + static_cast<float>(row) * kRowH - offset;
}

Rect Board::card_rect(int row, int col, bool final_position) const
{
    float across = 0.0f;
    if (row < static_cast<int>(across_.size()))
        across = final_position ? across_[row].position.target : across_[row].offset();
    return {theme::kLeft + static_cast<float>(col) * kPitch - across, row_top(row, final_position) + 64.0f,
            kCardW, kCardH};
}

void Board::update(const hui::InputFrame &input, float dt)
{
    age_ += dt;
    clock_ += dt;
    if (!content_ || content_->rows.empty())
        return;

    if (rail_.focused())
    {
        if (input.nav == Direction::up)
            rail_.move(-1);
        else if (input.nav == Direction::down)
            rail_.move(1);
        else if (input.nav == Direction::right)
            rail_.focus(false);
    }
    else
    {
        const int rows = static_cast<int>(content_->rows.size());
        if (input.nav != Direction::none)
            moved_ = true;
        int &col = col_[row_];
        const int count = static_cast<int>(content_->rows[row_].items.size());
        switch (input.nav)
        {
        case Direction::left:
            if (col == 0)
                rail_.focus(true);
            else
                --col;
            break;
        case Direction::right:
            col = std::min(col + 1, std::max(0, count - 1));
            break;
        case Direction::up:
        case Direction::down:
        {
            // Rows with nothing in them are passed over.
            const int step = input.nav == Direction::up ? -1 : 1;
            for (int r = row_ + step; r >= 0 && r < rows; r += step)
                if (!content_->rows[r].items.empty())
                {
                    row_ = r;
                    break;
                }
            break;
        }
        default:
            break;
        }
        const int n = static_cast<int>(content_->rows[row_].items.size());
        col_[row_] = std::min(col_[row_], std::max(0, n - 1));
    }
    rail_.set_page(Page::board);
    rail_.update(dt);

    // Where the rows and the focused row's strip have to be.
    down_.position.target = static_cast<float>(row_) * kRowH;
    const int c = col_[row_];
    across_[row_].reveal(static_cast<float>(c) * kPitch, static_cast<float>(c) * kPitch + kCardW, 1700.0f, 40.0f);
    for (std::size_t r = 0; r < across_.size(); ++r)
        across_[r].update(dt, 18.0f);
    down_.update(dt, 18.0f);

    // The pop of each card, and the ring that glides between them.
    for (std::size_t r = 0; r < scale_.size(); ++r)
        for (std::size_t i = 0; i < scale_[r].size(); ++i)
        {
            const bool focused = !rail_.focused() && static_cast<int>(r) == row_ && static_cast<int>(i) == col_[r];
            scale_[r][i].target = focused ? kPop : 1.0f;
            scale_[r][i].update(dt, 20.0f);
        }
    const Rect target = card_rect(row_, col_[row_], true);
    ring_.target(target);
    ring_.update(dt, 20.0f);
    focus_on_.target = rail_.focused() ? 0.0f : 1.0f;
    focus_on_.update(dt, 16.0f);

    // The hero follows the focus, crossfading from what it showed.
    const Key now{row_, col_[row_]};
    if (!(now == shown_))
    {
        before_ = shown_;
        shown_ = now;
        swap_.start(0.38f);
    }
    swap_.update(dt);
}

void Board::draw_row(hui::gfx::DrawList &list, int row, float top) const
{
    const Row &data = content_->rows[row];
    const bool current = row == row_ && !rail_.focused();
    const float row_in = tween::stagger(age_, row, 0.10f, 0.45f);
    list.push_opacity(row_in);
    text(list, env_.fonts.semibold,
         env_.fonts.semibold.font->fit(data.title, theme::kHeading, theme::kRight - theme::kLeft - 260.0f), theme::kLeft,
         top + 36.0f, theme::kHeading, current ? theme::ink : theme::ink_2, Align::left, -0.3f);
    if (data.items.empty())
        text(list, env_.fonts.regular, "Nothing here yet", theme::kLeft, top + 110.0f, theme::kSubtitle, theme::ink_3);
    // "See all": the Square glyph appears beside the row that has the focus. Continue Watching has none:
    // everything in it can be reached by scrolling.
    const bool continue_row = data.key == "continue";
    const float see_w = env_.fonts.regular.measure("See all", theme::kLabel + 2.0f);
    if (!continue_row)
    {
        text(list, env_.fonts.regular, "See all", theme::kRight - 6.0f, top + 34.0f, theme::kLabel + 2.0f,
             current ? theme::ink_2 : theme::ink_3, Align::right);
        if (current)
            hui::ui::draw_button(list, env_.fonts, hui::ui::GlyphStyle::dark(), hui::ui::Button::square,
                                 theme::kRight - 6.0f - see_w - 44.0f, top + 26.0f, 32.0f);
    }
    // On Continue Watching, Options takes the card off the row.
    if (current && continue_row && !data.items.empty())
    {
        const float lw = env_.fonts.regular.measure("Options", theme::kLabel + 2.0f);
        const float right = theme::kRight - 6.0f;
        text(list, env_.fonts.regular, "Options", right, top + 34.0f, theme::kLabel + 2.0f, theme::ink_2, Align::right);
        hui::ui::draw_button(list, env_.fonts, hui::ui::GlyphStyle::dark(), hui::ui::Button::options,
                             right - lw - 52.0f, top + 26.0f, 32.0f);
    }
    list.pop_opacity();

    // The strip is clipped at the left, under the rail's edge, and bleeds off the right.
    list.push_clip({theme::kLeft - 38.0f, top + 40.0f, theme::kScreenW, kRowH - 44.0f});
    const bool resume = data.title.find("Continue") != std::string::npos;
    for (std::size_t i = 0; i < data.items.size(); ++i)
    {
        const Rect r = card_rect(row, static_cast<int>(i), false);
        if (r.x > theme::kScreenW + 20.0f || r.x + r.w < theme::kLeft - 60.0f)
            continue;
        const float enter = tween::stagger(age_, row * 2 + static_cast<int>(i), 0.035f, 0.45f);
        const bool focused = row == row_ && static_cast<int>(i) == col_[row] && !rail_.focused();
        draw_poster_card(list, env_, data.items[i], {r.x, top + 64.0f, r.w, r.h}, scale_[row][i].value, enter, focused, resume);
    }
    list.pop_clip();
}

void Board::draw_hero(hui::gfx::DrawList &list, float offset) const
{
    const Title *now = focused_title();
    if (!now)
        return;
    const float mix = tween::cubic_out(swap_.progress());
    const Title *prev = nullptr;
    if (before_.row >= 0 && before_.row < static_cast<int>(content_->rows.size()))
    {
        const Row &row = content_->rows[before_.row];
        if (before_.col >= 0 && before_.col < static_cast<int>(row.items.size()))
            prev = &row.items[before_.col];
    }

    // The wallpaper: the focused title's backdrop over the whole screen, dimming into a glow as the
    // page scrolls. The page is laid over it with one unbroken fade from the screen's left edge
    // (nothing stops at the line where the content starts, so there is no stripe or cut there)
    // and one into the rows at the bottom.
    const float dim = 1.0f - 0.72f * tween::clamp01(offset / 450.0f);
    const float intro = tween::stagger(age_, 0, 0.0f, 0.7f);
    const Color g = theme::ground;
    const float W = theme::kScreenW, H = theme::kScreenH;
    const Rect full{0.0f, 0.0f, W, H};
    list.push_opacity(dim * intro);
    if (prev && prev->backdrop != 0 && mix < 1.0f)
        list.image(prev->backdrop, full, kFullUv, Color::rgb(0xffffff));
    if (now->backdrop != 0)
        list.image(now->backdrop, full, kFullUv, Color::rgb(0xffffff, prev ? mix : 1.0f));
    list.gradient_rect(full, 0, g.with_alpha(0.34f), g.with_alpha(0.34f));
    ramp_h(list, 0.0f, 0.0f, theme::kLeft, H, 0.93f, 0.78f, g, 12);
    ramp_h(list, theme::kLeft, 0.0f, 1000.0f, H, 0.78f, 0.0f, g, 36);
    ramp_v(list, 0.0f, 380.0f, W, H - 380.0f, 0.0f, 0.97f, g);
    list.pop_opacity();
    // The words, carried up with the page.
    list.push_transform(1.0f, 0.0f, 0.0f, 0.0f, -offset);
    const float text_in = tween::smoothstep(mix) * intro;
    const float rise = 10.0f * (1.0f - tween::smoothstep(mix));
    list.push_opacity(text_in);
    const hui::ui::FontRef &display = env_.fonts.display;
    // One line at full size; a long name steps down so two lines clear the search pill.
    float size = 68.0f, leading = 76.0f;
    std::vector<std::string> lines = display.font->wrap(now->name, size, 820.0f);
    if (lines.size() > 1)
    {
        size = 58.0f;
        leading = 66.0f;
        lines = display.font->wrap(now->name, size, 820.0f);
    }
    if (lines.size() > 2)
    {
        lines.resize(2);
        lines[1] = display.font->fit(lines[1] + " \xE2\x80\xA6", size, 820.0f);
    }
    float baseline = 240.0f - (lines.size() > 1 ? leading : 0.0f) + rise;
    for (const std::string &line : lines)
    {
        text(list, display, display.font->fit(line, size, 820.0f), theme::kLeft, baseline, size, theme::ink, Align::left,
             -1.4f);
        baseline += leading;
    }
    const std::string meta = env_.fonts.regular.font->fit(join_meta(*now), 26.0f, 880.0f);
    const float meta_w = text(list, env_.fonts.regular, meta, theme::kLeft, 294.0f + rise, 26.0f, theme::ink_2);
    draw_imdb(list, env_, now->imdb, theme::kLeft + meta_w + 30.0f, 294.0f + rise);
    hui::ui::paragraph(list, env_.fonts.regular, now->synopsis, theme::kLeft, 348.0f + rise, 26.0f, 780.0f, 38.0f,
                       theme::ink_2, 2);
    const bool resume = now->progress >= 0.0f;
    const float chip_w = draw_chip(list, env_, hui::ui::Button::cross, resume ? "Resume" : "Open", theme::kLeft,
                                   448.0f + rise, true);
    if (resume)
    {
        char left[48];
        std::snprintf(left, sizeof(left), "%d%% watched", static_cast<int>(now->progress * 100.0f + 0.5f));
        text(list, env_.fonts.regular, left, theme::kLeft + chip_w + 26.0f, 456.0f + rise, theme::kBody, theme::ink_3);
    }
    list.pop_opacity();
    list.pop_transform();
}

void Board::draw(hui::gfx::DrawList &list, hui::gfx::BackdropSpec &backdrop) const
{
    // The page: near-black with Stremio's violet light from the top right.
    backdrop.mode = hui::gfx::BackdropMode::gradient;
    backdrop.colors[0] = theme::ground;
    backdrop.colors[1] = theme::ground;
    backdrop.colors[2] = theme::accent_deep;
    backdrop.params[0] = 0.92f;
    backdrop.params[1] = 0.0f;
    backdrop.params[2] = 0.55f;
    backdrop.time = clock_;

    if (!content_ || content_->rows.empty())
    {
        // Nothing yet: say what is going on, or when a search found nothing, what to try.
        if (content_ && !content_->status.empty())
        {
            draw_status(list, env_, content_->status, 330.0f);
        }
        else if (!hero_ && query_ != nullptr)
        {
            const float in = tween::stagger(age_, 0, 0.0f, 0.5f);
            list.push_opacity(in);
            text(list, env_.fonts.semibold, "No results for \xE2\x80\x9C" + *query_ + "\xE2\x80\x9D", theme::kLeft, 330.0f,
                 44.0f, theme::ink, Align::left, -0.6f);
            text(list, env_.fonts.regular, "Check the spelling, or try a shorter name.", theme::kLeft, 392.0f,
                 theme::kSubtitle, theme::ink_2);
            list.pop_opacity();
        }
        draw_topbar(list, env_, false, age_, query_, clock_);
        rail_.draw(list, env_, age_);
        return;
    }

    const float offset = down_.offset();
    if (hero_)
        draw_hero(list, offset);

    // The glow that goes with the ring, under the cards.
    const Rect ring = ring_.value();
    const float pop_pad = kCardW * (kPop - 1.0f) * 0.5f + 8.0f;
    const Rect outer{ring.x - pop_pad, ring.y - pop_pad * 1.4f, ring.w + 2 * pop_pad, ring.h + 2 * pop_pad * 1.4f};
    const bool has_focus = focused_title() != nullptr;
    if (focus_on_.value > 0.01f && has_focus)
    {
        list.push_opacity(focus_on_.value);
        list.glow(outer, 26.0f, 34.0f, theme::accent.with_alpha(0.42f));
        list.pop_opacity();
    }
    for (int r = 0; r < static_cast<int>(content_->rows.size()); ++r)
    {
        const float top = row_top(r, false);
        if (top > theme::kScreenH || top + kRowH < 0.0f)
            continue;
        draw_row(list, r, top);
    }
    // The ring itself, over the poster it surrounds.
    if (focus_on_.value > 0.01f && has_focus)
    {
        list.push_opacity(focus_on_.value);
        list.bordered_rect(outer, 26.0f, Color::rgb(0xffffff, 0.0f), 4.0f, Color::rgb(0xffffff));
        list.pop_opacity();
    }

    // Chrome over the content: it scrolls under soft edges, not hard lines.
    // At the top of the page the backdrop shows through; once rows scroll under
    // the top bar the fade closes up so nothing collides with the search pill.
    const float closed = tween::clamp01(offset / 240.0f);
    // (Over the wallpaper they reach the screen's left edge: starting at the rail's edge they would
    // leave a step there.)
    const float edge = hero_ ? 0.0f : theme::kRailWidth;
    fade_down(list, edge, 0.0f, theme::kScreenW, 170.0f, tween::lerp(0.62f, 0.98f, closed),
              tween::lerp(0.0f, 104.0f, closed));
    fade_up(list, edge, 990.0f, theme::kScreenW, 90.0f, 0.94f);
    draw_topbar(list, env_, false, age_, query_, clock_);
    rail_.draw(list, env_, age_);
}

} // namespace sx
