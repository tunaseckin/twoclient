#pragma once

// OneConfig's "Modern" UI sounds, played through the FMOD the game already
// loads since the mod has no way into the game's own sound engine
namespace sound {

enum class Effect { Open, Close, Click };

void play(Effect effect);
// The ambient pad loops while the menu is open and fades out over four
// seconds once it closes, like OneConfig's FadingLoopSoundInstance
void ambience(bool on);
void update(float dt);

}  // namespace sound
