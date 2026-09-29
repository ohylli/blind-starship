#pragma once

// The obstacle cue family's shared COURSE: the frame the family reasons in (where the ship
// is and which way it is going, plus the two directions the player can steer off it), the
// one ray-versus-box primitive every course test is built from, and the ahead cue's
// verdict on a box — shared, so the directional cues' "is this the ahead cue's box"
// question is answered by the very code the ahead cue runs, and the two halves of the
// family can never both claim a box or both drop it. ObstacleScan.h stays the box
// producer (raw geometry, no margin, no policy); ObstacleCommon.h stays scalar-only for
// the F1 widgets; this file is where the family's geometry policy lives, and is
// game-coupled by design (Player, ObstacleBox). The design records:
// docs/accessibility-obstacle-cue.md (the ahead cue's course test, range gate and
// heightfield walk) and docs/accessibility-obstacle-direction-cues.md (the heading frame).

#include "ObstacleScan.h"

#include "port/CGameCompat.h"

// The frame the family reasons in: the ship's center, the course, and the two directions
// the player can steer off it. On rails the fixed -z track with world +x (right) and +y
// (up); in all-range the aim heading's frame (Player_AimBasis — bank ignored, so "above"
// is toward the canopy and "beside" is what a turn would bring onto the course). All
// three are unit vectors and mutually orthogonal.
struct ObstacleCourseFrame {
    Vec3f origin;
    Vec3f fwd, right, up;
};

// Builds the frame for the player's current mode. `origin` is the ship's real world
// position (trueZpos, not the rails path scroll — sf64player.h). In all-range the course
// is the aim heading, which only the Arwing / Blue Marine composition yields
// (Player_AimAnglesValid); the forms that compose differently (Landmaster, on-foot — see
// PlayerAim.h) never appear in solo all-range, and this returns false rather than guess
// if one ever does, so every family member stops on the same gate (`aimValid` in the
// `cues` dump). Always true on rails. Callers run behind Accessibility_PlayerHasControl.
bool ObstacleCourse_Frame(const Player& player, bool allRange, ObstacleCourseFrame* frame);

inline f32 ObstacleCourse_Dot(const Vec3f& a, const Vec3f& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

// The one slab test every course test is built from: clips the parameter interval
// [*lo, *hi] of the line s -> s * dir to the box |s * dir[i] - d[i]| <= h[i] on each of
// the first `n` axes (d is the box center relative to the line's origin, h its
// half-extents; both non-negative h). False when nothing of the interval survives. An
// axis the line runs parallel to (|dir[i]| under a direction-cosine floor) is a plain
// containment test on that axis. Written as explicit branches rather than the branchless
// min/max form because IEEE 0 * inf would seed NaNs there, and the family's policy is
// explicit guards over NaN-propagating arithmetic: a NaN input fails the test.
bool ObstacleCourse_ClipToBox(const f32* d, const f32* h, const f32* dir, int n, f32* lo, f32* hi);

// The ahead cue's verdict on a SOLID box (every box but a heightfield's): is the ahead
// cue warning about it this tick? True when the course runs into the box widened by
// `margin` (the footprint test on rails, a ray along the heading in all-range), on the
// stretch the engine's own range gate lets it be hit (a poly mesh is only tested while
// the ship is within its XZ range of obj.pos), with the entry still ahead and inside the
// warn band; `*gap` is then that entry distance, the signal the ahead cue maps. Once the
// ship is level with the entry the engine's own collision has resolved the encounter and
// the ahead cue lets the box go, and beyond the band it has not started — either way the
// box is the directional cues' to classify, however close.
bool ObstacleCourse_AheadClaimsSolid(const ObstacleBox& box, const ObstacleCourseFrame& frame, bool allRange,
                                     f32 margin, f32 warnDist, f32* gap);

// The ahead cue's walk over a terrain box (box.polyHeightfield), planned: the stretch
// [start, end] of the course it samples when `walks`, and the warn distance at which it
// drops a hit. One plan per box per tick, shared by the ahead cue's verdict
// (ObstacleCourse_AheadHitsTerrain), the below walk's per-sample claim and grid anchor,
// and the beside walk's grid (ObstacleDirectionCue_TerrainBelow / _TerrainBeside).
struct ObstacleAheadWalk {
    bool walks = false;
    f32 start = 0.0f;
    f32 end = 0.0f;
    f32 warnDist = 0.0f;
};

// Plans the ahead cue's walk over a terrain box: the course span through the
// margin-widened YAWED footprint box (ObstacleScan_YawedFootprint — applied here, and a
// no-op for a box the caller already yawed), so a turned hill's corners are walked;
// clipped to the engine's range gate, started AT THE SHIP when the ship is already inside
// (a bump's box is up to 2600 units deep and the slope may still rise ahead — unlike a
// solid box, being past the near face does not hand the encounter to the engine) and
// stopped at the warn band's edge. `walks` false when the ahead cue does not walk the box
// at all: the course misses it, its far face is behind the ship, or its near face is
// beyond the band. The below cue anchors its own walk's sampling grid at `start` so the
// two walks read the same points wherever they overlap, which is what makes its
// per-sample claim the ahead cue's own hit.
ObstacleAheadWalk ObstacleCourse_PlanAheadWalk(const ObstacleBox& box, const ObstacleCourseFrame& frame, bool allRange,
                                               f32 margin, f32 warnDist);

// The ahead cue's verdict on a TERRAIN box (box.polyHeightfield): is it warning about it
// this tick? The course is walked through the planned stretch (ObstacleCourse_PlanAheadWalk
// over the same box, frame and margin) and the engine's own surface test decides whether
// the course meets the slope (probing the bottom edge of the margin square around the
// course; see the .cpp). True with `*gap` the course distance of the first hit, inside the
// warn band; false without walking when the plan does not walk. `*probes` accumulates the
// engine calls spent. The caller applies the ground-vehicle gate
// (ObstacleCommon_TerrainIsFloor) first. Shared so the beside cue stays silent for exactly
// the hills the buzz is sounding for (ObstacleDirectionCue_TerrainSide).
bool ObstacleCourse_AheadHitsTerrain(const ObstacleBox& box, const ObstacleCourseFrame& frame,
                                     const ObstacleAheadWalk& plan, f32 margin, f32* gap, int32_t* probes);
