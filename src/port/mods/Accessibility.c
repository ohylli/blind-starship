#include "Accessibility.h"

#include "global.h"
#include "port/accessibility/Tts.h"
#include "accessibility/AccessibilityScreens.h"

// Listeners fire synchronously on the CALL_EVENT thread; Tts_Speak is main-thread-only (see Tts.h).

void Accessibility_Init(void) {
    CVarRegisterInteger("gAccessibilityScreenReader", 1);

    if (Accessibility_IsScreenReaderEnabled()) {
        Tts_Speak("Starship accessibility check", false);
    }

    AccessibilityTitleScreen_Register();
    AccessibilityMainMenu_Register();
    AccessibilitySoundMenu_Register();
}

void Accessibility_Exit(void) {
}

bool Accessibility_IsScreenReaderEnabled(void) {
    return CVarGetInteger("gAccessibilityScreenReader", 1) == 1;
}
