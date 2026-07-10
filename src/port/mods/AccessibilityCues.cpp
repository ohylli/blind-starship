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

// How many of the closest enemies to voice at once. Runtime-tunable (F1 -> Blind
// Starship) so the by-ear sweet spot can be found without rebuilding.
static s32 AccessibilityCues_EnemyCueVoiceCount() {
    s32 count = CVarGetInteger("gAccessibilityEnemyCueVoices", kAccessibilityEnemyCueDefaultVoices);
    if (count < 1) {
        count = 1;
    } else if (count > kAccessibilityEnemyCueMaxVoices) {
        count = kAccessibilityEnemyCueMaxVoices;
    }
    return count;
}

// All-range enemy-cue range limit, in world units. On-rails needs no cutoff — the
// engine's object streaming keeps gActors[] populated with only nearby entities — but
// all-range loads the whole arena at once, and past Object_ClampSfxSource's ±5000 box
// every target sounds the same ~15% volume regardless of distance, so unbounded scanning
// would drone about enemies too far to be actionable. Rebuild-to-tune constant (see
// docs/accessibility-cues-tuning.md); promote to a CVar if by-ear tuning wants it live.
static constexpr f32 kEnemyCueAllRangeMaxDist = 10000.0f;

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

// Turn a raw listener-relative offset (game convention: +x right, +y up, +z ahead) into
// what a cue voice is fed: the clamped source vector and the Y-derived pitch.
// Object_ClampSfxSource keeps the vector inside the SF64 engine's documented ±5000/±2000
// box — kept on the HRTF path so the by-ear-verified distance/pitch tuning is unchanged;
// whether the 3D backend still wants the clamp is a tuning follow-up. On top of the HRTF
// we drive pitch from the clamped Y: the generic HRTF only renders strong elevation when
// the target is nearly on the aim line (the vertical angle is tiny for most of the
// approach), so the raw-Y pitch supplies a distance-independent "above/below you" signal
// throughout.
static void AccessibilityCues_ComputeCueTarget(f32 dx, f32 dy, f32 dz, f32 outSrc[3], f32* outFreq) {
    outSrc[0] = dx;
    outSrc[1] = dy;
    outSrc[2] = dz;
    Object_ClampSfxSource(outSrc);
    *outFreq = AccessibilityCues_ComputeFreqModFromY(outSrc[1]);
}

// Point a single-voice cue at a listener-relative offset and start it if it isn't
// sounding yet.
static void AccessibilityCues_DriveCue(Cue* cue, f32 dx, f32 dy, f32 dz) {
    f32 src[3];
    f32 freq;
    AccessibilityCues_ComputeCueTarget(dx, dy, dz, src, &freq);
    cue->SetTarget(src[0], src[1], src[2], freq);
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

// Per-tick scan counters; populated by FindClosestEnemies, consumed by
// the listener's trace logs. Strictly debug-only; if we drop the tracing
// the struct can go too.
struct EnemyCueScanStats {
    s32 active;  // gActors slots with status == OBJ_ACTIVE
    s32 cueable; // also passed IsCueableEnemy predicate
    s32 kept;    // also passed the mode's direction/range filters
};

// One chosen enemy: what the cue voice needs (body-frame delta) plus what the sticky
// voice key and the trace log need (slot + actor).
struct EnemyCueTarget {
    Vec3f bodyDelta;
    f32 distSq;
    s32 slot; // gActors index
    Actor* actor;
};

// Sticky voice key for one enemy. gActors slots are reused when an enemy dies and a new
// one spawns, so the slot alone would let a voice silently glide onto the newcomer; fold
// in the two identity fields that distinguish enemy kinds (event-spawned actors all share
// id OBJ_ACTOR_EVENT and differ by eventType) so a reused slot gets a fresh key — and
// with it a stop/restart — unless the newcomer is the same kind of enemy.
static uint64_t AccessibilityCues_EnemyVoiceKey(const EnemyCueTarget* target) {
    return ((uint64_t) (uint32_t) target->slot << 32) | ((uint64_t) (uint16_t) target->actor->obj.id << 16) |
           (uint64_t) (uint16_t) target->actor->eventType;
}

// Walks gActors[], applies the lock-on predicate, rotates each candidate's
// world delta into body frame via the matrix the caller has already set on
// gCalcMatrix, and collects the `maxOut` candidates with the smallest 3D
// distance into `out`, sorted closest-first. The mode decides which candidates
// are in scope: on-rails keeps only enemies ahead of the aim line (behind-aim
// sources used to pan ambiguously, and everything relevant comes at you from
// ahead anyway); all-range keeps the full sphere — threats come from behind
// there and the HRTF renders the rear hemisphere — but drops anything past
// kEnemyCueAllRangeMaxDist. Returns how many it found (0..maxOut). Populates
// outStats for trace logging.
static s32 AccessibilityCues_FindClosestEnemies(Player* player, bool allRange, EnemyCueTarget* out, s32 maxOut,
                                                EnemyCueScanStats* outStats) {
    outStats->active = 0;
    outStats->cueable = 0;
    outStats->kept = 0;
    s32 count = 0;

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
        // bodyDelta.z >= 0 means at or behind the aim line.
        if (!allRange && bodyDelta.z >= 0.0f) {
            continue;
        }
        f32 distSq = (bodyDelta.x * bodyDelta.x) + (bodyDelta.y * bodyDelta.y) + (bodyDelta.z * bodyDelta.z);
        if (allRange && distSq > kEnemyCueAllRangeMaxDist * kEnemyCueAllRangeMaxDist) {
            continue;
        }
        outStats->kept++;
        // Insertion into the closest-first top-N array.
        s32 at;
        if (count == maxOut) {
            if (distSq >= out[maxOut - 1].distSq) {
                continue; // farther than everything kept so far
            }
            at = maxOut - 1; // displace the current farthest
        } else {
            at = count++;
        }
        while (at > 0 && out[at - 1].distSq > distSq) {
            out[at] = out[at - 1];
            at--;
        }
        out[at].bodyDelta = bodyDelta;
        out[at].distSq = distSq;
        out[at].slot = i;
        out[at].actor = actor;
    }
    return count;
}

static void AccessibilityCues_OnEnemyPostUpdate(IEvent* event) {
    (void) event;

    // The cue runs on-rails and in solo all-range; Versus shares LEVELMODE_ALL_RANGE
    // but is untested multiplayer territory, so gVersusMode gates it out. gLevelMode
    // and gPlayer both default to "ready-looking" zero values at process start
    // (LEVELMODE_ON_RAILS = 0, gPlayer = NULL pointer) before any level loads, so the
    // mode check alone doesn't filter the pre-game title/menu ticks. Null-check
    // gPlayer to keep the listener safe there.
    bool enabled = AccessibilityCues_IsEnabled();
    bool allRange = (gLevelMode == LEVELMODE_ALL_RANGE);
    bool modeOk = (gLevelMode == LEVELMODE_ON_RAILS) || (allRange && !gVersusMode);
    bool hasPlayer = (gPlayer != NULL);
    bool paused = AccessibilityCues_IsPaused();
    if (!enabled || !modeOk || !hasPlayer || paused) {
        ENEMY_CUE_TRACE("[enemy-cue] gated enabled={} mode={} versus={} hasPlayer={} paused={}", enabled,
                        (int) gLevelMode, gVersusMode, hasPlayer, paused);
        sEnemyCue->StopAllVoices();
        return;
    }

    Player* player = &gPlayer[0];
    AccessibilityCues_BuildWorldToBodyMatrix(player);

    EnemyCueScanStats stats;
    EnemyCueTarget targets[kAccessibilityEnemyCueMaxVoices];
    s32 count = AccessibilityCues_FindClosestEnemies(player, allRange, targets,
                                                     AccessibilityCues_EnemyCueVoiceCount(), &stats);

    if (count == 0) {
        ENEMY_CUE_TRACE("[enemy-cue] no target level={} active={} cueable={} kept={}", (int) gCurrentLevel,
                        stats.active, stats.cueable, stats.kept);
        sEnemyCue->StopAllVoices();
        return;
    }

    for (s32 i = 0; i < count; i++) {
        EnemyCueTarget* target = &targets[i];
        // Body-frame delta: +X right of aim, +Y above aim, -Z ahead. The cue takes
        // +z-ahead, so hand it the negated body Z (matching the ring cue's convention).
        f32 src[3];
        f32 freq;
        AccessibilityCues_ComputeCueTarget(target->bodyDelta.x, target->bodyDelta.y, -target->bodyDelta.z, src, &freq);
        sEnemyCue->TargetVoice(AccessibilityCues_EnemyVoiceKey(target), src[0], src[1], src[2], freq);

        // src/freq are the voice's post-clamp target — what was actually pushed this tick.
        // Voices this listener stops driving are reaped by CueRegistry_Tick.
        ENEMY_CUE_TRACE("[enemy-cue] voice {}/{} slot={} id={} level={} active={} cueable={} kept={} "
                        "bodyDelta=({:.1f},{:.1f},{:.1f}) src=({:.1f},{:.1f},{:.1f}) freq={:.3f}",
                        i + 1, count, target->slot, (int) target->actor->obj.id, (int) gCurrentLevel, stats.active,
                        stats.cueable, stats.kept, target->bodyDelta.x, target->bodyDelta.y, target->bodyDelta.z,
                        src[0], src[1], src[2], freq);
    }
}

// ===== Entry points =====

// Per-game-tick cue housekeeping: expires timed previews (the settings UI's "Preview"
// buttons stay alive-and-bounded even if the menu closes mid-preview) and reaps keyed
// voices whose targets the driving listeners stopped refreshing.
static void AccessibilityCues_OnCueTick(IEvent* event) {
    (void) event;
    CueRegistry_Tick();
}

void AccessibilityCues_Init() {
    CVarRegisterInteger("gAccessibilityAudioCues", 1);
    CVarRegisterInteger("gAccessibilityEnemyCueVoices", kAccessibilityEnemyCueDefaultVoices);
    CVarRegisterInteger("gAccessibilityEnemyCueLog", 0);

    sRingCue = CueRegistry_Register("Ring", "Ring guide", "Guides you toward the next training ring.",
                                    "assets/accessibility/ring.wav");
    sEnemyCue = CueRegistry_Register("Enemy", "Enemy locator",
                                     "Tracks the closest lockable enemies: ahead of your aim on rails, "
                                     "all around you in all-range mode.",
                                     "assets/accessibility/enemy.wav", kAccessibilityEnemyCueMaxVoices);

    REGISTER_LISTENER(GamePostUpdateEvent, AccessibilityCues_OnRingPostUpdate, EVENT_PRIORITY_NORMAL);
    REGISTER_LISTENER(GamePostUpdateEvent, AccessibilityCues_OnEnemyPostUpdate, EVENT_PRIORITY_NORMAL);
    REGISTER_LISTENER(GamePostUpdateEvent, AccessibilityCues_OnCueTick, EVENT_PRIORITY_NORMAL);
}

void AccessibilityCues_Exit() {
    // Stops every cue and drops the backend handles; the caller's next step
    // (Cue3D_Shutdown in Accessibility_Exit) frees the underlying sources. Any
    // listener tick after this point sees idle cues with no handle and no-ops.
    CueRegistry_UnloadAll();
}
