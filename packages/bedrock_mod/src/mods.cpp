#include "mods.h"

#include <android/log.h>
#include <dlfcn.h>
#include <link.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <vector>

#include "config.h"

namespace mods {
namespace {

// Key codes as the launcher reports them, which follow Windows' virtual keys
constexpr int KEY_W = 'W', KEY_A = 'A', KEY_S = 'S', KEY_D = 'D', KEY_SPACE = ' ';
constexpr int KEY_PRESS = 0, KEY_RELEASE = 2;
constexpr int BUTTON_PRESS = 0;

std::vector<Mod> list = {
        {"keystrokes", "Keystrokes", "Shows the movement keys and mouse buttons as you press them.", HUD,
         {
                 {Setting::SWITCH, "keystrokes.mouse", "Show Mouse Buttons", "Adds LMB and RMB under the keys.", "General", 1},
                 {Setting::SWITCH, "keystrokes.space", "Show Space Bar", "Adds the jump key along the bottom.", "General", 1},
                 {Setting::SLIDER, "keystrokes.background", "Background Opacity", "How dark the keys are when not pressed.",
                  "Appearance", 47, 0, 100, 1},
                 {Setting::SLIDER, "keystrokes.scale", "Scale", "Size of the HUD in percent.", "Appearance", 100, 50, 200, 5},
         }},
        {"fps", "FPS", "Shows how many frames the game draws each second.", HUD,
         {
                 {Setting::DROPDOWN, "fps.style", "Text Style", "How the counter reads.", "General", 0, 0, 0, 0,
                  {"120 FPS", "FPS: 120", "120"}},
                 {Setting::SWITCH, "fps.background", "Background", "Draws a dark box behind the text.", "Appearance", 1},
                 {Setting::SLIDER, "fps.scale", "Scale", "Size of the HUD in percent.", "Appearance", 100, 50, 200, 5},
         }},
        {"cps", "CPS", "Shows how many times you click each second.", HUD,
         {
                 {Setting::SWITCH, "cps.right", "Show Right Click", "Counts right clicks next to left clicks.", "General", 1},
                 {Setting::SWITCH, "cps.background", "Background", "Draws a dark box behind the text.", "Appearance", 1},
                 {Setting::SLIDER, "cps.scale", "Scale", "Size of the HUD in percent.", "Appearance", 100, 50, 200, 5},
         }},
        {"zoom", "Zoom", "Hold a key to zoom in, like OptiFine's zoom.", QOL,
         {
                 {Setting::KEYBIND, "key.zoom", "Zoom Key", "Hold to zoom in.", "General", 'C'},
                 {Setting::SLIDER, "zoom.factor", "Zoom Amount", "How many times closer the zoom brings things.", "General", 4, 1.5f,
                  50, 0.5f},
                 {Setting::SWITCH, "zoom.smooth", "Smooth Zoom", "Eases in and out instead of snapping.", "General", 1},
                 {Setting::SWITCH, "zoom.scroll", "Scroll to Zoom", "Scroll while zooming to zoom further in or out.", "General", 1},
         }},
};

Mod& get(const char* id) {
    for (Mod& mod : list)
        if (!std::strcmp(mod.id, id))
            return mod;
    return list[0];
}

bool enabled(const char* id) { return config::on(std::string(id) + ".enabled"); }

// --- input the HUDs watch ---------------------------------------------------

using Clock = std::chrono::steady_clock;

std::atomic<bool> keys[5];  // W A S D Space
std::atomic<bool> buttons[2];  // left, right
// clicks arrive with input and are counted in the frame
std::mutex clicks_lock;
std::deque<Clock::time_point> clicks[2];

int key_slot(int key) {
    switch (key) {
    case KEY_W: return 0;
    case KEY_A: return 1;
    case KEY_S: return 2;
    case KEY_D: return 3;
    case KEY_SPACE: return 4;
    default: return -1;
    }
}

size_t cps[2] = {};
int fps = 0;
int frames = 0;
float fps_window = 0;

// --- zoom -------------------------------------------------------------------

std::atomic<bool> zoom_held{false};
float zoom_now = 1;  // the multiplier applied this frame, eased towards the target

std::optional<float> (*try_get_fov)(void*) = nullptr;

// If a game update moves tryGetFOV again the hooked slot is some other
// function, so Zoom only touches values that look like a field of view and
// gives up after a run of ones that don't
std::atomic<int> implausible{0};
constexpr int GIVE_UP_AFTER = 120;

std::optional<float> zoomed_fov(void* camera) {
    std::optional<float> fov = try_get_fov(camera);
    if (implausible >= GIVE_UP_AFTER)
        return fov;
    if (!fov || !(*fov > 1 && *fov < 179)) {
        if (fov && ++implausible == GIVE_UP_AFTER)
            __android_log_print(ANDROID_LOG_ERROR, CLIENT_NAME, "Zoom turned itself off, this game version moved the camera's field of view");
        return fov;
    }
    implausible = 0;
    *fov *= zoom_now;
    return fov;
}

struct Segment {
    std::byte* begin;
    size_t size;
    bool exec;
};

std::vector<Segment> segments_of(void* lib) {
    struct Search {
        void* lib;
        std::vector<Segment> out;
    } search{lib, {}};
    dl_iterate_phdr(
            [](dl_phdr_info* info, size_t, void* data) {
                auto* s = static_cast<Search*>(data);
                void* h = dlopen(info->dlpi_name, RTLD_NOLOAD);
                if (h)
                    dlclose(h);
                if (h != s->lib)
                    return 0;
                for (int i = 0; i < info->dlpi_phnum; i++) {
                    const ElfW(Phdr)& ph = info->dlpi_phdr[i];
                    if (ph.p_type == PT_LOAD && (ph.p_flags & PF_R))
                        s->out.push_back({reinterpret_cast<std::byte*>(info->dlpi_addr + ph.p_vaddr), ph.p_memsz,
                                          (ph.p_flags & PF_X) != 0});
                }
                return 1;
            },
            &search);
    return search.out;
}

std::byte* find_string(const std::vector<Segment>& segs, const void* needle, size_t n) {
    for (const Segment& seg : segs)
        if (void* hit = memmem(seg.begin, seg.size, needle, n))
            return static_cast<std::byte*>(hit);
    return nullptr;
}

std::byte* find_pointer(const std::vector<Segment>& segs, const void* value) {
    for (const Segment& seg : segs) {
        auto* words = reinterpret_cast<const void* const*>(seg.begin);
        for (size_t i = 0; i < seg.size / sizeof(void*); i++)
            if (words[i] == value)
                return seg.begin + i * sizeof(void*);
    }
    return nullptr;
}

bool in_code(const std::vector<Segment>& segs, const void* p) {
    for (const Segment& seg : segs)
        if (seg.exec && p >= seg.begin && p < seg.begin + seg.size)
            return true;
    return false;
}

}  // namespace

std::span<Mod> all() { return list; }

// Where each HUD sits, as fractions of the screen, stacked down the top-left at first
static const char* const HUDS[] = {"fps", "cps", "keystrokes"};
constexpr int HUD_COUNT = 3;
static ImVec2 hud_rects[HUD_COUNT][2];
static int dragging = -1;

std::atomic<bool> editing_hud{false};

static void declare_positions() {
    for (int i = 0; i < HUD_COUNT; i++) {
        config::declare(std::string(HUDS[i]) + ".x", 0.005f);
        config::declare(std::string(HUDS[i]) + ".y", 0.01f + 0.05f * i);
    }
}

void declare() {
    declare_positions();
    for (const Mod& mod : list) {
        config::declare(mod.key("enabled"), 0);
        config::declare(mod.key("favorite"), 0);
        config::declare(mod.key("hidden"), 0);
        ::declare(mod.settings);
    }
}

void install_zoom() {
    void* game = dlopen("libminecraftpe.so", 0);
    void* api = dlopen("libmcpelauncher_mod.so", 0);
    if (!game || !api)
        return;
    auto patch = reinterpret_cast<void* (*)(void*, void*, size_t)>(dlsym(api, "mcpelauncher_patch"));
    auto* major = static_cast<int*>(dlsym(api, "mcpelauncher_package_version_major"));
    auto* minor = static_cast<int*>(dlsym(api, "mcpelauncher_package_version_minor"));
    auto* patch_v = static_cast<int*>(dlsym(api, "mcpelauncher_package_version_patch"));
    if (!patch || !major || !minor || !patch_v)
        return;

    // CameraAPI's vtable, found through its RTTI: the type name string, the
    // typeinfo that points at it, then the vtable that points at the typeinfo
    std::vector<Segment> segs = segments_of(game);
    static const char name[] = "\0" "9CameraAPI";
    std::byte* found = find_string(segs, name, sizeof name);
    if (!found)
        return;
    const void* type_name = found + 1;
    std::byte* name_ref = find_pointer(segs, type_name);
    if (!name_ref)
        return;
    const void* typeinfo = name_ref - sizeof(void*);
    std::byte* typeinfo_ref = find_pointer(segs, typeinfo);
    if (!typeinfo_ref)
        return;
    auto** vtable = reinterpret_cast<void**>(typeinfo_ref + sizeof(void*));

    // tryGetFOV moved one slot down in 1.21.130
    bool newer = *major > 1 || *minor > 21 || (*minor == 21 && *patch_v >= 130);
    void** slot = vtable + (newer ? 7 : 6);
    if (!in_code(segs, *slot)) {
        __android_log_print(ANDROID_LOG_ERROR, CLIENT_NAME, "Zoom couldn't find the camera on %d.%d.%d", *major, *minor, *patch_v);
        return;
    }
    try_get_fov = reinterpret_cast<decltype(try_get_fov)>(*slot);
    void* hook = reinterpret_cast<void*>(&zoomed_fov);
    patch(slot, &hook, sizeof hook);
}

void on_key(int key, int action, bool playing) {
    if (action != KEY_PRESS && action != KEY_RELEASE)
        return;
    bool down = action == KEY_PRESS;
    if (int slot = key_slot(key); slot >= 0)
        keys[slot] = down;
    if (key == int(config::get("key.zoom")))
        zoom_held = down && playing && enabled("zoom");
}

void on_mouse_button(int button, int action) {
    int index = button - 1;
    if (index < 0 || index > 1)
        return;
    bool down = action == BUTTON_PRESS;
    buttons[index] = down;
    if (down) {
        std::lock_guard lock(clicks_lock);
        clicks[index].push_back(Clock::now());
    }
}

bool on_scroll(double dy, bool playing) {
    if (!zoom_held || !playing || !config::on("zoom.scroll"))
        return false;
    // each notch zooms by a tenth, kept to the slider's range and steps
    float f = config::get("zoom.factor") * (dy > 0 ? 1.1f : 1 / 1.1f);
    config::set("zoom.factor", std::clamp(std::round(f * 2) / 2, 1.5f, 50.f));
    return true;
}

void tick(float dt) {
    frames++;
    fps_window += dt;
    if (fps_window >= 1) {
        fps = int(std::lround(frames / fps_window));
        frames = 0;
        fps_window = 0;
    }
    auto cutoff = Clock::now() - std::chrono::seconds(1);
    {
        std::lock_guard lock(clicks_lock);
        for (int i = 0; i < 2; i++) {
            while (!clicks[i].empty() && clicks[i].front() < cutoff)
                clicks[i].pop_front();
            cps[i] = clicks[i].size();
        }
    }

    float target = zoom_held ? 1 / config::get("zoom.factor") : 1;
    zoom_now = config::on("zoom.smooth") ? zoom_now + (target - zoom_now) * (1 - std::exp(-dt * 12)) : target;
}

bool hud_visible() { return enabled("keystrokes") || enabled("fps") || enabled("cps"); }

// In the editor every HUD shows; the ones that are off are drawn faded
static void draw_huds(ImDrawList* dl, float s, ImFont* text, ImFont* small, bool all) {
    const ImU32 fg = IM_COL32(255, 255, 255, 255);
    ImVec2 display = ImGui::GetIO().DisplaySize;
    float x = 0, y = 0;
    // moves to a HUD's spot and remembers the box it ends up taking, for the editor
    auto place = [&](int i) {
        x = std::round(config::get(std::string(HUDS[i]) + ".x") * display.x);
        y = std::round(config::get(std::string(HUDS[i]) + ".y") * display.y);
        hud_rects[i][0] = ImVec2(x, y);
    };
    auto label = [&](int i, const char* str, float scale, bool background) {
        place(i);
        float size = text->FontSize * scale, pad = 6 * s * scale;
        ImVec2 box = text->CalcTextSizeA(size, FLT_MAX, 0, str);
        if (background)
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + box.x + pad * 2, y + box.y + pad), IM_COL32(0, 0, 0, 120), 6 * s * scale);
        dl->AddText(text, size, ImVec2(std::round(x + pad), std::round(y + pad / 2)), fg, str);
        hud_rects[i][1] = ImVec2(x + box.x + pad * 2, y + box.y + pad);
    };
    for (auto& rect : hud_rects)
        rect[0] = rect[1] = ImVec2(-1, -1);
    int start = 0;
    auto begin = [&] { start = dl->VtxBuffer.Size; };
    auto end = [&](const char* id) {
        if (enabled(id))
            return;
        for (int v = start; v < dl->VtxBuffer.Size; v++) {
            ImU32& c = dl->VtxBuffer[v].col;
            c = (c & ~IM_COL32_A_MASK) | (ImU32((c >> IM_COL32_A_SHIFT & 0xFF) * 0.35f) << IM_COL32_A_SHIFT);
        }
    };

    char buf[64];
    if (all || enabled("fps")) {
        begin();
        const char* formats[] = {"%d FPS", "FPS: %d", "%d"};
        std::snprintf(buf, sizeof buf, formats[std::clamp(int(config::get("fps.style")), 0, 2)], fps);
        label(0, buf, config::get("fps.scale") / 100, config::on("fps.background"));
        end("fps");
    }
    if (all || enabled("cps")) {
        begin();
        if (config::on("cps.right"))
            std::snprintf(buf, sizeof buf, "%zu | %zu CPS", cps[0], cps[1]);
        else
            std::snprintf(buf, sizeof buf, "%zu CPS", cps[0]);
        label(1, buf, config::get("cps.scale") / 100, config::on("cps.background"));
        end("cps");
    }
    if (all || enabled("keystrokes")) {
        begin();
        place(2);
        float scale = config::get("keystrokes.scale") / 100;
        ImU32 bg = IM_COL32(0, 0, 0, int(config::get("keystrokes.background") / 100 * 255));
        float k = 30 * s * scale, gap = 3 * s * scale, r = 6 * s * scale, font = small->FontSize * scale;
        auto key = [&](float kx, float ky, float w, float h, bool down, const char* str) {
            ImVec2 a(x + kx, y + ky), b(a.x + w, a.y + h);
            dl->AddRectFilled(a, b, down ? IM_COL32(255, 255, 255, 200) : bg, r);
            if (!*str)
                return;
            ImVec2 size = small->CalcTextSizeA(font, FLT_MAX, 0, str);
            dl->AddText(small, font, ImVec2(std::round((a.x + b.x - size.x) / 2), std::round((a.y + b.y - size.y) / 2)),
                        down ? IM_COL32(0, 0, 0, 255) : fg, str);
        };
        float row = k + gap, wide = (3 * k + gap) / 2, y_next = 2 * row;
        key(row, 0, k, k, keys[0], "W");
        key(0, row, k, k, keys[1], "A");
        key(row, row, k, k, keys[2], "S");
        key(2 * row, row, k, k, keys[3], "D");
        if (config::on("keystrokes.mouse")) {
            key(0, y_next, wide, k, buttons[0], "LMB");
            key(wide + gap, y_next, wide, k, buttons[1], "RMB");
            y_next += row;
        }
        if (config::on("keystrokes.space")) {
            key(0, y_next, 3 * k + 2 * gap, k / 2, keys[4], "");
            y_next += k / 2 + gap;
        }
        hud_rects[2][1] = ImVec2(x + 3 * k + 2 * gap, y + y_next - gap);
        end("keystrokes");
    }
}

void draw_hud(ImDrawList* dl, float s, ImFont* text, ImFont* small) { draw_huds(dl, s, text, small, false); }

void edit_hud(ImDrawList* dl, float s, ImFont* text, ImFont* small, ImU32 accent) {
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 display = io.DisplaySize;
    dl->AddRectFilled(ImVec2(0, 0), display, IM_COL32(0, 0, 0, 110));
    draw_huds(dl, s, text, small, true);

    ImVec2 m = io.MousePos;
    int hovered = -1;
    for (int i = 0; i < HUD_COUNT; i++) {
        ImVec2 a = hud_rects[i][0], b = hud_rects[i][1];
        if (a.x < 0)
            continue;
        if (m.x >= a.x && m.x < b.x && m.y >= a.y && m.y < b.y)
            hovered = i;
    }
    static ImVec2 pressed_at;
    static bool moved = false;
    // a corner handle resizes instead of moving, with the opposite corner held in place
    static bool resizing = false;
    static int corner = 0;
    static float start_scale = 100, start_w = 1, start_h = 1;
    static ImVec2 anchor;
    auto handle_at = [&](int i) {
        ImVec2 a = hud_rects[i][0], b = hud_rects[i][1];
        float pad = 3 * s, grab = 8 * s;
        ImVec2 cs[] = {{a.x - pad, a.y - pad}, {b.x + pad, a.y - pad}, {a.x - pad, b.y + pad}, {b.x + pad, b.y + pad}};
        for (int c = 0; c < 4; c++)
            if (std::abs(m.x - cs[c].x) < grab && std::abs(m.y - cs[c].y) < grab)
                return c;
        return -1;
    };
    if (io.MouseClicked[0]) {
        resizing = false;
        for (int i = 0; i < HUD_COUNT && hovered < 0; i++)
            if (hud_rects[i][0].x >= 0 && handle_at(i) >= 0)
                hovered = i;
        dragging = hovered;
        pressed_at = m;
        moved = false;
        if (dragging >= 0 && (corner = handle_at(dragging)) >= 0) {
            resizing = true;
            ImVec2 a = hud_rects[dragging][0], b = hud_rects[dragging][1];
            start_scale = config::get(std::string(HUDS[dragging]) + ".scale");
            start_w = std::max(b.x - a.x, 1.f);
            start_h = std::max(b.y - a.y, 1.f);
            anchor = ImVec2(corner & 1 ? a.x : b.x, corner & 2 ? a.y : b.y);
        }
    }
    if (dragging >= 0 && std::abs(m.x - pressed_at.x) + std::abs(m.y - pressed_at.y) > 4 * s)
        moved = true;
    if (dragging >= 0 && io.MouseDown[0] && resizing) {
        moved = true;
        float w = std::abs(m.x - anchor.x);
        float scale = std::clamp(std::round(start_scale * w / start_w / 5) * 5, 50.f, 200.f);
        config::set(std::string(HUDS[dragging]) + ".scale", scale);
        // keep the far corner where it was, so the HUD grows toward the cursor
        float nw = start_w * scale / start_scale, nh = start_h * scale / start_scale;
        float nx = corner & 1 ? anchor.x : anchor.x - nw, ny = corner & 2 ? anchor.y : anchor.y - nh;
        config::set(std::string(HUDS[dragging]) + ".x", std::clamp(nx, 0.f, display.x) / display.x);
        config::set(std::string(HUDS[dragging]) + ".y", std::clamp(ny, 0.f, display.y) / display.y);
    } else if (dragging >= 0 && io.MouseDown[0]) {
        // kept fully on screen, saved as fractions so it stays put across window sizes
        ImVec2 a = hud_rects[dragging][0], b = hud_rects[dragging][1];
        float nx = std::clamp(a.x + io.MouseDelta.x, 0.f, display.x - (b.x - a.x));
        float ny = std::clamp(a.y + io.MouseDelta.y, 0.f, display.y - (b.y - a.y));
        config::set(std::string(HUDS[dragging]) + ".x", nx / display.x);
        config::set(std::string(HUDS[dragging]) + ".y", ny / display.y);
    } else if (dragging >= 0) {
        // a click that didn't drag turns the HUD on or off
        if (!moved) {
            std::string key = std::string(HUDS[dragging]) + ".enabled";
            config::set(key, config::on(key) ? 0 : 1);
        }
        dragging = -1;
        config::save();
    }

    for (int i = 0; i < HUD_COUNT; i++) {
        ImVec2 a = hud_rects[i][0], b = hud_rects[i][1];
        if (a.x < 0)
            continue;
        bool active = i == dragging || (dragging < 0 && i == hovered);
        float pad = 3 * s;
        ImVec2 o(a.x - pad, a.y - pad), p(b.x + pad, b.y + pad);
        if (!active) {
            dl->AddRect(o, p, IM_COL32(255, 255, 255, 70), 0, 0, s);
            continue;
        }
        // OneConfig's selection: a thin accent box, square handles, the size above it
        dl->AddRectFilled(o, p, (accent & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 30));
        dl->AddRect(o, p, accent, 0, 0, s);
        float h = 3 * s;
        for (ImVec2 c : {o, ImVec2(p.x, o.y), ImVec2(o.x, p.y), p})
            dl->AddRectFilled(ImVec2(c.x - h, c.y - h), ImVec2(c.x + h, c.y + h), IM_COL32(255, 255, 255, 255));
        char dims[32];
        std::snprintf(dims, sizeof dims, "%d x %d", int((b.x - a.x) / s), int((b.y - a.y) / s));
        float fs = small->FontSize;
        ImVec2 ts = small->CalcTextSizeA(fs, FLT_MAX, 0, dims);
        ImVec2 l(o.x + (p.x - o.x - ts.x) / 2 - 6 * s, o.y - ts.y - 12 * s);
        if (l.y < 0)
            l.y = p.y + 6 * s;
        dl->AddRectFilled(l, ImVec2(l.x + ts.x + 12 * s, l.y + ts.y + 6 * s), accent, 4 * s);
        dl->AddText(small, fs, ImVec2(std::round(l.x + 6 * s), std::round(l.y + 3 * s)), IM_COL32(255, 255, 255, 255), dims);
    }

    const char* hint = "Drag a HUD to move it, click to turn it on or off. Esc goes back.";
    ImVec2 size = text->CalcTextSizeA(text->FontSize, FLT_MAX, 0, hint);
    ImVec2 at(std::round((display.x - size.x) / 2), std::round(display.y * 0.08f));
    dl->AddRectFilled(ImVec2(at.x - 14 * s, at.y - 8 * s), ImVec2(at.x + size.x + 14 * s, at.y + size.y + 8 * s), IM_COL32(17, 20, 28, 230), 10 * s);
    dl->AddText(text, text->FontSize, at, IM_COL32(255, 255, 255, 255), hint);
}

}  // namespace mods
