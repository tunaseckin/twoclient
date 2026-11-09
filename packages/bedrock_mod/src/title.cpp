#include "title.h"

#include <EGL/egl.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "config.h"
#include "icons.h"
#include "quickplay.h"
#include "ui.h"

namespace title {
namespace {

enum Action { GAME, MENU, THEME };

// Fractions of the screen; ui/start_screen.json puts the game's buttons on the same spots
struct Button {
    const char* label;
    const char* icon;  // "creeper" is drawn by hand
    float x, y, w, h;
    Action action;  // GAME: the game's invisible button under it takes the click
    bool chevron = false;
};

const Button buttons[] = {
        {"Singleplayer", "right-arrow", 0.325f, 0.52f, 0.35f, 0.055f, GAME},
        {"Multiplayer", "profiles", 0.325f, 0.595f, 0.35f, 0.055f, GAME},
        {"Realms", "external-link", 0.325f, 0.67f, 0.35f, 0.055f, GAME},
        {"Settings", "settings", 0.325f, 0.745f, 0.17f, 0.055f, GAME},
        {"Mods", "qol", 0.505f, 0.745f, 0.17f, 0.055f, MENU},
        {"Profile", "profiles", 0.725f, 0.52f, 0.24f, 0.055f, GAME, true},
        {"Wardrobe", "paintbrush", 0.725f, 0.595f, 0.24f, 0.055f, GAME},
        // OneClient's top-right row: back to vanilla, theme, notifications, quit
        {nullptr, "creeper", 0.82f, 0.025f, 0.034f, 0.055f, GAME},
        {nullptr, "moon", 0.864f, 0.025f, 0.034f, 0.055f, THEME},
        {nullptr, "bell", 0.908f, 0.025f, 0.034f, 0.055f, GAME},
        {nullptr, "close", 0.952f, 0.025f, 0.034f, 0.055f, GAME},
};
const Button& close_button = buttons[10];

ImVec2 corner(const Button& b, ImVec2 d) { return ImVec2(std::round(b.x * d.x), std::round(b.y * d.y)); }
ImVec2 far(const Button& b, ImVec2 d) { return ImVec2(std::round((b.x + b.w) * d.x), std::round((b.y + b.h) * d.y)); }

// the screen's color next to the mark, to paint over it
unsigned char beside[4] = {}, beside_middle[4] = {};
// the game's own title screen is up, behind the pack's toggle
bool vanilla = false;

bool inside(const Button& b, ImVec2 p, ImVec2 d) {
    ImVec2 a = corner(b, d), z = far(b, d);
    return p.x >= a.x && p.x < z.x && p.y >= a.y && p.y < z.y;
}

// Minecraft's creeper face, 8x8, for the vanilla button
void creeper(ImDrawList* dl, ImVec2 a, float px, ImU32 c) {
    const char* rows[] = {"........", ".##..##.", ".##..##.", "...##...", "..####..", "..####..", "..#..#..", "........"};
    float u = px / 8;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            if (rows[y][x] == '#')
                dl->AddRectFilled(ImVec2(a.x + x * u, a.y + y * u), ImVec2(a.x + (x + 1) * u, a.y + (y + 1) * u), c);
}

}  // namespace

bool light() { return config::on("pref.title_light"); }

Look look(bool hover) {
    if (light())
        return {hover ? IM_COL32(235, 238, 248, 190) : IM_COL32(205, 210, 225, 150), hover ? IM_COL32(255, 255, 255, 240) : IM_COL32(255, 255, 255, 190),
                IM_COL32(28, 32, 48, 255), IM_COL32(28, 32, 48, 140)};
    return {hover ? IM_COL32(70, 72, 92, 150) : IM_COL32(24, 22, 34, 120), hover ? IM_COL32(235, 238, 255, 190) : IM_COL32(225, 228, 255, 120),
            IM_COL32(222, 225, 255, 255), IM_COL32(222, 225, 255, 120)};
}

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
    // the mark is the pack's 2px square in the top-left corner: 1, 254, 2 on our
    // screen, 2, 1, 254 on the vanilla one
    read(vp[0] + 1, vp[1] + vp[3] - 2, 1, 1, 0x1908, 0x1401, px);
    read(vp[0] + 14, vp[1] + vp[3] - 2, 1, 1, 0x1908, 0x1401, beside);
    vanilla = px[0] < 12 && px[1] < 12 && px[2] > 240;
    if (vanilla)
        return true;
    if (!(px[0] < 12 && px[1] > 240 && px[2] < 14))
        return false;
    // the second mark sits mid-screen, where a dialog like the quit prompt covers it
    int mx = vp[0] + vp[2] / 2 + 1, my = vp[1] + int(vp[3] * 0.44f) - 2;
    read(mx, my, 1, 1, 0x1908, 0x1401, px);
    read(mx + 14, my, 1, 1, 0x1908, 0x1401, beside_middle);
    return px[0] < 12 && px[1] > 240 && px[2] < 14;
}

bool owns(ImVec2 point, ImVec2 display) {
    if (vanilla)
        return false;
    for (const Button& b : buttons)
        if (b.action != GAME && inside(b, point, display))
            return true;
    return quickplay::owns(point, display);
}

void draw_button(ImDrawList* dl, const Button& b, ImVec2 d, float s, ImVec2 mouse) {
    ImFont* font = ui::fonts.m16;
    float text = d.y * 0.026f;
    ImVec2 a = corner(b, d), z = far(b, d);
    bool hover = inside(b, mouse, d);
    Look l = look(hover);
    float r = 5 * s;
    ImU32 fill = l.fill, fg = l.text;
    if (&b == &close_button)
        fill = hover ? IM_COL32(225, 95, 105, 230) : IM_COL32(205, 85, 95, 200), fg = IM_COL32(255, 235, 240, 255);
    else if (!b.label)
        fill = light() ? (hover ? IM_COL32(110, 116, 138, 230) : IM_COL32(90, 96, 118, 210)) : fill, fg = light() ? IM_COL32(230, 232, 255, 255) : fg;
    dl->AddRectFilled(a, z, fill, r);
    dl->AddRect(a, z, l.border, r, 0, std::max(1.f, s));
    float icon = text * 1.1f;
    if (!b.label) {
        ImVec2 at(std::round((a.x + z.x - icon) / 2), std::round((a.y + z.y - icon) / 2));
        const char* name = b.action == THEME ? (light() ? "sun" : "moon") : b.icon;
        if (!std::strcmp(name, "creeper"))
            creeper(dl, at, icon, fg);
        else
            icons::draw(dl, name, at, icon, fg);
        return;
    }
    ImVec2 ls = font->CalcTextSizeA(text, FLT_MAX, 0, b.label);
    float gap = text * 0.55f, total = icon + gap + ls.x;
    float x = std::round((a.x + z.x - total) / 2), mid = (a.y + z.y) / 2;
    icons::draw(dl, b.icon, ImVec2(x, std::round(mid - icon / 2)), icon, fg);
    dl->AddText(font, text, ImVec2(x + icon + gap, std::round(mid - ls.y / 2)), fg, b.label);
    if (b.chevron)
        icons::draw(dl, "down", ImVec2(z.x - text * 1.8f, std::round(mid - icon * 0.4f)), icon * 0.8f, fg);
}

bool draw(ImDrawList* dl, ImVec2 d, float s, ImVec2 mouse, bool clicked) {
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(12, 12), IM_COL32(beside[0], beside[1], beside[2], 255));
    if (vanilla) {
        // Java's way back: a stone button with the logo, in the corner
        static const Button back{nullptr, "logo", 0.952f, 0.025f, 0.034f, 0.055f, GAME};
        ImVec2 a = corner(back, d), z = far(back, d);
        bool hover = inside(back, mouse, d);
        float px = std::max(1.f, std::round(s * 1.5f));
        dl->AddRectFilled(a, z, IM_COL32(0, 0, 0, 255));
        dl->AddRectFilled(ImVec2(a.x + px, a.y + px), ImVec2(z.x - px, z.y - px), hover ? IM_COL32(128, 132, 160, 255) : IM_COL32(111, 111, 111, 255));
        dl->AddRectFilled(ImVec2(a.x + px, a.y + px), ImVec2(z.x - px, a.y + 2 * px), IM_COL32(170, 170, 170, 255));
        dl->AddRectFilled(ImVec2(a.x + px, z.y - 3 * px), ImVec2(z.x - px, z.y - px), IM_COL32(70, 70, 70, 255));
        float icon = (z.y - a.y) * 0.55f;
        icons::draw(dl, "logo", ImVec2(std::round((a.x + z.x - icon) / 2), std::round((a.y + z.y - icon) / 2)), icon, IM_COL32_WHITE);
        return false;
    }
    float mx = std::round(d.x / 2), my = std::round(d.y * 0.56f);
    dl->AddRectFilled(ImVec2(mx - 2, my - 2), ImVec2(mx + 12, my + 12), IM_COL32(beside_middle[0], beside_middle[1], beside_middle[2], 255));
    // the wordmark over the buttons, OneClient's semibold in white
    float logo = d.y * 0.09f;
    icons::draw(dl, "logo", ImVec2(std::round((d.x - logo) / 2), std::round(d.y * 0.26f)), logo, IM_COL32_WHITE);
    ImFont* brand = ui::fonts.s48;
    float size = d.y * 0.075f;
    ImVec2 ts = brand->CalcTextSizeA(size, FLT_MAX, 0, CLIENT_NAME_UPPER);
    dl->AddText(brand, size, ImVec2(std::round((d.x - ts.x) / 2) + 2, std::round(d.y * 0.37f) + 3), IM_COL32(0, 0, 0, 90), CLIENT_NAME_UPPER);
    dl->AddText(brand, size, ImVec2(std::round((d.x - ts.x) / 2), std::round(d.y * 0.37f)), IM_COL32(255, 255, 255, 255), CLIENT_NAME_UPPER);

    quickplay::draw_skin(dl, ImVec2(std::round(0.845f * d.x), std::round(0.5f * d.y)), d.y * 0.36f, float(ImGui::GetTime()));
    quickplay::draw_list(dl, d, s, mouse, clicked);
    bool open_menu = false;
    for (const Button& b : buttons) {
        draw_button(dl, b, d, s, mouse);
        if (!clicked || !inside(b, mouse, d))
            continue;
        if (b.action == MENU)
            open_menu = true;
        else if (b.action == THEME) {
            config::set("pref.title_light", light() ? 0 : 1);
            config::save();
        }
    }
    return open_menu;
}

}  // namespace title
