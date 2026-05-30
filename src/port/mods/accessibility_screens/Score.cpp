#include "AccessibilityScreens.h"

#include <string>
#include <spdlog/fmt/fmt.h>

#include "port/CGameCompat.h"
#include "port/mods/Accessibility.h"
#include "port/accessibility/Tts.h"
#include "port/hooks/Events.h"

// Dedicated toggle (default on): announcing every kill can be too chatty for
// some players, so score speech can be silenced without disabling the reader.
static bool Accessibility_IsScoreAnnounceEnabled() {
    return CVarGetInteger("gAccessibilityScoreAnnounce", 1) == 1;
}

static void Accessibility_OnScoreChanged(IEvent* event) {
    if (!Accessibility_IsScreenReaderEnabled() || !Accessibility_IsScoreAnnounceEnabled()) {
        return;
    }
    ScoreChangedEvent* e = (ScoreChangedEvent*) event;
    // Interrupt: during a burst the producer already coalesced one frame's kills
    // into a single delta, so interrupting just keeps the freshest total current.
    Tts_Speak(fmt::format("+{} score, total {}", e->delta, e->total).c_str(), true);
}

void AccessibilityScore_Register() {
    CVarRegisterInteger("gAccessibilityScoreAnnounce", 1);
    REGISTER_LISTENER(ScoreChangedEvent, Accessibility_OnScoreChanged, EVENT_PRIORITY_NORMAL);
}
