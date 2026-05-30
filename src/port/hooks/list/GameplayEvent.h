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
