#include "AccessibilityTrainingMinimal.h"

#include "global.h"
#include "port/hooks/Events.h"

static bool AccessibilityTrainingMinimal_IsEnabled(void) {
    return CVarGetInteger("gAccessibilityTrainingMinimal", 1) == 1;
}

// info.hitbox is a flat f32 array where element 0 is the record count cast to
// int (see docs/game-world.md section 7). The gNoHitbox sentinel is { 0.0f },
// meaning "no records" — the object can't collide with the player and is
// either purely decorative scenery or a pure script trigger (e.g. an
// actor-event with EVID_EVENT_HANDLER, used to play radio messages).
//
// This lets us distinguish hazards (real hitbox, count >= 1) from triggers
// without enumerating ObjectIds. Critical for actor-events specifically:
// ActorEvent_Load spawns directly into OBJ_ACTIVE bypassing OBJ_INIT, and
// the script's EVOP_SET_TYPE opcode (which installs the real hitbox for
// fighter-type events) runs inside info.action — i.e. after our Update
// listener fires. So on the first Update tick every actor-event still has
// gObjectInfo[OBJ_ACTOR_EVENT]'s default gNoHitbox; on the second tick
// fighters have their real hitbox and are filtered, while pure triggers
// keep gNoHitbox and live on to run their script.
static bool AccessibilityTrainingMinimal_HasCollidableHitbox(void* object, ObjectEventType type) {
    ObjectInfo* info;
    switch (type) {
        case OBJECT_TYPE_ACTOR:
            info = &((Actor*) object)->info;
            break;
        case OBJECT_TYPE_BOSS:
            info = &((Boss*) object)->info;
            break;
        case OBJECT_TYPE_SCENERY:
            info = &((Scenery*) object)->info;
            break;
        default:
            return false;
    }
    if (info->hitbox == NULL) {
        return false;
    }
    return info->hitbox[0] != 0.0f;
}

static bool AccessibilityTrainingMinimal_ShouldFilter(ObjectEventType type, void* object) {
    if (!AccessibilityTrainingMinimal_IsEnabled()) {
        return false;
    }
    if (gCurrentLevel != LEVEL_TRAINING) {
        return false;
    }
    if (gLevelMode != LEVELMODE_ON_RAILS) {
        return false;
    }
    if ((type != OBJECT_TYPE_ACTOR) && (type != OBJECT_TYPE_SCENERY) && (type != OBJECT_TYPE_BOSS)) {
        return false;
    }
    return AccessibilityTrainingMinimal_HasCollidableHitbox(object, type);
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

// Also filter on update: ActorEvent_Load (fox_enmy.c:396) and a few other
// dynamic-spawn paths set status straight to OBJ_ACTIVE, bypassing OBJ_INIT
// entirely. In training, actor-events drive both shooting fighters and radio
// messages, so without this hook hostile actor-events slip through.
// Cancelling Update skips the actor's motion and action callback for that
// tick; freeing the slot prevents any further frames.
static void AccessibilityTrainingMinimal_OnObjectUpdate(IEvent* event) {
    ObjectUpdateEvent* e = (ObjectUpdateEvent*) event;
    if (!AccessibilityTrainingMinimal_ShouldFilter(e->type, e->object)) {
        return;
    }
    ((Object*) e->object)->status = OBJ_FREE;
    event->cancelled = true;
}

void AccessibilityTrainingMinimal_Init(void) {
    CVarRegisterInteger("gAccessibilityTrainingMinimal", 1);
    REGISTER_LISTENER(ObjectInitEvent, AccessibilityTrainingMinimal_OnObjectInit, EVENT_PRIORITY_NORMAL);
    REGISTER_LISTENER(ObjectUpdateEvent, AccessibilityTrainingMinimal_OnObjectUpdate, EVENT_PRIORITY_NORMAL);
}
