#pragma once

#include <stdint.h>

#include "ObstacleCommon.h" // the family toggle + safety margin every obstacle cue shares

// The three directional obstacle cues — beside (left/right), above, below — are three
// registered cues driven by ONE policy in ObstacleDirectionCue.cpp: the same three
// questions asked of every scanned box with the axes swapped (see the .cpp's file
// comment). That is why they share a file pair rather than getting one each: the rule
// must never fork between the vertical pair and the side pair. Three registry ids so
// each has its own volume slider and preview; the ids name the per-cue volume CVars
// (gAccessibilityCueVolume.<id>) and are matched by the debug server's policy sections.
inline constexpr const char* kObstacleSideCueId = "ObstacleSide";
inline constexpr const char* kObstacleAboveCueId = "ObstacleAbove";
inline constexpr const char* kObstacleBelowCueId = "ObstacleBelow";

// Tuning knobs: F1 sliders under Developer -> Blind Starship -> Obstacle direction,
// shared by the mapping code, the sliders in ImguiUI.cpp, and the debug server's
// settings dump. All under the family's gAccessibilityObstacleCue... prefix (see
// ObstacleAheadCue.h for the naming rule) with a Dir infix marking them as this pair's:
// gAccessibilityObstacleCueDir<Knob>, constants kObstacleCueDir<Knob>. Tuning guidance:
// docs/accessibility-cues-tuning.md.

// How far ahead (world units) a box's near face may be for it to count as "about to be
// beside/above/below me". Deliberately much shorter than the ahead cue's warn distance:
// a wall alongside matters NOW, and every corridor wall droning from 4000 units out
// would bury the buzz. ~1 s of cruise flight (the ahead cue's 4000 is ~3.4 s). A box
// already alongside (near face behind the ship, far face still ahead) counts regardless.
inline constexpr const char* kObstacleCueDirLookaheadCVar = "gAccessibilityObstacleCueDirLookahead";
inline constexpr float kObstacleCueDirLookaheadDefault = 1200.0f;
inline constexpr float kObstacleCueDirLookaheadMax = 4000.0f;

// Side cue band: how far sideways (world units, clearance from the box face) a box may be
// and still sound. The pan runs over [margin, sideDist]: a box at the margin sits at the
// pan floor (almost centered — the closer, the nearer the center, the player's choice),
// one at sideDist is panned fully to its side. Must exceed the margin; the cue clamps.
// The Max values are the sliders' ceilings, applied to whatever a config stored.
inline constexpr const char* kObstacleCueDirSideDistCVar = "gAccessibilityObstacleCueDirSideDist";
inline constexpr float kObstacleCueDirSideDistDefault = 1000.0f;
inline constexpr float kObstacleCueDirSideDistMax = 2500.0f;
// Pan magnitude (0 = center, 1 = hard) of a box AT the margin. Non-zero so the closest
// possible wall never reaches dead center, where left and right would be
// indistinguishable at exactly the moment they matter most.
inline constexpr const char* kObstacleCueDirSidePanFloorCVar = "gAccessibilityObstacleCueDirSidePanFloor";
inline constexpr float kObstacleCueDirSidePanFloorDefault = 0.1f;

// Vertical cue band: same shape on the vertical steering axis (world Y on rails, the
// canopy direction in all-range). Loudness runs over [margin, vertDist]:
// full at the margin, the level floor at vertDist. Rails corridors are shorter than
// they are wide, so the default band is narrower than the side band.
inline constexpr const char* kObstacleCueDirVertDistCVar = "gAccessibilityObstacleCueDirVertDist";
inline constexpr float kObstacleCueDirVertDistDefault = 400.0f;
inline constexpr float kObstacleCueDirVertDistMax = 2500.0f;
// Level (0..1 of the slider) of a box AT vertDist, so the onset is audible rather than a
// fade-in from silence the player cannot place.
inline constexpr const char* kObstacleCueDirVertLevelFloorCVar = "gAccessibilityObstacleCueDirVertLevelFloor";
inline constexpr float kObstacleCueDirVertLevelFloorDefault = 0.15f;

// Loudness boost shared by the three chords (same timbre family, so one knob), applied
// under the volume sliders like the buzz's. Sustained sines read loud at equal sample
// level, so the default is unity.
inline constexpr const char* kObstacleCueDirBoostCVar = "gAccessibilityObstacleCueDirBoost";
inline constexpr float kObstacleCueDirBoostDefault = 1.0f;

// The four directions, in the order the debug mirror stores them.
enum ObstacleDirection {
    OBSTACLE_DIR_LEFT = 0,
    OBSTACLE_DIR_RIGHT,
    OBSTACLE_DIR_ABOVE,
    OBSTACLE_DIR_BELOW,
    OBSTACLE_DIR_COUNT,
};
const char* ObstacleDirection_Name(ObstacleDirection dir); // "left" / "right" / "above" / "below"

// Last-tick policy mirror for the debug server's `cues` command — shared discipline
// documented in CueCommon.h. Plain scalars only. One section per direction; the debug
// server reports left+right under the side cue and the other two under their own cue.
struct ObstacleDirectionTargetDebug {
    bool active = false;    // a box won this direction and its voice was driven
    int32_t candidates = 0; // boxes classified into this direction this tick
    int32_t array = -1;     // ObstacleArray; the debug server resolves the name
    int32_t slot = -1, objId = -1, record = -1;
    bool heightfield = false;     // a heightfield mesh box; its answer always comes from a
                                  // terrain walk, and dx..halfZ describe its YAWED footprint
                                  // box (ObstacleScan_YawedFootprint), not the scan's
    bool fromTerrainWalk = false; // `clear` is from a terrain walk over the engine's surface:
                                  // below, the course's height above it
                                  // (ObstacleDirectionCue_TerrainBelow); beside, the
                                  // horizontal distance to where it rises within the margin
                                  // of the course (ObstacleDirectionCue_TerrainBeside)
    float surfaceY = 0;           // terrain walk: the surface height at the winning point (the
                                  // engine's crash threshold, Object_PolyHeightfieldSurfaceY)
    float sampleT = 0;            // terrain walk: course distance of the winning sample
                                  // (0 = beneath the ship)
    bool upcoming = false;        // near face still ahead (vs already alongside); for a
                                  // terrain walk winner, the winning sample is ahead (vs
                                  // beneath the ship)
    float clear = 0;              // the winning clearance on the cue's axis (> 0; under the
                                  // margin only for a box already alongside / the terrain
                                  // sample beneath the ship, pinned at the band's near end)
    float gap = 0;                // course distance to the box's nearest point along the
                                  // course (rails: the near z face, gapZ); <= 0 alongside.
                                  // Not the ahead cue's `gap`, which is the entry into the
                                  // MARGIN-WIDENED box; the two agree only for a solid box
                                  // on rails
    float dx = 0, dy = 0, dz = 0;
    float halfX = 0, halfY = 0, halfZ = 0;
    float pan = 0;   // side directions: pan magnitude pushed (0 center .. 1 hard)
    float level = 0; // vertical directions: CueTarget::level pushed (0..1)
};

struct ObstacleDirectionCueDebug {
    int32_t frame = -1;
    bool scanned = false; // the scan ran; every direction inactive means nothing qualified
    bool enabled = false, obstacleEnabled = false, control = false;
    bool modeOk = false, allRange = false; // CueScan_ModeInScope result + mode flag
    // All-range only gates (true = pass, and left true on rails): the craft flies by the
    // aim composition (Player_AimAnglesValid), and is not in a U-turn or somersault — a
    // scripted maneuver the player does not steer, which also swings the heading frame
    // (and flips it upside down) faster than a chord can say anything useful.
    bool aimValid = true, noManeuver = true;
    float fwdX = 0, fwdY = 0, fwdZ = 0;                       // the course the rule reasoned along (rails: -z)
    int32_t scanActive = 0, scanObstacles = 0, scanBoxes = 0; // ObstacleScanStats
    int32_t aheadClaimed = 0;    // solid boxes skipped whole because the ahead cue is warning
                                 // about them this tick (ObstacleCourse_AheadClaimsSolid: on
                                 // course, inside the range gate, entry ahead and inside the
                                 // warn band); a heightfield box is never claimed whole — its
                                 // claim is per sample, see terrainWalkClaimed
    int32_t inWindow = 0;        // boxes inside the lookahead window (upcoming or alongside)
    int32_t fightersSkipped = 0; // non-lockable fighter boxes left out (OBJ_ACTOR_ALLRANGE:
                                 // the all-range wingmates and allied craft — moving, so
                                 // never "closed space"; the ahead cue still warns of them)
    // The below cue's terrain walk: heightfield boxes walked, engine surface reads spent
    // on them (the cost line), and boxes with at least one upcoming sample left to the
    // ahead cue (surface within the margin of the course).
    int32_t terrainWalkTested = 0, terrainWalkProbes = 0, terrainWalkClaimed = 0;
    // The beside cue's sideways terrain walk: heightfield boxes walked and the engine
    // surface reads spent on them; boxes skipped whole because the ahead cue is buzzing
    // for the hill (ObstacleCourse_AheadHitsTerrain) and the probes that verdict cost.
    int32_t terrainSideTested = 0, terrainSideProbes = 0, terrainSideAheadBuzzing = 0, terrainSideAheadProbes = 0;
    float lookahead = 0, margin = 0, sideDist = 0, panFloor = 0, vertDist = 0,
          levelFloor = 0; // effective (post-guard) knob values used this tick
    float warnDist = 0;   // the ahead cue's band its verdict was asked with (ObstacleAheadCue_WarnDist)
    ObstacleDirectionTargetDebug dir[OBSTACLE_DIR_COUNT];
};

const ObstacleDirectionCueDebug& ObstacleDirectionCue_DebugState();

// Registers the three cues, their CVars, and the listener; called once from
// AccessibilityCues_Init.
void ObstacleDirectionCue_Register();
