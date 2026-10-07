// Screens and pop-ups that sit over a page: starting a stream, a list of
// choices, signing in, and short messages. Typing is the console's own keyboard.

#pragma once

#include "chrome.hpp"
#include "content.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace sx
{

// The screen between picking a stream and the first picture. `clock` runs
// the three dots; `t` (0..1) is the entrance.
void draw_launch(hui::gfx::DrawList &list, const Env &env, const LaunchState &state, float clock, float t);

// The first moments: the mark over the page's ground while the first screen gets its catalogs and
// posters. 	 is the seconds since the app started; out (0..1) lifts it away once all is ready.
void draw_boot(hui::gfx::DrawList &list, const Env &env, float t, float out);
// Just the mark (and the glow behind it), for the first frame, drawn before the fonts exist.
void draw_boot_mark(hui::gfx::DrawList &list, const Icons &icons, float glow, float scale);

struct DropdownState
{
    std::string title;
    std::vector<std::string> options;
    std::string message; // under the title, for a question
    int active = 0;   // the current choice, ticked (-1: none)
    int selected = 0; // the cursor
    int danger = -1;  // the choice that destroys something, drawn in red
    // A question about a title, in the middle of the screen: its poster, how far it is watched, and the
    // choices as large buttons (see draw_dialog). Without it the list drops from the top, for long lists.
    bool dialog = false;
    std::uint32_t poster = 0; // GL texture, 0 for none
    float progress = -1.0f;   // 0..1, negative for none
};
void draw_dropdown(hui::gfx::DrawList &list, const Env &env, const DropdownState &state, float t);

struct SignInState
{
    std::uint32_t qr = 0;   // the QR code picture, 0 until it has arrived
    std::string link;       // "stremio.com/link"
    std::string code;       // "A7KP"
    std::string status;     // "Waiting for you to sign in"
};
void draw_sign_in(hui::gfx::DrawList &list, const Env &env, const SignInState &state, float clock, float t);

struct ToastState
{
    std::string message;
    bool error = false;
};
void draw_toast(hui::gfx::DrawList &list, const Env &env, const ToastState &state, float t);

} // namespace sx
