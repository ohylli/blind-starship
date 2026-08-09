#pragma once

#include <stdint.h>

// Registry id of the aim cue, shared by the registration in AimCue.cpp and the debug
// server's policy-section match (DebugCommands.cpp) so a rename cannot silently detach
// the cue from its policy dump.
inline constexpr const char* kAimCueId = "Aim";

// Aim cue knobs. The toggle lives under Accessibility -> Blind Starship; the tuning floats
// get F1 sliders under Developer -> Blind Starship -> Aim cue. Shared by the mapping code
// in AimCue.cpp and the sliders in ImguiUI.cpp; tuning guidance in
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

// Last-tick policy mirror for the debug server's `cues` command — shared discipline
// documented in CueCommon.h.
struct AimCueDebug {
    int32_t frame = -1;
    bool active = false;
    // Gate terms. arwing folds in control (the form read needs a live player), so control
    // is mirrored separately to tell "no control" from "wrong vehicle".
    bool enabled = false, aimEnabled = false, modeOk = false, control = false, arwing = false;
    bool allRange = false;
    float nx = 0, ny = 0;       // normalized aim signals [-1, 1]
    float minEnemyAngleRad = 0; // INFINITY when no enemy in scope
    float intervalSec = 0;      // geiger click interval pushed this tick
    float pitch = 1;            // playback rate pushed this tick
};

const AimCueDebug& AimCue_DebugState();

// Registers the cue, its CVars, and its listener; called once from AccessibilityCues_Init.
void AimCue_Register();
