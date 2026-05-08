#include "AccessibilityScreens.h"

#include "global.h"
#include "fox_option.h"
#include "port/mods/Accessibility.h"
#include "port/accessibility/Tts.h"
#include "port/hooks/Events.h"

extern s32 D_menu_801B9288; // sound-menu row cursor: 0=Mode, 1=Music, 2=Voice, 3=SE

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

void AccessibilitySoundMenu_Register(void) {
    REGISTER_LISTENER(SoundMenuReadyEvent, Accessibility_OnSoundMenuReady, EVENT_PRIORITY_NORMAL);
    REGISTER_LISTENER(SoundMenuCursorEvent, Accessibility_OnSoundMenuCursor, EVENT_PRIORITY_NORMAL);
    REGISTER_LISTENER(SoundMenuValueChangedEvent, Accessibility_OnSoundMenuValueChanged, EVENT_PRIORITY_NORMAL);
}
