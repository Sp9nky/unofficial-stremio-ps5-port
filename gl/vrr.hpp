// Refresh-rate matching on the PS5's VRR link, as PS5MC does it (xbmc/windowing/ps5): with the console's VRR setting
// on and a VRR-capable TV, the system runs the title on a VRR link (the output reports ~120 Hz) and the TV refreshes
// whenever the title presents a frame. So the title paces its own presents: 59.94 Hz in the menus, and during a video
// the lowest multiple of the video's frame rate within the PS5's VRR range of 48-120 Hz (23.976 fps: 71.928 Hz, each
// frame 3 times; 24: 48; 25: 50; 29.97/30: 59.94/60; 50, 59.94, 60: the same). Every frame is then on screen equally
// long: no 3:2 judder. Without a VRR link nothing here does anything.

#pragma once

namespace sx::vrr
{

// Every frame: `enabled` is the setting; `video_fps` the frame rate of the video being played (0 when none). Reads
// the link now and then (the console switches a title onto or off its VRR link at moments of its own).
void update(bool enabled, double video_fps);

// Right before presenting the frame: waits for the next presentation tick when the link is a VRR one.
void pace();

} // namespace sx::vrr
