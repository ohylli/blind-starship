#include "ObstacleScan.h"

const char* ObstacleScan_ArrayName(ObstacleArray array) {
    switch (array) {
        case OBSTACLE_ARRAY_SCENERY:
            return "scenery";
        case OBSTACLE_ARRAY_ACTOR:
            return "actor";
        case OBSTACLE_ARRAY_BOSS:
            return "boss";
        case OBSTACLE_ARRAY_SCENERY360:
            return "scenery360";
        default:
            return "?";
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
