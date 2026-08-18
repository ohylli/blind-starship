#pragma once

#include <stdint.h>

// Registry id of the obstacle-ahead cue, shared by the registration in
// ObstacleAheadCue.cpp and the debug server's policy-section match (DebugCommands.cpp).
// "ObstacleAhead", not "Obstacle": the id names the per-cue volume CVar
// (gAccessibilityCueVolume.<id>), and the planned directional siblings ("ObstacleSide",
// "ObstacleHeight") must not force a rename that would silently reset saved volumes.
inline constexpr const char* kObstacleAheadCueId = "ObstacleAhead";

// Master toggle for the OBSTACLE CUE FAMILY, not just this cue: the planned directional
// obstacle cues read the same CVar. When the first sibling lands, this constant moves to
// a scalar-only ObstacleCommon.h; the CVar name itself never changes, so saved configs
// and the F1 checkbox are unaffected. Per-cue toggles, if ever wanted, arrive as
// additions (e.g. gAccessibilityObstacleAheadCue), never renames.
inline constexpr const char* kObstacleCueEnabledCVar = "gAccessibilityObstacleCue";

// Tuning knobs: F1 sliders under Developer -> Blind Starship -> Obstacle warning, shared
// by the mapping code in ObstacleAheadCue.cpp, the sliders in ImguiUI.cpp, and the debug
// server's settings dump. Tuning guidance in docs/accessibility-cues-tuning.md.
// Naming: the knobs deliberately carry the family-level ObstacleCue prefix, not
// ObstacleAheadCue — WarnDist and MarginXY define the course band all obstacle cues
// would share (they move to ObstacleCommon.h with the enable CVar when a sibling lands),
// while Slow/Fast/Boost are this cue's cadence and timbre: siblings add their own under
// the same gAccessibilityObstacleCue... prefix rather than renaming these.
inline constexpr const char* kObstacleCueWarnDistCVar = "gAccessibilityObstacleCueWarnDist"; // world units
inline constexpr const char* kObstacleCueSlowCVar = "gAccessibilityObstacleCueSlowSec"; // interval at warn start
inline constexpr const char* kObstacleCueFastCVar = "gAccessibilityObstacleCueFastSec"; // interval at contact
inline constexpr const char* kObstacleCueMarginCVar = "gAccessibilityObstacleCueMarginXY"; // lateral safety margin
inline constexpr const char* kObstacleCueBoostCVar = "gAccessibilityObstacleCueBoost"; // loudness vs the other cues

// Warn-start gap to the obstacle's near Z face. On rails the streaming loop spawns an
// object at a per-object distance ahead: Object_LoadLevelObjects (fox_enmy.c) fires at
// the object's zPos1 trigger, and the Load functions place it 3000 - zPos2 world units
// ahead of the player. zPos2 = 0 gives exactly 3000, and some levels (Bolse, Fortuna,
// Sector Z, Venom 2) use that for their whole roster; most obstacles elsewhere carry a
// negative zPos2 and exist 4000-9000+ out when the player is 3000 away (median ~7000
// on Corneria/Training/Venom 1, per the level manifests). A small tail spawns closer
// (Zoness at 1500, Macbeth's falling boulders, terrain-emergers like Titania's
// crawlers) and onsets mid-ramp at ANY warn distance. 3000 is thus the largest value
// honest on every level; the default deliberately sits above it (~3.4 s at cruise
// speed) because reaction time won by ear beat ramp purity — on the zPos2 = 0 levels
// the buzz starts partway up the ramp the moment the object streams in. The F1 slider
// allows up to 6000 for the levels that spawn farther. None of this streaming honesty
// applies in all-range mode: the whole arena loads at once, so every warn distance is
// honest there and the ramp always starts at its top.
inline constexpr float kObstacleCueWarnDistDefault = 4000.0f;
inline constexpr float kObstacleCueSlowDefault = 0.8f;
inline constexpr float kObstacleCueFastDefault = 0.04f;
// Floor for the fast-interval slider AND the length budget of the synthesized buzz:
// the whole buffer must finish inside the fastest interval or every pulse truncates at
// the restart. A static_assert in ObstacleAheadCue_GenerateBuzz enforces the budget at
// compile time, so retuning the timbre longer without moving this fails the build
// instead of clicking by ear.
inline constexpr float kObstacleCueMinIntervalSec = 0.04f;
// Lateral/vertical slack added around the hitbox footprint before asking "am I on a
// collision course". Covers the Arwing's span (the test uses the ship's center point;
// the engine collides four body/wing points, wings at roughly +/- 40 units) plus the
// drift a player accumulates while reacting.
inline constexpr float kObstacleCueMarginDefault = 150.0f;
// The buzz is longer and lower than the aim click, so it reads louder at equal sample
// level; start below the click's 2.0 boost.
inline constexpr float kObstacleCueBoostDefault = 1.5f;

// Last-tick policy mirror for the debug server's `cues` command — shared discipline
// documented in CueCommon.h. Plain scalars only.
struct ObstacleAheadCueTargetDebug {
    int32_t array = -1; // ObstacleArray; the debug server resolves the name
    int32_t slot = -1, objId = -1, record = -1;
    float gap = 0;                // the course gap driving the interval: gapZ on rails,
                                  // the ray's near-face distance in all-range
    float gapZ = 0;               // raw distance to the near z face, world units
    float clearX = 0, clearY = 0; // footprint clearance (negative = inside)
    float dx = 0, dy = 0, dz = 0; // signed center offsets, the directional siblings' input
    float halfX = 0, halfY = 0, halfZ = 0;
};

struct ObstacleAheadCueDebug {
    int32_t frame = -1;
    bool active = false;  // an on-course obstacle was found and the buzz was driven
    bool scanned = false; // the scan ran; scanned && !active means nothing on course
    bool enabled = false, obstacleEnabled = false, control = false;
    bool modeOk = false, allRange = false; // CueScan_ModeInScope result + mode flag
    int32_t scanActive = 0, scanObstacles = 0, scanBoxes = 0; // ObstacleScanStats
    int32_t onCourse = 0;           // boxes that passed the course test this tick
    float warnDist = 0, margin = 0; // effective (post-guard) knob values used this tick
    float fwdX = 0, fwdY = 0, fwdZ = 0; // unit heading the all-range ray test cast along;
                                        // zero on rails (the fixed -z test needs none)
    float intervalSec = 0;          // repeat interval pushed this tick
    ObstacleAheadCueTargetDebug target; // the winning (nearest) box; valid when active
};

const ObstacleAheadCueDebug& ObstacleAheadCue_DebugState();

// Registers the cue, its CVars, and its listener; called once from AccessibilityCues_Init.
void ObstacleAheadCue_Register();
