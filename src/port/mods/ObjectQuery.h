#pragma once

#include "global.h"
#include "port/hooks/Events.h"

#ifdef __cplusplus
extern "C" {
#endif

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
// fighter-type events) runs inside info.action — i.e. after a Update listener
// fires. So on the first Update tick every actor-event still has
// gObjectInfo[OBJ_ACTOR_EVENT]'s default gNoHitbox; on the second tick
// fighters have their real hitbox while pure triggers keep gNoHitbox.
static inline bool Object_HasCollidableHitbox(ObjectEventType type, void* object) {
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

#ifdef __cplusplus
}
#endif
