#pragma once

// Upper bound on simultaneous enemy-cue voices — sizes the pool registered with the Cue
// layer and caps the gAccessibilityEnemyCueVoices CVar and its F1 slider (ImguiUI.cpp).
inline constexpr int kAccessibilityEnemyCueMaxVoices = 4;

void AccessibilityCues_Init();
void AccessibilityCues_Exit();
