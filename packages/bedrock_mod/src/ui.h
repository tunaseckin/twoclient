#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"
#include "settings.h"

// Drawing helpers and controls shared by the shell and its pages, after
// OneConfig's Compose components. Sizes are dp, colors are ARGB.
namespace ui {

// --- theme ------------------------------------------------------------------

// OneConfig's UITheme colors (themes/Impls.kt)
struct Theme {
    const char* family;
    bool light;
    uint32_t page, sidebar, chip, mod_card, component, popup, border, text, text_secondary, accent_text, thumb, favorite;
    // PolyGlass: no glow, a frosted inset footer on cards and a translucent selection
    bool glass;
};

extern const Theme themes[];
extern const int theme_count;
const Theme& theme();
uint32_t accent();
// pageBackground and sidebarBackground at the Page and Sidebar Opacity preferences
uint32_t page_color();
uint32_t sidebar_color();
constexpr uint32_t CONTROL_TRACK = 0xFF74777F;

// Radii.kt
constexpr float RADIUS_XS = 6, RADIUS_SM = 8, RADIUS_MD = 16, RADIUS_LG = 20;

// --- frame state ------------------------------------------------------------

extern float S;
extern ImVec2 O;
extern ImDrawList* dl;

inline float dp(float v) { return v * S; }
inline ImVec2 at(float x, float y) { return ImVec2(O.x + x * S, O.y + y * S); }

// Starts a frame: drains the keys typed since the last one
void begin_frame();
// Ends it, telling input whether the menu wants the keyboard to itself
void end_frame();

// Called from input with every key pressed while the menu is open
void push_key(int key, bool shift);
bool wants_keyboard();

// --- colors -----------------------------------------------------------------

ImU32 col(uint32_t argb, float alpha = 1);
uint32_t mix(uint32_t a, uint32_t b, float t);
uint32_t over(uint32_t top, uint32_t bottom);
uint32_t with_alpha(uint32_t argb, float alpha);

// --- fonts and text ---------------------------------------------------------

struct Fonts {
    ImFont *r11, *r12, *r13, *r14, *r16, *m11, *m15, *m16, *m22, *m24, *brand, *s18, *s24, *s48;
};
extern Fonts fonts;
void load_fonts(float scale);

ImVec2 text_size(ImFont* font, const char* text);
void text(ImFont* font, ImVec2 pos, ImU32 color, const char* str);
void text_centered(ImFont* font, ImVec2 center, ImU32 color, const char* str);
// Compose's Text box is a line tall, 1.5em for Poppins, in pixels
inline float line(ImFont* font) { return font->FontSize * 1.5f; }
// Draws str left-aligned and vertically centered in a box of height h starting at pos
void text_in(ImFont* font, ImVec2 pos, float h, ImU32 color, const char* str);

// Text wrapped to width; returns its height in pixels
float text_wrapped(ImFont* font, ImVec2 pos, float width, ImU32 color, const char* str);
float wrapped_height(ImFont* font, const char* str, float width);

// --- shapes -----------------------------------------------------------------

void fill(ImVec2 a, ImVec2 b, ImU32 color, float rounding, ImDrawFlags corners = 0);
// animatedFrame's border, one dp inside the shape
void frame(ImVec2 a, ImVec2 b, ImU32 color, float rounding, float width = 1);
float stops(std::initializer_list<std::pair<float, float>> s, float t);

template <class F>
void fade_vertices(int from, F f) {
    for (int i = from; i < dl->VtxBuffer.Size; i++) {
        ImDrawVert& v = dl->VtxBuffer[i];
        float a = float((v.col >> IM_COL32_A_SHIFT) & 255) * std::clamp(f(v.pos.y), 0.f, 1.f);
        v.col = (v.col & ~IM_COL32_A_MASK) | (ImU32(a) << IM_COL32_A_SHIFT);
    }
}

// A grid of vertices filling a rounded rectangle, colored per vertex, for the
// radial and linear gradients Compose draws and ImGui has no primitive for
template <class F>
void gradient(ImVec2 a, ImVec2 b, float r, int nx, int ny, F color_at) {
    int vx = nx + 1;
    dl->PrimReserve(nx * ny * 6, vx * (ny + 1));
    ImVec2 uv = dl->_Data->TexUvWhitePixel;
    ImDrawIdx base = ImDrawIdx(dl->_VtxCurrentIdx);
    for (int j = 0; j <= ny; j++) {
        for (int i = 0; i <= nx; i++) {
            float x = a.x + (b.x - a.x) * i / nx, y = a.y + (b.y - a.y) * j / ny;
            // pull corner vertices onto the rounded edge
            float cx = std::clamp(x, a.x + r, b.x - r), cy = std::clamp(y, a.y + r, b.y - r);
            float dx = x - cx, dy = y - cy, d = std::sqrt(dx * dx + dy * dy);
            if (d > r) {
                x = cx + dx * r / d;
                y = cy + dy * r / d;
            }
            dl->PrimWriteVtx(ImVec2(x, y), uv, color_at(x, y));
        }
    }
    for (int j = 0; j < ny; j++) {
        for (int i = 0; i < nx; i++) {
            ImDrawIdx p = ImDrawIdx(base + j * vx + i);
            dl->PrimWriteIdx(p);
            dl->PrimWriteIdx(ImDrawIdx(p + 1));
            dl->PrimWriteIdx(ImDrawIdx(p + vx + 1));
            dl->PrimWriteIdx(p);
            dl->PrimWriteIdx(ImDrawIdx(p + vx + 1));
            dl->PrimWriteIdx(ImDrawIdx(p + vx));
        }
    }
}

// A mod card or setting row's surface: modCardBackground, a vignette and a
// border fading out downwards
void card_surface(ImVec2 a, ImVec2 b);

// --- interaction ------------------------------------------------------------

// animateFloatAsState's default spring
float animate(const char* key, int index, float target);
bool hover(ImVec2 a, ImVec2 b);
// A press and release inside the box, which plays the click sound. Things
// drawn on top have to ask first.
bool click(ImVec2 a, ImVec2 b);
// Marks the frame's click as taken without a box, for popups closing
void consume_click();

// Scrolls a column of content of height content_h inside [a, b]; returns how
// far down it is, in pixels
float scroll_area(const char* id, ImVec2 a, ImVec2 b, float content_h);

// --- controls ---------------------------------------------------------------

// SwitchControl, its right edge at right and centered on mid
bool switch_control(const char* id, int index, float right, float mid, bool on);
// SliderControl with its value box; changes value while dragged
bool slider_control(const char* id, int index, float right, float mid, float& value, float min, float max, float step);
// These two read and write the config key directly
bool dropdown_control(const char* key, float right, float mid, const std::vector<const char*>& options);
bool keybind_control(const char* key, float right, float mid, bool conflict);

// A Chip, its left edge at x; returns its width in dp
float chip(const char* id, int index, float x, float y, const char* label, const char* icon, bool selected, bool* clicked);
// IconButton with hoverBorder
bool icon_button(const char* icon, ImVec2 a, bool enabled);
// A single-line text field taking keys while focused; returns true while focused
bool text_field(const char* id, std::string& value, ImVec2 a, ImVec2 b, const char* placeholder, const char* icon);
void blur_text_field();
void focus_text_field(std::string& value);
// Enter was pressed in a text field this frame
bool submitted();

// A setting row as ConfigScreen draws it, returns its height in dp
float setting_row(const Setting& setting, int index, float x, float y, float w);
float setting_row_height(const Setting& setting);
// A subcategory header, 22sp, returns its height in dp
float group_header(const char* title, float x, float y);

std::string key_name(int key);
// Lowercase contains, for search
bool matches(const char* text, const std::string& query);

// Popups draw last so they sit over the page; a control that opens one
// registers it here
void popups();

struct MenuItem {
    const char* icon;
    const char* label;
    bool danger = false;
    bool enabled = true;
};
// An actions menu opened at pos. The item picked comes back from
// take_menu_choice with the same id, once.
void open_menu(const std::string& id, ImVec2 pos, std::vector<MenuItem> items);
int take_menu_choice(const std::string& id);
bool menu_showing(const std::string& id);

}  // namespace ui
