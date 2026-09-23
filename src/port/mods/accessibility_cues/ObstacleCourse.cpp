#include "ObstacleCourse.h"

#include <math.h>

#include "port/PlayerAim.h"

bool ObstacleCourse_Frame(const Player& player, bool allRange, ObstacleCourseFrame* frame) {
    frame->origin = { player.pos.x, player.pos.y, player.trueZpos };
    if (!allRange) {
        frame->fwd = { 0.0f, 0.0f, -1.0f };
        frame->right = { 1.0f, 0.0f, 0.0f };
        frame->up = { 0.0f, 1.0f, 0.0f };
        return true;
    }
    if (!Player_AimAnglesValid(player)) {
        return false;
    }
    Player_AimBasis(player, &frame->fwd, &frame->right, &frame->up);
    return true;
}

bool ObstacleCourse_ClipToBox(const f32* d, const f32* h, const f32* dir, int n, f32* lo, f32* hi) {
    // The callers pass unit vectors (or their components), so this is a direction-cosine
    // floor, not a distance.
    constexpr f32 kParallelEps = 1e-6f;
    for (int i = 0; i < n; i++) {
        if (fabsf(dir[i]) < kParallelEps) {
            if (!(fabsf(d[i]) <= h[i])) {
                return false; // parallel to this slab pair and outside it (or NaN)
            }
            continue;
        }
        f32 s1 = (d[i] - h[i]) / dir[i];
        f32 s2 = (d[i] + h[i]) / dir[i];
        if (s1 > s2) {
            const f32 tmp = s1;
            s1 = s2;
            s2 = tmp;
        }
        if (s1 > *lo) {
            *lo = s1;
        }
        if (s2 < *hi) {
            *hi = s2;
        }
    }
    return *lo <= *hi; // false for NaN too
}

// The span [tNear, tFar] along the unit `fwd` ray from the ship through the box expanded
// by `margin` on every axis. fwd is unit-length, so at least one axis divides and both
// bounds end up finite.
static bool ObstacleCourse_RaySpan(const ObstacleBox& box, const Vec3f& fwd, f32 margin, f32* tNear, f32* tFar) {
    const f32 d[3] = { box.dx, box.dy, box.dz };
    const f32 h[3] = { box.half.x + margin, box.half.y + margin, box.half.z + margin };
    const f32 f[3] = { fwd.x, fwd.y, fwd.z };
    *tNear = -INFINITY;
    *tFar = INFINITY;
    return ObstacleCourse_ClipToBox(d, h, f, 3, tNear, tFar);
}

// The course test proper: the span [tNear, tFar] of the course's passage (distances
// along the unit `course` ray from the ship's center) through the box widened by
// `margin`; false when the course misses it. Either bound may be negative (entry behind
// or inside — the caller's call). Per mode:
//   - Rails (`allRange` false; the course is the fixed -z track and `course` is unused):
//     on course when the ship's (x, y) is inside the footprint widened by the margin on
//     both lateral axes; the span is the box's own Z extent, gapZ to its far face (no
//     margin along the track).
//   - All-range: a ray along `course` (the aim heading, Player_AimForward) through the
//     box widened by the margin on every axis — in all-range "lateral" is not
//     axis-aligned, and the extra margin along the ray is noise against the warn band.
static bool ObstacleCourse_Span(const ObstacleBox& box, bool allRange, const Vec3f& course, f32 margin, f32* tNear,
                                f32* tFar) {
    if (allRange) {
        return ObstacleCourse_RaySpan(box, course, margin, tNear, tFar);
    }
    if ((box.clearX >= margin) || (box.clearY >= margin)) {
        return false; // a NaN clearance fails here too
    }
    *tNear = box.gapZ;
    *tFar = box.gapZ + 2.0f * box.half.z;
    return true;
}

// The engine's XZ range gate (box.polyRangeXZ, Object_GetPolyCollisionRangeXZ) applied
// along the course. Player_CollisionCheck tests a scenery mesh only while the SHIP is
// within that distance of obj.pos, so along the course the mesh can only be hit on the
// stretch where the ship will be inside that circle: this clips [tNear, tFar] (course
// distances from `origin`, the ship) to it — a ray/circle intersection in the XZ plane —
// and returns false when nothing of the span survives. Ungated boxes pass through.
// Gating along the course rather than at the ship's current position is what a warning
// needs: a big mesh's far corners the engine never tests never warn, and a solid mesh
// the ship enters outside the circle keeps warning until the ship reaches the stretch
// where the engine's own test takes over.
static bool ObstacleCourse_ClipToPolyRange(const ObstacleBox& box, const Vec3f& origin, const Vec3f& course, f32* tNear,
                                           f32* tFar) {
    if (!(box.polyRangeXZ > 0.0f)) {
        return true;
    }
    const f32 ox = origin.x - box.objPos.x;
    const f32 oz = origin.z - box.objPos.z;
    // |o + d t|^2 = R^2 in XZ, i.e. a t^2 + 2 b t + c = 0.
    const f32 a = course.x * course.x + course.z * course.z;
    const f32 b = ox * course.x + oz * course.z;
    const f32 c = ox * ox + oz * oz - box.polyRangeXZ * box.polyRangeXZ;
    constexpr f32 kVerticalEps = 1e-6f; // course is unit-length: a is the squared horizontal cosine
    if (a < kVerticalEps) {
        return c < 0.0f; // straight up or down: the whole span is in or out with the ship
    }
    const f32 disc = b * b - a * c;
    if (!(disc >= 0.0f)) {
        return false; // the course never enters the circle (or NaN)
    }
    const f32 sq = sqrtf(disc);
    const f32 tIn = (-b - sq) / a;
    const f32 tOut = (-b + sq) / a;
    if (tIn > *tNear) {
        *tNear = tIn;
    }
    if (tOut < *tFar) {
        *tFar = tOut;
    }
    return *tNear <= *tFar;
}

// The span the ahead cue tests a box on: the course span, clipped to the engine's range
// gate when the box is a poly mesh of either family.
static bool ObstacleCourse_AheadSpan(const ObstacleBox& box, const ObstacleCourseFrame& frame, bool allRange,
                                     f32 margin, f32* tNear, f32* tFar) {
    if (!ObstacleCourse_Span(box, allRange, frame.fwd, margin, tNear, tFar)) {
        return false;
    }
    if ((box.record == kObstaclePolyRecord) &&
        !ObstacleCourse_ClipToPolyRange(box, frame.origin, frame.fwd, tNear, tFar)) {
        return false;
    }
    return true;
}

bool ObstacleCourse_AheadClaimsSolid(const ObstacleBox& box, const ObstacleCourseFrame& frame, bool allRange,
                                     f32 margin, f32 warnDist, f32* gap) {
    f32 tNear;
    f32 tFar;
    if (!ObstacleCourse_AheadSpan(box, frame, allRange, margin, &tNear, &tFar)) {
        return false;
    }
    // The entry still ahead and inside the warn band. tNear <= 0 is the ship level with
    // the near face (or, in all-range, already inside the expanded box).
    if (!(tNear > 0.0f) || (tNear >= warnDist)) {
        return false;
    }
    *gap = tNear;
    return true;
}

bool ObstacleCourse_HeightfieldWalkSpan(const ObstacleBox& box, const ObstacleCourseFrame& frame, bool allRange,
                                        f32 margin, f32 warnDist, f32* tStart, f32* tEnd) {
    f32 tNear;
    f32 tFar;
    if (!ObstacleCourse_AheadSpan(box, frame, allRange, margin, &tNear, &tFar)) {
        return false;
    }
    if (!(tFar > 0.0f) || !(tNear < warnDist)) {
        return false;
    }
    *tStart = (tNear > 0.0f) ? tNear : 0.0f;
    *tEnd = (tFar < warnDist) ? tFar : warnDist;
    return true;
}
