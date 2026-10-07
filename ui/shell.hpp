// Which screen is showing and how the controller moves between them: the five
// pages of the rail, search results, a title's page, and the launch screen and
// player over all of those, with the pop-ups on top. It decides what a button
// means, tells the session when something was chosen, and tells the screens
// when the session changed what they show.

#pragma once

#include "addons.hpp"
#include "board.hpp"
#include "detail.hpp"
#include "discover.hpp"
#include "library.hpp"
#include "overlays.hpp"
#include "player.hpp"
#include "settings.hpp"
#include "../gl/session.hpp"
#include "../gl/ui_sound.hpp"

namespace sx
{

class Shell
{
  public:
    Shell(const Env &env, Session &session);

    void update(const hui::InputFrame &input, float dt);
    // Loads the interface sounds from <dir>/glass and opens the audio port.
    void start_sound(const std::string &dir)
    {
        sound_.start(dir);
    }
    // The boot animation has been running for this long already (frames drawn while the app loaded).
    void start_boot_at(float seconds)
    {
        boot_t_ = seconds;
    }
    void draw(hui::gfx::DrawList &list, hui::gfx::BackdropSpec &backdrop) const;

    // Where the cursor is, for the session to load artwork around it.
    Session::Cursor cursor() const;

  private:
    enum class Screen
    {
        page,
        search,
        detail,
    };

    void go_page(int page);
    void open_title(const Title &title, const std::string &streams_video = std::string());
    void route_page(const hui::InputFrame &input, float dt);
    void route_search(const hui::InputFrame &input, float dt);
    void route_detail(const hui::InputFrame &input, float dt);
    void apply_changes();
    void leave_title();

    const Env &env_;
    Session &s_;
    Board board_;
    Board search_board_;
    Discover discover_;
    Library library_;
    Addons addons_;
    Settings settings_;
    Detail detail_;
    PlayerUi player_ui_;
    Screen screen_ = Screen::page;
    Screen came_from_ = Screen::page; // what a title's page goes back to
    int page_ = 0;
    float clock_ = 0.0f;
    UiSound sound_;
    // Where the focus is, as one number, to hear whether a button moved it.
    unsigned focus_signature();
    unsigned view_key() const;
    float boot_t_ = 0.0f;    // seconds since the app started
    float boot_out_ = -1.0f; // -1 while the first screen is being got ready, then seconds since it was
    static constexpr float kBootOut = 0.5f; // how long the mark takes to lift away
    float watch_t_ = 0.0f;   // seconds since the launch screen came up
    float typing_hold_ = 0.0f; // the keyboard's last button presses are not ours
    bool was_watching_ = false;
    // The pop-ups fade in and out.
    hui::tween::Spring dropdown_t_, sign_in_t_, toast_t_;
};

} // namespace sx
