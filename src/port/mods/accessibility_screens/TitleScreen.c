#include "AccessibilityScreens.h"

#include "global.h"
#include "port/mods/Accessibility.h"
#include "port/accessibility/Tts.h"
#include "port/hooks/Events.h"

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

void AccessibilityTitleScreen_Register(void) {
    REGISTER_LISTENER(TitleSequenceStartEvent, Accessibility_OnTitleSequenceStart, EVENT_PRIORITY_NORMAL);
    REGISTER_LISTENER(TitleScreenReadyEvent, Accessibility_OnTitleScreenReady, EVENT_PRIORITY_NORMAL);
}
