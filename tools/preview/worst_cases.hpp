// The worst realistic data for every screen: long names, other scripts,
// missing artwork, empty fields. The same screens are drawn with it to see
// what breaks (the break-ui pass).

#pragma once

#include "content.hpp"
#include "gfx/renderer.hpp"

#include <string>

namespace sx::preview
{

struct WorstCases
{
    BoardContent board;
    DiscoverContent discover;
    LibraryContent library;
    AddonsContent addons;
    SettingsContent settings;
    DetailContent detail;
    PlayerState player;
    LaunchState launch;
};

bool load_worst(hui::gfx::Renderer &renderer, const std::string &art_dir, WorstCases *out);

} // namespace sx::preview
