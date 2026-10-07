#include "system_status.hpp"

#include "util.h"

#include <cstring>

#ifdef PLATFORM_PS5
extern "C" int sceSystemServiceGetStatus(void *status);
#endif

namespace sx
{

SystemStatus read_system_status()
{
    SystemStatus out;
#ifdef PLATFORM_PS5
    // The console's status record: the event count, then flags (SceSystemServiceStatus: byte 4 is
    // isSystemUiOverlaid, byte 5 isInBackgroundExecution). What it holds is logged when it changes, so
    // that a console that reports it differently can be seen in the log.
    alignas(8) unsigned char raw[256];
    std::memset(raw, 0, sizeof(raw));
    static bool failed = false;
    if (failed)
        return out;
    const int r = sceSystemServiceGetStatus(raw);
    if (r != 0)
    {
        failed = true;
        dlog("system status: sceSystemServiceGetStatus 0x%08x (not used)", static_cast<unsigned>(r));
        return out;
    }
    static unsigned char last[16];
    static bool first = true;
    if (first || std::memcmp(last, raw, sizeof(last)) != 0)
    {
        first = false;
        std::memcpy(last, raw, sizeof(last));
        dlog("system status: %02x%02x%02x%02x %02x %02x %02x %02x %02x %02x %02x %02x", raw[0], raw[1], raw[2], raw[3], raw[4],
             raw[5], raw[6], raw[7], raw[8], raw[9], raw[10], raw[11]);
    }
    out.valid = true;
    out.overlaid = raw[4] != 0;
    out.background = raw[5] != 0;
#endif
    return out;
}

} // namespace sx