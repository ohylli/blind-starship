#include "ObstacleDirectionCue.h"
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

// The directional obstacle cues: sustained chords that say "the space beside / above /
// below you is closed" — there is something solid there that you are NOT going to hit if
// you fly straight, but would hit if you steered that way. They are the "which way is
// closed" half of the obstacle family; the ahead cue (ObstacleAheadCue.cpp) is the
// "how soon" half, and the two never claim the same box.
//
// The rule, asked of every box the shared scan yields (ObstacleScan.h), in the same order
// as the design record. It is stated in the HEADING FRAME (ObstacleCourseFrame): distance
// along the course, and offsets to the right and up off it. On rails that frame is the
// world's (the -z track, +x, +y); in all-range it is the aim heading's, bank ignored.
//   1. Is it alongside me now, or about to be? The box's nearest point along the course
//      within the lookahead ("upcoming"), or the ship already level with part of it
//      ("alongside"). Wholly behind, it is dropped. Unlike the ahead cue's "past the
//      near face -> silent" rule, a wall alongside still matters — drifting into it is
//      the whole risk.
//   2. Is it the ahead cue's? A box the course runs into, widened by the safety margin,
//      with the entry still ahead, is the one the ahead cue is warning about — decided by
//      the ahead cue's own test (ObstacleScan_CourseSpan: the footprint on rails, the
//      ray in all-range). Skipped here — the ahead cue never says which edge is nearest,
//      and this cue never says "the wall ahead is more to your left"; the split keeps
//      each sound's meaning single. Once the ship is level with the entry the ahead cue
//      drops the box, so from there on it is classified below however close it is.
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
// poly box, whose box spans the whole hill) is refined for BELOW only: the box top is
// replaced by the engine's own surface height sampled along the course
// (ObstacleDirectionCue_TerrainBelow). The side pair still reads a terrain box as a box,
// and so over-reports a hill flown beside near its peak height — Fortuna's mountains
// included. The engine's poly range gate is applied to the terrain walk and nowhere else:
// a solid mesh inside the side band is inside the gate anyway. Ground, water and lava are
// not objects and stay out. Every box is axis-aligned in world space (obj.rot ignored,
// ObstacleScan.h), which in a turned heading shifts the clearance of a yawed wall.
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
    f32 gap = 0.0f; // the box's nearest course distance (question 1's nearT)
};

// The heading frame the rule reasons in: the ship's center, the course, and the two
// directions the player can steer off it. On rails the fixed -z track with world +x
// (right) and +y (up); in all-range the aim heading's frame (Player_AimBasis — bank
// ignored, so "above" is toward the canopy and "beside" is what a turn would bring onto
// the course). All three are unit vectors and mutually orthogonal.
struct ObstacleCourseFrame {
    Vec3f origin;
    Vec3f fwd, right, up;
};

static f32 ObstacleDirectionCue_Dot(const Vec3f& a, const Vec3f& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

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
        const f32 t = ObstacleDirectionCue_Dot(q, fwd);
        const f32 o = ObstacleDirectionCue_Dot(q, other);
        if (!((t >= -kEps) && (t <= lookahead + kEps) && (fabsf(o) <= margin + kEps))) {
            return; // outside the slab
        }
        const f32 s = ObstacleDirectionCue_Dot(q, shift);
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
                const f32 denom = ObstacleDirectionCue_Dot(*planeN[k], e);
                if (fabsf(denom) < kParallelEps) {
                    continue; // the edge runs parallel to this plane
                }
                const f32 lambda = (planeC[k] - ObstacleDirectionCue_Dot(*planeN[k], p0)) / denom;
                if ((lambda >= 0.0f) && (lambda <= 1.0f)) {
                    consider({ p0.x + e.x * lambda, p0.y + e.y * lambda, p0.z + e.z * lambda });
                }
            }
        }
    }
    const f32 sh[3] = { shift.x, shift.y, shift.z };
    for (int ti = 0; ti < 2; ti++) {
        for (int oi = 0; oi < 2; oi++) {
            const f32 t0 = ti ? lookahead : 0.0f;
            const f32 o0 = oi ? margin : -margin;
            const f32 q0[3] = { fwd.x * t0 + other.x * o0, fwd.y * t0 + other.y * o0, fwd.z * t0 + other.z * o0 };
            f32 lo = -INFINITY;
            f32 hi = INFINITY;
            bool hit = true;
            for (int i = 0; (i < 3) && hit; i++) {
                if (fabsf(sh[i]) < kParallelEps) {
                    hit = fabsf(q0[i] - d[i]) <= h[i];
                    continue;
                }
                f32 s1 = (d[i] - h[i] - q0[i]) / sh[i];
                f32 s2 = (d[i] + h[i] - q0[i]) / sh[i];
                if (s1 > s2) {
                    const f32 tmp = s1;
                    s1 = s2;
                    s2 = tmp;
                }
                lo = (s1 > lo) ? s1 : lo;
                hi = (s2 < hi) ? s2 : hi;
            }
            if (hit && (lo <= hi)) {
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
// The samples sit on the grid anchor + k * kObstacleWalkStep, plus the span's two ends.
// The anchor is where the ahead cue's walk over this box starts, so where the two walks
// overlap they read the same points; on rails it is tLo itself unless the engine's range
// gate moves the ahead cue's start (the grid then shifts to match it).
//
// Ownership is decided per SAMPLE, not per box (one hill is both "ground 300 below me"
// and "a slope rising into my course"): an upcoming sample whose surface is within the
// margin of the course is the ahead cue's — the same threshold its walk hits at — and is
// skipped, while the rest still compete, so a rising slope sounds as the buzz plus a
// loud below chord: "the thing ahead is ground, climb". The sample beneath the ship (t 0,
// present when the ship is already over the box) is the "alongside" case and pins at the
// band's near end however close. (The ahead cue's walk starts beneath the ship too, so
// skimming within the margin sounds both — a known, accepted overlap: the chord is what
// says the buzz is about the ground.) A sample at or below the surface is the engine's.
//
// A sample is dropped when the ship there would be outside the engine's XZ range gate
// (box.polyRangeXZ): Player_CollisionCheck never tests the mesh from out there, and a
// bump's box is deep enough for its far end to lie beyond the gate.
static ObstacleTerrainBelow ObstacleDirectionCue_TerrainBelow(const ObstacleBox& box, const ObstacleCourseFrame& frame,
                                                              f32 tLo, f32 tHi, f32 anchor, f32 margin,
                                                              int32_t* probes) {
    ObstacleTerrainBelow out;
    PolyHeightfield hf;
    if (!Object_ResolvePolyHeightfield(box.polyColId, &box.objPos, box.rotY, &hf)) {
        return out; // not a tabled mesh — unreachable for a scan-produced box
    }
    int steps;
    if (!ObstacleCommon_WalkSteps(tHi - tLo, &steps)) {
        return out; // NaN or a reversed span
    }
    if (!(fabsf(anchor - tLo) <= (f32) kObstacleWalkMaxSteps * kObstacleWalkStep)) {
        anchor = tLo; // NaN, or so far off the span that the grid phase is meaningless
    }
    const s32 lateral = (margin > 0.0f) ? 1 : 0;
    const f32 rangeSq = box.polyRangeXZ * box.polyRangeXZ;
    auto sample = [&](f32 t, bool beneath) {
        const Vec3f p = { frame.origin.x + frame.fwd.x * t, frame.origin.y + frame.fwd.y * t,
                          frame.origin.z + frame.fwd.z * t };
        if (box.polyRangeXZ > 0.0f) {
            const f32 rx = p.x - box.objPos.x;
            const f32 rz = p.z - box.objPos.z;
            if ((rx * rx + rz * rz) > rangeSq) {
                return;
            }
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
        if (!beneath && (clear < margin)) {
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
    };
    // The span's ends are sampled exactly; grid points within a sliver of either end are
    // skipped as duplicates. With the anchor at tLo this is exactly tLo, tLo + step, ...,
    // tHi — the rails walk as first committed.
    constexpr f32 kEndTol = 0.01f;
    sample(tLo, !(tLo > 0.0f));
    const f32 kFirst = ceilf((tLo - anchor) / kObstacleWalkStep);
    for (int i = 0; i <= steps; i++) {
        const f32 t = anchor + (kFirst + (f32) i) * kObstacleWalkStep;
        if (t <= tLo + kEndTol) {
            continue;
        }
        if (t >= tHi - kEndTol) {
            break;
        }
        sample(t, false);
    }
    if (tHi > tLo + kEndTol) {
        sample(tHi, false);
    }
    return out;
}

// All-range's terrain walk span: the stretch of the course whose horizontal position is
// over the box's footprint widened by the margin on both world axes (the lateral probes
// reach that far), clipped to [0, lookahead]. False when the course never passes over it.
// Rails keeps its own span (the box's Z extent, the stretch the rails walk has always
// covered), so its sampling is unchanged.
static bool ObstacleDirectionCue_FootprintSpan(const ObstacleBox& box, const Vec3f& fwd, f32 margin, f32 lookahead,
                                               f32* tLo, f32* tHi) {
    constexpr f32 kParallelEps = 1e-6f;
    const f32 d[2] = { box.dx, box.dz };
    const f32 h[2] = { box.half.x + margin, box.half.z + margin };
    const f32 f[2] = { fwd.x, fwd.z };
    f32 lo = 0.0f;
    f32 hi = lookahead;
    for (int i = 0; i < 2; i++) {
        if (fabsf(f[i]) < kParallelEps) {
            if (!(fabsf(d[i]) <= h[i])) {
                return false;
            }
            continue;
        }
        f32 t1 = (d[i] - h[i]) / f[i];
        f32 t2 = (d[i] + h[i]) / f[i];
        if (t1 > t2) {
            const f32 tmp = t1;
            t1 = t2;
            t2 = tmp;
        }
        lo = (t1 > lo) ? t1 : lo;
        hi = (t2 < hi) ? t2 : hi;
    }
    if (!(lo <= hi)) {
        return false;
    }
    *tLo = lo;
    *tHi = hi;
    return true;
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
    d.gap = w.gap;
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
    // reads below safe).
    bool allRange = false;
    dbg.modeOk = CueScan_ModeInScope(&allRange);
    dbg.allRange = allRange;
    dbg.control = Accessibility_PlayerHasControl();
    dbg.frame = (int32_t) gGameFrameCount;
    if (!dbg.enabled || !dbg.obstacleEnabled || !dbg.modeOk || !dbg.control) {
        ObstacleDirectionCue_StopAll();
        return;
    }

    Player* player = &gPlayer[0];

    // The heading frame. On rails the course is the fixed -z track and the world axes
    // are the player's left/right and up/down. In all-range it is the aim heading
    // (Player_AimBasis), which only the Arwing / Blue Marine composition yields — every
    // solo arena flies an Arwing, and the ahead cue stops on the same gate. A U-turn or
    // somersault is a scripted maneuver: the player is not steering, "you would hit it by
    // steering that way" does not apply, and the frame swings through (and upside down)
    // faster than a chord can say anything, so all three go quiet until it ends. Rails
    // keeps its world frame through a somersault loop and is unchanged.
    ObstacleCourseFrame frame;
    frame.origin = { player->pos.x, player->pos.y, player->trueZpos }; // trueZpos: the real world Z
    if (allRange) {
        if (!Player_AimAnglesValid(*player)) {
            dbg.aimValid = false;
            ObstacleDirectionCue_StopAll();
            return;
        }
        if ((player->state == PLAYERSTATE_U_TURN) || player->somersault) {
            dbg.noManeuver = false;
            ObstacleDirectionCue_StopAll();
            return;
        }
        Player_AimBasis(*player, &frame.fwd, &frame.right, &frame.up);
    } else {
        frame.fwd = { 0.0f, 0.0f, -1.0f };
        frame.right = { 1.0f, 0.0f, 0.0f };
        frame.up = { 0.0f, 1.0f, 0.0f };
    }
    dbg.fwdX = frame.fwd.x;
    dbg.fwdY = frame.fwd.y;
    dbg.fwdZ = frame.fwd.z;

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

    // A heightfield is the floor, not a crash, for the ground vehicles (ObstacleCommon.h),
    // so the terrain walk is skipped for them — a forward guard, no level in scope pairs
    // the two today.
    const bool terrainIsFloor = ObstacleCommon_TerrainIsFloor();

    ObstacleScanStats stats;
    ObstacleDirectionWinner winners[OBSTACLE_DIR_COUNT];
    ObstacleScan_ForEachBox(player, &stats, [&](const ObstacleBox& box) {
        // The non-lockable fighters — the all-range wingmates (Falco, Slippy, Peppy) and
        // the allied craft — pass the obstacle predicate, but they fly: a wingmate beside
        // you does not keep that space closed, and they are near you constantly. Left to
        // the ahead cue, which still warns of one crossing your course. Lockable fighters
        // are enemies and never reach the scan.
        if ((box.array == OBSTACLE_ARRAY_ACTOR) && (box.objId == OBJ_ACTOR_ALLRANGE)) {
            dbg.fightersSkipped++;
            return;
        }
        // Question 1: the lookahead window along the course. The box's extent along it is
        // [nearT, farT] (its center's course distance, plus or minus the projection of its
        // half-extents); on rails nearT is gapZ, the near Z face.
        const f32 centerT = ObstacleDirectionCue_Dot({ box.dx, box.dy, box.dz }, frame.fwd);
        const f32 extentT =
            fabsf(frame.fwd.x) * box.half.x + fabsf(frame.fwd.y) * box.half.y + fabsf(frame.fwd.z) * box.half.z;
        const f32 nearT = centerT - extentT;
        const f32 farT = centerT + extentT;
        const bool upcoming = (nearT > 0.0f) && (nearT < lookahead);
        const bool alongside = !(nearT > 0.0f) && (farT > 0.0f);
        if (!upcoming && !alongside) {
            return;
        }
        dbg.inWindow++;
        // Question 2: the ahead cue's box — the course runs into it (ObstacleScan_CourseSpan,
        // the ahead cue's own test) and its entry is still ahead. Once the ship is level
        // with the entry the ahead cue lets the box go, so it is ours however close (a NaN
        // fails every test below). A terrain box is never claimed whole: its claim is
        // settled per sample inside the walk (ObstacleDirectionCue_TerrainBelow), so it
        // goes on to question 3 with boxOnCourse still set — the side half honors it,
        // the vertical half decides.
        f32 spanNear = 0.0f;
        f32 spanFar = 0.0f;
        const bool courseHits = ObstacleScan_CourseSpan(box, allRange, frame.fwd, margin, &spanNear, &spanFar);
        const bool boxOnCourse = courseHits && (spanNear > 0.0f);
        if (boxOnCourse && !box.polyHeightfield) {
            dbg.aheadClaimed++;
            return;
        }
        // Question 3: the part of the box inside the window and within the margin of the
        // course on one steering axis, measured along the other (SliceRange).
        const ObstacleSliceRange vertRange =
            ObstacleDirectionCue_SliceRange(box, frame.fwd, lookahead, frame.right, margin, frame.up);
        // Vertical half. For a terrain box "outside in Y" is the course's height above
        // the surface under it, not the box top, and a heightfield is only ever hit from
        // above, so its answer is always BELOW. It is walked when some of it lies within
        // the margin of the course laterally (the vertical slice is non-empty).
        ObstacleAxisAnswer vert;
        if (box.polyHeightfield) {
            if (vertRange.found && !terrainIsFloor) {
                // The walk's span: rails keeps the box's Z extent, all-range the stretch of
                // course over the footprint. Its grid is anchored where the ahead cue's walk
                // over this box starts, when it walks it (see TerrainBelow).
                f32 tLo;
                f32 tHi;
                bool walk;
                if (allRange) {
                    walk = ObstacleDirectionCue_FootprintSpan(box, frame.fwd, margin, lookahead, &tLo, &tHi);
                } else {
                    tLo = alongside ? 0.0f : nearT;
                    tHi = (farT < lookahead) ? farT : lookahead;
                    walk = true;
                }
                f32 anchor = tLo;
                f32 aheadNear = spanNear;
                f32 aheadFar = spanFar;
                if (courseHits && ObstacleScan_ClipToPolyRange(box, frame.origin, frame.fwd, &aheadNear, &aheadFar) &&
                    (aheadFar > 0.0f)) {
                    anchor = (aheadNear > 0.0f) ? aheadNear : 0.0f;
                }
                if (walk) {
                    dbg.terrainWalkTested++;
                    const ObstacleTerrainBelow terrain =
                        ObstacleDirectionCue_TerrainBelow(box, frame, tLo, tHi, anchor, margin, &dbg.terrainWalkProbes);
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
            }
        } else {
            vert = ObstacleDirectionCue_Classify(vertRange, vertDist, OBSTACLE_DIR_ABOVE, OBSTACLE_DIR_BELOW);
            vert.upcoming = upcoming;
        }
        // Side half. The !boxOnCourse only bites for a terrain box (a solid one returned
        // above): its box claim still holds here, so a hill on course never sounds beside.
        ObstacleAxisAnswer side;
        if (!boxOnCourse) {
            const ObstacleSliceRange sideRange =
                ObstacleDirectionCue_SliceRange(box, frame.fwd, lookahead, frame.up, margin, frame.right);
            side = ObstacleDirectionCue_Classify(sideRange, sideDist, OBSTACLE_DIR_RIGHT, OBSTACLE_DIR_LEFT);
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
            w.gap = nearT;
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
