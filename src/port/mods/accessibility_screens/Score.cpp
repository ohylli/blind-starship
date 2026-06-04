#include "AccessibilityScreens.h"

#include <string>
#include <vector>
#include <spdlog/fmt/fmt.h>

#include "port/CGameCompat.h"
#include "port/mods/Accessibility.h"
#include "port/accessibility/Tts.h"
#include "port/hooks/Events.h"

// On-screen bonus popups ("hit +N" / "GREAT" / "1UP") are announced instead of the
// raw gHitCount delta so the speech matches what a sighted player sees. The displayed
// value is captured as each popup fires (BonusTextEvent), then spoken once the frame's
// score is settled: gHitCount is incremented *after* BonusText_Display at every call
// site, so it is only final later in the frame. We therefore buffer the popups and
// flush them on DisplayPostUpdateEvent, where gHitCount is the running total the HUD
// shows. Both events fire on the game thread, so the buffer needs no locking (and
// Tts_Speak is game/main-thread only, per Tts.h).
//
// DisplayPostUpdateEvent only fires on DRAW_PLAY frames, so a popup that fires on the
// frame the draw mode flips away (mission end / game over) would otherwise be stranded
// and later flushed against the next mission's reset gHitCount. GamePreUpdateEvent runs
// at the top of every frame, before any popup is buffered, so clearing there guarantees
// a stranded entry can never survive into a later frame.
static std::vector<s32> sPendingBonuses;

// Dedicated toggle (default on, shared with the Training-ring streak announcements):
// announcing every kill can be too chatty for some players, so this speech can be
// silenced without disabling the reader.
static bool Accessibility_IsScoreAnnounceEnabled() {
    return CVarGetInteger("gAccessibilityScoreAnnounce", 1) == 1;
}

// Spoken form of one popup plus the running total. Numeric popups echo the on-screen
// "hit +N" verbatim; the two sentinels become words. total is gHitCount, read at flush time.
static std::string Accessibility_BonusPhrase(s32 value, s32 total) {
    if (value == BONUS_TEXT_GREAT) {
        return fmt::format("great, total {}", total);
    }
    if (value == BONUS_TEXT_1UP) {
        return fmt::format("extra life, total {}", total);
    }
    return fmt::format("hit +{}, total {}", value, total);
}

static void Accessibility_OnBonusText(IEvent* event) {
    if (!Accessibility_IsScreenReaderEnabled() || !Accessibility_IsScoreAnnounceEnabled()) {
        return;
    }
    BonusTextEvent* e = (BonusTextEvent*) event;
    // BONUS_TEXT_FREE is the free-slot sentinel, not a real popup.
    if (e->value == BONUS_TEXT_FREE) {
        return;
    }
    sPendingBonuses.push_back(e->value);
}

static void Accessibility_OnDisplayPostUpdate(IEvent* event) {
    (void) event;
    if (sPendingBonuses.empty()) {
        return;
    }
    if (!Accessibility_IsScreenReaderEnabled() || !Accessibility_IsScoreAnnounceEnabled()) {
        sPendingBonuses.clear();
        return;
    }

    // gHitCount is the per-mission running score the HUD animates toward, and by this
    // point in the frame it reflects every popup buffered this frame. It is snapshotted
    // once, so every popup in a same-frame cluster reports the same final frame total
    // rather than a per-popup running subtotal — matching the single number the HUD shows.
    s32 total = gHitCount;
    for (size_t i = 0; i < sPendingBonuses.size(); i++) {
        // Interrupt on the first so the freshest frame wins over stale speech; queue
        // the rest (Tts_Speak false → PRISM append) so a same-frame cluster is heard
        // in order rather than each cutting off the last.
        Tts_Speak(Accessibility_BonusPhrase(sPendingBonuses[i], total).c_str(), i == 0);
    }
    sPendingBonuses.clear();
}

static void Accessibility_OnGamePreUpdate(IEvent* event) {
    (void) event;
    // Defensive clear (see the sPendingBonuses header comment): drop any popup left
    // unflushed by a frame whose draw mode was not DRAW_PLAY, so it can never be spoken
    // against a later mission's score.
    sPendingBonuses.clear();
}

void AccessibilityScore_Register() {
    CVarRegisterInteger("gAccessibilityScoreAnnounce", 1);
    REGISTER_LISTENER(BonusTextEvent, Accessibility_OnBonusText, EVENT_PRIORITY_NORMAL);
    REGISTER_LISTENER(DisplayPostUpdateEvent, Accessibility_OnDisplayPostUpdate, EVENT_PRIORITY_NORMAL);
    REGISTER_LISTENER(GamePreUpdateEvent, Accessibility_OnGamePreUpdate, EVENT_PRIORITY_NORMAL);
}
