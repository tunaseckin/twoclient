#include "title.h"

#include <EGL/egl.h>

#include <cmath>

#include "icons.h"
#include "ui.h"

namespace title {
namespace {

// Fractions of the screen; ui/start_screen.json puts the game's buttons on the same spots
struct Button {
    const char* label;
    const char* icon;
    float x, y, w, h;
    bool ours;  // the mod handles it, the game has nothing under it
};

const Button buttons[] = {
        {"Singleplayer", "right-arrow", 0.325f, 0.52f, 0.35f, 0.055f, false},
        {"Multiplayer", "profiles", 0.325f, 0.595f, 0.35f, 0.055f, false},
        {"Realms", "external-link", 0.325f, 0.67f, 0.35f, 0.055f, false},
        {"Settings", "settings", 0.325f, 0.745f, 0.17f, 0.055f, false},
        {"Mods", "qol", 0.505f, 0.745f, 0.17f, 0.055f, true},
        {"Profile", "profiles", 0.725f, 0.52f, 0.24f, 0.055f, false},
        {"Wardrobe", "paintbrush", 0.725f, 0.595f, 0.24f, 0.055f, false},
        {"Notifications", "bell", 0.725f, 0.67f, 0.24f, 0.055f, false},
        {nullptr, "hud", 0.925f, 0.035f, 0.034f, 0.052f, true},
        // the pack's toggle back to the game's own title screen sits under this one
        {"Vanilla menu", "eye", 0.012f, 0.035f, 0.11f, 0.052f, false},
};

ImVec2 corner(const Button& b, ImVec2 d) { return ImVec2(std::round(b.x * d.x), std::round(b.y * d.y)); }
ImVec2 far(const Button& b, ImVec2 d) { return ImVec2(std::round((b.x + b.w) * d.x), std::round((b.y + b.h) * d.y)); }

// the screen's color next to the mark, to paint over it
unsigned char beside[4] = {};
// the game's own title screen is up, behind the pack's toggle
bool vanilla = false;

bool inside(const Button& b, ImVec2 p, ImVec2 d) {
    ImVec2 a = corner(b, d), z = far(b, d);
    return p.x >= a.x && p.x < z.x && p.y >= a.y && p.y < z.y;
}

}  // namespace

bool showing() {
    using ReadPixels = void (*)(int, int, int, int, unsigned, unsigned, void*);
    static auto read = reinterpret_cast<ReadPixels>(eglGetProcAddress("glReadPixels"));
    using Viewport = void (*)(unsigned, int*);
    static auto get = reinterpret_cast<Viewport>(eglGetProcAddress("glGetIntegerv"));
    if (!read || !get)
        return false;
    int vp[4] = {};
    get(0x0BA2, vp);  // GL_VIEWPORT
    unsigned char px[4] = {};
    // the mark is the pack's 2px square in the top-left corner, colored 1, 254, 2
    read(vp[0] + 1, vp[1] + vp[3] - 2, 1, 1, 0x1908, 0x1401, px);
    read(vp[0] + 14, vp[1] + vp[3] - 2, 1, 1, 0x1908, 0x1401, beside);
    vanilla = px[0] < 12 && px[1] < 12 && px[2] > 240;
    return vanilla || (px[0] < 12 && px[1] > 240 && px[2] < 14);
}

bool owns(ImVec2 point, ImVec2 display) {
    if (vanilla)
        return false;
    for (const Button& b : buttons)
        if (b.ours && inside(b, point, display))
            return true;
    return false;
}

void draw_button(ImDrawList* dl, const Button& b, ImVec2 d, float s, ImVec2 mouse) {
    ImFont* font = ui::fonts.m16;
    float text = d.y * 0.026f;
    ImVec2 a = corner(b, d), z = far(b, d);
    bool hover = inside(b, mouse, d);
    float r = 5 * s;
    dl->AddRectFilled(a, z, hover ? IM_COL32(70, 72, 92, 150) : IM_COL32(24, 22, 34, 120), r);
    dl->AddRect(a, z, hover ? IM_COL32(235, 238, 255, 190) : IM_COL32(225, 228, 255, 120), r, 0, std::max(1.f, s));
    ImU32 fg = IM_COL32(222, 225, 255, 255);
    float icon = text * 1.1f;
    if (!b.label) {
        icons::draw(dl, b.icon, ImVec2(std::round((a.x + z.x - icon) / 2), std::round((a.y + z.y - icon) / 2)), icon, fg);
        return;
    }
    ImVec2 ls = font->CalcTextSizeA(text, FLT_MAX, 0, b.label);
    float gap = text * 0.55f, total = icon + gap + ls.x;
    float x = std::round((a.x + z.x - total) / 2), mid = (a.y + z.y) / 2;
    icons::draw(dl, b.icon, ImVec2(x, std::round(mid - icon / 2)), icon, fg);
    dl->AddText(font, text, ImVec2(x + icon + gap, std::round(mid - ls.y / 2)), fg, b.label);
}

bool draw(ImDrawList* dl, ImVec2 d, float s, ImVec2 mouse, bool clicked) {
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(12, 12), IM_COL32(beside[0], beside[1], beside[2], 255));

    if (vanilla) {
        // only the way back, in the toggle's spot
        static const Button back{"OneClient menu", "hud", 0.012f, 0.035f, 0.11f, 0.052f, false};
        draw_button(dl, back, d, s, mouse);
        return false;
    }
    // the wordmark over the buttons, OneClient's semibold in white
    ImFont* brand = ui::fonts.s48;
    float size = d.y * 0.075f;
    ImVec2 ts = brand->CalcTextSizeA(size, FLT_MAX, 0, CLIENT_NAME_UPPER);
    dl->AddText(brand, size, ImVec2(std::round((d.x - ts.x) / 2) + 2, std::round(d.y * 0.37f) + 3), IM_COL32(0, 0, 0, 90), CLIENT_NAME_UPPER);
    dl->AddText(brand, size, ImVec2(std::round((d.x - ts.x) / 2), std::round(d.y * 0.37f)), IM_COL32(255, 255, 255, 255), CLIENT_NAME_UPPER);

    bool open_menu = false;
    for (const Button& b : buttons) {
        draw_button(dl, b, d, s, mouse);
        if (b.ours && inside(b, mouse, d) && clicked)
            open_menu = true;
    }
    return open_menu;
}

}  // namespace title
