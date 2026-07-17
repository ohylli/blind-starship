#pragma once

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
inline constexpr float kAimCueOctavesDefault = 1.0f;
inline constexpr float kAimCueGeigerAngleDefault = 30.0f;
inline constexpr float kAimCueGeigerFastDefault = 0.06f;
inline constexpr float kAimCueGeigerSlowDefault = 0.8f;
// A short click is perceptually quieter than the sustained ring/enemy loops at equal sample
// level, so the aim cue starts boosted (Cue::SetGainBoost) instead of leaving its volume
// slider with no headroom at 100%.
inline constexpr float kAimCueBoostDefault = 2.0f;

void AccessibilityCues_Init();
void AccessibilityCues_Exit();
