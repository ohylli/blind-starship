#include "AccessibilityTrainingMinimal.h"

#include "port/CGameCompat.h"
#include "ObjectQuery.h"
#include "port/hooks/Events.h"

// AccessibilityTrainingMinimal — strips collidable environmental obstacles
// from Training so a blind player can fly the level without colliding into
// geometry they cannot see (see CLAUDE.md "Accessibility fork"). Active
// only when gAccessibilityTrainingMinimal is set AND the player is in
// Training (see ScopeActive); covers both the on-rails phase and the
// all-range phase.
//
// WHAT IT REMOVES:
//   - Collidable scenery and bosses (e.g. OBJ_SCENERY_TR_BUILDING).
//   - The all-range phase's buildings — the same OBJ_SCENERY_TR_BUILDING id,
//     but spawned by Training_Setup360 (fox_tr360.c) from a second object
//     list directly into the gScenery360 array, bypassing the fox_enmy.c
//     lifecycle. Caught via the ObjectInitEvent that Training_Setup360 now
//     fires per spawn (type OBJECT_TYPE_SCENERY360). Side effect: the
//     enemy AI's building-avoidance check (Training_EnemyObstacleCheck)
//     then never triggers, so enemies fly straight paths — acceptable,
//     they remain lockable cue targets.
//   - Actors with a collidable hitbox AND info.targetOffset == 0.0f — i.e.
//     things the engine treats as non-lockable hazards. Catches the scripted
//     training barrier (OBJ_ACTOR_EVENT / EVID_TR_BARRIER) and would catch
//     any future non-event hazard actor. Predicate matches the engine's own
//     lock-on check (PlayerShot_FindLockTarget, fox_beam.c) and what the
//     enemy audio cue will use (docs/accessibility-enemy-cue.md).
//   All filtered via ShouldFilter on both ObjectInit and ObjectUpdate.
//
// WHAT IT DELIBERATELY KEEPS:
//   - Lockable actor enemies — non-zero info.targetOffset. In Training all
//     enemies live in OBJ_ACTOR_EVENT slots (Venom tanks, spy-eyes, tripods,
//     ...). Their engine/firing SFX are kept too, so the enemy audio cue
//     has audible targets. An earlier iteration killed non-handler
//     actor-events via a GamePostUpdate sweep; that sweep was removed when
//     the enemy cue work began, and the targetOffset check replaced the
//     coarse "skip all OBJ_ACTOR_EVENT" rule with one that re-strips the
//     non-enemy actor-events like the training barrier.
//   - EVID_EVENT_HANDLER actor-events. These have no collidable hitbox
//     (gNoHitbox) so the hitbox check passes them through automatically.
//     They drive ring placement and radio chatter via EVOP_PLAY_MSG /
//     EVOP_SET_CALL — killing them would defeat the test.
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
           (gCurrentLevel == LEVEL_TRAINING);
}

static bool AccessibilityTrainingMinimal_ShouldFilter(ObjectEventType type, void* object) {
    if (!AccessibilityTrainingMinimal_ScopeActive()) {
        return false;
    }
    if ((type != OBJECT_TYPE_ACTOR) && (type != OBJECT_TYPE_SCENERY) && (type != OBJECT_TYPE_SCENERY360) &&
        (type != OBJECT_TYPE_BOSS)) {
        return false;
    }
    if (!Object_HasCollidableHitbox(type, object)) {
        return false;
    }
    // For actors (regular or OBJ_ACTOR_EVENT), distinguish enemies from
    // environmental obstacles by info.targetOffset: enemies have a non-zero
    // lock-on offset (same predicate the engine uses for missile lock-on in
    // PlayerShot_FindLockTarget, fox_beam.c, and the predicate the upcoming
    // enemy audio cue uses — see docs/accessibility-enemy-cue.md). Obstacles
    // like EVID_TR_BARRIER (the training barrier) have targetOffset == 0.0f
    // even though they have a collidable hitbox; those are exactly what we
    // want to strip. For OBJ_ACTOR_EVENT actors info.targetOffset is the
    // default 0.0f until the script runs EVOP_INIT_ACTOR (which also installs
    // the real hitbox — see the second-tick note in Object_HasCollidableHitbox),
    // so the hitbox and targetOffset land together and the check is consistent.
    if (type == OBJECT_TYPE_ACTOR) {
        Actor* a = (Actor*) object;
        if (a->info.targetOffset != 0.0f) {
            return false;
        }
    }
    return true;
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
// further frames. This is also the path that catches OBJ_ACTOR_EVENT
// obstacles like EVID_TR_BARRIER, whose hitbox isn't installed until the
// second update tick — by then ShouldFilter can read both info.hitbox and
// info.targetOffset and discriminate against the barrier while leaving
// enemies (non-zero targetOffset) alive.
static void AccessibilityTrainingMinimal_OnObjectUpdate(IEvent* event) {
    ObjectUpdateEvent* e = (ObjectUpdateEvent*) event;
    if (!AccessibilityTrainingMinimal_ShouldFilter(e->type, e->object)) {
        return;
    }
    ((Object*) e->object)->status = OBJ_FREE;
    event->cancelled = true;
}

void AccessibilityTrainingMinimal_Init() {
    CVarRegisterInteger("gAccessibilityTrainingMinimal", 1);
    REGISTER_LISTENER(ObjectInitEvent, AccessibilityTrainingMinimal_OnObjectInit, EVENT_PRIORITY_NORMAL);
    REGISTER_LISTENER(ObjectUpdateEvent, AccessibilityTrainingMinimal_OnObjectUpdate, EVENT_PRIORITY_NORMAL);
}
