// Profiles: the "Who's watching?" screen (the chosen profile large in the middle, the others beside it,
// smaller) and the dialog that edits one.

#pragma once

#include "chrome.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace sx
{

struct ProfileEntry
{
    std::string name;
    std::string email; // empty: not signed in yet
    std::uint32_t colour = 0x7b5bf5;
};

// The colours a profile can have.
constexpr int kProfileColours = 6;
std::uint32_t profile_colour(int index);
int profile_colour_index(std::uint32_t rgb); // the nearest, for a stored colour

struct ProfilesState
{
    std::vector<ProfileEntry> items; // the "Add profile" tile comes after them
    float focus = 0.0f;              // where the cursor is, in tiles: it glides, so it is a fraction while moving
    bool can_leave = false;          // opened from Settings: Circle goes back
};

void draw_profile_picker(hui::gfx::DrawList &list, const Env &env, const ProfilesState &state, float t);

struct EditorState
{
    ProfileEntry profile;
    bool is_new = false;
    int row = 0; // 0 name, 1 colour, 2 account, 3 done, 4 remove (not for a new one)
    int colour_index = 0;
    bool signed_in = false;
    float pos = -1.0f; // where the ring is, in rows (a fraction while it glides); negative: on `row`
};

void draw_profile_editor(hui::gfx::DrawList &list, const Env &env, const EditorState &state, float t);

} // namespace sx
