#pragma once

// The shared obstacle scan: which world objects count as obstacles (the shared
// Object_IsObstacle predicate — collidable hitbox, poly mesh or sphere collider, not
// lockable — minus the cue-side exclusions below) and their collision shapes as
// world-space boxes with the player-relative geometry every consumer needs already
// derived. The obstacle-ahead cue was the first consumer; the directional obstacle cues
// (ObstacleDirectionCue.cpp: beside, above, below) are the second, and the reason the
// geometry is yielded raw — no thresholding, no margin, no direction naming happens
// here. Every filter beyond "is a warn-worthy obstacle with a solid box" is the caller's
// policy; the tests the family shares (the course frame, the ahead cue's verdict on a
// box) live in ObstacleCourse.h over these boxes. Game-coupled by design, like CueScan.h.
//
// Cue-side exclusions on top of the predicate (policy shared by every obstacle cue,
// deliberately NOT in Object_IsObstacle, whose other consumer is
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
//   - Actors whose timer_0C2 is running are skipped, mirroring the engine's own gate:
//     Player_CollisionCheck's actor loop tests only actors with timer_0C2 == 0. The
//     engine uses that timer as a no-collide window (freshly hit or dying actors, the
//     all-range supplies drop) and, permanently, on the all-range EVENT HANDLER: every
//     all-range arena reserves gActors[0] for an invisible AI360_EVENT_HANDLER actor at
//     the world origin that runs the arena's event script (fox_360.c). It is given the
//     generic OBJ_ACTOR_ALLRANGE info, so it carries a fighter's 40-unit hitbox and
//     passes the predicate, but it re-arms timer_0C2 every tick and never collides. On
//     Katina it shares the origin with the base, so before this gate the cue buzzed at
//     "the base" — which as a boss is not scanned at all.
//
// Three collision mechanisms, one box shape (ObstacleScan_RecordKind names them):
//   - Hitbox objects yield one box per solid record (Object_ReadSolidHitboxes).
//   - Poly-mesh objects (Object_GetPolyCollider — the terrain bumps, mountains, reefs,
//     the molar rock, the Sector Y capital ships) yield ONE box, the mesh's own bounding
//     box around obj.pos (Object_GetPolyBounds), record index kObstaclePolyRecord — and
//     it REPLACES the object's hitbox records, because the engine never tests those for
//     the player once an id is routed to the poly test (the capital ship's real hitbox
//     and the island's are dead data as far as crashing goes). The box is exact for the
//     solid meshes but a coarse over-approximation for the heightfield family (see the
//     ObjectQuery.h comment): a terrain bump's box spans the whole hill, so a course
//     through the box at an altitude that clears the slope still counts as on course.
//     Refining that is the consumer's job — the scan stays box-only so every consumer
//     shares one shape, and carries the mesh identity, obj.pos/rot.y and the engine's
//     range gate on the box (the poly* fields) so a consumer can replay the engine's own
//     surface test, as the ahead cue does along its course
//     (ObstacleCourse_AheadHitsTerrain over Object_PolyHeightfieldHit) and the
//     directional cues do below and beside it (ObstacleDirectionCue_TerrainBelow /
//     _TerrainBeside over Object_PolyHeightfieldSurfaceY).
//   - Sphere-collided objects (Object_GetSphereCollider — today only Meteo's big meteor,
//     which carries gNoHitbox and is collided by a hand-written 900-unit sphere) are
//     boxed the same way: one synthesized record centered on obj.pos with half-extents
//     equal to the radius on every axis, record index kObstacleSphereRecord. The cube
//     over-warns at its corners (a diagonal pass 900-1270 units off center) — accepted,
//     in the same spirit as the yawed-wall approximation below: a conservative warning
//     about an 1800-unit rock is the right failure direction, and it keeps the course
//     tests and the directional cues on one box shape.
//
// Deliberate non-features, so a future session doesn't "fix" them by accident: obj.rot
// is ignored (the box is axis-aligned in world space even for a yawed wall or a
// rotated mesh), and HITBOX_ROTATED's rotation floats are skipped with its box used
// as-is — both absorbed by the caller's safety margin. The one exception is opt-in: a
// consumer planning a terrain walk asks for the heightfield's yawed footprint
// (ObstacleScan_YawedFootprint), because a walk that never visits a turned hill's
// corner cannot be saved by any margin. Shadow and whoosh records are
// dropped entirely (Object_ReadSolidHitboxes). gSprites[] is not scanned in either mode
// (accepted miss — docs/accessibility-obstacle-cue.md): sprites do collide (Fortuna's
// poles, Corneria's trees — a harmless stagger, fox_play.c's sprite loop), but warning
// about them is a scope decision deferred with the user, not an oversight.

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
// "hitbox", "sphere" or "poly" for an ObstacleBox::record value, so the `cues` dump
// never shows the sentinels below as bare magic numbers.
const char* ObstacleScan_RecordKind(s32 record);

// One solid hitbox record of one obstacle, in world space, plus the player-relative
// geometry. Plain scalars: safe to copy into a debug mirror that outlives the object.
// Frame convention is the game's: +x right, +y up, +z BACKWARD (ahead is -z).
struct ObstacleBox {
    ObstacleArray array;
    s32 slot;     // index in that array
    s32 objId;    // obj.id
    s32 record;   // hitbox record index, 0-based (the engine's hit index is this + 1), or
                  // kObstacleSphereRecord / kObstaclePolyRecord for a box synthesized from
                  // a sphere collider / a poly mesh's bounds
    Vec3f center; // obj.pos + the record's offsets; with `half`, the full box in world
                  // space. Consumed to derive dx/dy/dz (ObstacleScan_DeriveRelative) and
                  // by the footprint yaw (ObstacleScan_YawedFootprint), which turns it
                  // about obj.pos.
    Vec3f half;   // record half-extents, non-negative
    f32 dx, dy;   // center minus player (pos.x / pos.y); their signs pick the directional cues' side
    f32 dz;       // center minus player trueZpos — with dx/dy the full 3D center delta
                  // the all-range ray test casts along (negative = ahead of the player)
    // How far OUTSIDE the footprint the player is on each axis: fabsf(d) - half.
    // Negative means the player is inside the footprint on that axis. A collision-course
    // test is "both below the caller's safety margin"; the directional cues read the
    // sign of dx / dy together with these (ObstacleDirectionCue.cpp).
    f32 clearX, clearY;
    // Player to the box's NEAR face in Z — the face the player, flying toward -z, meets
    // first: center.z + half.z. Positive while the box is still ahead, <= 0 once the
    // player is level with or past the face. The "distance to impact" a warning maps.
    f32 gapZ;
    // The producing object's obj.pos and obj.rot.y, for consumers that need the object
    // rather than the box (the heightfield sampling rotates its probes into the mesh's
    // frame the way Player_CheckPolyCollision does).
    Vec3f objPos;
    f32 rotY;
    // Poly-mesh identity, valid only when record == kObstaclePolyRecord (else -1 / false /
    // 0): the engine's mesh index and family (Object_GetPolyCollider), whether that
    // family is the CollisionHeader2 HEIGHTFIELD — a surface hit only from above, whose
    // box therefore over-approximates (see the header comment) — and the XZ range gate
    // the engine applies before testing it (Object_GetPolyCollisionRangeXZ, 0 = none).
    s32 polyColId;
    bool polyHeightfield;
    f32 polyRangeXZ;
    // True once the XZ half of the box is the yawed footprint (ObstacleScan_YawedFootprint)
    // rather than the scan's unrotated one; the scan always yields false.
    bool footprintYawed;
};

// Derives the player-relative fields (dx, dy, dz, clearX, clearY, gapZ) from the box's
// center and half-extents. `origin` is the ship's real world position: pos.x, pos.y and
// trueZpos (pos.z is the rails path scroll — sf64player.h). The one derivation, shared by
// the scan and the footprint yaw, so a box they produce can never disagree on it.
inline void ObstacleScan_DeriveRelative(ObstacleBox& box, const Vec3f& origin) {
    box.dx = box.center.x - origin.x;
    box.dy = box.center.y - origin.y;
    box.dz = box.center.z - origin.z;
    box.clearX = fabsf(box.dx) - box.half.x;
    box.clearY = fabsf(box.dy) - box.half.y;
    box.gapZ = origin.z - (box.center.z + box.half.z);
}

// A poly mesh's box with the object's yaw applied: the world-axis-aligned box around the
// mesh's footprint turned by obj.rot.y about obj.pos, the rotation the engine undoes
// before testing (Player_CheckPolyCollision; the probes in ObjectQuery.h apply the same
// one). The scan's box ignores the yaw (see the header comment), and most terrain meshes
// are yawed (every Corneria bump family, Fortuna's mountains, Zoness's islands, Aquas's
// reefs and bumps), so the unrotated box can leave out a corner of the real footprint.
// The XZ extents become the turned footprint's bounding box — one can shrink as well as
// grow (at 90 degrees they swap) — while Y, the mesh identity and obj.pos/rot.y are kept
// and the player-relative fields are re-derived against `origin`
// (ObstacleScan_DeriveRelative). Used to plan every terrain walk, the ahead cue's and the
// directional cues', so they decide where to probe on the real footprint — the probes
// themselves are exact either way. Idempotent (footprintYawed): a box already yawed is
// returned as is.
ObstacleBox ObstacleScan_YawedFootprint(const ObstacleBox& box, const Vec3f& origin);

// `record` values of a box synthesized from a sphere collider / a poly mesh's bounds
// (see the header comment). Negative so they can never collide with a real record index
// in the `cues` dump.
inline constexpr s32 kObstacleSphereRecord = -1;
inline constexpr s32 kObstaclePolyRecord = -2;

struct ObstacleScanStats {
    s32 active;    // occupied OBJ_ACTIVE slots visited
    s32 obstacles; // also passed Object_IsObstacle and the cue-side exclusions
    s32 boxes;     // solid hitbox records yielded
};

namespace ObstacleScanDetail {
// The wingmate exclusion from the header comment. Only Actor carries eventType, so the
// event-actor half of the test is compiled in for that wrapper alone.
template <typename T> inline bool IsTeammate(const T* entry) {
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
    if constexpr (std::is_same_v<T, Actor>) {
        if (entry->timer_0C2 != 0) {
            return; // the engine's own no-collide gate (Player_CollisionCheck) — see the header comment
        }
    }
    if (stats != nullptr) {
        stats->obstacles++;
    }
    HitboxBox records[kObjectMaxHitboxRecords + 1];
    s32 n;
    s32 polyColId = -1;
    bool polyUseCol2 = false;
    f32 sphereRadius;
    if (Object_GetPolyCollider(type, entry, &polyColId, &polyUseCol2)) {
        // The mesh's bounding box, INSTEAD of the hitbox records — see the header
        // comment. min/max are obj.pos-relative corners; the record wants a center
        // offset plus half-extents. A colId outside the header table (unreachable: the
        // engine's id map only yields tabled indices) yields no box at all rather than
        // falling back to the dead hitbox records.
        Vec3f min, max;
        n = 0;
        if (Object_GetPolyBounds(polyColId, polyUseCol2, &min, &max)) {
            records[0].zOffset = (min.z + max.z) * 0.5f;
            records[0].zHalf = (max.z - min.z) * 0.5f;
            records[0].yOffset = (min.y + max.y) * 0.5f;
            records[0].yHalf = (max.y - min.y) * 0.5f;
            records[0].xOffset = (min.x + max.x) * 0.5f;
            records[0].xHalf = (max.x - min.x) * 0.5f;
            records[0].record = kObstaclePolyRecord;
            n = 1;
        }
    } else {
        n = Object_ReadSolidHitboxes(entry->info.hitbox, records, kObjectMaxHitboxRecords);
    }
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
        box.center.z = entry->obj.pos.z + records[r].zOffset; // (Play_CheckSingleHitbox, fox_play.c)
        box.half.x = records[r].xHalf;
        box.half.y = records[r].yHalf;
        box.half.z = records[r].zHalf;
        ObstacleScan_DeriveRelative(box, { player->pos.x, player->pos.y, player->trueZpos });
        box.objPos = entry->obj.pos;
        box.rotY = entry->obj.rot.y;
        if (records[r].record == kObstaclePolyRecord) {
            box.polyColId = polyColId;
            box.polyHeightfield = polyUseCol2;
            box.polyRangeXZ = Object_GetPolyCollisionRangeXZ(type);
        } else {
            box.polyColId = -1;
            box.polyHeightfield = false;
            box.polyRangeXZ = 0.0f;
        }
        box.footprintYawed = false;
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
    // (Object_CheckCollision, fox_enmy.c). That formulation also covers Venom-Andross,
    // which skips the allocation and reuses a prior level's block in place after
    // re-zeroing its statuses (Play_Main, fox_play.c).
    if ((gLevelMode == LEVELMODE_ALL_RANGE) && (gScenery360 != nullptr)) {
        for (s32 i = 0; i < kScenery360Count; i++) {
            ObstacleScanDetail::EmitBoxes(OBJECT_TYPE_SCENERY360, &gScenery360[i], OBSTACLE_ARRAY_SCENERY360, i, player,
                                          stats, fn);
        }
    }
}
