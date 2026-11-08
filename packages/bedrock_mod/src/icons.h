#pragma once

#include "imgui.h"

// OneConfig's SVG icons, rasterized once per pixel size into white textures
// so a draw call can tint them any color
namespace icons {

// Draws the icon called name (its file name without .svg) into a box of
// size px with its top-left at pos
void draw(ImDrawList* dl, const char* name, ImVec2 pos, float px, ImU32 color);
// The same, turned by degrees clockwise around its middle, for chevrons
void draw_rotated(ImDrawList* dl, const char* name, ImVec2 pos, float px, ImU32 color, float degrees);

}  // namespace icons
