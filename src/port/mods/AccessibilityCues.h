#pragma once

#include <stdint.h>

// Upper bound on simultaneous enemy-cue voices — sizes the pool registered with the Cue
// layer and caps the gAccessibilityEnemyCueVoices CVar and its F1 slider (ImguiUI.cpp).
inline constexpr int kAccessibilityEnemyCueMaxVoices = 5;

// Value the gAccessibilityEnemyCueVoices CVar starts at, shared by the CVar registration
// and the F1 slider's reset-to-default.
inline constexpr int kAccessibilityEnemyCueDefaultVoices = kAccessibilityEnemyCueMaxVoices;

// Height->pitch cue knobs (Developer -> Blind Starship). The toggle gates the whole
// effect; the two floats are the sensitivity divisor (world height per octave) and the
// max pitch deviation (octaves at the height extremes). Shared by the mapping in
// AccessibilityCues.cpp and the F1 sliders in ImguiUI.cpp. Defaults reproduce the old
// hard-coded mapping exactly.
inline constexpr const char* kCuePitchForHeightCVar = "gAccessibilityCuePitchForHeight";
inline constexpr const char* kCuePitchScaleCVar = "gAccessibilityCuePitchScale"; // world units per octave
inline constexpr const char* kCuePitchRangeOctavesCVar =
    "gAccessibilityCuePitchRangeOctaves"; // max octaves of bend
inline constexpr float kCuePitchScaleDefault = 1000.0f;
inline constexpr float kCuePitchRangeOctavesDefault = 1.0f;

// Aim cue knobs. The toggle lives under Accessibility -> Blind Starship; the tuning floats
// get F1 sliders under Developer -> Blind Starship -> Aim cue. Shared by the mapping code in
// AccessibilityCues.cpp and the sliders in ImguiUI.cpp; tuning guidance in
// docs/accessibility-cues-tuning.md.
inline constexpr const char* kAimCueEnabledCVar = "gAccessibilityAimCue";
inline constexpr const char* kAimCueProjDistCVar = "gAccessibilityAimCueProjDist"; // on-rails projection, world units
inline constexpr const char* kAimCueYawRangeCVar = "gAccessibilityAimCueYawRangeDeg"; // all-range full-pan deflection
inline constexpr const char* kAimCuePitchRangeDegCVar =
    "gAccessibilityAimCuePitchRangeDeg"; // all-range full-pitch aim elevation
inline constexpr const char* kAimCueOctavesCVar = "gAccessibilityAimCueOctaves"; // pitch bend at the extremes
inline constexpr const char* kAimCueGeigerAngleCVar =
    "gAccessibilityAimCueGeigerAngleDeg"; // aim-to-enemy angle where the speed-up starts
inline constexpr const char* kAimCueGeigerFastCVar = "gAccessibilityAimCueGeigerFastSec"; // interval dead on target
inline constexpr const char* kAimCueGeigerSlowCVar = "gAccessibilityAimCueGeigerSlowSec"; // interval with no target
inline constexpr const char* kAimCueBoostCVar = "gAccessibilityAimCueBoost"; // loudness vs the sustained cues

inline constexpr float kAimCueProjDistDefault = 1200.0f; // the near reticle's distance (fox_display.c)
inline constexpr float kAimCueYawRangeDefault = 55.0f;
inline constexpr float kAimCuePitchRangeDegDefault = 90.0f;
inline constexpr float kAimCueOctavesDefault = 2.0f;
inline constexpr float kAimCueGeigerAngleDefault = 30.0f;
inline constexpr float kAimCueGeigerFastDefault = 0.06f;
inline constexpr float kAimCueGeigerSlowDefault = 0.8f;
// A short click is perceptually quieter than the sustained ring/enemy loops at equal sample
// level, so the aim cue starts boosted (Cue::SetGainBoost) instead of leaving its volume
// slider with no headroom at 100%.
inline constexpr float kAimCueBoostDefault = 2.0f;

// ===== Last-tick policy debug state =====
//
// A mirror of what each cue listener decided on its most recent GamePostUpdateEvent tick,
// read by the debug server's `cues` command (it merges this "why" layer with the Cue
// layer's per-voice "what" from Cue::Snapshot). Every listener exit path writes its
// section — gated ticks record the gate terms with active=false — and stamps `frame`
// last, so `frame == gGameFrameCount` at query time means the data is from this tick.
// Plain scalars only, never an Actor*/Item*: a section read after the level ends must
// not dangle. frame == -1 means the section has never been written.

struct AccessibilityCuesRingDebug {
    int32_t frame = -1;
    bool active = false;
    bool enabled = false, inTraining = false, control = false; // gate terms
    int32_t itemIndex = -1;             // gItems slot of the tracked ring
    float dx = 0, dy = 0, dz = 0;       // player-relative delta fed to the cue (pre-clamp)
    float src[3] = {};                  // post-clamp target actually pushed
    float freq = 1;
};

struct AccessibilityCuesEnemyTargetDebug {
    int32_t slot = -1;                  // gActors index
    int32_t objId = -1, eventType = -1; // decoded identity (the key layout stays in the .cpp)
    uint64_t voiceKey = 0;              // join field against CueVoiceSnapshot::key
    float distance = 0;                 // 3D body-frame distance
    float bodyDelta[3] = {};            // pre-clamp body-frame delta (+X right, +Y up, -Z ahead)
    float src[3] = {};                  // post-clamp target actually pushed
    float freq = 1;
};

struct AccessibilityCuesEnemyDebug {
    int32_t frame = -1;
    bool active = false; // the scan ran (even if it found nothing)
    bool enabled = false, modeOk = false, allRange = false, versus = false, control = false;
    int32_t scanActive = 0, scanCueable = 0, scanKept = 0; // EnemyCueScanStats
    int32_t requestedVoices = 0;        // clamped gAccessibilityEnemyCueVoices used this tick
    int32_t count = 0;                  // valid entries in targets[]
    AccessibilityCuesEnemyTargetDebug targets[kAccessibilityEnemyCueMaxVoices];
};

struct AccessibilityCuesAimDebug {
    int32_t frame = -1;
    bool active = false;
    bool enabled = false, aimEnabled = false, modeOk = false, arwing = false; // gate terms
    bool allRange = false;
    float nx = 0, ny = 0;               // normalized aim signals [-1, 1]
    float minEnemyAngleRad = 0;         // INFINITY when no enemy in scope
    float intervalSec = 0;              // geiger click interval pushed this tick
    float pitch = 1;                    // playback rate pushed this tick
};

struct AccessibilityCuesDebugState {
    AccessibilityCuesRingDebug ring;
    AccessibilityCuesEnemyDebug enemy;
    AccessibilityCuesAimDebug aim;
};

const AccessibilityCuesDebugState& AccessibilityCues_DebugState();

void AccessibilityCues_Init();
void AccessibilityCues_Exit();
