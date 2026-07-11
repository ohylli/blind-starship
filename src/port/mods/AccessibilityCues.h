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

void AccessibilityCues_Init();
void AccessibilityCues_Exit();
