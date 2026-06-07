#include "SpatialAudioTest.h"

#ifdef HAVE_STEAM_AUDIO

#include "Cue3D.h"

// Events.h transitively pulls in the C game headers (functions.h et al.) that
// declare params named `this`; CGameCompat.h must precede it to rename the
// keyword for C++. Same ordering as AccessibilityCues.cpp.
#include "port/CGameCompat.h"
#include "port/hooks/Events.h"

#include <cmath>
#include <vector>

// Smoke test for the Cue3D seam. A single tone orbits the listener in the
// horizontal plane; on headphones it should sweep front -> right -> behind ->
// left once every 4 s. This validates the whole chain (device, per-source
// binaural effect, mixing, block-size matching) AND that the game's +z-ahead
// convention is what callers pass — the -Z negation lives inside the backend, so
// here θ=0 maps to +z (ahead). See docs/steam-audio-handoff.md "Step 2".

namespace {

constexpr int kSampleRate = 48000;
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

    // Exactly one second of a 440 Hz sine: an integer cycle count over 48000
    // samples, so the loop seam is continuous in both value and slope.
    std::vector<float> tone(kSampleRate);
    for (int i = 0; i < kSampleRate; i++) {
        tone[i] = kToneGain * sinf(kTwoPi * kToneHz * (float) i / (float) kSampleRate);
    }

    sSource = Cue3D_LoadPcm(tone.data(), (int) tone.size(), true);
    if (sSource == nullptr) {
        return; // backend disabled or load failed; nothing to orbit
    }
    Cue3D_Play(sSource);

    REGISTER_LISTENER(GamePostUpdateEvent, OnPostUpdate, EVENT_PRIORITY_NORMAL);
}

#else // HAVE_STEAM_AUDIO not defined

extern "C" void SpatialAudioTest_Start(void) {}

#endif // HAVE_STEAM_AUDIO
