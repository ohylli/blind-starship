#pragma once

#include <stdint.h>

// Registry id of the ring cue, shared by the registration in RingCue.cpp and the debug
// server's policy-section match (DebugCommands.cpp) so a rename cannot silently detach
// the cue from its policy dump.
inline constexpr const char* kRingCueId = "Ring";

// Last-tick policy mirror for the debug server's `cues` command — shared discipline
// documented in CueCommon.h.
struct RingCueDebug {
    int32_t frame = -1;
    bool active = false;
    bool enabled = false, inTraining = false, control = false; // gate terms
    int32_t itemIndex = -1;                                    // gItems slot of the tracked ring
    float dx = 0, dy = 0, dz = 0;                              // player-relative delta fed to the cue (pre-clamp)
    float src[3] = {};                                         // post-clamp target actually pushed
    float freq = 1;
};

const RingCueDebug& RingCue_DebugState();

// Registers the cue and its listener; called once from AccessibilityCues_Init.
void RingCue_Register();
