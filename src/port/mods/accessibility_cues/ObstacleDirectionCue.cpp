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
// The side chord alone is pulsed, at a FIXED rate (kSidePulseHz): a constant-power pan is
// only a level difference between the ears, and a steady low tone is the hardest sound to
// place from level alone (the ear wants onsets and timing), so the aim click read as a
// clearer "how far left" than the drone did. Each pulse is a fresh onset the ear localizes
// anew. The rate never varies — it is not a signal — so the buzz's varying pulse rate keeps
// its one meaning; above/below stay drones (centered, nothing to localize).
//
// v1 scope (docs/accessibility-obstacle-direction-cues.md): rails only — in all-range
// "left" is heading-relative and needs the offsets rotated into the aim frame, the ahead
// cue's own second step. Terrain (a heightfield poly box, whose box spans the whole hill)
// is refined for BELOW only: the box top is replaced by the engine's own surface height
// sampled along the course (ObstacleDirectionCue_TerrainBelow). The side pair still reads
// a terrain box as a box, and so over-reports a hill flown beside near its peak height.
// The engine's poly range gate is applied to the terrain walk and nowhere else: a solid
// mesh inside the side band is inside the gate anyway. Ground, water and lava are not
// objects and stay out.
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

// The side chord's pulse: rate, attack, and the floor the decay settles on (the chord
// never fully gates off, so the pan stays audible between onsets). The attack is a few
// ms — long enough not to click, short enough to be an onset.
static constexpr float kSidePulseHz = 5.0f;
static constexpr float kSidePulseAttackSec = 0.004f;
static constexpr float kSidePulseFloor = 0.25f;
// Makeup gain for the pulse, on the side cue only, multiplied into the shared chord boost:
// the envelope above averages to an RMS of ~0.45 of the drone's (about -7 dB), so the
// pulsed chord sat under the other cues by ear. 2.2 restores the drone's average level;
// retune it together with the floor / rate, which change that average.
static constexpr float kSidePulseMakeupGain = 2.2f;

// Synthesized chord: two sines, the root and a just perfect fourth above it (4:3),
// looped seamlessly (interval 0). The loop is click-free because the buffer holds an
// exact whole number of cycles of BOTH partials: rootCycles is a multiple of 3, so the
// fourth completes 4/3 as many, and the root is snapped (by a few cents at most) to
// whatever frequency makes that cycle count land on an integer sample count. Each sine
// at 0.45 keeps the summed peak under 0.9. A pulsed chord (pulseHz > 0) holds exactly
// one pulse per loop, so rootCycles is chosen as the multiple of 3 nearest rootHz /
// pulseHz and the pulse rate is snapped along with the root; the envelope starts at the
// floor, ramps to full over the attack, and decays back to (within 1% of) the floor by
// the end of the buffer, so the loop seam is at the quietest, flattest point.
static std::vector<float> ObstacleDirectionCue_GenerateChord(int sampleRate, float rootHz, float pulseHz) {
    constexpr int kDroneRootCycles = 30;
    constexpr float kTwoPi = 6.2831853f;
    constexpr float kPartialAmplitude = 0.45f;
    int rootCycles = kDroneRootCycles;
    if (pulseHz > 0.0f) {
        rootCycles = 3 * (int) lroundf(rootHz / pulseHz / 3.0f);
        if (rootCycles < 3) {
            rootCycles = 3;
        }
    }
    int samples = (int) lroundf((float) rootCycles * (float) sampleRate / rootHz);
    if (samples < 2) {
        samples = 2;
    }
    const float root = (float) rootCycles * (float) sampleRate / (float) samples;
    const float fourth = root * (4.0f / 3.0f);
    const float duration = (float) samples / (float) sampleRate;
    const float attack = (kSidePulseAttackSec < duration * 0.5f) ? kSidePulseAttackSec : duration * 0.5f;
    const float decayRate = logf(100.0f) / (duration - attack); // 1% of the swing left at the seam
    std::vector<float> pcm((size_t) samples, 0.0f);
    for (int i = 0; i < samples; i++) {
        const float t = (float) i / (float) sampleRate;
        float env = 1.0f;
        if (pulseHz > 0.0f) {
            if (t < attack) {
                env = kSidePulseFloor + (1.0f - kSidePulseFloor) * (t / attack);
            } else {
                env = kSidePulseFloor + (1.0f - kSidePulseFloor) * expf(-decayRate * (t - attack));
            }
        }
        pcm[(size_t) i] = env * kPartialAmplitude * (sinf(kTwoPi * root * t) + sinf(kTwoPi * fourth * t));
    }
    return pcm;
}

static std::vector<float> ObstacleDirectionCue_GenerateSide(int sampleRate) {
    return ObstacleDirectionCue_GenerateChord(sampleRate, kSideRootHz, kSidePulseHz);
}
static std::vector<float> ObstacleDirectionCue_GenerateAbove(int sampleRate) {
    return ObstacleDirectionCue_GenerateChord(sampleRate, kAboveRootHz, 0.0f);
}
static std::vector<float> ObstacleDirectionCue_GenerateBelow(int sampleRate) {
    return ObstacleDirectionCue_GenerateChord(sampleRate, kBelowRootHz, 0.0f);
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

// One axis's answer to question 3 for one box: is the box in that axis's band, which
// direction, and how close. The side and vertical halves of the rule each fill one and
// the closer wins the box — one shape for a solid box and a terrain box alike, so the
// walk-vs-box difference stays inside the vertical half.
struct ObstacleAxisAnswer {
    bool ok = false;
    f32 clear = INFINITY; // the clearance the band maps; INFINITY while !ok
    ObstacleDirection dir = OBSTACLE_DIR_BELOW;
    bool upcoming = false;        // the box's near face still ahead; for a terrain walk
                                  // answer, the winning sample ahead (vs beneath the ship)
    bool fromTerrainWalk = false; // clear is a surface clearance from the terrain walk
                                  // (ObstacleDirectionCue_TerrainBelow); surfaceY / sampleT
                                  // are valid
    f32 surfaceY = 0.0f;
    f32 sampleT = 0.0f;
};

// Per-direction winner bookkeeping for one tick.
struct ObstacleDirectionWinner {
    bool found = false;
    ObstacleAxisAnswer answer; // answer.clear is INFINITY until a box wins
    ObstacleBox box{};
};

// The terrain walk samples on the family's shared grid (ObstacleCommon.h: kObstacleWalkStep
// / kObstacleWalkMaxSteps, the same grid as the ahead cue's walk, which is what makes the
// per-sample claim below the ahead cue's own hit); the cap must still reach the far end of
// this cue's longest lookahead.
static_assert((f32) kObstacleWalkMaxSteps * kObstacleWalkStep >= kObstacleCueDirLookaheadMax,
              "the terrain walk must reach the far end of the longest lookahead");

struct ObstacleTerrainBelow {
    bool found = false;   // a sample qualified; the fields below describe the closest one
    f32 clear = INFINITY; // ship Y minus the surface height there
    f32 surfaceY = 0.0f;
    f32 t = 0.0f;         // its course distance; 0 = beneath the ship
    bool claimed = false; // at least one upcoming sample was left to the ahead cue
};

// "Below" for a heightfield poly box (box.polyHeightfield — the terrain bumps, reefs,
// the island, the mountains). The engine hits such a mesh only when the ship is at or
// below the surface under it, so the box top — the hill's PEAK — says nothing about the
// ground under a ship over the outer slope, and the box rule droned over most of
// Corneria. This asks the engine for the surface height instead
// (Object_PolyHeightfieldSurfaceY, one read per point, the number its crash test
// compares against) along the stretch of course the box rule's Z window covers: from
// beneath the ship — or the box's near face, if that is still ahead — to the far face or
// the lookahead, whichever is nearer, in kObstacleWalkStep steps. Each step reads the surface
// under the course and `margin` to either side (the box rule's "within the margin on
// the other axis", and the same three points the ahead cue's walk probes) and keeps the
// highest; the clearance is the ship's Y above it.
//
// Ownership is decided per SAMPLE, not per box (one hill is both "ground 300 below me"
// and "a slope rising into my course"): an upcoming sample whose surface is within the
// margin of the course is the ahead cue's — the same threshold its walk hits at — and is
// skipped, while the rest still compete, so a rising slope sounds as the buzz plus a
// loud below chord: "the thing ahead is ground, climb". The sample beneath the ship is
// the "alongside" case and pins at the band's near end however close. (The ahead cue's
// walk starts beneath the ship too, so skimming within the margin sounds both — a known,
// accepted overlap: the chord is what says the buzz is about the ground.) A sample at or
// below the surface is the engine's.
//
// A sample is dropped when the ship there would be outside the engine's XZ range gate
// (box.polyRangeXZ): Player_CollisionCheck never tests the mesh from out there, and a
// bump's box is deep enough for its far end to lie beyond the gate.
static ObstacleTerrainBelow ObstacleDirectionCue_TerrainBelow(const ObstacleBox& box, const Player* player,
                                                              bool alongside, f32 lookahead, f32 margin,
                                                              int32_t* probes) {
    ObstacleTerrainBelow out;
    PolyHeightfield hf;
    if (!Object_ResolvePolyHeightfield(box.polyColId, &box.objPos, box.rotY, &hf)) {
        return out; // not a tabled mesh — unreachable for a scan-produced box
    }
    const f32 tStart = alongside ? 0.0f : box.gapZ;
    f32 tEnd = box.gapZ + 2.0f * box.half.z;
    if (tEnd > lookahead) {
        tEnd = lookahead;
    }
    int steps;
    if (!ObstacleCommon_WalkSteps(tEnd - tStart, &steps)) {
        return out; // NaN or a reversed span
    }
    const s32 lateral = (margin > 0.0f) ? 1 : 0;
    const f32 rangeSq = box.polyRangeXZ * box.polyRangeXZ;
    for (int i = 0; i <= steps; i++) {
        f32 t = tStart + (f32) i * kObstacleWalkStep;
        if (t > tEnd) {
            t = tEnd;
        }
        const f32 z = player->trueZpos - t; // the rails course is -z
        if (box.polyRangeXZ > 0.0f) {
            const f32 rx = player->pos.x - box.objPos.x;
            const f32 rz = z - box.objPos.z;
            if ((rx * rx + rz * rz) > rangeSq) {
                continue;
            }
        }
        bool any = false;
        f32 top = 0.0f;
        for (s32 k = -lateral; k <= lateral; k++) {
            const f32 x = player->pos.x + (f32) k * margin;
            f32 surfaceY;
            (*probes)++;
            if (Object_PolyHeightfieldSurfaceY(&hf, x, z, &surfaceY) && (!any || (surfaceY > top))) {
                any = true;
                top = surfaceY;
            }
        }
        if (!any) {
            continue; // no surface under this stretch (outside the mesh's outline)
        }
        const f32 clear = player->pos.y - top;
        const bool beneath = alongside && (i == 0);
        if (!beneath && (clear < margin)) {
            out.claimed = true; // the ahead cue's walk hits here
            continue;
        }
        if (!(clear > 0.0f)) {
            continue; // at or below the surface: the engine's
        }
        if (clear < out.clear) {
            out.found = true;
            out.clear = clear;
            out.surfaceY = top;
            out.t = t;
        }
    }
    return out;
}

static void ObstacleDirectionCue_StopAll() {
    sSideCue->StopAllVoices();
    sAboveCue->Stop();
    sBelowCue->Stop();
}

static void ObstacleDirectionCue_FillTargetDebug(ObstacleDirectionTargetDebug& d, const ObstacleDirectionWinner& w) {
    d.active = true;
    d.array = (int32_t) w.box.array;
    d.slot = w.box.slot;
    d.objId = w.box.objId;
    d.record = w.box.record;
    d.heightfield = w.box.polyHeightfield;
    d.fromTerrainWalk = w.answer.fromTerrainWalk;
    d.surfaceY = w.answer.surfaceY;
    d.sampleT = w.answer.sampleT;
    d.upcoming = w.answer.upcoming;
    d.clear = w.answer.clear;
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
    sSideCue->SetGainBoost(boost * kSidePulseMakeupGain); // the pulse's average-level makeup
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
    // A heightfield is the floor, not a crash, for the ground vehicles (ObstacleCommon.h),
    // so the terrain walk is skipped for them — a forward guard, no rails level pairs the
    // two today.
    const bool terrainIsFloor = ObstacleCommon_TerrainIsFloor();

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
        // A terrain box is never claimed whole: its claim is settled per sample inside
        // the walk (ObstacleDirectionCue_TerrainBelow), so it goes on to question 3 with
        // boxOnCourse still set — the side half honors it, the vertical half decides.
        const bool withinX = box.clearX < margin;
        const bool withinY = box.clearY < margin;
        const bool boxOnCourse = withinX && withinY && upcoming;
        if (boxOnCourse && !box.polyHeightfield) {
            dbg.aheadClaimed++;
            return;
        }
        // Question 3, vertical half: outside the footprint in Y, within the margin in X.
        // For a terrain box "outside in Y" is the ship's height above the surface under
        // the course, not the box top, and a heightfield is only ever hit from above, so
        // its answer is always BELOW.
        ObstacleAxisAnswer vert;
        if (box.polyHeightfield) {
            if (withinX && !terrainIsFloor) {
                dbg.terrainWalkTested++;
                const ObstacleTerrainBelow terrain = ObstacleDirectionCue_TerrainBelow(
                    box, player, alongside, lookahead, margin, &dbg.terrainWalkProbes);
                if (terrain.claimed) {
                    dbg.terrainWalkClaimed++;
                }
                if (terrain.found && (terrain.clear < vertDist)) {
                    vert.ok = true;
                    vert.clear = terrain.clear;
                    vert.dir = OBSTACLE_DIR_BELOW;
                    vert.upcoming = terrain.t > 0.0f;
                    vert.fromTerrainWalk = true;
                    vert.surfaceY = terrain.surfaceY;
                    vert.sampleT = terrain.t;
                }
            }
        } else if ((box.clearY > 0.0f) && withinX && (box.clearY < vertDist)) {
            vert.ok = true;
            vert.clear = box.clearY;
            vert.dir = (box.dy > 0.0f) ? OBSTACLE_DIR_ABOVE : OBSTACLE_DIR_BELOW;
            vert.upcoming = upcoming;
        }
        // Question 3, side half: outside the footprint in X, within the margin in Y. The
        // !boxOnCourse only bites for a terrain box (a solid one returned above): its box
        // claim still holds here, so a hill on course never sounds beside.
        ObstacleAxisAnswer side;
        if ((box.clearX > 0.0f) && withinY && (box.clearX < sideDist) && !boxOnCourse) {
            side.ok = true;
            side.clear = box.clearX;
            side.dir = (box.dx < 0.0f) ? OBSTACLE_DIR_LEFT : OBSTACLE_DIR_RIGHT;
            side.upcoming = upcoming;
        }
        // The nearer face wins the box. A corner beyond the margin on both axes, or a box
        // the ship is inside on both, answers neither.
        const ObstacleAxisAnswer* pick;
        if (side.ok && (!vert.ok || (side.clear <= vert.clear))) {
            pick = &side;
        } else if (vert.ok) {
            pick = &vert;
        } else {
            return;
        }
        ObstacleDirectionWinner& w = winners[pick->dir];
        dbg.dir[pick->dir].candidates++;
        if (pick->clear < w.answer.clear) {
            w.found = true;
            w.answer = *pick;
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
        const f32 t = ObstacleDirectionCue_BandT(w.answer.clear, margin, sideDist);
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
        const f32 t = ObstacleDirectionCue_BandT(w.answer.clear, margin, vertDist);
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
                                    "A pulsing mid chord panned toward something solid beside you that you would hit "
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
