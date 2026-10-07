// The console's own keyboard (libSceImeDialog): a full-screen dialog that takes
// the controller while it is up and hands back what was typed. The app only
// asks for it and waits.

#pragma once

#include <string>

namespace sx
{

class Ime
{
  public:
    enum class State
    {
        idle,      // no dialog
        open,      // the dialog is up
        done,      // the user finished: text is set
        cancelled, // the user closed it
    };

    // Opens the dialog; false if the console would not (or this is not a console).
    bool open();
    // Once per frame while a dialog is up.
    State poll(std::string *text);
    bool active() const
    {
        return active_;
    }

  private:
    bool active_ = false;
    double opened_ = 0.0;
    bool seen_running_ = false;
};

} // namespace sx
