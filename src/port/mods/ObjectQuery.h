#pragma once

#include <math.h>

#include "global.h"
#include "port/hooks/Events.h"

#ifdef __cplusplus
extern "C" {
#endif

// The ObjectInfo of one live world-array entry, or NULL for types that carry none.
// The wrappers all embed `Object obj; ObjectInfo info;` but only by convention, so the
// switch keeps the access type-safe instead of punning through Object*.
static inline ObjectInfo* Object_GetInfo(ObjectEventType type, void* object) {
    switch (type) {
        case OBJECT_TYPE_ACTOR:
            return &((Actor*) object)->info;
        case OBJECT_TYPE_BOSS:
            return &((Boss*) object)->info;
        case OBJECT_TYPE_SCENERY:
            return &((Scenery*) object)->info;
        case OBJECT_TYPE_SCENERY360:
            return &((Scenery360*) object)->info;
        default:
            return NULL;
    }
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
// fighter-type events) runs inside info.action — i.e. after a Update listener
// fires. So on the first Update tick every actor-event still has
// gObjectInfo[OBJ_ACTOR_EVENT]'s default gNoHitbox; on the second tick
// fighters have their real hitbox while pure triggers keep gNoHitbox.
static inline bool Object_HasCollidableHitbox(ObjectEventType type, void* object) {
    ObjectInfo* info = Object_GetInfo(type, object);
    if ((info == NULL) || (info->hitbox == NULL)) {
        return false;
    }
    return info->hitbox[0] != 0.0f;
}

// Is this object a collidable, non-lockable obstacle — something the player can crash
// into but cannot shoot down? Classification only: deliberately NO obj.status check, so
// the same predicate serves both event-time filtering (AccessibilityTrainingMinimal
// calls it from ObjectInitEvent, where status is still OBJ_INIT) and world scans (the
// obstacle cue adds its own status == OBJ_ACTIVE test). Shared by
// AccessibilityTrainingMinimal (which strips exactly this set from Training) and the
// obstacle-ahead cue (which warns about exactly this set) — that agreement is why it
// lives here.
//
// For actors (regular or OBJ_ACTOR_EVENT), enemies are distinguished from environmental
// obstacles by info.targetOffset: enemies have a non-zero lock-on offset (the engine's
// own missile lock-on predicate, PlayerShot_FindLockTarget in fox_beam.c, mirrored by
// the enemy cue's CueScan_IsCueableEnemy). Obstacles like EVID_TR_BARRIER (the training
// barrier) have targetOffset == 0.0f even though they carry a collidable hitbox; those
// are exactly what this classifies as obstacles. For OBJ_ACTOR_EVENT actors
// info.targetOffset is the default 0.0f until the script runs EVOP_INIT_ACTOR (which
// also installs the real hitbox — see the second-tick note above), so the hitbox and
// targetOffset land together and the check is consistent. Scenery is never lockable and
// skips the test. Bosses deliberately get NO targetOffset test: nearly every gBosses
// entry has targetOffset == 0 even though it is shootable (gObjectInfo,
// fox_edata_info.c — Sarumarine is the lone lockable one), so the field cannot separate
// "boss you fight" from "boss-shaped wall" and any collidable boss classifies as an
// obstacle here. Consumers that don't want bosses (the obstacle cue: a boss fight is not
// a crash warning) exclude the array wholesale as policy — see ObstacleScan.h.
static inline bool Object_IsObstacle(ObjectEventType type, void* object) {
    if ((type != OBJECT_TYPE_ACTOR) && (type != OBJECT_TYPE_SCENERY) && (type != OBJECT_TYPE_SCENERY360) &&
        (type != OBJECT_TYPE_BOSS)) {
        return false;
    }
    if (!Object_HasCollidableHitbox(type, object)) {
        return false;
    }
    if (type == OBJECT_TYPE_ACTOR) {
        if (Object_GetInfo(type, object)->targetOffset != 0.0f) {
            return false;
        }
    }
    return true;
}

// One decoded solid hitbox record: the plain axis-aligned box with the sentinel variants
// resolved. Offsets are relative to obj.pos; sizes are HALF extents (Hitbox in
// sf64object.h, docs/game-world.md section 7). `record` is the 0-based index in the
// source array's record order (the engine's collision hit index is the same plus one).
typedef struct HitboxBox {
    f32 zOffset, zHalf;
    f32 yOffset, yHalf;
    f32 xOffset, xHalf;
    s32 record;
} HitboxBox;

// Sanity ceiling on the record count read out of info.hitbox[0]. Real hitboxes have a
// handful of records (the largest hand-written ones stay well under this); anything
// larger means the ObjectInfo isn't installed yet, or the pointer is garbage.
#define kObjectMaxHitboxRecords 32

// Decode the solid (collidable) records of a flat info.hitbox array: shadow (screen
// dimming) and whoosh (near-miss sound) records are skipped — the engine tests them but
// they never damage the player — and the rotated variant's box is used with its rotation
// ignored (slightly wrong footprint for yawed walls; callers absorb it with margins).
// Writes at most maxOut records, returns how many. The stride arithmetic mirrors
// Player_CheckHitboxCollision (fox_play.c:1270-1291) exactly: 6 floats per record, +4
// extra for HITBOX_ROTATED (sentinel + 3 rotation floats), +1 extra for records at or
// above HITBOX_SHADOW (shadow and whoosh), so a data variant the engine understands can
// never desynchronize this walk. Records with a non-finite field are dropped —
// info.hitbox is runtime-mutable (see fox_andross.c) and a NaN half-extent would ride
// straight into a caller's distance math and from there into JSON dumps.
static inline s32 Object_ReadSolidHitboxes(const f32* hitbox, HitboxBox* out, s32 maxOut) {
    const f32* rec;
    s32 count;
    s32 written = 0;
    s32 i;

    if ((hitbox == NULL) || (out == NULL) || (maxOut <= 0)) {
        return 0;
    }
    count = (s32) hitbox[0];
    if ((count <= 0) || (count > kObjectMaxHitboxRecords)) {
        return 0;
    }
    rec = hitbox + 1;
    for (i = 0; i < count; i++, rec += 6) {
        if (rec[0] == HITBOX_ROTATED) {
            rec += 4; // sentinel + xRot/yRot/zRot skipped; the loop's += 6 completes the 10-float stride
        } else if (rec[0] >= HITBOX_SHADOW) {
            rec += 1; // shadow or whoosh; the += 6 completes the 7-float stride
            continue;
        }
        if (written >= maxOut) {
            break;
        }
        if (!isfinite(rec[0]) || !isfinite(rec[1]) || !isfinite(rec[2]) || !isfinite(rec[3]) || !isfinite(rec[4]) ||
            !isfinite(rec[5])) {
            continue;
        }
        out[written].zOffset = rec[0];
        out[written].zHalf = fabsf(rec[1]);
        out[written].yOffset = rec[2];
        out[written].yHalf = fabsf(rec[3]);
        out[written].xOffset = rec[4];
        out[written].xHalf = fabsf(rec[5]);
        out[written].record = i;
        written++;
    }
    return written;
}

#ifdef __cplusplus
}
#endif
