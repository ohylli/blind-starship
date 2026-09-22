#include "CueBench.h"

#ifdef HAVE_STEAM_AUDIO

#include "Cue3D.h"
#include "Cue.h"

// Events.h transitively pulls in the C game headers (functions.h et al.) that declare
// params named `this`; CGameCompat.h must precede it to rename the keyword for C++. Same
// ordering as SpatialAudioTest.cpp / AccessibilityCues.cpp.
#include "port/CGameCompat.h"
#include "port/hooks/Events.h"

#include <cmath>
#include <string>
#include <vector>

// Live Cue3D test bench. Supersedes the throwaway SpatialAudioTest smoke test: the
// orbiting tone is now one bench control among many, and the restart-required wart is
// gone. The bench drives the RAW seam
// for its continuous source (that is the layer under test) and a HIDDEN one-shot Cue for the
// PlayOnce / generator path, so both layers of the new capabilities get exercised by ear.
//
// Source-lifetime constraint: the seam has NO per-source free (a slot is only reclaimed by
// Cue3D_Shutdown). So the bench must NOT recreate sources on toggles. It lazily creates BOTH
// continuous sources exactly once (WAV + generated tone) and the "Synthesized tone" checkbox
// only switches WHICH of the two is playing. Fixed cost: 2 continuous slots + 1 hidden-cue
// voice, alongside gameplay's up-to-9, inside the pool of 16.

namespace {

// The WAV the old smoke test orbited, reused so "WAV vs tone" A/Bs against a real cue sound.
constexpr const char* kWavAsset = "assets/accessibility/enemy.wav";

constexpr float kTwoPi = 6.2831853f;
constexpr float kOrbitHz = 0.25f;       // one circle every 4 s (front -> right -> behind -> left)
constexpr float kGameFps = 30.0f;       // SF64 game logic ticks at 30 fps
constexpr int kStressPeriodTicks = 12;  // start/stop stress: ~0.4 s per Play/Stop phase
constexpr int kRapidRetriggerTicks = 6; // rapid re-trigger: a one-shot every ~0.2 s
constexpr float kBlipFreqHz = 660.0f;   // a clear tone, distinct from the enemy WAV
constexpr float kBlipDurSec = 0.15f;    // short: as a one-shot it's a blip, looped it pulses
constexpr float kBlipAmplitude = 0.6f;
constexpr float kFallbackRadius = 100.0f; // if the backend reports no unity distance yet

// Bench state, game-thread only (same single-threaded assumption as the old SpatialAudioTest:
// the game update and the ImGui draw both run on the main thread, so plain scalars are safe).
bool sShutdown = false;       // latched by CueBench_Shutdown: the listener then no-ops forever
bool sSourcesCreated = false; // the two continuous sources are created exactly once
Cue3DSource* sWavSource = nullptr;
Cue3DSource* sToneSource = nullptr;
Cue3DSource* sPlayingSource = nullptr; // which continuous source we last called Cue3D_Play on
float sOrbit = 0.0f;                   // current orbit angle, radians
int sStressCounter = 0;                // start/stop stress phase counter
int sRapidCounter = 0;                 // rapid-retrigger cadence counter
int sOneShotRequests = 0;              // pending PlayOnce calls (button presses + rapid cadence)
Cue* sHiddenCue = nullptr;             // hidden one-shot cue for the PlayOnce / generator path

// A short enveloped sine blip at the backend's rate. The Hann envelope is zero at both ends,
// so it loops without a click AND stands alone as a clean one-shot. Shared by the continuous
// tone source (looped) and the hidden cue's generator.
std::vector<float> GenerateBlip(int sampleRate) {
    int frames = (int) (kBlipDurSec * (float) sampleRate);
    if (frames < 2) {
        frames = 2;
    }
    std::vector<float> pcm((size_t) frames);
    for (int i = 0; i < frames; i++) {
        float t = (float) i / (float) sampleRate;
        float env = 0.5f * (1.0f - cosf(kTwoPi * (float) i / (float) (frames - 1)));
        pcm[(size_t) i] = env * sinf(kTwoPi * kBlipFreqHz * t) * kBlipAmplitude;
    }
    return pcm;
}

// Lazily create both continuous sources once (never recreated — see the source-lifetime note
// above). A null result (backend disabled / load failed) is tolerated downstream.
void EnsureSourcesCreated() {
    if (sSourcesCreated) {
        return;
    }
    sSourcesCreated = true;
    Cue3D_Init(); // idempotent; opens the second OS audio device if not already open
    const std::string path = Ship::Context::GetPathRelativeToAppDirectory(kWavAsset);
    sWavSource = Cue3D_Load(path.c_str(), true);
    std::vector<float> tone = GenerateBlip(Cue3D_GetSampleRate());
    sToneSource = Cue3D_LoadPcm(tone.data(), (int) tone.size(), true);
}

// Comfortable listening distance: the range the backend renders at unity gain, so the source
// sits at reference loudness. Falls back to a fixed radius before the backend reports one.
float BenchRadius() {
    float r = Cue3D_GetUnityGainDistance();
    return (r > 0.0f) ? r : kFallbackRadius;
}

void OnPostUpdate(IEvent* event) {
    (void) event;
    if (sShutdown) {
        return;
    }

    if (CVarGetInteger(kCueBenchActiveCVar, 0) == 0) {
        // Bench off: silence everything it owns immediately (continuous source + any in-flight
        // one-shot) and drop pending requests. Cheap early-return the rest of the time.
        if (sPlayingSource != nullptr) {
            Cue3D_Stop(sPlayingSource);
            sPlayingSource = nullptr;
        }
        if (sHiddenCue != nullptr) {
            sHiddenCue->StopAllVoices();
        }
        sOneShotRequests = 0;
        return;
    }

    EnsureSourcesCreated();

    // --- Position, shared by the continuous source and the one-shot ---
    float radius = BenchRadius();
    float x, y, z;
    if (CVarGetInteger(kCueBenchOrbitCVar, 1) != 0) {
        sOrbit += kTwoPi * kOrbitHz / kGameFps;
        if (sOrbit >= kTwoPi) {
            sOrbit -= kTwoPi;
        }
        // Game convention: +x right, +z ahead. angle 0 -> ahead, +90 deg -> right.
        x = sinf(sOrbit) * radius;
        y = 0.0f;
        z = cosf(sOrbit) * radius;
    } else {
        sOrbit = 0.0f;
        x = 0.0f;
        y = 0.0f;
        z = radius; // fixed straight ahead
    }

    // --- Live params, re-read every tick so the sliders are immediate ---
    Cue3DMode mode = (Cue3DMode) CVarGetInteger(kCueBenchModeCVar, 0);
    float pitch = CVarGetFloat(kCueBenchPitchCVar, 1.0f);
    float interval = CVarGetFloat(kCueBenchIntervalCVar, 0.0f);
    float lowPass = CVarGetFloat(kCueBenchLowPassCVar, 0.0f);
    // Same volume product the Cue layer uses (game master x cue master), so bench loudness is
    // sane; the hidden one-shot cue handles its own gain via the Cue layer.
    float gain = CVarGetFloat("gGameMasterVolume", 1.0f) * CVarGetFloat(kCueMasterVolumeCVar, 1.0f);

    // --- Continuous source: pick WAV vs tone, gate on the start/stop stress phase ---
    Cue3DSource* desired = (CVarGetInteger(kCueBenchSynthToneCVar, 0) != 0) ? sToneSource : sWavSource;
    if (CVarGetInteger(kCueBenchStartStopStressCVar, 0) != 0) {
        // Toggle Play/Stop every kStressPeriodTicks ticks — listen for clicks at the ramps.
        sStressCounter++;
        if ((sStressCounter / kStressPeriodTicks) % 2 == 1) {
            desired = nullptr; // the silent half of the stress cycle
        }
    } else {
        sStressCounter = 0;
    }

    // We never recreate sources; we only swap which pre-created one is playing. Every
    // Cue3D_Play restarts from sample 0, which is fine (and inaudible) for these test tones.
    if (sPlayingSource != desired) {
        if (sPlayingSource != nullptr) {
            Cue3D_Stop(sPlayingSource);
        }
        sPlayingSource = desired;
        if (sPlayingSource != nullptr) {
            Cue3D_Play(sPlayingSource);
        }
    }
    // Push all params every tick (the seam contract: drive a live source like a normal driver).
    if (sPlayingSource != nullptr) {
        Cue3D_SetPosition(sPlayingSource, x, y, z);
        Cue3D_SetGain(sPlayingSource, gain);
        Cue3D_SetPitch(sPlayingSource, pitch);
        Cue3D_SetMode(sPlayingSource, mode);
        Cue3D_SetInterval(sPlayingSource, interval);
        Cue3D_SetLowPass(sPlayingSource, lowPass);
    }

    // --- One-shot: manual button presses plus the rapid-retrigger stress cadence ---
    if (CVarGetInteger(kCueBenchRapidRetriggerCVar, 0) != 0) {
        sRapidCounter++;
        if (sRapidCounter >= kRapidRetriggerTicks) {
            sRapidCounter = 0;
            sOneShotRequests++;
        }
    } else {
        sRapidCounter = 0;
    }
    if (sHiddenCue != nullptr && sOneShotRequests > 0) {
        CueTarget t;
        t.x = x;
        t.y = y;
        t.z = z;
        t.pitch = pitch;
        t.lowPassHz = lowPass; // muffle applies to the one-shot too; interval is ignored on it
        while (sOneShotRequests > 0) {
            sOneShotRequests--;
            sHiddenCue->PlayOnce(t);
        }
    }
}

} // namespace

extern "C" void CueBench_Init(void) {
    CVarRegisterInteger(kCueBenchActiveCVar, 0);
    CVarRegisterInteger(kCueBenchSynthToneCVar, 0);
    CVarRegisterInteger(kCueBenchOrbitCVar, 1);
    CVarRegisterInteger(kCueBenchModeCVar, 0);
    CVarRegisterFloat(kCueBenchPitchCVar, 1.0f);
    CVarRegisterFloat(kCueBenchIntervalCVar, 0.0f);
    CVarRegisterFloat(kCueBenchLowPassCVar, 0.0f);
    CVarRegisterInteger(kCueBenchRapidRetriggerCVar, 0);
    CVarRegisterInteger(kCueBenchStartStopStressCVar, 0);

    // Hidden one-shot cue over the Cue layer, sourced from the generated blip so the generator
    // + one-shot paths get bench coverage the raw-seam continuous source cannot give them.
    // hiddenFromSettings keeps it out of the volume-slider list.
    CueSpec spec;
    spec.generator = [](int sampleRate) { return GenerateBlip(sampleRate); };
    spec.loop = false;
    spec.hiddenFromSettings = true;
    spec.maxVoices = 1;
    sHiddenCue = CueRegistry_Register("CueBench", "Cue bench one-shot",
                                      "Test-bench one-shot blip (developer bench only).", spec);

    // Registered unconditionally; the listener early-returns cheaply while the bench is off.
    REGISTER_LISTENER(GamePostUpdateEvent, OnPostUpdate, EVENT_PRIORITY_NORMAL);
}

extern "C" void CueBench_Shutdown(void) {
    // Latch the listener inert and drop our raw source handles BEFORE Cue3D_Shutdown frees
    // them, so no later tick touches a freed source (same reasoning as the old
    // SpatialAudioTest_Stop handle-null). The hidden cue's backend handles were already
    // dropped by CueRegistry_UnloadAll, which Accessibility_Exit runs just before this.
    sShutdown = true;
    sPlayingSource = nullptr;
    sWavSource = nullptr;
    sToneSource = nullptr;
    sHiddenCue = nullptr;
}

extern "C" void CueBench_RequestOneShot(void) {
    if (sShutdown) {
        return;
    }
    // The listener drains this on the next tick at the current bench position, so every press
    // sounds even though the PlayOnce itself runs on the game tick.
    sOneShotRequests++;
}

#else // HAVE_STEAM_AUDIO not defined — no cue backend, so the bench is inert.

extern "C" void CueBench_Init(void) {
}
extern "C" void CueBench_Shutdown(void) {
}
extern "C" void CueBench_RequestOneShot(void) {
}

#endif // HAVE_STEAM_AUDIO
