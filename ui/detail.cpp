#include "detail.hpp"

#include <algorithm>
#include <cstdio>

namespace sx
{

using hui::Action;
using hui::Direction;
using hui::gfx::Align;
using hui::gfx::Color;
using hui::gfx::kFullUv;
using hui::gfx::Rect;
using hui::ui::text;
namespace tween = hui::tween;

namespace
{

constexpr Rect kPanel{1040.0f, 84.0f, 820.0f, 912.0f};
constexpr float kListTop = 214.0f;
constexpr float kEpisodeH = 116.0f;
constexpr float kEpisodePitch = 128.0f;
constexpr float kStreamH = 124.0f;
constexpr float kStreamPitch = 136.0f;
constexpr float kListViewH = 770.0f;

Color resolution_colour(const std::string &r)
{
    if (r == "4K")
        return Color::rgb(0x7b5bf5);
    if (r == "1080p")
        return Color::rgb(0x1d78d8);
    if (r == "720p")
        return Color::rgb(0x1a9474);
    return Color::rgb(0x5b5870);
}

} // namespace

Rect Detail::episode_rect(int index, bool final_position) const
{
    const float scroll = final_position ? episode_scroll_.position.target : episode_scroll_.offset();
    return {kPanel.x + 28.0f, kListTop + static_cast<float>(index) * kEpisodePitch - scroll, kPanel.w - 56.0f,
            kEpisodeH};
}

Rect Detail::stream_rect(int index, bool final_position) const
{
    const float scroll = final_position ? stream_scroll_.position.target : stream_scroll_.offset();
    return {kPanel.x + 28.0f, kListTop + static_cast<float>(index) * kStreamPitch - scroll, kPanel.w - 56.0f,
            kStreamH};
}

void Detail::set_content(const DetailContent *content)
{
    content_ = content;
    episode_ = 0;
    stream_ = 0;
    streams_wanted_ = false;
    hint_serial_ = content ? content->hint_serial : 0;
    streams_serial_ = content ? content->streams_serial : 0;
    episode_scroll_.position.snap(0.0f);
    stream_scroll_.position.snap(0.0f);
    set_mode(content && content->series ? Mode::episodes : Mode::streams);
}

void Detail::set_mode(Mode mode)
{
    mode_ = mode;
    swap_.start(0.3f);
    ring_.snap(mode_ == Mode::episodes ? episode_rect(episode_, true) : stream_rect(stream_, true));
}

void Detail::enter()
{
    age_ = 0.0f;
}

void Detail::refresh()
{
    if (!content_)
        return;
    const int episodes = static_cast<int>(content_->episodes.size());
    const int streams = static_cast<int>(content_->streams.size());
    if (content_->hint_serial != hint_serial_)
    {
        hint_serial_ = content_->hint_serial;
        episode_ = std::clamp(content_->episode_hint, 0, std::max(0, episodes - 1));
        episode_scroll_.position.snap(0.0f);
    }
    episode_ = std::min(episode_, std::max(0, episodes - 1));
    stream_ = std::min(stream_, std::max(0, streams - 1));
    // The streams of an episode were asked for from outside (Continue Watching): show them.
    if (content_->streams_serial != streams_serial_)
    {
        streams_serial_ = content_->streams_serial;
        if (content_->series)
        {
            set_mode(Mode::streams);
            streams_wanted_ = true;
        }
    }
    // The details arrived and it is a film after all: there are no episodes to show.
    if (!content_->series && mode_ == Mode::episodes)
        set_mode(Mode::streams);
    else if (content_->series && mode_ == Mode::streams && !streams_wanted_)
        set_mode(Mode::episodes);
}

void Detail::update(const hui::InputFrame &input, float dt)
{
    age_ += dt;
    clock_ += dt;
    if (!content_)
        return;
    if (mode_ == Mode::episodes)
    {
        const int count = static_cast<int>(content_->episodes.size());
        if (input.nav == Direction::up)
            episode_ = std::max(0, episode_ - 1);
        else if (input.nav == Direction::down)
            episode_ = std::min(std::max(0, count - 1), episode_ + 1);
        if (input.is_pressed(Action::confirm) && count > 0)
        {
            set_mode(Mode::streams);
            streams_wanted_ = true;
        }
    }
    else
    {
        const int count = static_cast<int>(content_->streams.size());
        if (input.nav == Direction::up)
            stream_ = std::max(0, stream_ - 1);
        else if (input.nav == Direction::down)
            stream_ = std::min(std::max(0, count - 1), stream_ + 1);
        if (input.is_pressed(Action::back) && content_->series)
        {
            set_mode(Mode::episodes);
            streams_wanted_ = false;
        }
    }
    const float e = static_cast<float>(episode_) * kEpisodePitch;
    episode_scroll_.reveal(e, e + kEpisodeH, kListViewH, 12.0f);
    const float s = static_cast<float>(stream_) * kStreamPitch;
    stream_scroll_.reveal(s, s + kStreamH, kListViewH, 12.0f);
    episode_scroll_.update(dt, 18.0f);
    stream_scroll_.update(dt, 18.0f);
    ring_.target(mode_ == Mode::episodes ? episode_rect(episode_, true) : stream_rect(stream_, true));
    ring_.update(dt, 22.0f);
    swap_.update(dt);
}

void Detail::draw_info(hui::gfx::DrawList &list) const
{
    const Title &t = content_->title;
    const float x = 110.0f;
    const float w = 840.0f;
    const float in = tween::stagger(age_, 0, 0.0f, 0.6f);
    list.push_opacity(in);
    const hui::ui::FontRef &display = env_.fonts.display;
    std::vector<std::string> lines = display.font->wrap(t.name, 66.0f, w);
    if (lines.size() > 2)
    {
        lines.resize(2);
        lines[1] = display.font->fit(lines[1] + " \xE2\x80\xA6", 66.0f, w);
    }
    float baseline = 206.0f;
    for (const std::string &line : lines)
    {
        text(list, display, display.font->fit(line, 66.0f, w), x, baseline, 66.0f, theme::ink, Align::left, -1.4f);
        baseline += 74.0f;
    }
    baseline += 2.0f;
    std::string meta = t.year;
    if (!t.runtime.empty())
        meta += "   \xC2\xB7   " + t.runtime;
    if (!t.genres.empty())
        meta += "   \xC2\xB7   " + t.genres;
    // The rating badge follows the line, so the line leaves room for it before the panel.
    meta = env_.fonts.regular.font->fit(meta, 26.0f, t.imdb.empty() ? w : w - 190.0f);
    const float meta_w = text(list, env_.fonts.regular, meta, x, baseline, 26.0f, theme::ink_2);
    draw_imdb(list, env_, t.imdb, x + meta_w + 30.0f, baseline);
    baseline += 58.0f;
    baseline = hui::ui::paragraph(list, env_.fonts.regular, t.synopsis, x, baseline, 27.0f, w - 40.0f, 41.0f,
                                  theme::ink_2, 6);
    baseline += 18.0f;
    if (!content_->directors.empty())
    {
        text(list, env_.fonts.regular, content_->series ? "Creators" : "Director", x, baseline, 22.0f, theme::ink_3);
        text(list, env_.fonts.regular, env_.fonts.regular.font->fit(content_->directors, 24.0f, w - 190.0f), x + 150.0f,
             baseline, 24.0f, theme::ink_2);
        baseline += 42.0f;
    }
    if (!content_->cast.empty())
    {
        text(list, env_.fonts.regular, "Cast", x, baseline, 22.0f, theme::ink_3);
        hui::ui::paragraph(list, env_.fonts.regular, content_->cast, x + 150.0f, baseline, 24.0f, w - 190.0f, 36.0f,
                           theme::ink_2, 2);
    }
    list.pop_opacity();
}

void Detail::draw_episodes(hui::gfx::DrawList &list) const
{
    // The season bar: L1 and R1 turn the pages.
    const Rect bar{kPanel.x + 28.0f, 112.0f, kPanel.w - 56.0f, 76.0f};
    list.rounded_rect(bar, 26.0f, theme::surface);
    const std::string season =
        content_->seasons.empty() ? "Episodes" : content_->seasons[std::min<std::size_t>(content_->season, content_->seasons.size() - 1)];
    char count[48];
    std::snprintf(count, sizeof(count), "%zu episodes", content_->episodes.size());
    const float sw = env_.fonts.semibold.measure(season, 30.0f);
    const float cw = env_.fonts.regular.measure(count, 24.0f);
    const float start = bar.cx() - (sw + 22.0f + cw) * 0.5f;
    text(list, env_.fonts.semibold, season, start, bar.cy() + 11.0f, 30.0f, theme::ink);
    text(list, env_.fonts.regular, count, start + sw + 22.0f, bar.cy() + 10.0f, 24.0f, theme::ink_3);
    hui::ui::draw_button(list, env_.fonts, hui::ui::GlyphStyle::dark(), hui::ui::Button::l1, bar.x + 22.0f, bar.cy(),
                         40.0f);
    hui::ui::draw_button(list, env_.fonts, hui::ui::GlyphStyle::dark(), hui::ui::Button::r1, bar.x + bar.w - 66.0f,
                         bar.cy(), 40.0f);

    list.push_clip({kPanel.x, kListTop - 8.0f, kPanel.w, kListViewH + 16.0f});
    const Rect ring = ring_.value();
    if (!content_->episodes.empty())
        list.glow(ring, 22.0f, 24.0f, theme::accent.with_alpha(0.30f));
    for (int i = 0; i < static_cast<int>(content_->episodes.size()); ++i)
    {
        const Rect r = episode_rect(i, false);
        if (r.y > 1080.0f || r.y + r.h < 0.0f)
            continue;
        const Episode &e = content_->episodes[i];
        const bool focused = i == episode_;
        const float enter = tween::stagger(age_, i, 0.04f, 0.4f);
        list.push_opacity(enter);
        list.rounded_rect(r, 22.0f, focused ? theme::surface_hi : Color::rgb(0xffffff, 0.045f));
        const Rect thumb{r.x + 12.0f, r.y + 11.0f, 168.0f, 94.0f};
        if (e.thumb != 0)
            list.image(e.thumb, thumb, kFullUv, Color::rgb(0xffffff), 12.0f);
        else
            list.gradient_rect(thumb, 12.0f, Color::rgb(0x2a2540), Color::rgb(0x14111f));
        if (e.progress >= 0.0f)
        {
            list.rounded_rect({thumb.x + 8.0f, thumb.y + thumb.h - 14.0f, thumb.w - 16.0f, 5.0f}, 2.5f,
                              Color::rgb(0xffffff, 0.30f));
            list.rounded_rect({thumb.x + 8.0f, thumb.y + thumb.h - 14.0f, (thumb.w - 16.0f) * e.progress, 5.0f}, 2.5f,
                              theme::accent_soft);
        }
        const float tx = thumb.x + thumb.w + 22.0f;
        const float room = r.x + r.w - tx - 74.0f;
        char number[16];
        std::snprintf(number, sizeof(number), "%d", e.number);
        const float nw = text(list, env_.fonts.semibold, number, tx, r.y + 50.0f, 26.0f, theme::ink_3) + 14.0f;
        text(list, env_.fonts.semibold, env_.fonts.semibold.font->fit(e.title, 26.0f, room - nw), tx + nw, r.y + 50.0f,
             26.0f, e.watched ? theme::ink_3 : theme::ink);
        text(list, env_.fonts.regular, e.info, tx, r.y + 86.0f, 22.0f, theme::ink_3);
        if (e.watched)
        {
            const float cx = r.x + r.w - 40.0f, cy = r.cy();
            list.circle(cx, cy, 17.0f, theme::watched);
            list.line(cx - 7.0f, cy + 1.0f, cx - 2.0f, cy + 6.5f, 3.4f, Color::rgb(0xffffff));
            list.line(cx - 2.0f, cy + 6.5f, cx + 8.0f, cy - 6.0f, 3.4f, Color::rgb(0xffffff));
        }
        list.pop_opacity();
    }
    if (!content_->episodes.empty())
        list.bordered_rect(ring, 22.0f, Color::rgb(0xffffff, 0.0f), 4.0f, Color::rgb(0xffffff));
    list.pop_clip();
    if (content_->episodes.empty())
        hui::ui::paragraph(list, env_.fonts.regular, content_->episodes_status, kPanel.x + 44.0f, kListTop + 56.0f, 26.0f,
                           kPanel.w - 88.0f, 38.0f, theme::ink_3, 5);
}

void Detail::draw_streams(hui::gfx::DrawList &list) const
{
    // The heading, with the way back when this follows an episode.
    const float hx = kPanel.x + 40.0f;
    float tx = hx;
    if (content_->series)
    {
        draw_chevron(list, hx + 8.0f, 150.0f, 20.0f, 2, theme::ink_2);
        tx = hx + 38.0f;
    }
    char count[48];
    std::snprintf(count, sizeof(count), "%zu streams", content_->streams.size());
    const float cw = env_.fonts.regular.measure(count, 24.0f);
    text(list, env_.fonts.regular, count, kPanel.x + kPanel.w - 40.0f, 158.0f, 24.0f, theme::ink_3, Align::right);
    const std::string heading = content_->stream_heading.empty() ? "Streams" : content_->stream_heading;
    text(list, env_.fonts.semibold, env_.fonts.semibold.font->fit(heading, 32.0f, kPanel.w - 120.0f - cw), tx, 160.0f,
         32.0f, theme::ink);

    list.push_clip({kPanel.x, kListTop - 8.0f, kPanel.w, kListViewH + 16.0f});
    const Rect ring = ring_.value();
    if (!content_->streams.empty())
        list.glow(ring, 22.0f, 24.0f, theme::accent.with_alpha(0.30f));
    for (int i = 0; i < static_cast<int>(content_->streams.size()); ++i)
    {
        const Rect r = stream_rect(i, false);
        if (r.y > 1080.0f || r.y + r.h < 0.0f)
            continue;
        const Stream &s = content_->streams[i];
        const bool focused = i == stream_;
        const float enter = tween::stagger(age_, i, 0.04f, 0.4f);
        list.push_opacity(enter);
        list.rounded_rect(r, 22.0f, focused ? theme::surface_hi : Color::rgb(0xffffff, 0.045f));
        // The resolution, as a badge in its own colour.
        const Rect badge{r.x + 18.0f, r.cy() - 31.0f, 108.0f, 62.0f};
        list.gradient_rect(badge, 16.0f, hui::gfx::mix(resolution_colour(s.resolution), Color::rgb(0xffffff), 0.14f),
                           resolution_colour(s.resolution));
        text(list, env_.fonts.semibold, s.resolution, badge.cx(), badge.cy() + 10.0f, 28.0f, Color::rgb(0xffffff),
             Align::center);
        const float x = badge.x + badge.w + 24.0f;
        const float right = r.x + r.w - 28.0f;
        const float room = right - x - 150.0f;
        text(list, env_.fonts.semibold, env_.fonts.semibold.font->fit(s.name, 26.0f, room), x, r.y + 44.0f, 26.0f,
             focused ? theme::ink : theme::ink_2);
        text(list, env_.fonts.regular, env_.fonts.regular.font->fit(s.detail, 21.0f, room), x, r.y + 76.0f, 21.0f,
             theme::ink_3);
        if (!s.tags.empty())
            text(list, env_.fonts.semibold, env_.fonts.semibold.font->fit(s.tags, 20.0f, room), x, r.y + 106.0f, 20.0f,
                 theme::accent_soft);
        text(list, env_.fonts.semibold, s.size, right, r.y + 54.0f, 26.0f, theme::ink, Align::right);
        if (!s.seeds.empty())
            text(list, env_.fonts.regular, s.seeds, right, r.y + 88.0f, 21.0f, theme::ink_3, Align::right);
        list.pop_opacity();
    }
    if (!content_->streams.empty())
        list.bordered_rect(ring, 22.0f, Color::rgb(0xffffff, 0.0f), 4.0f, Color::rgb(0xffffff));
    list.pop_clip();
    if (content_->streams.empty())
        hui::ui::paragraph(list, env_.fonts.regular, content_->streams_status, kPanel.x + 44.0f, kListTop + 56.0f, 26.0f,
                           kPanel.w - 88.0f, 38.0f, theme::ink_3, 5);
}

void Detail::draw_hints_row(hui::gfx::DrawList &list) const
{
    hui::ui::HintLayout layout;
    layout.size = 36.0f;
    layout.text_size = 24.0f;
    layout.cy = 1038.0f;
    layout.item_gap = 40.0f;
    layout.font = &env_.fonts.regular;
    if (mode_ == Mode::episodes)
    {
        const hui::ui::Hint hints[] = {{hui::ui::Button::cross, "Streams"},
                                       {hui::ui::Button::l1, "Season", hui::ui::Button::r1},
                                       {hui::ui::Button::circle, "Back"}};
        hui::ui::draw_hints(list, env_.fonts, hui::ui::GlyphStyle::dark(), hints, 3, theme::kRight, true, layout);
    }
    else
    {
        const hui::ui::Hint hints[] = {{hui::ui::Button::cross, "Play"},
                                       {hui::ui::Button::square, "Play with server"},
                                       {hui::ui::Button::circle, "Back"}};
        hui::ui::draw_hints(list, env_.fonts, hui::ui::GlyphStyle::dark(), hints, 3, theme::kRight, true, layout);
    }
}

void Detail::draw(hui::gfx::DrawList &list, hui::gfx::BackdropSpec &backdrop) const
{
    backdrop.mode = hui::gfx::BackdropMode::none;
    list.rounded_rect({0, 0, theme::kScreenW, theme::kScreenH}, 0, theme::ground);
    if (!content_)
        return;
    const float intro = tween::stagger(age_, 0, 0.0f, 0.8f);
    // The artwork fills the screen, then the page closes over it where text goes.
    if (content_->title.backdrop != 0)
    {
        list.push_opacity(intro);
        list.image(content_->title.backdrop, {0, 0, theme::kScreenW, theme::kScreenH}, kFullUv,
                   Color::rgb(0xffffff, 0.82f));
        list.pop_opacity();
    }
    list.gradient_rect_h({0, 0, 1400.0f, theme::kScreenH}, 0, theme::ground.with_alpha(0.93f),
                         theme::ground.with_alpha(0.16f));
    list.gradient_rect({0, 640.0f, theme::kScreenW, 440.0f}, 0, theme::ground.with_alpha(0.0f),
                       theme::ground.with_alpha(0.88f));
    list.gradient_rect({0, 0, theme::kScreenW, 200.0f}, 0, theme::ground.with_alpha(0.6f),
                       theme::ground.with_alpha(0.0f));

    list.push_opacity(intro);
    draw_logo(list, env_);
    list.pop_opacity();
    draw_info(list);

    // The panel.
    const float panel_in = tween::stagger(age_, 1, 0.08f, 0.5f);
    list.push_opacity(panel_in);
    list.shadow({kPanel.x + 8, kPanel.y + 26, kPanel.w - 16, kPanel.h - 20}, 34, 56, Color::rgb(0x000000, 0.45f));
    list.bordered_rect(kPanel, 36.0f, Color::rgb(0x0d0b16, 0.86f), 1.5f, theme::hairline);
    const float swap = tween::smoothstep(swap_.progress());
    list.push_opacity(swap);
    if (mode_ == Mode::episodes)
        draw_episodes(list);
    else
        draw_streams(list);
    list.pop_opacity();
    list.pop_opacity();
    draw_hints_row(list);
}

} // namespace sx
