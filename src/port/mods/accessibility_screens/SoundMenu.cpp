#include "AccessibilityScreens.h"

#include <spdlog/fmt/fmt.h>

#include "port/CGameCompat.h"
#include "fox_option.h"
#include "port/mods/Accessibility.h"
#include "port/accessibility/Tts.h"
#include "port/hooks/Events.h"

// extern "C" so MSVC doesn't mangle the lookup; see MainMenu.cpp comment.
extern "C" s32 D_menu_801B9288; // sound-menu row cursor: 0=Mode, 1=Music, 2=Voice, 3=SE

static const char* Accessibility_SoundMenuRowLabel() {
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

static const char* Accessibility_SoundModeLabel() {
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
    if (D_menu_801B9288 == 0) {
        Tts_Speak(Accessibility_SoundModeLabel(), interrupt);
    } else {
        Tts_Speak(fmt::format("{}", gVolumeSettings[D_menu_801B9288 - 1]).c_str(), interrupt);
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

void AccessibilitySoundMenu_Register() {
    REGISTER_LISTENER(SoundMenuReadyEvent, Accessibility_OnSoundMenuReady, EVENT_PRIORITY_NORMAL);
    REGISTER_LISTENER(SoundMenuCursorEvent, Accessibility_OnSoundMenuCursor, EVENT_PRIORITY_NORMAL);
    REGISTER_LISTENER(SoundMenuValueChangedEvent, Accessibility_OnSoundMenuValueChanged, EVENT_PRIORITY_NORMAL);
}
