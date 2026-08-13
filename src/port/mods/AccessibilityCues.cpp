#include "AccessibilityCues.h"

#include "accessibility_cues/CueCommon.h"
#include "accessibility_cues/RingCue.h"
#include "accessibility_cues/EnemyCue.h"
#include "accessibility_cues/AimCue.h"
#include "accessibility_cues/ObstacleAheadCue.h"

#include "port/CGameCompat.h" // Events.h pulls the C game headers; the shim must come first
#include "port/hooks/Events.h"
#include "port/accessibility/Cue.h"

// Per-game-tick cue housekeeping: expires timed previews (the settings UI's "Preview"
// buttons stay alive-and-bounded even if the menu closes mid-preview) and reaps keyed
// voices whose targets the driving listeners stopped refreshing.
static void AccessibilityCues_OnCueTick(IEvent* event) {
    (void) event;
    CueRegistry_Tick();
}

void AccessibilityCues_Init() {
    CueCommon_RegisterCVars();
    RingCue_Register();
    EnemyCue_Register();
    AimCue_Register();
    ObstacleAheadCue_Register();

    // Registered after the per-cue listeners (same event, same priority, so registration
    // order is execution order), matching the pre-split layout: the reap then acts on the
    // refreshes pushed this same tick. The Cue layer tolerates either order (see the
    // voice-steal comment in Cue.cpp), so this is about keeping the reap prompt, not
    // correctness.
    REGISTER_LISTENER(GamePostUpdateEvent, AccessibilityCues_OnCueTick, EVENT_PRIORITY_NORMAL);
}

void AccessibilityCues_Exit() {
    // Stops every cue and drops the backend handles; the caller's next step
    // (Cue3D_Shutdown in Accessibility_Exit) frees the underlying sources. Any
    // listener tick after this point sees idle cues with no handle and no-ops.
    CueRegistry_UnloadAll();
}
