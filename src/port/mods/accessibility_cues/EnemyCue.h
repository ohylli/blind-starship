#pragma once

#include <stdint.h>

// Registry id of the enemy cue, shared by the registration in EnemyCue.cpp and the debug
// server's policy-section match (DebugCommands.cpp) so a rename cannot silently detach
// the cue from its policy dump.
inline constexpr const char* kEnemyCueId = "Enemy";

// The enemy-voice count CVar, named here so every reader (the listener, the F1 slider in
// ImguiUI.cpp, the debug server's `cues` dump) shares one spelling.
inline constexpr const char* kEnemyCueVoicesCVar = "gAccessibilityEnemyCueVoices";

// Upper bound on simultaneous enemy-cue voices — sizes the pool registered with the Cue
// layer and caps the kEnemyCueVoicesCVar CVar and its F1 slider (ImguiUI.cpp).
inline constexpr int kAccessibilityEnemyCueMaxVoices = 5;

// Value the gAccessibilityEnemyCueVoices CVar starts at, shared by the CVar registration
// and the F1 slider's reset-to-default.
inline constexpr int kAccessibilityEnemyCueDefaultVoices = kAccessibilityEnemyCueMaxVoices;

// The kEnemyCueVoicesCVar value clamped to [1, kAccessibilityEnemyCueMaxVoices] — the
// count the enemy listener actually uses. Shared with the debug server's `cues` dump so
// the clamp policy lives in one place.
int32_t EnemyCue_VoiceCount();

// Last-tick policy mirror for the debug server's `cues` command — shared discipline
// documented in CueCommon.h.
struct EnemyCueTargetDebug {
    int32_t slot = -1;                  // gActors index
    int32_t objId = -1, eventType = -1; // decoded identity (the key layout stays in the .cpp)
    uint64_t voiceKey = 0;              // join field against CueVoiceSnapshot::key
    float distance = 0;                 // 3D body-frame distance
    float bodyDelta[3] = {};            // pre-clamp body-frame delta (+X right, +Y up, -Z ahead)
    float src[3] = {};                  // post-clamp target actually pushed
    float freq = 1;
};

struct EnemyCueDebug {
    int32_t frame = -1;
    bool active = false;  // voices were driven this tick (count > 0)
    bool scanned = false; // the scan ran; scanned && !active means it found nothing
    bool enabled = false, modeOk = false, allRange = false, versus = false, control = false;
    int32_t scanActive = 0, scanCueable = 0, scanKept = 0; // CueScanStats
    int32_t requestedVoices = 0; // clamped gAccessibilityEnemyCueVoices used this tick
    int32_t count = 0;           // valid entries in targets[]
    EnemyCueTargetDebug targets[kAccessibilityEnemyCueMaxVoices];
};

const EnemyCueDebug& EnemyCue_DebugState();

// Registers the cue, its CVars, and its listener; called once from AccessibilityCues_Init.
void EnemyCue_Register();
