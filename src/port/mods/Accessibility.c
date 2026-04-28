#include "Accessibility.h"

#include "global.h"
#include "port/accessibility/Tts.h"
#include "port/hooks/list/MenuEvent.h"

static void Accessibility_OnTitleSequenceStart(IEvent* event) {
    if (!Accessibility_IsScreenReaderEnabled()) {
        return;
    }
    Tts_Speak("Star Fox 64", false);
}

static void Accessibility_OnTitlePressStartReady(IEvent* event) {
    if (!Accessibility_IsScreenReaderEnabled()) {
        return;
    }
    Tts_Speak("Title screen, press Start to continue", false);
}

void Accessibility_Init(void) {
    CVarRegisterInteger("gAccessibilityScreenReader", 1);

    if (Accessibility_IsScreenReaderEnabled()) {
        Tts_Speak("Starship accessibility check", false);
    }

    REGISTER_LISTENER(TitleSequenceStartEvent, Accessibility_OnTitleSequenceStart, EVENT_PRIORITY_NORMAL);
    REGISTER_LISTENER(TitlePressStartReadyEvent, Accessibility_OnTitlePressStartReady, EVENT_PRIORITY_NORMAL);
}

void Accessibility_Exit(void) {
}

bool Accessibility_IsScreenReaderEnabled(void) {
    return CVarGetInteger("gAccessibilityScreenReader", 1) == 1;
}
