#include "AccessibilityCues.h"

#include <math.h>

#include "port/CGameCompat.h"
#include "sfx.h"
#include "port/hooks/Events.h"
#include "port/accessibility/Cue3D.h"

#include <spdlog/spdlog.h>

// Ring-cue SFX. Picked from a scan of bank-1/2/3 SFX in include/sfx.h:
// range 3 (audible from ~6350 units, well beyond the ~3000-unit ring spawn
// distance), no SFX_FLAG_22 so distance attenuation still applies, no
// SFX_FLAG_23 so the Y->pitch mapping isn't muddied by random per-frame
// wobble, and a high importance byte so the cue won't get evicted under
// polyphony pressure.
//
// Other bank-1/2/3 candidates considered, all range 3 unless noted:
//   NA_SE_GREATFOX_ENGINE     (0x11030010) — low ship drone; importance 0
//                                            means it evicts first when the
//                                            SFX pool fills.
//   NA_SE_EN_S_BEAM_CHARGE    (0x31016056) — cleanest tonal charge of the
//                                            shortlist, but range 1 (~2200u)
//                                            so it cuts out near ring spawn.
// (NA_SE_KA_UFO_ENGINE from the original shortlist is now the enemy cue.)
#define RING_CUE_SFX NA_SE_EN_GRN_BEAM_CHARGE

// Enemy-cue SFX. NA_SE_KA_UFO_ENGINE (0x11037025): bank 1, range 3, no
// SFX_FLAG_22/23, importance 0x70. Sustained UFO-engine whir — distinct
// timbre from the ring cue's beam-charge tone so the two can fire
// simultaneously on Training without blending into one sound.
#define ENEMY_CUE_SFX NA_SE_KA_UFO_ENGINE

// Engine reads these pointers every audio frame, so they must outlive the
// Audio_PlaySfx call. File-static is the right scope. The two cues each
// keep their own state block; both can fire at the same time.
static f32 sRingCueSrc[3] = { 0.0f, 0.0f, 0.0f };
static f32 sRingCueFreqMod = 1.0f;
static f32 sRingCueVolMod = 1.0f;
static s8 sRingCueReverb = 0;

// Which backend is currently sounding the ring cue. Lets a mid-level
// gAccessibilityCue3D flip stop the old backend before starting the other so
// the two never overlap.
enum RingCueBackend { RING_CUE_NONE, RING_CUE_SFX_ENGINE, RING_CUE_3D };
static RingCueBackend sRingCueBackend = RING_CUE_NONE;

// HRTF ring cue (gAccessibilityCue3D). The ring.wav source is loaded lazily on
// first use — Cue3D_Init opens a second OS audio device, so we defer that until
// the cue is actually wanted. Cached for the process lifetime; freed wholesale
// by Cue3D_Shutdown in Accessibility_Exit (after which this pointer dangles but
// is never touched again).
static Cue3DSource* sRing3DSource = nullptr;
static bool sRing3DLoadFailed = false; // don't retry a failed load every frame

static f32 sEnemyCueSrc[3] = { 0.0f, 0.0f, 0.0f };
static f32 sEnemyCueFreqMod = 1.0f;
static f32 sEnemyCueVolMod = 1.0f;
static s8 sEnemyCueReverb = 0;
static bool sEnemyCueActive = false;

static bool AccessibilityCues_IsEnabled() {
    return CVarGetInteger("gAccessibilityAudioCues", 1) == 1;
}

// When on, the ring cue plays through the Cue3D HRTF backend (ring.wav) instead
// of the SF64 audio engine. Runtime-toggleable; the listener switches backends
// on the next tick. Default on.
static bool AccessibilityCues_IsCue3DEnabled() {
    return CVarGetInteger("gAccessibilityCue3D", 1) == 1;
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

// Distance attenuation for the Cue3D (HRTF) ring cue. Steam Audio's binaural
// effect applies no distance falloff of its own (it only spatializes
// direction), so we model it here and push the result through Cue3D_SetGain.
// The SF64 audio-engine path doesn't use this — that engine attenuates
// internally (see docs/audio-system.md §7).
//
// This is deliberately NOT a physically-correct 1/d curve: a navigation cue
// must stay clearly audible while the target is still far (rings spawn ~3000
// units ahead), so the curve floors at a still-audible minimum instead of
// fading to silence, and only conveys "am I closing in" on top of the HRTF's
// direction and the pitch's elevation. Three knobs, documented in
// docs/accessibility-cues-tuning.md:
//   - kCueNear: full volume at or inside this distance (world units).
//   - kCueFar:  floored at or beyond this distance.
//   - kCueFloorGain: the never-go-quieter-than floor (0..1), so the cue is
//     never lost even at max spawn distance.
// Between near and far it tapers quadratically (mirroring the SF64 engine's
// squared falloff curve).
static const f32 kCueNear = 500.0f;
static const f32 kCueFar = 4500.0f;
static const f32 kCueFloorGain = 0.3f;

static f32 AccessibilityCues_ComputeGainFromDistance(f32 distance) {
    if (distance <= kCueNear) {
        return 1.0f;
    }
    if (distance >= kCueFar) {
        return kCueFloorGain;
    }
    f32 t = (distance - kCueNear) / (kCueFar - kCueNear);
    return 1.0f - (1.0f - kCueFloorGain) * (t * t);
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

// Lazily load (and cache) the ring.wav HRTF source. Returns NULL if the Cue3D
// backend isn't compiled in (e.g. Switch) or the file can't be loaded, in which
// case we record the failure so we don't retry it every frame.
static Cue3DSource* AccessibilityCues_GetRing3DSource() {
    if (sRing3DSource != NULL) {
        return sRing3DSource;
    }
    if (sRing3DLoadFailed) {
        return NULL;
    }
    Cue3D_Init(); // idempotent; opens the device + HRTF on first use only
    std::string path = Ship::Context::GetPathRelativeToAppDirectory("assets/accessibility/ring.wav");
    sRing3DSource = Cue3D_Load(path.c_str(), true); // loop: continuous while a ring is ahead
    if (sRing3DSource == NULL) {
        sRing3DLoadFailed = true;
        SPDLOG_WARN("AccessibilityCues: Cue3D ring cue unavailable (failed to load '{}')", path);
    }
    return sRing3DSource;
}

static void AccessibilityCues_RefreshRingSource(Item* ring) {
    Player* player = &gPlayer[0];
    // Player-relative bypass of Object_SetSfxSourceToPos. The converter pans
    // by camera position, but on-rails mode (which training is) decouples the
    // camera from the Arwing's lateral drift; a ring "in front of the camera"
    // can be off to the player's right. See docs/audio-system.md section 6.
    sRingCueSrc[0] = ring->obj.pos.x - player->pos.x;
    sRingCueSrc[1] = ring->obj.pos.y - player->pos.y;
    sRingCueSrc[2] = -(ring->obj.pos.z - player->trueZpos);
    Object_ClampSfxSource(sRingCueSrc);
    sRingCueFreqMod = AccessibilityCues_ComputeFreqModFromY(sRingCueSrc[1]);
    // The HRTF backend needs the position pushed every frame (the SF64 engine
    // reads the sRingCueSrc pointer itself, so its path needs no per-frame call).
    // Pass the vector verbatim in the game convention (+x right, +y up, +z
    // ahead); the backend negates Z internally. On top of the HRTF we also drive
    // pitch from the raw clamped Y (sRingCueFreqMod): the generic HRTF only renders
    // strong elevation when the ring is nearly on the aim line (the vertical angle
    // is tiny for most of the approach), so the raw-Y pitch supplies a
    // distance-independent "ring is above/below you" signal throughout the flight.
    // This matches the SFX-engine path, which already passes sRingCueFreqMod.
    if (sRingCueBackend == RING_CUE_3D) {
        Cue3D_SetPosition(sRing3DSource, sRingCueSrc[0], sRingCueSrc[1], sRingCueSrc[2]);
        Cue3D_SetPitch(sRing3DSource, sRingCueFreqMod);
        // The HRTF backend applies no distance falloff; model it here. y/2.5
        // de-emphasises vertical separation (mirrors the SF64 engine, and keeps
        // a high ring from reading as "far" when elevation is already carried by
        // pitch + HRTF).
        f32 yScaled = sRingCueSrc[1] / 2.5f;
        f32 distance = sqrtf((sRingCueSrc[0] * sRingCueSrc[0]) + (yScaled * yScaled) + (sRingCueSrc[2] * sRingCueSrc[2]));
        Cue3D_SetGain(sRing3DSource, AccessibilityCues_ComputeGainFromDistance(distance));
    }
}

static void AccessibilityCues_StartRingCue(Item* ring) {
    if (AccessibilityCues_IsCue3DEnabled()) {
        Cue3DSource* source = AccessibilityCues_GetRing3DSource();
        if (source == NULL) {
            // 3D selected but unavailable: no cue. The SF64 path is deliberately
            // suppressed while the 3D backend is chosen.
            return;
        }
        sRingCueBackend = RING_CUE_3D;
        AccessibilityCues_RefreshRingSource(ring); // computes src + pushes position
        Cue3D_Play(source);
    } else {
        AccessibilityCues_RefreshRingSource(ring);
        Audio_PlaySfx(RING_CUE_SFX, sRingCueSrc, 0, &sRingCueFreqMod, &sRingCueVolMod, &sRingCueReverb);
        sRingCueBackend = RING_CUE_SFX_ENGINE;
    }
}

static void AccessibilityCues_StopRingCue() {
    switch (sRingCueBackend) {
        case RING_CUE_SFX_ENGINE:
            Audio_KillSfxBySource(sRingCueSrc);
            break;
        case RING_CUE_3D:
            Cue3D_Stop(sRing3DSource);
            break;
        case RING_CUE_NONE:
            return;
    }
    sRingCueBackend = RING_CUE_NONE;
}

static void AccessibilityCues_OnRingPostUpdate(IEvent* event) {
    (void) event;

    // gPlayer is a pointer (sf64context.h:324), zero-initialized at process
    // start and only allocated when a level loads. The listener fires on
    // GamePostUpdateEvent which can tick before that — guard it.
    if (!AccessibilityCues_IsEnabled() || gCurrentLevel != LEVEL_TRAINING || gPlayer == NULL) {
        AccessibilityCues_StopRingCue();
        return;
    }

    Item* target = AccessibilityCues_FindNextTrainingRing();
    if (target == NULL) {
        AccessibilityCues_StopRingCue();
        return;
    }

    // Stop the active backend if the CVar was flipped mid-level, so the two
    // never sound at once.
    RingCueBackend desired = AccessibilityCues_IsCue3DEnabled() ? RING_CUE_3D : RING_CUE_SFX_ENGINE;
    if (sRingCueBackend != RING_CUE_NONE && sRingCueBackend != desired) {
        AccessibilityCues_StopRingCue();
    }

    if (sRingCueBackend == RING_CUE_NONE) {
        AccessibilityCues_StartRingCue(target);
    } else {
        AccessibilityCues_RefreshRingSource(target);
    }
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

static void AccessibilityCues_RefreshEnemySource(Vec3f bodyDelta) {
    // src[0] = body X (positive = right of aim); src[1] = body Y (positive
    // = above aim); src[2] = -body Z so positive = ahead of aim, matching
    // the ring cue's sign convention. The engine clamps internally but
    // Object_ClampSfxSource keeps us inside the documented ±5000/±2000 box.
    sEnemyCueSrc[0] = bodyDelta.x;
    sEnemyCueSrc[1] = bodyDelta.y;
    sEnemyCueSrc[2] = -bodyDelta.z;
    Object_ClampSfxSource(sEnemyCueSrc);
    sEnemyCueFreqMod = AccessibilityCues_ComputeFreqModFromY(sEnemyCueSrc[1]);
}

static void AccessibilityCues_StartEnemyCue(Vec3f bodyDelta) {
    AccessibilityCues_RefreshEnemySource(bodyDelta);
    ENEMY_CUE_TRACE("[enemy-cue] START sfx=0x{:08X} src=({:.1f},{:.1f},{:.1f}) freq={:.3f} vol={:.3f}",
                    (u32) ENEMY_CUE_SFX, sEnemyCueSrc[0], sEnemyCueSrc[1], sEnemyCueSrc[2],
                    sEnemyCueFreqMod, sEnemyCueVolMod);
    Audio_PlaySfx(ENEMY_CUE_SFX, sEnemyCueSrc, 0, &sEnemyCueFreqMod, &sEnemyCueVolMod, &sEnemyCueReverb);
    sEnemyCueActive = true;
}

static void AccessibilityCues_StopEnemyCue() {
    if (!sEnemyCueActive) {
        return;
    }
    ENEMY_CUE_TRACE("[enemy-cue] STOP last_src=({:.1f},{:.1f},{:.1f})",
                    sEnemyCueSrc[0], sEnemyCueSrc[1], sEnemyCueSrc[2]);
    Audio_KillSfxBySource(sEnemyCueSrc);
    sEnemyCueActive = false;
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
    if (!enabled || !onRails || !hasPlayer) {
        ENEMY_CUE_TRACE("[enemy-cue] gated enabled={} onRails={} mode={} hasPlayer={} active={}",
                        enabled, onRails, (int) gLevelMode, hasPlayer, sEnemyCueActive);
        AccessibilityCues_StopEnemyCue();
        return;
    }

    Player* player = &gPlayer[0];
    AccessibilityCues_BuildWorldToBodyMatrix(player);

    EnemyCueScanStats stats;
    Vec3f bodyDelta;
    bool found = AccessibilityCues_FindClosestEnemyAhead(player, &bodyDelta, &stats);

    if (!found) {
        ENEMY_CUE_TRACE("[enemy-cue] no target level={} active={} cueable={} ahead={} cueActive={}",
                        (int) gCurrentLevel, stats.active, stats.cueable, stats.ahead, sEnemyCueActive);
        AccessibilityCues_StopEnemyCue();
        return;
    }

    if (!sEnemyCueActive) {
        AccessibilityCues_StartEnemyCue(bodyDelta);
    } else {
        AccessibilityCues_RefreshEnemySource(bodyDelta);
    }

    // Logged after Start/Refresh so the src/freq values reflect this tick's
    // refresh, not the previous one's residue.
    ENEMY_CUE_TRACE("[enemy-cue] target id={} level={} active={} cueable={} ahead={} bodyDelta=({:.1f},{:.1f},{:.1f}) src=({:.1f},{:.1f},{:.1f}) freq={:.3f}",
                    (int) stats.chosen->obj.id, (int) gCurrentLevel,
                    stats.active, stats.cueable, stats.ahead,
                    bodyDelta.x, bodyDelta.y, bodyDelta.z,
                    sEnemyCueSrc[0], sEnemyCueSrc[1], sEnemyCueSrc[2],
                    sEnemyCueFreqMod);
}

// ===== Entry points =====

void AccessibilityCues_Init() {
    CVarRegisterInteger("gAccessibilityAudioCues", 1);
    CVarRegisterInteger("gAccessibilityCue3D", 1);
    CVarRegisterInteger("gAccessibilityEnemyCueLog", 0);
    REGISTER_LISTENER(GamePostUpdateEvent, AccessibilityCues_OnRingPostUpdate, EVENT_PRIORITY_NORMAL);
    REGISTER_LISTENER(GamePostUpdateEvent, AccessibilityCues_OnEnemyPostUpdate, EVENT_PRIORITY_NORMAL);
}

void AccessibilityCues_Exit() {
    AccessibilityCues_StopRingCue();
    AccessibilityCues_StopEnemyCue();
}
