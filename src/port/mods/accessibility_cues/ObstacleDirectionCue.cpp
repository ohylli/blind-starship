#include "ObstacleDirectionCue.h"
#include "CueCommon.h"
#include "CueScan.h"
#include "ObstacleAheadCue.h" // ObstacleAheadCue_WarnDist: the band the ahead cue's verdict is asked with
#include "ObstacleCourse.h"
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
// The rule, asked of every box the shared scan yields (ObstacleScan.h), in the same order
// as the design record. It is stated in the HEADING FRAME (ObstacleCourseFrame,
// ObstacleCourse.h): distance along the course, and offsets to the right and up off it.
// On rails that frame is the world's (the -z track, +x, +y); in all-range it is the aim
// heading's, bank ignored.
//   1. Is it alongside me now, or about to be? The box's nearest point along the course
//      within the lookahead ("upcoming"), or the ship already level with part of it
//      ("alongside"). Wholly behind, it is dropped. Unlike the ahead cue's "past the
//      near face -> silent" rule, a wall alongside still matters — drifting into it is
//      the whole risk.
//   2. Is it the ahead cue's? A box the ahead cue is warning about this tick — decided by
//      the ahead cue's own verdict (ObstacleCourse_AheadClaimsSolid: the course runs into
//      the box widened by the safety margin, on the stretch the engine's range gate lets
//      it be hit, with the entry still ahead and inside the warn band) — is skipped here.
//      The ahead cue never says which edge is nearest, and this cue never says "the wall
//      ahead is more to your left"; the split keeps each sound's meaning single. Once the
//      ship is level with the entry the ahead cue drops the box, and beyond the band it
//      has not started, so either way it is classified below however close it is.
//      Terrain is never claimed whole (see Scope below): the below walk leaves the ahead
//      cue its samples one by one, and the beside walk goes silent for the whole hill
//      while the ahead cue's terrain verdict (ObstacleCourse_AheadHitsTerrain) is buzzing
//      for it.
//   3. Which direction, and how close? Take the part of the box inside the window and
//      within the margin of the course on one steering axis; if all of it lies off to
//      one side on the other axis, that axis names the pair (right -> beside, up ->
//      above/below), the side names the member, and the distance to its nearest point is
//      the clearance — the signal, which must be under that pair's band distance (side /
//      vertical dist). On rails that is the box face's clearance on that axis; in a
//      turned heading it comes from the box's corners and edges
//      (ObstacleDirectionCue_SliceRange). For an upcoming box the clearance is at least
//      the margin (question 2 took the rest); an alongside box can be closer, and pins
//      at the band's near end. A box beyond the margin on both axes is a corner,
//      nobody's in v1; one within the margin on both (only possible alongside, hugging a
//      corner) goes to the nearer face. A box the course runs inside is the engine's.
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
// Scope (docs/accessibility-obstacle-direction-cues.md): on rails and in solo all-range,
// where the three are quiet through a U-turn or somersault and skip the non-lockable
// fighters (the wingmates and allied craft, left to the ahead cue). Terrain (a heightfield
// poly box, whose box spans the whole hill) is never read as a box: below reads the
// engine's own surface height along the course (ObstacleDirectionCue_TerrainBelow) and
// beside walks sideways from the course to where the surface rises within the margin of
// it (ObstacleDirectionCue_TerrainBeside), both over the mesh's yawed footprint, and one
// hill may answer below, left and right at once, each answer competing in its own
// direction. Ownership is split as in question 2: the below walk hands the ahead cue
// individual samples, the beside walk yields the whole hill while the buzz sounds for
// it. The engine's poly range gate enters only
// through the ahead cue's verdict and the terrain walks: a solid mesh inside the side band
// is inside the gate anyway. Ground, water and lava are not objects and stay out. Every
// solid box is axis-aligned in world space (obj.rot ignored, ObstacleScan.h), which in a
// turned heading shifts the clearance of a yawed wall.
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

// The beside cue's two directions, in the index order every per-side array in this file
// uses: [0] left, [1] right.
static constexpr ObstacleDirection kSideDirs[2] = { OBSTACLE_DIR_LEFT, OBSTACLE_DIR_RIGHT };

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

// The tick's effective knob values, read once (sanitized) and mirrored into the debug
// state, so every stage of the rule sees the same numbers.
struct ObstacleDirectionKnobs {
    f32 margin;
    f32 lookahead;
    f32 sideDist, vertDist;
    f32 panFloor, levelFloor;
    f32 warnDist;        // the ahead cue's band, which its verdict on a box is asked with
    bool terrainIsFloor; // a heightfield is the floor, not a crash, for the current vehicle
};

static ObstacleDirectionKnobs ObstacleDirectionCue_ReadKnobs(ObstacleDirectionCueDebug& dbg) {
    ObstacleDirectionKnobs k;
    k.margin = ObstacleCommon_Margin();
    k.lookahead = CueCommon_ReadPositiveFloat(kObstacleCueDirLookaheadCVar, kObstacleCueDirLookaheadDefault,
                                              kObstacleCueDirLookaheadMax);
    // The bands must reach past the margin or the mapping divides by zero; a band the
    // margin has swallowed (someone dragged the margin above it) is widened to one unit.
    k.sideDist = CueCommon_ReadPositiveFloat(kObstacleCueDirSideDistCVar, kObstacleCueDirSideDistDefault,
                                             kObstacleCueDirSideDistMax);
    if (k.sideDist <= k.margin) {
        k.sideDist = k.margin + 1.0f;
    }
    k.vertDist = CueCommon_ReadPositiveFloat(kObstacleCueDirVertDistCVar, kObstacleCueDirVertDistDefault,
                                             kObstacleCueDirVertDistMax);
    if (k.vertDist <= k.margin) {
        k.vertDist = k.margin + 1.0f;
    }
    k.panFloor =
        CueCommon_ReadFloat(kObstacleCueDirSidePanFloorCVar, kObstacleCueDirSidePanFloorDefault, 0.0f, kFloorMax);
    k.levelFloor =
        CueCommon_ReadFloat(kObstacleCueDirVertLevelFloorCVar, kObstacleCueDirVertLevelFloorDefault, 0.0f, kFloorMax);
    k.warnDist = ObstacleAheadCue_WarnDist();
    // The terrain walk is skipped for the ground vehicles (ObstacleCommon.h) — a forward
    // guard, no level in scope pairs the two today.
    k.terrainIsFloor = ObstacleCommon_TerrainIsFloor();
    dbg.margin = k.margin;
    dbg.lookahead = k.lookahead;
    dbg.sideDist = k.sideDist;
    dbg.panFloor = k.panFloor;
    dbg.vertDist = k.vertDist;
    dbg.levelFloor = k.levelFloor;
    dbg.warnDist = k.warnDist;
    return k;
}

// One answer to question 3 for one box: is the box in a direction's band, which
// direction, and how close. One shape for a solid box and a terrain box alike, offered to
// the direction's contest (ObstacleDirectionCue_Offer). A solid box fills one per pair
// (side, vertical) and the closer of the two takes the box; a terrain box fills up to
// three from its walks — below, left, right — and each competes in its own direction.
struct ObstacleAxisAnswer {
    bool ok = false;
    f32 clear = INFINITY; // the clearance the band maps; INFINITY while !ok
    ObstacleDirection dir = OBSTACLE_DIR_BELOW;
    bool upcoming = false;        // the box's near face still ahead; for a terrain walk
                                  // answer, the winning sample ahead (vs beneath the ship)
    bool fromTerrainWalk = false; // clear is from a terrain walk over the engine's surface
                                  // (ObstacleDirectionCue_TerrainBelow: height above it;
                                  // ObstacleDirectionCue_TerrainBeside: horizontal distance
                                  // to where it rises); surfaceY / sampleT are valid
    f32 surfaceY = 0.0f;
    f32 sampleT = 0.0f;
};

// Per-direction winner bookkeeping for one tick.
struct ObstacleDirectionWinner {
    bool found = false;
    ObstacleAxisAnswer answer; // answer.clear is INFINITY until a box wins
    ObstacleBox box{};
    f32 gap = 0.0f; // the box's nearest course distance (question 1's nearT)
};

// The range of offsets along `shift` over the part of a box that lies inside the
// lookahead window along the course (0 <= t <= lookahead) and within `margin` of the
// course on the OTHER steering axis (|q . other| <= margin), q being a point relative to
// the ship. `found` false when no part of the box is in that slab. This is question 3's
// geometry, exact for the axis-aligned box in any heading: an offset range entirely
// above zero means the box is off to the + side (right / above) by `lo`, entirely below
// zero off to the - side by `-hi`, and one containing zero means the course itself runs
// through that slice of the box (the ahead cue's, or already inside it).
//
// On rails (fwd -z, right +x, up +y) the box's offsets decouple from the window and the
// slab, and the range is exactly the rails rule's [dx - half.x, dx + half.x] — clearX
// with its sign — gated by "clearY within the margin", so the rails numbers are
// unchanged. In a turned heading the box is a rotated block in the frame and its
// clearance must come from its corners and edges, not from world-axis extents: the
// answer is a small linear program (extremes of a linear function over a convex
// polytope), solved by evaluating every point that can be a vertex of that polytope —
// the box corners inside the slab, the box edges crossing the four slab planes, and the
// four slab-plane intersection lines (each runs along `shift`) clipped to the box. At
// most 8 + 48 + 8 candidates, each a few multiplies; only boxes inside the window get here.
struct ObstacleSliceRange {
    bool found = false;
    f32 lo = INFINITY;
    f32 hi = -INFINITY;
};

static ObstacleSliceRange ObstacleDirectionCue_SliceRange(const ObstacleBox& box, const Vec3f& fwd, f32 lookahead,
                                                          const Vec3f& other, f32 margin, const Vec3f& shift) {
    // World-unit tolerance on every containment test, so a candidate computed on a face
    // is not rejected by rounding (coordinates reach ~20000, where a float ulp is ~0.002).
    constexpr f32 kEps = 0.05f;
    constexpr f32 kParallelEps = 1e-6f; // direction-cosine floor for the unit vectors below
    const f32 d[3] = { box.dx, box.dy, box.dz };
    const f32 h[3] = { box.half.x, box.half.y, box.half.z };
    ObstacleSliceRange out;
    auto consider = [&](const Vec3f& q) {
        const f32 qa[3] = { q.x, q.y, q.z };
        for (int i = 0; i < 3; i++) {
            if (!(fabsf(qa[i] - d[i]) <= h[i] + kEps)) {
                return; // outside the box (or NaN)
            }
        }
        const f32 t = ObstacleCourse_Dot(q, fwd);
        const f32 o = ObstacleCourse_Dot(q, other);
        if (!((t >= -kEps) && (t <= lookahead + kEps) && (fabsf(o) <= margin + kEps))) {
            return; // outside the slab
        }
        const f32 s = ObstacleCourse_Dot(q, shift);
        out.found = true;
        if (s < out.lo) {
            out.lo = s;
        }
        if (s > out.hi) {
            out.hi = s;
        }
    };
    auto vertex = [&](int bits) -> Vec3f {
        return { d[0] + ((bits & 1) ? h[0] : -h[0]), d[1] + ((bits & 2) ? h[1] : -h[1]),
                 d[2] + ((bits & 4) ? h[2] : -h[2]) };
    };
    // The four slab planes, n . q = c.
    const Vec3f* planeN[4] = { &fwd, &fwd, &other, &other };
    const f32 planeC[4] = { 0.0f, lookahead, -margin, margin };

    for (int bits = 0; bits < 8; bits++) {
        consider(vertex(bits));
    }
    for (int axis = 0; axis < 3; axis++) {
        for (int bits = 0; bits < 8; bits++) {
            if (bits & (1 << axis)) {
                continue; // each edge once, from its low end along `axis`
            }
            const Vec3f p0 = vertex(bits);
            const Vec3f p1 = vertex(bits | (1 << axis));
            const Vec3f e = { p1.x - p0.x, p1.y - p0.y, p1.z - p0.z };
            for (int k = 0; k < 4; k++) {
                const f32 denom = ObstacleCourse_Dot(*planeN[k], e);
                if (fabsf(denom) < kParallelEps) {
                    continue; // the edge runs parallel to this plane
                }
                const f32 lambda = (planeC[k] - ObstacleCourse_Dot(*planeN[k], p0)) / denom;
                if ((lambda >= 0.0f) && (lambda <= 1.0f)) {
                    consider({ p0.x + e.x * lambda, p0.y + e.y * lambda, p0.z + e.z * lambda });
                }
            }
        }
    }
    // The slab-plane intersection lines: q0 + shift * s, clipped to the box
    // (ObstacleCourse_ClipToBox, with the box center taken relative to q0).
    const f32 sh[3] = { shift.x, shift.y, shift.z };
    for (int ti = 0; ti < 2; ti++) {
        for (int oi = 0; oi < 2; oi++) {
            const f32 t0 = ti ? lookahead : 0.0f;
            const f32 o0 = oi ? margin : -margin;
            const f32 q0[3] = { fwd.x * t0 + other.x * o0, fwd.y * t0 + other.y * o0, fwd.z * t0 + other.z * o0 };
            const f32 rel[3] = { d[0] - q0[0], d[1] - q0[1], d[2] - q0[2] };
            f32 lo = -INFINITY;
            f32 hi = INFINITY;
            if (ObstacleCourse_ClipToBox(rel, h, sh, 3, &lo, &hi)) {
                consider({ q0[0] + shift.x * lo, q0[1] + shift.y * lo, q0[2] + shift.z * lo });
                consider({ q0[0] + shift.x * hi, q0[1] + shift.y * hi, q0[2] + shift.z * hi });
            }
        }
    }
    return out;
}

// A slice range, classified into one pair's answer: off to the + side by `lo`, to the -
// side by `-hi`, or no answer when the range straddles zero or lies beyond `band`, the
// pair's outer edge.
static ObstacleAxisAnswer ObstacleDirectionCue_Classify(const ObstacleSliceRange& r, f32 band, ObstacleDirection plus,
                                                        ObstacleDirection minus) {
    ObstacleAxisAnswer out;
    if (!r.found) {
        return out;
    }
    f32 clear;
    ObstacleDirection dir;
    if (r.lo > 0.0f) {
        clear = r.lo;
        dir = plus;
    } else if (r.hi < 0.0f) {
        clear = -r.hi;
        dir = minus;
    } else {
        return out;
    }
    if (clear < band) {
        out.ok = true;
        out.clear = clear;
        out.dir = dir;
    }
    return out;
}

// The terrain walk samples on the family's shared grid (ObstacleCommon.h: kObstacleWalkStep
// / kObstacleWalkMaxSteps, the same grid as the ahead cue's walk, which is what makes the
// per-sample claim below the ahead cue's own hit); the cap must still reach the far end of
// this cue's longest lookahead.
static_assert((f32) kObstacleWalkMaxSteps * kObstacleWalkStep >= kObstacleCueDirLookaheadMax,
              "the terrain walk must reach the far end of the longest lookahead");

struct ObstacleTerrainBelow {
    bool found = false;   // a sample qualified; the fields below describe the closest one
    f32 clear = INFINITY; // the course's height above the surface there
    f32 surfaceY = 0.0f;
    f32 t = 0.0f;         // its course distance; 0 = beneath the ship
    bool claimed = false; // at least one upcoming sample was left to the ahead cue
};

// The course samples a terrain walk reads over [tLo, tHi]: the span's two ends exactly,
// and between them the grid points anchor + k * kObstacleWalkStep. The anchor is where
// the ahead cue's walk over the box starts (`grid.start` when `grid.walks`, else tLo), so
// the walks read the same points wherever they overlap; on rails it is tLo itself unless
// the engine's range gate moves the ahead cue's start (the grid then shifts to match it).
// Grid points within a sliver of either end are skipped as duplicates, so with the anchor
// at tLo this is exactly tLo, tLo + step, ..., tHi. Calls fn(t, beneath), `beneath`
// marking the sample under the ship (t 0, present when the ship is already level with
// the box). False, and no samples, for a NaN or reversed span.
template <typename Fn>
static bool ObstacleDirectionCue_ForEachWalkSample(f32 tLo, f32 tHi, const ObstacleAheadWalk& grid, Fn&& fn) {
    int steps;
    if (!ObstacleCommon_WalkSteps(tHi - tLo, &steps)) {
        return false;
    }
    f32 anchor = grid.walks ? grid.start : tLo;
    if (!(fabsf(anchor - tLo) <= (f32) kObstacleWalkMaxSteps * kObstacleWalkStep)) {
        anchor = tLo; // NaN, or so far off the span that the grid phase is meaningless
    }
    constexpr f32 kEndTol = 0.01f;
    fn(tLo, !(tLo > 0.0f));
    const f32 firstStep = ceilf((tLo - anchor) / kObstacleWalkStep); // grid index of the first point past tLo
    for (int i = 0; i <= steps; i++) {
        const f32 t = anchor + (firstStep + (f32) i) * kObstacleWalkStep;
        if (t <= tLo + kEndTol) {
            continue;
        }
        if (t >= tHi - kEndTol) {
            break;
        }
        fn(t, false);
    }
    if (tHi > tLo + kEndTol) {
        fn(tHi, false);
    }
    return true;
}

// The engine's XZ range gate (box.polyRangeXZ) at a world point: Player_CollisionCheck
// never tests the mesh while the ship is farther than that from obj.pos, so a surface out
// there cannot be hit. True for an ungated box.
static bool ObstacleDirectionCue_InPolyRange(const ObstacleBox& box, f32 x, f32 z) {
    if (!(box.polyRangeXZ > 0.0f)) {
        return true;
    }
    const f32 rx = x - box.objPos.x;
    const f32 rz = z - box.objPos.z;
    return (rx * rx + rz * rz) <= box.polyRangeXZ * box.polyRangeXZ;
}

// "Below" for a heightfield poly box (box.polyHeightfield — the terrain bumps, reefs,
// the island, the mountains). The engine hits such a mesh only when the ship is at or
// below the surface under it, so the box top — the hill's PEAK — says nothing about the
// ground under a ship over the outer slope, and the box rule droned over most of
// Corneria. This asks the engine for the surface height instead
// (Object_PolyHeightfieldSurfaceY, one read per point, the number its crash test
// compares against) along the stretch [tLo, tHi] of the course over the box, reading the
// surface under each sample point and `margin` to either side along frame.right (the
// box rule's "within the margin on the other axis", and the same three points the ahead
// cue's walk probes) and keeping the highest; the clearance is the course's height above
// it — on rails the ship's own Y, in all-range the course climbing or diving with the
// aim. The clearance is measured straight down (world Y) whatever the heading: the
// surface is hit from above, and gravity's "below" is what a height above ground means.
//
// The samples sit on the family grid anchored where the ahead cue's walk over this box
// starts, plus the span's two ends (ObstacleDirectionCue_ForEachWalkSample), so where
// the two walks overlap they read the same points.
//
// Ownership is decided per SAMPLE, not per box (one hill is both "ground 300 below me"
// and "a slope rising into my course"): an upcoming sample whose surface is within the
// margin of the course is the ahead cue's — the same threshold its walk hits at — and is
// skipped, but only where that walk actually reaches (`claim`, its plan from
// ObstacleCourse_PlanAheadWalk, short of the warn distance, where it drops a
// hit); past it, e.g. with the warn distance below the lookahead, such a sample competes
// as an ordinary below answer rather than going silent in both cues. The rest compete
// too, so a rising slope sounds as the buzz plus a loud below chord: "the thing ahead
// is ground, climb". The sample beneath the ship (t 0,
// present when the ship is already over the box) is the "alongside" case and pins at the
// band's near end however close. (The ahead cue's walk starts beneath the ship too, so
// skimming within the margin sounds both — a known, accepted overlap: the chord is what
// says the buzz is about the ground.) A sample at or below the surface is the engine's.
//
// A sample is dropped when the ship there would be outside the engine's XZ range gate
// (ObstacleDirectionCue_InPolyRange): a bump's box is deep enough for its far end to lie
// beyond it.
static ObstacleTerrainBelow ObstacleDirectionCue_TerrainBelow(const ObstacleBox& box, const ObstacleCourseFrame& frame,
                                                              f32 tLo, f32 tHi, const ObstacleAheadWalk& claim,
                                                              f32 margin, int32_t* probes) {
    ObstacleTerrainBelow out;
    PolyHeightfield hf;
    if (!Object_ResolvePolyHeightfield(box.polyColId, &box.objPos, box.rotY, &hf)) {
        return out; // not a tabled mesh — unreachable for a scan-produced box
    }
    const s32 lateral = (margin > 0.0f) ? 1 : 0;
    ObstacleDirectionCue_ForEachWalkSample(tLo, tHi, claim, [&](f32 t, bool beneath) {
        const Vec3f p = { frame.origin.x + frame.fwd.x * t, frame.origin.y + frame.fwd.y * t,
                          frame.origin.z + frame.fwd.z * t };
        if (!ObstacleDirectionCue_InPolyRange(box, p.x, p.z)) {
            return;
        }
        bool any = false;
        f32 top = 0.0f;
        for (s32 k = -lateral; k <= lateral; k++) {
            const f32 x = p.x + frame.right.x * (f32) k * margin;
            const f32 z = p.z + frame.right.z * (f32) k * margin;
            f32 surfaceY;
            (*probes)++;
            if (Object_PolyHeightfieldSurfaceY(&hf, x, z, &surfaceY) && (!any || (surfaceY > top))) {
                any = true;
                top = surfaceY;
            }
        }
        if (!any) {
            return; // no surface under this stretch (outside the mesh's outline)
        }
        const f32 clear = p.y - top;
        const bool aheadWalksHere = claim.walks && (t >= claim.start) && (t <= claim.end) && (t < claim.warnDist);
        if (!beneath && aheadWalksHere && (clear < margin)) {
            out.claimed = true; // the ahead cue's walk hits here
            return;
        }
        if (!(clear > 0.0f)) {
            return; // at or below the surface: the engine's
        }
        if (clear < out.clear) {
            out.found = true;
            out.clear = clear;
            out.surfaceY = top;
            out.t = t;
        }
    });
    return out;
}

// All-range's terrain walk span: the stretch of the course whose horizontal position is
// over the box's footprint widened by the margin on both world axes (the lateral probes
// reach that far), clipped to [0, lookahead]. False when the course never passes over it.
// Rails keeps its own span (the box's Z extent, the stretch the rails walk has always
// covered), so its sampling is unchanged.
static bool ObstacleDirectionCue_FootprintSpan(const ObstacleBox& box, const Vec3f& fwd, f32 margin, f32 lookahead,
                                               f32* tLo, f32* tHi) {
    const f32 d[2] = { box.dx, box.dz };
    const f32 h[2] = { box.half.x + margin, box.half.z + margin };
    const f32 f[2] = { fwd.x, fwd.z };
    *tLo = 0.0f;
    *tHi = lookahead;
    return ObstacleCourse_ClipToBox(d, h, f, 2, tLo, tHi);
}

// The vertical half of question 3 for a terrain box: always BELOW (a heightfield is only
// ever hit from above), measured to the engine's surface along the course rather than
// the box top. Walked when some of the box lies within the margin of the course
// laterally (`vertRange` non-empty) and terrain is a crash for the current vehicle. The
// walk's span is this cue's own — rails the box's Z extent inside the window, all-range
// the stretch of course over the footprint — but its grid is anchored where the ahead
// cue's walk starts (`ahead.start`, valid when `ahead.walks`), see TerrainBelow. `box`
// is the yawed footprint box (ObstacleScan_YawedFootprint), so both spans cover a turned
// hill's corners.
static ObstacleAxisAnswer ObstacleDirectionCue_TerrainVertical(const ObstacleBox& box, const ObstacleCourseFrame& frame,
                                                               bool allRange, const ObstacleDirectionKnobs& knobs,
                                                               const ObstacleSliceRange& vertRange, f32 nearT, f32 farT,
                                                               const ObstacleAheadWalk& ahead,
                                                               ObstacleDirectionCueDebug& dbg) {
    ObstacleAxisAnswer vert;
    if (!vertRange.found || knobs.terrainIsFloor) {
        return vert;
    }
    f32 tLo;
    f32 tHi;
    if (allRange) {
        if (!ObstacleDirectionCue_FootprintSpan(box, frame.fwd, knobs.margin, knobs.lookahead, &tLo, &tHi)) {
            return vert;
        }
    } else {
        tLo = (nearT > 0.0f) ? nearT : 0.0f;
        tHi = (farT < knobs.lookahead) ? farT : knobs.lookahead;
    }
    dbg.terrainWalkTested++;
    const ObstacleTerrainBelow terrain =
        ObstacleDirectionCue_TerrainBelow(box, frame, tLo, tHi, ahead, knobs.margin, &dbg.terrainWalkProbes);
    if (terrain.claimed) {
        dbg.terrainWalkClaimed++;
    }
    if (terrain.found && (terrain.clear < knobs.vertDist)) {
        vert.ok = true;
        vert.clear = terrain.clear;
        vert.dir = OBSTACLE_DIR_BELOW;
        vert.upcoming = terrain.t > 0.0f;
        vert.fromTerrainWalk = true;
        vert.surfaceY = terrain.surfaceY;
        vert.sampleT = terrain.t;
    }
    return vert;
}

// The sideways walk's lateral grid: the family step (a mesh triangle is hundreds of
// units across, so it cannot step over a slope), then a few halvings between the last
// clear point and the first blocked one, so the clearance — which goes straight into the
// pan — is good to step / 2^bisections (about 6 units) instead of jumping a tenth of the
// pan range per step as the ship drifts.
static constexpr int kSideWalkMaxSteps = (int) (kObstacleCueDirSideDistMax / kObstacleWalkStep) + 1;
static constexpr int kSideWalkBisections = 4;

struct ObstacleTerrainSide {
    bool found = false;   // a crossing within reach; the fields below describe the closest
    f32 clear = INFINITY; // horizontal distance from the course to the crossing
    f32 surfaceY = 0.0f;  // the surface height at the crossing's blocked end
    f32 t = 0.0f;         // course distance of the sample it was found from; 0 = beneath the ship
};

// "Beside" for a heightfield poly box, the sideways counterpart of TerrainBelow. The box
// spans the whole hill, so its side face says nothing about the slope at the ship's
// altitude: flown beside a hill near its peak height the face is far closer than the
// slope, and flown low over its outer slope with the peak rising to one side the box
// surrounds the ship and the box rule has no answer at all. This walks sideways instead:
// from each course sample on [tLo, tHi] (the grid of ForEachWalkSample), out along
// frame.right to either side — horizontal in both modes, so the line stays at the
// course's height — and asks the engine for the surface under each point
// (Object_PolyHeightfieldSurfaceY). A point is BLOCKED when the surface there comes
// within `margin` of the course height: the box rule's "within the margin on the other
// axis" restated for a surface hit from above, and the threshold the ahead cue's walk
// hits at. The first blocked point on a side, refined by bisection, is the crossing; its
// horizontal distance is the clearance the pan maps. Per side the smallest over the
// samples wins.
//
// A sample whose own course point is already blocked is skipped: the surface under the
// course is the vertical question's (the below chord, or the ahead cue's buzz), not a
// wall to steer around. Points outside the engine's range gate (InPolyRange) are clear —
// the engine never tests the mesh from there, which trims the far flanks of Fortuna's
// 2000-wide mountains. Each side's march stops at `reach[side]` (the side band, cut to
// where the box's footprint ends) and at the side's best crossing so far, since only a
// closer one can win; a point outside the mesh's outline costs only the bounds check.
// `reach` and `out` are indexed in kSideDirs order.
static void ObstacleDirectionCue_TerrainBeside(const ObstacleBox& box, const ObstacleCourseFrame& frame, f32 tLo,
                                               f32 tHi, const ObstacleAheadWalk& grid, f32 margin,
                                               const f32 (&reach)[2], ObstacleTerrainSide (&out)[2], int32_t* probes) {
    PolyHeightfield hf;
    if (!Object_ResolvePolyHeightfield(box.polyColId, &box.objPos, box.rotY, &hf)) {
        return; // not a tabled mesh — unreachable for a scan-produced box
    }
    ObstacleDirectionCue_ForEachWalkSample(tLo, tHi, grid, [&](f32 t, bool beneath) {
        const Vec3f p = { frame.origin.x + frame.fwd.x * t, frame.origin.y + frame.fwd.y * t,
                          frame.origin.z + frame.fwd.z * t };
        const f32 threshold = p.y - margin;
        // Is the point `s` along `sign * right` blocked? `surfaceY` is written when it is.
        auto blocked = [&](f32 sign, f32 s, f32* surfaceY) {
            const f32 x = p.x + frame.right.x * sign * s;
            const f32 z = p.z + frame.right.z * sign * s;
            if (!ObstacleDirectionCue_InPolyRange(box, x, z)) {
                return false;
            }
            (*probes)++;
            f32 y;
            if (!Object_PolyHeightfieldSurfaceY(&hf, x, z, &y) || !(y >= threshold)) {
                return false; // no surface there, or it clears (a NaN course height clears too)
            }
            *surfaceY = y;
            return true;
        };
        f32 unused;
        if (blocked(1.0f, 0.0f, &unused)) {
            return; // the course itself is over the surface's margin: the vertical question's
        }
        for (int side = 0; side < 2; side++) {
            const f32 sign = (kSideDirs[side] == OBSTACLE_DIR_LEFT) ? -1.0f : 1.0f;
            const f32 limit = (reach[side] < out[side].clear) ? reach[side] : out[side].clear;
            if (!(limit > 0.0f)) {
                continue;
            }
            f32 prev = 0.0f;
            for (int i = 1; i <= kSideWalkMaxSteps; i++) {
                f32 s = (f32) i * kObstacleWalkStep;
                const bool last = !(s < limit);
                if (last) {
                    s = limit;
                }
                f32 surfaceY;
                if (blocked(sign, s, &surfaceY)) {
                    f32 lo = prev;
                    f32 hi = s;
                    for (int b = 0; b < kSideWalkBisections; b++) {
                        const f32 mid = 0.5f * (lo + hi);
                        f32 midY;
                        if (blocked(sign, mid, &midY)) {
                            hi = mid;
                            surfaceY = midY;
                        } else {
                            lo = mid;
                        }
                    }
                    if (hi < out[side].clear) {
                        out[side].found = true;
                        out[side].clear = hi;
                        out[side].surfaceY = surfaceY;
                        out[side].t = beneath ? 0.0f : t;
                    }
                    break;
                }
                prev = s;
                if (last) {
                    break;
                }
            }
        }
    });
}

// The side half of question 3 for a terrain box: the sideways walk, both sides, each
// answered independently (a valley inside one mesh sounds as a corridor), into `sides`
// in kSideDirs order. `box` is the yawed footprint box (ObstacleScan_YawedFootprint) and
// `ahead` the ahead cue's plan over it (ObstacleCourse_PlanAheadWalk), which both anchors
// the walk's grid and is what the ahead cue's verdict walks. Silent for the whole box
// while the ahead cue is buzzing for it (ObstacleCourse_AheadHitsTerrain): a hill whose
// slope the course runs into is the buzz's, and "that slope is more to your left" would
// give the chord a second meaning. The sideways walk is cut at the lookahead like the
// below walk. Skipped, like every terrain walk, for the ground vehicles.
static void ObstacleDirectionCue_TerrainSide(const ObstacleBox& box, const ObstacleCourseFrame& frame,
                                             const ObstacleDirectionKnobs& knobs, f32 nearT, f32 farT,
                                             const ObstacleAheadWalk& ahead, ObstacleAxisAnswer (&sides)[2],
                                             ObstacleDirectionCueDebug& dbg) {
    if (knobs.terrainIsFloor) {
        return;
    }
    const f32 tLo = (nearT > 0.0f) ? nearT : 0.0f;
    const f32 tHi = (farT < knobs.lookahead) ? farT : knobs.lookahead;
    if (!(tHi >= tLo)) {
        return;
    }
    // Cheap pre-filters on the yawed box, both supersets of what the walk could find.
    // Height: the walk's blocked test compares the course's WORLD height against the
    // engine's surface value, which is in the mesh's LOCAL frame (obj.pos.y is never added
    // back — Object_PolyHeightfieldSurfaceY), so the surface can reach at most the box top
    // minus obj.pos.y in those terms. That local top must come within the margin of the
    // course's lowest point on the span. Comparing the world top instead would skip a real
    // crossing on a mesh placed below zero (Zoness's islands sit as low as -80). Side: the
    // footprint must lie within the side band on a side (its corners' offsets along
    // frame.right, which is horizontal).
    const f32 lowestCourseY = (frame.fwd.y * tLo < frame.fwd.y * tHi) ? frame.fwd.y * tLo : frame.fwd.y * tHi;
    if (!(box.dy + box.half.y - box.objPos.y >= lowestCourseY - knobs.margin)) {
        return;
    }
    const f32 centerR = box.dx * frame.right.x + box.dz * frame.right.z;
    const f32 extentR = fabsf(frame.right.x) * box.half.x + fabsf(frame.right.z) * box.half.z;
    f32 reach[2]; // kSideDirs order: how far the footprint extends to that side
    reach[0] = -(centerR - extentR);
    reach[1] = centerR + extentR;
    bool any = false;
    for (int side = 0; side < 2; side++) {
        if (reach[side] > knobs.sideDist) {
            reach[side] = knobs.sideDist;
        }
        if (reach[side] > 0.0f) {
            any = true;
        }
    }
    if (!any) {
        return;
    }
    f32 gap;
    if (ObstacleCourse_AheadHitsTerrain(box, frame, ahead, knobs.margin, &gap, &dbg.terrainSideAheadProbes)) {
        dbg.terrainSideAheadBuzzing++;
        return;
    }
    dbg.terrainSideTested++;
    ObstacleTerrainSide found[2];
    ObstacleDirectionCue_TerrainBeside(box, frame, tLo, tHi, ahead, knobs.margin, reach, found, &dbg.terrainSideProbes);
    for (int side = 0; side < 2; side++) {
        if (!found[side].found || !(found[side].clear < knobs.sideDist)) {
            continue;
        }
        ObstacleAxisAnswer& a = sides[side];
        a.ok = true;
        a.clear = found[side].clear;
        a.dir = kSideDirs[side];
        a.upcoming = found[side].t > 0.0f;
        a.fromTerrainWalk = true;
        a.surfaceY = found[side].surfaceY;
        a.sampleT = found[side].t;
    }
}

static void ObstacleDirectionCue_StopAll() {
    sSideCue->StopAllVoices();
    sAboveCue->Stop();
    sBelowCue->Stop();
}

// The gates, in the ahead cue's order plus this family's own, and the frame. False when
// the tick is gated off (every voice stopped, the failing gate recorded in `dbg`).
static bool ObstacleDirectionCue_Gate(ObstacleDirectionCueDebug& dbg, bool* allRange, ObstacleCourseFrame* frame) {
    dbg.enabled = CueCommon_IsEnabled();
    dbg.obstacleEnabled = ObstacleCommon_IsEnabled();
    // Same gate set as the ahead cue (see its comment on why `control` is what makes the
    // reads below safe).
    dbg.modeOk = CueScan_ModeInScope(allRange);
    dbg.allRange = *allRange;
    dbg.control = Accessibility_PlayerHasControl();
    dbg.frame = (int32_t) gGameFrameCount;
    if (!dbg.enabled || !dbg.obstacleEnabled || !dbg.modeOk || !dbg.control) {
        ObstacleDirectionCue_StopAll();
        return false;
    }
    const Player& player = gPlayer[0];
    // The heading frame (ObstacleCourse_Frame): on rails the fixed -z track with the world
    // axes as the player's left/right and up/down, in all-range the aim heading's frame,
    // which only the Arwing / Blue Marine composition yields — every solo arena flies an
    // Arwing, and the ahead cue stops on the same gate.
    if (!ObstacleCourse_Frame(player, *allRange, frame)) {
        dbg.aimValid = false;
        ObstacleDirectionCue_StopAll();
        return false;
    }
    // A U-turn or somersault is a scripted maneuver: the player is not steering, "you
    // would hit it by steering that way" does not apply, and the frame swings through
    // (and upside down) faster than a chord can say anything, so all three go quiet until
    // it ends. Rails keeps its world frame through a somersault loop and is unchanged.
    if (*allRange && ((player.state == PLAYERSTATE_U_TURN) || player.somersault)) {
        dbg.noManeuver = false;
        ObstacleDirectionCue_StopAll();
        return false;
    }
    dbg.fwdX = frame->fwd.x;
    dbg.fwdY = frame->fwd.y;
    dbg.fwdZ = frame->fwd.z;
    return true;
}

// One answer from one box, offered to its direction's contest: the smallest clearance
// wins. `gap` is the box's nearest course distance (question 1's nearT).
static void ObstacleDirectionCue_Offer(ObstacleDirectionWinner* winners, const ObstacleAxisAnswer& answer,
                                       const ObstacleBox& box, f32 gap, ObstacleDirectionCueDebug& dbg) {
    ObstacleDirectionWinner& w = winners[answer.dir];
    dbg.dir[answer.dir].candidates++;
    if (answer.clear < w.answer.clear) {
        w.found = true;
        w.answer = answer;
        w.box = box;
        w.gap = gap;
    }
}

// The three questions, asked of one box; records the box in `winners` when it answers.
static void ObstacleDirectionCue_ClassifyBox(const ObstacleBox& scanned, const ObstacleCourseFrame& frame,
                                             bool allRange, const ObstacleDirectionKnobs& knobs,
                                             ObstacleDirectionWinner* winners, ObstacleDirectionCueDebug& dbg) {
    // The non-lockable fighters — the all-range wingmates (Falco, Slippy, Peppy) and
    // the allied craft — pass the obstacle predicate, but they fly: a wingmate beside
    // you does not keep that space closed, and they are near you constantly. Left to
    // the ahead cue, which still warns of one crossing your course. Lockable fighters
    // are enemies and never reach the scan.
    if ((scanned.array == OBSTACLE_ARRAY_ACTOR) && (scanned.objId == OBJ_ACTOR_ALLRANGE)) {
        dbg.fightersSkipped++;
        return;
    }
    // A terrain mesh is reasoned about on its YAWED footprint box (the scan's box ignores
    // obj.rot.y, and most terrain meshes are turned), so a turned hill's corners are not
    // left out of the window or the walks' spans; the walks' probes are exact either way.
    // Yawed once here and used for everything below, the ahead cue's plan and verdict
    // included, as the ahead cue does with the same box.
    const ObstacleBox box = scanned.polyHeightfield ? ObstacleScan_YawedFootprint(scanned, frame.origin) : scanned;
    // Question 1: the lookahead window along the course. The box's extent along it is
    // [nearT, farT] (its center's course distance, plus or minus the projection of its
    // half-extents); on rails nearT is gapZ, the near Z face.
    const f32 centerT = ObstacleCourse_Dot({ box.dx, box.dy, box.dz }, frame.fwd);
    const f32 extentT =
        fabsf(frame.fwd.x) * box.half.x + fabsf(frame.fwd.y) * box.half.y + fabsf(frame.fwd.z) * box.half.z;
    const f32 nearT = centerT - extentT;
    const f32 farT = centerT + extentT;
    const bool upcoming = (nearT > 0.0f) && (nearT < knobs.lookahead);
    const bool alongside = !(nearT > 0.0f) && (farT > 0.0f);
    if (!upcoming && !alongside) {
        return;
    }
    dbg.inWindow++;
    // Question 3's slice for the vertical pair: the part of the box inside the window and
    // within the margin of the course laterally, measured vertically (SliceRange).
    const ObstacleSliceRange vertRange =
        ObstacleDirectionCue_SliceRange(box, frame.fwd, knobs.lookahead, frame.right, knobs.margin, frame.up);

    if (box.polyHeightfield) {
        // Terrain: question 2 is never settled for the whole box. The below walk leaves
        // the ahead cue its samples one by one, and the beside walk stays silent while
        // the ahead cue is buzzing for the hill (TerrainBelow, TerrainSide). One hill can
        // honestly be ground below, a slope rising to the right and another to the left
        // at once, and each answer comes from a different part of its surface, so each
        // competes in its own direction rather than the nearest taking the box.
        const ObstacleAheadWalk ahead =
            ObstacleCourse_PlanAheadWalk(box, frame, allRange, knobs.margin, knobs.warnDist);
        const ObstacleAxisAnswer below =
            ObstacleDirectionCue_TerrainVertical(box, frame, allRange, knobs, vertRange, nearT, farT, ahead, dbg);
        ObstacleAxisAnswer sides[2];
        ObstacleDirectionCue_TerrainSide(box, frame, knobs, nearT, farT, ahead, sides, dbg);
        const ObstacleAxisAnswer* answers[3] = { &below, &sides[0], &sides[1] };
        for (const ObstacleAxisAnswer* a : answers) {
            if (a->ok) {
                ObstacleDirectionCue_Offer(winners, *a, box, nearT, dbg);
            }
        }
        return;
    }

    // Question 2: the ahead cue's box, by the ahead cue's own verdict (ObstacleCourse.h;
    // a NaN fails every test below). A solid box it is warning about is skipped whole.
    f32 gap;
    if (ObstacleCourse_AheadClaimsSolid(box, frame, allRange, knobs.margin, knobs.warnDist, &gap)) {
        dbg.aheadClaimed++;
        return;
    }
    // Question 3: which steering axis the box is off to, and how far.
    ObstacleAxisAnswer vert =
        ObstacleDirectionCue_Classify(vertRange, knobs.vertDist, OBSTACLE_DIR_ABOVE, OBSTACLE_DIR_BELOW);
    vert.upcoming = upcoming;
    const ObstacleSliceRange sideRange =
        ObstacleDirectionCue_SliceRange(box, frame.fwd, knobs.lookahead, frame.up, knobs.margin, frame.right);
    ObstacleAxisAnswer side =
        ObstacleDirectionCue_Classify(sideRange, knobs.sideDist, OBSTACLE_DIR_RIGHT, OBSTACLE_DIR_LEFT);
    side.upcoming = upcoming;
    // The nearer face wins a solid box. A corner beyond the margin on both axes, or a box
    // the ship is inside on both, answers neither.
    if (side.ok && (!vert.ok || (side.clear <= vert.clear))) {
        ObstacleDirectionCue_Offer(winners, side, box, nearT, dbg);
    } else if (vert.ok) {
        ObstacleDirectionCue_Offer(winners, vert, box, nearT, dbg);
    }
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
    d.gap = w.gap;
    d.dx = w.box.dx;
    d.dy = w.box.dy;
    d.dz = w.box.dz;
    d.halfX = w.box.half.x;
    d.halfY = w.box.half.y;
    d.halfZ = w.box.half.z;
}

// Drives the voices from the tick's winners. Every voice sits on the unity-gain arc
// (CueCommon_PlaceOnPanArc), so the backend's distance attenuation never becomes a
// second loudness signal.
static void ObstacleDirectionCue_Drive(const ObstacleDirectionWinner* winners, const ObstacleDirectionKnobs& knobs,
                                       ObstacleDirectionCueDebug& dbg) {
    // Beside: one keyed voice per side, placed at a pan magnitude of exactly `pan`.
    // Refresh-or-stop: a side not targeted this tick is reaped by CueRegistry_Tick.
    for (ObstacleDirection dir : kSideDirs) {
        const ObstacleDirectionWinner& w = winners[dir];
        if (!w.found) {
            continue;
        }
        const f32 t = ObstacleDirectionCue_BandT(w.answer.clear, knobs.margin, knobs.sideDist);
        const f32 pan = knobs.panFloor + (1.0f - knobs.panFloor) * t; // reversed: far = hard, near = floor
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
        const f32 t = ObstacleDirectionCue_BandT(w.answer.clear, knobs.margin, knobs.vertDist);
        const f32 level = 1.0f - (1.0f - knobs.levelFloor) * t; // full at the margin, floor at the edge
        CueTarget target;
        CueCommon_PlaceOnPanArc(target, 0.0f);
        target.level = level;
        cue->SetTarget(target);
        cue->Start();
        ObstacleDirectionCue_FillTargetDebug(dbg.dir[dir], w);
        dbg.dir[dir].level = level;
    }
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
    bool allRange = false;
    ObstacleCourseFrame frame;
    if (!ObstacleDirectionCue_Gate(dbg, &allRange, &frame)) {
        return;
    }
    const ObstacleDirectionKnobs knobs = ObstacleDirectionCue_ReadKnobs(dbg);

    ObstacleScanStats stats;
    ObstacleDirectionWinner winners[OBSTACLE_DIR_COUNT];
    ObstacleScan_ForEachBox(&gPlayer[0], &stats, [&](const ObstacleBox& box) {
        ObstacleDirectionCue_ClassifyBox(box, frame, allRange, knobs, winners, dbg);
    });
    dbg.scanned = true;
    dbg.scanActive = stats.active;
    dbg.scanObstacles = stats.obstacles;
    dbg.scanBoxes = stats.boxes;

    ObstacleDirectionCue_Drive(winners, knobs, dbg);
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
