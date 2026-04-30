#include "Accessibility.h"

#include "global.h"
#include "fox_option.h"
#include "port/accessibility/Tts.h"
#include "port/hooks/Events.h"

// Listeners fire synchronously on the CALL_EVENT thread; Tts_Speak is main-thread-only (see Tts.h).

extern s32 sMainMenuCursor;
extern s32 sExpertModeCursor;
extern s32 sExpertSoundCursor;

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

static const char* Accessibility_MainMenuLabel(void) {
    switch (sMainMenuCursor) {
        case OPTION_MAP:
            return sExpertModeCursor ? "Expert mode" : "Main game";
        case OPTION_TRAINING:
            return "Training";
        case OPTION_VERSUS:
            return "Versus";
        case OPTION_RANKING:
            return "Ranking";
        case OPTION_SOUND:
            return sExpertSoundCursor ? "Expert sound" : "Sound";
        case OPTION_DATA:
            return "Data";
        default:
            return "unknown option";
    }
}

static void Accessibility_OnMainMenuReady(IEvent* event) {
    if (!Accessibility_IsScreenReaderEnabled()) {
        return;
    }
    Tts_Speak("Main menu", false);
    Tts_Speak(Accessibility_MainMenuLabel(), false);
}

static void Accessibility_OnMainMenuCursor(IEvent* event) {
    if (!Accessibility_IsScreenReaderEnabled()) {
        return;
    }
    Tts_Speak(Accessibility_MainMenuLabel(), true);
}

void Accessibility_Init(void) {
    CVarRegisterInteger("gAccessibilityScreenReader", 1);

    if (Accessibility_IsScreenReaderEnabled()) {
        Tts_Speak("Starship accessibility check", false);
    }

    REGISTER_LISTENER(TitleSequenceStartEvent, Accessibility_OnTitleSequenceStart, EVENT_PRIORITY_NORMAL);
    REGISTER_LISTENER(TitleScreenReadyEvent, Accessibility_OnTitleScreenReady, EVENT_PRIORITY_NORMAL);
    REGISTER_LISTENER(MainMenuReadyEvent, Accessibility_OnMainMenuReady, EVENT_PRIORITY_NORMAL);
    REGISTER_LISTENER(MainMenuCursorEvent, Accessibility_OnMainMenuCursor, EVENT_PRIORITY_NORMAL);
}

void Accessibility_Exit(void) {
}

bool Accessibility_IsScreenReaderEnabled(void) {
    return CVarGetInteger("gAccessibilityScreenReader", 1) == 1;
}
