#include "ui_sound.hpp"

#include "pcm_out.h"
#include "util.h"

#ifdef PLATFORM_PS5
#include "audio/mixer.hpp"
#include "audio/wav.hpp"
#include "core/save_file.hpp"
#include "platform/ps5/audio_out.hpp"

#include <chrono>
#endif

namespace sx
{

#ifdef PLATFORM_PS5

namespace
{

using Clock = std::chrono::steady_clock;

// How soon the same cue may play again: moving along posters must not turn into a rattle.
double min_gap_seconds(hui::audio::Cue cue)
{
    using hui::audio::Cue;
    switch (cue)
    {
    case Cue::focus:
        return 0.045;
    case Cue::error:
        return 0.30;
    default:
        return 0.12;
    }
}

} // namespace

struct UiSound::Impl
{
    hui::audio::Mixer mixer;
    hui::audio::SoundBank bank;
    hui::ps5::AudioOut out;
    bool loaded = false;
    bool open = false;
    double back_in = 0.0; // seconds until the port is taken back from the player
    bool has_pending = false;
    hui::audio::Cue pending = hui::audio::Cue::focus;
    Clock::time_point last[static_cast<std::size_t>(hui::audio::Cue::count)]{};
};

UiSound::UiSound() : impl_(new Impl)
{
}

UiSound::~UiSound()
{
    if (impl_ && impl_->open)
    {
        impl_->out.stop();
        AudioPort::ui_holds = false;
    }
}

bool UiSound::start(const std::string &dir)
{
    // The sounds are loaded by name, one by one: the console does not list a folder reliably (the toolkit's
    // own loader needs an index.txt for that), and these ten are all there are.
    struct Entry
    {
        hui::audio::Cue cue;
        const char *file;
    };
    using hui::audio::Cue;
    static const Entry kSounds[] = {{Cue::focus, "focus_02.wav"},   {Cue::tab, "tab_01.wav"},       {Cue::back, "back_01.wav"},
                                    {Cue::toggle, "toggle_02.wav"}, {Cue::error, "error_01.wav"},   {Cue::notify, "notify_01.wav"},
                                    {Cue::launch, "launch_01.wav"}, {Cue::saved, "saved_01.wav"},   {Cue::resume, "resume_01.wav"},
                                    {Cue::welcome, "welcome_01.wav"}};
    int files = 0;
    for (const Entry &e : kSounds)
    {
        const std::string path = dir + "/glass/" + e.file;
        std::string data;
        if (!hui::save::read_file(path, &data, 4u << 20))
        {
            dlog("sound: cannot read %s", path.c_str());
            continue;
        }
        hui::audio::DecodedWav wav = hui::audio::decode_wav(data);
        if (!wav.ok())
        {
            dlog("sound: %s: %s", e.file, wav.error.c_str());
            continue;
        }
        impl_->bank.add(hui::audio::SoundSet::glass, e.cue, std::move(wav.samples), wav.frames);
        ++files;
    }
    dlog("sound: %d of %zu sounds loaded from %s", files, sizeof(kSounds) / sizeof(kSounds[0]), dir.c_str());
    if (files == 0)
        return false;
    impl_->loaded = true;
    impl_->mixer.set_bus_gain(hui::audio::Bus::ui, 0.7f);
    impl_->mixer.set_bus_gain(hui::audio::Bus::sfx, 0.7f);
    if (!impl_->out.start(impl_->mixer))
    {
        dlog("sound: the audio port did not open");
        return false;
    }
    impl_->open = true;
    AudioPort::ui_holds = true;
    return true;
}

void UiSound::play(hui::audio::Cue cue)
{
    if (!enabled_ || !impl_->open)
        return;
    const std::size_t i = static_cast<std::size_t>(cue);
    const Clock::time_point now = Clock::now();
    if (std::chrono::duration<double>(now - impl_->last[i]).count() < min_gap_seconds(cue))
        return;
    impl_->last[i] = now;
    hui::audio::CueEvent event;
    event.cue = cue;
    event.set = hui::audio::SoundSet::glass;
    impl_->bank.play(impl_->mixer, hui::audio::SoundSet::glass, event);
}

void UiSound::play_when_back(hui::audio::Cue cue)
{
    if (impl_->open)
    {
        play(cue);
        return;
    }
    impl_->has_pending = true;
    impl_->pending = cue;
}

void UiSound::update(float dt)
{
    if (!impl_->loaded)
        return;
    if (AudioPort::player_wants.load())
    {
        // The player starts or is playing: it has the port.
        if (impl_->open)
        {
            impl_->out.stop();
            impl_->open = false;
            AudioPort::ui_holds = false;
        }
        impl_->back_in = 0.4;
        return;
    }
    if (!impl_->open)
    {
        impl_->back_in -= dt;
        if (impl_->back_in > 0.0)
            return;
        if (impl_->out.start(impl_->mixer))
        {
            impl_->open = true;
            AudioPort::ui_holds = true;
            if (impl_->has_pending)
            {
                impl_->has_pending = false;
                play(impl_->pending);
            }
        }
        else
        {
            impl_->back_in = 1.0; // try again in a moment
        }
    }
}

#else // on a PC the screens are only drawn: no sound

struct UiSound::Impl
{
};

UiSound::UiSound() : impl_(new Impl)
{
}

UiSound::~UiSound() = default;

bool UiSound::start(const std::string &)
{
    return false;
}

void UiSound::play(hui::audio::Cue)
{
}

void UiSound::play_when_back(hui::audio::Cue)
{
}

void UiSound::update(float)
{
}

#endif

} // namespace sx
