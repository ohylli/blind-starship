#include "AccessibilityScreens.h"

#include "port/CGameCompat.h"
#include "port/mods/Accessibility.h"
#include "port/accessibility/Tts.h"
#include "port/hooks/Events.h"

// Speaks the on-rails / all-range mode the player is flying in: at every level
// start and at every mid-level switch (the gChangeTo360 boss/Training
// transition, the Andross fight's sub-phases, the debug complete-mission cheat).
//
// The mode is established while the player still lacks control (the level-intro
// cutscene, or a mid-level cutscene like Andross's), so we do NOT speak straight
// from the event. Instead each LevelModeChangedEvent just records the latest
// mode, and a per-frame tick speaks it once the player actually has control —
// i.e. after the intro plays out or is skipped. sSpokenMode dedups so a redundant
// or already-announced write stays silent; a level start resets it to force the
// fresh level's mode to be announced even when it matches the previous level's
// ending mode. LEVELMODE_TURRET (the scrapped, unused mode) falls into the
// all-range phrasing harmlessly, mirroring CameraView.
static s32 sCurrentMode = -1; // latest mode from events; -1 before any level
static s32 sSpokenMode = -1;  // last mode actually announced

static const char* Accessibility_LevelModeLabel(s32 mode) {
    return (mode == LEVELMODE_ON_RAILS) ? "On-rails mode" : "All-range mode";
}

static void Accessibility_OnLevelModeChanged(IEvent* event) {
    // Record even while the reader is off (the tick owns the speak gate) so the
    // state stays fresh; a level start reseats sSpokenMode to force the announce.
    LevelModeChangedEvent* e = (LevelModeChangedEvent*) event;
    sCurrentMode = e->mode;
    if (e->isLevelStart) {
        sSpokenMode = -1;
    }
}

static void Accessibility_LevelModeTick(IEvent* event) {
    (void) event;
    // Gated by the master screen-reader toggle only: a mode change is a
    // deliberate, infrequent gameplay-state cue, not chatter.
    if (!Accessibility_IsScreenReaderEnabled() || (sCurrentMode == sSpokenMode)) {
        return;
    }
    if (!Accessibility_PlayerHasControl()) {
        return; // wait for the intro/cutscene to hand control to the player
    }
    sSpokenMode = sCurrentMode;
    // Interrupt: the freshest mode is the one that matters.
    Tts_Speak(Accessibility_LevelModeLabel(sCurrentMode), true);
}

void AccessibilityLevelMode_Register() {
    REGISTER_LISTENER(LevelModeChangedEvent, Accessibility_OnLevelModeChanged, EVENT_PRIORITY_NORMAL);
    REGISTER_LISTENER(DisplayPostUpdateEvent, Accessibility_LevelModeTick, EVENT_PRIORITY_NORMAL);
}
