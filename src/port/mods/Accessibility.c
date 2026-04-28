#include "Accessibility.h"

#include "global.h"
#include "port/accessibility/Tts.h"
#include "port/hooks/Events.h"

// Listeners fire synchronously on the CALL_EVENT thread; Tts_Speak is main-thread-only (see Tts.h).

static void Accessibility_OnTitleSequenceStart(IEvent* event) {
    if (!Accessibility_IsScreenReaderEnabled()) {
        return;
    }
    Tts_Speak("Star Fox 64", false);
}

static void Accessibility_OnTitleScreenReady(IEvent* event) {
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
    REGISTER_LISTENER(TitleScreenReadyEvent, Accessibility_OnTitleScreenReady, EVENT_PRIORITY_NORMAL);
}

void Accessibility_Exit(void) {
}

bool Accessibility_IsScreenReaderEnabled(void) {
    return CVarGetInteger("gAccessibilityScreenReader", 1) == 1;
}
