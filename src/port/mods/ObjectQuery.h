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
// meaning "no records" — no hitbox-record collision with the player. Such an object
// either cannot collide at all (purely decorative scenery, or a pure script trigger
// like an actor-event with EVID_EVENT_HANDLER, used to play radio messages) or collides
// through one of the engine's other two mechanisms: a poly mesh (Object_GetPolyCollider
// below) or the hand-written sphere test (Object_GetSphereCollider below).
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

// The engine's SECOND collision mechanism: a poly mesh. Player_CollisionCheck
// (fox_play.c) routes a fixed set of object ids to Player_CheckPolyCollision INSTEAD of
// the hitbox test — for those ids the hitbox is never consulted for the player, whether
// it is gNoHitbox (most of them) or a real one the engine simply ignores (the Sector Y
// capital ship, Zoness's island, Aquas's coral reefs). The id -> mesh map itself is the
// engine's Play_GetPolyColId; what this function adds is the DISPATCH lists, mirrored
// from the call sites in Player_CollisionCheck so a new entry means finding another id
// there:
//   - gScenery, on rails (the scenery loop's poly branch): the Corneria bumps 1-5,
//     Zoness's island, Aquas's coral reefs 1-2 and bumps 1-2.
//   - gScenery360, all-range (the scenery360 loop): Aquas coral reef 1, the Katina
//     base, the Versus pyramids, Fortuna's mountains 1-3, Venom 2's mountain, Corneria
//     bumps 1 and 3.
//   - gActors (the actor loop): Meteo's molar rock, and OBJ_ACTOR_EVENT actors with
//     eventType EVID_SY_SHIP_2 (the Sector Y capital ships), which the engine looks up
//     under the pseudo-id ACTOR_EVENT_ID.
//   - gBosses (the boss loop): the Fortuna, Venom 2 and Bolse bases and the Great Fox.
//     Tabled for the predicate's completeness; the obstacle cue excludes gBosses
//     wholesale as policy (ObstacleScan.h).
// Returns true and writes the mesh's index (`colId`) and which of the two header tables
// it lives in (`useCol2`: D_800D2CA0 / CollisionHeader2 vs D_800D2B38 /
// CollisionHeader, fox_colheaders.c) when the object is one. The two tables are NOT the
// same kind of collision: the CollisionHeader family (func_col1_800998FC) is a solid
// swept-triangle test, while the CollisionHeader2 family (func_col2_800A3690) is a
// HEIGHTFIELD — the engine finds the triangle under the player's X/Z and hits only when
// the player's Y is at or below that surface (fox_col2.c). Consumers that reason about
// the shape must keep the two apart; Object_GetPolyBounds below is only the box.
static inline bool Object_GetPolyCollider(ObjectEventType type, void* object, s32* colId, bool* useCol2) {
    s32 objId; // obj.id is a u16 and ACTOR_EVENT_ID a plain literal; cast to the enum only at the engine call
    bool dispatched = false;

    switch (type) {
        case OBJECT_TYPE_SCENERY:
            objId = ((Scenery*) object)->obj.id;
            dispatched = (objId == OBJ_SCENERY_CO_BUMP_1) || (objId == OBJ_SCENERY_CO_BUMP_2) ||
                         (objId == OBJ_SCENERY_CO_BUMP_3) || (objId == OBJ_SCENERY_CO_BUMP_4) ||
                         (objId == OBJ_SCENERY_CO_BUMP_5) || (objId == OBJ_SCENERY_ZO_ISLAND) ||
                         (objId == OBJ_SCENERY_AQ_CORAL_REEF_1) || (objId == OBJ_SCENERY_AQ_CORAL_REEF_2) ||
                         (objId == OBJ_SCENERY_AQ_BUMP_1) || (objId == OBJ_SCENERY_AQ_BUMP_2);
            break;
        case OBJECT_TYPE_SCENERY360:
            objId = ((Scenery360*) object)->obj.id;
            dispatched = (objId == OBJ_SCENERY_AQ_CORAL_REEF_1) || (objId == OBJ_SCENERY_VS_KA_FLBASE) ||
                         (objId == OBJ_SCENERY_VS_PYRAMID_1) || (objId == OBJ_SCENERY_VS_PYRAMID_2) ||
                         (objId == OBJ_SCENERY_FO_MOUNTAIN_1) || (objId == OBJ_SCENERY_FO_MOUNTAIN_2) ||
                         (objId == OBJ_SCENERY_FO_MOUNTAIN_3) || (objId == OBJ_SCENERY_VE2_MOUNTAIN) ||
                         (objId == OBJ_SCENERY_CO_BUMP_1) || (objId == OBJ_SCENERY_CO_BUMP_3);
            break;
        case OBJECT_TYPE_ACTOR: {
            Actor* actor = (Actor*) object;
            objId = actor->obj.id;
            if (objId == OBJ_ACTOR_ME_MOLAR_ROCK) {
                dispatched = true;
            } else if ((objId == OBJ_ACTOR_EVENT) && (actor->eventType == EVID_SY_SHIP_2)) {
                objId = ACTOR_EVENT_ID;
                dispatched = true;
            }
            break;
        }
        case OBJECT_TYPE_BOSS:
            objId = ((Boss*) object)->obj.id;
            dispatched = (objId == OBJ_BOSS_VE2_BASE) || (objId == OBJ_BOSS_FO_BASE) ||
                         (objId == OBJ_BOSS_SZ_GREAT_FOX) || (objId == OBJ_BOSS_BO_BASE);
            break;
        default:
            break;
    }
    if (!dispatched) {
        return false;
    }
    *colId = Play_GetPolyColId((ObjectId) objId, useCol2);
    return true;
}

// The horizontal range gate the engine applies BEFORE running a scenery piece's poly
// test: Player_CollisionCheck only calls Player_CheckPolyCollision when obj.pos is
// within this XZ distance of the ship's center (pos.x / trueZpos) — 1100 in the
// on-rails gScenery loop, and in the gScenery360 loop 1100, or 4000 on Sector Y and
// Venom-Andross (both literals sit at the top of those loops in Player_CollisionCheck).
// The actor and boss loops have no such gate. Returns 0 for "ungated". A consumer that
// projects a course through the mesh (the obstacle cue) honors it so a big mesh's far
// corners, which the engine never tests, do not warn.
static inline f32 Object_GetPolyCollisionRangeXZ(ObjectEventType type) {
    switch (type) {
        case OBJECT_TYPE_SCENERY:
            return 1100.0f;
        case OBJECT_TYPE_SCENERY360:
            return ((gCurrentLevel == LEVEL_SECTOR_Y) || (gCurrentLevel == LEVEL_VENOM_ANDROSS)) ? 4000.0f : 1100.0f;
        default:
            return 0.0f;
    }
}

// The bounding box of a poly mesh from Object_GetPolyCollider, as the engine stores it:
// min/max corners relative to obj.pos in the object's LOCAL frame (the engine rotates
// the player's offset by -obj.rot.y before testing, Player_CheckPolyCollision). The
// header arrays are plain in-process data (fox_colheaders.c, included into fox_col2.c);
// only their polys/mesh fields are asset references, and those are not touched here.
// False for an index outside the table (Col_GetPolyBounds owns the sizes) — unreachable
// through Object_GetPolyCollider, whose ids all map into range, but a caller holding a
// colId it did not get from there (say a sentinel -1) must not index blindly.
static inline bool Object_GetPolyBounds(s32 colId, bool useCol2, Vec3f* min, Vec3f* max) {
    return Col_GetPolyBounds(colId, useCol2, min, max);
}

// A CollisionHeader2 heightfield mesh resolved once for repeated point probes, in the
// producing object's frame. Object_ResolvePolyHeightfield fills it; the asset tables
// are resolved here and nowhere per probe (Col2_ResolveMesh explains the cost). sn/cs
// are the terms of the rotation the engine applies before testing — Player_CheckPolyCollision
// runs Matrix_RotateY(-obj.rot.y) over gCalcMatrix — multiplied out, so a probe never
// touches the shared scratch matrix or the frame-interpolation recording behind it.
typedef struct PolyHeightfield {
    CollisionHeader2* header;
    Triangle* polys;
    Vec3f* mesh;
    Vec3f objPos;
    f32 sn, cs;
} PolyHeightfield;

static inline bool Object_ResolvePolyHeightfield(s32 colId, const Vec3f* objPos, f32 rotY, PolyHeightfield* out) {
    if (!Col2_ResolveMesh(colId, &out->header, &out->polys, &out->mesh)) {
        return false;
    }
    out->objPos = *objPos;
    out->sn = sinf(-rotY * M_DTOR);
    out->cs = cosf(-rotY * M_DTOR);
    return true;
}

// Is a world-space point at or below the heightfield's surface? The engine's own test
// (Col2_CheckSurface, the body of the func_col2_800A36FC the collision pass calls) on
// the point rotated into the mesh's frame: the row-vector product of MTXF_NEW's
// RotateY (m[0][0] = cs, m[0][2] = -sn, m[2][0] = sn, m[2][2] = cs) written out. Pure —
// reads gCurrentLevel, writes nothing the engine owns.
static inline bool Object_PolyHeightfieldHit(const PolyHeightfield* hf, const Vec3f* worldPoint) {
    f32 rx = worldPoint->x - hf->objPos.x;
    f32 rz = worldPoint->z - hf->objPos.z;
    Vec3f probe = { hf->objPos.x + hf->cs * rx + hf->sn * rz, worldPoint->y, hf->objPos.z - hf->sn * rx + hf->cs * rz };
    Vec3f objPos = hf->objPos; // the engine API takes non-const pointers
    Vec3f hitData;
    return Col2_CheckSurface(&probe, &objPos, hf->header, hf->polys, hf->mesh, &hitData);
}

// The heightfield's surface height under a world-space X/Z, in ONE probe: the world Y at
// or below which Object_PolyHeightfieldHit reports a hit there. Col2_CheckSurface
// computes that height on the way to its verdict — it finds the triangle under the
// point, evaluates the triangle's plane at the point's X/Z into hitData.y, and only then
// compares the point's Y against it — and writes it whether or not the point is low
// enough to hit (the engine's own Player_FloorCheck reads the same output to seat the
// ship's shadow on a bump). So the caller supplies no Y: the probe's own Y only has to
// pass the bounds check, and it is placed mid-box.
//
// The value is the engine's crash threshold, not the drawn surface: hitData.y is in the
// mesh's LOCAL frame and the engine compares the WORLD Y against it without adding
// obj.pos.y back (a quirk invisible for the bumps, which sit at y = 0, and up to 80 units
// on Zoness's islands), and it is truncated to an integer (func_col1_800988B4 returns
// s32). Both are deliberate here — "how far above a crash am I" wants the number the
// crash test uses.
//
// False when no triangle lies under the point (outside the mesh's outline or its box):
// hitData.y is preset to NaN, which the range test below rejects. A degenerate (vertical)
// triangle, whose plane formula divides by zero, yields a NaN the engine's s32 return
// converts with undefined behaviour — INT_MIN on x86, which the range test also rejects,
// but 0 on AArch64, which it would accept for a mesh whose box straddles y = 0. Reaching
// it takes the probe's X/Z landing exactly on the triangle's projected line, and the
// conversion is the engine's own (Object_PolyHeightfieldHit runs the same code), so it is
// noted, not guarded.
static inline bool Object_PolyHeightfieldSurfaceY(const PolyHeightfield* hf, f32 worldX, f32 worldZ, f32* surfaceY) {
    f32 rx = worldX - hf->objPos.x;
    f32 rz = worldZ - hf->objPos.z;
    Vec3f probe = { hf->objPos.x + hf->cs * rx + hf->sn * rz,
                    hf->objPos.y + 0.5f * (hf->header->min.y + hf->header->max.y),
                    hf->objPos.z - hf->sn * rx + hf->cs * rz };
    Vec3f objPos = hf->objPos; // the engine API takes non-const pointers
    Vec3f hitData = { 0.0f, NAN, 0.0f };
    Col2_CheckSurface(&probe, &objPos, hf->header, hf->polys, hf->mesh, &hitData);
    // One unit of slack for the integer truncation at the box's faces.
    if (!((hitData.y >= hf->header->min.y - 1.0f) && (hitData.y <= hf->header->max.y + 1.0f))) {
        return false;
    }
    *surfaceY = hitData.y;
    return true;
}

// The engine's THIRD collision mechanism, beside hitbox records and poly meshes: a few
// actor-event types carry gNoHitbox and are instead collided by a hand-written sphere
// test keyed on eventType, with the radius a literal in the collision loop rather than
// data on the object. The table below mirrors those literals; a new entry means finding
// another such special case in Player_CollisionCheck's actor loop.
// Returns true and writes the PLAYER-collision radius (the crash distance, which is what
// an obstacle warning cares about; the shot-collision radius may differ) when the object
// is one. Only OBJ_ACTOR_EVENT actors are tabled here, so every other type is a fast
// false. Bosses have sphere cases of their own in Player_CollisionCheck's boss loop —
// OBJ_BOSS_BO_BASE_SHIELD, 1500 units and gNoHitbox so the sphere is its only collision,
// and OBJ_BOSS_KA_SAUCERER, 2700 units on top of a real hitbox — deliberately NOT
// tabled: the only consumer that would see them, the obstacle cue, excludes gBosses
// wholesale as policy (ObstacleScan.h).
//
//   EVID_ME_BIG_METEOR — Meteo's bouncing big meteor: 900 units around obj.pos for the
//   player (Player_CollisionCheck's actor loop, VEC3F_MAG against pos.x/pos.y/trueZpos),
//   1000 for player shots (PlayerShot_CollisionCheck, fox_beam.c). Non-lockable
//   (targetOffset 0 in its sEventActorInfo row, fox_enmy2.c) and destructible (script
//   health 500).
static inline bool Object_GetSphereCollider(ObjectEventType type, void* object, f32* radius) {
    if (type != OBJECT_TYPE_ACTOR) {
        return false;
    }
    Actor* actor = (Actor*) object;
    if (actor->obj.id != OBJ_ACTOR_EVENT) {
        return false;
    }
    switch (actor->eventType) {
        case EVID_ME_BIG_METEOR:
            *radius = 900.0f;
            return true;
        default:
            return false;
    }
}

// Is this object a collidable, non-lockable obstacle — something the player can crash
// into but cannot shoot down? "Collidable" is a hitbox with solid records, OR a poly
// mesh (Object_GetPolyCollider above), OR one of the sphere-collided event types
// (Object_GetSphereCollider above) — the latter two mostly carry gNoHitbox and would
// otherwise be misread as pure triggers. Classification only: deliberately NO
// obj.status check, so the same predicate serves both event-time filtering
// (AccessibilityTrainingMinimal calls it from ObjectInitEvent, where status is still
// OBJ_INIT) and world scans (the obstacle cue adds its own status == OBJ_ACTIVE test).
// Shared by
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
    f32 sphereRadius;
    s32 polyColId;
    bool polyUseCol2;
    if (!Object_HasCollidableHitbox(type, object) && !Object_GetPolyCollider(type, object, &polyColId, &polyUseCol2) &&
        !Object_GetSphereCollider(type, object, &sphereRadius)) {
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
// Player_CheckHitboxCollision's record walk (fox_play.c) exactly: 6 floats per record, +4
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
