#include "Accessibility.h"

#include "global.h"
#include "fox_option.h"
#include "port/accessibility/Tts.h"
#include "port/hooks/Events.h"

// Listeners fire synchronously on the CALL_EVENT thread; Tts_Speak is main-thread-only (see Tts.h).

extern s32 sMainMenuCursor;
extern s32 sExpertModeCursor;
extern s32 sExpertSoundCursor;
extern s32 D_menu_801B9288; // sound-menu row cursor: 0=Mode, 1=Music, 2=Voice, 3=SE

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

static const char* Accessibility_SoundMenuRowLabel(void) {
    switch (D_menu_801B9288) {
        case 0:
            return "Mode";
        case 1:
            return "Music";
        case 2:
            return "Voice";
        case 3:
            return "Sound effects";
        default:
            return "unknown row";
    }
}

static const char* Accessibility_SoundModeLabel(void) {
    switch (gOptionSoundMode) {
        case OPTIONSOUND_STEREO:
            return "Stereo";
        case OPTIONSOUND_MONO:
            return "Mono";
        case OPTIONSOUND_HEADSET:
            return "Headphone";
        default:
            return "unknown mode";
    }
}

static void Accessibility_SpeakSoundMenuValue(bool interrupt) {
    char buf[16];
    if (D_menu_801B9288 == 0) {
        Tts_Speak(Accessibility_SoundModeLabel(), interrupt);
    } else {
        snprintf(buf, sizeof(buf), "%d", gVolumeSettings[D_menu_801B9288 - 1]);
        Tts_Speak(buf, interrupt);
    }
}

static void Accessibility_OnSoundMenuReady(IEvent* event) {
    if (!Accessibility_IsScreenReaderEnabled()) {
        return;
    }
    Tts_Speak("Sound menu, press R to toggle sound test.", false);
    Tts_Speak(Accessibility_SoundMenuRowLabel(), false);
    Accessibility_SpeakSoundMenuValue(false);
}

static void Accessibility_OnSoundMenuCursor(IEvent* event) {
    if (!Accessibility_IsScreenReaderEnabled()) {
        return;
    }
    Tts_Speak(Accessibility_SoundMenuRowLabel(), true);
    Accessibility_SpeakSoundMenuValue(false);
}

static void Accessibility_OnSoundMenuValueChanged(IEvent* event) {
    if (!Accessibility_IsScreenReaderEnabled()) {
        return;
    }
    Accessibility_SpeakSoundMenuValue(true);
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
    REGISTER_LISTENER(SoundMenuReadyEvent, Accessibility_OnSoundMenuReady, EVENT_PRIORITY_NORMAL);
    REGISTER_LISTENER(SoundMenuCursorEvent, Accessibility_OnSoundMenuCursor, EVENT_PRIORITY_NORMAL);
    REGISTER_LISTENER(SoundMenuValueChangedEvent, Accessibility_OnSoundMenuValueChanged, EVENT_PRIORITY_NORMAL);
}

void Accessibility_Exit(void) {
}

bool Accessibility_IsScreenReaderEnabled(void) {
    return CVarGetInteger("gAccessibilityScreenReader", 1) == 1;
}
