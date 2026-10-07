#pragma once

#include "gfx/backdrop_spec.hpp"
#include "gfx/draw_list.hpp"
#include "ui/fonts.hpp"

namespace sx::preview
{

void draw_scripts(hui::gfx::DrawList &list, hui::gfx::BackdropSpec &backdrop, const hui::ui::Fonts &fonts);

}
