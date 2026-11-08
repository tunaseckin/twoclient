#pragma once

#include <vector>

// One row on a settings page, OneConfig's @Switch, @Slider, @Dropdown and
// @Keybind. The value lives in config under key.
struct Setting {
    enum Kind { SWITCH, SLIDER, DROPDOWN, KEYBIND };

    Kind kind;
    const char* key;
    const char* title;
    const char* description;
    // the subcategory header the row sits under
    const char* group;
    float def;
    float min = 0, max = 1, step = 0;
    std::vector<const char*> options = {};
};

void declare(const std::vector<Setting>& settings);
