#include "ime.hpp"

#include "util.h"

#include <cstdint>
#include <cstring>

#ifdef PLATFORM_PS5_NATIVE

extern "C"
{
    int sceUserServiceInitialize(const void *params);
    int sceUserServiceGetForegroundUser(int *user);
    int sceImeDialogInit(const void *param, const void *extended);
    int sceImeDialogGetStatus(void);
    int sceImeDialogGetResult(void *result);
    int sceImeDialogTerm(void);
}

namespace sx
{

namespace
{

// OrbisImeDialogSetting. SDL's driver for the console fills in only the user, the
// length and the buffer, and leaves everything else zero; so does this, apart from where
// the dialog is put.
struct ImeParam
{
    std::uint32_t user_id;
    std::uint32_t type;
    std::uint64_t supported_languages;
    std::int32_t enter_label;
    std::int32_t input_method;
    void *filter;
    std::uint32_t option;
    std::uint32_t max_text_length;
    std::uint16_t *buffer;
    float pos_x, pos_y;
    std::int32_t horizontal_alignment, vertical_alignment;
    const std::uint16_t *placeholder;
    const std::uint16_t *title;
    std::uint8_t reserved[16];
};
static_assert(sizeof(ImeParam) == 96, "the dialog's setting is 96 bytes");

struct ImeResult
{
    std::int32_t end_status; // 0: the user accepted the text; 1 and 2: closed
    std::int32_t reserved[12];
};

constexpr int kMaxChars = 2048;
constexpr int kRunning = 1, kFinished = 2;
std::uint16_t g_text[kMaxChars + 8];
ImeParam g_param;

// The dialog writes UTF-16. Some firmware versions have been seen to widen the
// units to 32 bits; both are read.
std::string utf8_of(const std::uint16_t *u)
{
    const bool wide = u[0] != 0 && u[1] == 0 && u[2] != 0;
    std::string out;
    const auto put = [&](std::uint32_t cp) {
        if (cp < 0x80)
            out += static_cast<char>(cp);
        else if (cp < 0x800)
        {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        else if (cp < 0x10000)
        {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        else
        {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    };
    const int step = wide ? 2 : 1;
    for (int i = 0; i < kMaxChars; i += step)
    {
        std::uint32_t c = u[i];
        if (c == 0)
            break;
        if (c >= 0xD800 && c < 0xDC00 && u[i + step] >= 0xDC00 && u[i + step] < 0xE000)
        {
            c = 0x10000 + ((c - 0xD800) << 10) + (u[i + step] - 0xDC00);
            i += step;
        }
        put(c);
    }
    return out;
}

} // namespace

bool Ime::open()
{
    if (active_)
        return true;
    const int init = sceUserServiceInitialize(nullptr);
    (void)init; // already initialised is fine
    int user = 0;
    if (sceUserServiceGetForegroundUser(&user) != 0)
    {
        dlog("ime: no foreground user");
        return false;
    }
    std::memset(g_text, 0, sizeof(g_text));
    std::memset(&g_param, 0, sizeof(g_param));
    g_param.user_id = static_cast<std::uint32_t>(user);
    g_param.max_text_length = kMaxChars;
    g_param.buffer = g_text;
    // The dialog sits at the top right, not in the corner it opens in by default: its top right corner
    // is put at this point of the 1920 x 1080 screen (alignments: 0 left/top, 1 centre, 2 right/bottom).
    g_param.horizontal_alignment = 2;
    g_param.vertical_alignment = 0;
    g_param.pos_x = 1890.0f;
    g_param.pos_y = 30.0f;
    const int r = sceImeDialogInit(&g_param, nullptr);
    if (r != 0)
    {
        dlog("ime: sceImeDialogInit 0x%08x", static_cast<unsigned>(r));
        return false;
    }
    active_ = true;
    seen_running_ = false;
    opened_ = now_seconds();
    return true;
}

Ime::State Ime::poll(std::string *text)
{
    if (!active_)
        return State::idle;
    const int status = sceImeDialogGetStatus();
    if (status == kRunning)
    {
        seen_running_ = true;
        return State::open;
    }
    if (status != kFinished)
    {
        // Never started: give up after a few seconds rather than wait for ever.
        if (!seen_running_ && now_seconds() - opened_ > 3.0)
        {
            sceImeDialogTerm();
            active_ = false;
            return State::cancelled;
        }
        return seen_running_ ? State::cancelled : State::open;
    }
    ImeResult result;
    std::memset(&result, 0, sizeof(result));
    State out = State::cancelled;
    if (sceImeDialogGetResult(&result) == 0 && result.end_status == 0)
    {
        if (text)
            *text = utf8_of(g_text);
        out = State::done;
    }
    sceImeDialogTerm();
    active_ = false;
    return out;
}

} // namespace sx

#else // not on the console: there is no keyboard dialog

namespace sx
{
bool Ime::open()
{
    return false;
}
Ime::State Ime::poll(std::string *)
{
    return State::idle;
}
} // namespace sx

#endif
