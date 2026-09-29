#include "ObstacleAheadCue.h"
#include "CueCommon.h"
#include "CueScan.h"
#include "ObstacleCourse.h"
#include "ObstacleScan.h"

#include <math.h>
#include <vector>

#include "port/CGameCompat.h"
#include "port/hooks/Events.h"
#include "port/accessibility/Cue.h"
#include "port/mods/Accessibility.h"

// The obstacle-ahead cue: a low buzz that pulses faster as the player closes on
// something solid they will hit if they keep flying as they are — a collidable,
// non-lockable object on their course. What "on course" means is per mode: on rails the
// ship always travels down -z, so the test is the player's (x, y) inside the hitbox
// footprint; in all-range the course is a ray cast along the aim heading through the
// same boxes. That verdict lives in ObstacleCourse.h, shared with the directional cues so
// they skip exactly the boxes this cue claims: ObstacleCourse_AheadClaimsSolid for a solid
// box, and for a heightfield mesh's box (the terrain bumps and their kin, ObstacleScan.h)
// a refinement — the course is walked through the box's yawed footprint
// (ObstacleCourse_PlanAheadWalk) and the engine's own surface test decides whether it
// actually meets the slope (ObstacleCourse_AheadHitsTerrain). Rendered CUE3D_MODE_DIRECT (dead center, no
// spatialization, no distance attenuation): a warning is not a navigation target — it
// must not occupy the spatial channel the ring/enemy cues use, and "how soon" is the
// only actionable dimension for something you steer away from. The pulse interval is
// deliberately the ONLY signal: loudness and pitch stay constant so "faster" is
// unambiguously "closer". The directional siblings (ObstacleDirectionCue.cpp: beside,
// above, below) carry the "which way is closed" half over the same scan. Design record:
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
// it across the whole warning distance. warnDist is sanitized by the caller.
static f32 ObstacleAheadCue_Interval(f32 gap, f32 warnDist) {
    const f32 slow = CueCommon_ReadPositiveFloat(kObstacleCueSlowCVar, kObstacleCueSlowDefault);
    const f32 fast = CueCommon_ReadPositiveFloat(kObstacleCueFastCVar, kObstacleCueFastDefault);
    f32 t = gap / warnDist;
    if (!(t > 0.0f)) {
        t = 0.0f; // contact or NaN
    }
    if (t > 1.0f) {
        t = 1.0f;
    }
    return fast * powf(slow / fast, t);
}

// This cue's heightfield walk (ObstacleCourse_AheadHitsTerrain) samples on the family's
// shared grid (ObstacleCommon.h: kObstacleWalkStep / kObstacleWalkMaxSteps, the grid the
// directional cues' terrain walks share). The walk never spans more than the warn
// distance, and warnDist is clamped to kObstacleCueWarnDistMax, so no real span reaches
// the cap — the assert keeps it so, since
// a walk cut short would leave a slope past the cap unwarned.
static_assert((f32) kObstacleWalkMaxSteps * kObstacleWalkStep >= kObstacleCueWarnDistMax,
              "the heightfield walk must reach the far end of the widest warn band");

f32 ObstacleAheadCue_WarnDist() {
    // Capped at the slider's ceiling: the heightfield walks' step cap is sized to it (see
    // the assert above).
    return CueCommon_ReadPositiveFloat(kObstacleCueWarnDistCVar, kObstacleCueWarnDistDefault, kObstacleCueWarnDistMax);
}

static void ObstacleAheadCue_OnPostUpdate(IEvent* event) {
    (void) event;

    // Loudness normalization, pushed before the gate like the aim cue's: the
    // settings-menu preview must honor the boost even when gameplay is gated off.
    sObstacleCue->SetGainBoost(CVarGetFloat(kObstacleCueBoostCVar, kObstacleCueBoostDefault));

    sDebugState = ObstacleAheadCueDebug{};
    ObstacleAheadCueDebug& dbg = sDebugState;
    dbg.enabled = CueCommon_IsEnabled();
    dbg.obstacleEnabled = ObstacleCommon_IsEnabled();
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

    const f32 warnDist = ObstacleAheadCue_WarnDist();
    const f32 margin = ObstacleCommon_Margin();
    dbg.warnDist = warnDist;
    dbg.margin = margin;
    // A heightfield is the floor, not a crash, for the ground vehicles (ObstacleCommon.h).
    const bool terrainIsFloor = ObstacleCommon_TerrainIsFloor();

    Player* player = &gPlayer[0];

    // The course as a ray from the ship's center: the fixed -z track on rails, the aim
    // heading in all-range (ObstacleCourse_Frame, which stops the cue when the craft's
    // heading does not compose — gates.aimValid is the `cues` dump's tell).
    ObstacleCourseFrame frame;
    if (!ObstacleCourse_Frame(*player, allRange, &frame)) {
        dbg.aimValid = false;
        sObstacleCue->Stop();
        return;
    }
    if (allRange) {
        dbg.fwdX = frame.fwd.x;
        dbg.fwdY = frame.fwd.y;
        dbg.fwdZ = frame.fwd.z;
    }

    ObstacleScanStats stats;
    ObstacleBox best{};
    f32 bestGap = INFINITY;
    ObstacleScan_ForEachBox(player, &stats, [&](const ObstacleBox& scanned) {
        ObstacleBox box = scanned;
        f32 gap;
        if (scanned.polyHeightfield) {
            // A heightfield box is a candidate, not a verdict: the course is walked
            // through it and the engine's surface test decides
            // (ObstacleCourse_AheadHitsTerrain, shared with the directional cues). It is
            // reasoned about — and reported — on its yawed footprint, as the directional
            // cues' walks are.
            if (terrainIsFloor) {
                return;
            }
            box = ObstacleScan_YawedFootprint(scanned, frame.origin);
            const ObstacleAheadWalk plan = ObstacleCourse_PlanAheadWalk(box, frame, allRange, margin, warnDist);
            if (!plan.walks) {
                return;
            }
            dbg.heightfieldTested++;
            if (!ObstacleCourse_AheadHitsTerrain(box, frame, plan, margin, &gap, &dbg.heightfieldProbes)) {
                dbg.heightfieldCleared++;
                return;
            }
        } else if (!ObstacleCourse_AheadClaimsSolid(box, frame, allRange, margin, warnDist, &gap)) {
            return; // not on course, past the near face, or beyond the band
        }
        dbg.onCourse++;
        if (gap < bestGap) {
            bestGap = gap;
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

    // DIRECT mode ignores the position for rendering; straight ahead on the unity-gain
    // arc keeps the `cues` voice dump readable. Pitch stays 1.0: the interval is the
    // whole signal.
    CueTarget target;
    CueCommon_PlaceOnPanArc(target, 0.0f);
    target.intervalSec = ObstacleAheadCue_Interval(bestGap, warnDist);
    sObstacleCue->SetTarget(target);
    sObstacleCue->Start();

    dbg.active = true;
    dbg.intervalSec = target.intervalSec;
    dbg.target.array = (int32_t) best.array;
    dbg.target.slot = best.slot;
    dbg.target.objId = best.objId;
    dbg.target.record = best.record;
    dbg.target.heightfield = best.polyHeightfield;
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
    // The family toggle and the margin are registered by ObstacleCommon_RegisterCVars.
    CVarRegisterFloat(kObstacleCueWarnDistCVar, kObstacleCueWarnDistDefault);
    CVarRegisterFloat(kObstacleCueSlowCVar, kObstacleCueSlowDefault);
    CVarRegisterFloat(kObstacleCueFastCVar, kObstacleCueFastDefault);
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
