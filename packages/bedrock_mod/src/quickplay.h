#pragma once

#include "imgui.h"

// The title screen's extras that need the game's files: the Quickplay list of
// recent worlds and servers, and the player's skin turning on the right
namespace quickplay {

// Draws the list in the left column, refreshing it every few seconds
void draw_list(ImDrawList* dl, ImVec2 display, float scale, ImVec2 mouse, bool clicked);

// The header is the mod's (its chevron folds the list), and so are the rows while folded
bool owns(ImVec2 point, ImVec2 display);

// Draws the skin as a turning 3D model, its feet at bottom, height tall
void draw_skin(ImDrawList* dl, ImVec2 bottom, float height, float time);

}  // namespace quickplay
