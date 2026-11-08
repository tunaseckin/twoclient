#pragma once

#include <string>

#include "imgui.h"

// What the shell shows to the right of the sidebar
namespace pages {

enum Page { MODS, PROFILES, KEYBINDS, THEMES, PREFERENCES, CHANGELOG, CREDITS, MOD_CONFIG, CHANGELOG_ENTRY };

struct Route {
    Page page = MODS;
    int mod = -1;  // which mod a MOD_CONFIG page or which entry a CHANGELOG_ENTRY page is for

    bool operator==(const Route&) const = default;
};

const char* title(const Route& route);
const char* icon(Page page);

// Declares the menu's own settings with config
void declare();

// Draws a page into [a, b] in pixels, filtered by the search text. A page
// that wants to go somewhere sets *go.
void draw(const Route& route, ImVec2 a, ImVec2 b, const std::string& search, Route* go);

}  // namespace pages
