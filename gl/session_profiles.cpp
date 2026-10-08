// Profiles: one Stremio account per person. Each profile has a folder (profiles/<id>/) with its own
// settings.json (the sign-in, addons and preferences) and progress.json (where things were left); the list is
// profiles.json. Only the active profile is loaded; switching saves it and loads the other from scratch.

#include "session.hpp"

#include "profiles.hpp"

#include <algorithm>
#include <cstdio>
#include <unistd.h>

namespace sx
{

namespace
{

// A name that fits the tiles: trimmed, and at most 24 bytes, cut at a character.
std::string clean_name(const std::string &raw)
{
    std::string n = trim(raw);
    if (n.size() > 24)
    {
        std::size_t cut = 24;
        while (cut > 0 && (static_cast<unsigned char>(n[cut]) & 0xc0) == 0x80)
            --cut;
        n = trim(n.substr(0, cut));
    }
    return n;
}

} // namespace

std::string Session::profile_dir(const std::string &id) const
{
    return data_dir_ + "/profiles/" + id;
}

std::string Session::profile_file(const char *name) const
{
    const std::string id = active_ >= 0 && active_ < static_cast<int>(profiles_.size()) ? profiles_[static_cast<std::size_t>(active_)].id : "p1";
    return profile_dir(id) + "/" + name;
}

std::string Session::profile_name(int index) const
{
    return index >= 0 && index < static_cast<int>(profiles_.size()) ? profiles_[static_cast<std::size_t>(index)].name : std::string();
}

void Session::save_profiles() const
{
    json j;
    j["active"] = active_ >= 0 && active_ < static_cast<int>(profiles_.size()) ? profiles_[static_cast<std::size_t>(active_)].id : "";
    json list = json::array();
    for (const ProfileInfo &p : profiles_)
        list.push_back(json{{"id", p.id}, {"name", p.name}, {"colour", p.colour}});
    j["profiles"] = list;
    save_json(data_dir_ + "/profiles.json", j);
}

void Session::load_profiles()
{
    profiles_.clear();
    active_ = 0;
    std::string active_id;
    json j;
    if (load_json(data_dir_ + "/profiles.json", j))
    {
        active_id = jstr(j, "active");
        const json &list = jobj(j, "profiles");
        if (list.is_array())
            for (const auto &e : list)
            {
                ProfileInfo p;
                p.id = jstr(e, "id");
                p.name = jstr(e, "name");
                p.colour = static_cast<std::uint32_t>(jnum(e, "colour"));
                if (!p.id.empty())
                    profiles_.push_back(p);
            }
    }
    if (profiles_.empty())
    {
        // First start with profiles: what was signed in and watched so far becomes the first profile.
        ProfileInfo p;
        p.id = "p1";
        p.colour = profile_colour(0);
        json old;
        if (load_json(data_dir_ + "/settings.json", old))
        {
            std::string email = jstr(old, "user_email");
            const std::size_t at = email.find('@');
            if (at != std::string::npos && at > 0)
            {
                p.name = clean_name(email.substr(0, at));
                if (!p.name.empty() && p.name[0] >= 'a' && p.name[0] <= 'z')
                    p.name[0] = static_cast<char>(p.name[0] - 'a' + 'A');
            }
        }
        if (p.name.empty())
            p.name = "Main";
        make_dirs(profile_dir(p.id));
        json data;
        if (load_json(data_dir_ + "/settings.json", data))
            save_json(profile_dir(p.id) + "/settings.json", data);
        if (load_json(data_dir_ + "/progress.json", data))
            save_json(profile_dir(p.id) + "/progress.json", data);
        profiles_.push_back(p);
        active_id = p.id;
    }
    for (std::size_t i = 0; i < profiles_.size(); ++i)
        if (profiles_[i].id == active_id)
            active_ = static_cast<int>(i);
    make_dirs(profile_dir(profiles_[static_cast<std::size_t>(active_)].id));
    save_profiles();
}

std::string Session::profile_email(int index) const
{
    if (index < 0 || index >= static_cast<int>(profiles_.size()))
        return std::string();
    if (index == active_)
        return signed_in() ? prefs_.user_email : std::string();
    json j;
    if (!load_json(profile_dir(profiles_[static_cast<std::size_t>(index)].id) + "/settings.json", j) || jstr(j, "auth_key").empty())
        return std::string();
    const std::string email = jstr(j, "user_email");
    return email.empty() ? "Signed in" : email;
}

// Everything that belongs to the person who was loaded goes; the other's is read from disk.
void Session::reset_for_profile()
{
    sign_in_cancel();
    ++home_gen_;
    ++addons_gen_;
    prefs_ = Prefs();
    progress_.clear();
    watched_.clear();
    library_.clear();
    library_loaded_ = false;
    addons_.clear();
    board_.clear();
    board_map_.clear();
    lib_items_.clear();
    disc_items_.clear();
    confirm_sign_out_ = false;
    search_open = false;
    search.rows.clear();
    search_.clear();
    search_map_.clear();
    changed_ |= kSearchNew;
    board.rows.clear();
    board.status = "Loading your catalogs...";
    changed_ |= kBoard;
    load_prefs();
    load_progress();
    refresh_settings();
    refresh_library();
    refresh_addons();
    load_addons();
    if (signed_in())
        load_library();
}

void Session::profile_select(int index)
{
    if (index < 0 || index >= static_cast<int>(profiles_.size()) || index == active_)
        return;
    save_prefs();
    save_progress();
    active_ = index;
    save_profiles();
    reset_for_profile();
}

int Session::profile_add(const std::string &name, std::uint32_t colour)
{
    ProfileInfo p;
    for (int n = static_cast<int>(profiles_.size()) + 1;; ++n)
    {
        p.id = "p" + std::to_string(n);
        const bool used = std::any_of(profiles_.begin(), profiles_.end(), [&](const ProfileInfo &q) { return q.id == p.id; });
        if (!used)
            break;
    }
    p.name = clean_name(name);
    if (p.name.empty())
        p.name = "Profile " + std::to_string(profiles_.size() + 1);
    p.colour = colour;
    // The new person starts with this one's preferences (subtitles, server, sounds), but none of the account.
    Prefs fresh = prefs_;
    fresh.auth_key.clear();
    fresh.user_email.clear();
    fresh.extra_addons.clear();
    make_dirs(profile_dir(p.id));
    save_json(profile_dir(p.id) + "/settings.json", prefs_json(fresh));
    profiles_.push_back(p);
    save_profiles();
    ++profiles_rev_;
    return static_cast<int>(profiles_.size()) - 1;
}

void Session::profile_update(int index, const std::string &name, std::uint32_t colour)
{
    if (index < 0 || index >= static_cast<int>(profiles_.size()))
        return;
    ProfileInfo &p = profiles_[static_cast<std::size_t>(index)];
    const std::string n = clean_name(name);
    if (!n.empty())
        p.name = n;
    p.colour = colour;
    save_profiles();
    ++profiles_rev_;
    if (index == active_)
        refresh_settings();
}

void Session::profile_remove_ask(int index)
{
    if (index < 0 || index >= static_cast<int>(profiles_.size()))
        return;
    if (profiles_.size() < 2)
    {
        say("Keep at least one profile.", 4.0, true);
        return;
    }
    const std::string id = profiles_[static_cast<std::size_t>(index)].id;
    open_dropdown("Remove " + profiles_[static_cast<std::size_t>(index)].name + "?", {"Remove profile", "Keep"}, 1, [this, id](int i) {
        if (i != 0)
            return;
        const auto at = std::find_if(profiles_.begin(), profiles_.end(), [&](const ProfileInfo &p) { return p.id == id; });
        if (at == profiles_.end() || profiles_.size() < 2)
            return;
        const int index = static_cast<int>(at - profiles_.begin());
        const bool was_active = index == active_;
        profiles_.erase(profiles_.begin() + index);
        if (was_active)
        {
            // The next one is loaded; the removed folder is not written to again.
            active_ = std::min(index, static_cast<int>(profiles_.size()) - 1);
            reset_for_profile();
        }
        else if (active_ > index)
        {
            --active_;
        }
        const std::string dir = profile_dir(id);
        std::remove((dir + "/settings.json").c_str());
        std::remove((dir + "/progress.json").c_str());
        rmdir(dir.c_str());
        save_profiles();
        ++profiles_rev_;
        say("Profile removed");
    });
    dropdown.message = "Deletes its sign-in on this console.";
    dropdown.danger = 0;
    dropdown.dialog = false;
}

void Session::profile_sign_in()
{
    if (!signed_in())
        login_start();
}

} // namespace sx
