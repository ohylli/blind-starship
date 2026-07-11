#include "SpatialAudioTest.h"

#ifdef HAVE_STEAM_AUDIO

#include "Cue3D.h"

// Events.h transitively pulls in the C game headers (functions.h et al.) that
// declare params named `this`; CGameCompat.h must precede it to rename the
// keyword for C++. Same ordering as AccessibilityCues.cpp.
#include "port/CGameCompat.h"
#include "port/hooks/Events.h"

#include <cmath>
#include <string>

// Smoke test for the Cue3D seam. The enemy cue sound orbits the listener in the
// horizontal plane; on headphones it should sweep front -> right -> behind ->
// left once every 4 s. This validates the whole chain (device, per-source
// binaural effect, mixing, block-size matching) AND that the game's +z-ahead
// convention is what callers pass — the -Z negation lives inside the backend, so
// here θ=0 maps to +z (ahead). See docs/steam-audio-handoff.md "Step 2".

namespace {

// Reuse the enemy cue's asset so the smoke test orbits the real sound callers
// hear in-game. Intentionally duplicated from the enemy-cue registration in
// AccessibilityCues.cpp (there is no shared path constant today); this file is
// throwaway, so a literal is acceptable.
constexpr const char* kEnemyCueWav = "assets/accessibility/enemy.wav";

constexpr float kTwoPi = 6.2831853f;
constexpr float kOrbitHz = 0.25f;  // one circle every 4 s
constexpr float kGameFps = 30.0f;  // SF64 game logic ticks at 30 fps

Cue3DSource* sSource = nullptr;
float sOrbit = 0.0f; // current orbit angle, radians (game-thread only)

void OnPostUpdate(IEvent* event) {
    (void) event;
    if (sSource == nullptr) {
        return;
    }
    sOrbit += kTwoPi * kOrbitHz / kGameFps;
    if (sOrbit >= kTwoPi) {
        sOrbit -= kTwoPi;
    }
    // Game convention: +x right, +z ahead. θ=0 -> ahead, θ=+90° -> right.
    Cue3D_SetPosition(sSource, sinf(sOrbit), 0.0f, cosf(sOrbit));
}

} // namespace

extern "C" void SpatialAudioTest_Start(void) {
    Cue3D_Init();

    // Load the enemy cue WAV through the seam (looped), resolving the app-relative
    // path the same way Cue::EnsureVoiceLoaded does. Cue3D_Load decodes/downmixes/
    // resamples to the backend rate, so no manual sample-rate handling is needed.
    const std::string path = Ship::Context::GetPathRelativeToAppDirectory(kEnemyCueWav);
    sSource = Cue3D_Load(path.c_str(), true);
    if (sSource == nullptr) {
        return; // backend disabled or load failed; nothing to orbit
    }
    Cue3D_Play(sSource);

    REGISTER_LISTENER(GamePostUpdateEvent, OnPostUpdate, EVENT_PRIORITY_NORMAL);
}

extern "C" void SpatialAudioTest_Stop(void) {
    // The event bus has no safe positional unregister (RegisterListener returns a
    // post-sort index that other GamePostUpdateEvent listeners can invalidate), so
    // we leave OnPostUpdate registered and instead null the handle it reads. Its
    // sSource == nullptr guard then makes every later tick a no-op, which also
    // prevents touching the source after Cue3D_Shutdown frees it.
    sSource = nullptr;
}

#else // HAVE_STEAM_AUDIO not defined

extern "C" void SpatialAudioTest_Start(void) {}
extern "C" void SpatialAudioTest_Stop(void) {}

#endif // HAVE_STEAM_AUDIO
