#include "AccessibilityTrainingMinimal.h"

#include "port/CGameCompat.h"
#include "ObjectQuery.h"
#include "port/hooks/Events.h"

// AccessibilityTrainingMinimal — strips Training down to just the ring cues
// for the blind-accessibility audio-cue test (see CLAUDE.md "Accessibility
// fork"). Active only when gAccessibilityTrainingMinimal is set AND the player
// is in Training's on-rails phase (see ScopeActive); the all-range phase is
// intentionally out of scope.
//
// WHAT IT REMOVES:
//   - Collidable actors, scenery, and bosses — the on-rails obstacles and
//     statically-placed enemies. Caught by the hitbox check in ShouldFilter,
//     applied on both ObjectInit and ObjectUpdate.
//   - Enemy actor-events: OBJ_ACTOR_EVENT actors whose resolved eventType is
//     a non-handler EVID (Venom tanks, spy-eyes, tripods, Granga fighters,
//     ...). These drive Training's scripted shooting fighters. They get a
//     SEPARATE path — the GamePostUpdate sweep — because an actor-event's
//     collidable hitbox is not installed until its script runs EVOP_INIT_ACTOR,
//     so the hitbox check above cannot see them in time. See the ShouldFilter
//     and OnGamePostUpdate comments for the full timing rationale.
//
// WHAT IT DELIBERATELY KEEPS:
//   - EVID_EVENT_HANDLER actor-events. These orchestrate the level: training
//     ring placement and radio/dialogue (EVOP_PLAY_MSG / EVOP_SET_CALL). They
//     are the cue producers — killing them would defeat the test.
//   - All items, INCLUDING bonus items (bombs, lasers, silver/gold rings, wing
//     repair). Note for future sessions: a few bonus items are placed at the
//     exact coordinates of training rings, so flying the cue path collects
//     them and plays a one-shot pickup chime. As of 2026-05-20 those residual
//     pickup sounds were identified as a candidate but items were left
//     unfiltered by choice (the residual-sound cause was not fully confirmed).
//     If a future session does want them gone, filter OBJECT_TYPE_ITEM where
//     id != OBJ_ITEM_TRAINING_RING inside ShouldFilter.

static bool AccessibilityTrainingMinimal_IsEnabled() {
    return CVarGetInteger("gAccessibilityTrainingMinimal", 1) == 1;
}

static bool AccessibilityTrainingMinimal_ScopeActive() {
    return AccessibilityTrainingMinimal_IsEnabled() &&
           (gCurrentLevel == LEVEL_TRAINING) &&
           (gLevelMode == LEVELMODE_ON_RAILS);
}

static bool AccessibilityTrainingMinimal_ShouldFilter(ObjectEventType type, void* object) {
    if (!AccessibilityTrainingMinimal_ScopeActive()) {
        return false;
    }
    if ((type != OBJECT_TYPE_ACTOR) && (type != OBJECT_TYPE_SCENERY) && (type != OBJECT_TYPE_BOSS)) {
        return false;
    }
    // OBJ_ACTOR_EVENT actors are handled exclusively by the GamePostUpdate
    // sweep below — not by this hitbox check. Two reasons: (1) the actor's
    // collidable hitbox isn't installed until its script runs EVOP_INIT_ACTOR,
    // which happens after the first ObjectUpdateEvent, so the hitbox check
    // here only catches it a frame late; (2) more importantly, this path only
    // sets status=OBJ_FREE, which leaves the looping engine SFX the script
    // started still playing. The sweep uses Object_Kill, which also calls
    // Audio_KillSfxBySource. Letting the hitbox check free the slot here would
    // preempt the sweep and strand the sound.
    if ((type == OBJECT_TYPE_ACTOR) && (((Object*) object)->id == OBJ_ACTOR_EVENT)) {
        return false;
    }
    return Object_HasCollidableHitbox(type, object);
}

// Free the slot before cancelling: cancellation skips the engine's OBJ_INIT
// branch (fox_enmy.c Actor_Update / Scenery_Update / Boss_Update), which is
// where status would have transitioned to OBJ_ACTIVE. Without OBJ_FREE the
// slot stays in OBJ_INIT and the cancellable event refires forever.
static void AccessibilityTrainingMinimal_OnObjectInit(IEvent* event) {
    ObjectInitEvent* e = (ObjectInitEvent*) event;
    if (!AccessibilityTrainingMinimal_ShouldFilter(e->type, e->object)) {
        return;
    }
    ((Object*) e->object)->status = OBJ_FREE;
    event->cancelled = true;
}

// Also filter on update: some dynamic-spawn paths set status straight to
// OBJ_ACTIVE, bypassing OBJ_INIT entirely, so a collidable actor / scenery /
// boss can slip past the ObjectInit hook. Cancelling Update skips the object's
// motion and action callback for that tick; freeing the slot prevents any
// further frames. Note: OBJ_ACTOR_EVENT actors are NOT filtered here —
// ShouldFilter returns false for them and the GamePostUpdate sweep owns them.
static void AccessibilityTrainingMinimal_OnObjectUpdate(IEvent* event) {
    ObjectUpdateEvent* e = (ObjectUpdateEvent*) event;
    if (!AccessibilityTrainingMinimal_ShouldFilter(e->type, e->object)) {
        return;
    }
    ((Object*) e->object)->status = OBJ_FREE;
    event->cancelled = true;
}

// Second-stage filter for OBJ_ACTOR_EVENT enemies.
//
// The hitbox check above can't catch enemy actor-events on their first frame:
// ObjectUpdateEvent fires from Actor_Update *before* Actor_Move runs the
// actor's script (fox_enmy.c:2873-2880), and the script's EVOP_INIT_ACTOR
// opcode is what installs the EVID's real hitbox (fox_enmy2.c:1132-1199).
// So at the early hook the actor still has the generic OBJ_ACTOR_EVENT info
// with no collidable hitbox, and the filter passes it through. By frame N+1
// the hitbox is set and the early filter would catch it — but the actor has
// already lived one frame, run an action callback, and possibly started a
// looping engine/firing SFX that the simple status=OBJ_FREE cancel wouldn't
// silence.
//
// GamePostUpdateEvent fires once per tick after Play_Update (fox_game.c:614),
// by which time the script has fully resolved `eventType`. We sweep gActors
// for any OBJ_ACTOR_EVENT whose eventType is not the orchestration handler
// (which drives ring placement and radio chatter via EVOP_PLAY_MSG / SET_CALL
// scripts and must be preserved). Object_Kill is used rather than a bare
// status assignment so any SFX the actor's first-frame action callback
// started gets killed via Audio_KillSfxBySource.
//
// Leaving EVID_FFF alone: the sentinel value means the script hasn't yet run
// EVOP_INIT_ACTOR. By GamePostUpdate that's rare, and killing an unresolved
// actor risks tearing down a future friendly we haven't classified.
static void AccessibilityTrainingMinimal_OnGamePostUpdate(IEvent* event) {
    (void) event;
    if (!AccessibilityTrainingMinimal_ScopeActive()) {
        return;
    }
    for (s32 i = 0; i < (s32) ARRAY_COUNT(gActors); i++) {
        Actor* a = &gActors[i];
        if (a->obj.status != OBJ_ACTIVE) {
            continue;
        }
        if (a->obj.id != OBJ_ACTOR_EVENT) {
            continue;
        }
        s16 eventType = a->eventType;
        if (eventType == EVID_FFF) {
            continue;
        }
        if (eventType == EVID_EVENT_HANDLER) {
            continue;
        }
        Object_Kill(&a->obj, a->sfxSource);
    }
}

void AccessibilityTrainingMinimal_Init() {
    CVarRegisterInteger("gAccessibilityTrainingMinimal", 1);
    REGISTER_LISTENER(ObjectInitEvent, AccessibilityTrainingMinimal_OnObjectInit, EVENT_PRIORITY_NORMAL);
    REGISTER_LISTENER(ObjectUpdateEvent, AccessibilityTrainingMinimal_OnObjectUpdate, EVENT_PRIORITY_NORMAL);
    REGISTER_LISTENER(GamePostUpdateEvent, AccessibilityTrainingMinimal_OnGamePostUpdate, EVENT_PRIORITY_NORMAL);
}
