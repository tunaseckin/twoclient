#include "icons.h"

#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "assets.h"
#include "gl.h"

#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg.h"
#include "nanosvgrast.h"

#define ICON(name, file) EMBED(icon_##name, ASSET_DIR "/ico/" file ".svg")
ICON(bell, "bell")
ICON(logo, "logo")
ICON(close, "close")
ICON(cog, "cog")
ICON(eye_off, "eye-off")
ICON(eye, "eye")
ICON(hud, "hud")
ICON(keyboard, "keyboard")
ICON(left_arrow, "left-arrow")
ICON(lightning, "lightning-01")
ICON(paintbrush, "paintbrush")
ICON(profiles, "profiles")
ICON(qol, "qol")
ICON(refresh, "refresh")
ICON(right_arrow, "right-arrow")
ICON(search, "search")
ICON(settings, "settings")
ICON(star_filled, "star-filled")
ICON(star, "star")
ICON(up, "up")
ICON(alert_circle, "alert-circle")
ICON(down, "down")
ICON(sun, "sun")
ICON(moon, "moon")
ICON(plus, "plus")
ICON(trash, "trash")
ICON(copy, "copy")
ICON(tick, "tick")
ICON(text_input, "text-input")
ICON(external_link, "external-link")
ICON(info, "info")

namespace icons {
namespace {

const std::map<std::string, Asset> files = {
        {"bell", ASSET(icon_bell)},
        {"logo", ASSET(icon_logo)},
        {"close", ASSET(icon_close)},
        {"cog", ASSET(icon_cog)},
        {"eye-off", ASSET(icon_eye_off)},
        {"eye", ASSET(icon_eye)},
        {"hud", ASSET(icon_hud)},
        {"keyboard", ASSET(icon_keyboard)},
        {"left-arrow", ASSET(icon_left_arrow)},
        {"lightning-01", ASSET(icon_lightning)},
        {"paintbrush", ASSET(icon_paintbrush)},
        {"profiles", ASSET(icon_profiles)},
        {"qol", ASSET(icon_qol)},
        {"refresh", ASSET(icon_refresh)},
        {"right-arrow", ASSET(icon_right_arrow)},
        {"search", ASSET(icon_search)},
        {"settings", ASSET(icon_settings)},
        {"star-filled", ASSET(icon_star_filled)},
        {"star", ASSET(icon_star)},
        {"up", ASSET(icon_up)},
        {"alert-circle", ASSET(icon_alert_circle)},
        {"down", ASSET(icon_down)},
        {"sun", ASSET(icon_sun)},
        {"moon", ASSET(icon_moon)},
        {"plus", ASSET(icon_plus)},
        {"trash", ASSET(icon_trash)},
        {"copy", ASSET(icon_copy)},
        {"tick", ASSET(icon_tick)},
        {"text-input", ASSET(icon_text_input)},
        {"external-link", ASSET(icon_external_link)},
        {"info", ASSET(icon_info)},
};

std::map<std::pair<std::string, int>, GLuint> textures;

GLuint rasterize(const Asset& svg, int px) {
    // nanosvg parses in place, and the embedded copy is read-only and NUL terminated
    std::string text(reinterpret_cast<const char*>(svg.data), svg.size);
    NSVGimage* image = nsvgParse(text.data(), "px", 96);
    if (!image)
        return 0;
    float scale = px / std::max(image->width, image->height);
    std::vector<unsigned char> pixels(size_t(px) * px * 4);
    NSVGrasterizer* rast = nsvgCreateRasterizer();
    nsvgRasterize(rast, image, 0, 0, scale, pixels.data(), px, px, px * 4);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(image);
    // keep the shape, drop the SVG's own color so the draw color tints it
    for (size_t i = 0; i < pixels.size(); i += 4)
        pixels[i] = pixels[i + 1] = pixels[i + 2] = 255;

    // the game's texture binding and unpack alignment have to survive this
    GLint bound = 0, alignment = 4;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, px, px, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
    glBindTexture(GL_TEXTURE_2D, GLuint(bound));
    return tex;
}

}  // namespace

namespace {

GLuint texture(const char* name, int size) {
    auto file = files.find(name);
    if (file == files.end())
        return 0;
    auto [it, fresh] = textures.try_emplace({name, size}, 0);
    if (fresh)
        it->second = rasterize(file->second, size);
    return it->second;
}

}  // namespace

void draw(ImDrawList* dl, const char* name, ImVec2 pos, float px, ImU32 color) {
    int size = std::max(1, int(std::lround(px)));
    GLuint tex = texture(name, size);
    if (!tex)
        return;
    ImVec2 p(std::round(pos.x), std::round(pos.y));
    dl->AddImage(ImTextureID(intptr_t(tex)), p, ImVec2(p.x + size, p.y + size), ImVec2(0, 0), ImVec2(1, 1), color);
}

void draw_rotated(ImDrawList* dl, const char* name, ImVec2 pos, float px, ImU32 color, float degrees) {
    int size = std::max(1, int(std::lround(px)));
    GLuint tex = texture(name, size);
    if (!tex)
        return;
    float r = degrees * 3.14159265f / 180, c = std::cos(r), s = std::sin(r), h = size / 2.f;
    ImVec2 mid(pos.x + h, pos.y + h);
    auto corner = [&](float x, float y) { return ImVec2(mid.x + x * c - y * s, mid.y + x * s + y * c); };
    dl->AddImageQuad(ImTextureID(intptr_t(tex)), corner(-h, -h), corner(h, -h), corner(h, h), corner(-h, h), ImVec2(0, 0),
                     ImVec2(1, 0), ImVec2(1, 1), ImVec2(0, 1), color);
}

}  // namespace icons
