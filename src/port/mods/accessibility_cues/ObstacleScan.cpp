#include "ObstacleScan.h"

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
