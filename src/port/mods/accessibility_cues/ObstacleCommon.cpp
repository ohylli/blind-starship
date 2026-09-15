#include "ObstacleCommon.h"

#include <libultraship/libultraship.h>

void ObstacleCommon_RegisterCVars() {
    CVarRegisterInteger(kObstacleCueEnabledCVar, 1);
    CVarRegisterFloat(kObstacleCueMarginCVar, kObstacleCueMarginDefault);
}

bool ObstacleCommon_IsEnabled() {
    return CVarGetInteger(kObstacleCueEnabledCVar, 1) == 1;
}

float ObstacleCommon_Margin() {
    float margin = CVarGetFloat(kObstacleCueMarginCVar, kObstacleCueMarginDefault);
    if (!(margin >= 0.0f)) {
        margin = kObstacleCueMarginDefault; // negative or NaN
    }
    return margin;
}
