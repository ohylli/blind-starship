#include "AccessibilityScreens.h"

#include <spdlog/fmt/fmt.h>

#include "port/CGameCompat.h"
#include "port/accessibility/Tts.h"
#include "port/hooks/Events.h"
#include "port/mods/Accessibility.h"

static void Accessibility_SpeakLevelSelectorSelection(const char* levelName, const char* startOption, bool interrupt) {
    if (startOption != nullptr) {
        Tts_Speak(fmt::format("{}, alternate start: {}", levelName, startOption).c_str(), interrupt);
    } else {
        Tts_Speak(fmt::format("{}, normal start", levelName).c_str(), interrupt);
    }
}

static void Accessibility_OnLevelSelectorReady(IEvent* event) {
    if (!Accessibility_IsScreenReaderEnabled()) {
        return;
    }
    LevelSelectorReadyEvent* levelSelectorEvent = (LevelSelectorReadyEvent*) event;
    Tts_Speak("Level selector. Left and right choose a mission; up and down choose a route; L changes the alternate "
              "start when available; A starts the level.",
              false);
    Accessibility_SpeakLevelSelectorSelection(levelSelectorEvent->levelName, levelSelectorEvent->startOption, false);
}

static void Accessibility_OnLevelSelectorSelectionChanged(IEvent* event) {
    if (!Accessibility_IsScreenReaderEnabled()) {
        return;
    }
    LevelSelectorSelectionChangedEvent* levelSelectorEvent = (LevelSelectorSelectionChangedEvent*) event;
    Accessibility_SpeakLevelSelectorSelection(levelSelectorEvent->levelName, levelSelectorEvent->startOption, true);
}

static void Accessibility_OnLevelSelectorStartOptionChanged(IEvent* event) {
    if (!Accessibility_IsScreenReaderEnabled()) {
        return;
    }
    LevelSelectorStartOptionChangedEvent* levelSelectorEvent = (LevelSelectorStartOptionChangedEvent*) event;
    if (levelSelectorEvent->startOption != nullptr) {
        Tts_Speak(fmt::format("Alternate start: {}", levelSelectorEvent->startOption).c_str(), true);
    } else {
        Tts_Speak("Normal start", true);
    }
}

void AccessibilityLevelSelector_Register() {
    REGISTER_LISTENER(LevelSelectorReadyEvent, Accessibility_OnLevelSelectorReady, EVENT_PRIORITY_NORMAL);
    REGISTER_LISTENER(LevelSelectorSelectionChangedEvent, Accessibility_OnLevelSelectorSelectionChanged,
                      EVENT_PRIORITY_NORMAL);
    REGISTER_LISTENER(LevelSelectorStartOptionChangedEvent, Accessibility_OnLevelSelectorStartOptionChanged,
                      EVENT_PRIORITY_NORMAL);
}
