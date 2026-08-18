#include "ObstacleAheadCue.h"
#include "CueCommon.h"
#include "CueScan.h"
#include "ObstacleScan.h"

#include <math.h>
#include <vector>

#include "port/CGameCompat.h"
#include "port/PlayerAim.h"
#include "port/hooks/Events.h"
#include "port/accessibility/Cue.h"
#include "port/mods/Accessibility.h"

// The obstacle-ahead cue: a low buzz that pulses faster as the player closes on
// something solid they will hit if they keep flying as they are — a collidable,
// non-lockable object on their course. What "on course" means is per mode: on rails the
// ship always travels down -z, so the test is the player's (x, y) inside the hitbox
// footprint; in all-range the course is a ray cast along the aim heading through the
// same boxes (the slab test below). Rendered CUE3D_MODE_DIRECT (dead center, no
// spatialization, no distance attenuation): a warning is not a navigation target — it
// must not occupy the spatial channel the ring/enemy cues use, and "how soon" is the
// only actionable dimension for something you steer away from. The pulse interval is
// deliberately the ONLY signal: loudness and pitch stay constant so "faster" is
// unambiguously "closer". The planned directional siblings (obstacle left/right,
// above/below) will carry the "which way to dodge" half. Design record:
// docs/accessibility-obstacle-cue.md.
static Cue* sObstacleCue = nullptr;

static ObstacleAheadCueDebug sDebugState;

const ObstacleAheadCueDebug& ObstacleAheadCue_DebugState() {
    return sDebugState;
}

// Synthesized low buzz: ~8 ms of silence, then a ~30 ms damped 300 Hz tone with a touch
// of second harmonic, via the shared generator (CueCommon_GenerateDampedTone, which owns
// the lead-in and tail rationale). 300 Hz against the aim cue's 1500 Hz keeps the two
// unmistakable even when both pulse fast; the harmonic keeps the buzz audible on small
// speakers. This cue never modulates pitch, so the buffer length is exact — the assert
// pins it inside the fast-interval slider's floor with no RESAMPLE stretch to budget.
static std::vector<float> ObstacleAheadCue_GenerateBuzz(int sampleRate) {
    constexpr f32 kLeadInSec = 0.008f;
    constexpr f32 kToneSec = 0.03f;
    constexpr f32 kToneHz = 300.0f;
    constexpr f32 kHarmonicMix = 0.35f;
    constexpr f32 kDecayPerSec = 45.0f;
    constexpr f32 kAmplitude = 0.9f;
    constexpr f32 kTailSec = 0.004f;
    static_assert(kLeadInSec + kToneSec <= kObstacleCueMinIntervalSec,
                  "the buzz must finish inside the fastest pulse interval or every pulse truncates at the restart");
    return CueCommon_GenerateDampedTone(sampleRate, kLeadInSec, kToneSec, kToneHz, kHarmonicMix, kDecayPerSec,
                                        kAmplitude, kTailSec);
}

// Near-face gap -> pulse interval, geometric: slow at/beyond the warning distance, fast
// at contact (gap 0), interpolated in the log domain so the pulse *rate* climbs by
// equal-sounding tempo steps per unit of distance closed. A linear-in-interval ramp
// packed nearly all audible urgency into the last half second of the approach (rate is
// 1/interval, so it crawled early and exploded at contact); the geometric ramp spreads
// it across the whole warning distance. NaN-guarded in the usual !(x > lo) style (see
// CueCommon_ComputeFreqModFromY for the rationale); warnDist is sanitized by the caller.
static f32 ObstacleAheadCue_Interval(f32 gap, f32 warnDist) {
    f32 slow = CVarGetFloat(kObstacleCueSlowCVar, kObstacleCueSlowDefault);
    f32 fast = CVarGetFloat(kObstacleCueFastCVar, kObstacleCueFastDefault);
    if (!(slow > 0.0f)) {
        slow = kObstacleCueSlowDefault;
    }
    if (!(fast > 0.0f)) {
        fast = kObstacleCueFastDefault;
    }
    f32 t = gap / warnDist;
    if (!(t > 0.0f)) {
        t = 0.0f; // contact or NaN
    }
    if (t > 1.0f) {
        t = 1.0f;
    }
    return fast * powf(slow / fast, t);
}

// All-range course test: distance along the unit `fwd` ray from the player to the box
// expanded by `margin`, or a negative value when the ray misses or the entry point is
// behind/inside. Standard ray-vs-AABB slab test, with an explicit near-parallel branch
// instead of the branchless min/max form — IEEE 0 * inf would seed NaNs there, and this
// file's policy is explicit guards over NaN-propagating arithmetic. The margin expands
// every axis uniformly: in all-range "lateral" is not axis-aligned, and the extra
// ~150 units on the ray axis against a 4000-unit warn band is noise.
static f32 ObstacleAheadCue_RayGap(const ObstacleBox& box, const Vec3f& fwd, f32 margin) {
    const f32 d[3] = { box.dx, box.dy, box.dz };
    const f32 h[3] = { box.half.x + margin, box.half.y + margin, box.half.z + margin };
    const f32 f[3] = { fwd.x, fwd.y, fwd.z };
    f32 tNear = -INFINITY; // fwd is unit-length, so at least one axis divides and both
    f32 tFar = INFINITY;   // bounds end up finite
    // fwd is unit-length, so this is a direction-cosine floor, not a distance.
    constexpr f32 kRayParallelEps = 1e-6f;
    for (int axis = 0; axis < 3; axis++) {
        if (fabsf(f[axis]) < kRayParallelEps) {
            if (fabsf(d[axis]) > h[axis]) {
                return -1.0f; // parallel to this slab pair and outside it
            }
            continue;
        }
        f32 t1 = (d[axis] - h[axis]) / f[axis];
        f32 t2 = (d[axis] + h[axis]) / f[axis];
        if (t1 > t2) {
            f32 tmp = t1;
            t1 = t2;
            t2 = tmp;
        }
        if (t1 > tNear) {
            tNear = t1;
        }
        if (t2 < tFar) {
            tFar = t2;
        }
    }
    if (tNear > tFar) {
        return -1.0f; // the ray misses the box
    }
    return tNear;
}

static void ObstacleAheadCue_OnPostUpdate(IEvent* event) {
    (void) event;

    // Loudness normalization, pushed before the gate like the aim cue's: the
    // settings-menu preview must honor the boost even when gameplay is gated off.
    sObstacleCue->SetGainBoost(CVarGetFloat(kObstacleCueBoostCVar, kObstacleCueBoostDefault));

    sDebugState = ObstacleAheadCueDebug{};
    ObstacleAheadCueDebug& dbg = sDebugState;
    dbg.enabled = CueCommon_IsEnabled();
    dbg.obstacleEnabled = (CVarGetInteger(kObstacleCueEnabledCVar, 1) == 1);
    // gLevelMode is 0 (== LEVELMODE_ON_RAILS) before any level loads, so modeOk alone
    // does not filter the pre-game ticks; control (which also null-checks gPlayer) is
    // what makes the reads below safe. CueScan_ModeInScope is the sibling cues' scoping:
    // on-rails or solo all-range, with Versus (the untested multiplayer mode) excluded
    // there rather than here.
    bool allRange = false;
    dbg.modeOk = CueScan_ModeInScope(&allRange);
    dbg.allRange = allRange;
    dbg.control = Accessibility_PlayerHasControl();
    dbg.frame = (int32_t) gGameFrameCount;
    if (!dbg.enabled || !dbg.obstacleEnabled || !dbg.modeOk || !dbg.control) {
        sObstacleCue->Stop();
        return;
    }

    f32 warnDist = CVarGetFloat(kObstacleCueWarnDistCVar, kObstacleCueWarnDistDefault);
    if (!(warnDist > 0.0f)) {
        warnDist = kObstacleCueWarnDistDefault; // zero, negative, or NaN
    }
    f32 margin = CVarGetFloat(kObstacleCueMarginCVar, kObstacleCueMarginDefault);
    if (!(margin >= 0.0f)) {
        margin = kObstacleCueMarginDefault; // negative or NaN
    }
    dbg.warnDist = warnDist;
    dbg.margin = margin;

    Player* player = &gPlayer[0];

    // All-range flies by aim heading, so its course is a ray along Player_AimForward
    // (PlayerAim.h owns the composition). The forms whose heading composes differently
    // (Landmaster, on-foot — see PlayerAim.h) never appear in solo all-range, Versus
    // being out of scope above; stop rather than guess if one ever does, with
    // gates.aimValid as the `cues` dump's tell.
    Vec3f fwd = { 0.0f, 0.0f, 0.0f };
    if (allRange) {
        if (!Player_AimAnglesValid(*player)) {
            dbg.aimValid = false;
            sObstacleCue->Stop();
            return;
        }
        fwd = Player_AimForward(*player);
        dbg.fwdX = fwd.x;
        dbg.fwdY = fwd.y;
        dbg.fwdZ = fwd.z;
    }

    ObstacleScanStats stats;
    ObstacleBox best{};
    f32 bestGap = INFINITY;
    ObstacleScan_ForEachBox(player, &stats, [&](const ObstacleBox& box) {
        if (allRange) {
            // On course: the heading ray enters the margin-expanded box inside the warn
            // band. gap <= 0 (entry behind, or the player already inside the expanded
            // box) drops it for the same reason as the rails branch's gapZ <= 0 — past
            // the near face the engine's own collision has already resolved the
            // encounter and a warning is noise.
            f32 gap = ObstacleAheadCue_RayGap(box, fwd, margin);
            if (!(gap > 0.0f) || (gap >= warnDist)) {
                return;
            }
            dbg.onCourse++;
            if (gap < bestGap) {
                bestGap = gap;
                best = box;
            }
            return;
        }
        // On course: still ahead, inside the warn band, and the player's current (x, y)
        // inside the margin-expanded footprint. gapZ <= 0 drops the box the moment the
        // player is level with its near face — past that the engine's own collision has
        // already resolved the encounter and a warning is noise.
        if (!(box.gapZ > 0.0f) || (box.gapZ >= warnDist)) {
            return;
        }
        if ((box.clearX >= margin) || (box.clearY >= margin)) {
            return;
        }
        dbg.onCourse++;
        if (box.gapZ < bestGap) {
            bestGap = box.gapZ;
            best = box;
        }
    });
    dbg.scanned = true;
    dbg.scanActive = stats.active;
    dbg.scanObstacles = stats.obstacles;
    dbg.scanBoxes = stats.boxes;

    if (dbg.onCourse == 0) {
        sObstacleCue->Stop();
        return;
    }

    // DIRECT mode ignores the position for rendering; a sane straight-ahead one at the
    // unity-gain radius keeps the `cues` voice dump readable. Pitch stays 1.0: the
    // interval is the whole signal.
    f32 radius = Cue3D_GetUnityGainDistance();
    if (!(radius > 0.0f)) {
        radius = 100.0f; // backend not up yet; DIRECT renders the same regardless
    }
    CueTarget target;
    target.x = 0.0f;
    target.y = 0.0f;
    target.z = radius;
    target.intervalSec = ObstacleAheadCue_Interval(bestGap, warnDist);
    sObstacleCue->SetTarget(target);
    sObstacleCue->Start();

    dbg.active = true;
    dbg.intervalSec = target.intervalSec;
    dbg.target.array = (int32_t) best.array;
    dbg.target.slot = best.slot;
    dbg.target.objId = best.objId;
    dbg.target.record = best.record;
    dbg.target.gap = bestGap;
    dbg.target.gapZ = best.gapZ;
    dbg.target.clearX = best.clearX;
    dbg.target.clearY = best.clearY;
    dbg.target.dx = best.dx;
    dbg.target.dy = best.dy;
    dbg.target.dz = best.dz;
    dbg.target.halfX = best.half.x;
    dbg.target.halfY = best.half.y;
    dbg.target.halfZ = best.half.z;
}

void ObstacleAheadCue_Register() {
    CVarRegisterInteger(kObstacleCueEnabledCVar, 1);
    CVarRegisterFloat(kObstacleCueWarnDistCVar, kObstacleCueWarnDistDefault);
    CVarRegisterFloat(kObstacleCueSlowCVar, kObstacleCueSlowDefault);
    CVarRegisterFloat(kObstacleCueFastCVar, kObstacleCueFastDefault);
    CVarRegisterFloat(kObstacleCueMarginCVar, kObstacleCueMarginDefault);
    CVarRegisterFloat(kObstacleCueBoostCVar, kObstacleCueBoostDefault);

    // DIRECT render mode: see the file comment. Pinned RESAMPLE pitch style even though
    // this cue never pitches — the spectral shifter would only add ~0.1 s of content
    // latency and CPU to a source whose timing IS the signal.
    sObstacleCue = CueRegistry_Register(kObstacleAheadCueId, "Obstacle warning",
                                        "A low buzz that beats faster as you close on something solid on "
                                        "your course that you cannot shoot down.",
                                        { .generator = ObstacleAheadCue_GenerateBuzz,
                                          .mode = CUE3D_MODE_DIRECT,
                                          .pitchStyle = CUE3D_SOURCE_PITCH_RESAMPLE });

    REGISTER_LISTENER(GamePostUpdateEvent, ObstacleAheadCue_OnPostUpdate, EVENT_PRIORITY_NORMAL);
}
