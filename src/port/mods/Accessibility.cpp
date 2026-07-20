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
    AccessibilityLevelMode_Register();
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

// True only while the player is actually flying the ship. Each clause has a case
// it alone catches:
//  - gGameState: the dev return-to-map shortcut (PortEnhancements.c) jumps to
//    GSTATE_MAP leaving gPlayer/gActors as stale-but-valid level memory, so
//    only the game state betrays that the level is gone.
//  - gPlayState: PLAY_PAUSE while paused (positions frozen), PLAY_INIT during
//    level setup.
//  - player state: cutscenes park the player outside PLAYERSTATE_ACTIVE —
//    LEVEL_INTRO on entry, STANDBY for mid-level scenes (Star Wolf entry in
//    fox_360.c, Katina's mothership in fox_ka.c), LEVEL_COMPLETE/DOWN/... for
//    victory and shot-down sequences. U_TURN is kept: it is player-initiated,
//    combat stays live through it (the game gates on ACTIVE || U_TURN all over).
// Also covers the gPlayer null guard: gPlayer is a pointer (sf64context.h:324),
// zero-initialized at process start and only allocated once a level loads.
bool Accessibility_PlayerHasControl() {
    return gGameState == GSTATE_PLAY && gPlayState == PLAY_UPDATE && gPlayer != NULL &&
           (gPlayer[0].state == PLAYERSTATE_ACTIVE || gPlayer[0].state == PLAYERSTATE_U_TURN);
}
