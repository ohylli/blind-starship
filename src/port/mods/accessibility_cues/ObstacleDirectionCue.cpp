#include "ObstacleDirectionCue.h"
#include "CueCommon.h"
#include "CueScan.h"
#include "ObstacleScan.h"

#include <math.h>
#include <vector>

#include "port/CGameCompat.h"
#include "port/hooks/Events.h"
#include "port/accessibility/Cue.h"
#include "port/mods/Accessibility.h"

// The directional obstacle cues: sustained chords that say "the space beside / above /
// below you is closed" — there is something solid there that you are NOT going to hit if
// you fly straight, but would hit if you steered that way. They are the "which way is
// closed" half of the obstacle family; the ahead cue (ObstacleAheadCue.cpp) is the
// "how soon" half, and the two never claim the same box.
//
// The rule, asked of every box the shared scan yields (ObstacleScan.h), on rails, in the
// same order as the design record:
//   1. Is it alongside me now, or about to be? The near face within the lookahead ahead
//      of the ship ("upcoming"), or the ship already between the near and far faces
//      ("alongside"). Past the far face it is dropped. Unlike the ahead cue's "past the
//      near face -> silent" rule, a wall alongside still matters — drifting into it is
//      the whole risk.
//   2. Is it the ahead cue's? An UPCOMING box the ship's position is inside, widened by
//      the safety margin, on BOTH lateral axes is on course and the ahead cue is warning
//      about it. Skipped here — the ahead cue never says which edge is nearest, and this
//      cue never says "the wall ahead is more to your left"; the split keeps each sound's
//      meaning single. An alongside box is never the ahead cue's (it drops a box at its
//      near face), so it is classified below however close it is.
//   3. Which direction, and how close? The ship must be outside the box's footprint on
//      an axis (clearance > 0) and within the margin of it on the other: the outside
//      axis names the pair (X -> beside, Y -> above/below) and the sign of the center
//      offset picks the member; the clearance beyond the box face on that axis is the
//      signal, and it must be under that pair's band distance (side / vertical dist).
//      For an upcoming box that outside clearance is at least the margin (question 2
//      took the rest); an alongside box can be closer, and pins at the band's near end.
//      A box outside on both axes beyond the margin is a corner, nobody's in v1; one
//      within the margin on both (only possible alongside, hugging a corner) goes to the
//      nearer face. A box the ship is inside on both axes is the engine's, not ours.
// Per direction the box with the smallest clearance wins. The side cue has two voices,
// keyed left and right, so a corridor sounds both; above and below are separate cues.
//
// How each pair renders (the user's choices, docs/accessibility-obstacle-direction-cues.md):
//   - Beside: CUE3D_MODE_PAN at the unity-gain radius; the pan magnitude is the signal,
//     REVERSED from the naive mapping — a box at the band's edge is panned hard to its
//     side, one at the margin (or closer) sits near the center (floored, so left and
//     right never merge). "The closer the sound to center, the closer it is to you" is
//     the user's by-ear preference. Loudness is not a signal here, but it is not quite
//     constant either: the Cue layer's 1/sqrt(N) headroom trim (Cue::HeadroomTrim)
//     lowers both voices ~3 dB while the second wall sounds. Accepted for now; opting
//     the side cue out of the trim is the fix if the step reads as "closer" by ear.
//   - Above / below: CUE3D_MODE_DIRECT, centered, the loudness (CueTarget::level under
//     the slider) is the signal — full at the margin (or closer), the level floor at
//     the band's edge.
// Timbre: each is a two-sine chord a just perfect fourth apart (4:3), on equal-tempered
// roots in pitch order low/middle/high = below/beside/above — C3, G3 (a fifth up), C4
// (the octave) — so the three are told apart by register, and all sit under the aim
// click's 1500 Hz. The shared 300 Hz buzz is a damped pulse, a different shape entirely.
//
// v1 scope (docs/accessibility-obstacle-direction-cues.md): rails only — in all-range
// "left" is heading-relative and needs the offsets rotated into the aim frame, the ahead
// cue's own second step. Terrain is box-only: a heightfield bump's box spans the whole
// hill, so the below cue over-reports over Corneria's bumps until the surface-distance
// refinement lands. The engine's poly range gate is not applied: a mesh inside the side
// band is inside the gate anyway. Ground, water and lava are not objects and stay out.
static Cue* sSideCue = nullptr;
static Cue* sAboveCue = nullptr;
static Cue* sBelowCue = nullptr;

static ObstacleDirectionCueDebug sDebugState;

const ObstacleDirectionCueDebug& ObstacleDirectionCue_DebugState() {
    return sDebugState;
}

const char* ObstacleDirection_Name(ObstacleDirection dir) {
    switch (dir) {
        case OBSTACLE_DIR_LEFT:
            return "left";
        case OBSTACLE_DIR_RIGHT:
            return "right";
        case OBSTACLE_DIR_ABOVE:
            return "above";
        case OBSTACLE_DIR_BELOW:
            return "below";
        default:
            return "?";
    }
}

// Equal-tempered roots: C3, G3, C4 (A4 = 440). See the file comment for the ordering.
static constexpr float kBelowRootHz = 130.8128f;
static constexpr float kSideRootHz = 195.9977f;
static constexpr float kAboveRootHz = 261.6256f;

// Synthesized chord: two sines, the root and a just perfect fourth above it (4:3),
// looped seamlessly (interval 0). The loop is click-free because the buffer holds an
// exact whole number of cycles of BOTH partials: kRootCycles is a multiple of 3, so the
// fourth completes 4/3 as many, and the root is snapped (by a few cents at most) to
// whatever frequency makes that cycle count land on an integer sample count. Each sine
// at 0.45 keeps the summed peak under 0.9.
static std::vector<float> ObstacleDirectionCue_GenerateChord(int sampleRate, float rootHz) {
    constexpr int kRootCycles = 30;
    constexpr float kTwoPi = 6.2831853f;
    constexpr float kPartialAmplitude = 0.45f;
    int samples = (int) lroundf((float) kRootCycles * (float) sampleRate / rootHz);
    if (samples < 2) {
        samples = 2;
    }
    const float root = (float) kRootCycles * (float) sampleRate / (float) samples;
    const float fourth = root * (4.0f / 3.0f);
    std::vector<float> pcm((size_t) samples, 0.0f);
    for (int i = 0; i < samples; i++) {
        const float t = (float) i / (float) sampleRate;
        pcm[(size_t) i] = kPartialAmplitude * (sinf(kTwoPi * root * t) + sinf(kTwoPi * fourth * t));
    }
    return pcm;
}

static std::vector<float> ObstacleDirectionCue_GenerateSide(int sampleRate) {
    return ObstacleDirectionCue_GenerateChord(sampleRate, kSideRootHz);
}
static std::vector<float> ObstacleDirectionCue_GenerateAbove(int sampleRate) {
    return ObstacleDirectionCue_GenerateChord(sampleRate, kAboveRootHz);
}
static std::vector<float> ObstacleDirectionCue_GenerateBelow(int sampleRate) {
    return ObstacleDirectionCue_GenerateChord(sampleRate, kBelowRootHz);
}

// Clearance -> position in the band, 0 at the margin, 1 at the band's edge. The band
// is [margin, dist]; the caller guarantees dist > margin. Clamped both ends.
static f32 ObstacleDirectionCue_BandT(f32 clear, f32 margin, f32 dist) {
    f32 t = (clear - margin) / (dist - margin);
    if (!(t > 0.0f)) {
        return 0.0f; // at/inside the margin, or NaN
    }
    return (t < 1.0f) ? t : 1.0f;
}

// The pan and level floors are [0, 1) fraction knobs, capped just under 1 so the band
// always has somewhere to go.
static constexpr f32 kFloorMax = 0.95f;

// Per-direction winner bookkeeping for one tick.
struct ObstacleDirectionWinner {
    bool found = false;
    f32 clear = INFINITY;
    bool upcoming = false;
    ObstacleBox box{};
};

static void ObstacleDirectionCue_StopAll() {
    sSideCue->StopAllVoices();
    sAboveCue->Stop();
    sBelowCue->Stop();
}

static void ObstacleDirectionCue_FillTargetDebug(ObstacleDirectionTargetDebug& d,
                                                 const ObstacleDirectionWinner& w) {
    d.active = true;
    d.array = (int32_t) w.box.array;
    d.slot = w.box.slot;
    d.objId = w.box.objId;
    d.record = w.box.record;
    d.heightfield = w.box.polyHeightfield;
    d.upcoming = w.upcoming;
    d.clear = w.clear;
    d.gapZ = w.box.gapZ;
    d.dx = w.box.dx;
    d.dy = w.box.dy;
    d.dz = w.box.dz;
    d.halfX = w.box.half.x;
    d.halfY = w.box.half.y;
    d.halfZ = w.box.half.z;
}

static void ObstacleDirectionCue_OnPostUpdate(IEvent* event) {
    (void) event;

    // Loudness normalization, pushed before the gate like the other synthesized cues:
    // the settings-menu previews must honor the boost even when gameplay is gated off.
    const f32 boost = CVarGetFloat(kObstacleCueDirBoostCVar, kObstacleCueDirBoostDefault);
    sSideCue->SetGainBoost(boost);
    sAboveCue->SetGainBoost(boost);
    sBelowCue->SetGainBoost(boost);

    sDebugState = ObstacleDirectionCueDebug{};
    ObstacleDirectionCueDebug& dbg = sDebugState;
    dbg.enabled = CueCommon_IsEnabled();
    dbg.obstacleEnabled = ObstacleCommon_IsEnabled();
    // Same gate set as the ahead cue (see its comment on why `control` is what makes the
    // reads below safe), plus v1's rails-only scope: all-range needs the offsets rotated
    // into the aim frame, which is the planned second step.
    bool allRange = false;
    dbg.modeOk = CueScan_ModeInScope(&allRange);
    dbg.allRange = allRange;
    dbg.control = Accessibility_PlayerHasControl();
    dbg.frame = (int32_t) gGameFrameCount;
    if (!dbg.enabled || !dbg.obstacleEnabled || !dbg.modeOk || !dbg.control || allRange) {
        ObstacleDirectionCue_StopAll();
        return;
    }

    const f32 margin = ObstacleCommon_Margin();
    const f32 lookahead = CueCommon_ReadPositiveFloat(kObstacleCueDirLookaheadCVar, kObstacleCueDirLookaheadDefault,
                                                      kObstacleCueDirLookaheadMax);
    // The bands must reach past the margin or the mapping divides by zero; a band the
    // margin has swallowed (someone dragged the margin above it) is widened to one unit.
    f32 sideDist = CueCommon_ReadPositiveFloat(kObstacleCueDirSideDistCVar, kObstacleCueDirSideDistDefault,
                                               kObstacleCueDirSideDistMax);
    if (sideDist <= margin) {
        sideDist = margin + 1.0f;
    }
    f32 vertDist = CueCommon_ReadPositiveFloat(kObstacleCueDirVertDistCVar, kObstacleCueDirVertDistDefault,
                                               kObstacleCueDirVertDistMax);
    if (vertDist <= margin) {
        vertDist = margin + 1.0f;
    }
    const f32 panFloor =
        CueCommon_ReadFloat(kObstacleCueDirSidePanFloorCVar, kObstacleCueDirSidePanFloorDefault, 0.0f, kFloorMax);
    const f32 levelFloor =
        CueCommon_ReadFloat(kObstacleCueDirVertLevelFloorCVar, kObstacleCueDirVertLevelFloorDefault, 0.0f, kFloorMax);
    dbg.margin = margin;
    dbg.lookahead = lookahead;
    dbg.sideDist = sideDist;
    dbg.panFloor = panFloor;
    dbg.vertDist = vertDist;
    dbg.levelFloor = levelFloor;

    Player* player = &gPlayer[0];

    ObstacleScanStats stats;
    ObstacleDirectionWinner winners[OBSTACLE_DIR_COUNT];
    ObstacleScan_ForEachBox(player, &stats, [&](const ObstacleBox& box) {
        // Question 1: the Z window. gapZ is the near face (positive = still ahead); the
        // far face is 2 * half.z further back.
        const f32 farGap = box.gapZ + 2.0f * box.half.z;
        const bool upcoming = (box.gapZ > 0.0f) && (box.gapZ < lookahead);
        const bool alongside = !(box.gapZ > 0.0f) && (farGap > 0.0f);
        if (!upcoming && !alongside) {
            return;
        }
        dbg.inWindow++;
        // Question 2: the ahead cue's box — on course (within the margin on both lateral
        // axes) AND still upcoming. The ahead cue drops a box at its near face, so an
        // alongside box is ours however close (a NaN clearance fails every test below).
        const bool withinX = box.clearX < margin;
        const bool withinY = box.clearY < margin;
        if (withinX && withinY && upcoming) {
            dbg.aheadClaimed++;
            return;
        }
        // Question 3: outside the footprint on an axis, within the margin on the other.
        const bool sideOk = (box.clearX > 0.0f) && withinY && (box.clearX < sideDist);
        const bool vertOk = (box.clearY > 0.0f) && withinX && (box.clearY < vertDist);
        ObstacleDirection dir;
        f32 clear;
        if (sideOk && (!vertOk || (box.clearX <= box.clearY))) {
            dir = (box.dx < 0.0f) ? OBSTACLE_DIR_LEFT : OBSTACLE_DIR_RIGHT;
            clear = box.clearX;
        } else if (vertOk) {
            dir = (box.dy > 0.0f) ? OBSTACLE_DIR_ABOVE : OBSTACLE_DIR_BELOW;
            clear = box.clearY;
        } else {
            return; // a corner beyond the margin, or inside the footprint on both axes
        }
        ObstacleDirectionWinner& w = winners[dir];
        dbg.dir[dir].candidates++;
        if (clear < w.clear) {
            w.found = true;
            w.clear = clear;
            w.upcoming = upcoming;
            w.box = box;
        }
    });
    dbg.scanned = true;
    dbg.scanActive = stats.active;
    dbg.scanObstacles = stats.obstacles;
    dbg.scanBoxes = stats.boxes;

    // Every voice below sits on the unity-gain arc (CueCommon_PlaceOnPanArc), so the
    // backend's distance attenuation never becomes a second loudness signal.

    // Beside: one keyed voice per side, placed at a pan magnitude of exactly `pan`.
    // Refresh-or-stop: a side not targeted this tick is reaped by CueRegistry_Tick.
    static const ObstacleDirection kSides[2] = { OBSTACLE_DIR_LEFT, OBSTACLE_DIR_RIGHT };
    for (ObstacleDirection dir : kSides) {
        const ObstacleDirectionWinner& w = winners[dir];
        if (!w.found) {
            continue;
        }
        const f32 t = ObstacleDirectionCue_BandT(w.clear, margin, sideDist);
        const f32 pan = panFloor + (1.0f - panFloor) * t; // reversed: far = hard, near = floor
        const f32 sign = (dir == OBSTACLE_DIR_LEFT) ? -1.0f : 1.0f;
        CueTarget target;
        CueCommon_PlaceOnPanArc(target, sign * pan);
        sSideCue->TargetVoice((uint64_t) dir, target); // the direction is the sticky key
        ObstacleDirectionCue_FillTargetDebug(dbg.dir[dir], w);
        dbg.dir[dir].pan = pan;
    }

    // Above / below: single-voice cues, centered, loudness is the signal.
    static const ObstacleDirection kVerticals[2] = { OBSTACLE_DIR_ABOVE, OBSTACLE_DIR_BELOW };
    for (ObstacleDirection dir : kVerticals) {
        Cue* cue = (dir == OBSTACLE_DIR_ABOVE) ? sAboveCue : sBelowCue;
        const ObstacleDirectionWinner& w = winners[dir];
        if (!w.found) {
            cue->Stop();
            continue;
        }
        const f32 t = ObstacleDirectionCue_BandT(w.clear, margin, vertDist);
        const f32 level = 1.0f - (1.0f - levelFloor) * t; // full at the margin, floor at the edge
        CueTarget target;
        CueCommon_PlaceOnPanArc(target, 0.0f);
        target.level = level;
        cue->SetTarget(target);
        cue->Start();
        ObstacleDirectionCue_FillTargetDebug(dbg.dir[dir], w);
        dbg.dir[dir].level = level;
    }
}

void ObstacleDirectionCue_Register() {
    // The family toggle and the margin are registered by ObstacleCommon_RegisterCVars.
    CVarRegisterFloat(kObstacleCueDirLookaheadCVar, kObstacleCueDirLookaheadDefault);
    CVarRegisterFloat(kObstacleCueDirSideDistCVar, kObstacleCueDirSideDistDefault);
    CVarRegisterFloat(kObstacleCueDirSidePanFloorCVar, kObstacleCueDirSidePanFloorDefault);
    CVarRegisterFloat(kObstacleCueDirVertDistCVar, kObstacleCueDirVertDistDefault);
    CVarRegisterFloat(kObstacleCueDirVertLevelFloorCVar, kObstacleCueDirVertLevelFloorDefault);
    CVarRegisterFloat(kObstacleCueDirBoostCVar, kObstacleCueDirBoostDefault);

    // Pinned RESAMPLE pitch style on all three, like the buzz: none of them pitches, and
    // the spectral shifter would only add latency and CPU to a sustained sine pair.
    sSideCue = CueRegistry_Register(kObstacleSideCueId, "Obstacle beside",
                                    "A mid chord panned toward something solid beside you that you would hit "
                                    "by steering that way. The closer it is, the nearer the center it sounds.",
                                    { .generator = ObstacleDirectionCue_GenerateSide,
                                      .maxVoices = 2,
                                      .mode = CUE3D_MODE_PAN,
                                      .pitchStyle = CUE3D_SOURCE_PITCH_RESAMPLE });
    sAboveCue = CueRegistry_Register(kObstacleAboveCueId, "Obstacle above",
                                     "A high chord that sounds while something solid is above you that you would "
                                     "hit by climbing. Louder the closer it is.",
                                     { .generator = ObstacleDirectionCue_GenerateAbove,
                                       .mode = CUE3D_MODE_DIRECT,
                                       .pitchStyle = CUE3D_SOURCE_PITCH_RESAMPLE });
    sBelowCue = CueRegistry_Register(kObstacleBelowCueId, "Obstacle below",
                                     "A low chord that sounds while something solid is below you that you would "
                                     "hit by diving. Louder the closer it is.",
                                     { .generator = ObstacleDirectionCue_GenerateBelow,
                                       .mode = CUE3D_MODE_DIRECT,
                                       .pitchStyle = CUE3D_SOURCE_PITCH_RESAMPLE });

    REGISTER_LISTENER(GamePostUpdateEvent, ObstacleDirectionCue_OnPostUpdate, EVENT_PRIORITY_NORMAL);
}
