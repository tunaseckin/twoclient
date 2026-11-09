#pragma once

#include "imgui.h"

// OneClient's title screen. The resource pack lays the game's own buttons out
// invisibly on the same grid and marks the screen with a pixel; the mod sees the
// mark, draws the buttons over them, and handles the ones the game has no
// button for, like opening the menu.
namespace title {

// Looks for the pack's mark in the frame the game just drew
bool showing();

// Draws the screen. Returns true when the menu should open.
bool draw(ImDrawList* dl, ImVec2 display, float scale, ImVec2 mouse, bool clicked);

// The light or dark look the title screen buttons share
struct Look {
    ImU32 fill, border, text, dim;
};
bool light();
Look look(bool hover);

// Whether a click at this point lands on a button only the mod handles
bool owns(ImVec2 point, ImVec2 display);

}  // namespace title
