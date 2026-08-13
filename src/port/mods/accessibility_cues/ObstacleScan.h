#pragma once

// The shared obstacle scan: which world objects count as obstacles (the shared
// Object_IsObstacle predicate — collidable hitbox, not lockable — minus the cue-side
// exclusions below) and their solid hitbox records as world-space boxes with the
// player-relative geometry every consumer needs already derived. The obstacle-ahead cue
// is the first consumer; the planned all-range extension and the directional obstacle
// cues (left/right, above/below) are the reason the geometry is yielded raw — no
// thresholding, no margin, no direction naming happens here. Every filter beyond "is a
// warn-worthy obstacle with a solid box" is the caller's policy. Game-coupled by design,
// like CueScan.h.
//
// Cue-side exclusions on top of the predicate (policy shared by the future obstacle
// cues, deliberately NOT in Object_IsObstacle, whose other consumer is
// AccessibilityTrainingMinimal):
//   - gBosses[] is not scanned at all: targetOffset cannot separate "boss you fight"
//     from "wall" (see the predicate's comment in ObjectQuery.h), and droning a crash
//     warning through an on-rails boss fight would bury the aim/enemy cues exactly when
//     they matter most.
//   - Teammate Arwings (OBJ_ACTOR_TEAM_BOSS — the wingmates escorting you into a boss
//     run, placed on-rails in Meteo and Area 6) carry a small collidable hitbox with
//     targetOffset 0, and would otherwise buzz whenever one weaves across your nose.
//     OBJ_ACTOR_TEAM_ARWING needs no entry — it has gNoHitbox and never passes the
//     predicate.
//
// Deliberate non-features, so a future session doesn't "fix" them by accident: obj.rot
// is ignored (the box is axis-aligned in world space even for a yawed wall), and
// HITBOX_ROTATED's rotation floats are skipped with its box used as-is — both absorbed
// by the caller's safety margin; shadow and whoosh records are dropped entirely
// (Object_ReadSolidHitboxes). Poly-mesh scenery carrying gNoHitbox never appears at all
// (accepted miss — docs/accessibility-obstacle-cue.md).

#include "port/CGameCompat.h"
#include "port/mods/ObjectQuery.h"

// Which active-world array a box came from. Reported in the debug mirror so a policy
// decision can be joined against the `objects` command's dumps. BOSS and SCENERY360
// currently have no producer (bosses are excluded as policy, gScenery360 is all-range —
// the planned extension adds its loop); they keep their slots so the debug indices stay
// stable when that happens.
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
    s32 record;   // hitbox record index, 0-based (the engine's hit index is this + 1)
    Vec3f center; // obj.pos + the record's offsets; with `half`, the full box — what the
                  // all-range course test will ray-cast against (unused on rails)
    Vec3f half;   // record half-extents, non-negative
    f32 dx, dy;   // center minus player (pos.x / pos.y); the directional cues' signal
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

struct ObstacleScanStats {
    s32 active;    // occupied OBJ_ACTIVE slots visited
    s32 obstacles; // also passed Object_IsObstacle and the cue-side exclusions
    s32 boxes;     // solid hitbox records yielded
};

namespace ObstacleScanDetail {
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
    if (entry->obj.id == OBJ_ACTOR_TEAM_BOSS) {
        return; // wingmates are not obstacles — see the header comment
    }
    if (stats != nullptr) {
        stats->obstacles++;
    }
    HitboxBox records[kObjectMaxHitboxRecords];
    s32 n = Object_ReadSolidHitboxes(entry->info.hitbox, records, kObjectMaxHitboxRecords);
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
        box.clearX = fabsf(box.dx) - box.half.x;
        box.clearY = fabsf(box.dy) - box.half.y;
        // trueZpos is the player's real world Z; pos.z is the path scroll (sf64player.h).
        box.gapZ = player->trueZpos - (box.center.z + box.half.z);
        if (stats != nullptr) {
            stats->boxes++;
        }
        fn(box);
    }
}
} // namespace ObstacleScanDetail

// Walks gScenery[] and gActors[] (the on-rails world; gScenery360 is all-range-only and
// deliberately not scanned yet — the all-range extension adds it here, behind a mode
// flag, together with a range cutoff like kEnemyCueAllRangeMaxDist; gBosses[] is
// excluded as policy, see the header comment) and calls fn(const ObstacleBox&) once per
// solid hitbox record of every active obstacle. `stats` may be null. Cheap: ~110 slots,
// a handful of records each, no allocation.
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
}
