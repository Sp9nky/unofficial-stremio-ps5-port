// Real-Debrid and Torbox in Settings: the API key (Real-Debrid) and the sign-in with a code (Torbox). What they do
// to playback is in src/debrid.cpp and play_stream().

#include "session.hpp"

#include <algorithm>

namespace sx
{

debrid::Credentials Session::debrid_creds() const
{
    return {std::string(), prefs_.torbox_key}; // Real-Debrid is used through its addon (Torrentio...), as in Stremio
}

void Session::debrid_settings_press(int service)
{
    (void)service; // only Torbox has a row
    if (!prefs_.torbox_key.empty())
    {
        if (debrid_confirm_ != 2)
        {
            debrid_confirm_ = 2;
        }
        else
        {
            debrid_confirm_ = 0;
            prefs_.torbox_key.clear();
            save_prefs();
            say("Torbox disconnected");
        }
        refresh_settings();
        return;
    }
    debrid_confirm_ = 0;
    debrid_connect_torbox();
    refresh_settings();
}

void Session::debrid_connect_torbox()
{
    const int gen = ++login_gen_;
    sign_in_open = true;
    sign_in = SignInState();
    sign_in.title = "Connect Torbox";
    sign_in.logo = false;
    sign_in.link = "torbox.app";
    sign_in.status = "Loading...";
    login_code_raw_.clear();
    login_qr_url_.clear();
    login_polling_ = false;
    dl_active_ = true;
    dl_device_code_.clear();
    dl_polling_ = false;
    bg<debrid::DeviceAuth>([]() { return debrid::torbox_device_start("Stremio for PS5"); },
                           [this, gen](debrid::DeviceAuth &a) {
                               if (gen != login_gen_ || !sign_in_open || !dl_active_)
                                   return;
                               if (!a.ok)
                               {
                                   sign_in.status = "Could not get a code: " + a.error;
                                   return;
                               }
                               dl_device_code_ = a.device_code;
                               dl_interval_ = std::max(2, a.interval_s);
                               sign_in.code = a.user_code;
                               std::string shown = a.friendly_url;
                               if (starts_with(shown, "https://"))
                                   shown = shown.substr(8);
                               sign_in.link = shown;
                               login_qr_url_ = "https://link.stremio.com/qr?data=" + url_encode(a.verification_url);
                               sign_in.status = "Waiting for you to approve it...";
                               dl_next_poll_ = now_seconds() + dl_interval_;
                               const std::int64_t left = iso8601_to_ms(a.expires_at) - now_epoch_ms();
                               dl_expires_ = now_seconds() + (left > 60000 ? static_cast<double>(left) / 1000.0 : 600.0);
                           });
}

void Session::debrid_poll()
{
    sign_in.qr = login_qr_url_.empty() ? 0 : tex_.get(login_qr_url_, ArtKind::qr);
    if (dl_device_code_.empty() || dl_polling_ || now_seconds() < dl_next_poll_)
        return;
    if (now_seconds() > dl_expires_)
    {
        sign_in.status = "The code expired. Try again from Settings.";
        dl_device_code_.clear();
        return;
    }
    dl_polling_ = true;
    const int gen = login_gen_;
    const std::string code = dl_device_code_;
    bg<debrid::DeviceToken>([code]() { return debrid::torbox_device_redeem(code); },
                            [this, gen](debrid::DeviceToken &t) {
                                dl_polling_ = false;
                                dl_next_poll_ = now_seconds() + dl_interval_;
                                if (gen != login_gen_ || !sign_in_open || !dl_active_)
                                    return;
                                if (t.status == debrid::DeviceToken::authorized)
                                {
                                    prefs_.torbox_key = t.access_token;
                                    save_prefs();
                                    sign_in_cancel();
                                    cue(hui::audio::Cue::saved);
                                    say("Torbox connected");
                                    refresh_settings();
                                }
                                else if (t.status == debrid::DeviceToken::expired)
                                {
                                    sign_in.status = "The code expired. Try again from Settings.";
                                    dl_device_code_.clear();
                                }
                                else if (t.status == debrid::DeviceToken::failed)
                                {
                                    sign_in.status = t.message.empty() ? "Torbox did not accept the sign-in. Try again from Settings."
                                                                       : "Torbox said: " + t.message;
                                    dl_device_code_.clear();
                                }
                            });
}

} // namespace sx
