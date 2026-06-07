#include "Accessibility.h"

#include "port/CGameCompat.h"
#include "port/accessibility/Tts.h"
#include "port/accessibility/Cue3D.h"
#include "port/accessibility/SpatialAudioTest.h"
#include "accessibility_screens/AccessibilityScreens.h"
#include "AccessibilityCues.h"
#include "AccessibilityTrainingMinimal.h"
#include "ObjectSpawnLog.h"

void Accessibility_Init() {
    CVarRegisterInteger("gAccessibilityScreenReader", 1);

    // Opt-in Cue3D smoke test (orbiting tone over the Steam Audio backend). Off by
    // default so the second OS audio device only opens when explicitly requested.
    // Toggling requires a restart for now.
    CVarRegisterInteger("gAccessibilitySpatialTest", 0);
    if (CVarGetInteger("gAccessibilitySpatialTest", 0)) {
        SpatialAudioTest_Start();
    }

    AccessibilityTitleScreen_Register();
    AccessibilityMainMenu_Register();
    AccessibilitySoundMenu_Register();
    AccessibilityPauseMenu_Register();
    AccessibilityTrainingRings_Register();
    AccessibilityScore_Register();

    AccessibilityCues_Init();
    AccessibilityTrainingMinimal_Init();
    ObjectSpawnLog_Init();
}

void Accessibility_Exit() {
    AccessibilityCues_Exit();
    Cue3D_Shutdown();
}

bool Accessibility_IsScreenReaderEnabled() {
    return CVarGetInteger("gAccessibilityScreenReader", 1) == 1;
}
