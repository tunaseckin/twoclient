#include "shell.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <vector>

#include "config.h"
#include "icons.h"
#include "mods.h"
#include "pages.h"
#include "ui.h"

using namespace ui;

namespace shell {
namespace {

constexpr float WIDTH = 1391, HEIGHT = 700, SIDEBAR_WIDTH = 264, PAGE_GUTTER = 25;

// --- the window -------------------------------------------------------------

// Shell.kt's accent glow, two soft blobs behind the translucent panels
float glow_at(float t) {
    return stops({{0, .95f}, {.2f, .77f}, {.4f, .42f}, {.5f, .25f}, {.6f, .13f}, {.7f, .055f}, {.8f, .018f}, {1, 0}}, t);
}

void window_background() {
    const Theme& t = theme();
    ImVec2 a = at(0, 0), b = at(WIDTH, HEIGHT);
    float r = dp(RADIUS_LG);

    // the 50dp shadow with its spread, offset and hole, as rings outside the window
    constexpr float sigma = 50 * 0.57735f, spread = 4, offset = 13, alpha = 0.42f;
    constexpr int rings = 24;
    float step = 3 * sigma / rings;
    for (int i = 0; i < rings; i++) {
        float d = (i + 0.5f) * step;
        float cover = 0.5f * std::erfc((d - spread) / (sigma * 1.41421356f));
        float down = std::min(d, offset);
        ImVec2 ra(a.x - dp(d), a.y - dp(d) + dp(down)), rb(b.x + dp(d), b.y + dp(d) + dp(down));
        dl->AddRect(ra, rb, col(0xFF000000, alpha * cover), r + dp(d), 0, dp(step) + 1);
    }

    float glow = t.glass ? 0 : config::get("pref.glow_opacity") / 100;
    if (glow > 0) {
        uint32_t acc = accent();
        gradient(a, b, r, 64, 32, [&](float x, float y) {
            float px = (x - O.x) / S, py = (y - O.y) / S;
            float g1 = glow_at(std::hypot(px - 147, py + 199) / 1400);
            float g2 = glow_at(std::hypot(px - 893, py - 736) / 1400);
            return col(acc, (1 - (1 - g1) * (1 - g2)) * glow);
        });
    }

    float sw = dp(SIDEBAR_WIDTH);
    fill(a, ImVec2(a.x + sw, b.y), col(sidebar_color()), r, ImDrawFlags_RoundCornersLeft);
    fill(ImVec2(a.x + sw, a.y), b, col(page_color()), r, ImDrawFlags_RoundCornersRight);
    dl->AddLine(ImVec2(a.x + sw, a.y), ImVec2(a.x + sw, b.y), col(t.border), dp(1));
    frame(a, b, col(t.border), r);
}

// --- navigation -------------------------------------------------------------

using pages::Page;
using pages::Route;

struct Section {
    const char* title;
    std::vector<Page> entries;
};

// navigation/Pages.kt's groups
const Section sections[] = {
        {"Mods & Options", {pages::MODS, pages::PROFILES, pages::KEYBINDS}},
        {"Personalization", {pages::THEMES, pages::PREFERENCES}},
        {CLIENT_NAME, {pages::CHANGELOG, pages::CREDITS}},
};

std::vector<Route> history = {Route{}};
size_t history_at = 0;
std::string search;
float page_changed_at = -10;
int page_direction = 1;  // 1 forward, -1 back

const Route& current() { return history[history_at]; }

void moved(int direction) {
    search.clear();
    blur_text_field();
    page_changed_at = float(ImGui::GetTime());
    page_direction = direction;
}

void navigate(const Route& route) {
    if (route == current()) {
        search.clear();
        return;
    }
    history.resize(history_at + 1);
    history.push_back(route);
    history_at++;
    moved(1);
}

// Sidebar.kt's NavigationEntry: 225x33, the accent behind the selected one
bool nav_entry(int index, const char* icon, const char* title, float y, bool selected) {
    const Theme& t = theme();
    ImVec2 a = at((SIDEBAR_WIDTH - 225) / 2, y), b(a.x + dp(225), a.y + dp(33));
    bool hovered = hover(a, b);
    float sel = animate("nav", index * 2, selected ? 1.f : 0.f);
    float edge = animate("nav", index * 2 + 1, hovered || selected ? 1.f : 0.f);
    uint32_t idle = hovered ? t.text : with_alpha(t.text, 0.8f);
    uint32_t color;
    if (t.glass) {
        // translucentSelection: a faint fill and no border on the selected entry
        fill(a, b, col(t.text, 0.12f * sel), dp(RADIUS_SM));
        frame(a, b, col(t.border, edge * (1 - sel)), dp(RADIUS_SM));
        color = mix(idle, t.text, sel);
    } else {
        fill(a, b, col(accent(), sel), dp(RADIUS_SM));
        frame(a, b, col(t.border, edge), dp(RADIUS_SM));
        color = mix(idle, t.accent_text, sel);
    }
    float icon_px = dp(18);
    icons::draw(dl, icon, ImVec2(a.x + dp(12), a.y + (dp(33) - icon_px) / 2), icon_px, col(color));
    text_in(fonts.r14, ImVec2(a.x + dp(12) + icon_px + dp(16), a.y), dp(33), col(color), title);
    return click(a, b);
}

void sidebar() {
    const Theme& t = theme();
    float y = 28;
    // the logo's 167x19 slot, with the client's name in place of OneConfig's wordmark
    text_centered(fonts.brand, at(SIDEBAR_WIDTH / 2, y + 19.f / 2), col(accent()), CLIENT_NAME);
    y += 19 + 28;

    if (nav_entry(0, "hud", "Edit HUD", y, false))
        mods::editing_hud = true;
    y += 33 + 18;

    int index = 1;
    for (const Section& section : sections) {
        std::string title = section.title;
        for (char& c : title)
            c = char(std::toupper((unsigned char)c));
        text_in(fonts.m11, at((SIDEBAR_WIDTH - 225) / 2 + 6, y), line(fonts.m11), col(t.text), title.c_str());
        y += line(fonts.m11) / S + 2 + 8;
        for (Page page : section.entries) {
            Page here = current().page == pages::MOD_CONFIG        ? pages::MODS
                        : current().page == pages::CHANGELOG_ENTRY ? pages::CHANGELOG
                                                                   : current().page;
            if (nav_entry(index++, pages::icon(page), pages::title(Route{page}), y, here == page))
                navigate(Route{page});
            y += 33 + 8;
        }
        y += 18 - 8;
    }

    // Account: the bottom 73dp under a border, with the active profile under the edition
    ImVec2 a = at(0, HEIGHT - 73), b = at(SIDEBAR_WIDTH, HEIGHT);
    dl->AddLine(a, ImVec2(b.x, a.y), col(t.border), dp(1));
    float mid = (a.y + b.y) / 2;
    float name_h = line(fonts.r14), label_h = line(fonts.r11);
    float top = mid - (name_h + dp(2) + label_h) / 2;
    text_in(fonts.r14, ImVec2(a.x + dp(18), top), name_h, col(t.text), "Bedrock");
    text_in(fonts.r11, ImVec2(a.x + dp(18), top + name_h + dp(2)), label_h, col(t.text_secondary), config::profile().c_str());
    icons::draw(dl, "bell", ImVec2(b.x - dp(36), mid - dp(9)), dp(18), col(t.text));
}

// --- header -----------------------------------------------------------------

bool header() {
    const Theme& t = theme();
    float x0 = SIDEBAR_WIDTH + PAGE_GUTTER, x1 = WIDTH - PAGE_GUTTER;
    float mid = 19 + 18;

    if (icon_button("left-arrow", at(x0, mid - 16), history_at > 0)) {
        history_at--;
        moved(-1);
    }
    if (icon_button("right-arrow", at(x0 + 40, mid - 16), history_at + 1 < history.size())) {
        history_at++;
        moved(1);
    }
    text(fonts.m24, ImVec2(at(x0 + 32 + 8 + 32 + 24, 0).x, at(0, mid).y - fonts.m24->FontSize / 2), col(t.text),
         pages::title(current()));

    bool close = icon_button("close", at(x1 - 32, mid - 16), true);

    // GlobalSearchBar, 256x32
    std::string placeholder = std::string("Search ") + pages::title(current()) + "...";
    text_field("search", search, at(x1 - 32 - 26 - 256, mid - 16), at(x1 - 32 - 26, mid + 16), placeholder.c_str(), "search");
    return close;
}

// FastOutSlowInEasing, Compose's tween default: cubic-bezier(0.4, 0, 0.2, 1)
float fast_out_slow_in(float x) {
    float lo = 0, hi = 1;
    for (int i = 0; i < 20; i++) {
        float t = (lo + hi) / 2;
        float bx = 3 * (1 - t) * (1 - t) * t * 0.4f + 3 * (1 - t) * t * t * 0.2f + t * t * t;
        (bx < x ? lo : hi) = t;
    }
    float t = (lo + hi) / 2;
    return 3 * (1 - t) * t * t + t * t * t;
}

}  // namespace

float scale_for(ImVec2 display) {
    // OneConfigInterface keeps the design size unless the window is too small for it at 90%
    float fit = std::min({display.x * 0.9f / WIDTH, display.y * 0.9f / HEIGHT, 2.f});
    return fit * config::get("pref.ui_scale") / 100;
}

void load_fonts(float scale) { ui::load_fonts(scale); }

ImFont* hud_text() { return fonts.r14; }
ImFont* hud_small() { return fonts.m11; }

bool draw(ImVec2 display, float scale) {
    S = scale;
    O = ImVec2(std::round((display.x - dp(WIDTH)) / 2), std::round((display.y - dp(HEIGHT)) / 2));
    begin_frame();

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(display);
    ImGui::Begin("shell", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs);
    dl = ImGui::GetWindowDrawList();

    window_background();
    // the header and page sit on top, so their clicks go first
    bool close = header();

    // the page, sliding a fifth of its width in from the side it came from, and fading in
    ImVec2 pa = at(SIDEBAR_WIDTH + PAGE_GUTTER, 19 + 36 + 19), pb = at(WIDTH - PAGE_GUTTER, HEIGHT - 19);
    ImVec2 clip_a = at(SIDEBAR_WIDTH, 19 + 36 + 19), clip_b = at(WIDTH, HEIGHT);
    float duration = config::get("pref.page_animation_time");
    float t = config::on("pref.page_animations") ? std::clamp((float(ImGui::GetTime()) - page_changed_at) / duration, 0.f, 1.f) : 1.f;
    float e = fast_out_slow_in(t);
    int from = dl->VtxBuffer.Size;
    dl->PushClipRect(clip_a, clip_b, true);
    Route go = current();
    pages::draw(current(), pa, pb, search, &go);
    dl->PopClipRect();
    if (e < 1) {
        float shift = (1 - e) * (pb.x - pa.x) / 5 * float(page_direction);
        for (int i = from; i < dl->VtxBuffer.Size; i++) {
            ImDrawVert& v = dl->VtxBuffer[i];
            v.pos.x += shift;
            float al = float((v.col >> IM_COL32_A_SHIFT) & 255) * e;
            v.col = (v.col & ~IM_COL32_A_MASK) | (ImU32(al) << IM_COL32_A_SHIFT);
        }
    }
    if (!(go == current()))
        navigate(go);

    sidebar();
    popups();

    ImGui::End();
    end_frame();
    return close;
}

}  // namespace shell
