#include "AccessibilityScreens.h"

#include "port/CGameCompat.h"
#include "port/mods/Accessibility.h"
#include "port/accessibility/Tts.h"
#include "port/hooks/Events.h"

// extern "C" so MSVC doesn't mangle the lookup; see MainMenu.cpp comment.
extern "C" s32 sPauseScreenIwork[10];

static const char* Accessibility_PauseMenuLabel() {
    if (sPauseScreenIwork[1] == 0) {
        return "Continue";
    }
    if (gCurrentLevel == LEVEL_TRAINING) {
        return "Quit Training";
    }
    return gLifeCount[gPlayerNum] ? "Retry Course" : "Restart Game (Game Over)";
}

static void Accessibility_OnPauseMenuReady(IEvent* event) {
    if (!Accessibility_IsScreenReaderEnabled()) {
        return;
    }
    Tts_Speak("Pause menu", false);
    Tts_Speak(Accessibility_PauseMenuLabel(), false);
}

static void Accessibility_OnPauseMenuCursor(IEvent* event) {
    if (!Accessibility_IsScreenReaderEnabled()) {
        return;
    }
    Tts_Speak(Accessibility_PauseMenuLabel(), true);
}

void AccessibilityPauseMenu_Register() {
    REGISTER_LISTENER(PauseMenuReadyEvent, Accessibility_OnPauseMenuReady, EVENT_PRIORITY_NORMAL);
    REGISTER_LISTENER(PauseMenuCursorEvent, Accessibility_OnPauseMenuCursor, EVENT_PRIORITY_NORMAL);
}
