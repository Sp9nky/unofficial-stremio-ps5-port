// The version of this build, shown in Settings and compared with the newest release on GitHub.
#pragma once

namespace sx
{

constexpr const char *kAppVersion = "1.1 (HDR test 3)";
constexpr const char *kTitleId = "PPSA77711";
constexpr const char *kUpdateRepo = "Sp9nky/unofficial-stremio-ps5-port";
// Only for trying the update before a newer release exists: offers the newest published release even when it is
// not newer than this build. Keep it false in a release.
constexpr bool kUpdateOfferAnyVersion = false;

} // namespace sx
