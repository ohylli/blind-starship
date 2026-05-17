#include "AccessibilityTrainingMinimal.h"

#include "global.h"
#include "ObjectQuery.h"
#include "port/hooks/Events.h"

static bool AccessibilityTrainingMinimal_IsEnabled(void) {
    return CVarGetInteger("gAccessibilityTrainingMinimal", 1) == 1;
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
