#include "sound.h"

#include <android/log.h>
#include <dlfcn.h>


#include "assets.h"
#include "config.h"

EMBED(sound_open, ASSET_DIR "/sounds/open.ogg")
EMBED(sound_close, ASSET_DIR "/sounds/close.ogg")
EMBED(sound_click, ASSET_DIR "/sounds/click.ogg")
EMBED(sound_pad_loop, ASSET_DIR "/sounds/pad_loop.ogg")

namespace sound {
namespace {

// FMOD 2.02's C API, the version the game links against
struct ExInfo {
    int cbsize;
    unsigned length, fileoffset;
    int numchannels, defaultfrequency, format;
    unsigned decodebuffersize;
    int initialsubsound, numsubsounds;
    int* inclusionlist;
    int inclusionlistnum;
    void *pcmreadcallback, *pcmsetposcallback, *nonblockcallback;
    const char *dlsname, *encryptionkey;
    int maxpolyphony;
    void* userdata;
    int suggestedsoundtype;
    void *fileuseropen, *fileuserclose, *fileuserread, *fileuserseek, *fileuserasyncread, *fileuserasynccancel, *fileuserdata;
    int filebuffersize, channelorder;
    void* initialsoundgroup;
    unsigned initialseekposition;
    int initialseekpostype, ignoresetfilesystem;
    unsigned audioqueuepolicy, minmidigranularity;
    int nonblockthreadid;
    void* fsbguid;
};

constexpr unsigned FMOD_VERSION = 0x00020200;
constexpr unsigned LOOP_OFF = 0x1, LOOP_NORMAL = 0x2, MODE_2D = 0x8, CREATE_STREAM = 0x80, CREATE_SAMPLE = 0x100,
                   OPEN_MEMORY = 0x800, OPEN_MEMORY_POINT = 0x10000000;

int (*System_Create)(void**, unsigned);
int (*System_Init)(void*, int, unsigned, void*);
int (*System_CreateSound)(void*, const void*, unsigned, ExInfo*, void**);
int (*System_PlaySound)(void*, void*, void*, int, void**);
int (*System_Update)(void*);
int (*Channel_SetVolume)(void*, float);
int (*Channel_Stop)(void*);
int (*Channel_IsPlaying)(void*, int*);
int (*Channel_SetPaused)(void*, int);

// OneConfig's volumes: effects at full, the Modern ambience at half
constexpr float AMBIENCE_VOLUME = 0.5f;

// OneConfig's sound preferences: a master switch, one per kind, and two volumes
bool allowed(Effect effect) {
    if (!config::on("pref.ui_sounds"))
        return false;
    return effect == Effect::Click ? config::on("pref.click_sounds") : config::on("pref.menu_sounds");
}

float ambience_volume() { return AMBIENCE_VOLUME * config::get("pref.ambience_volume"); }
constexpr float FADE_SECONDS = 4;

void* fmod = nullptr;
void* effects[3] = {};
void* pad = nullptr;
void* pad_channel = nullptr;
float pad_volume = 0;
bool fading = false;
bool tried = false;

void* load(Asset asset, unsigned mode) {
    ExInfo info{};
    info.cbsize = sizeof info;
    info.length = unsigned(asset.size);
    void* sound = nullptr;
    if (System_CreateSound(fmod, asset.data, mode | OPEN_MEMORY | MODE_2D, &info, &sound) != 0)
        return nullptr;
    return sound;
}

bool ready() {
    if (tried)
        return fmod;
    tried = true;
    void* lib = dlopen("libfmod.so", RTLD_NOW);
    if (!lib)
        return false;
    System_Create = reinterpret_cast<decltype(System_Create)>(dlsym(lib, "FMOD_System_Create"));
    System_Init = reinterpret_cast<decltype(System_Init)>(dlsym(lib, "FMOD_System_Init"));
    System_CreateSound = reinterpret_cast<decltype(System_CreateSound)>(dlsym(lib, "FMOD_System_CreateSound"));
    System_PlaySound = reinterpret_cast<decltype(System_PlaySound)>(dlsym(lib, "FMOD_System_PlaySound"));
    System_Update = reinterpret_cast<decltype(System_Update)>(dlsym(lib, "FMOD_System_Update"));
    Channel_SetVolume = reinterpret_cast<decltype(Channel_SetVolume)>(dlsym(lib, "FMOD_Channel_SetVolume"));
    Channel_Stop = reinterpret_cast<decltype(Channel_Stop)>(dlsym(lib, "FMOD_Channel_Stop"));
    Channel_IsPlaying = reinterpret_cast<decltype(Channel_IsPlaying)>(dlsym(lib, "FMOD_Channel_IsPlaying"));
    Channel_SetPaused = reinterpret_cast<decltype(Channel_SetPaused)>(dlsym(lib, "FMOD_Channel_SetPaused"));
    if (!System_Create || !System_Init || !System_CreateSound || !System_PlaySound || !System_Update || !Channel_SetVolume ||
        !Channel_Stop || !Channel_IsPlaying || !Channel_SetPaused)
        return false;

    void* system = nullptr;
    if (System_Create(&system, FMOD_VERSION) != 0 || System_Init(system, 32, 0, nullptr) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, CLIENT_NAME, "couldn't start FMOD for the menu sounds");
        return false;
    }
    fmod = system;
    effects[int(Effect::Open)] = load(ASSET(sound_open), CREATE_SAMPLE | LOOP_OFF);
    effects[int(Effect::Close)] = load(ASSET(sound_close), CREATE_SAMPLE | LOOP_OFF);
    effects[int(Effect::Click)] = load(ASSET(sound_click), CREATE_SAMPLE | LOOP_OFF);
    // The pad is a long track, so it streams straight out of the library
    pad = load(ASSET(sound_pad_loop), CREATE_STREAM | OPEN_MEMORY_POINT | LOOP_NORMAL);
    return true;
}

bool playing(void* channel) {
    int on = 0;
    return channel && Channel_IsPlaying(channel, &on) == 0 && on;
}

}  // namespace

void play(Effect effect) {
    if (!allowed(effect) || !ready() || !effects[int(effect)])
        return;
    // started paused so the volume is set before anything is heard
    void* channel = nullptr;
    if (System_PlaySound(fmod, effects[int(effect)], nullptr, 1, &channel) != 0 || !channel)
        return;
    Channel_SetVolume(channel, config::get("pref.sound_volume"));
    Channel_SetPaused(channel, 0);
}

void ambience(bool on) {
    if (!ready() || !pad)
        return;
    if (!on || !config::on("pref.ui_sounds") || !config::on("pref.ambience")) {
        fading = playing(pad_channel);
        return;
    }
    fading = false;
    pad_volume = ambience_volume();
    if (!playing(pad_channel)) {
        pad_channel = nullptr;
        System_PlaySound(fmod, pad, nullptr, 0, &pad_channel);
    }
    if (pad_channel)
        Channel_SetVolume(pad_channel, pad_volume);
}

void update(float dt) {
    if (!fmod)
        return;
    // a volume change while the pad plays takes effect right away
    if (!fading && playing(pad_channel) && pad_volume != ambience_volume()) {
        pad_volume = ambience_volume();
        Channel_SetVolume(pad_channel, pad_volume);
    }
    if (fading && pad_channel) {
        pad_volume -= ambience_volume() * dt / FADE_SECONDS;
        if (pad_volume <= 0) {
            Channel_Stop(pad_channel);
            pad_channel = nullptr;
            fading = false;
        } else {
            Channel_SetVolume(pad_channel, pad_volume);
        }
    }
    System_Update(fmod);
}

}  // namespace sound
