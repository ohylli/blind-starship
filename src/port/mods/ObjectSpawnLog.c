#include "ObjectSpawnLog.h"

#include "global.h"
#include "ObjectQuery.h"
#include "log/luslog.h"
#include "port/hooks/Events.h"

static bool ObjectSpawnLog_IsEnabled(void) {
    return CVarGetInteger("gObjectSpawnLog", 0) == 1;
}

static const char* ObjectSpawnLog_TypeName(ObjectEventType type) {
    switch (type) {
        case OBJECT_TYPE_ACTOR:       return "ACTOR";
        case OBJECT_TYPE_ACTOR_EVENT: return "ACTOR_EVENT";
        case OBJECT_TYPE_BOSS:        return "BOSS";
        case OBJECT_TYPE_SCENERY:     return "SCENERY";
        case OBJECT_TYPE_SCENERY360:  return "SCENERY360";
        case OBJECT_TYPE_SPRITE:      return "SPRITE";
        case OBJECT_TYPE_ITEM:        return "ITEM";
        case OBJECT_TYPE_EFFECT:      return "EFFECT";
        default:                      return "UNKNOWN";
    }
}

static const char* ObjectSpawnLog_LevelName(s32 level) {
    switch (level) {
        case LEVEL_CORNERIA:       return "LEVEL_CORNERIA";
        case LEVEL_METEO:          return "LEVEL_METEO";
        case LEVEL_SECTOR_X:       return "LEVEL_SECTOR_X";
        case LEVEL_AREA_6:         return "LEVEL_AREA_6";
        case LEVEL_UNK_4:          return "LEVEL_UNK_4";
        case LEVEL_SECTOR_Y:       return "LEVEL_SECTOR_Y";
        case LEVEL_VENOM_1:        return "LEVEL_VENOM_1";
        case LEVEL_SOLAR:          return "LEVEL_SOLAR";
        case LEVEL_ZONESS:         return "LEVEL_ZONESS";
        case LEVEL_VENOM_ANDROSS:  return "LEVEL_VENOM_ANDROSS";
        case LEVEL_TRAINING:       return "LEVEL_TRAINING";
        case LEVEL_MACBETH:        return "LEVEL_MACBETH";
        case LEVEL_TITANIA:        return "LEVEL_TITANIA";
        case LEVEL_AQUAS:          return "LEVEL_AQUAS";
        case LEVEL_FORTUNA:        return "LEVEL_FORTUNA";
        case LEVEL_UNK_15:         return "LEVEL_UNK_15";
        case LEVEL_KATINA:         return "LEVEL_KATINA";
        case LEVEL_BOLSE:          return "LEVEL_BOLSE";
        case LEVEL_SECTOR_Z:       return "LEVEL_SECTOR_Z";
        case LEVEL_VENOM_2:        return "LEVEL_VENOM_2";
        case LEVEL_VERSUS:         return "LEVEL_VERSUS";
        case LEVEL_WARP_ZONE:      return "LEVEL_WARP_ZONE";
        default:                   return "LEVEL_UNKNOWN";
    }
}

static void ObjectSpawnLog_Emit(ObjectEventType type, void* object, bool cancelled) {
    Object* obj = (Object*) object;
    LUSLOG_TRACE("[spawn] type=%s id=%d (%s) pos=(%.1f,%.1f,%.1f) hitbox=%s level=%s(%d) status=%s",
                 ObjectSpawnLog_TypeName(type),
                 obj->id,
                 ObjectId_GetName(obj->id),
                 obj->pos.x, obj->pos.y, obj->pos.z,
                 Object_HasCollidableHitbox(type, object) ? "collidable" : "none",
                 ObjectSpawnLog_LevelName(gCurrentLevel), gCurrentLevel,
                 cancelled ? "FILTERED" : "PASSED");
}

// Dedup bitmap for actor-event Update logging. ActorEvent_Load spawns into
// OBJ_ACTIVE bypassing OBJ_INIT, so the only way to see one is via Update.
// We log the first Update per actor-event slot and skip the rest; the bit
// clears when the filter cancels the slot (typical end-of-life under the
// training mod). Slots that die naturally (engine sets OBJ_FREE outside
// our hooks) leave a stale bit; the next actor-event in that slot would be
// missed. Acceptable for a diagnostic log — Init-based logging covers
// every other type without this caveat.
static bool sActorEventSeen[ARRAY_COUNT(gActors)];

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

    if (event->cancelled) {
        if (!sActorEventSeen[slot]) {
            ObjectSpawnLog_Emit(e->type, e->object, true);
        }
        sActorEventSeen[slot] = false;
        return;
    }
    if (sActorEventSeen[slot]) {
        return;
    }
    sActorEventSeen[slot] = true;
    ObjectSpawnLog_Emit(e->type, e->object, false);
}

// EVENT_PRIORITY_HIGH so this runs after the TrainingMinimal filter (NORMAL)
// and can read event->cancelled to report the PASSED/FILTERED tag.
//
// Output is at LUSLOG_TRACE, which is filtered by the gDeveloperTools.LogLevel
// runtime threshold (defaults to debug — see CLAUDE.md Logging section).
// Set the CVar to 0 / pick "trace" in the dev menu to actually see the lines.
//
// Readable names for `obj->id` come from ObjectId_GetName, defined in
// ObjectIdNames.generated.c — produced at CMake configure time from
// include/sf64object.h by cmake/GenerateObjectIdNames.cmake.
void ObjectSpawnLog_Init(void) {
    CVarRegisterInteger("gObjectSpawnLog", 0);
    REGISTER_LISTENER(ObjectInitEvent, ObjectSpawnLog_OnObjectInit, EVENT_PRIORITY_HIGH);
    REGISTER_LISTENER(ObjectUpdateEvent, ObjectSpawnLog_OnObjectUpdate, EVENT_PRIORITY_HIGH);
}
