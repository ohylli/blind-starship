#include "Accessibility.h"

#include "global.h"
#include "port/accessibility/Tts.h"
#include "accessibility_screens/AccessibilityScreens.h"
#include "AccessibilityCues.h"
#include "AccessibilityTrainingMinimal.h"
#include "ObjectSpawnLog.h"

void Accessibility_Init(void) {
    CVarRegisterInteger("gAccessibilityScreenReader", 1);

    AccessibilityTitleScreen_Register();
    AccessibilityMainMenu_Register();
    AccessibilitySoundMenu_Register();

    AccessibilityCues_Init();
    AccessibilityTrainingMinimal_Init();
    ObjectSpawnLog_Init();
}

void Accessibility_Exit(void) {
    AccessibilityCues_Exit();
}

bool Accessibility_IsScreenReaderEnabled(void) {
    return CVarGetInteger("gAccessibilityScreenReader", 1) == 1;
}
