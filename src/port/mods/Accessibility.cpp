#include "Accessibility.h"

#include "port/CGameCompat.h"
#include "port/accessibility/Tts.h"
#include "accessibility_screens/AccessibilityScreens.h"
#include "AccessibilityCues.h"
#include "AccessibilityTrainingMinimal.h"
#include "ObjectSpawnLog.h"

void Accessibility_Init() {
    CVarRegisterInteger("gAccessibilityScreenReader", 1);

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
}

bool Accessibility_IsScreenReaderEnabled() {
    return CVarGetInteger("gAccessibilityScreenReader", 1) == 1;
}
