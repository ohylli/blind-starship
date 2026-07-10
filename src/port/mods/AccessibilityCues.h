#pragma once

// Upper bound on simultaneous enemy-cue voices — sizes the pool registered with the Cue
// layer and caps the gAccessibilityEnemyCueVoices CVar and its F1 slider (ImguiUI.cpp).
inline constexpr int kAccessibilityEnemyCueMaxVoices = 5;

// Value the gAccessibilityEnemyCueVoices CVar starts at, shared by the CVar registration
// and the F1 slider's reset-to-default.
inline constexpr int kAccessibilityEnemyCueDefaultVoices = kAccessibilityEnemyCueMaxVoices;

void AccessibilityCues_Init();
void AccessibilityCues_Exit();
