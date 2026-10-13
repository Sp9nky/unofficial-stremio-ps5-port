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
    if (s_.take_profile_picker() && layer_ == Layer::none)
        open_picker(true);
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

// ---- profiles ----------------------------------------------------------------------------------------

void Shell::fill_picker()
{
    pstate_.items.clear();
    const auto &list = s_.profiles();
    for (int i = 0; i < static_cast<int>(list.size()); ++i)
    {
        ProfileEntry e;
        e.name = list[static_cast<std::size_t>(i)].name;
        e.colour = list[static_cast<std::size_t>(i)].colour;
        e.email = s_.profile_email(i);
        pstate_.items.push_back(e);
    }
    profiles_seen_ = s_.profiles_revision();
}

void Shell::open_picker(bool can_leave)
{
    fill_picker();
    pstate_.can_leave = can_leave;
    pick_ = s_.profile_active();
    pfocus_.snap(static_cast<float>(pick_));
    pick_t_.snap(0.0f);
    layer_ = Layer::picker;
}

void Shell::open_editor(int index)
{
    const auto &list = s_.profiles();
    estate_ = EditorState();
    edit_index_ = index;
    estate_.is_new = index < 0;
    if (index >= 0 && index < static_cast<int>(list.size()))
    {
        estate_.profile = pstate_.items[static_cast<std::size_t>(index)];
        edit_id_ = list[static_cast<std::size_t>(index)].id;
        estate_.colour_index = profile_colour_index(estate_.profile.colour);
        estate_.profile.colour = profile_colour(estate_.colour_index);
        estate_.signed_in = !estate_.profile.email.empty();
    }
    else
    {
        edit_id_.clear();
        estate_.profile.name = "Profile " + std::to_string(list.size() + 1);
        estate_.colour_index = static_cast<int>(list.size()) % kProfileColours;
        estate_.profile.colour = profile_colour(estate_.colour_index);
    }
    estate_.row = 0;
    edit_t_.snap(0.0f);
    layer_ = Layer::editor;
}

// A profile was picked: it is loaded and the Board shown (or Settings again if it is the same one).
void Shell::profile_chosen(int index)
{
    const bool changed = index != s_.profile_active();
    const bool at_start = !pstate_.can_leave;
    s_.profile_select(index);
    layer_ = Layer::none;
    go_page(changed ? 0 : page_);
    // At start-up, a profile without an account gets the sign-in screen before the Board.
    if (at_start && s_.start_sign_in_screen())
        layer_ = Layer::sign_in;
}

void Shell::route_profiles(const hui::InputFrame &input, float dt)
{
    (void)dt;
    // Something changed the list (a profile removed): the picker is made again, the editor closes if its
    // profile is gone.
    if (profiles_seen_ != s_.profiles_revision())
    {
        fill_picker();
        pick_ = std::clamp(pick_, 0, static_cast<int>(pstate_.items.size()));
        pfocus_.target = static_cast<float>(pick_);
        if (layer_ == Layer::editor && !estate_.is_new)
        {
            const auto &list = s_.profiles();
            const bool there = std::any_of(list.begin(), list.end(), [&](const Session::ProfileInfo &p) { return p.id == edit_id_; });
            if (!there)
                layer_ = Layer::picker;
        }
    }
    const int count = static_cast<int>(pstate_.items.size());
    if (layer_ == Layer::picker)
    {
        if (input.nav == Direction::left)
            pick_ = std::max(0, pick_ - 1);
        else if (input.nav == Direction::right)
            pick_ = std::min(count, pick_ + 1);
        pfocus_.target = static_cast<float>(pick_);
        if (input.is_pressed(Action::confirm))
        {
            if (pick_ < count)
                profile_chosen(pick_);
            else
                open_editor(-1);
        }
        else if (input.is_pressed(Action::west) && pick_ < count)
        {
            open_editor(pick_);
        }
        else if (input.is_pressed(Action::back) && pstate_.can_leave)
        {
            layer_ = Layer::none;
            go_page(page_);
        }
        return;
    }
    // The editor.
    EditorState &e = estate_;
    const int last = e.is_new ? 3 : 4;
    if (input.nav == Direction::up)
        e.row = std::max(0, e.row - 1);
    else if (input.nav == Direction::down)
        e.row = std::min(last, e.row + 1);
    else if (e.row == 1 && (input.nav == Direction::left || input.nav == Direction::right))
    {
        e.colour_index = std::clamp(e.colour_index + (input.nav == Direction::right ? 1 : -1), 0, kProfileColours - 1);
        e.profile.colour = profile_colour(e.colour_index);
    }
    if (input.is_pressed(Action::back))
    {
        layer_ = Layer::picker;
        return;
    }
    if (!input.is_pressed(Action::confirm))
        return;
    const auto save_and_close = [&]() {
        if (e.is_new)
        {
            const int index = s_.profile_add(e.profile.name, e.profile.colour);
            s_.profile_select(index);
            layer_ = Layer::none;
            go_page(0);
            s_.profile_sign_in(); // its own account: the QR code comes up at once
        }
        else
        {
            s_.profile_update(edit_index_, e.profile.name, e.profile.colour);
            fill_picker();
            layer_ = Layer::picker;
        }
    };
    switch (e.row)
    {
    case 0:
        s_.profile_ask_name([this](const std::string &v) {
            std::string n = v;
            while (!n.empty() && n.back() == ' ')
                n.pop_back();
            if (!n.empty())
                estate_.profile.name = n.size() > 24 ? n.substr(0, 24) : n;
        });
        break;
    case 1:
        e.colour_index = (e.colour_index + 1) % kProfileColours;
        e.profile.colour = profile_colour(e.colour_index);
        break;
    case 2:
        if (e.signed_in)
        {
            s_.say("To sign out, use Settings, Stremio account.", 5.0);
        }
        else if (e.is_new)
        {
            save_and_close();
        }
        else
        {
            // Saved, loaded, and the QR code up: the sign-in belongs to the profile that is loaded.
            s_.profile_update(edit_index_, e.profile.name, e.profile.colour);
            s_.profile_select(edit_index_);
            layer_ = Layer::none;
            go_page(0);
            s_.profile_sign_in();
        }
        break;
    case 3:
        save_and_close();
        s_.cue(hui::audio::Cue::saved);
        break;
    default:
        s_.profile_remove_ask(edit_index_);
        break;
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
    return static_cast<unsigned>(screen_) * 64u + static_cast<unsigned>(page_) + (s_.dropdown_open ? 1000u : 0u) +
           static_cast<unsigned>(layer_) * 5000u;
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
    mix(s_.update_open ? s_.update_dialog.selected : -1);
    mix(static_cast<int>(layer_));
    mix(pick_);
    mix(estate_.row);
    mix(estate_.colour_index);
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

    if (s_.update_open)
    {
        UpdateState &u = s_.update_dialog;
        if (u.phase == UpdateState::Phase::offer)
        {
            if (input.nav == Direction::up)
                u.selected = 0;
            else if (input.nav == Direction::down)
                u.selected = 1;
        }
        if (input.is_pressed(Action::confirm))
            s_.update_confirm();
        else if (input.is_pressed(Action::back))
            s_.update_back();
    }
    else if (s_.dropdown_open)
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
        // Circle: no account for now. On the sign-in screen only once it is up (not a press made during the boot).
        const bool up = !s_.sign_in_screen || (layer_ == Layer::sign_in && boot_out_ >= 0.35f);
        if (up && input.is_pressed(Action::back))
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
        if (rebooting_)
        {
            // The Board the sign-in screen made us wait for is ready: it comes in as the mark lifts.
            rebooting_ = false;
            go_page(0);
        }
        else
        {
            board_.enter();
            sound_.play(hui::audio::Cue::welcome);
            // More than one person uses this console: who is watching comes first. No account: signing in does.
            if (s_.profiles().size() > 1)
                open_picker(false);
            else if (s_.sign_in_screen)
                layer_ = Layer::sign_in;
        }
    }
    else
    {
        boot_out_ += dt;
    }
    // The sign-in screen ends with its sign-in (approved on the phone, or Circle): the Board comes in as it goes.
    if (layer_ == Layer::sign_in && !s_.sign_in_screen)
    {
        layer_ = Layer::none;
        if (s_.board_ready_or_wait())
        {
            go_page(0);
        }
        else
        {
            // Its catalogs or pictures are not here yet (the account's own, or ones still on their way): the boot
            // animation comes back over the fading sign-in screen until they are.
            rebooting_ = true;
            boot_out_ = -1.0f;
        }
    }
    const bool booting = boot_out_ < 0.35f;
    const bool held = s_.update_open || update_t_.value > 0.3f || s_.dropdown_open || s_.sign_in_open || dropdown_t_.value > 0.3f || typing_hold_ > 0.0f || booting;
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
    else if (layer_ == Layer::picker || layer_ == Layer::editor)
    {
        route_profiles(in, dt);
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

    // Dialogs open a little slower than they close; the ring glides at once (and rests on the row when it
    // is not on show, so a dialog never opens with the ring travelling).
    const auto fade = [dt](hui::tween::Spring &t, bool on, float in, float out) {
        t.target = on ? 1.0f : 0.0f;
        t.update(dt, on ? in : out);
    };
    pfocus_.update(dt, 16.0f);
    pstate_.focus = pfocus_.value;
    fade(pick_t_, layer_ == Layer::picker || layer_ == Layer::editor, 18.0f, 30.0f);
    // The sign-in screen comes in once the boot's mark has nearly lifted, and goes as fast as the pop-ups do.
    fade(signin_t_, layer_ == Layer::sign_in && boot_out_ >= 0.3f, 18.0f, 30.0f);
    if (s_.sign_in_screen)
    {
        wall_.resize(s_.wall.size());
        for (std::size_t i = 0; i < wall_.size(); ++i)
        {
            if (wall_[i].id != s_.wall[i])
                wall_[i] = WallTile{s_.wall[i], 0.0f};
            if (wall_[i].id != 0)
                wall_[i].alpha = std::min(1.0f, wall_[i].alpha + dt / 0.6f);
        }
    }
    else if (signin_t_.value < 0.01f)
    {
        // Kept while the screen fades out (the cache keeps a picture used in the last second on the GPU).
        wall_.clear();
    }
    fade(edit_t_, layer_ == Layer::editor, 22.0f, 34.0f);
    fade(update_t_, s_.update_open, 22.0f, 34.0f);
    fade(dropdown_t_, s_.dropdown_open, 22.0f, 34.0f);
    fade(sign_in_t_, s_.sign_in_open && !s_.sign_in_screen, 18.0f, 30.0f);
    const auto ring = [dt](hui::tween::Spring &p, float target, bool shown) {
        p.target = target;
        if (!shown)
            p.snap(target);
        p.update(dt, 30.0f);
    };
    ring(dd_pos_, static_cast<float>(s_.dropdown.selected), s_.dropdown_open);
    ring(up_pos_, static_cast<float>(s_.update_dialog.selected), s_.update_open);
    ring(ed_pos_, static_cast<float>(estate_.row), layer_ == Layer::editor);
    up_prog_.target = s_.update_dialog.progress;
    if (!s_.update_open || s_.update_dialog.phase != UpdateState::Phase::downloading)
        up_prog_.snap(s_.update_dialog.progress);
    up_prog_.update(dt, 8.0f); // a download arrives in steps; the bar does not
    fade(toast_t_, s_.toast_open, 18.0f, 30.0f);
}

void Shell::draw(hui::gfx::DrawList &list, hui::gfx::BackdropSpec &backdrop) const
{
    // The first moments: the page's light and the mark, nothing else (the pages under it are not drawn).
    if (boot_out_ < 0.0f)
    {
        apply_page_backdrop(backdrop, boot_t_);
        draw_boot(list, env_, boot_t_, 0.0f);
        // The sign-in screen, fading out over it when the wait comes back after signing in.
        if (signin_t_.value > 0.01f)
            draw_sign_in_screen(list, env_, s_.sign_in, wall_, clock_, signin_t_.value);
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
            const bool hdr_picture = s_.video_hdr && (s_.video_gpu ? s_.video_plane_y != 0 : s_.video_texture_hdr != 0);
            if (hdr_picture)
            {
                // The HDR pass draws the video (and the black around it); only the interface goes in the list.
                const float w = static_cast<float>(s_.video_disp_w), h = static_cast<float>(s_.video_disp_h);
                s_.hdr_rect = {(theme::kScreenW - w) * 0.5f, (theme::kScreenH - h) * 0.5f, w, h};
            }
            else
            {
                s_.hdr_rect = {};
                list.rounded_rect({0, 0, theme::kScreenW, theme::kScreenH}, 0, hui::gfx::Color::rgb(0x000000));
            }
            if (!hdr_picture && s_.video_texture != 0)
            {
                const float w = static_cast<float>(s_.video_w), h = static_cast<float>(s_.video_h);
                list.image(s_.video_texture, {(theme::kScreenW - w) * 0.5f, (theme::kScreenH - h) * 0.5f, w, h},
                           hui::gfx::kFullUv, hui::gfx::Color::rgb(0xffffff));
            }
            player_ui_.draw(list);
        }
    }
    else if (layer_ == Layer::sign_in)
    {
        // The sign-in screen covers everything: under it only the page's light, which the boot animation stood on.
        apply_page_backdrop(backdrop, clock_);
    }
    else if (layer_ != Layer::none)
    {
        apply_page_backdrop(backdrop, clock_);
        draw_profile_picker(list, env_, pstate_, pick_t_.value);
        if (edit_t_.value > 0.01f)
        {
            EditorState e = estate_;
            e.pos = ed_pos_.value;
            draw_profile_editor(list, env_, e, edit_t_.value);
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
    if (signin_t_.value > 0.01f)
        draw_sign_in_screen(list, env_, s_.sign_in, wall_, clock_, signin_t_.value);
    if (dropdown_t_.value > 0.01f)
    {
        DropdownState d = s_.dropdown;
        d.pos = dd_pos_.value;
        draw_dropdown(list, env_, d, dropdown_t_.value);
    }
    if (update_t_.value > 0.01f)
    {
        UpdateState u = s_.update_dialog;
        u.pos = up_pos_.value;
        u.progress = up_prog_.value;
        draw_update(list, env_, u, clock_, update_t_.value);
    }
    if (sign_in_t_.value > 0.01f)
        draw_sign_in(list, env_, s_.sign_in, clock_, sign_in_t_.value);
    if (toast_t_.value > 0.01f)
        draw_toast(list, env_, s_.toast, toast_t_.value, s_.watching && s_.launch_open);
    if (boot_out_ < kBootOut)
        draw_boot(list, env_, boot_t_, boot_out_ < 0.0f ? 0.0f : boot_out_ / kBootOut);
}

} // namespace sx
