#pragma once

#include "imgui.h"

// The menu itself, laid out after OneConfig's Polyfrosted Dark shell: a
// 1391x700 dp window with the sidebar on the left and the mods page on the
// right. Sizes below are OneConfig's dp, multiplied by scale() to get pixels.
namespace shell {

// Pixels per dp for a display of this size, shrinking the shell to fit
// inside 90% of it like OneConfig does
float scale_for(ImVec2 display);

// Fonts are rasterized at one size, so they are rebuilt whenever the scale changes
void load_fonts(float scale);

// The fonts the HUDs draw with, loaded alongside the menu's
ImFont* hud_text();
ImFont* hud_small();

// Builds this frame's draw lists. Returns true when the close button was pressed.
bool draw(ImVec2 display, float scale);

}  // namespace shell
