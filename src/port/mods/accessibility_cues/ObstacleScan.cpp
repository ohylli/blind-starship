#include "ObstacleScan.h"

#include <math.h>

const char* ObstacleScan_ArrayName(ObstacleArray array) {
    switch (array) {
        case OBSTACLE_ARRAY_SCENERY:
            return "scenery";
        case OBSTACLE_ARRAY_ACTOR:
            return "actors";
        case OBSTACLE_ARRAY_BOSS:
            return "bosses";
        case OBSTACLE_ARRAY_SCENERY360:
            return "scenery360";
        default:
            return "unknown";
    }
}

const char* ObstacleScan_RecordKind(s32 record) {
    switch (record) {
        case kObstacleSphereRecord:
            return "sphere";
        case kObstaclePolyRecord:
            return "poly";
        default:
            break;
    }
    return (record >= 0) ? "hitbox" : "unknown";
}

ObstacleBox ObstacleScan_YawedFootprint(const ObstacleBox& box, const Vec3f& origin) {
    if (box.footprintYawed) {
        return box;
    }
    // The inverse of the probes' world -> mesh rotation (Object_PolyHeightfieldHit: local =
    // (cs * rx + sn * rz, -sn * rx + cs * rz) with sn/cs of -rot.y), applied to the box's
    // center offset; the extents become those of the turned rectangle's bounding box.
    const f32 sn = sinf(-box.rotY * M_DTOR);
    const f32 cs = cosf(-box.rotY * M_DTOR);
    const f32 lx = box.center.x - box.objPos.x;
    const f32 lz = box.center.z - box.objPos.z;
    ObstacleBox out = box;
    out.center.x = box.objPos.x + cs * lx - sn * lz;
    out.center.z = box.objPos.z + sn * lx + cs * lz;
    out.half.x = fabsf(cs) * box.half.x + fabsf(sn) * box.half.z;
    out.half.z = fabsf(sn) * box.half.x + fabsf(cs) * box.half.z;
    ObstacleScan_DeriveRelative(out, origin);
    out.footprintYawed = true;
    return out;
}
