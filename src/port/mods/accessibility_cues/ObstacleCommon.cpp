#include "ObstacleCommon.h"
#include "CueCommon.h"

#include <libultraship/libultraship.h>
#include <math.h>

#include "port/CGameCompat.h"

void ObstacleCommon_RegisterCVars() {
    CVarRegisterInteger(kObstacleCueEnabledCVar, 1);
    CVarRegisterFloat(kObstacleCueMarginCVar, kObstacleCueMarginDefault);
}

bool ObstacleCommon_IsEnabled() {
    return CVarGetInteger(kObstacleCueEnabledCVar, 1) == 1;
}

float ObstacleCommon_Margin() {
    return CueCommon_ReadFloat(kObstacleCueMarginCVar, kObstacleCueMarginDefault, 0.0f);
}

bool ObstacleCommon_WalkSteps(float span, int* steps) {
    if (!(span >= 0.0f)) {
        return false; // NaN or a reversed span
    }
    *steps = kObstacleWalkMaxSteps;
    if (span < (float) kObstacleWalkMaxSteps * kObstacleWalkStep) {
        *steps = (int) ceilf(span / kObstacleWalkStep);
    }
    return true;
}

bool ObstacleCommon_TerrainIsFloor() {
    // Callers run behind Accessibility_PlayerHasControl, which null-checks gPlayer; this
    // repeats the check so the predicate is safe from anywhere.
    if (gPlayer == NULL) {
        return false;
    }
    const s32 form = gPlayer[0].form;
    return (form == FORM_LANDMASTER) || (form == FORM_ON_FOOT);
}
