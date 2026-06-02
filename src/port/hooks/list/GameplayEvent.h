#pragma once

#include "global.h"
#include "port/hooks/impl/EventSystem.h"

// Semantic gameplay state transitions (as opposed to the render/update hooks in
// EngineEvent.h). Producers fire these at the exact moment a game-state change
// happens; consumers (e.g. accessibility announcements) decide what to do.

// Training consecutive-ring streak transitions. count carries the streak value:
// the new streak after collecting (passed), or the streak just lost (missed,
// fired unconditionally including count == 0 so the consumer owns announcement policy).
DEFINE_EVENT(TrainingRingPassedEvent, s32 count;);
DEFINE_EVENT(TrainingRingMissedEvent, s32 count;);

// On-screen bonus popup ("hit +N" / "GREAT" / "1UP"). Fired from BonusText_Display
// the instant a popup is actually shown, carrying value exactly as the game passes
// it: 1-10 and 20/30/40/50 render as numbers, BONUS_TEXT_GREAT (100) as "GREAT",
// BONUS_TEXT_1UP (101) as "1UP". This is what the player sees, which is deliberately
// not the same as the gHitCount increment (a hidden "+1 tax" at most sites, lock-on
// bonuses >10 clamped to "GREAT", 1UP not scoring at all). Consumers that also want
// the running total read gHitCount on DisplayPostUpdateEvent, where it is final for
// the frame (the popup fires before gHitCount is incremented at the call site).
DEFINE_EVENT(BonusTextEvent, s32 value;);
