#include "ObstacleCommon.h"
#include "CueCommon.h"

#include <libultraship/libultraship.h>

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
