#include "ObstacleScan.h"

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
