#pragma once

// Listeners registered by these functions fire synchronously on the CALL_EVENT thread;
// Tts_Speak is main-thread-only (see Tts.h).

void AccessibilityTitleScreen_Register();
void AccessibilityMainMenu_Register();
void AccessibilitySoundMenu_Register();
void AccessibilityPauseMenu_Register();
void AccessibilityLevelSelector_Register();
void AccessibilityTrainingRings_Register();
void AccessibilityScore_Register();
void AccessibilityCameraView_Register();
void AccessibilityLevelMode_Register();
// Full API (frame tick, narrator) lives in ImGuiMenu.h.
void AccessibilityImGuiMenu_Register();
