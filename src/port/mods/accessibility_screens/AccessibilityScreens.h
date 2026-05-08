#pragma once

// Listeners registered by these functions fire synchronously on the CALL_EVENT thread;
// Tts_Speak is main-thread-only (see Tts.h).

#ifdef __cplusplus
extern "C" {
#endif

void AccessibilityTitleScreen_Register(void);
void AccessibilityMainMenu_Register(void);
void AccessibilitySoundMenu_Register(void);

#ifdef __cplusplus
}
#endif
