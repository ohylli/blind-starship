#include "ObjectSpawnLog.h"

#include "port/CGameCompat.h"
#include "ObjectQuery.h"
#include "port/hooks/Events.h"

#include <spdlog/spdlog.h>

static bool ObjectSpawnLog_IsEnabled() {
    return CVarGetInteger("gObjectSpawnLog", 0) == 1;
}

static const char* ObjectSpawnLog_TypeName(ObjectEventType type) {
    switch (type) {
        case OBJECT_TYPE_ACTOR:
            return "ACTOR";
        case OBJECT_TYPE_ACTOR_EVENT:
            return "ACTOR_EVENT";
        case OBJECT_TYPE_BOSS:
            return "BOSS";
        case OBJECT_TYPE_SCENERY:
            return "SCENERY";
        case OBJECT_TYPE_SCENERY360:
            return "SCENERY360";
        case OBJECT_TYPE_SPRITE:
            return "SPRITE";
        case OBJECT_TYPE_ITEM:
            return "ITEM";
        case OBJECT_TYPE_EFFECT:
            return "EFFECT";
        default:
            return "UNKNOWN";
    }
}

const char* Starship_LevelName(int level) {
    switch (level) {
        case LEVEL_CORNERIA:
            return "LEVEL_CORNERIA";
        case LEVEL_METEO:
            return "LEVEL_METEO";
        case LEVEL_SECTOR_X:
            return "LEVEL_SECTOR_X";
        case LEVEL_AREA_6:
            return "LEVEL_AREA_6";
        case LEVEL_UNK_4:
            return "LEVEL_UNK_4";
        case LEVEL_SECTOR_Y:
            return "LEVEL_SECTOR_Y";
        case LEVEL_VENOM_1:
            return "LEVEL_VENOM_1";
        case LEVEL_SOLAR:
            return "LEVEL_SOLAR";
        case LEVEL_ZONESS:
            return "LEVEL_ZONESS";
        case LEVEL_VENOM_ANDROSS:
            return "LEVEL_VENOM_ANDROSS";
        case LEVEL_TRAINING:
            return "LEVEL_TRAINING";
        case LEVEL_MACBETH:
            return "LEVEL_MACBETH";
        case LEVEL_TITANIA:
            return "LEVEL_TITANIA";
        case LEVEL_AQUAS:
            return "LEVEL_AQUAS";
        case LEVEL_FORTUNA:
            return "LEVEL_FORTUNA";
        case LEVEL_UNK_15:
            return "LEVEL_UNK_15";
        case LEVEL_KATINA:
            return "LEVEL_KATINA";
        case LEVEL_BOLSE:
            return "LEVEL_BOLSE";
        case LEVEL_SECTOR_Z:
            return "LEVEL_SECTOR_Z";
        case LEVEL_VENOM_2:
            return "LEVEL_VENOM_2";
        case LEVEL_VERSUS:
            return "LEVEL_VERSUS";
        case LEVEL_WARP_ZONE:
            return "LEVEL_WARP_ZONE";
        default:
            return "LEVEL_UNKNOWN";
    }
}

static bool ObjectSpawnLog_IsActorEvent(ObjectEventType type, Object* obj) {
    return (type == OBJECT_TYPE_ACTOR) && (obj->id == OBJ_ACTOR_EVENT);
}

// "collidable" for solid hitbox records, "sphere" for the hand-written sphere test
// (Object_GetSphereCollider — the object carries gNoHitbox yet still collides), "none"
// otherwise. Keeps the log in step with what Object_IsObstacle treats as collidable.
static const char* ObjectSpawnLog_HitboxLabel(ObjectEventType type, void* object) {
    f32 radius;
    if (Object_HasCollidableHitbox(type, object)) {
        return "collidable";
    }
    if (Object_GetSphereCollider(type, object, &radius)) {
        return "sphere";
    }
    return "none";
}

static void ObjectSpawnLog_Emit(ObjectEventType type, void* object, bool cancelled) {
    Object* obj = (Object*) object;
    if (ObjectSpawnLog_IsActorEvent(type, obj)) {
        s16 eventType = ((Actor*) object)->eventType;
        SPDLOG_TRACE(
            "[spawn] type={} id={} ({}) event={}({}) pos=({:.1f},{:.1f},{:.1f}) hitbox={} level={}({}) status={}",
            ObjectSpawnLog_TypeName(type), obj->id, ObjectId_GetName(obj->id), EventId_GetName(eventType), eventType,
            obj->pos.x, obj->pos.y, obj->pos.z, ObjectSpawnLog_HitboxLabel(type, object),
            Starship_LevelName(gCurrentLevel), (int) gCurrentLevel, cancelled ? "FILTERED" : "PASSED");
        return;
    }
    SPDLOG_TRACE("[spawn] type={} id={} ({}) pos=({:.1f},{:.1f},{:.1f}) hitbox={} level={}({}) status={}",
                 ObjectSpawnLog_TypeName(type), obj->id, ObjectId_GetName(obj->id), obj->pos.x, obj->pos.y, obj->pos.z,
                 ObjectSpawnLog_HitboxLabel(type, object), Starship_LevelName(gCurrentLevel), (int) gCurrentLevel,
                 cancelled ? "FILTERED" : "PASSED");
}

// Per-slot state for OBJ_ACTOR_EVENT logging.
//
// ActorEvent_Load (fox_enmy.c:396) spawns into OBJ_ACTIVE bypassing OBJ_INIT,
// then calls Actor_Update on the same line. Inside Actor_Update the
// ObjectUpdateEvent fires *before* Actor_Move runs (fox_enmy.c:2873-2880), so
// at our ObjectUpdateEvent listener the actor's eventType is always EVID_FFF
// — the script hasn't yet executed EVOP_INIT_ACTOR (fox_enmy2.c:1132-1134).
// Logging there alone would never resolve eventType for actor events that
// die inside their first frame (formation-trigger / spawn-leader style),
// because there is no second ObjectUpdate tick.
//
// Two-stage scheme:
//   1. ObjectUpdateEvent sets sActorEventSeenThisFrame[slot] = true.
//   2. GamePostUpdateEvent (fox_game.c:614, fires once per tick after
//      Play_Update) scans, reads each seen slot's now-resolved eventType,
//      and logs if it differs from the last logged value.
//
// sActorEventLoggedType uses -1 as "never logged" sentinel so the first
// observation of any real eventType (including EVID_FFF, in the rare case a
// script truly never resolved) emits a line. Idle slots (not seen this
// frame) reset to -1 so a fresh occupant always re-logs — even if its
// eventType happens to match the previous occupant's last value.
//
// sActorEventFilteredThisFrame records whether a NORMAL-priority listener
// (e.g. AccessibilityTrainingMinimal) cancelled this slot's update event
// this frame. When set, GamePostUpdate emits a FILTERED line bypassing the
// eventType-dedup check — otherwise the cancellation would be silent in
// the log: cancellation happens on the second tick (after EVOP_INIT_ACTOR
// resolved hitbox + targetOffset), by which time the eventType already
// matches the value logged on the first tick.
//
// Side effect: actor-event slots that get filtered emit two lines — a
// PASSED line on tick 1 (when the filter genuinely can't act yet because
// hitbox/targetOffset aren't installed), then a FILTERED line on tick 2.
// Accepted as a clarity tradeoff over a one-frame delayed emit.
static bool sActorEventSeenThisFrame[ARRAY_COUNT(gActors)];
static bool sActorEventFilteredThisFrame[ARRAY_COUNT(gActors)];
static s32 sActorEventLoggedType[ARRAY_COUNT(gActors)];

static void ObjectSpawnLog_OnObjectInit(IEvent* event) {
    if (!ObjectSpawnLog_IsEnabled()) {
        return;
    }
    ObjectInitEvent* e = (ObjectInitEvent*) event;
    ObjectSpawnLog_Emit(e->type, e->object, event->cancelled);
}

static void ObjectSpawnLog_OnObjectUpdate(IEvent* event) {
    if (!ObjectSpawnLog_IsEnabled()) {
        return;
    }
    ObjectUpdateEvent* e = (ObjectUpdateEvent*) event;
    if (e->type != OBJECT_TYPE_ACTOR) {
        return;
    }
    Object* obj = (Object*) e->object;
    if (obj->id != OBJ_ACTOR_EVENT) {
        return;
    }
    s32 slot = (s32) (((Actor*) e->object) - gActors);
    if ((slot < 0) || (slot >= (s32) ARRAY_COUNT(gActors))) {
        return;
    }
    sActorEventSeenThisFrame[slot] = true;
    if (event->cancelled) {
        sActorEventFilteredThisFrame[slot] = true;
    }
}

static void ObjectSpawnLog_OnGamePostUpdate(IEvent* event) {
    (void) event;
    if (!ObjectSpawnLog_IsEnabled()) {
        return;
    }
    for (s32 i = 0; i < (s32) ARRAY_COUNT(gActors); i++) {
        if (!sActorEventSeenThisFrame[i]) {
            // Slot wasn't active this frame; clear tracker so a future
            // occupant always logs fresh, even if its eventType matches
            // the previous occupant.
            sActorEventLoggedType[i] = -1;
            sActorEventFilteredThisFrame[i] = false;
            continue;
        }
        sActorEventSeenThisFrame[i] = false;
        bool filtered = sActorEventFilteredThisFrame[i];
        sActorEventFilteredThisFrame[i] = false;
        Actor* a = &gActors[i];
        if (a->obj.id != OBJ_ACTOR_EVENT) {
            // Slot was actor-event during the frame but has been re-purposed
            // before post-update (unusual). Reset and skip.
            sActorEventLoggedType[i] = -1;
            continue;
        }
        s32 eventType = a->eventType;
        if (filtered) {
            // Emit a FILTERED line on the tick the filter actually fires,
            // bypassing dedup — without this the cancellation is silent
            // because eventType matches the value logged on tick 1.
            sActorEventLoggedType[i] = eventType;
            ObjectSpawnLog_Emit(OBJECT_TYPE_ACTOR, a, true);
            continue;
        }
        if (eventType == sActorEventLoggedType[i]) {
            continue;
        }
        sActorEventLoggedType[i] = eventType;
        ObjectSpawnLog_Emit(OBJECT_TYPE_ACTOR, a, false);
    }
}

// EVENT_PRIORITY_HIGH so the Init listener runs after the TrainingMinimal
// filter (NORMAL) and can read event->cancelled to report the PASSED/FILTERED
// tag.
//
// Output is at LUSLOG_TRACE, which is filtered by the gDeveloperTools.LogLevel
// runtime threshold (defaults to debug — see CLAUDE.md Logging section).
// Set the CVar to 0 / pick "trace" in the dev menu to actually see the lines.
//
// Readable names for `obj->id` come from ObjectId_GetName, defined in
// ObjectIdNames.generated.c — produced at CMake configure time from
// include/sf64object.h by cmake/GenerateObjectIdNames.cmake.
// Readable names for OBJ_ACTOR_EVENT `eventType` come from EventId_GetName,
// defined in EventIdNames.generated.c — produced from include/sf64event.h by
// cmake/GenerateEventIdNames.cmake.
void ObjectSpawnLog_Init() {
    CVarRegisterInteger("gObjectSpawnLog", 0);
    for (s32 i = 0; i < (s32) ARRAY_COUNT(sActorEventLoggedType); i++) {
        sActorEventLoggedType[i] = -1;
        sActorEventSeenThisFrame[i] = false;
        sActorEventFilteredThisFrame[i] = false;
    }
    REGISTER_LISTENER(ObjectInitEvent, ObjectSpawnLog_OnObjectInit, EVENT_PRIORITY_HIGH);
    REGISTER_LISTENER(ObjectUpdateEvent, ObjectSpawnLog_OnObjectUpdate, EVENT_PRIORITY_HIGH);
    REGISTER_LISTENER(GamePostUpdateEvent, ObjectSpawnLog_OnGamePostUpdate, EVENT_PRIORITY_HIGH);
}
