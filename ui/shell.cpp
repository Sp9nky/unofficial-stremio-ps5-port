#include "shell.hpp"

#include <algorithm>

namespace sx
{

using hui::Action;
using hui::Direction;

namespace
{
constexpr int kPageCount = 5;
}

Shell::Shell(const Env &env, Session &session)
    : env_(env), s_(session), board_(env), search_board_(env), discover_(env), library_(env), addons_(env), settings_(env),
      detail_(env), player_ui_(env)
{
    board_.set_content(&s_.board);
    search_board_.set_search(&s_.search_query);
    search_board_.set_content(&s_.search);
    discover_.set_content(&s_.discover);
    library_.set_content(&s_.library);
    addons_.set_content(&s_.addons);
    settings_.set_content(&s_.settings);
    go_page(0);
}

Session::Cursor Shell::cursor() const
{
    Session::Cursor c;
    c.board_row = board_.cursor_row();
    c.board_col = board_.cursor_col();
    c.discover = discover_.cursor_index();
    c.library = library_.cursor_index();
    c.episode = detail_.episode_index();
    c.search_row = search_board_.cursor_row();
    c.search_col = search_board_.cursor_col();
    return c;
}

void Shell::go_page(int page)
{
    page_ = std::clamp(page, 0, kPageCount - 1);
    screen_ = Screen::page;
    switch (page_)
    {
    case 0:
        board_.enter();
        break;
    case 1:
        discover_.enter();
        break;
    case 2:
        library_.enter();
        break;
    case 3:
        addons_.enter();
        break;
    default:
        settings_.enter();
        break;
    }
    s_.page_opened(static_cast<Page>(page_));
}

void Shell::open_title(const Title &title, const std::string &streams_video)
{
    came_from_ = screen_;
    s_.open_title(title, streams_video);
    detail_.set_content(&s_.detail);
    detail_.enter();
    screen_ = Screen::detail;
}

// Back from a title's page: to the page or the search results it was opened from.
void Shell::leave_title()
{
    s_.close_detail();
    if (came_from_ == Screen::search && s_.search_open)
    {
        screen_ = Screen::search;
        search_board_.enter();
    }
    else
    {
        go_page(page_);
    }
}

// What the session changed since the last frame, passed on to the screens that show it.
void Shell::apply_changes()
{
    const unsigned c = s_.take_changed();
    if (c & Session::kBoard)
        board_.refresh();
    if (c & Session::kDiscoverNew)
        discover_.set_content(&s_.discover);
    else if (c & Session::kDiscover)
        discover_.refresh();
    if (c & Session::kLibraryNew)
        library_.set_content(&s_.library);
    else if (c & Session::kLibrary)
        library_.refresh();
    if (c & Session::kAddons)
        addons_.refresh();
    if (c & Session::kSettings)
        settings_.refresh();
    if (c & Session::kDetail)
        detail_.refresh();
    if (c & Session::kSearchNew)
    {
        search_board_.set_content(&s_.search);
        search_board_.enter();
        if (s_.search_open)
            screen_ = Screen::search;
        else if (screen_ == Screen::search)
            screen_ = Screen::page;
    }
    else if (c & Session::kSearch)
    {
        search_board_.refresh();
    }
    Title requested;
    std::string requested_streams;
    if (s_.take_open(&requested, &requested_streams))
        open_title(requested, requested_streams);
    Page page;
    if (s_.take_goto(&page))
    {
        sound_.play(hui::audio::Cue::tab);
        go_page(static_cast<int>(page));
    }
}

void Shell::route_detail(const hui::InputFrame &input, float dt)
{
    if (input.pressed != 0 || input.nav != Direction::none)
        s_.cancel_autoplay(); // any button cancels playing the next episode
    const bool streams = detail_.mode() == Detail::Mode::streams;
    const bool series = s_.detail.series;
    // Back leaves the page, except from a series' streams, where it goes back to the episodes.
    const bool leave = input.is_pressed(Action::back) && !(series && streams);
    if (streams)
    {
        if (input.is_pressed(Action::confirm))
            s_.detail_play(detail_.stream_index(), false);
        else if (input.is_pressed(Action::west))
            s_.detail_play(detail_.stream_index(), true);
    }
    else
    {
        if (input.is_pressed(Action::page_prev))
            s_.detail_change_season(-1);
        else if (input.is_pressed(Action::page_next))
            s_.detail_change_season(1);
        else if (input.is_pressed(Action::north))
            s_.detail_toggle_watched(detail_.episode_index());
        else if (input.is_pressed(Action::confirm) && !s_.detail.episodes.empty())
            s_.detail_pick_episode(detail_.episode_index());
    }
    detail_.update(input, dt);
    if (leave)
        leave_title();
}

void Shell::route_search(const hui::InputFrame &input, float dt)
{
    search_board_.update(input, dt);
    const bool on_rail = search_board_.rail_focused();
    if (input.is_pressed(Action::back))
    {
        s_.close_search();
        go_page(page_);
        return;
    }
    if (input.is_pressed(Action::north) || input.is_pressed(Action::menu))
    {
        s_.begin_search();
        return;
    }
    if (!input.is_pressed(Action::confirm))
        return;
    if (on_rail)
    {
        const int cursor = search_board_.rail_cursor();
        s_.close_search();
        sound_.play(hui::audio::Cue::tab);
        go_page(cursor);
    }
    else if (const Title *under = search_board_.focused_title())
    {
        open_title(*under);
    }
}

void Shell::route_page(const hui::InputFrame &input, float dt)
{
    const bool confirm = input.is_pressed(Action::confirm);
    bool on_rail = false;
    int cursor = 0;
    const Title *under = nullptr;
    switch (page_)
    {
    case 0:
        board_.update(input, dt);
        on_rail = board_.rail_focused();
        cursor = board_.rail_cursor();
        under = board_.focused_title();
        if (!on_rail && input.is_pressed(Action::west))
            s_.board_square(board_.cursor_row(), board_.cursor_col());
        // Options on a Continue Watching card: remove it (after asking). Elsewhere it is search.
        if (!on_rail && input.is_pressed(Action::menu) && s_.board_options(board_.cursor_row(), board_.cursor_col()))
            return;
        break;
    case 1:
        discover_.update(input, dt);
        on_rail = discover_.rail_focused();
        cursor = discover_.rail_cursor();
        under = discover_.focused_title();
        if (confirm && discover_.filter_cursor() >= 0)
        {
            s_.filter_activate(Page::discover, discover_.filter_cursor());
            return;
        }
        break;
    case 2:
        library_.update(input, dt);
        on_rail = library_.rail_focused();
        cursor = library_.rail_cursor();
        under = library_.focused_title();
        if (confirm && library_.filter_cursor() >= 0)
        {
            s_.filter_activate(Page::library, library_.filter_cursor());
            return;
        }
        break;
    case 3:
        addons_.update(input, dt);
        on_rail = addons_.rail_focused();
        cursor = addons_.rail_cursor();
        if (!on_rail && confirm)
            s_.addon_activate(addons_.cursor_index());
        if (!on_rail && input.is_pressed(Action::west))
            s_.addons_reload();
        break;
    default:
        settings_.update(input, dt);
        on_rail = settings_.rail_focused();
        cursor = settings_.rail_cursor();
        if (confirm && settings_.cursor_row() >= 0)
        {
            s_.settings_activate(settings_.cursor_row());
            return;
        }
        break;
    }
    // Circle from any page but the Board goes to the Board (and from the Board it does nothing: the end of
    // the way back).
    if (page_ != 0 && input.is_pressed(Action::back))
    {
        go_page(0);
        return;
    }
    // Triangle or Options: search, from every page.
    if (!on_rail && (input.is_pressed(Action::north) || input.is_pressed(Action::menu)))
    {
        s_.begin_search();
        return;
    }
    if (!confirm)
        return;
    if (on_rail)
    {
        if (cursor != page_)
        {
            sound_.play(hui::audio::Cue::tab);
            go_page(cursor);
        }
    }
    else if (under)
    {
        // A card of Continue Watching asks what to do (or goes to the next episode's streams); any other opens.
        if (page_ == 0 && s_.board_is_continue(board_.cursor_row()))
            s_.continue_press(board_.cursor_row(), board_.cursor_col());
        else
            open_title(*under);
    }
}

unsigned Shell::view_key() const
{
    return static_cast<unsigned>(screen_) * 64u + static_cast<unsigned>(page_) + (s_.dropdown_open ? 1000u : 0u);
}

unsigned Shell::focus_signature()
{
    const Session::Cursor c = cursor();
    unsigned h = 17u;
    const auto mix = [&h](int v) { h = h * 131u + static_cast<unsigned>(v + 7); };
    mix(c.board_row);
    mix(c.board_col);
    mix(c.discover);
    mix(c.library);
    mix(c.episode);
    mix(c.search_row);
    mix(c.search_col);
    mix(board_.rail_focused());
    mix(board_.rail_cursor());
    mix(discover_.rail_focused());
    mix(discover_.rail_cursor());
    mix(discover_.filter_cursor());
    mix(library_.rail_focused());
    mix(library_.rail_cursor());
    mix(library_.filter_cursor());
    mix(addons_.rail_focused());
    mix(addons_.rail_cursor());
    mix(addons_.cursor_index());
    mix(settings_.rail_focused());
    mix(settings_.rail_cursor());
    mix(settings_.cursor_row());
    mix(search_board_.rail_focused());
    mix(search_board_.rail_cursor());
    mix(detail_.stream_index());
    mix(detail_.episode_index());
    mix(s_.dropdown_open ? s_.dropdown.selected : -1);
    return h;
}
void Shell::update(const hui::InputFrame &input, float dt)
{
    clock_ += dt;
    // What arrived since the last frame, before any screen looks at its content.
    apply_changes();
    // The sounds: what the session wants played, and the cursor's move before and after the buttons.
    sound_.set_enabled(s_.ui_sounds());
    for (const hui::audio::Cue cue : s_.take_cues())
        sound_.play(cue);
    const bool sounds_ok = !s_.watching && !s_.sign_in_open && typing_hold_ <= 0.0f && boot_out_ >= 0.35f;
    const unsigned sig_before = sounds_ok ? focus_signature() : 0u;
    const unsigned view_before = view_key();
    if (sounds_ok && input.is_pressed(Action::back) && (s_.dropdown_open || screen_ != Screen::page || page_ != 0))
        sound_.play(hui::audio::Cue::back);
    const hui::InputFrame idle = [] {
        hui::InputFrame f;
        f.connected = true;
        return f;
    }();

    if (s_.dropdown_open)
    {
        DropdownState &d = s_.dropdown;
        const int count = static_cast<int>(d.options.size());
        if (input.nav == Direction::up)
            d.selected = std::max(0, d.selected - 1);
        else if (input.nav == Direction::down)
            d.selected = std::min(std::max(0, count - 1), d.selected + 1);
        if (input.is_pressed(Action::confirm))
            s_.dropdown_confirm();
        else if (input.is_pressed(Action::back))
            s_.dropdown_cancel();
    }
    else if (s_.sign_in_open)
    {
        if (input.is_pressed(Action::back))
            s_.sign_in_cancel();
    }
    // The keyboard dialog has the controller; its last button presses must not reach the screens.
    typing_hold_ = s_.typing() ? 0.35f : std::max(0.0f, typing_hold_ - dt);
    // The pop-ups hold the controller while they are up; the screens underneath keep moving.
    // The first moments: the mark is up until the first screen has its catalogs and posters; the
    // screens underneath are idle, and start their entrance when it lifts.
    if (boot_out_ < kBootOut)
        boot_t_ += dt;
    if (!s_.boot_ready())
    {
        boot_out_ = -1.0f;
    }
    else if (boot_out_ < 0.0f)
    {
        boot_out_ = 0.0f;
        board_.enter();
        sound_.play(hui::audio::Cue::welcome);
    }
    else
    {
        boot_out_ += dt;
    }
    const bool booting = boot_out_ < 0.35f;
    const bool held = s_.dropdown_open || s_.sign_in_open || dropdown_t_.value > 0.3f || typing_hold_ > 0.0f || booting;
    const hui::InputFrame &in = held ? idle : input;

    if (s_.watching)
    {
        if (!was_watching_)
            watch_t_ = 0.0f;
        watch_t_ += dt;
        s_.watch_input(in);
        player_ui_.state = s_.playing;
        player_ui_.update(dt);
    }
    else if (screen_ == Screen::detail)
    {
        route_detail(in, dt);
    }
    else if (screen_ == Screen::search)
    {
        route_search(in, dt);
    }
    else
    {
        route_page(in, dt);
    }
    if (was_watching_ && !s_.watching)
        sound_.play_when_back(hui::audio::Cue::resume);
    if (sounds_ok && input.nav != Direction::none && view_key() == view_before)
    {
        // A direction that moved the focus makes a soft sound; one that could not (the end of a list) a
        // short one that says so.
        sound_.play(focus_signature() != sig_before ? hui::audio::Cue::focus : hui::audio::Cue::error);
    }
    sound_.update(dt);
    was_watching_ = s_.watching;

    dropdown_t_.target = s_.dropdown_open ? 1.0f : 0.0f;
    dropdown_t_.update(dt, 22.0f);
    sign_in_t_.target = s_.sign_in_open ? 1.0f : 0.0f;
    sign_in_t_.update(dt, 18.0f);
    toast_t_.target = s_.toast_open ? 1.0f : 0.0f;
    toast_t_.update(dt, 18.0f);
}

void Shell::draw(hui::gfx::DrawList &list, hui::gfx::BackdropSpec &backdrop) const
{
    // The first moments: the page's light and the mark, nothing else (the pages under it are not drawn).
    if (boot_out_ < 0.0f)
    {
        apply_page_backdrop(backdrop, boot_t_);
        draw_boot(list, env_, boot_t_, 0.0f);
        return;
    }
    if (s_.watching)
    {
        if (s_.launch_open)
        {
            backdrop.mode = hui::gfx::BackdropMode::none;
            draw_launch(list, env_, s_.launch, watch_t_, std::min(1.0f, watch_t_ * 3.0f));
        }
        else
        {
            backdrop.mode = hui::gfx::BackdropMode::none;
            list.rounded_rect({0, 0, theme::kScreenW, theme::kScreenH}, 0, hui::gfx::Color::rgb(0x000000));
            if (s_.video_texture != 0)
            {
                const float w = static_cast<float>(s_.video_w), h = static_cast<float>(s_.video_h);
                list.image(s_.video_texture, {(theme::kScreenW - w) * 0.5f, (theme::kScreenH - h) * 0.5f, w, h},
                           hui::gfx::kFullUv, hui::gfx::Color::rgb(0xffffff));
            }
            player_ui_.draw(list);
        }
    }
    else if (screen_ == Screen::detail)
    {
        detail_.draw(list, backdrop);
    }
    else if (screen_ == Screen::search)
    {
        search_board_.draw(list, backdrop);
    }
    else
    {
        switch (page_)
        {
        case 0:
            board_.draw(list, backdrop);
            break;
        case 1:
            discover_.draw(list, backdrop);
            break;
        case 2:
            library_.draw(list, backdrop);
            break;
        case 3:
            addons_.draw(list, backdrop);
            break;
        default:
            settings_.draw(list, backdrop);
            break;
        }
    }
    if (dropdown_t_.value > 0.01f)
        draw_dropdown(list, env_, s_.dropdown, dropdown_t_.value);
    if (sign_in_t_.value > 0.01f)
        draw_sign_in(list, env_, s_.sign_in, clock_, sign_in_t_.value);
    if (toast_t_.value > 0.01f)
        draw_toast(list, env_, s_.toast, toast_t_.value);
    if (boot_out_ < kBootOut)
        draw_boot(list, env_, boot_t_, boot_out_ < 0.0f ? 0.0f : boot_out_ / kBootOut);
}

} // namespace sx
