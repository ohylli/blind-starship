#pragma once

// The shared enemy scan: which actors count as cueable enemies and which of those are in
// scope for the current mode. Both the enemy cue (top-N closest voices) and the aim cue
// (geiger min-angle) consume it; keeping the predicate and the scoping here is what
// guarantees the two cues always agree on what "a lockable enemy" means. Consumers keep
// their own accumulation on top of CueScan_ForEachCueableEnemy — a future "scan once per
// tick, fan out" optimization can slot in behind the same helper if it ever proves
// needed. Game-coupled by design (this header pulls in the game types); scalar-only
// shared helpers live in CueCommon.h instead so UI/debug consumers stay light.

#include "port/CGameCompat.h"

// All-range enemy-scan range limit, in world units. On-rails needs no cutoff — the
// engine's object streaming keeps gActors[] populated with only nearby entities — but
// all-range loads the whole arena at once, and past Object_ClampSfxSource's ±5000 box
// every target sounds the same ~15% volume regardless of distance, so unbounded scanning
// would drone about enemies too far to be actionable. Rebuild-to-tune constant (see
// docs/accessibility-cues-tuning.md); promote to a CVar if by-ear tuning wants it live.
inline constexpr f32 kEnemyCueAllRangeMaxDist = 10000.0f;

// Per-scan counters, populated by CueScan_ForEachCueableEnemy when the caller passes a
// stats out-param. Strictly debug-only (trace logs and the debug server's `cues` dump).
struct CueScanStats {
    s32 active;  // gActors slots with status == OBJ_ACTIVE
    s32 cueable; // also passed CueScan_IsCueableEnemy
    s32 kept;    // also passed the mode's direction/range filters
};

// Lock-on predicate; matches PlayerShot_FindLockTarget (fox_beam.c). See the definition
// for why there is deliberately no id filter.
bool CueScan_IsCueableEnemy(Actor* actor);

// Build the world->body rotation on gCalcMatrix from the player's aim angles; must run
// before CueScan_ForEachCueableEnemy. See the definition for the frame convention.
void CueScan_BuildWorldToBodyMatrix(Player* player);

// Walks gActors[], applies the lock-on predicate, rotates each candidate's world delta
// into body frame via the matrix CueScan_BuildWorldToBodyMatrix left on gCalcMatrix, and
// calls fn(actor, slot, bodyDelta, distSq) for each in-scope candidate. The mode decides
// which candidates are in scope: on-rails keeps only enemies ahead of the aim line
// (behind-aim sources used to pan ambiguously, and everything relevant comes at you from
// ahead anyway); all-range keeps the full sphere — threats come from behind there and the
// HRTF renders the rear hemisphere — but drops anything past kEnemyCueAllRangeMaxDist.
// `stats` may be null. Body frame: +X right of aim, +Y above aim, -Z ahead of aim.
template <typename Fn> void CueScan_ForEachCueableEnemy(Player* player, bool allRange, CueScanStats* stats, Fn&& fn) {
    if (stats != nullptr) {
        stats->active = 0;
        stats->cueable = 0;
        stats->kept = 0;
    }
    for (s32 i = 0; i < ARRAY_COUNT(gActors); i++) {
        Actor* actor = &gActors[i];
        if ((stats != nullptr) && (actor->obj.status == OBJ_ACTIVE)) {
            stats->active++;
        }
        if (!CueScan_IsCueableEnemy(actor)) {
            continue;
        }
        if (stats != nullptr) {
            stats->cueable++;
        }
        Vec3f worldDelta;
        worldDelta.x = actor->obj.pos.x - player->pos.x;
        worldDelta.y = actor->obj.pos.y - player->pos.y;
        worldDelta.z = actor->obj.pos.z - player->trueZpos;
        Vec3f bodyDelta;
        Matrix_MultVec3fNoTranslate(gCalcMatrix, &worldDelta, &bodyDelta);
        // bodyDelta.z >= 0 means at or behind the aim line.
        if (!allRange && bodyDelta.z >= 0.0f) {
            continue;
        }
        f32 distSq = (bodyDelta.x * bodyDelta.x) + (bodyDelta.y * bodyDelta.y) + (bodyDelta.z * bodyDelta.z);
        if (allRange && distSq > kEnemyCueAllRangeMaxDist * kEnemyCueAllRangeMaxDist) {
            continue;
        }
        if (stats != nullptr) {
            stats->kept++;
        }
        fn(actor, i, bodyDelta, distSq);
    }
}
