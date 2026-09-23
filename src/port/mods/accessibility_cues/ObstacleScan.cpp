#include "ObstacleScan.h"

#include <math.h>

const char* ObstacleScan_ArrayName(ObstacleArray array) {
    switch (array) {
        case OBSTACLE_ARRAY_SCENERY:
            return "gScenery";
        case OBSTACLE_ARRAY_ACTOR:
            return "gActors";
        case OBSTACLE_ARRAY_BOSS:
            return "gBosses";
        case OBSTACLE_ARRAY_SCENERY360:
            return "gScenery360";
        default:
            return "unknown";
    }
}

const char* ObstacleScan_RecordKind(s32 record) {
    if (record == kObstacleSphereRecord) {
        return "sphere";
    }
    if (record == kObstaclePolyRecord) {
        return "poly";
    }
    return (record >= 0) ? "hitbox" : "unknown";
}

// All-range course test: the span [tNear, tFar] along the unit `fwd` ray from the
// player through the box expanded by `margin`. Standard ray-vs-AABB slab test, with an
// explicit near-parallel branch instead of the branchless min/max form — IEEE 0 * inf
// would seed NaNs there, and the obstacle cues' policy is explicit guards over
// NaN-propagating arithmetic. The margin expands every axis uniformly (see the header).
static bool ObstacleScan_RaySpan(const ObstacleBox& box, const Vec3f& fwd, f32 margin, f32* tNearOut, f32* tFarOut) {
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
                return false; // parallel to this slab pair and outside it
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
        return false; // the ray misses the box
    }
    *tNearOut = tNear;
    *tFarOut = tFar;
    return true;
}

bool ObstacleScan_CourseSpan(const ObstacleBox& box, bool allRange, const Vec3f& course, f32 margin, f32* tNear,
                             f32* tFar) {
    if (allRange) {
        return ObstacleScan_RaySpan(box, course, margin, tNear, tFar);
    }
    if ((box.clearX >= margin) || (box.clearY >= margin)) {
        return false; // a NaN clearance fails here too
    }
    *tNear = box.gapZ;
    *tFar = box.gapZ + 2.0f * box.half.z;
    return true;
}

bool ObstacleScan_ClipToPolyRange(const ObstacleBox& box, const Vec3f& origin, const Vec3f& course, f32* tNear,
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
