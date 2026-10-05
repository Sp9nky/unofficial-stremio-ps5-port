// System modules a native app doesn't get at start-up.
//
// The PS5 loads only an app's standard set of modules. libSceImeDialog (the
// on-screen keyboard) isn't in it: SDL's calls into it jumped to address 0.
// Looking its functions up by name doesn't work in a title either (no runtime
// loader). What works, as in the Kodi port for PS5 (PS5ImeDialog.cpp): import
// the functions normally and have the system load the module by its ID
// before the first call - ps5_load_modules(), called before SDL starts.
//
// libSceKeyboard (USB keyboards) is not needed; these stand-ins report none.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

int sceSysmoduleLoadModule(uint16_t id);
int sceKernelDebugOutText(int channel, const char* text);

#define SCE_SYSMODULE_IME_DIALOG 0x0096

void ps5_load_modules(void) {
	char line[128];
	int r = sceSysmoduleLoadModule(SCE_SYSMODULE_IME_DIALOG);
	snprintf(line, sizeof(line), "[stremio] on-screen keyboard module: %s (0x%x)\n", r == 0 ? "loaded" : "FAILED",
	         (unsigned)r);
	sceKernelDebugOutText(0, line);
}

// ---- USB keyboards (libSceKeyboard): none ---------------------------------------------------

int sceKeyboardInit(void) { return 0; }

int sceKeyboardOpen(int user, int type, int index, void* param) {
	(void)user; (void)type; (void)index; (void)param;
	return -1;  // SDL then reads no keyboard
}

int sceKeyboardClose(int handle) { (void)handle; return 0; }

int sceKeyboardReadState(int handle, void* state) {
	(void)handle; (void)state;
	return -1;
}
