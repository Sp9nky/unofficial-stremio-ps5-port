#include "pad_ps5.h"

#include <cstdint>
#include <cstdlib>

#include "util.h"

extern "C" {
// libScePad, as declared by the payload SDK's SDL port.
struct PadData {
	uint32_t buttons;
	uint8_t lx, ly, rx, ry;
	uint8_t l2, r2;
	uint16_t padding;
	float quat[4], vel[3], accel[3];
	uint8_t touch[24];  // finger count + 2 touch points
	uint8_t connected;
	uint64_t timestamp;
	uint8_t ext[16];
	uint8_t count;
	uint8_t unknown[15];
};
static_assert(sizeof(PadData) == 120, "must match libScePad's ScePadData");
struct PadVibration {
	uint8_t large_motor, small_motor;
};
int scePadInit(void);
int scePadOpen(int user_id, int type, int index, void* param);
int scePadGetHandle(int user_id, int type, int index);
int scePadReadState(int handle, PadData* data);
int scePadSetVibration(int handle, const PadVibration* vib);
int scePadClose(int handle);
int sceUserServiceInitialize(void*);
int sceUserServiceGetForegroundUser(int* user_id);
int sceUserServiceGetLoginUserIdList(int user_ids[4]);
}

namespace {

const uint32_t kL3 = 0x0002, kR3 = 0x0004, kOptions = 0x0008, kUp = 0x0010, kRight = 0x0020, kDown = 0x0040,
               kLeft = 0x0080, kL2 = 0x0100, kR2 = 0x0200, kL1 = 0x0400, kR1 = 0x0800, kTriangle = 0x1000,
               kCircle = 0x2000, kCross = 0x4000, kSquare = 0x8000, kTouchpad = 0x100000;
// Set while the system (PS button menu, notifications) has the controller.
const uint32_t kIntercepted = 0x80000000u;
const int kAlreadyOpened = int(0x80920004u);

const struct {
	uint32_t mask;
	Btn btn;
} kButtons[] = {
    {kCross, Btn::Cross}, {kCircle, Btn::Circle}, {kSquare, Btn::Square},     {kTriangle, Btn::Triangle},
    {kOptions, Btn::Options}, {kL1, Btn::L1},     {kR1, Btn::R1},             {kL2, Btn::L2},
    {kR2, Btn::R2},       {kL3, Btn::L3},         {kR3, Btn::R3},             {kTouchpad, Btn::Touchpad},
    {kUp, Btn::Up},       {kDown, Btn::Down},     {kLeft, Btn::Left},         {kRight, Btn::Right},
};

inline unsigned bit(Btn b) { return 1u << unsigned(b); }

// A stick pushed far enough counts as a d-pad press; it lets go a bit
// closer to the centre so it doesn't flicker at the edge.
bool stick(int v, bool negative, bool held) {
	int d = negative ? 128 - v : v - 128;
	return d > (held ? 50 : 80);
}

}  // namespace

bool PadPS5::init() {
	int r = sceUserServiceInitialize(nullptr);
	if (r != 0 && r != int(0x80960003u)) dlog("pad: sceUserServiceInitialize: 0x%x", r);
	r = scePadInit();
	if (r != 0) {
		dlog("pad: scePadInit: 0x%x", r);
		return false;
	}
	return true;
}

void PadPS5::release_all(const std::function<void(Btn)>& up) {
	for (unsigned i = 0; held_ && i < 32; i++)
		if (held_ & (1u << i)) {
			held_ &= ~(1u << i);
			up(Btn(i));
		}
}

void PadPS5::poll(const std::function<void(Btn)>& down, const std::function<void(Btn)>& up) {
	int user = -1;
	if (sceUserServiceGetForegroundUser(&user) != 0 || user < 0) {
		int users[4] = {-1, -1, -1, -1};
		user = sceUserServiceGetLoginUserIdList(users) == 0 ? users[0] : -1;
	}
	if (user != user_ && handle_ >= 0) {
		scePadClose(handle_);
		handle_ = -1;
		release_all(up);
	}
	user_ = user;
	if (handle_ < 0) {
		if (user < 0 || now_seconds() < next_open_) return;
		int h = scePadOpen(user, 0, 0, nullptr);
		if (h == kAlreadyOpened) h = scePadGetHandle(user, 0, 0);
		if (h < 0) {
			dlog("pad: scePadOpen(user 0x%x): 0x%x", user, h);
			next_open_ = now_seconds() + 2;
			return;
		}
		handle_ = h;
		PadVibration off = {0, 0};
		scePadSetVibration(handle_, &off);
		dlog("pad: opened for user 0x%x", user);
	}

	PadData pad = {};
	int r = scePadReadState(handle_, &pad);
	int state = r != 0 ? 1 : !pad.connected ? 2 : (pad.buttons & kIntercepted) ? 3 : 0;
	if (state != last_state_) {
		static const char* names[] = {"ok", "read failed", "not connected", "system has it"};
		dlog("pad: %s (0x%x, buttons 0x%x)", names[state], r, pad.buttons);
		last_state_ = state;
	}
	// While the system has the controller it sends no presses here anyway;
	// only a failed read or a missing pad stops input.
	if (state == 1 || state == 2) {
		release_all(up);
		return;
	}

	unsigned want = 0;
	for (auto& b : kButtons)
		if (pad.buttons & b.mask) want |= bit(b.btn);
	// Both sticks steer like the d-pad.
	auto held = [&](Btn b) { return (held_ & bit(b)) != 0; };
	if (stick(pad.ly, true, held(Btn::Up)) || stick(pad.ry, true, held(Btn::Up))) want |= bit(Btn::Up);
	if (stick(pad.ly, false, held(Btn::Down)) || stick(pad.ry, false, held(Btn::Down))) want |= bit(Btn::Down);
	if (stick(pad.lx, true, held(Btn::Left)) || stick(pad.rx, true, held(Btn::Left))) want |= bit(Btn::Left);
	if (stick(pad.lx, false, held(Btn::Right)) || stick(pad.rx, false, held(Btn::Right))) want |= bit(Btn::Right);

	unsigned changed = want ^ held_;
	for (unsigned i = 0; changed && i < 32; i++) {
		if (!(changed & (1u << i))) continue;
		if (want & (1u << i)) {
			held_ |= 1u << i;
			down(Btn(i));
		} else {
			held_ &= ~(1u << i);
			up(Btn(i));
		}
	}
}
