#include "EnemyCue.h"
#include "CueCommon.h"
#include "CueScan.h"

#include <math.h>

#include "port/CGameCompat.h"
#include "port/hooks/Events.h"
#include "port/accessibility/Cue.h"
#include "port/mods/Accessibility.h"

#include <spdlog/spdlog.h>

// The enemy cue: voices the closest lockable enemies relative to the Arwing's aim —
// ahead of the aim line on on-rails levels, the full sphere (within a range limit) in
// solo all-range mode. Registered in EnemyCue_Register; process-lifetime (the registry
// never frees). Design rationale: docs/accessibility-enemy-cue.md.
static Cue* sEnemyCue = nullptr;

static EnemyCueDebug sDebugState;

const EnemyCueDebug& EnemyCue_DebugState() {
    return sDebugState;
}

// How many of the closest enemies to voice at once. Runtime-tunable (F1 -> Blind
// Starship) so the by-ear sweet spot can be found without rebuilding.
int32_t EnemyCue_VoiceCount() {
    s32 count = CVarGetInteger(kEnemyCueVoicesCVar, kAccessibilityEnemyCueDefaultVoices);
    if (count < 1) {
        count = 1;
    } else if (count > kAccessibilityEnemyCueMaxVoices) {
        count = kAccessibilityEnemyCueMaxVoices;
    }
    return count;
}

// Per-tick diagnostic trace for the enemy cue. Off by default. Even when on,
// emitted lines still go through SPDLOG_TRACE, so the global log threshold
// (gDeveloperTools.LogLevel) must also be at "trace" to actually appear in
// the log. Pattern matches gObjectSpawnLog. Use ENEMY_CUE_TRACE(...) below.
static bool EnemyCue_IsLogEnabled() {
    return CVarGetInteger(kEnemyCueLogCVar, 0) == 1;
}

#define ENEMY_CUE_TRACE(...)              \
    do {                                  \
        if (EnemyCue_IsLogEnabled()) {    \
            SPDLOG_TRACE(__VA_ARGS__);    \
        }                                 \
    } while (0)

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
static uint64_t EnemyCue_VoiceKey(const EnemyCueTarget* target) {
    return ((uint64_t) (uint32_t) target->slot << 32) | ((uint64_t) (uint16_t) target->actor->obj.id << 16) |
           (uint64_t) (uint16_t) target->actor->eventType;
}

// Collects the `maxOut` in-scope enemies with the smallest 3D distance into `out`,
// sorted closest-first, via the shared scan (predicate and per-mode scoping in
// CueScan.h). Returns how many it found (0..maxOut). Populates outStats for trace
// logging and the debug mirror.
static s32 EnemyCue_FindClosest(Player* player, bool allRange, EnemyCueTarget* out, s32 maxOut,
                                CueScanStats* outStats) {
    s32 count = 0;
    CueScan_ForEachCueableEnemy(player, allRange, outStats,
                                [&](Actor* actor, s32 slot, const Vec3f& bodyDelta, f32 distSq) {
                                    // Insertion into the closest-first top-N array.
                                    s32 at;
                                    if (count == maxOut) {
                                        if (distSq >= out[maxOut - 1].distSq) {
                                            return; // farther than everything kept so far
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
                                    out[at].slot = slot;
                                    out[at].actor = actor;
                                });
    return count;
}

static void EnemyCue_OnPostUpdate(IEvent* event) {
    (void) event;

    sDebugState = EnemyCueDebug{};
    EnemyCueDebug& dbg = sDebugState;
    dbg.enabled = CueCommon_IsEnabled();
    dbg.versus = gVersusMode;
    dbg.modeOk = CueScan_ModeInScope(&dbg.allRange);
    dbg.control = Accessibility_PlayerHasControl();
    dbg.frame = (int32_t) gGameFrameCount;
    if (!dbg.enabled || !dbg.modeOk || !dbg.control) {
        ENEMY_CUE_TRACE("[enemy-cue] gated enabled={} mode={} versus={} control={}", dbg.enabled, (int) gLevelMode,
                        gVersusMode, dbg.control);
        sEnemyCue->StopAllVoices();
        return;
    }

    Player* player = &gPlayer[0];
    CueScan_BuildWorldToBodyMatrix(player);

    bool allRange = dbg.allRange;
    CueScanStats stats;
    EnemyCueTarget targets[kAccessibilityEnemyCueMaxVoices];
    dbg.requestedVoices = EnemyCue_VoiceCount();
    s32 count = EnemyCue_FindClosest(player, allRange, targets, dbg.requestedVoices, &stats);
    dbg.scanned = true;
    dbg.active = (count > 0);
    dbg.scanActive = stats.active;
    dbg.scanCueable = stats.cueable;
    dbg.scanKept = stats.kept;
    dbg.count = count;

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
        CueCommon_ComputeCueTarget(target->bodyDelta.x, target->bodyDelta.y, -target->bodyDelta.z, src, &freq);
        uint64_t key = EnemyCue_VoiceKey(target);
        sEnemyCue->TargetVoice(key, src[0], src[1], src[2], freq);

        EnemyCueTargetDebug& tdbg = dbg.targets[i];
        tdbg.slot = target->slot;
        tdbg.objId = (int32_t) target->actor->obj.id;
        tdbg.eventType = (int32_t) target->actor->eventType;
        tdbg.voiceKey = key;
        tdbg.distance = sqrtf(target->distSq);
        tdbg.bodyDelta[0] = target->bodyDelta.x;
        tdbg.bodyDelta[1] = target->bodyDelta.y;
        tdbg.bodyDelta[2] = target->bodyDelta.z;
        tdbg.src[0] = src[0];
        tdbg.src[1] = src[1];
        tdbg.src[2] = src[2];
        tdbg.freq = freq;

        // src/freq are the voice's post-clamp target — what was actually pushed this tick.
        // Voices this listener stops driving are reaped by CueRegistry_Tick.
        ENEMY_CUE_TRACE("[enemy-cue] voice {}/{} slot={} id={} level={} active={} cueable={} kept={} "
                        "bodyDelta=({:.1f},{:.1f},{:.1f}) src=({:.1f},{:.1f},{:.1f}) freq={:.3f}",
                        i + 1, count, target->slot, (int) target->actor->obj.id, (int) gCurrentLevel, stats.active,
                        stats.cueable, stats.kept, target->bodyDelta.x, target->bodyDelta.y, target->bodyDelta.z,
                        src[0], src[1], src[2], freq);
    }
}

void EnemyCue_Register() {
    CVarRegisterInteger(kEnemyCueVoicesCVar, kAccessibilityEnemyCueDefaultVoices);
    CVarRegisterInteger(kEnemyCueLogCVar, 0);

    sEnemyCue = CueRegistry_Register(kEnemyCueId, "Enemy locator",
                                     "Tracks the closest lockable enemies: ahead of your aim on rails, "
                                     "all around you in all-range mode.",
                                     { .wavPath = "assets/accessibility/enemy.wav",
                                       .maxVoices = kAccessibilityEnemyCueMaxVoices });

    REGISTER_LISTENER(GamePostUpdateEvent, EnemyCue_OnPostUpdate, EVENT_PRIORITY_NORMAL);
}
