#include "menu.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>

#include "gl.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "config.h"
#include "title.h"
#include "ui.h"
#include "mods.h"
#include "shell.h"
#include "sound.h"

namespace menu {
namespace {

// Key codes and actions as the launcher's game window reports them
constexpr int KEY_ESCAPE = 27;
constexpr int KEY_SHIFT = 16;
constexpr int KEY_PRESS = 0;
constexpr int BUTTON_PRESS = 0;

// What the player asked for. Input arrives outside the frame, so the frame
// notices the change and runs the sounds and animation from there.
std::atomic<bool> wanted{false};
bool shown = false;
bool ready = false;
float width = 0, height = 0, scale = 0;
// While the game holds the mouse it only sends relative motion, so the menu
// keeps its own cursor and draws it
float cursor_x = 0, cursor_y = 0;
bool relative_mouse = false;
std::chrono::steady_clock::time_point last_frame;
bool (*mouse_locked)() = nullptr;
const int* menubar_height = nullptr;

// A world is being played with no screen over it, the game holding the mouse
bool playing() { return !mouse_locked || mouse_locked(); }

// The launcher's window coordinates put the game's top edge below its menubar
float window_y(double y) { return float(y) - (menubar_height ? float(*menubar_height) : 0.f); }

// OneConfigInterface's RetainedVisibility: in over animationTime (0.6s) with
// EaseOutExpo, out over at most 160ms with EaseOutCubic, fading with the
// progress and growing from 90% to full size around the middle
constexpr float MAX_CLOSE_SECONDS = 0.16f, HIDDEN_SCALE = 0.9f;
std::atomic<bool> shift_down{false};
// The title screen the pack marks, and a click on one of the buttons the mod owns there
std::atomic<bool> on_title{false};
std::atomic<bool> title_click{false};
float progress = 0;
float tween_from = 0, tween_elapsed = 0;

float ease_out_expo(float t) { return t >= 1 ? 1 : 1 - std::pow(2.f, -10 * t); }
float ease_out_cubic(float t) { return 1 - std::pow(1 - t, 3.f); }

void animate(float dt) {
    tween_elapsed += dt;
    float target = shown ? 1.f : 0.f;
    // the Opening Time preference, the close capped like guiCloseAnimationMillis; turning
    // an animation off makes it one frame
    float time = config::get("pref.opening_time");
    bool on = config::on(shown ? "pref.open_animation" : "pref.close_animation");
    float duration = !on ? 1e-3f : shown ? time : std::min(time, MAX_CLOSE_SECONDS);
    float t = std::min(tween_elapsed / duration, 1.f);
    float eased = shown ? ease_out_expo(t) : ease_out_cubic(t);
    progress = tween_from + (target - tween_from) * eased;
}

bool init() {
    if (!gl::load())
        return false;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ImGui_ImplOpenGL3_Init("#version 300 es");
    ready = true;
    return true;
}

// Applies the open animation to the finished frame, scaling every vertex and
// clip rect around the middle of the screen and fading them with the progress
void transform(ImDrawData* data) {
    // the HUDs and the cursor stay put, only the menu animates
    ImDrawList* fixed[] = {ImGui::GetBackgroundDrawList(), ImGui::GetForegroundDrawList()};
    float s = HIDDEN_SCALE + (1 - HIDDEN_SCALE) * progress;
    ImVec2 c(width / 2, height / 2);
    auto move = [&](float v, float center) { return center + (v - center) * s; };
    for (ImDrawList* list : data->CmdLists) {
        if (list == fixed[0] || list == fixed[1])
            continue;
        for (ImDrawVert& v : list->VtxBuffer) {
            v.pos = ImVec2(move(v.pos.x, c.x), move(v.pos.y, c.y));
            float a = float((v.col >> IM_COL32_A_SHIFT) & 255) * progress;
            v.col = (v.col & ~IM_COL32_A_MASK) | (ImU32(a) << IM_COL32_A_SHIFT);
        }
        for (ImDrawCmd& cmd : list->CmdBuffer)
            cmd.ClipRect = ImVec4(move(cmd.ClipRect.x, c.x), move(cmd.ClipRect.y, c.y), move(cmd.ClipRect.z, c.x),
                                  move(cmd.ClipRect.w, c.y));
    }
}

}  // namespace

void connect(bool (*locked)(), const int* menubar) {
    mouse_locked = locked;
    menubar_height = menubar;
}

bool key(int key, int action) {
    if ((key & 255) == KEY_SHIFT && action != 1)
        shift_down = action == KEY_PRESS;
    mods::on_key(key, action, !wanted && playing());
    // a focused text field or a keybind being recorded gets every key, Escape and the menu key too
    if (wanted && ui::wants_keyboard()) {
        if (action == KEY_PRESS || action == 1)
            ui::push_key(key, shift_down);
        return true;
    }
    if (wanted && (action == KEY_PRESS || action == 1))
        ui::push_key(key, shift_down);
    // Like OneConfig on Java, Right Shift opens the menu in a world and not
    // over other screens; the game only holds the mouse while nothing is open
    if (action == KEY_PRESS && key == int(config::get("key.menu")) && (wanted || playing())) {
        wanted = !wanted;
        mods::editing_hud = false;
        return true;
    }
    if (wanted && action == KEY_PRESS && key == KEY_ESCAPE) {
        // Escape leaves the HUD editor for the menu, like OneConfig's
        if (mods::editing_hud)
            mods::editing_hud = false;
        else
            wanted = false;
        return true;
    }
    return wanted;
}

bool mouse_button(double x, double y, int button, int action) {
    if (!wanted && on_title && button == 1 && !relative_mouse) {
        ImVec2 at(float(x), window_y(y));
        if (title::owns(at, ImVec2(width, height))) {
            if (action == BUTTON_PRESS)
                title_click = true;
            return true;
        }
    }
    if (!wanted)
        mods::on_mouse_button(button, action);
    if (!wanted || !ready)
        return wanted;
    // The launcher numbers buttons from 1, left first
    int index = button - 1;
    if (index >= 0 && index < 3) {
        if (!relative_mouse) {
            cursor_x = float(x);
            cursor_y = window_y(y);
        }
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(cursor_x, cursor_y);
        io.AddMouseButtonEvent(index, action == BUTTON_PRESS);
    }
    return true;
}

bool mouse_move(double x, double y, bool relative) {
    relative_mouse = relative;
    if (!wanted && !relative) {
        // tracked outside the menu too, for the title screen's hover
        cursor_x = float(x);
        cursor_y = window_y(y);
    }
    if (!wanted)
        return false;
    if (relative) {
        cursor_x = std::clamp(cursor_x + float(x), 0.f, width);
        cursor_y = std::clamp(cursor_y + float(y), 0.f, height);
    } else {
        cursor_x = float(x);
        cursor_y = window_y(y);
    }
    if (ready)
        ImGui::GetIO().AddMousePosEvent(cursor_x, cursor_y);
    return true;
}

bool scroll(double dx, double dy) {
    if (!wanted && mods::on_scroll(dy, playing()))
        return true;
    if (wanted && ready)
        ImGui::GetIO().AddMouseWheelEvent(float(dx), float(dy));
    return wanted;
}

void frame(EGLDisplay display, EGLSurface surface) {
    auto now = std::chrono::steady_clock::now();
    float dt = last_frame.time_since_epoch().count() ? std::chrono::duration<float>(now - last_frame).count() : 1 / 60.f;
    dt = std::clamp(dt, 1 / 1000.f, 0.25f);
    last_frame = now;
    sound::update(dt);
    mods::tick(dt);

    bool want = wanted;
    if (want != shown) {
        shown = want;
        tween_from = progress;
        tween_elapsed = 0;
        sound::play(want ? sound::Effect::Open : sound::Effect::Close);
        sound::ambience(want);
        if (want) {
            cursor_x = width / 2;
            cursor_y = height / 2;
        }
    }
    animate(dt);
    bool menu_visible = shown || progress > 0;
    // HUDs belong to a world being played, not the title screen or other menus
    bool huds = mods::hud_visible() && (playing() || menu_visible);
    on_title = !playing() && title::showing();
    if (!menu_visible && !huds && !on_title)
        return;

    EGLint w = 0, h = 0;
    eglQuerySurface(display, surface, EGL_WIDTH, &w);
    eglQuerySurface(display, surface, EGL_HEIGHT, &h);
    if (w <= 0 || h <= 0)
        return;
    width = float(w);
    height = float(h);
    if (!ready && !init()) {
        wanted = false;
        return;
    }

    ImVec2 size(width, height);
    float s = shell::scale_for(size);
    if (s != scale) {
        scale = s;
        shell::load_fonts(scale);
    }

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = size;
    io.DeltaTime = dt;
    io.MouseDrawCursor = relative_mouse && shown;

    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    bool editing = shown && mods::editing_hud;
    bool close = false;
    if (editing)
        mods::edit_hud(ImGui::GetForegroundDrawList(), scale, shell::hud_text(), shell::hud_small(), ui::col(ui::accent()));
    else {
        if (on_title && !shown && title::draw(ImGui::GetBackgroundDrawList(), size, scale, ImVec2(cursor_x, cursor_y), title_click.exchange(false)))
            wanted = true;
        if (huds)
            mods::draw_hud(ImGui::GetBackgroundDrawList(), scale, shell::hud_text(), shell::hud_small());
        close = menu_visible && shell::draw(size, scale);
    }
    ImGui::Render();
    transform(ImGui::GetDrawData());
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    if (close) {
        wanted = false;
        mods::editing_hud = false;
    }
}

}  // namespace menu
