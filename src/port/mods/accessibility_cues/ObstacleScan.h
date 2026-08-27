#pragma once

// The shared obstacle scan: which world objects count as obstacles (the shared
// Object_IsObstacle predicate — collidable hitbox or sphere collider, not lockable —
// minus the cue-side
// exclusions below) and their solid hitbox records as world-space boxes with the
// player-relative geometry every consumer needs already derived. The obstacle-ahead cue
// (its rails footprint test and its all-range ray test) is the first consumer; the
// planned directional obstacle cues (left/right, above/below) are the reason the
// geometry is yielded raw — no thresholding, no margin, no direction naming happens
// here. Every filter beyond "is a warn-worthy obstacle with a solid box" is the
// caller's policy. Game-coupled by design, like CueScan.h.
//
// Cue-side exclusions on top of the predicate (policy shared by the future obstacle
// cues, deliberately NOT in Object_IsObstacle, whose other consumer is
// AccessibilityTrainingMinimal):
//   - gBosses[] is not scanned at all: targetOffset cannot separate "boss you fight"
//     from "wall" (see the predicate's comment in ObjectQuery.h), and droning a crash
//     warning through an on-rails boss fight would bury the aim/enemy cues exactly when
//     they matter most.
//   - Teammate Arwings carry a small collidable hitbox with targetOffset 0, and would
//     otherwise buzz whenever one weaves across your nose. They come in two forms that
//     both need excluding: OBJ_ACTOR_TEAM_BOSS (the escorts placed on-rails for a boss
//     run in Meteo and Area 6) and the OBJ_ACTOR_EVENT actors with
//     eventType == EVID_TEAMMATE that fly the rest of an on-rails level (their event
//     row installs gCubeHitbox100, fox_enmy2.c's sEventActorInfo). OBJ_ACTOR_TEAM_ARWING
//     needs no entry — it has gNoHitbox and never passes the predicate.
//
// Sphere-collided objects (Object_GetSphereCollider — today only Meteo's big meteor,
// which carries gNoHitbox and is collided by a hand-written 900-unit sphere) are boxed:
// one synthesized record centered on obj.pos with half-extents equal to the radius on
// every axis, record index kObstacleSphereRecord. The cube over-warns at its corners
// (a diagonal pass 900-1270 units off center) — accepted, in the same spirit as the
// yawed-wall approximation below: a conservative warning about an 1800-unit rock is
// the right failure direction, and it keeps the course tests and the planned
// directional cues on one box shape.
//
// Deliberate non-features, so a future session doesn't "fix" them by accident: obj.rot
// is ignored (the box is axis-aligned in world space even for a yawed wall), and
// HITBOX_ROTATED's rotation floats are skipped with its box used as-is — both absorbed
// by the caller's safety margin; shadow and whoosh records are dropped entirely
// (Object_ReadSolidHitboxes). Poly-mesh scenery carrying gNoHitbox never appears at all
// (accepted miss — docs/accessibility-obstacle-cue.md). gSprites[] is not scanned in
// either mode (accepted miss, same doc): sprites do collide (Fortuna's poles,
// Corneria's trees — a harmless stagger, fox_play.c's sprite loop), but warning about
// them is a scope decision deferred with the user, not an oversight.

#include <type_traits>

#include "port/CGameCompat.h"
#include "port/mods/ObjectQuery.h"

// Which active-world array a box came from. Reported in the debug mirror so a policy
// decision can be joined against the `objects` command's dumps. BOSS currently has no
// producer (bosses are excluded as policy); it keeps its slot so the debug indices stay
// stable if that ever changes.
enum ObstacleArray {
    OBSTACLE_ARRAY_SCENERY = 0,
    OBSTACLE_ARRAY_ACTOR,
    OBSTACLE_ARRAY_BOSS,
    OBSTACLE_ARRAY_SCENERY360,
};
const char* ObstacleScan_ArrayName(ObstacleArray array); // for JSON / logs

// One solid hitbox record of one obstacle, in world space, plus the player-relative
// geometry. Plain scalars: safe to copy into a debug mirror that outlives the object.
// Frame convention is the game's: +x right, +y up, +z BACKWARD (ahead is -z).
struct ObstacleBox {
    ObstacleArray array;
    s32 slot;     // index in that array
    s32 objId;    // obj.id
    s32 record;   // hitbox record index, 0-based (the engine's hit index is this + 1), or
                  // kObstacleSphereRecord for a box synthesized from a sphere collider
    Vec3f center; // obj.pos + the record's offsets; with `half`, the full box in world
                  // space. Consumed here to derive dx/dy/dz; kept public for debug joins
                  // and the planned directional cues — no course test reads it today.
    Vec3f half;   // record half-extents, non-negative
    f32 dx, dy;   // center minus player (pos.x / pos.y); the directional cues' signal
    f32 dz;       // center minus player trueZpos — with dx/dy the full 3D center delta
                  // the all-range ray test casts along (negative = ahead of the player)
    // How far OUTSIDE the footprint the player is on each axis: fabsf(d) - half.
    // Negative means the player is inside the footprint on that axis. A collision-course
    // test is "both below the caller's safety margin"; a left/right or above/below cue
    // reads the sign of dx / dy together with these.
    f32 clearX, clearY;
    // Player to the box's NEAR face in Z — the face the player, flying toward -z, meets
    // first: center.z + half.z. Positive while the box is still ahead, <= 0 once the
    // player is level with or past the face. The "distance to impact" a warning maps.
    f32 gapZ;
};

// `record` value of a box synthesized from a sphere collider (see the header comment).
// Negative so it can never collide with a real record index in the `cues` dump.
inline constexpr s32 kObstacleSphereRecord = -1;

struct ObstacleScanStats {
    s32 active;    // occupied OBJ_ACTIVE slots visited
    s32 obstacles; // also passed Object_IsObstacle and the cue-side exclusions
    s32 boxes;     // solid hitbox records yielded
};

namespace ObstacleScanDetail {
// The wingmate exclusion from the header comment. Only Actor carries eventType, so the
// event-actor half of the test is compiled in for that wrapper alone.
template <typename T>
inline bool IsTeammate(const T* entry) {
    if (entry->obj.id == OBJ_ACTOR_TEAM_BOSS) {
        return true;
    }
    if constexpr (std::is_same_v<T, Actor>) {
        return (entry->obj.id == OBJ_ACTOR_EVENT) && (entry->eventType == EVID_TEAMMATE);
    }
    return false;
}

// Yields the boxes of ONE object. T is the array's wrapper type (Scenery/Actor/...);
// they all embed `Object obj; ObjectInfo info;`, and templating on the wrapper keeps the
// access type-safe without hand-passing the pieces.
template <typename T, typename Fn>
inline void EmitBoxes(ObjectEventType type, T* entry, ObstacleArray array, s32 slot, Player* player,
                      ObstacleScanStats* stats, Fn& fn) {
    if (entry->obj.status != OBJ_ACTIVE) {
        return;
    }
    if (stats != nullptr) {
        stats->active++;
    }
    if (!Object_IsObstacle(type, entry)) {
        return;
    }
    if (IsTeammate(entry)) {
        return; // wingmates are not obstacles — see the header comment
    }
    if (stats != nullptr) {
        stats->obstacles++;
    }
    HitboxBox records[kObjectMaxHitboxRecords + 1];
    s32 n = Object_ReadSolidHitboxes(entry->info.hitbox, records, kObjectMaxHitboxRecords);
    f32 sphereRadius;
    if (Object_GetSphereCollider(type, entry, &sphereRadius)) {
        // Boxed sphere — see the header comment. The +1 slot above reserves room so it
        // is never dropped behind a full record walk.
        records[n].zOffset = 0.0f;
        records[n].zHalf = sphereRadius;
        records[n].yOffset = 0.0f;
        records[n].yHalf = sphereRadius;
        records[n].xOffset = 0.0f;
        records[n].xHalf = sphereRadius;
        records[n].record = kObstacleSphereRecord;
        n++;
    }
    for (s32 r = 0; r < n; r++) {
        ObstacleBox box;
        box.array = array;
        box.slot = slot;
        box.objId = entry->obj.id;
        box.record = records[r].record;
        box.center.x = entry->obj.pos.x + records[r].xOffset; // offsets are obj.pos-relative,
        box.center.y = entry->obj.pos.y + records[r].yOffset; // sizes half-extents
        box.center.z = entry->obj.pos.z + records[r].zOffset; // (Play_CheckSingleHitbox, fox_play.c:1249)
        box.half.x = records[r].xHalf;
        box.half.y = records[r].yHalf;
        box.half.z = records[r].zHalf;
        box.dx = box.center.x - player->pos.x;
        box.dy = box.center.y - player->pos.y;
        // trueZpos is the player's real world Z; pos.z is the path scroll (sf64player.h).
        box.dz = box.center.z - player->trueZpos;
        box.clearX = fabsf(box.dx) - box.half.x;
        box.clearY = fabsf(box.dy) - box.half.y;
        box.gapZ = player->trueZpos - (box.center.z + box.half.z);
        if (stats != nullptr) {
            stats->boxes++;
        }
        fn(box);
    }
}
} // namespace ObstacleScanDetail

// gScenery360 is a heap array of exactly this many slots — it is an `extern Scenery360*`
// (sf64context.h) sized by the literal 200 at its Memory_Allocate calls (fox_play.c), so
// ARRAY_COUNT cannot apply.
inline constexpr s32 kScenery360Count = 200;

// Walks gScenery[] and gActors[], plus gScenery360[] in all-range mode (gBosses[] is
// excluded as policy, see the header comment), and calls fn(const ObstacleBox&) once per
// solid hitbox record of every active obstacle. No range cutoff on the all-range walk:
// distance thresholding is the caller's policy (the ahead cue's warnDist already bounds
// it), and 200 extra slots per tick is negligible. `stats` may be null. Cheap: ~310
// slots, a handful of records each, no allocation.
template <typename Fn> void ObstacleScan_ForEachBox(Player* player, ObstacleScanStats* stats, Fn&& fn) {
    if (stats != nullptr) {
        stats->active = 0;
        stats->obstacles = 0;
        stats->boxes = 0;
    }
    for (s32 i = 0; i < ARRAY_COUNT(gScenery); i++) {
        ObstacleScanDetail::EmitBoxes(OBJECT_TYPE_SCENERY, &gScenery[i], OBSTACLE_ARRAY_SCENERY, i, player, stats, fn);
    }
    for (s32 i = 0; i < ARRAY_COUNT(gActors); i++) {
        ObstacleScanDetail::EmitBoxes(OBJECT_TYPE_ACTOR, &gActors[i], OBSTACLE_ARRAY_ACTOR, i, player, stats, fn);
    }
    // The mode gate is load-bearing, not just the null check — but not because the
    // pointer dangles: Memory_FreeAll is a bump-pointer reset over a static buffer
    // (sys_memory.c), so gScenery360 always points at mapped memory. The hazard is that
    // after an all-range level ends the block is REUSED by later allocations, and
    // info.hitbox is a pointer field that would then be dereferenced with arbitrary
    // bits — no null or bounds check can catch that. The safe invariant is to walk the
    // array only while the engine itself walks it under this same mode gate
    // (fox_enmy.c:888). That formulation also covers Venom-Andross, which skips the
    // allocation and reuses a prior level's block in place after re-zeroing its
    // statuses (fox_play.c:7128).
    if ((gLevelMode == LEVELMODE_ALL_RANGE) && (gScenery360 != nullptr)) {
        for (s32 i = 0; i < kScenery360Count; i++) {
            ObstacleScanDetail::EmitBoxes(OBJECT_TYPE_SCENERY360, &gScenery360[i], OBSTACLE_ARRAY_SCENERY360, i,
                                          player, stats, fn);
        }
    }
}
