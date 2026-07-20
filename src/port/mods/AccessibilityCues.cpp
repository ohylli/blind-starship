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
static Cue* sAimCue = nullptr;

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

// True only while the player is actually flying the ship. The 3D cue backend
// runs its own OS audio device that the game's pause/cutscene handling never
// reaches, so a looping Cue3D source keeps sounding until we stop it by hand —
// the listeners treat "no control" as the signal to stop, and the first
// in-control tick re-acquires targets and restarts. Each clause has a case it
// alone catches:
//  - gGameState: the dev return-to-map shortcut (PortEnhancements.c) jumps to
//    GSTATE_MAP leaving gPlayer/gActors as stale-but-valid level memory, so
//    only the game state betrays that the level is gone.
//  - gPlayState: PLAY_PAUSE while paused (positions frozen, resume re-acquires
//    the same targets), PLAY_INIT during level setup.
//  - player state: cutscenes park the player outside PLAYERSTATE_ACTIVE —
//    LEVEL_INTRO on entry, STANDBY for mid-level scenes (Star Wolf entry in
//    fox_360.c, Katina's mothership in fox_ka.c), LEVEL_COMPLETE/DOWN/... for
//    victory and shot-down sequences. U_TURN is kept: it is player-initiated,
//    combat stays live through it (the game gates on ACTIVE || U_TURN all
//    over), and it's exactly when you want the enemy cue to help reacquire.
static bool AccessibilityCues_PlayerHasControl() {
    return gGameState == GSTATE_PLAY && gPlayState == PLAY_UPDATE && gPlayer != NULL &&
           (gPlayer[0].state == PLAYERSTATE_ACTIVE || gPlayer[0].state == PLAYERSTATE_U_TURN);
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

// Shared Y->pitch mapping for both cues: higher source = higher pitch. Two knobs,
// both live via CVars under Developer -> Blind Starship (see
// docs/accessibility-cues-tuning.md): the sensitivity divisor (world height per
// octave) and the max pitch deviation (octaves clamped at the height extremes). The
// whole effect is gated by the gAccessibilityCuePitchForHeight toggle; when off the
// cue keeps native pitch and height is conveyed only by the HRTF elevation. Defaults
// (1000 units/oct, ±1 octave) reproduce the original hard-coded mapping.
static f32 AccessibilityCues_ComputeFreqModFromY(f32 y) {
    if (CVarGetInteger(kCuePitchForHeightCVar, 1) != 1) {
        return 1.0f; // effect off: native pitch, height not conveyed
    }
    // The guards below are written as !(x >= lo) rather than (x < lo) on purpose: every
    // comparison against NaN is false, so the natural spelling would wave a NaN straight
    // through. These CVars are reachable from the console and a hand-edited config, and a
    // NaN here would ride out as a NaN playback rate, which used to be an out-of-bounds
    // read on the audio thread. Cue3D_SetPitch validates too — this just keeps the bad
    // value from ever being built.
    f32 scale = CVarGetFloat(kCuePitchScaleCVar, kCuePitchScaleDefault);
    if (!(scale >= 1.0f)) {
        scale = kCuePitchScaleDefault; // divide-by-tiny, negative, or NaN
    }
    f32 range = CVarGetFloat(kCuePitchRangeOctavesCVar, kCuePitchRangeOctavesDefault);
    if (!(range >= 0.0f)) {
        range = kCuePitchRangeOctavesDefault; // negative or NaN
    }
    f32 octaves = y / scale;
    if (octaves > range) {
        octaves = range;
    } else if (octaves < -range) {
        octaves = -range;
    } else if (!(octaves == octaves)) {
        octaves = 0.0f; // NaN y: neither clamp fires, so pin to native pitch
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

    // PlayerHasControl also covers the gPlayer null guard: gPlayer is a pointer
    // (sf64context.h:324), zero-initialized at process start and only allocated
    // when a level loads, and this listener fires on GamePostUpdateEvent which
    // can tick before that.
    if (!AccessibilityCues_IsEnabled() || gCurrentLevel != LEVEL_TRAINING || !AccessibilityCues_PlayerHasControl()) {
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
    // defaults to a "ready-looking" zero value at process start (LEVELMODE_ON_RAILS
    // = 0) before any level loads, so the mode check alone doesn't filter the
    // pre-game title/menu ticks; PlayerHasControl (which also null-checks gPlayer)
    // does.
    bool enabled = AccessibilityCues_IsEnabled();
    bool allRange = (gLevelMode == LEVELMODE_ALL_RANGE);
    bool modeOk = (gLevelMode == LEVELMODE_ON_RAILS) || (allRange && !gVersusMode);
    bool control = AccessibilityCues_PlayerHasControl();
    if (!enabled || !modeOk || !control) {
        ENEMY_CUE_TRACE("[enemy-cue] gated enabled={} mode={} versus={} control={}", enabled, (int) gLevelMode,
                        gVersusMode, control);
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

// ===== Aim cue =====
//
// A repeated synthesized click whose stereo pan and pitch tell the player where they are
// AIMING — unlike the ring/enemy cues it encodes the aim itself, not a target. Rendered in
// CUE3D_MODE_PAN (plain stereo, no HRTF) at the backend's unity-gain distance, so distance
// attenuation never varies and pan/pitch are pure functions of the aim. On rails the encoded
// quantity is the projected aim point relative to the corridor center — lateral drift plus
// the stick deflection projected kAimCueProjDistCVar ahead, normalized by the corridor
// half-extents (pathWidth/pathHeight, the same box the engine clamps flight to) — so with a
// neutral stick the cue doubles as a "where am I on screen" indicator. In all-range there is
// no corridor: pan encodes the stick's yaw deflection (turning) and pitch the aim's world
// elevation angle. On top of both, the click repeats faster as the aim line passes closer to
// a lockable enemy (geiger-counter style), reusing the enemy cue's predicate and scoping.

// Synthesized click: ~12 ms of silence, then a ~20 ms damped-sine tick, at the backend rate.
// The lead-in exists because the backend fades each interval restart in from silence over
// ~5 ms (the click guard) and the tick's energy sits entirely in its first few milliseconds —
// without the lead-in the ramp eats the attack (~6 dB of loudness). The silence lets the ramp
// open before the transient hits; it also delays every pulse by a constant 12 ms, inaudible
// for a cadence signal. (The RESAMPLE pitch path scales the lead-in with pitch — at +1 octave
// it halves to 6 ms, so the extremes give back a little of that loudness; the gain-boost knob
// below covers the rest.) The whole buffer must finish inside the fastest geiger interval
// (slider floor in ImguiUI.cpp) so pulses never truncate; the raised-cosine tail pins the
// buffer to zero so the interval restart's rewind cannot click.
static std::vector<float> AccessibilityCues_GenerateAimClick(int sampleRate) {
    constexpr f32 kTwoPi = 6.2831853f;
    constexpr f32 kLeadInSec = 0.012f;
    constexpr f32 kTickSec = 0.02f;
    constexpr f32 kToneHz = 1500.0f;
    constexpr f32 kDecayPerSec = 150.0f;
    constexpr f32 kAmplitude = 0.95f;
    constexpr f32 kTailSec = 0.003f;
    int lead = (int) (kLeadInSec * (f32) sampleRate);
    int frames = (int) (kTickSec * (f32) sampleRate);
    if (frames < 2) {
        frames = 2;
    }
    int tail = (int) (kTailSec * (f32) sampleRate);
    std::vector<float> pcm((size_t) (lead + frames), 0.0f);
    for (int i = 0; i < frames; i++) {
        f32 t = (f32) i / (f32) sampleRate;
        f32 env = expf(-kDecayPerSec * t);
        int remaining = frames - 1 - i;
        if (remaining < tail) {
            env *= 0.5f * (1.0f - cosf(0.5f * kTwoPi * (f32) remaining / (f32) tail));
        }
        pcm[(size_t) (lead + i)] = kAmplitude * env * sinf(kTwoPi * kToneHz * t);
    }
    return pcm;
}

// Clamp a normalized aim signal to [-1, 1]; NaN pins to center. The divisors upstream are
// engine-owned (pathWidth/pathHeight) or guarded CVars, but a zero divisor's ±inf still
// lands on a sane extreme here instead of riding into the pan/pitch math.
static f32 AccessibilityCues_ClampUnit(f32 v) {
    if (v > 1.0f) {
        return 1.0f;
    }
    if (v < -1.0f) {
        return -1.0f;
    }
    return (v == v) ? v : 0.0f;
}

// Normalized vertical aim [-1, 1] -> playback rate 2^(n * octaves). Deliberately separate
// from AccessibilityCues_ComputeFreqModFromY: that maps a world-space HEIGHT (units per
// octave) for HRTF cues, this maps an already-normalized aim signal, and the aim cue must
// keep conveying the vertical even when the height-to-pitch toggle is off (in PAN mode
// pitch is the only vertical channel there is).
static f32 AccessibilityCues_AimPitch(f32 n) {
    f32 octaves = CVarGetFloat(kAimCueOctavesCVar, kAimCueOctavesDefault);
    if (!(octaves >= 0.0f)) {
        octaves = kAimCueOctavesDefault; // negative or NaN
    }
    return powf(2.0f, AccessibilityCues_ClampUnit(n) * octaves);
}

// Smallest aim-to-enemy angle -> click repeat interval, linear in angle: at or past the max
// angle — and when no enemy is in scope (angle = INFINITY) — the click idles at the slow
// interval; dead on a target it reaches the fast one. NaN-guarded in the file's usual
// !(x >= lo) style.
static f32 AccessibilityCues_AimInterval(f32 angleRad) {
    f32 maxAngle = CVarGetFloat(kAimCueGeigerAngleCVar, kAimCueGeigerAngleDefault) * M_DTOR;
    f32 slow = CVarGetFloat(kAimCueGeigerSlowCVar, kAimCueGeigerSlowDefault);
    f32 fast = CVarGetFloat(kAimCueGeigerFastCVar, kAimCueGeigerFastDefault);
    if (!(maxAngle > 0.0f)) {
        maxAngle = kAimCueGeigerAngleDefault * M_DTOR;
    }
    if (!(slow > 0.0f)) {
        slow = kAimCueGeigerSlowDefault;
    }
    if (!(fast > 0.0f)) {
        fast = kAimCueGeigerFastDefault;
    }
    if (!(angleRad < maxAngle)) {
        return slow; // no target (INFINITY), NaN, or wider than the max angle
    }
    f32 t = angleRad / maxAngle;
    if (t < 0.0f) {
        t = 0.0f;
    }
    return fast + (slow - fast) * t;
}

// Smallest angle between the aim line and any in-scope cueable enemy, radians; INFINITY if
// none. Same predicate and scoping as the enemy cue's scan: ahead of the aim only on rails,
// full sphere within kEnemyCueAllRangeMaxDist in all-range. Assumes the caller just ran
// AccessibilityCues_BuildWorldToBodyMatrix (gCalcMatrix holds world -> body).
static f32 AccessibilityCues_MinEnemyAimAngle(Player* player, bool allRange) {
    f32 best = INFINITY;
    for (s32 i = 0; i < ARRAY_COUNT(gActors); i++) {
        Actor* actor = &gActors[i];
        if (!AccessibilityCues_IsCueableEnemy(actor)) {
            continue;
        }
        Vec3f worldDelta;
        worldDelta.x = actor->obj.pos.x - player->pos.x;
        worldDelta.y = actor->obj.pos.y - player->pos.y;
        worldDelta.z = actor->obj.pos.z - player->trueZpos;
        Vec3f bodyDelta;
        Matrix_MultVec3fNoTranslate(gCalcMatrix, &worldDelta, &bodyDelta);
        if (!allRange && bodyDelta.z >= 0.0f) {
            continue; // at or behind the aim line
        }
        f32 distSq = (bodyDelta.x * bodyDelta.x) + (bodyDelta.y * bodyDelta.y) + (bodyDelta.z * bodyDelta.z);
        if (allRange && distSq > kEnemyCueAllRangeMaxDist * kEnemyCueAllRangeMaxDist) {
            continue;
        }
        // 0 on the aim line, growing toward pi dead behind (reachable in all-range only).
        f32 lateral = sqrtf((bodyDelta.x * bodyDelta.x) + (bodyDelta.y * bodyDelta.y));
        f32 angle = atan2f(lateral, -bodyDelta.z);
        if (angle < best) {
            best = angle;
        }
    }
    return best;
}

static void AccessibilityCues_OnAimPostUpdate(IEvent* event) {
    (void) event;

    // Loudness normalization against the sustained cues (the setter sanitizes and no-ops
    // while stable). Pushed before the gate, not after: the settings-menu preview must
    // honor the boost (and track its slider) even when gameplay is gated off.
    sAimCue->SetGainBoost(CVarGetFloat(kAimCueBoostCVar, kAimCueBoostDefault));

    bool allRange = (gLevelMode == LEVELMODE_ALL_RANGE);
    bool modeOk = (gLevelMode == LEVELMODE_ON_RAILS) || (allRange && !gVersusMode);
    bool control = AccessibilityCues_PlayerHasControl();
    // v1 is Arwing-only: the mappings below read the Arwing's aim fields. Landmaster /
    // Blue-Marine / on-foot need their own mappings (future work). `control` guarantees
    // gPlayer is non-null before the form read.
    bool arwing = control && (gPlayer[0].form == FORM_ARWING);
    if (!AccessibilityCues_IsEnabled() || CVarGetInteger(kAimCueEnabledCVar, 1) != 1 || !modeOk || !arwing) {
        sAimCue->Stop();
        return;
    }
    Player* player = &gPlayer[0];

    // Normalized aim signals, both [-1, 1]: nx -> pan (positive = right), ny -> pitch
    // (positive = up). Sign notes, derived from the engine and worth keeping straight:
    // the stick is NEGATED into rot (fox_play.c:4005/:4064), so rot.y < 0 means "aiming
    // right", and a positive total pitch angle means "aiming up" (Player_SetupArwingShot's
    // matrix maps positive pitch to +Y velocity). Hence the minus on the yaw terms and the
    // plus on the pitch terms below.
    f32 nx;
    f32 ny;
    if (allRange) {
        f32 yawRange = CVarGetFloat(kAimCueYawRangeCVar, kAimCueYawRangeDefault);
        if (!(yawRange >= 1.0f)) {
            yawRange = kAimCueYawRangeDefault; // tiny, negative, or NaN
        }
        f32 pitchRange = CVarGetFloat(kAimCuePitchRangeDegCVar, kAimCuePitchRangeDegDefault);
        if (!(pitchRange >= 1.0f)) {
            pitchRange = kAimCuePitchRangeDegDefault;
        }
        nx = AccessibilityCues_ClampUnit(-player->rot.y / yawRange);
        ny = AccessibilityCues_ClampUnit((player->xRot_120 + player->rot.x + player->aerobaticPitch) / pitchRange);
    } else {
        f32 dist = CVarGetFloat(kAimCueProjDistCVar, kAimCueProjDistDefault);
        if (!(dist >= 0.0f)) {
            dist = kAimCueProjDistDefault; // negative or NaN
        }
        // Projected aim point relative to the corridor center: lateral drift plus the
        // stick deflection carried `dist` units ahead. xRot_120/yRot_114 (the path's own
        // direction) stay out of the deflection — the path forward IS the neutral center.
        f32 horiz = (player->pos.x - player->xPath) - dist * sinf(player->rot.y * M_DTOR);
        f32 vert = (player->pos.y - player->yPath) + dist * sinf((player->rot.x + player->aerobaticPitch) * M_DTOR);
        nx = AccessibilityCues_ClampUnit(horiz / player->pathWidth);
        ny = AccessibilityCues_ClampUnit(vert / player->pathHeight);
    }

    AccessibilityCues_BuildWorldToBodyMatrix(player);
    f32 interval = AccessibilityCues_AimInterval(AccessibilityCues_MinEnemyAimAngle(player, allRange));

    // Place the source on the unity-gain arc: PAN mode derives its pan purely from the
    // horizontal direction (x over the x/z length), so radius * (nx, 0, sqrt(1 - nx^2))
    // renders a constant-power pan of exactly nx with distance attenuation pinned at
    // unity. Y is irrelevant to PAN; the vertical is carried by pitch instead.
    f32 radius = Cue3D_GetUnityGainDistance();
    if (!(radius > 0.0f)) {
        radius = 100.0f; // backend not up yet; any positive radius pans the same
    }
    CueTarget target;
    target.x = radius * nx;
    target.y = 0.0f;
    target.z = radius * sqrtf(1.0f - nx * nx);
    target.pitch = AccessibilityCues_AimPitch(ny);
    target.intervalSec = interval;
    sAimCue->SetTarget(target);
    sAimCue->Start();
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
    CVarRegisterInteger(kCuePitchForHeightCVar, 1);
    CVarRegisterFloat(kCuePitchScaleCVar, kCuePitchScaleDefault);
    CVarRegisterFloat(kCuePitchRangeOctavesCVar, kCuePitchRangeOctavesDefault);
    CVarRegisterInteger(kAimCueEnabledCVar, 1);
    CVarRegisterFloat(kAimCueProjDistCVar, kAimCueProjDistDefault);
    CVarRegisterFloat(kAimCueYawRangeCVar, kAimCueYawRangeDefault);
    CVarRegisterFloat(kAimCuePitchRangeDegCVar, kAimCuePitchRangeDegDefault);
    CVarRegisterFloat(kAimCueOctavesCVar, kAimCueOctavesDefault);
    CVarRegisterFloat(kAimCueGeigerAngleCVar, kAimCueGeigerAngleDefault);
    CVarRegisterFloat(kAimCueGeigerFastCVar, kAimCueGeigerFastDefault);
    CVarRegisterFloat(kAimCueGeigerSlowCVar, kAimCueGeigerSlowDefault);
    CVarRegisterFloat(kAimCueBoostCVar, kAimCueBoostDefault);

    sRingCue = CueRegistry_Register("Ring", "Ring guide", "Guides you toward the next training ring.",
                                    { .wavPath = "assets/accessibility/ring.wav" });
    sEnemyCue = CueRegistry_Register("Enemy", "Enemy locator",
                                     "Tracks the closest lockable enemies: ahead of your aim on rails, "
                                     "all around you in all-range mode.",
                                     { .wavPath = "assets/accessibility/enemy.wav",
                                       .maxVoices = kAccessibilityEnemyCueMaxVoices });
    // PAN render mode: the pan/pitch ARE the signal, so no HRTF; pinned RESAMPLE pitch: the
    // spectral shifter's latency and transient softening would smear the click's attack.
    sAimCue = CueRegistry_Register("Aim", "Aim guide",
                                   "A repeating click that tells you where you are aiming: pan for "
                                   "left/right, pitch for up/down; it clicks faster as your aim nears "
                                   "a lockable enemy.",
                                   { .generator = AccessibilityCues_GenerateAimClick,
                                     .mode = CUE3D_MODE_PAN,
                                     .pitchStyle = CUE3D_SOURCE_PITCH_RESAMPLE });

    REGISTER_LISTENER(GamePostUpdateEvent, AccessibilityCues_OnRingPostUpdate, EVENT_PRIORITY_NORMAL);
    REGISTER_LISTENER(GamePostUpdateEvent, AccessibilityCues_OnEnemyPostUpdate, EVENT_PRIORITY_NORMAL);
    REGISTER_LISTENER(GamePostUpdateEvent, AccessibilityCues_OnAimPostUpdate, EVENT_PRIORITY_NORMAL);
    REGISTER_LISTENER(GamePostUpdateEvent, AccessibilityCues_OnCueTick, EVENT_PRIORITY_NORMAL);
}

void AccessibilityCues_Exit() {
    // Stops every cue and drops the backend handles; the caller's next step
    // (Cue3D_Shutdown in Accessibility_Exit) frees the underlying sources. Any
    // listener tick after this point sees idle cues with no handle and no-ops.
    CueRegistry_UnloadAll();
}
