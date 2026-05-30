#include "AccessibilityScreens.h"

#include <string>
#include <spdlog/fmt/fmt.h>

#include "port/CGameCompat.h"
#include "port/mods/Accessibility.h"
#include "port/accessibility/Tts.h"
#include "port/hooks/Events.h"

// "1 ring" vs "5 rings" — singularize the count of one.
static std::string Accessibility_RingCountPhrase(s32 count) {
    return fmt::format("{} {}", count, count == 1 ? "ring" : "rings");
}

// Shares the score-announcement toggle: both are per-event gameplay speech
// that some players find too chatty, so they mute together.
static bool Accessibility_IsRingAnnounceEnabled() {
    return CVarGetInteger("gAccessibilityScoreAnnounce", 1) == 1;
}

static void Accessibility_OnTrainingRingPassed(IEvent* event) {
    if (!Accessibility_IsScreenReaderEnabled() || !Accessibility_IsRingAnnounceEnabled()) {
        return;
    }
    TrainingRingPassedEvent* e = (TrainingRingPassedEvent*) event;
    // Interrupt: when flying through a cluster of rings the freshest count wins.
    Tts_Speak(Accessibility_RingCountPhrase(e->count).c_str(), true);
}

static void Accessibility_OnTrainingRingMissed(IEvent* event) {
    if (!Accessibility_IsScreenReaderEnabled() || !Accessibility_IsRingAnnounceEnabled()) {
        return;
    }
    TrainingRingMissedEvent* e = (TrainingRingMissedEvent*) event;
    // The miss event always fires; suppressing the no-streak case is an
    // accessibility-side decision (the game side stays policy-free).
    if (e->count == 0) {
        return;
    }
    Tts_Speak(fmt::format("Ring missed, was {}", Accessibility_RingCountPhrase(e->count)).c_str(), true);
}

void AccessibilityTrainingRings_Register() {
    REGISTER_LISTENER(TrainingRingPassedEvent, Accessibility_OnTrainingRingPassed, EVENT_PRIORITY_NORMAL);
    REGISTER_LISTENER(TrainingRingMissedEvent, Accessibility_OnTrainingRingMissed, EVENT_PRIORITY_NORMAL);
}
