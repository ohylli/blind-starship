#include "AccessibilityTrainingMinimal.h"

#include "port/CGameCompat.h"
#include "ObjectQuery.h"
#include "port/hooks/Events.h"

// AccessibilityTrainingMinimal — strips collidable environmental obstacles
// from Training so a blind player can fly the on-rails phase without
// colliding into geometry they cannot see (see CLAUDE.md "Accessibility
// fork"). Active only when gAccessibilityTrainingMinimal is set AND the
// player is in Training's on-rails phase (see ScopeActive); the all-range
// phase is intentionally out of scope.
//
// WHAT IT REMOVES:
//   - Collidable scenery, bosses, and non-event actors with a real hitbox —
//     the on-rails environmental obstacles (e.g. OBJ_SCENERY_TR_BUILDING).
//     Caught by the hitbox check in ShouldFilter on both ObjectInit and
//     ObjectUpdate.
//
// WHAT IT DELIBERATELY KEEPS:
//   - OBJ_ACTOR_EVENT actors of every eventType. These cover both
//     orchestration (EVID_EVENT_HANDLER drives ring placement and radio
//     chatter via EVOP_PLAY_MSG / EVOP_SET_CALL) AND Training's scripted
//     shooting fighters (Venom tanks, spy-eyes, tripods, ...). Enemies are
//     kept alive — with their engine/firing SFX — so the enemy audio cue
//     (docs/accessibility-enemy-cue.md) has targets to attach to. An earlier
//     iteration killed non-handler actor-events via a GamePostUpdate sweep;
//     that sweep was removed when the enemy cue work began.
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
    // OBJ_ACTOR_EVENT actors are always kept. The slot is either an event
    // handler (EVID_EVENT_HANDLER — drives ring placement and radio chatter
    // and must be preserved) or a scripted shooting fighter (Venom tank,
    // spy-eye, tripod, ...). Enemies are wanted alive for the enemy audio
    // cue (docs/accessibility-enemy-cue.md), and the hitbox check would
    // otherwise catch them once their script runs EVOP_INIT_ACTOR and
    // installs the collidable hitbox.
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
// further frames. OBJ_ACTOR_EVENT actors are not filtered here either —
// ShouldFilter returns false for them so enemies (and the event handler) can
// run to completion.
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
