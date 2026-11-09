#include "quickplay.h"

#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "assets.h"
#include "gl.h"
#include "imgui_impl_opengl3.h"

#include <EGL/egl.h>
#include "icons.h"
#include "title.h"
#include "ui.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include "stb_image.h"

EMBED(steve_png, ASSET_DIR "/steve.png")

namespace fs = std::filesystem;

namespace quickplay {
namespace {

struct Texture {
    GLuint id = 0;
    int w = 0, h = 0;
};

Texture upload(const unsigned char* data, size_t size) {
    Texture t;
    int n = 0;
    unsigned char* px = stbi_load_from_memory(data, int(size), &t.w, &t.h, &n, 4);
    if (!px)
        return t;
    GLint bound = 0, alignment = 4;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
    glGenTextures(1, &t.id);
    glBindTexture(GL_TEXTURE_2D, t.id);
    // skins stay crisp, world pictures are smoothed
    GLint filter = t.w <= 64 ? GL_NEAREST : GL_LINEAR;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, t.w, t.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
    glBindTexture(GL_TEXTURE_2D, GLuint(bound));
    stbi_image_free(px);
    return t;
}

Texture upload_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)), {});
    return bytes.empty() ? Texture{} : upload(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
}

// The game's com.mojang folder: TwoClient passes an instance's, otherwise mcpelauncher's default
fs::path game_dir() {
    if (const char* dir = std::getenv("TWOCLIENT_GAME_DIR"))
        return dir;
    const char* home = std::getenv("HOME");
    if (!home)
        return {};
    // built for Android either way, so look for macOS's folder before Linux's
    fs::path mac = fs::path(home) / "Library/Application Support/mcpelauncher/games/com.mojang";
    std::error_code ec;
    return fs::exists(mac, ec) ? mac : fs::path(home) / ".local/share/mcpelauncher/games/com.mojang";
}

struct Entry {
    std::string name, subtitle;
    fs::path icon;
    long long when = 0;
    Texture texture;
    bool loaded = false;
};

std::vector<Entry> entries;
bool folded = false;
float open_amount = 1;  // eases toward folded or not

bool within(ImVec2 p, float x0, float y0, float x1, float y1, ImVec2 d) {
    return p.x >= x0 * d.x && p.x < x1 * d.x && p.y >= y0 * d.y && p.y < y1 * d.y;
}
std::chrono::steady_clock::time_point last_scan;

std::string read_line(const fs::path& path) {
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    return line;
}

void scan() {
    for (Entry& e : entries)
        if (e.texture.id)
            glDeleteTextures(1, &e.texture.id);
    entries.clear();
    fs::path root = game_dir();
    std::error_code ec;
    for (const auto& world : fs::directory_iterator(root / "minecraftWorlds", ec)) {
        struct stat st {};
        if (stat((world.path() / "level.dat").c_str(), &st) != 0)
            continue;
        entries.push_back({read_line(world.path() / "levelname.txt"), "Singleplayer", world.path() / "world_icon.jpeg",
                           (long long)st.st_mtime});
    }
    // external_servers.txt: index:name:address:port:last played, newest last
    std::ifstream servers(root / "minecraftpe/external_servers.txt");
    for (std::string line; std::getline(servers, line);) {
        std::vector<std::string> f;
        for (size_t at = 0, next; at <= line.size(); at = next + 1) {
            next = line.find(':', at);
            if (next == std::string::npos)
                next = line.size();
            f.push_back(line.substr(at, next - at));
        }
        if (f.size() >= 4)
            entries.push_back({f[1], "Multiplayer · " + f[2], {}, f.size() >= 5 ? std::atoll(f[4].c_str()) : 0});
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.when > b.when; });
    if (entries.size() > 3)
        entries.resize(3);
}

}  // namespace

bool owns(ImVec2 p, ImVec2 d) {
    return within(p, 0.035f, 0.35f, 0.275f, 0.405f, d) || (folded && within(p, 0.035f, 0.405f, 0.275f, 0.72f, d));
}

void draw_list(ImDrawList* dl, ImVec2 d, float s, ImVec2 mouse, bool clicked) {
    auto now = std::chrono::steady_clock::now();
    if (now - last_scan > std::chrono::seconds(5)) {
        scan();
        last_scan = now;
    }
    ImFont* font = ui::fonts.m16;
    ImFont* small = ui::fonts.r14;
    float x0 = std::round(0.035f * d.x), x1 = std::round(0.275f * d.x);
    float text = d.y * 0.026f, r = 5 * s;
    auto fade = [](ImU32 c, float a) { return (c & ~IM_COL32_A_MASK) | (ImU32(((c >> IM_COL32_A_SHIFT) & 0xFF) * a) << IM_COL32_A_SHIFT); };
    auto box = [&](float y, float h, float alpha, bool hoverable) {
        ImVec2 a(x0, std::round(y * d.y)), b(x1, std::round((y + h) * d.y));
        bool hover = hoverable && mouse.x >= a.x && mouse.x < b.x && mouse.y >= a.y && mouse.y < b.y;
        title::Look l = title::look(hover);
        dl->AddRectFilled(a, b, fade(l.fill, alpha), r);
        dl->AddRect(a, b, fade(l.border, alpha), r, 0, std::max(1.f, s));
        return std::pair(a, b);
    };
    title::Look l = title::look(false);
    ImU32 fg = l.text, dim = l.dim;

    // the chevron folds the list away, easing like OneClient's dropdown
    if (clicked && within(mouse, 0.035f, 0.35f, 0.275f, 0.405f, d) && mouse.x > x1 - text * 3)
        folded = !folded;
    open_amount += ((folded ? 0.f : 1.f) - open_amount) * std::min(1.f, ImGui::GetIO().DeltaTime * 14);

    // the header row, like OneClient's Quickplay dropdown
    auto [ha, hb] = box(0.35f, 0.055f, 1, true);
    float icon = text * 1.1f, mid = (ha.y + hb.y) / 2;
    icons::draw(dl, "right-arrow", ImVec2(ha.x + text, std::round(mid - icon / 2)), icon, fg);
    ImVec2 ts = font->CalcTextSizeA(text, FLT_MAX, 0, "Quickplay");
    dl->AddText(font, text, ImVec2(std::round((ha.x + hb.x - ts.x) / 2), std::round(mid - ts.y / 2)), fg, "Quickplay");
    icons::draw_rotated(dl, "down", ImVec2(hb.x - text - icon * 0.8f, std::round(mid - icon * 0.4f)), icon * 0.8f, fg,
                        (1 - open_amount) * -90);

    float y = 0.425f;
    for (size_t i = 0; i < entries.size(); i++) {
        Entry& e = entries[i];
        // rows slide up under the header and fade, the lower ones first
        float k = std::clamp(open_amount * 1.6f - i * 0.3f, 0.f, 1.f);
        if (k <= 0.01f) {
            y += 0.095f;
            continue;
        }
        float slide = (1 - k) * (y - 0.35f);
        auto [a, b] = box(y - slide, 0.08f, k, !folded);
        fg = fade(l.text, k);
        dim = fade(l.dim, k);
        float pic = (b.y - a.y) - 2 * text * 0.5f;
        ImVec2 pa(a.x + text * 0.6f, a.y + text * 0.5f), pb(pa.x + pic, pa.y + pic);
        if (!e.loaded) {
            e.loaded = true;
            if (!e.icon.empty())
                e.texture = upload_file(e.icon);
        }
        if (e.texture.id)
            dl->AddImageRounded(ImTextureID(e.texture.id), pa, pb, ImVec2(0, 0), ImVec2(1, 1), fade(IM_COL32_WHITE, k), 3 * s);
        else
            dl->AddRectFilled(pa, pb, fade(IM_COL32(60, 64, 80, 200), k), 3 * s);
        icons::draw(dl, "right-arrow", ImVec2(b.x - text * 1.6f, std::round((a.y + b.y) / 2 - text * 0.35f)), text * 0.7f, dim);
        float tx = pb.x + text * 0.8f;
        dl->AddText(font, text, ImVec2(tx, std::round(a.y + (b.y - a.y) * 0.18f)), fg, e.name.c_str());
        dl->AddText(small, text * 0.85f, ImVec2(tx, std::round(a.y + (b.y - a.y) * 0.55f)), dim, e.subtitle.c_str());
        y += 0.095f;
    }
}

namespace {

Texture skin;
bool skin_loaded = false;

struct Face {
    std::array<ImVec2, 4> p;
    std::array<ImVec2, 4> uv;
    std::array<float, 4> y;  // model height of each corner, for the fade
    float depth, light;
};

}  // namespace

namespace {

// Renders a draw list into a texture of its own and returns it
GLuint render_offscreen(ImDrawList& list, int w, int h) {
    using GenFb = void (*)(int, GLuint*);
    using BindFb = void (*)(unsigned, GLuint);
    using FbTex = void (*)(unsigned, unsigned, unsigned, GLuint, int);
    using Clear = void (*)(unsigned);
    using ClearColor = void (*)(float, float, float, float);
    using GetFloat = void (*)(unsigned, float*);
    static auto gen = reinterpret_cast<GenFb>(eglGetProcAddress("glGenFramebuffers"));
    static auto bind = reinterpret_cast<BindFb>(eglGetProcAddress("glBindFramebuffer"));
    static auto attach = reinterpret_cast<FbTex>(eglGetProcAddress("glFramebufferTexture2D"));
    static auto clear = reinterpret_cast<Clear>(eglGetProcAddress("glClear"));
    static auto clear_color = reinterpret_cast<ClearColor>(eglGetProcAddress("glClearColor"));
    static auto get_float = reinterpret_cast<GetFloat>(eglGetProcAddress("glGetFloatv"));
    if (!gen || !bind || !attach || !clear || !clear_color || !get_float || w <= 0 || h <= 0)
        return 0;
    static GLuint fbo = 0, tex = 0;
    static int size_w = 0, size_h = 0;
    GLint bound_fb = 0, bound_tex = 0;
    glGetIntegerv(0x8CA6, &bound_fb);  // GL_FRAMEBUFFER_BINDING
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound_tex);
    if (!fbo)
        gen(1, &fbo);
    if (!tex || size_w != w || size_h != h) {
        if (tex)
            glDeleteTextures(1, &tex);
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindTexture(GL_TEXTURE_2D, GLuint(bound_tex));
        size_w = w, size_h = h;
        bind(0x8D40, fbo);  // GL_FRAMEBUFFER
        attach(0x8D40, 0x8CE0, GL_TEXTURE_2D, tex, 0);  // GL_COLOR_ATTACHMENT0
    }
    bind(0x8D40, fbo);

    ImDrawData data;
    data.Valid = true;
    data.DisplayPos = ImVec2(0, 0);
    data.DisplaySize = ImVec2(float(w), float(h));
    data.FramebufferScale = ImVec2(1, 1);
    data.AddDrawList(&list);
    // the game keeps alpha writes off, which would leave the picture see-through
    using Mask = void (*)(unsigned char, unsigned char, unsigned char, unsigned char);
    using GetBool = void (*)(unsigned, unsigned char*);
    static auto mask = reinterpret_cast<Mask>(eglGetProcAddress("glColorMask"));
    static auto get_bool = reinterpret_cast<GetBool>(eglGetProcAddress("glGetBooleanv"));
    unsigned char writes[4] = {1, 1, 1, 1};
    if (get_bool)
        get_bool(0x0C23, writes);  // GL_COLOR_WRITEMASK
    if (mask)
        mask(1, 1, 1, 1);
    float was[4];
    get_float(0x0C22, was);  // GL_COLOR_CLEAR_VALUE
    clear_color(0, 0, 0, 0);
    clear(0x4000);  // GL_COLOR_BUFFER_BIT
    clear_color(was[0], was[1], was[2], was[3]);
    ImGui_ImplOpenGL3_RenderDrawData(&data);
    if (mask)
        mask(writes[0], writes[1], writes[2], writes[3]);
    bind(0x8D40, GLuint(bound_fb));
    return tex;
}

}  // namespace

void draw_skin(ImDrawList* dl, ImVec2 bottom, float height, float time) {
    if (!skin_loaded) {
        skin_loaded = true;
        // TwoClient leaves the player's skin next to the game's files; Steve otherwise
        if (const char* path = std::getenv("TWOCLIENT_SKIN"))
            skin = upload_file(path);
        if (!skin.id)
            skin = upload_file(game_dir() / "twoclient_skin.png");
        if (!skin.id)
            skin = upload(steve_png, size_t(steve_png_end - steve_png));
    }
    if (!skin.id)
        return;

    // turning slowly all the way round, seen straight on; OneClient shows the head
    // and shoulders, fading out down the chest
    float yaw = -time * 0.55f, pitch = 0;
    float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
    constexpr float CUT = 12;  // model height at the bottom of the view
    float unit = height / (33 - CUT);
    // the model is drawn into its own picture first, so it can fade out as a flat
    // image instead of every face turning see-through on its own
    int tw = int(std::ceil(unit * 22)), th = int(std::ceil(height));
    ImVec2 origin(tw / 2.f, float(th));
    auto project = [&](float x, float y, float z, float& depth) {
        float rx = x * cy + z * sy, rz = -x * sy + z * cy;
        float ry = (y - CUT) * cp - rz * sp;
        depth = y * sp + rz * cp;
        return ImVec2(origin.x + rx * unit, origin.y - ry * unit);
    };
    auto turn = [&](float x, float y, float z) {
        float rz = -x * sy + z * cy;
        return y * sp + rz * cp;
    };

    std::vector<Face> faces;
    // box at center (x, y, z) of size (w, h, d) in skin pixels, its texture at (u, v)
    auto add_box = [&](float x, float y, float z, float w, float h, float d, float u, float v, float grow = 0) {
        float x0 = x - w / 2 - grow, x1 = x + w / 2 + grow, y0 = y - h / 2 - grow, y1 = y + h / 2 + grow;
        float z0 = z - d / 2 - grow, z1 = z + d / 2 + grow;
        struct Side {
            float c[4][3];
            float u0, v0, u1, v1;
            float n[3];
        } sides[] = {
                {{{x0, y1, z1}, {x1, y1, z1}, {x1, y0, z1}, {x0, y0, z1}}, u + d, v + d, u + d + w, v + d + h, {0, 0, 1}},
                {{{x1, y1, z0}, {x0, y1, z0}, {x0, y0, z0}, {x1, y0, z0}}, u + 2 * d + w, v + d, u + 2 * d + 2 * w, v + d + h, {0, 0, -1}},
                {{{x0, y1, z0}, {x0, y1, z1}, {x0, y0, z1}, {x0, y0, z0}}, u, v + d, u + d, v + d + h, {-1, 0, 0}},
                {{{x1, y1, z1}, {x1, y1, z0}, {x1, y0, z0}, {x1, y0, z1}}, u + d + w, v + d, u + 2 * d + w, v + d + h, {1, 0, 0}},
                {{{x0, y1, z0}, {x1, y1, z0}, {x1, y1, z1}, {x0, y1, z1}}, u + d, v, u + d + w, v + d, {0, 1, 0}},
                {{{x0, y0, z1}, {x1, y0, z1}, {x1, y0, z0}, {x0, y0, z0}}, u + d + w, v, u + d + 2 * w, v + d, {0, -1, 0}},
        };
        for (const Side& sd : sides) {
            if (turn(sd.n[0], sd.n[1], sd.n[2]) <= 0.01f)
                continue;
            Face f{};
            float sum = 0, dep;
            for (int i = 0; i < 4; i++) {
                f.p[i] = project(sd.c[i][0], sd.c[i][1], sd.c[i][2], dep);
                f.y[i] = sd.c[i][1];
                sum += dep;
            }
            float tw = float(skin.w), th = float(skin.h);
            f.uv = {ImVec2(sd.u0 / tw, sd.v0 / th), ImVec2(sd.u1 / tw, sd.v0 / th), ImVec2(sd.u1 / tw, sd.v1 / th),
                    ImVec2(sd.u0 / tw, sd.v1 / th)};
            f.depth = sum / 4;
            f.light = std::abs(sd.n[0]) > 0 ? 0.82f : 1.f;
            faces.push_back(f);
        }
    };
    bool wide = skin.h >= 64;
    // the outer layer (hats, glasses, jackets) sits half a pixel out from the base
    auto layer = [&](float x, float y, float z, float w, float h, float d, float u, float v) {
        add_box(x, y, z, w, h, d, u, v, 0.5f);
    };
    add_box(0, 28, 0, 8, 8, 8, 0, 0);    // head
    add_box(0, 18, 0, 8, 12, 4, 16, 16);  // body
    add_box(-6, 18, 0, 4, 12, 4, 40, 16);  // right arm
    add_box(6, 18, 0, 4, 12, 4, wide ? 32 : 40, wide ? 48 : 16);  // left arm
    layer(0, 28, 0, 8, 8, 8, 32, 0);
    if (wide) {
        layer(0, 18, 0, 8, 12, 4, 16, 32);
        layer(-6, 18, 0, 4, 12, 4, 40, 32);
        layer(6, 18, 0, 4, 12, 4, 48, 48);
    }
    std::sort(faces.begin(), faces.end(), [](const Face& a, const Face& b) { return a.depth < b.depth; });
    static ImDrawList list(ImGui::GetDrawListSharedData());
    list._ResetForNewFrame();
    list.PushClipRect(ImVec2(0, 0), ImVec2(float(tw), float(th)));
    for (const Face& f : faces) {
        int l = int(255 * f.light);
        list.AddImageQuad(ImTextureID(skin.id), f.p[0], f.p[1], f.p[2], f.p[3], f.uv[0], f.uv[1], f.uv[2], f.uv[3], IM_COL32(l, l, l, 255));
    }
    list.PopClipRect();
    GLuint picture = render_offscreen(list, tw, th);
    if (!picture)
        return;

    // the picture, solid down to the chest and fading out below like OneClient's
    ImVec2 a(std::round(bottom.x - tw / 2.f), bottom.y - th), z(a.x + tw, bottom.y);
    float split = a.y + th * 0.5f;
    // the texture was drawn bottom-up, so it is read flipped
    float v_split = 1 - (split - a.y) / th;
    dl->AddImage(ImTextureID(picture), a, ImVec2(z.x, split), ImVec2(0, 1), ImVec2(1, v_split));
    dl->PushTextureID(ImTextureID(picture));
    dl->PrimReserve(6, 4);
    dl->PrimRectUV(ImVec2(a.x, split), z, ImVec2(0, v_split), ImVec2(1, 0), IM_COL32_WHITE);
    ImDrawVert* v = dl->_VtxWritePtr - 4;
    v[2].col = v[3].col = IM_COL32(255, 255, 255, 0);
    dl->PopTextureID();
}

}  // namespace quickplay
