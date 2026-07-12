#include "Accessibility.h"

#include "port/CGameCompat.h"
#include "port/accessibility/Tts.h"
#include "port/accessibility/Cue3D.h"
#include "port/accessibility/CueBench.h"
#include "accessibility_screens/AccessibilityScreens.h"
#include "AccessibilityCues.h"
#include "AccessibilityTrainingMinimal.h"
#include "ObjectSpawnLog.h"

void Accessibility_Init() {
    CVarRegisterInteger("gAccessibilityScreenReader", 1);

    // Live Cue3D test bench (F1 -> Developer -> Blind Starship). Registers its listener and
    // hidden cue up front but stays silent until the bench is toggled on — no restart, and
    // the second OS audio device only opens the first time the bench is activated.
    CueBench_Init();

    AccessibilityTitleScreen_Register();
    AccessibilityMainMenu_Register();
    AccessibilitySoundMenu_Register();
    AccessibilityPauseMenu_Register();
    AccessibilityLevelSelector_Register();
    AccessibilityTrainingRings_Register();
    AccessibilityScore_Register();
    AccessibilityCameraView_Register();
    AccessibilityImGuiMenu_Register();

    AccessibilityCues_Init();
    AccessibilityTrainingMinimal_Init();
    ObjectSpawnLog_Init();
}

void Accessibility_Exit() {
    AccessibilityCues_Exit();
    CueBench_Shutdown();
    Cue3D_Shutdown();
}

bool Accessibility_IsScreenReaderEnabled() {
    return CVarGetInteger("gAccessibilityScreenReader", 1) == 1;
}
