#include "AccessibilityScreens.h"

#include "port/CGameCompat.h"
#include "port/mods/Accessibility.h"
#include "port/accessibility/Tts.h"
#include "port/hooks/Events.h"

// Speaks the camera view the player just switched to with C-Up. The same
// alternateView toggle means different things per mode, so phrasing branches on
// gLevelMode: on rails it swaps the first-person cockpit for the external chase
// cam; in all-range (and the versus Landmaster path) it swaps the follow
// distance. Anything that isn't LEVELMODE_ON_RAILS is treated as the all-range
// far/near case, which also covers the scrapped turret mode harmlessly.
static const char* Accessibility_CameraViewLabel(s32 alternateView) {
    if (gLevelMode == LEVELMODE_ON_RAILS) {
        return alternateView ? "Cockpit view" : "External view";
    }
    return alternateView ? "Far view" : "Near view";
}

static void Accessibility_OnCameraViewChanged(IEvent* event) {
    // Gated by the master screen-reader toggle only: the player pressed the
    // button deliberately and always wants the confirmation, so this has no
    // separate chattiness mute (unlike the score/ring announcements).
    if (!Accessibility_IsScreenReaderEnabled()) {
        return;
    }
    CameraViewChangedEvent* e = (CameraViewChangedEvent*) event;
    // Interrupt: rapid toggling should land on the freshest view.
    Tts_Speak(Accessibility_CameraViewLabel(e->alternateView), true);
}

void AccessibilityCameraView_Register() {
    REGISTER_LISTENER(CameraViewChangedEvent, Accessibility_OnCameraViewChanged, EVENT_PRIORITY_NORMAL);
}
