#include "Accessibility.h"

#include "global.h"
#include "port/accessibility/Tts.h"

void Accessibility_Init(void) {
    CVarRegisterInteger("gAccessibilityScreenReader", 1);

    if (Accessibility_IsScreenReaderEnabled()) {
        Tts_Speak("Starship accessibility check", false);
    }
}

void Accessibility_Exit(void) {
}

bool Accessibility_IsScreenReaderEnabled(void) {
    return CVarGetInteger("gAccessibilityScreenReader", 1) == 1;
}
