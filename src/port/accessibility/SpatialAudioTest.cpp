#include "SpatialAudioTest.h"

#ifdef HAVE_STEAM_AUDIO

#include "Cue3D.h"

// Events.h transitively pulls in the C game headers (functions.h et al.) that
// declare params named `this`; CGameCompat.h must precede it to rename the
// keyword for C++. Same ordering as AccessibilityCues.cpp.
#include "port/CGameCompat.h"
#include "port/hooks/Events.h"

#include <cmath>
#include <exception>
#include <vector>

// Smoke test for the Cue3D seam. A single tone orbits the listener in the
// horizontal plane; on headphones it should sweep front -> right -> behind ->
// left once every 4 s. This validates the whole chain (device, per-source
// binaural effect, mixing, block-size matching) AND that the game's +z-ahead
// convention is what callers pass — the -Z negation lives inside the backend, so
// here θ=0 maps to +z (ahead). See docs/steam-audio-handoff.md "Step 2".

namespace {

constexpr float kTwoPi = 6.2831853f;
constexpr float kToneHz = 440.0f;  // 440 whole cycles in 1 s -> click-free loop
constexpr float kToneGain = 0.2f;
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

    // Ask the backend for its rate instead of hardcoding one, so the tone's cycle
    // count stays integral (click-free loop) even if the backend rate changes.
    const int sampleRate = Cue3D_GetSampleRate();
    if (sampleRate <= 0) {
        return; // no backend compiled in / available
    }

    // Exactly one second of a 440 Hz sine: an integer cycle count over the backend's
    // sample rate, so the loop seam is continuous in both value and slope.
    std::vector<float> tone;
    try {
        tone.resize((size_t) sampleRate);
    } catch (const std::exception&) {
        return; // OOM — don't let it unwind through extern "C"
    }
    for (int i = 0; i < sampleRate; i++) {
        tone[i] = kToneGain * sinf(kTwoPi * kToneHz * (float) i / (float) sampleRate);
    }

    sSource = Cue3D_LoadPcm(tone.data(), (int) tone.size(), true);
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
