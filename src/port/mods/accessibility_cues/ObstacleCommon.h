#pragma once

// What the obstacle cue FAMILY shares: the family toggle, the safety margin that defines
// the collision-course band every obstacle cue reasons from, and the margin read that
// sanitizes it. Scalar-only on purpose (no game headers), so the F1 widgets in
// ImguiUI.cpp and the debug server's settings dump can include it as cheaply as
// CueCommon.h. The family today: the obstacle-ahead cue (ObstacleAheadCue.h — "how
// soon", a centered buzz whose pulse rate is the signal) and the three directional cues
// (ObstacleDirectionCue.h — "which way is closed": beside, above, below). Each cue's own
// knobs stay in its own header under the same gAccessibilityObstacleCue... prefix.

// Master toggle for the whole obstacle cue family (int CVar, default 1; F1 -> Settings
// -> Blind Starship -> "Obstacle warning"). Every obstacle cue reads this one CVar; the
// name predates the directional cues and never changes, so saved configs and the F1
// checkbox are unaffected by new family members. Per-cue toggles, if ever wanted, arrive
// as additions (e.g. gAccessibilityObstacleAheadCue), never renames.
inline constexpr const char* kObstacleCueEnabledCVar = "gAccessibilityObstacleCue";

// Safety margin (float CVar, world units): slack added around a hitbox footprint before
// asking "am I on a collision course". It covers the Arwing's span (the course tests use
// the ship's center point; the engine collides four body/wing points, wings at roughly
// +/- 40 units) plus the drift a player accumulates while reacting. It is also the
// boundary between the family members: a box the ship's position is inside by less than
// the margin on both lateral axes is the AHEAD cue's; a box outside the margin on
// exactly one axis is a directional cue's, and its clearance beyond the margin is that
// cue's signal. For heightfield terrain the ahead cue reads the same value as the
// vertical clearance that counts as a hit (ObstacleAheadCue_HeightfieldGap).
inline constexpr const char* kObstacleCueMarginCVar = "gAccessibilityObstacleCueMarginXY";
inline constexpr float kObstacleCueMarginDefault = 150.0f;

// Registers the family toggle and the margin. Called once from AccessibilityCues_Init,
// before the per-cue Register functions (which register their own knobs).
void ObstacleCommon_RegisterCVars();

// The family toggle, read fresh each tick so it can be flipped at runtime.
bool ObstacleCommon_IsEnabled();

// The margin CVar, sanitized: negative or NaN falls back to the default. Every family
// member reads it through here so the fallback can never fork.
float ObstacleCommon_Margin();
