#include "pages.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <string_view>
#include <vector>

#include "config.h"
#include "icons.h"
#include "mods.h"
#include "ui.h"

using namespace ui;

namespace pages {
namespace {

constexpr float CARD_HEIGHT = 140, CARD_FOOTER = 36, CARD_GLOW = 50, GAP = 19;

// --- the menu's own settings ------------------------------------------------

// OneConfigConfig's preferences that mean something here, with its wording
const std::vector<Setting> preferences = {
        {Setting::SLIDER, "pref.page_opacity", "Page Opacity",
         "How opaque the content panel background is. 0 = fully transparent, 100 = fully opaque.", "GUI", 88, 0, 100, 1},
        {Setting::SLIDER, "pref.sidebar_opacity", "Sidebar Opacity",
         "How opaque the sidebar background is. 0 = fully transparent, 100 = fully opaque.", "GUI", 80, 0, 100, 1},
        {Setting::SLIDER, "pref.glow_opacity", "Glow Opacity", "How strong the accent glow behind the menu is.", "GUI", 25, 0, 100,
         1},
        {Setting::SLIDER, "pref.ui_scale", "UI Scale", "Size of the menu in percent. 100 fits it to the window.", "GUI", 100, 50,
         150, 5},
        {Setting::SWITCH, "pref.open_animation", "Opening Animation", "Plays an animation when opening the menu.", "Animations",
         1},
        {Setting::SWITCH, "pref.close_animation", "Closing Animation", "Plays an animation when closing the menu.", "Animations",
         1},
        {Setting::SLIDER, "pref.opening_time", "Opening Time", "Duration of the opening and closing animations, in seconds.",
         "Animations", 0.6f, 0.1f, 2, 0.05f},
        {Setting::SWITCH, "pref.page_animations", "Show Page Animations", "Animate transitions between pages.", "Animations", 1},
        {Setting::SLIDER, "pref.page_animation_time", "Page Animation Duration",
         "Duration of page transition animations, in seconds.", "Animations", 0.3f, 0.05f, 1, 0.05f},
        {Setting::SWITCH, "pref.ui_sounds", "UI Sounds", "Play sound effects when interacting with the menu.", "Sounds", 1},
        {Setting::SWITCH, "pref.menu_sounds", "Menu Sounds", "Play sounds when opening and closing the menu.", "Sounds", 1},
        {Setting::SWITCH, "pref.click_sounds", "Click Sounds", "Play sounds when clicking controls.", "Sounds", 1},
        {Setting::SWITCH, "pref.ambience", "UI Ambience", "Play a soft ambient loop while the menu is open.", "Sounds", 1},
        {Setting::SLIDER, "pref.sound_volume", "UI Sound Volume", "Volume of the UI sound effects.", "Sounds", 1, 0, 1, 0.05f},
        {Setting::SLIDER, "pref.ambience_volume", "UI Ambience Volume", "Volume of the UI ambience.", "Sounds", 1, 0, 1, 0.05f},
};

const std::vector<Setting> theme_settings = {
        {Setting::SWITCH, "theme.animations", "Animations", "Smooth, clean animations in the interface", "Theme", 1},
};

const std::vector<Setting> keybinds = {
        {Setting::KEYBIND, "key.menu", "Open Menu", "The keybind used to open this menu.", CLIENT_NAME, 16 | 256},
        {Setting::KEYBIND, "key.zoom", "Zoom", "Hold to zoom in.", "Zoom", 'C'},
};

// Accent swatches standing in for OneConfig's color picker
const uint32_t accents[] = {0x2B4BFF, 0x7B3FFF, 0xE0368A, 0xFF4B4B, 0xFF8A00, 0xF5C400, 0x2BC46A, 0x14B8C9};

// --- a ConfigScreen ---------------------------------------------------------

// Rows grouped under their subcategory headers, scrolled, and filtered by search
void config_screen(const char* id, const std::vector<Setting>& settings, ImVec2 a, ImVec2 b, const std::string& search,
                   float top = 0) {
    std::vector<const Setting*> shown;
    for (const Setting& s : settings)
        if (matches(s.title, search) || (s.description && matches(s.description, search)))
            shown.push_back(&s);
    if (shown.empty()) {
        std::string msg = search.empty() ? "No settings available." : "No settings match \"" + search + "\"";
        text_centered(fonts.r14, ImVec2((a.x + b.x) / 2, (a.y + b.y) / 2), col(theme().text_secondary), msg.c_str());
        return;
    }

    float x = (a.x - O.x) / S, w = (b.x - a.x) / S;
    auto layout = [&](bool draw, float offset) {
        float y = (a.y - O.y) / S + top - offset / S;
        const char* group = nullptr;
        int index = 0;
        for (const Setting* s : shown) {
            if (!group || std::strcmp(group, s->group) != 0) {
                group = s->group;
                if (draw)
                    group_header(group, x, y);
                y += 8 + line(fonts.m22) / S + 2 + 8;
            }
            if (draw)
                setting_row(*s, index, x, y, w);
            y += setting_row_height(*s) + 8;
            index++;
        }
        return y - (a.y - O.y) / S + offset / S;
    };
    float content = layout(false, 0) * S;
    float offset = scroll_area(id, a, b, content);
    layout(true, offset);
}

// --- mods page --------------------------------------------------------------

using mods::Category;
using mods::Mod;
using enum mods::Category;

struct CategoryInfo {
    const char* title;
    const char* icon;
};

const CategoryInfo categories[] = {
        {"All", nullptr},          {"Favorites", "star"},  {"Hypixel", nullptr}, {"Performance", "lightning-01"},
        {"Visuals", "paintbrush"}, {"HUD", "hud"},         {"Utility", "settings"}, {"Quality of Life", "qol"},
        {"Other", nullptr},        {"Hidden", "eye-off"},
};

Category active = ALL;

bool flag(const Mod& mod, const char* field) { return config::on(mod.key(field)); }
void flip(const Mod& mod, const char* field) { config::set(mod.key(field), flag(mod, field) ? 0 : 1); }

void toggle_ring(ImVec2 center) {
    uint32_t ring = mix(accent(), 0xFF181600, 0.42f);
    float glow_r = dp(6 + 5), inner = 6.f / 11.f;
    gradient(ImVec2(center.x - glow_r, center.y - glow_r), ImVec2(center.x + glow_r, center.y + glow_r), 0, 12, 12,
             [&](float x, float y) {
                 float t = std::hypot(x - center.x, y - center.y) / glow_r;
                 float al = stops({{inner, .41f}, {inner + (1 - inner) * .4f, .25f}, {inner + (1 - inner) * .8f, .09f}, {1, 0}}, t);
                 return col(ring, t < inner ? 0 : al);
             });
    dl->AddCircleFilled(center, dp(6), col(ring), 32);
    dl->AddCircleFilled(center, dp(3), col(accent()), 24);
}

// Mods.kt's ModCard
void card(const Mod& mod, int index, ImVec2 a, float w, Route* go) {
    const Theme& t = theme();
    ImVec2 b(a.x + w, a.y + dp(CARD_HEIGHT));
    float r = dp(RADIUS_MD);
    bool card_hovered = hover(a, b);

    ImVec2 star_a(a.x + dp(4), a.y + dp(4)), star_b(star_a.x + dp(24), star_a.y + dp(24));
    ImVec2 eye_a(star_b.x, star_a.y), eye_b(eye_a.x + dp(24), star_b.y);
    bool star_hovered = hover(star_a, star_b), eye_hovered = hover(eye_a, eye_b);
    if (click(star_a, star_b))
        flip(mod, "favorite");
    if (click(eye_a, eye_b))
        flip(mod, "hidden");
    // PolyGlass insets the footer 5dp into the card
    float inset = t.glass ? dp(5) : 0;
    ImVec2 foot_a(a.x + inset, b.y - dp(CARD_FOOTER) - inset), foot_b(b.x - inset, b.y - inset);
    if (click(foot_a, foot_b))
        flip(mod, "enabled");
    if (click(a, b))
        *go = {MOD_CONFIG, index};

    bool enabled = flag(mod, "enabled");
    float on = animate("footer", index, enabled ? 1.f : 0.f);
    float glow = animate("card_glow", index, enabled ? 0.5f : 0.f);

    fill(a, b, col(t.mod_card), r);
    text_centered(fonts.m16, ImVec2((a.x + b.x) / 2, (a.y + foot_a.y) / 2), col(t.text), mod.title);

    float fr = t.glass ? dp(10) : r;
    ImDrawFlags corners = t.glass ? 0 : ImDrawFlags_RoundCornersBottom;
    fill(foot_a, foot_b, col(t.text, 0.08f * (1 - on)), fr, corners);
    fill(foot_a, foot_b, col(accent(), on), fr, corners);
    uint32_t foot_text = mix(with_alpha(t.text, 0.75f), t.glass ? t.text : t.accent_text, on);
    text_centered(fonts.r16, ImVec2((a.x + b.x) / 2, (foot_a.y + foot_b.y) / 2), col(foot_text), mod.title);

    if (!t.glass) {
        float cx = (a.x + b.x) / 2, cy = (a.y + b.y) / 2, radius = std::min(w, dp(CARD_HEIGHT)) * 0.9f;
        gradient(a, b, r, 16, 12, [&](float x, float y) {
            float d = std::hypot(x - cx, y - cy) / radius;
            return col(t.text, stops({{0, 0}, {.5f, .04f}, {1, .08f}}, d));
        });
        if (glow > 0.001f) {
            float top = b.y - dp(CARD_GLOW);
            gradient(ImVec2(a.x, top), b, r, 8, 8, [&](float, float y) {
                return col(accent(), stops({{0, 0}, {.4f, .2f}, {1, .4f}}, (y - top) / dp(CARD_GLOW)) * glow);
            });
        }
    }

    int from = dl->VtxBuffer.Size;
    frame(a, b, col(t.border), r);
    fade_vertices(from, [&](float y) { return 1 - (y - a.y) / (b.y - a.y); });

    bool fav = flag(mod, "favorite"), hidden = flag(mod, "hidden");
    float star_alpha = animate("star", index, fav || star_hovered ? 1.f : card_hovered ? 0.6f : 0.f);
    float star_color = animate("star_color", index, fav ? 1.f : 0.f);
    icons::draw(dl, fav ? "star-filled" : "star", ImVec2(star_a.x + dp(3), star_a.y + dp(3)), dp(18),
                col(mix(t.text, t.favorite, star_color), star_alpha));
    float eye_alpha = animate("eye", index, hidden || eye_hovered ? 1.f : card_hovered ? 0.6f : 0.f);
    icons::draw(dl, hidden ? "eye-off" : "eye", ImVec2(eye_a.x + dp(3), eye_a.y + dp(3)), dp(18), col(t.text, eye_alpha));

    toggle_ring(ImVec2(b.x - dp(17), a.y + dp(17)));
}

bool mod_matches(const Mod& mod, const std::string& search) {
    if (matches(mod.title, search) || matches(mod.description, search))
        return true;
    for (const Setting& s : mod.settings)
        if (matches(s.title, search))
            return true;
    return false;
}

void mods_page(ImVec2 a, ImVec2 b, const std::string& search, Route* go) {
    std::span<Mod> list = mods::all();
    float x0 = (a.x - O.x) / S, y0 = (a.y - O.y) / S;

    // chips, left out while searching like OneConfig's search results
    float grid_top = y0;
    if (search.empty()) {
        float x = x0;
        for (int c = ALL; c <= HIDDEN; c++) {
            bool shown = c == ALL || c == FAVORITES || c == active;
            for (const Mod& mod : list) {
                if (c == HIDDEN)
                    shown |= flag(mod, "hidden");
                else if (c != ALL && c != FAVORITES)
                    shown |= mod.category == c && !flag(mod, "hidden");
            }
            if (!shown)
                continue;
            bool clicked = false;
            x += chip("chip", c, x, y0, categories[c].title, categories[c].icon, active == c, &clicked) + 8;
            if (clicked)
                active = active != c ? Category(c) : ALL;
        }
        grid_top = y0 + 33 + GAP;
    }

    std::vector<int> shown;
    for (int i = 0; i < int(list.size()); i++) {
        const Mod& mod = list[i];
        if (!search.empty()) {
            if (mod_matches(mod, search))
                shown.push_back(i);
            continue;
        }
        if (flag(mod, "hidden") != (active == HIDDEN))
            continue;
        if (active != ALL && active != FAVORITES && active != HIDDEN && mod.category != active)
            continue;
        if (active == FAVORITES && !flag(mod, "favorite"))
            continue;
        shown.push_back(i);
    }
    std::stable_sort(shown.begin(), shown.end(), [&](int l, int r) { return flag(list[l], "favorite") && !flag(list[r], "favorite"); });

    ImVec2 ga = at(x0, grid_top);
    if (shown.empty()) {
        const char* msg = !search.empty()       ? "No mods match your search."
                          : active == HIDDEN    ? "No hidden mods."
                          : active == FAVORITES ? "No favorite mods."
                                                : "No mods.";
        text_centered(fonts.r14, ImVec2((ga.x + b.x) / 2, (ga.y + b.y) / 2), col(theme().text_secondary), msg);
        return;
    }

    float w = (b.x - a.x) / S;
    float card_w = (w - 3 * GAP) / 4;
    int rows = int((shown.size() + 3) / 4);
    float content = dp(rows * CARD_HEIGHT + (rows - 1) * GAP);
    float offset = scroll_area("mods", ga, b, content) / S;
    dl->PushClipRect(ga, b, true);
    for (size_t n = 0; n < shown.size(); n++) {
        float x = x0 + float(n % 4) * (card_w + GAP);
        float y = grid_top + float(n / 4) * (CARD_HEIGHT + GAP) - offset;
        card(list[shown[n]], shown[n], at(x, y), dp(card_w), go);
    }
    dl->PopClipRect();
}

// Mod settings, with the card's on/off switch first
void mod_config(int index, ImVec2 a, ImVec2 b, const std::string& search) {
    static std::deque<std::string> keys;  // keeps the generated keys alive for the Setting pointers
    static std::vector<std::vector<Setting>> pages;
    std::span<Mod> list = mods::all();
    if (pages.empty()) {
        for (const Mod& mod : list) {
            keys.push_back(mod.key("enabled"));
            std::vector<Setting> rows = {{Setting::SWITCH, keys.back().c_str(), "Enabled", mod.description, "General", 0}};
            rows.insert(rows.end(), mod.settings.begin(), mod.settings.end());
            pages.push_back(rows);
        }
    }
    if (index < 0 || index >= int(pages.size()))
        return;
    config_screen(list[index].id, pages[index], a, b, search);
}

// --- profiles ---------------------------------------------------------------

constexpr float PROFILE_CARD = 180;
constexpr uint32_t DANGER = 0xFFE35B5B;

const char* profile_icons[] = {"profiles", "star", "hud", "paintbrush", "lightning-01", "qol", "keyboard", "cog", "settings"};
const char* profile_icon_names[] = {"Profiles", "Star", "HUD", "Brush", "Lightning", "Quality of Life", "Keyboard", "Cog", "Sliders"};

// What a profile card is busy with, Profiles.kt's ProfileEditor
enum class Editing { NONE, CREATE, RENAME, DELETE };
Editing editing = Editing::NONE;
std::string editing_profile;
std::string name_text;
std::string profile_error;
bool favorites_only = false;

// ActionIcon: an 18dp icon in a 22dp box
bool action_icon(const char* icon, ImVec2 a, uint32_t tint, uint32_t hovered_tint) {
    ImVec2 b(a.x + dp(22), a.y + dp(22));
    bool hovered = hover(a, b);
    float h = animate("action_icon", int(ImHashStr(icon) ^ ImHashData(&a, sizeof a)), hovered ? 1.f : 0.f);
    icons::draw(dl, icon, ImVec2(a.x + dp(2), a.y + dp(2)), dp(18), col(mix(tint, hovered_tint, h)));
    return click(a, b);
}

std::string profile_label(const std::string& name) { return name; }

bool valid_name(const std::string& name, const std::vector<std::string>& all, const std::string& except) {
    std::string n = name;
    while (!n.empty() && n.back() == ' ')
        n.pop_back();
    if (n.empty()) {
        profile_error = "Enter a name.";
        return false;
    }
    for (const std::string& other : all)
        if (other != except && ui::matches(other.c_str(), n) && other.size() == n.size()) {
            profile_error = "A profile with that name exists.";
            return false;
        }
    profile_error.clear();
    return true;
}

void close_editor() {
    editing = Editing::NONE;
    editing_profile.clear();
    name_text.clear();
    profile_error.clear();
    blur_text_field();
}

// The name field in the middle of a card while it's being named
void name_editor(ImVec2 a, ImVec2 b, float y) {
    float w = dp(150);
    float cx = (a.x + b.x) / 2;
    text_field("profile_name", name_text, ImVec2(cx - w / 2, y), ImVec2(cx + w / 2, y + dp(32)), "Profile name", nullptr);
    if (!profile_error.empty())
        text_centered(fonts.r12, ImVec2(cx, y + dp(32) + dp(12)), col(DANGER), profile_error.c_str());
}

void create_card(ImVec2 a, ImVec2 b, const std::vector<std::string>& all) {
    const Theme& t = theme();
    bool creating = editing == Editing::CREATE;
    bool hovered = hover(a, b);
    float on = animate("create_card", 0, creating || hovered ? 1.f : 0.f);
    uint32_t border = mix(t.border, accent(), on);

    auto submit = [&] {
        if (!valid_name(name_text, all, ""))
            return;
        config::create_profile(name_text);
        close_editor();
    };
    if (creating) {
        ImVec2 ia(b.x - dp(13) - dp(22), a.y + dp(13));
        if (action_icon("close", ia, t.text_secondary, t.text_secondary))
            close_editor();
        else if (action_icon("tick", ImVec2(ia.x - dp(32), ia.y), accent(), accent()))
            submit();
    }
    if (!creating && click(a, b)) {
        editing = Editing::CREATE;
        editing_profile.clear();
        name_text.clear();
        focus_text_field(name_text);
    }

    fill(a, b, col(t.mod_card), dp(RADIUS_MD));
    int from = dl->VtxBuffer.Size;
    frame(a, b, col(border), dp(RADIUS_MD));
    fade_vertices(from, [&](float y) { return 1 - (y - a.y) / (b.y - a.y); });

    float cx = (a.x + b.x) / 2;
    float top = (a.y + b.y) / 2 - (dp(64 + 24) + line(fonts.s18)) / 2;
    dl->AddCircleFilled(ImVec2(cx, top + dp(32)), dp(32), col(mix(with_alpha(t.text, 0.06f), with_alpha(accent(), 0.18f), on)), 48);
    icons::draw(dl, "plus", ImVec2(cx - dp(15), top + dp(32) - dp(15)), dp(30), col(mix(t.text_secondary, accent(), on)));
    float ny = top + dp(64 + 24);
    if (creating) {
        name_editor(a, b, ny - dp(4));
        if (submitted())
            submit();
    } else {
        text_centered(fonts.s18, ImVec2(cx, ny + line(fonts.s18) / 2), col(t.text), "New profile");
    }
}

void profile_card(const std::string& name, int index, ImVec2 a, ImVec2 b, const std::vector<std::string>& all) {
    const Theme& t = theme();
    bool active = name == config::profile();
    bool favorite = config::profile_value(name, "profile.favorite", 0) != 0;
    int icon = std::clamp(int(config::profile_value(name, "profile.icon", 0)), 0, IM_ARRAYSIZE(profile_icons) - 1);
    bool mine = editing_profile == name;
    bool renaming = mine && editing == Editing::RENAME, deleting = mine && editing == Editing::DELETE;
    std::string menu_id = "profile:" + name;
    bool hovered = hover(a, b);

    // actions along the top right, checked first since they sit on the card
    ImVec2 ia(b.x - dp(13) - dp(22), a.y + dp(13));
    bool acted = false;
    if (renaming || deleting) {
        if (action_icon("close", ia, t.text_secondary, t.text_secondary)) {
            close_editor();
            acted = true;
        } else if (action_icon("tick", ImVec2(ia.x - dp(32), ia.y), renaming ? accent() : DANGER, renaming ? accent() : DANGER)) {
            if (renaming && valid_name(name_text, all, name)) {
                config::rename_profile(name, name_text);
                close_editor();
            } else if (deleting) {
                config::delete_profile(name);
                close_editor();
            }
            acted = true;
        }
    } else {
        float x = ia.x;
        if (active || hovered || menu_showing(menu_id)) {
            if (action_icon("settings", ImVec2(x, ia.y), t.text_secondary, accent())) {
                open_menu(menu_id, ImVec2(x, ia.y + dp(26)),
                          {{"copy", "Clone"},
                           {"text-input", "Rename"},
                           {"paintbrush", "Change icon"},
                           {favorite ? "star-filled" : "star", favorite ? "Remove favorite" : "Favorite"},
                           {"trash", "Delete", true, all.size() > 1}});
                acted = true;
            }
            x -= dp(32);
        }
        if (favorite && action_icon("star-filled", ImVec2(x, ia.y), t.favorite, t.favorite)) {
            config::set_profile_value(name, "profile.favorite", 0);
            acted = true;
        }
    }

    switch (take_menu_choice(menu_id)) {
    case 0: {
        // Clone: the next free "<name> Copy" name
        std::string copy = name + " Copy";
        for (int n = 2; std::find(all.begin(), all.end(), copy) != all.end(); n++)
            copy = name + " Copy " + std::to_string(n);
        config::switch_profile(name);
        config::create_profile(copy);
        break;
    }
    case 1:
        editing = Editing::RENAME;
        editing_profile = name;
        name_text = name;
        focus_text_field(name_text);
        break;
    case 2: {
        std::vector<MenuItem> items;
        for (int i = 0; i < IM_ARRAYSIZE(profile_icons); i++)
            items.push_back({profile_icons[i], profile_icon_names[i]});
        open_menu("icon:" + name, ImVec2(ia.x, ia.y + dp(26)), items);
        break;
    }
    case 3: config::set_profile_value(name, "profile.favorite", favorite ? 0 : 1); break;
    case 4:
        editing = Editing::DELETE;
        editing_profile = name;
        break;
    default: break;
    }
    if (int picked = take_menu_choice("icon:" + name); picked >= 0)
        config::set_profile_value(name, "profile.icon", float(picked));

    if (!acted && !renaming && !deleting && click(a, b))
        config::switch_profile(name);

    // the active profile wears a 3dp accent ring
    float sel = animate("profile_active", index, active ? 1.f : 0.f);
    fill(a, b, col(t.mod_card), dp(RADIUS_MD));
    int from = dl->VtxBuffer.Size;
    frame(a, b, col(t.border), dp(RADIUS_MD));
    fade_vertices(from, [&](float y) { return 1 - (y - a.y) / (b.y - a.y); });
    frame(a, b, col(accent(), sel), dp(RADIUS_MD), 3);

    float cx = (a.x + b.x) / 2;
    bool compact = renaming || deleting;
    float icon_px = dp(compact ? 42 : 64), gap = dp(compact ? 14 : 24);
    float block = icon_px + gap + line(fonts.s18) + (deleting ? dp(4) + line(fonts.r13) : 0);
    float top = (a.y + b.y) / 2 - block / 2;
    icons::draw(dl, profile_icons[icon], ImVec2(cx - icon_px / 2, top), icon_px, col(t.text));
    float ny = top + icon_px + gap;
    if (renaming) {
        name_editor(a, b, ny - dp(4));
        if (submitted() && valid_name(name_text, all, name)) {
            config::rename_profile(name, name_text);
            close_editor();
        }
    } else {
        dl->PushClipRect(ImVec2(a.x + dp(18), a.y), ImVec2(b.x - dp(18), b.y), true);
        text_centered(fonts.s18, ImVec2(cx, ny + line(fonts.s18) / 2), col(t.text), name.c_str());
        dl->PopClipRect();
        if (deleting)
            text_centered(fonts.r13, ImVec2(cx, ny + line(fonts.s18) + dp(4) + line(fonts.r13) / 2), col(DANGER), "Delete this profile?");
    }
}

void profiles_page(ImVec2 a, ImVec2 b, const std::string& search) {
    const Theme& t = theme();
    float x = (a.x - O.x) / S, y = (a.y - O.y) / S, w = (b.x - a.x) / S;
    std::vector<std::string> all = config::profiles();

    bool clicked = false;
    float cx = x;
    cx += chip("profile_chip", 0, cx, y, "All profiles", nullptr, !favorites_only, &clicked) + 8;
    if (clicked)
        favorites_only = false;
    chip("profile_chip", 1, cx, y, "Favorites", "star", favorites_only, &clicked);
    if (clicked)
        favorites_only = true;
    y += 33 + GAP;

    std::vector<std::string> shown;
    for (const std::string& name : all) {
        if (favorites_only && config::profile_value(name, "profile.favorite", 0) == 0)
            continue;
        if (!matches(name.c_str(), search))
            continue;
        shown.push_back(name);
    }
    bool with_create = !favorites_only && search.empty();
    if (shown.empty() && !with_create) {
        std::string msg = search.empty() ? "No favorite profiles." : "No profiles match \"" + search + "\"";
        text_centered(fonts.r14, ImVec2((a.x + b.x) / 2, (at(0, y).y + b.y) / 2), col(t.text_secondary), msg.c_str());
        return;
    }

    float card_w = (w - 4 * GAP) / 5;
    int count = int(shown.size()) + (with_create ? 1 : 0);
    int rows = (count + 4) / 5;
    ImVec2 ga = at(x, y);
    float offset = scroll_area("profiles", ga, b, dp(rows * PROFILE_CARD + (rows - 1) * GAP)) / S;
    dl->PushClipRect(ga, b, true);
    for (int n = 0; n < count; n++) {
        float px = x + float(n % 5) * (card_w + GAP), py = y + float(n / 5) * (PROFILE_CARD + GAP) - offset;
        ImVec2 ca = at(px, py), cb = at(px + card_w, py + PROFILE_CARD);
        if (with_create && n == 0)
            create_card(ca, cb, all);
        else
            profile_card(shown[n - (with_create ? 1 : 0)], n, ca, cb, all);
    }
    dl->PopClipRect();
}

// --- keybinds ---------------------------------------------------------------

struct KeybindEntry {
    const Setting* setting;
    const char* path;  // "category / subcategory", as the row's third line
};

struct KeybindGroup {
    const char* title;
    const char* icon;
    std::vector<KeybindEntry> entries;
};

std::vector<KeybindGroup> keybind_groups() {
    return {
            {CLIENT_NAME, "settings", {{&keybinds[0], "General / GUI"}}},
            {"Zoom", "qol", {{&keybinds[1], "General / General"}}},
    };
}

void keybinds_page(ImVec2 a, ImVec2 b, const std::string& search) {
    const Theme& t = theme();
    static std::vector<bool> collapsed(8, false);
    std::vector<KeybindGroup> groups = keybind_groups();
    bool searching = !search.empty();

    // which keys more than one keybind uses
    auto conflicts = [&](const Setting* s) {
        std::vector<std::string> names;
        int k = int(config::get(s->key));
        for (const KeybindGroup& g : groups)
            for (const KeybindEntry& e : g.entries)
                if (e.setting != s && k != 0 && int(config::get(e.setting->key)) == k)
                    names.push_back(e.setting->title);
        return names;
    };

    float x = (a.x - O.x) / S, w = (b.x - a.x) / S;
    auto row_h = [&](const Setting* s, bool conflict) {
        return 12 + (line(fonts.m15) + dp(2) + line(fonts.r13) + dp(2) + line(fonts.r11) + (conflict ? dp(2) + line(fonts.r11) : 0)) / S + 12;
    };
    auto layout = [&](bool draw, float offset) {
        float y = (a.y - O.y) / S - offset / S, start = y;
        bool any = false;
        for (size_t gi = 0; gi < groups.size(); gi++) {
            const KeybindGroup& g = groups[gi];
            bool group_hit = matches(g.title, search);
            std::vector<const KeybindEntry*> rows;
            for (const KeybindEntry& e : g.entries)
                if (!searching || group_hit || matches(e.setting->title, search))
                    rows.push_back(&e);
            if (rows.empty())
                continue;
            any = true;
            bool expanded = searching || !collapsed[gi];
            // KeybindGroupHeader: icon, title and a chevron that turns
            float hh = 4 + line(fonts.m11) / S + 2;
            if (draw) {
                ImVec2 ha = at(x, y), hb = at(x + w, y + hh);
                if (click(ha, hb))
                    collapsed[gi] = !collapsed[gi];
                float mid = (ha.y + hb.y) / 2 + dp(1);
                float hx = ha.x;
                icons::draw(dl, g.icon, ImVec2(hx, mid - dp(9)), dp(18), col(t.text_secondary));
                hx += dp(18 + 10);
                text_in(fonts.m11, ImVec2(hx, mid - line(fonts.m11) / 2), line(fonts.m11), col(t.text_secondary), g.title);
                hx += text_size(fonts.m11, g.title).x + dp(10);
                float turn = animate("keybind_chevron", int(gi), expanded ? 180.f : 90.f);
                icons::draw_rotated(dl, "up", ImVec2(hx, mid - dp(6)), dp(12), col(t.text_secondary), turn);
            }
            y += hh + 12;
            if (!expanded)
                continue;
            for (const KeybindEntry* e : rows) {
                std::vector<std::string> clash = conflicts(e->setting);
                float h = row_h(e->setting, !clash.empty());
                if (draw) {
                    ImVec2 ra = at(x, y), rb = at(x + w, y + h);
                    card_surface(ra, rb);
                    float ty = ra.y + dp(12);
                    text_in(fonts.m15, ImVec2(ra.x + dp(16), ty), line(fonts.m15), col(t.text), e->setting->title);
                    ty += line(fonts.m15) + dp(2);
                    text_in(fonts.r13, ImVec2(ra.x + dp(16), ty), line(fonts.r13), col(t.text_secondary), e->setting->description);
                    ty += line(fonts.r13) + dp(2);
                    text_in(fonts.r11, ImVec2(ra.x + dp(16), ty), line(fonts.r11), col(t.text_secondary), e->path);
                    ty += line(fonts.r11) + dp(2);
                    if (!clash.empty()) {
                        std::string msg = "Conflicts with ";
                        for (size_t i = 0; i < clash.size(); i++)
                            msg += (i ? ", " : "") + clash[i];
                        icons::draw(dl, "alert-circle", ImVec2(ra.x + dp(16), ty + line(fonts.r11) / 2 - dp(6)), dp(12), col(0xFFE0524F));
                        text_in(fonts.r11, ImVec2(ra.x + dp(16 + 12 + 4), ty), line(fonts.r11), col(0xFFE0524F), msg.c_str());
                    }
                    keybind_control(e->setting->key, rb.x - dp(16), (ra.y + rb.y) / 2, !clash.empty());
                }
                y += h + 12;
            }
        }
        return std::pair(y - start, any);
    };
    auto [content, any] = layout(false, 0);
    if (!any) {
        std::string msg = search.empty() ? "No keybinds available." : "No keybinds match \"" + search + "\"";
        text_centered(fonts.r14, ImVec2((a.x + b.x) / 2, (a.y + b.y) / 2), col(t.text_secondary), msg.c_str());
        return;
    }
    float offset = scroll_area("keybinds", a, b, content * S);
    layout(true, offset);
}

// --- themes -----------------------------------------------------------------

// A small drawing of the shell in a theme's colors, in place of OneConfig's preview art
void theme_preview(const Theme& th, ImVec2 a, ImVec2 b) {
    uint32_t base = th.light ? 0xFFE6EAF2 : 0xFF0B0F12;
    float r = dp(10);
    fill(a, b, col(base), r);
    fill(a, b, col(th.page), r);
    float sw = (b.x - a.x) * 0.3f;
    fill(a, ImVec2(a.x + sw, b.y), col(th.sidebar), r, ImDrawFlags_RoundCornersLeft);
    fill(ImVec2(a.x + dp(8), a.y + dp(10)), ImVec2(a.x + sw - dp(8), a.y + dp(16)), col(accent()), dp(3));
    for (int i = 0; i < 3; i++)
        fill(ImVec2(a.x + dp(8), a.y + dp(24 + i * 12)), ImVec2(a.x + sw - dp(8), a.y + dp(30 + i * 12)), col(th.text, 0.3f), dp(3));
    float cw = (b.x - a.x - sw - dp(24)) / 2;
    for (int i = 0; i < 4; i++) {
        float cx = a.x + sw + dp(8) + float(i % 2) * (cw + dp(8)), cy = a.y + dp(10) + float(i / 2) * dp(38);
        fill(ImVec2(cx, cy), ImVec2(cx + cw, cy + dp(32)), col(th.mod_card | 0x40000000), dp(5));
    }
    frame(a, b, col(th.border), r);
}

void themes_page(ImVec2 a, ImVec2 b, const std::string& search) {
    const Theme& t = theme();
    float x = (a.x - O.x) / S, y = (a.y - O.y) / S, w = (b.x - a.x) / S;
    int current = std::clamp(int(config::get("theme.index")), 0, theme_count - 1);

    // one card per family with its dark and light variants
    const char* families[] = {"Polyfrosted", "PolyGlass"};
    float card_w = (w - 20) / 2;
    for (int f = 0; f < 2; f++) {
        int dark = f * 2, light = f * 2 + 1;
        bool selected = current == dark || current == light;
        static int picked[2] = {0, 0};
        if (selected)
            picked[f] = current == light;
        int shown = picked[f] ? light : dark;

        ImVec2 ca = at(x + f * (card_w + 20), y), cb = at(x + f * (card_w + 20) + card_w, y + 114);
        float sel = animate("theme_card", f, selected ? 1.f : 0.f);
        // the variant toggle sits on the card, so its clicks go first
        ImVec2 ta(ca.x + dp(12 + 140 + 14), ca.y + dp(12) + line(fonts.m15) + dp(12)), tb(ta.x + dp(3 + 32 + 2 + 32 + 3), ta.y + dp(30));
        for (int v = 0; v < 2; v++) {
            ImVec2 sa(ta.x + dp(3 + v * 34), ta.y + dp(3)), sb(sa.x + dp(32), tb.y - dp(3));
            if (click(sa, sb)) {
                picked[f] = v;
                if (selected)
                    config::set("theme.index", float(f * 2 + v));
            }
        }
        if (click(ca, cb))
            config::set("theme.index", float(shown));

        fill(ca, cb, col(t.mod_card), dp(RADIUS_LG));
        frame(ca, cb, col(mix(t.border, accent(), sel)), dp(RADIUS_LG), 1 + 2 * sel);
        theme_preview(themes[shown], ImVec2(ca.x + dp(12), ca.y + dp(12)), ImVec2(ca.x + dp(152), ca.y + dp(102)));
        text_in(fonts.m15, ImVec2(ca.x + dp(166), ca.y + dp(12)), line(fonts.m15), col(t.text), families[f]);
        fill(ta, tb, col(t.chip), dp(RADIUS_SM));
        frame(ta, tb, col(t.border), dp(RADIUS_SM));
        for (int v = 0; v < 2; v++) {
            ImVec2 sa(ta.x + dp(3 + v * 34), ta.y + dp(3)), sb(sa.x + dp(32), tb.y - dp(3));
            float on = animate("variant", f * 2 + v, picked[f] == v ? 1.f : 0.f);
            fill(sa, sb, col(t.text, 0.1f * on), dp(RADIUS_SM - 3));
            icons::draw(dl, v ? "sun" : "moon", ImVec2((sa.x + sb.x) / 2 - dp(7), (sa.y + sb.y) / 2 - dp(7)), dp(14), col(t.text));
        }
    }
    y += 114 + 20;

    // Accent Color, with swatches for the picker
    if (matches("Accent Color", search)) {
        Setting row{Setting::SWITCH, "theme.accent", "Accent Color", "The accent color of the interface", "Theme", 0};
        float h = setting_row_height(row);
        ImVec2 ra = at(x, y), rb = at(x + w, y + h);
        card_surface(ra, rb);
        float label_h = line(fonts.m15) + dp(2) + line(fonts.r13);
        float ly = (ra.y + rb.y - label_h) / 2;
        text_in(fonts.m15, ImVec2(ra.x + dp(16), ly), line(fonts.m15), col(t.text), row.title);
        text_in(fonts.r13, ImVec2(ra.x + dp(16), ly + line(fonts.m15) + dp(2)), line(fonts.r13), col(t.text_secondary), row.description);
        float mid = (ra.y + rb.y) / 2, sx = rb.x - dp(16) - dp(24);
        for (int i = IM_ARRAYSIZE(accents) - 1; i >= 0; i--) {
            ImVec2 sa(sx, mid - dp(12)), sb(sx + dp(24), mid + dp(12));
            bool chosen = (accent() & 0xFFFFFF) == accents[i];
            dl->AddCircleFilled(ImVec2(sx + dp(12), mid), dp(10), col(0xFF000000 | accents[i]), 24);
            if (chosen)
                dl->AddCircle(ImVec2(sx + dp(12), mid), dp(12), col(t.text), 24, dp(2));
            if (click(sa, sb))
                config::set("theme.accent", float(accents[i]));
            sx -= dp(32);
        }
        y += h + 8;
    }
    config_screen("themes", theme_settings, at(x, y), b, search);
}

// --- changelog and credits --------------------------------------------------

struct ChangelogEntry {
    const char* title;
    const char* author;
    const char* date;
    // a small slice of markdown: "# " and "## " headings, "- " bullets, the rest paragraphs
    std::vector<const char*> lines;
};

const ChangelogEntry changelog[] = {
        {CLIENT_NAME " for Bedrock", "Tuna Seçkin", "November 8, 2026",
         {"The menu you know from Java, now in Bedrock on macOS and Linux.",
          "## Menu",
          "- Opens with Right Shift while you're in a world, with OneConfig's look, sounds and animations.",
          "- Mods, Profiles, Keybinds, Themes and Preferences, kept between sessions.",
          "- Search on every page.",
          "## Mods",
          "- Keystrokes, FPS and CPS HUDs.",
          "- Zoom, with smooth zoom and scroll to zoom.",
          "## Themes",
          "- Polyfrosted and PolyGlass, each in dark and light, with an accent color."}},
};

// MdBlocks: blocks 6dp apart, bullet lines 3dp apart; returns the height in pixels
float markdown(const ChangelogEntry& e, ImVec2 pos, float width, bool draw) {
    const Theme& t = theme();
    float y = pos.y;
    bool prev_bullet = false;
    for (size_t i = 0; i < e.lines.size(); i++) {
        std::string_view l = e.lines[i];
        bool bullet = l.rfind("- ", 0) == 0;
        if (i > 0)
            y += dp(bullet && prev_bullet ? 3 : 6);
        if (l.rfind("# ", 0) == 0) {
            if (draw)
                text_wrapped(fonts.s18, ImVec2(pos.x, y), width, col(t.text), e.lines[i] + 2);
            y += wrapped_height(fonts.s18, e.lines[i] + 2, width);
        } else if (l.rfind("## ", 0) == 0) {
            if (draw)
                text_wrapped(fonts.m15, ImVec2(pos.x, y), width, col(t.text), e.lines[i] + 3);
            y += wrapped_height(fonts.m15, e.lines[i] + 3, width);
        } else if (bullet) {
            float indent = text_size(fonts.r13, "•").x + dp(6);
            if (draw) {
                text(fonts.r13, ImVec2(pos.x, y), col(t.text_secondary), "•");
                text_wrapped(fonts.r13, ImVec2(pos.x + indent, y), width - indent, col(t.text_secondary), e.lines[i] + 2);
            }
            y += wrapped_height(fonts.r13, e.lines[i] + 2, width - indent);
        } else {
            if (draw)
                text_wrapped(fonts.r13, ImVec2(pos.x, y), width, col(t.text_secondary), e.lines[i]);
            y += wrapped_height(fonts.r13, e.lines[i], width);
        }
        prev_bullet = bullet;
    }
    return y - pos.y;
}

bool entry_matches(const ChangelogEntry& e, const std::string& search) {
    if (matches(e.title, search) || matches(e.author, search))
        return true;
    for (const char* l : e.lines)
        if (matches(l, search))
            return true;
    return false;
}

// The faint radial wash Changelog.kt draws behind each card
void soft_vignette(ImVec2 a, ImVec2 b) {
    uint32_t text = theme().text;
    float cx = (a.x + b.x) / 2, cy = (a.y + b.y) / 2, radius = std::min(b.x - a.x, b.y - a.y) * 0.9f;
    gradient(a, b, dp(RADIUS_MD), 16, 8, [&](float x, float y) {
        return col(text, stops({{0, 0}, {.5f, .02f}, {1, .05f}}, std::hypot(x - cx, y - cy) / radius));
    });
}

void changelog_page(ImVec2 a, ImVec2 b, const std::string& search, Route* go) {
    const Theme& t = theme();
    float x = (a.x - O.x) / S, w = (b.x - a.x) / S;
    std::vector<int> shown;
    for (int i = 0; i < IM_ARRAYSIZE(changelog); i++)
        if (entry_matches(changelog[i], search))
            shown.push_back(i);
    if (shown.empty()) {
        std::string msg = "No changelog entries match \"" + search + "\"";
        text_centered(fonts.r14, ImVec2((a.x + b.x) / 2, (a.y + b.y) / 2), col(t.text_secondary), msg.c_str());
        return;
    }
    // ChangelogEntry: 16 by 30 padding, title, a 160dp preview fading out, then the date and Read more
    float card_h = 16 + line(fonts.s18) / S + 8 + 160 + 8 + line(fonts.r12) / S + 16;
    float content = float(shown.size()) * (card_h + 20) - 20;
    float offset = scroll_area("changelog", a, b, dp(content)) / S;
    float y = (a.y - O.y) / S - offset;
    for (int i : shown) {
        const ChangelogEntry& e = changelog[i];
        ImVec2 ca = at(x, y), cb = at(x + w, y + card_h);
        fill(ca, cb, col(t.mod_card), dp(RADIUS_MD));
        soft_vignette(ca, cb);
        int from = dl->VtxBuffer.Size;
        frame(ca, cb, col(t.border), dp(RADIUS_MD));
        fade_vertices(from, [&](float yy) { return 1 - (yy - ca.y) / (cb.y - ca.y); });

        float px = ca.x + dp(30), pw = cb.x - ca.x - dp(60);
        float yy = ca.y + dp(16);
        text_in(fonts.s18, ImVec2(px, yy), line(fonts.s18), col(t.text), e.title);
        yy += line(fonts.s18) + dp(8);
        ImVec2 ba(px, yy), bb(px + pw, yy + dp(160));
        dl->PushClipRect(ba, bb, true);
        int body = dl->VtxBuffer.Size;
        markdown(e, ba, pw, true);
        dl->PopClipRect();
        fade_vertices(body, [&](float vy) { return 1 - std::clamp((vy - (ba.y + dp(120))) / dp(40), 0.f, 1.f); });
        yy += dp(160 + 8);
        text_in(fonts.r12, ImVec2(px, yy), line(fonts.r12), col(t.text_secondary), e.date);
        float more_w = text_size(fonts.m11, "Read more").x * 12 / 11;
        ImVec2 ma(px + pw - more_w, yy), mb(px + pw, yy + line(fonts.r12));
        text_in(fonts.r12, ma, line(fonts.r12), col(accent()), "Read more");
        if (click(ma, mb))
            *go = {CHANGELOG_ENTRY, i};
        y += card_h + 20;
    }
}

void changelog_entry_page(int index, ImVec2 a, ImVec2 b) {
    if (index < 0 || index >= IM_ARRAYSIZE(changelog))
        return;
    const Theme& t = theme();
    const ChangelogEntry& e = changelog[index];
    float w = b.x - a.x;
    auto layout = [&](bool draw, float offset) {
        float y = a.y + dp(22) - offset;
        // title on the left, the byline and date on the right
        if (draw) {
            text_in(fonts.m24, ImVec2(a.x, y), line(fonts.m24), col(t.text), e.title);
            std::string by = std::string("Written by ") + e.author;
            float bw = text_size(fonts.r12, "Written by ").x + text_size(fonts.r12, e.author).x;
            text(fonts.r12, ImVec2(b.x - bw, y + dp(6)), col(t.text_secondary), "Written by ");
            text(fonts.r12, ImVec2(b.x - text_size(fonts.r12, e.author).x, y + dp(6)), col(t.text), e.author);
            float dw = text_size(fonts.r12, e.date).x;
            text(fonts.r12, ImVec2(b.x - dw, y + dp(6) + line(fonts.r12) + dp(2)), col(t.text_secondary), e.date);
        }
        y += line(fonts.m24) + dp(12);
        y += markdown(e, ImVec2(a.x, y), w, draw);
        return y + dp(22) + offset - a.y;
    };
    float offset = scroll_area("changelog_entry", a, b, layout(false, 0));
    layout(true, offset);
}

struct CreditSection {
    const char* title;
    std::vector<const char*> entries;
};

const CreditSection credit_sections[] = {
        {"People", {"Tuna Seçkin - " CLIENT_NAME " for Bedrock"}},
        {"Built on",
         {"OneConfig by Polyfrost - the interface, icons and sounds this menu follows (LGPL-3.0)",
          "mcpelauncher by the minecraft-linux team - runs the game and loads this mod on macOS and Linux",
          "Dear ImGui by Omar Cornut - draws the menu (MIT)", "NanoSVG by Mikko Mononen - draws the icons (zlib)",
          "Poppins by the Indian Type Foundry - the font (SIL Open Font License)",
          "FMOD by Firelight Technologies - plays the sounds, through the copy the game ships"}},
        {"Special Thanks", {"The Polyfrost team, whose OneConfig made this look possible",
                            "Everyone testing on Linux and Windows"}},
};

// Credits.kt: the logo and version, a byline, then sections of " - " entries
void credits_page(ImVec2 a, ImVec2 b, const std::string& search) {
    const Theme& t = theme();
    auto layout = [&](bool draw, float offset) {
        float y = a.y + dp(16) - offset;
        float logo_h = dp(54);
        if (draw) {
            float lw = text_size(fonts.s48, CLIENT_NAME).x;
            text(fonts.s48, ImVec2(a.x, y + logo_h - fonts.s48->FontSize), col(accent()), CLIENT_NAME);
            text(fonts.r13, ImVec2(a.x + lw + dp(8), y + logo_h - dp(6) - fonts.r13->FontSize), col(t.text_secondary), "v0.1");
        }
        y += logo_h + dp(8);
        if (draw) {
            text_in(fonts.r13, ImVec2(a.x, y), dp(24), col(t.text_secondary), "for Bedrock Edition");
        }
        y += dp(24 + 8);
        for (const CreditSection& section : credit_sections) {
            std::vector<const char*> hits;
            for (const char* entry : section.entries)
                if (matches(entry, search) || matches(section.title, search))
                    hits.push_back(entry);
            if (hits.empty())
                continue;
            y += dp(24);
            if (draw)
                text_in(fonts.s24, ImVec2(a.x, y), line(fonts.s24), col(t.text), section.title);
            y += line(fonts.s24) + dp(8);
            for (size_t i = 0; i < hits.size(); i++) {
                std::string line_text = std::string(" - ") + hits[i];
                if (draw)
                    text_wrapped(fonts.r13, ImVec2(a.x, y), b.x - a.x, col(t.text_secondary), line_text.c_str());
                y += wrapped_height(fonts.r13, line_text.c_str(), b.x - a.x) + (i + 1 < hits.size() ? dp(4) : 0);
            }
        }
        y += dp(24);
        if (draw)
            text_in(fonts.r13, ImVec2(a.x, y), line(fonts.r13), col(t.text), "Not affiliated with Mojang or Microsoft.");
        y += line(fonts.r13) + dp(16);
        return y + offset - a.y;
    };
    float offset = scroll_area("credits", a, b, layout(false, 0));
    layout(true, offset);
}

}  // namespace

const char* title(const Route& route) {
    switch (route.page) {
    case MODS: return "Mods";
    case PROFILES: return "Profiles";
    case KEYBINDS: return "Keybinds";
    case THEMES: return "Themes";
    case PREFERENCES: return "Preferences";
    case CHANGELOG: return "Changelog";
    case CREDITS: return "Credits";
    case CHANGELOG_ENTRY:
        return route.mod >= 0 && route.mod < IM_ARRAYSIZE(changelog) ? changelog[route.mod].title : "Changelog";
    case MOD_CONFIG: {
        std::span<Mod> list = mods::all();
        return route.mod >= 0 && route.mod < int(list.size()) ? list[route.mod].title : "Mod";
    }
    }
    return "";
}

const char* icon(Page page) {
    switch (page) {
    case MODS: return "settings";
    case PROFILES: return "profiles";
    case KEYBINDS: return "keyboard";
    case THEMES: return "paintbrush";
    case PREFERENCES: return "cog";
    case CHANGELOG: return "refresh";
    case CREDITS: return "star";
    default: return "settings";
    }
}

void declare() {
    ::declare(preferences);
    ::declare(theme_settings);
    ::declare(keybinds);
    config::declare("theme.index", 0);
    config::declare("profile.icon", 0);
    config::declare("profile.favorite", 0);
    config::declare("theme.accent", float(0x2B4BFF));
}

void draw(const Route& route, ImVec2 a, ImVec2 b, const std::string& search, Route* go) {
    switch (route.page) {
    case MODS: mods_page(a, b, search, go); break;
    case MOD_CONFIG: mod_config(route.mod, a, b, search); break;
    case PROFILES: profiles_page(a, b, search); break;
    case KEYBINDS: keybinds_page(a, b, search); break;
    case THEMES: themes_page(a, b, search); break;
    case PREFERENCES: config_screen("preferences", preferences, a, b, search); break;
    case CHANGELOG: changelog_page(a, b, search, go); break;
    case CHANGELOG_ENTRY: changelog_entry_page(route.mod, a, b); break;
    case CREDITS: credits_page(a, b, search); break;
    }
}

}  // namespace pages
