// The update dialog: a new version is available, downloading it, installing it, the app closing, and the
// two ways it can end without an update (up to date, failed).

#pragma once

#include "chrome.hpp"

#include <string>
#include <vector>

namespace sx
{

struct UpdateState
{
    enum class Phase
    {
        offer,       // "Update available": Update now / Later
        downloading, // a bar, the amount and the time left; Circle cancels
        installing,  // the helper is checking and staging the image
        closing,     // "The app closes now"
        failed,      // what went wrong; nothing was changed
        current,     // "You have the latest version"
    };
    Phase phase = Phase::offer;
    std::string version;            // "1.1"
    std::string current;            // what is running
    std::string size;               // "29.5 MB"
    std::vector<std::string> notes; // a few lines from the release
    float progress = 0.0f;          // 0..1 while downloading
    std::string detail;             // "12.4 of 29.5 MB"
    std::string left;               // "about 8 s left"
    std::string message;            // for failed
    bool test_build = false;        // the offer is a release that is not newer: say so
    int selected = 0;               // the button the cursor is on
    float pos = -1.0f;              // where the ring is, in buttons (a fraction while it glides); negative: on `selected`
};

void draw_update(hui::gfx::DrawList &list, const Env &env, const UpdateState &state, float clock, float t);

} // namespace sx
