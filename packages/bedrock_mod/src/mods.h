#pragma once

#include <atomic>
#include <span>
#include <string>
#include <vector>

#include "imgui.h"
#include "settings.h"

// The mods the menu lists, what they do, and their settings
namespace mods {

// Mods.kt's ModCategory, in its order
enum Category { ALL, FAVORITES, HYPIXEL, PERFORMANCE, VISUALS, HUD, UTILITY, QOL, OTHER, HIDDEN };

struct Mod {
    const char* id;
    const char* title;
    const char* description;
    Category category;
    std::vector<Setting> settings;

    std::string key(const char* field) const { return std::string(id) + "." + field; }
};

std::span<Mod> all();

// Declares every mod setting with config
void declare();

// Finds the game's camera so Zoom can change the field of view. Needs the
// game library loaded, so it runs from mod_init.
void install_zoom();

// Input the game sees, observed for the HUDs and Zoom. on_scroll returns true
// when Zoom took the scroll.
void on_key(int key, int action, bool playing);
void on_mouse_button(int button, int action);
bool on_scroll(double dy, bool playing);

// Called once per frame, before drawing
void tick(float dt);
bool hud_visible();
void draw_hud(ImDrawList* dl, float scale, ImFont* text, ImFont* small);

// Edit HUD: the menu steps aside and the HUDs can be dragged around
extern std::atomic<bool> editing_hud;
void edit_hud(ImDrawList* dl, float scale, ImFont* text, ImFont* small, ImU32 accent);

}  // namespace mods
