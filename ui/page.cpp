#include "page.hpp"

namespace sx
{

void apply_page_backdrop(hui::gfx::BackdropSpec &backdrop, float time)
{
    backdrop.mode = hui::gfx::BackdropMode::gradient;
    backdrop.colors[0] = theme::ground;
    backdrop.colors[1] = theme::ground;
    backdrop.colors[2] = theme::accent_deep;
    backdrop.params[0] = 0.92f;
    backdrop.params[1] = 0.0f;
    backdrop.params[2] = 0.55f;
    backdrop.time = time;
}

} // namespace sx
