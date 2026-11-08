#include "ui.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <mutex>
#include <unordered_map>

#include "assets.h"
#include "config.h"
#include "icons.h"
#include "imgui_impl_opengl3.h"
#include "sound.h"

EMBED(poppins_regular, FONT_DIR "/Poppins-Regular.ttf")
EMBED(poppins_medium, FONT_DIR "/Poppins-Medium.ttf")
EMBED(poppins_semibold, FONT_DIR "/Poppins-SemiBold.ttf")

void declare(const std::vector<Setting>& settings) {
    for (const Setting& s : settings)
        config::declare(s.key, s.def);
}

namespace ui {

const Theme themes[] = {
        {"Polyfrosted", false, 0xC411171C, 0xB3151C22, 0xB2232D32, 0x59232D32, 0xFF1A2229, 0xFF1A2229, 0x1AFFFFFF, 0xFFD5DBFF,
         0xFF757883, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFD700, false},
        {"Polyfrosted", true, 0xB0D0D7F3, 0xB2D0D7F3, 0x59EBF2FF, 0x59EBF2FF, 0xB2EBF2FF, 0xFFD0D7F3, 0x4DA3A3A3, 0xFF000000,
         0xFF4E5661, 0xFFD5DBFF, 0xFF1A2229, 0xFFB57D00, false},
        {"PolyGlass", false, 0x8C16181D, 0x99121419, 0x7322252C, 0x6620232A, 0x7322252C, 0xCC16181D, 0x14FFFFFF, 0xFFFFFFFF,
         0xB3FFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFD60A, true},
        {"PolyGlass", true, 0xA6FAFBFF, 0xB8F2F4F9, 0x66FFFFFF, 0x57FFFFFF, 0x66FFFFFF, 0xD1F6F8FC, 0x8CFFFFFF, 0xFF0E1118,
         0x9E0E1118, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFC98A00, true},
};
const int theme_count = IM_ARRAYSIZE(themes);

const Theme& theme() { return themes[std::clamp(int(config::get("theme.index")), 0, theme_count - 1)]; }
uint32_t accent() { return uint32_t(config::get("theme.accent")) | 0xFF000000; }

namespace {

// Structs.kt's surfaceOpacity: PolyGlass scales its own alpha, the others replace it
uint32_t surface(uint32_t color, float percent, float def) {
    float alpha = float(color >> 24) / 255;
    float out;
    if (theme().glass)
        out = percent <= def ? alpha * percent / def : alpha + (1 - alpha) * (percent - def) / (100 - def);
    else
        out = percent / 100;
    return with_alpha(color | 0xFF000000, std::clamp(out, 0.f, 1.f));
}

}  // namespace

uint32_t page_color() { return surface(theme().page, config::get("pref.page_opacity"), 88); }
uint32_t sidebar_color() { return surface(theme().sidebar, config::get("pref.sidebar_opacity"), 80); }

float S = 1;
ImVec2 O;
ImDrawList* dl;
Fonts fonts;

namespace {

bool consumed;
// keys typed since the last frame, queued from the input thread
std::mutex keys_lock;
std::vector<std::pair<int, bool>> queued, frame_keys;
std::atomic<bool> keyboard_wanted{false};

std::string* focused = nullptr;
bool submitted_now = false;
std::string focused_id;
std::string recording;  // the config key of a keybind waiting for its key

// the open dropdown, drawn over everything by popups()
struct Popup {
    std::string key;
    ImVec2 a, b;
    std::vector<const char*> options;
    int opened_frame;
} popup;
bool popup_open = false, in_popup = false;

// an actions menu, like ProfileActionsMenu
struct Menu {
    std::string id;
    ImVec2 pos;
    std::vector<MenuItem> items;
    int opened_frame;
} menu;
bool menu_open = false;
std::string menu_chosen_id;
int menu_chosen = -1;
ImVec2 popup_a, popup_b;  // last frame's popup box, which blocks clicks under it

struct Spring {
    float x, v;
};
std::unordered_map<ImGuiID, Spring> springs;
std::unordered_map<ImGuiID, float> scrolls;

const char* active_slider = nullptr;
int active_slider_index = -1;

}  // namespace

void begin_frame() {
    consumed = false;
    submitted_now = false;
    std::lock_guard lock(keys_lock);
    frame_keys.swap(queued);
    queued.clear();
}

void end_frame() { keyboard_wanted = focused || !recording.empty(); }

void push_key(int key, bool shift) {
    std::lock_guard lock(keys_lock);
    queued.emplace_back(key, shift);
}

bool wants_keyboard() { return keyboard_wanted; }

// --- colors -----------------------------------------------------------------

ImU32 col(uint32_t argb, float alpha) {
    float a = float(argb >> 24) * alpha;
    return IM_COL32((argb >> 16) & 255, (argb >> 8) & 255, argb & 255, int(std::clamp(a, 0.f, 255.f)));
}

uint32_t mix(uint32_t a, uint32_t b, float t) {
    uint32_t out = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        float x = float((a >> shift) & 255), y = float((b >> shift) & 255);
        out |= uint32_t(std::clamp(std::lround(x + (y - x) * t), 0l, 255l)) << shift;
    }
    return out;
}

uint32_t over(uint32_t top, uint32_t bottom) {
    float ta = float(top >> 24) / 255, ba = float(bottom >> 24) / 255;
    float a = ta + ba * (1 - ta);
    uint32_t out = uint32_t(std::lround(a * 255)) << 24;
    for (int shift = 0; shift < 24; shift += 8) {
        float t = float((top >> shift) & 255), b = float((bottom >> shift) & 255);
        out |= uint32_t(std::lround((t * ta + b * ba * (1 - ta)) / std::max(a, 1e-6f))) << shift;
    }
    return out;
}

uint32_t with_alpha(uint32_t argb, float alpha) {
    return (argb & 0xFFFFFF) | (uint32_t(std::lround(float(argb >> 24) * alpha)) << 24);
}

// --- fonts ------------------------------------------------------------------

void load_fonts(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    ImFontConfig config;
    config.FontDataOwnedByAtlas = false;
    auto add = [&](const unsigned char* begin, const unsigned char* end, float sp) {
        return io.Fonts->AddFontFromMemoryTTF((void*)begin, int(end - begin), sp * scale, &config);
    };
    fonts.r14 = add(poppins_regular, poppins_regular_end, 14);
    fonts.r11 = add(poppins_regular, poppins_regular_end, 11);
    fonts.r12 = add(poppins_regular, poppins_regular_end, 12);
    fonts.r13 = add(poppins_regular, poppins_regular_end, 13);
    fonts.r16 = add(poppins_regular, poppins_regular_end, 16);
    fonts.m11 = add(poppins_medium, poppins_medium_end, 11);
    fonts.m15 = add(poppins_medium, poppins_medium_end, 15);
    fonts.m16 = add(poppins_medium, poppins_medium_end, 16);
    fonts.m22 = add(poppins_medium, poppins_medium_end, 22);
    fonts.m24 = add(poppins_medium, poppins_medium_end, 24);
    fonts.brand = add(poppins_semibold, poppins_semibold_end, 24);
    fonts.s18 = add(poppins_semibold, poppins_semibold_end, 18);
    fonts.s24 = add(poppins_semibold, poppins_semibold_end, 24);
    fonts.s48 = add(poppins_semibold, poppins_semibold_end, 48);
    ImGui_ImplOpenGL3_DestroyFontsTexture();
    ImGui_ImplOpenGL3_CreateFontsTexture();
}

ImVec2 text_size(ImFont* font, const char* text) { return font->CalcTextSizeA(font->FontSize, FLT_MAX, 0, text); }

void text(ImFont* font, ImVec2 pos, ImU32 color, const char* str) {
    dl->AddText(font, font->FontSize, ImVec2(std::round(pos.x), std::round(pos.y)), color, str);
}

void text_centered(ImFont* font, ImVec2 center, ImU32 color, const char* str) {
    ImVec2 size = text_size(font, str);
    text(font, ImVec2(center.x - size.x / 2, center.y - size.y / 2), color, str);
}

void text_in(ImFont* font, ImVec2 pos, float h, ImU32 color, const char* str) {
    text(font, ImVec2(pos.x, pos.y + (h - font->FontSize) / 2), color, str);
}

float wrapped_height(ImFont* font, const char* str, float width) {
    return font->CalcTextSizeA(font->FontSize, FLT_MAX, width, str).y;
}

float text_wrapped(ImFont* font, ImVec2 pos, float width, ImU32 color, const char* str) {
    dl->AddText(font, font->FontSize, ImVec2(std::round(pos.x), std::round(pos.y)), color, str, nullptr, width);
    return wrapped_height(font, str, width);
}

// --- shapes -----------------------------------------------------------------

void fill(ImVec2 a, ImVec2 b, ImU32 color, float rounding, ImDrawFlags corners) {
    if ((color >> IM_COL32_A_SHIFT) & 255)
        dl->AddRectFilled(a, b, color, rounding, corners);
}

void frame(ImVec2 a, ImVec2 b, ImU32 color, float rounding, float width) {
    if (!((color >> IM_COL32_A_SHIFT) & 255))
        return;
    float h = dp(width) / 2;
    dl->AddRect(ImVec2(a.x + h, a.y + h), ImVec2(b.x - h, b.y - h), color, std::max(0.f, rounding - h), 0, dp(width));
}

float stops(std::initializer_list<std::pair<float, float>> s, float t) {
    auto prev = *s.begin();
    if (t <= prev.first)
        return prev.second;
    for (auto& next : s) {
        if (t <= next.first)
            return prev.second + (next.second - prev.second) * (t - prev.first) / std::max(next.first - prev.first, 1e-6f);
        prev = next;
    }
    return prev.second;
}

void card_surface(ImVec2 a, ImVec2 b) {
    const Theme& t = theme();
    float r = dp(RADIUS_MD);
    fill(a, b, col(t.mod_card), r);
    if (!t.glass) {
        float cx = (a.x + b.x) / 2, cy = (a.y + b.y) / 2, radius = std::min(b.x - a.x, b.y - a.y) * 0.9f;
        uint32_t text = t.text;
        gradient(a, b, r, 16, 8, [&](float x, float y) {
            float d = std::hypot(x - cx, y - cy) / radius;
            return col(text, stops({{0, 0}, {.5f, .04f}, {1, .08f}}, d));
        });
    }
    int from = dl->VtxBuffer.Size;
    frame(a, b, col(t.border), r);
    fade_vertices(from, [&](float y) { return 1 - (y - a.y) / (b.y - a.y); });
}

// --- interaction ------------------------------------------------------------

float animate(const char* key, int index, float target) {
    ImGuiID id = ImHashStr(key) ^ (ImGuiID(index) * 2654435761u);
    auto [it, fresh] = springs.try_emplace(id, Spring{target, 0});
    Spring& s = it->second;
    if (!config::on("theme.animations")) {
        s = {target, 0};
        return target;
    }
    constexpr float k = 1500, damping = 2 * 38.729833f;
    float dt = std::min(ImGui::GetIO().DeltaTime, 0.1f);
    for (float left = dt; left > 0; left -= 1 / 240.f) {
        float step = std::min(left, 1 / 240.f);
        s.v += (-k * (s.x - target) - damping * s.v) * step;
        s.x += s.v * step;
    }
    return s.x;
}

bool hover(ImVec2 a, ImVec2 b) {
    if (!in_popup && (popup_open || menu_open) && ImGui::IsMouseHoveringRect(popup_a, popup_b, false))
        return false;
    return ImGui::IsMouseHoveringRect(a, b, false);
}

bool click(ImVec2 a, ImVec2 b) {
    if (consumed || !ImGui::IsMouseReleased(0) || !hover(a, b))
        return false;
    ImVec2 p = ImGui::GetIO().MouseClickedPos[0];
    if (p.x < a.x || p.y < a.y || p.x >= b.x || p.y >= b.y)
        return false;
    consumed = true;
    sound::play(sound::Effect::Click);
    return true;
}

void consume_click() { consumed = true; }

float scroll_area(const char* id, ImVec2 a, ImVec2 b, float content_h) {
    ImGuiID key = ImHashStr(id);
    float& target = scrolls[key];
    float view = b.y - a.y;
    float max = std::max(0.f, content_h - view);
    if (hover(a, b))
        target -= ImGui::GetIO().MouseWheel * dp(60);

    // ScrollbarBox: an 8dp thumb centered in the page gutter, at least 24dp tall
    if (max > 0) {
        float gutter = dp(25), thick = dp(8);
        float x = b.x + (gutter - thick) / 2;
        float thumb_h = std::max(dp(24), view * view / content_h);
        float travel = view - thumb_h;
        float shown = animate("scroll", int(key), std::clamp(target, 0.f, max));
        float ty = a.y + travel * (shown / max);
        ImVec2 ta(x, ty), tb(x + thick, ty + thumb_h);
        ImGuiIO& io = ImGui::GetIO();
        static ImGuiID dragging = 0;
        static float grab = 0;
        if (!dragging && !consumed && ImGui::IsMouseClicked(0) && hover(ImVec2(x, a.y), ImVec2(x + thick, b.y))) {
            dragging = key;
            consumed = true;
            bool on_thumb = io.MousePos.y >= ta.y && io.MousePos.y < tb.y;
            grab = on_thumb ? io.MousePos.y - ty : thumb_h / 2;
        }
        if (dragging == key) {
            target = std::clamp((io.MousePos.y - grab - a.y) / std::max(travel, 1.f), 0.f, 1.f) * max;
            if (!io.MouseDown[0])
                dragging = 0;
        }
        float h = animate("scrollbar", int(key), dragging == key || hover(ta, tb) ? 1.f : 0.f);
        fill(ta, tb, col(theme().text_secondary, 0.4f + 0.3f * h), dp(RADIUS_SM));
    }
    target = std::clamp(target, 0.f, max);
    return animate("scroll", int(key), target);
}

// --- controls ---------------------------------------------------------------

bool switch_control(const char* id, int index, float right, float mid, bool on) {
    const Theme& t = theme();
    ImVec2 a(right - dp(42), mid - dp(10.5f)), b(right, mid + dp(10.5f));
    bool hovered = hover(a, b);
    float on_a = animate(id, index * 3, on ? 1.f : 0.f);
    float thumb = animate(id, index * 3 + 1, on ? 24.f : 3.f);
    float edge = animate(id, index * 3 + 2, hovered ? 1.f : 0.f);
    float r = dp(10.5f);
    fill(a, b, col(CONTROL_TRACK), r);
    fill(a, b, col(accent(), on_a), r);
    frame(a, b, col(t.text, 0.15f * edge), r);
    dl->AddCircleFilled(ImVec2(a.x + dp(thumb + 7.5f), mid), dp(7.5f), col(t.thumb), 24);
    const char* label = on ? "On" : "Off";
    ImVec2 size = text_size(fonts.r12, label);
    text(fonts.r12, ImVec2(a.x - dp(8) - size.x, mid - fonts.r12->FontSize / 2), col(t.text_secondary), label);
    return click(a, b);
}

namespace {

std::string format_number(float v, float step) {
    char buf[32];
    if (step >= 1 || step == 0)
        std::snprintf(buf, sizeof buf, "%d", int(std::lround(v)));
    else if (step >= 0.1f)
        std::snprintf(buf, sizeof buf, "%.1f", v);
    else
        std::snprintf(buf, sizeof buf, "%.2f", v);
    return buf;
}

// The rounded box OneConfig's fields and dropdowns sit in
void field_box(ImVec2 a, ImVec2 b, bool hovered) {
    const Theme& t = theme();
    fill(a, b, col(t.chip), dp(RADIUS_SM));
    frame(a, b, col(hovered ? t.text_secondary : t.border), dp(RADIUS_SM));
}

}  // namespace

bool slider_control(const char* id, int index, float right, float mid, float& value, float min, float max, float step) {
    const Theme& t = theme();
    // the value box, then the 200dp track to its left
    ImVec2 va(right - dp(80), mid - dp(16)), vb(right, mid + dp(16));
    field_box(va, vb, false);
    text_centered(fonts.r14, ImVec2((va.x + vb.x) / 2, mid), col(t.text), format_number(value, step).c_str());

    float thumb = dp(19), track_h = dp(5);
    ImVec2 a(va.x - dp(16) - dp(200), mid - thumb / 2), b(va.x - dp(16), mid + thumb / 2);
    bool changed = false;
    bool active = active_slider == id && active_slider_index == index;
    ImGuiIO& io = ImGui::GetIO();
    if (!active && !consumed && ImGui::IsMouseClicked(0) && hover(a, b)) {
        active_slider = id;
        active_slider_index = index;
        active = true;
        consumed = true;
    }
    if (active) {
        float usable = std::max(1.f, b.x - a.x - thumb);
        float f = std::clamp((io.MousePos.x - a.x - thumb / 2) / usable, 0.f, 1.f);
        float v = min + f * (max - min);
        if (step > 0)
            v = std::round(v / step) * step;
        v = std::clamp(v, min, max);
        if (v != value) {
            value = v;
            changed = true;
        }
        if (!io.MouseDown[0]) {
            active_slider = nullptr;
            active_slider_index = -1;
        }
    }
    float fraction = animate(id, index, (value - min) / std::max(max - min, 1e-6f));
    float ty = mid - track_h / 2;
    fill(ImVec2(a.x, ty), ImVec2(b.x, ty + track_h), col(CONTROL_TRACK), dp(RADIUS_XS));
    fill(ImVec2(a.x, ty), ImVec2(a.x + (b.x - a.x) * fraction, ty + track_h), col(accent()), dp(RADIUS_XS));
    float tx = a.x + fraction * (b.x - a.x - thumb);
    dl->AddCircleFilled(ImVec2(tx + thumb / 2, mid), thumb / 2, col(t.thumb), 24);
    return changed;
}

bool dropdown_control(const char* key, float right, float mid, const std::vector<const char*>& options) {
    const Theme& t = theme();
    ImVec2 a(right - dp(200), mid - dp(16)), b(right, mid + dp(16));
    bool hovered = hover(a, b);
    field_box(a, b, hovered);
    int value = int(config::get(key));
    const char* shown = value >= 0 && value < int(options.size()) ? options[value] : "";
    text_in(fonts.r14, ImVec2(a.x + dp(12), a.y), dp(32), col(t.text), shown);
    icons::draw(dl, "down", ImVec2(b.x - dp(12) - dp(14), mid - dp(7)), dp(14), col(t.text_secondary));
    if (click(a, b)) {
        bool same = popup_open && popup.key == key;
        popup_open = !same;
        if (!same)
            popup = {key, ImVec2(a.x, b.y + dp(6)), ImVec2(b.x, b.y + dp(6) + dp(32) * float(options.size()) + dp(8)), options,
                     ImGui::GetFrameCount()};
    }
    return false;
}

bool keybind_control(const char* key, float right, float mid, bool conflict) {
    const Theme& t = theme();
    bool listening = recording == key;
    std::string label = listening ? "Press keys..." : key_name(int(config::get(key)));
    // KeybindOption: a keyboard icon and the key, padded 12 by 7
    float w = dp(12 + 16 + 8 + 12) + text_size(fonts.r13, label.c_str()).x;
    float h = dp(7 * 2) + line(fonts.r13);
    ImVec2 a(right - w, mid - h / 2), b(right, mid + h / 2);
    bool hovered = hover(a, b);
    std::string id = std::string("keybind:") + key;
    float rec = animate(id.c_str(), 0, listening ? 1.f : 0.f);
    float hov = animate(id.c_str(), 1, hovered ? 1.f : 0.f);
    uint32_t bg = mix(mix(t.component, with_alpha(t.component, 0.8f), hov), with_alpha(accent(), 0.2f), rec);
    uint32_t idle_border = conflict ? 0xFFE0524F : mix(t.border, with_alpha(t.text, 0.2f), hov);
    uint32_t idle_text = conflict ? 0xFFE0524F : t.text;
    fill(a, b, col(bg), dp(RADIUS_SM));
    frame(a, b, col(mix(idle_border, accent(), rec)), dp(RADIUS_SM));
    uint32_t fg = mix(idle_text, accent(), rec);
    icons::draw(dl, "keyboard", ImVec2(a.x + dp(12), mid - dp(8)), dp(16), col(fg));
    text_in(fonts.r13, ImVec2(a.x + dp(12 + 16 + 8), a.y), h, col(fg), label.c_str());
    if (click(a, b)) {
        recording = listening ? "" : key;
        blur_text_field();
        return false;
    }
    if (!listening)
        return false;
    for (auto [k, shift] : frame_keys) {
        // Escape leaves the key as it was
        if (k == 27) {
            recording.clear();
            return false;
        }
        config::set(key, float(k));
        recording.clear();
        return true;
    }
    return false;
}

float chip(const char* id, int index, float x, float y, const char* label, const char* icon, bool selected, bool* clicked) {
    const Theme& t = theme();
    float icon_w = icon ? 18 + 10 : 0;
    float w = 12 + icon_w + text_size(fonts.r14, label).x / S + 12;
    ImVec2 a = at(x, y), b = at(x + w, y + 33);
    bool hovered = hover(a, b);
    float h = animate(id, index * 2, hovered ? 1.f : 0.f);
    float sel = animate(id, index * 2 + 1, selected ? 1.f : 0.f);
    uint32_t bg = mix(t.chip, over(with_alpha(accent(), 0.5f), t.chip), h);
    uint32_t fg = mix(t.text, t.accent_text, sel);
    fill(a, b, col(bg), dp(RADIUS_SM));
    fill(a, b, col(accent(), sel), dp(RADIUS_SM));
    frame(a, b, col(t.border), dp(RADIUS_SM));
    float cx = a.x + dp(12);
    if (icon) {
        icons::draw(dl, icon, ImVec2(cx, a.y + dp(7.5f)), dp(18), col(fg));
        cx += dp(18 + 10);
    }
    text_in(fonts.r14, ImVec2(cx, a.y), dp(33), col(fg), label);
    *clicked = click(a, b);
    return w;
}

bool icon_button(const char* icon, ImVec2 a, bool enabled) {
    const Theme& t = theme();
    ImVec2 b(a.x + dp(32), a.y + dp(32));
    bool hovered = enabled && hover(a, b);
    float h = animate("icon_button", int(ImHashStr(icon)), hovered ? 1.f : 0.f);
    uint32_t color = enabled ? mix(with_alpha(t.text, 0.7f), t.text, h) : with_alpha(t.text_secondary, 0.35f);
    frame(a, b, col(t.border, h), dp(RADIUS_SM));
    icons::draw(dl, icon, ImVec2(a.x + dp(4), a.y + dp(4)), dp(24), col(color));
    return enabled && click(a, b);
}

namespace {

// What a key code types, US layout
char typed(int key, bool shift) {
    if (key >= 'A' && key <= 'Z')
        return shift ? char(key) : char(std::tolower(key));
    if (key >= '0' && key <= '9' && !shift)
        return char(key);
    switch (key) {
    case ' ': return ' ';
    case 189: return shift ? '_' : '-';
    case 190: return '.';
    case 222: return '\'';
    default: return 0;
    }
}

}  // namespace

bool text_field(const char* id, std::string& value, ImVec2 a, ImVec2 b, const char* placeholder, const char* icon) {
    const Theme& t = theme();
    bool is_focused = focused == &value;
    if (click(a, b) && !is_focused) {
        focused = &value;
        focused_id = id;
        recording.clear();
        is_focused = true;
    }
    if (is_focused) {
        for (auto [k, shift] : frame_keys) {
            if (k == 8 && !value.empty())
                value.pop_back();
            else if (k == 13 || k == 27) {
                submitted_now = k == 13;
                focused = nullptr;
            }
            else if (char c = typed(k, shift); c && value.size() < 64)
                value += c;
        }
        is_focused = focused == &value;
    }
    fill(a, b, col(t.chip), dp(RADIUS_SM));
    frame(a, b, col(is_focused ? t.text_secondary : t.border), dp(RADIUS_SM));
    float x = a.x + dp(10);
    if (icon) {
        icons::draw(dl, icon, ImVec2(x, (a.y + b.y) / 2 - dp(7)), dp(14), col(t.text_secondary));
        x += dp(14 + 10);
    }
    float h = b.y - a.y;
    dl->PushClipRect(ImVec2(x, a.y), ImVec2(b.x - dp(8), b.y), true);
    if (value.empty() && !is_focused) {
        text_in(fonts.r12, ImVec2(x, a.y), h, col(t.text_secondary), placeholder);
    } else {
        text_in(fonts.r12, ImVec2(x, a.y), h, col(t.text), value.c_str());
        // a blinking caret after the text
        if (is_focused && std::fmod(ImGui::GetTime(), 1.0) < 0.5) {
            float cx = x + text_size(fonts.r12, value.c_str()).x + dp(1);
            dl->AddLine(ImVec2(cx, a.y + h * 0.25f), ImVec2(cx, b.y - h * 0.25f), col(t.text_secondary), dp(1));
        }
    }
    dl->PopClipRect();
    // a click anywhere else lets go of the field
    if (is_focused && ImGui::IsMouseClicked(0) && !ImGui::IsMouseHoveringRect(a, b, false))
        focused = nullptr;
    return focused == &value;
}

void blur_text_field() { focused = nullptr; }

void focus_text_field(std::string& value) {
    focused = &value;
    recording.clear();
}

bool submitted() { return submitted_now; }

float setting_row_height(const Setting& s) {
    float label = line(fonts.m15) / S + (s.description ? 2 + line(fonts.r13) / S : 0);
    return std::max(label, 32.f) + 24;
}

float setting_row(const Setting& s, int index, float x, float y, float w) {
    const Theme& t = theme();
    float h = setting_row_height(s);
    ImVec2 a = at(x, y), b = at(x + w, y + h);
    card_surface(a, b);

    // SettingLabel: the title over its description, 16dp in
    float label_h = line(fonts.m15) + (s.description ? dp(2) + line(fonts.r13) : 0);
    float ly = (a.y + b.y - label_h) / 2;
    text_in(fonts.m15, ImVec2(a.x + dp(16), ly), line(fonts.m15), col(t.text), s.title);
    if (s.description)
        text_in(fonts.r13, ImVec2(a.x + dp(16), ly + line(fonts.m15) + dp(2)), line(fonts.r13), col(t.text_secondary), s.description);

    float right = b.x - dp(16), mid = (a.y + b.y) / 2;
    switch (s.kind) {
    case Setting::SWITCH:
        if (switch_control(s.key, index, right, mid, config::on(s.key)))
            config::set(s.key, config::on(s.key) ? 0 : 1);
        break;
    case Setting::SLIDER: {
        float v = config::get(s.key);
        if (slider_control(s.key, index, right, mid, v, s.min, s.max, s.step))
            config::set(s.key, v);
        break;
    }
    case Setting::DROPDOWN:
        dropdown_control(s.key, right, mid, s.options);
        break;
    case Setting::KEYBIND:
        keybind_control(s.key, right, mid, false);
        break;
    }
    return h;
}

float group_header(const char* title, float x, float y) {
    float h = 8 + line(fonts.m22) / S + 2;
    text_in(fonts.m22, at(x, y + 8), line(fonts.m22), col(theme().text), title);
    return h;
}

std::string key_name(int key) {
    bool right = key & 256;
    int k = key & 255;
    std::string side = right ? "Right " : "";
    if ((k >= 'A' && k <= 'Z') || (k >= '0' && k <= '9'))
        return std::string(1, char(k));
    if (k >= 112 && k <= 123)
        return "F" + std::to_string(k - 111);
    switch (k) {
    case 0: return "None";
    case 8: return "Backspace";
    case 9: return "Tab";
    case 13: return "Enter";
    case 16: return side + "Shift";
    case 17: return side + "Control";
    case 18: return side + "Alt";
    case 20: return "Caps Lock";
    case 27: return "Escape";
    case 32: return "Space";
    case 37: return "Left";
    case 38: return "Up";
    case 39: return "Right";
    case 40: return "Down";
    case 189: return "-";
    case 190: return ".";
    default: return "Key " + std::to_string(key);
    }
}

bool matches(const char* text, const std::string& query) {
    if (query.empty())
        return true;
    std::string t = text, q = query;
    for (char& c : t)
        c = char(std::tolower((unsigned char)c));
    for (char& c : q)
        c = char(std::tolower((unsigned char)c));
    return t.find(q) != std::string::npos;
}

void open_menu(const std::string& id, ImVec2 pos, std::vector<MenuItem> items) {
    menu = {id, pos, std::move(items), ImGui::GetFrameCount()};
    menu_open = true;
    popup_open = false;
}

int take_menu_choice(const std::string& id) {
    if (menu_chosen_id != id)
        return -1;
    menu_chosen_id.clear();
    return menu_chosen;
}

bool menu_showing(const std::string& id) { return menu_open && menu.id == id; }

namespace {

void draw_menu() {
    const Theme& t = theme();
    // as wide as the widest item: padding 10, icon 14, gap 8, label
    float w = 0;
    for (const MenuItem& item : menu.items)
        w = std::max(w, text_size(fonts.r13, item.label).x);
    w += dp(4 + 10 + 14 + 8 + 10 + 4);
    float row = dp(6) * 2 + line(fonts.r13);
    ImVec2 a = menu.pos, b(a.x + w, a.y + dp(8) + row * float(menu.items.size()));
    // keep it on the shell's side of the screen
    float shift = std::max(0.f, b.x - ImGui::GetIO().DisplaySize.x + dp(8));
    a.x -= shift;
    b.x -= shift;
    popup_a = a;
    popup_b = b;
    fill(a, b, col(t.popup), dp(RADIUS_MD));
    frame(a, b, col(t.border), dp(RADIUS_MD));
    for (size_t i = 0; i < menu.items.size(); i++) {
        const MenuItem& item = menu.items[i];
        ImVec2 ia(a.x + dp(4), a.y + dp(4) + row * float(i)), ib(b.x - dp(4), ia.y + row);
        bool hovered = item.enabled && hover(ia, ib);
        float h = animate("menu_item", int(i), hovered ? 1.f : 0.f);
        uint32_t color = item.danger ? 0xFFE35B5B : t.text;
        if (!item.enabled)
            color = with_alpha(color, 0.4f);
        fill(ia, ib, col(item.danger ? 0xFFE35B5B : t.text, (item.danger ? 0.12f : 0.08f) * h), dp(RADIUS_MD - 4));
        icons::draw(dl, item.icon, ImVec2(ia.x + dp(10), (ia.y + ib.y) / 2 - dp(7)), dp(14), col(color));
        text_in(fonts.r13, ImVec2(ia.x + dp(10 + 14 + 8), ia.y), row, col(color), item.label);
        if (item.enabled && click(ia, ib)) {
            menu_chosen_id = menu.id;
            menu_chosen = int(i);
            menu_open = false;
        }
    }
    if (menu_open && ImGui::IsMouseReleased(0) && ImGui::GetFrameCount() != menu.opened_frame &&
        !ImGui::IsMouseHoveringRect(a, b, false)) {
        menu_open = false;
        consumed = true;
    }
}

}  // namespace

void popups() {
    if (menu_open) {
        in_popup = true;
        draw_menu();
        in_popup = false;
        return;
    }
    if (!popup_open) {
        popup_a = popup_b = ImVec2(0, 0);
        return;
    }
    const Theme& t = theme();
    in_popup = true;
    popup_a = popup.a;
    popup_b = popup.b;
    fill(popup.a, popup.b, col(t.popup), dp(RADIUS_MD));
    frame(popup.a, popup.b, col(t.border), dp(RADIUS_MD));
    for (size_t i = 0; i < popup.options.size(); i++) {
        ImVec2 a(popup.a.x + dp(4), popup.a.y + dp(4) + dp(32) * float(i)), b(popup.b.x - dp(4), a.y + dp(32));
        bool hovered = hover(a, b);
        float h = animate("popup", int(i), hovered ? 1.f : 0.f);
        fill(a, b, col(t.text, 0.08f * h), dp(RADIUS_SM));
        bool selected = int(config::get(popup.key)) == int(i);
        text_in(fonts.r14, ImVec2(a.x + dp(8), a.y), dp(32), col(selected ? accent() : t.text), popup.options[i]);
        if (click(a, b)) {
            config::set(popup.key, float(i));
            popup_open = false;
        }
    }
    // a click outside closes it
    if (popup_open && ImGui::IsMouseReleased(0) && ImGui::GetFrameCount() != popup.opened_frame &&
        !ImGui::IsMouseHoveringRect(popup.a, popup.b, false))
        popup_open = false;
    in_popup = false;
}

}  // namespace ui
