// The interface's sounds: soft sounds for moving around, going back, switching page and so on.
// They come from ps5-homebrew-ui's sound bank (the "glass" set, in <app>/sounds/glass) and play
// through its mixer on the console's audio port.
//
// The console gives the app one audio port at a time: while a video plays, the player has it.
// The player asks for it when it starts (AudioPort::player_wants) and this lets go of it, then
// takes it back when the player is done.

#pragma once

#include "audio/cues.hpp"

#include <memory>
#include <string>

namespace sx
{

class UiSound
{
  public:
    UiSound();
    ~UiSound();

    // Loads the sounds in <dir>/glass and opens the audio port. False: no sound (not fatal).
    bool start(const std::string &dir);
    // Plays a cue now. Cues asked for while the player has the port are dropped.
    void play(hui::audio::Cue cue);
    // Plays a cue as soon as the port is back (the player has just finished).
    void play_when_back(hui::audio::Cue cue);
    void set_enabled(bool on)
    {
        enabled_ = on;
    }
    // Once a frame: hands the port to the player and takes it back.
    void update(float dt);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool enabled_ = true;
};

} // namespace sx
