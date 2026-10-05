// The DualSense, read straight from libScePad (PS5 only).
//
// SDL's PS5 joystick driver switches the controller into a vibration mode
// when it opens it, and the controller then kept vibrating while the system
// menu (PS button) was open. This reads the pad without touching vibration,
// and lets go of every button while the system UI has the controller.
#pragma once

#include <functional>

#include "app.h"

class PadPS5 {
public:
	bool init();
	// Calls down/up for buttons that changed since the last poll. The sticks
	// act as the d-pad, L2/R2 and the touchpad click as buttons.
	void poll(const std::function<void(Btn)>& down, const std::function<void(Btn)>& up);

private:
	void release_all(const std::function<void(Btn)>& up);

	int handle_ = -1;
	int user_ = -1;
	unsigned held_ = 0;  // bit per Btn
	double next_open_ = 0;
	int last_state_ = -1;
};
