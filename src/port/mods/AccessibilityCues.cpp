#include "AccessibilityCues.h"

#include <math.h>

#include "port/CGameCompat.h"
#include "port/hooks/Events.h"
#include "port/accessibility/Cue.h"

#include <spdlog/spdlog.h>

// The cues themselves (sound file, volume CVar, preview, lazy loading) live in the
// game-agnostic Cue class over the Cue3D HRTF seam; this file is the Star Fox side —
// it registers the cues and decides, per game tick, what each one targets. The old
// SF64-audio-engine cue path was removed once the HRTF backend proved out: the game
// engine's camera-relative panning and per-frame SFX lifetime made it a dead end for
// continuous navigation cues (see docs/accessibility-hrtf-cues.md), so game SFX are
// reserved for future one-shot flourishes, not primary cues.

// Registered in AccessibilityCues_Init; process-lifetime (the registry never frees).
static Cue* sRingCue = nullptr;
static Cue* sEnemyCue = nullptr;

static bool AccessibilityCues_IsEnabled() {
    return CVarGetInteger("gAccessibilityAudioCues", 1) == 1;
}

// True while the level is paused (START during play -> gPlayState == PLAY_PAUSE,
// see fox_play.c). The 3D cue backend runs its own OS audio device that the
// game's pause doesn't reach, so a looping Cue3D source keeps sounding through a
// pause unless we stop it by hand. The listeners treat pause as one more reason
// to stop the cue; because Play_Update is skipped while paused, the player /
// ring / enemy positions are frozen, so the next unpaused tick re-acquires the
// same target and restarts the cue.
static bool AccessibilityCues_IsPaused() {
    return gPlayState == PLAY_PAUSE;
}

// Per-tick diagnostic trace for the enemy cue. Off by default. Even when on,
// emitted lines still go through SPDLOG_TRACE, so the global log threshold
// (gDeveloperTools.LogLevel) must also be at "trace" to actually appear in
// the log. Pattern matches gObjectSpawnLog. Use ENEMY_CUE_TRACE(...) below.
static bool AccessibilityCues_IsEnemyCueLogEnabled() {
    return CVarGetInteger("gAccessibilityEnemyCueLog", 0) == 1;
}

#define ENEMY_CUE_TRACE(...)                              \
    do {                                                  \
        if (AccessibilityCues_IsEnemyCueLogEnabled()) {   \
            SPDLOG_TRACE(__VA_ARGS__);                    \
        }                                                 \
    } while (0)

// Shared Y->pitch mapping for both cues: higher source = higher pitch,
// ±1 octave clamped at ±1000 world units. The 1000.0f divisor and the ±1
// octave clamp are the two tuning knobs (see
// docs/accessibility-cues-tuning.md).
static f32 AccessibilityCues_ComputeFreqModFromY(f32 y) {
    f32 octaves = y / 1000.0f;
    if (octaves > 1.0f) {
        octaves = 1.0f;
    } else if (octaves < -1.0f) {
        octaves = -1.0f;
    }
    return powf(2.0f, octaves);
}

// Point a cue at a listener-relative offset (game convention: +x right, +y up,
// +z ahead) and start it if it isn't sounding yet. Object_ClampSfxSource keeps the
// vector inside the SF64 engine's documented ±5000/±2000 box — kept on the HRTF path
// so the by-ear-verified distance/pitch tuning is unchanged; whether the 3D backend
// still wants the clamp is a tuning follow-up. On top of the HRTF we drive pitch from
// the clamped Y: the generic HRTF only renders strong elevation when the target is
// nearly on the aim line (the vertical angle is tiny for most of the approach), so the
// raw-Y pitch supplies a distance-independent "above/below you" signal throughout.
static void AccessibilityCues_DriveCue(Cue* cue, f32 dx, f32 dy, f32 dz) {
    f32 src[3] = { dx, dy, dz };
    Object_ClampSfxSource(src);
    cue->SetTarget(src[0], src[1], src[2], AccessibilityCues_ComputeFreqModFromY(src[1]));
    cue->Start();
}

// ===== Ring cue =====

static Item* AccessibilityCues_FindNextTrainingRing() {
    Player* player = &gPlayer[0];
    Item* best = NULL;
    // dz < 0 means the ring is ahead of the player (player flies in -Z).
    // We want the ring closest to the player while still ahead, i.e. the dz
    // closest to zero from below, i.e. the maximum dz that is still negative.
    f32 bestDz = -1.0e9f;

    for (s32 i = 0; i < ARRAY_COUNT(gItems); i++) {
        Item* item = &gItems[i];
        if (item->obj.status != OBJ_ACTIVE) {
            continue;
        }
        if (item->obj.id != OBJ_ITEM_TRAINING_RING) {
            continue;
        }
        // state 1 = ring is in its fly-to-player animation after being
        // collected; no longer a navigation target.
        if (item->state != 0) {
            continue;
        }
        f32 dz = item->obj.pos.z - player->trueZpos;
        if (dz >= 0.0f) {
            continue;
        }
        if (dz > bestDz) {
            bestDz = dz;
            best = item;
        }
    }
    return best;
}

static void AccessibilityCues_OnRingPostUpdate(IEvent* event) {
    (void) event;

    // gPlayer is a pointer (sf64context.h:324), zero-initialized at process
    // start and only allocated when a level loads. The listener fires on
    // GamePostUpdateEvent which can tick before that — guard it.
    if (!AccessibilityCues_IsEnabled() || gCurrentLevel != LEVEL_TRAINING || gPlayer == NULL ||
        AccessibilityCues_IsPaused()) {
        sRingCue->Stop();
        return;
    }

    Item* target = AccessibilityCues_FindNextTrainingRing();
    if (target == NULL) {
        sRingCue->Stop();
        return;
    }

    // Player-relative bypass of Object_SetSfxSourceToPos. The converter pans
    // by camera position, but on-rails mode (which training is) decouples the
    // camera from the Arwing's lateral drift; a ring "in front of the camera"
    // can be off to the player's right. See docs/audio-system.md section 6.
    Player* player = &gPlayer[0];
    AccessibilityCues_DriveCue(sRingCue, target->obj.pos.x - player->pos.x, target->obj.pos.y - player->pos.y,
                               -(target->obj.pos.z - player->trueZpos));
}

// ===== Enemy cue =====

// Build the world->body rotation on gCalcMatrix from the player's aim
// angles. Body frame: +X right of aim, +Y above aim, -Z ahead of aim
// (right-handed, conventional FPS convention). This is the inverse of
// Player_SetupArwingShot's body->world rotation (fox_play.c:3023-3025),
// minus the +180° yaw (we choose body +X = right rather than left) and
// minus bank (irrelevant for direction). See docs/accessibility-enemy-cue.md
// for the derivation. gCalcMatrix is a scratch the game reuses every frame;
// we own it for the brief window between this call and the loop below.
static void AccessibilityCues_BuildWorldToBodyMatrix(Player* player) {
    f32 yaw = player->yRot_114 + player->rot.y;
    f32 pitch = player->xRot_120 + player->rot.x + player->aerobaticPitch;
    // body = Rx(-pitch) · Ry(-yaw) · world
    Matrix_RotateX(gCalcMatrix, -pitch * M_DTOR, MTXF_NEW);
    Matrix_RotateY(gCalcMatrix, -yaw * M_DTOR, MTXF_APPLY);
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
static bool AccessibilityCues_IsCueableEnemy(Actor* actor) {
    return (actor->obj.status == OBJ_ACTIVE) && (actor->info.targetOffset != 0.0f);
}

// Per-tick scan counters; populated by FindClosestEnemyAhead, consumed by
// the listener's trace logs. Strictly debug-only; if we drop the tracing
// the struct can go too.
struct EnemyCueScanStats {
    s32 active;    // gActors slots with status == OBJ_ACTIVE
    s32 cueable;   // also passed IsCueableEnemy predicate
    s32 ahead;     // also passed bodyDelta.z < 0 (in front of aim)
    Actor* chosen; // closest one; NULL if none picked
};

// Walks gActors[], applies the lock-on predicate, rotates each candidate's
// world delta into body frame via the matrix the caller has already set on
// gCalcMatrix, drops anyone behind the aim line, and returns the body-frame
// delta of whoever has the smallest 3D distance. Returns false if no
// candidate passes. Populates outStats for trace logging.
static bool AccessibilityCues_FindClosestEnemyAhead(Player* player, Vec3f* outBodyDelta, EnemyCueScanStats* outStats) {
    outStats->active = 0;
    outStats->cueable = 0;
    outStats->ahead = 0;
    outStats->chosen = NULL;
    bool found = false;
    f32 bestDistSq = 1.0e18f;

    for (s32 i = 0; i < ARRAY_COUNT(gActors); i++) {
        Actor* actor = &gActors[i];
        if (actor->obj.status == OBJ_ACTIVE) {
            outStats->active++;
        }
        if (!AccessibilityCues_IsCueableEnemy(actor)) {
            continue;
        }
        outStats->cueable++;
        Vec3f worldDelta;
        worldDelta.x = actor->obj.pos.x - player->pos.x;
        worldDelta.y = actor->obj.pos.y - player->pos.y;
        worldDelta.z = actor->obj.pos.z - player->trueZpos;
        Vec3f bodyDelta;
        Matrix_MultVec3fNoTranslate(gCalcMatrix, &worldDelta, &bodyDelta);
        // bodyDelta.z >= 0 means at or behind the aim line. Drop it.
        if (bodyDelta.z >= 0.0f) {
            continue;
        }
        outStats->ahead++;
        f32 distSq = (bodyDelta.x * bodyDelta.x) + (bodyDelta.y * bodyDelta.y) + (bodyDelta.z * bodyDelta.z);
        if (distSq < bestDistSq) {
            bestDistSq = distSq;
            *outBodyDelta = bodyDelta;
            outStats->chosen = actor;
            found = true;
        }
    }
    return found;
}

static void AccessibilityCues_OnEnemyPostUpdate(IEvent* event) {
    (void) event;

    // gLevelMode and gPlayer both default to "ready-looking" zero values at
    // process start (LEVELMODE_ON_RAILS = 0, gPlayer = NULL pointer) before
    // any level loads, so the mode check alone doesn't filter the pre-game
    // title/menu ticks. Null-check gPlayer to keep the listener safe there.
    bool enabled = AccessibilityCues_IsEnabled();
    bool onRails = (gLevelMode == LEVELMODE_ON_RAILS);
    bool hasPlayer = (gPlayer != NULL);
    bool paused = AccessibilityCues_IsPaused();
    if (!enabled || !onRails || !hasPlayer || paused) {
        ENEMY_CUE_TRACE("[enemy-cue] gated enabled={} onRails={} mode={} hasPlayer={} paused={}", enabled, onRails,
                        (int) gLevelMode, hasPlayer, paused);
        sEnemyCue->Stop();
        return;
    }

    Player* player = &gPlayer[0];
    AccessibilityCues_BuildWorldToBodyMatrix(player);

    EnemyCueScanStats stats;
    Vec3f bodyDelta;
    bool found = AccessibilityCues_FindClosestEnemyAhead(player, &bodyDelta, &stats);

    if (!found) {
        ENEMY_CUE_TRACE("[enemy-cue] no target level={} active={} cueable={} ahead={}", (int) gCurrentLevel,
                        stats.active, stats.cueable, stats.ahead);
        sEnemyCue->Stop();
        return;
    }

    // Body-frame delta: +X right of aim, +Y above aim, -Z ahead. DriveCue takes
    // +z-ahead, so hand it the negated body Z (matching the ring cue's convention).
    AccessibilityCues_DriveCue(sEnemyCue, bodyDelta.x, bodyDelta.y, -bodyDelta.z);

    // src/freq are the cue's post-clamp target — what DriveCue actually pushed this tick.
    ENEMY_CUE_TRACE("[enemy-cue] target id={} level={} active={} cueable={} ahead={} bodyDelta=({:.1f},{:.1f},{:.1f}) "
                    "src=({:.1f},{:.1f},{:.1f}) freq={:.3f}",
                    (int) stats.chosen->obj.id, (int) gCurrentLevel, stats.active, stats.cueable, stats.ahead,
                    bodyDelta.x, bodyDelta.y, bodyDelta.z, sEnemyCue->TargetX(), sEnemyCue->TargetY(),
                    sEnemyCue->TargetZ(), sEnemyCue->TargetPitch());
}

// ===== Entry points =====

// Timed cue previews (the settings UI's "Preview" buttons) expire on the game tick,
// which keeps them alive-and-bounded even if the menu closes mid-preview.
static void AccessibilityCues_OnPreviewTick(IEvent* event) {
    (void) event;
    CueRegistry_TickPreviews();
}

void AccessibilityCues_Init() {
    CVarRegisterInteger("gAccessibilityAudioCues", 1);
    CVarRegisterInteger("gAccessibilityEnemyCueLog", 0);

    sRingCue = CueRegistry_Register("Ring", "Ring guide", "Guides you toward the next training ring.",
                                    "assets/accessibility/ring.wav");
    sEnemyCue = CueRegistry_Register("Enemy", "Enemy locator",
                                     "Tracks the closest lockable enemy ahead of your aim.",
                                     "assets/accessibility/enemy.wav");

    REGISTER_LISTENER(GamePostUpdateEvent, AccessibilityCues_OnRingPostUpdate, EVENT_PRIORITY_NORMAL);
    REGISTER_LISTENER(GamePostUpdateEvent, AccessibilityCues_OnEnemyPostUpdate, EVENT_PRIORITY_NORMAL);
    REGISTER_LISTENER(GamePostUpdateEvent, AccessibilityCues_OnPreviewTick, EVENT_PRIORITY_NORMAL);
}

void AccessibilityCues_Exit() {
    // Stops every cue and drops the backend handles; the caller's next step
    // (Cue3D_Shutdown in Accessibility_Exit) frees the underlying sources. Any
    // listener tick after this point sees idle cues with no handle and no-ops.
    CueRegistry_UnloadAll();
}
