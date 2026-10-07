// Invented sample content for the UI previews. None of it is real.

#pragma once

#include "content.hpp"
#include "gfx/renderer.hpp"

#include <string>

namespace sx::preview
{

// Decodes a PNG and uploads it. Returns 0 on failure.
std::uint32_t load_texture(hui::gfx::Renderer &renderer, const std::string &path, int *w = nullptr,
                           int *h = nullptr);

// Each uploads the generated artwork (make_sample_art.py) as needed.
bool load_board(hui::gfx::Renderer &renderer, const std::string &art_dir, BoardContent *out);
bool load_discover(hui::gfx::Renderer &renderer, const std::string &art_dir, DiscoverContent *out);
bool load_library(hui::gfx::Renderer &renderer, const std::string &art_dir, LibraryContent *out);
bool load_detail(hui::gfx::Renderer &renderer, const std::string &art_dir, DetailContent *series, DetailContent *movie);
bool load_search(hui::gfx::Renderer &renderer, const std::string &art_dir, SearchContent *found, SearchContent *none);
// A backdrop used as a stand-in for a video frame.
std::uint32_t load_frame(hui::gfx::Renderer &renderer, const std::string &art_dir, int index);
PlayerState sample_player();
std::uint32_t load_poster(hui::gfx::Renderer &renderer, const std::string &art_dir, int index);
LaunchState sample_launch(std::uint32_t backdrop, std::uint32_t poster);
void load_addons(AddonsContent *out);
void load_settings(SettingsContent *out);

} // namespace sx::preview
