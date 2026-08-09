#include "CueScan.h"

#include "port/PlayerAim.h"

// The cues run on-rails and in solo all-range; Versus shares LEVELMODE_ALL_RANGE but
// is untested multiplayer territory, so gVersusMode gates it out. gLevelMode defaults
// to a "ready-looking" zero value at process start (LEVELMODE_ON_RAILS = 0) before any
// level loads, so this check alone doesn't filter the pre-game title/menu ticks;
// callers pair it with Accessibility_PlayerHasControl (which also null-checks gPlayer).
bool CueScan_ModeInScope(bool* outAllRange) {
    bool allRange = (gLevelMode == LEVELMODE_ALL_RANGE);
    if (outAllRange != nullptr) {
        *outAllRange = allRange;
    }
    return (gLevelMode == LEVELMODE_ON_RAILS) || (allRange && !gVersusMode);
}

// Lock-on predicate. Matches PlayerShot_FindLockTarget (fox_beam.c:1741):
// status == OBJ_ACTIVE && info.targetOffset != 0. No `id != OBJ_ACTOR_EVENT`
// here — the on-rails levels spawn essentially all gameplay enemies as
// OBJ_ACTOR_EVENT (Venom tanks, Spy Eyes, etc.) and EVOP_INIT_ACTOR
// (fox_enmy2.c:1132-1258) overwrites `actor->info` with the per-event info
// at init: line 1211 copies targetOffset from sEventActorInfo[eventType] for
// EVID < 200, line 1157 hardcodes targetOffset = 1.0 for EVID_200..EVID_300.
// So after init, a OBJ_ACTOR_EVENT actor whose eventType is a real enemy has
// a non-zero targetOffset; the id field stays as OBJ_ACTOR_EVENT but no
// longer reflects whether the actor is lockable. Filtering on id would
// reject every event-spawned enemy.
bool CueScan_IsCueableEnemy(Actor* actor) {
    return (actor->obj.status == OBJ_ACTIVE) && (actor->info.targetOffset != 0.0f);
}

// Build the world->body rotation on gCalcMatrix from the player's aim
// angles. Body frame: +X right of aim, +Y above aim, -Z ahead of aim
// (right-handed, conventional FPS convention). This is the inverse of
// Player_SetupArwingShot's body->world rotation (fox_play.c:3023-3025),
// minus the +180° yaw (we choose body +X = right rather than left) and
// minus bank (irrelevant for direction). See docs/accessibility-enemy-cue.md
// for the derivation. gCalcMatrix is a scratch the game reuses every frame;
// we own it for the brief window between this call and the caller's scan.
void CueScan_BuildWorldToBodyMatrix(Player* player) {
    f32 yaw = Player_AimYaw(*player);
    f32 pitch = Player_AimPitch(*player);
    // body = Rx(-pitch) · Ry(-yaw) · world
    Matrix_RotateX(gCalcMatrix, -pitch * M_DTOR, MTXF_NEW);
    Matrix_RotateY(gCalcMatrix, -yaw * M_DTOR, MTXF_APPLY);
}
