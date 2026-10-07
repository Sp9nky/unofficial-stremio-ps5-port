// Whether the PlayStation's own menu is over the app, or the app is in the background: the video
// then pauses (pressing the PS button, going to the home screen).

#pragma once

namespace sx
{

struct SystemStatus
{
    bool valid = false;      // the console answered
    bool overlaid = false;   // the system's menu or a message is on top of the app
    bool background = false; // the app is not the one in front
};

SystemStatus read_system_status();

} // namespace sx