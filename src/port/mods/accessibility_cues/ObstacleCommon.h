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
// boundary between the family members: a box the course runs into, widened by the
// margin (on rails the ship's position inside the footprint by less than the margin on
// both lateral axes; in all-range a ray along the heading through the widened box), is
// the AHEAD cue's while its entry is ahead and inside the warn band
// (ObstacleCourse_AheadClaimsSolid); a box outside the margin on exactly one steering
// axis is a directional cue's, and its clearance beyond the margin is that cue's
// signal. For heightfield terrain the ahead cue reads the same value as the
// vertical clearance that counts as a hit (ObstacleAheadCue_HeightfieldGap), and the
// below cue's terrain walk (ObstacleDirectionCue_TerrainBelow) uses it twice over: as the
// clearance under which a sample is the ahead cue's rather than its own, and as the
// offset of its lateral probes. Raising it therefore hands more of an approaching slope
// to the buzz and takes it from the below chord.
inline constexpr const char* kObstacleCueMarginCVar = "gAccessibilityObstacleCueMarginXY";
inline constexpr float kObstacleCueMarginDefault = 150.0f;

// The heightfield walks' sampling step and step cap, shared by the ahead cue's
// ObstacleAheadCue_HeightfieldGap and the below cue's ObstacleDirectionCue_TerrainBelow so
// the two sample one hill on the same grid — the below cue's per-sample claim is the ahead
// cue's own hit only while the samples coincide. Step: world units along the course, two
// to three play frames at cruise speed; a mesh triangle is hundreds of units across, so a
// finer step buys nothing while a much coarser one could step over a narrow ridge. Cap:
// steps per box per tick; each walk's static_assert keeps its longest real span under it,
// so the cap only ever guards a NaN or absurd input, not a mesh.
inline constexpr float kObstacleWalkStep = 100.0f;
inline constexpr int kObstacleWalkMaxSteps = 64;

// Registers the family toggle and the margin. Called once from AccessibilityCues_Init,
// before the per-cue Register functions (which register their own knobs).
void ObstacleCommon_RegisterCVars();

// The family toggle, read fresh each tick so it can be flipped at runtime.
bool ObstacleCommon_IsEnabled();

// The margin CVar, sanitized: negative or NaN falls back to the default. Every family
// member reads it through here so the fallback can never fork.
float ObstacleCommon_Margin();

// The step count for a walk over `span` world units: ceil(span / kObstacleWalkStep),
// capped at kObstacleWalkMaxSteps. A walk then runs i = 0..steps and clamps the last
// sample to the span's end. False for a NaN or negative span, decided as a float BEFORE
// the integer cast: converting a non-finite or out-of-range float to int is undefined,
// and the old "cast, then test the int" order only worked by what x86 and ARM happen to
// produce.
bool ObstacleCommon_WalkSteps(float span, int* steps);

// Is a heightfield mesh the FLOOR for the player's current vehicle rather than a crash?
// The engine seats the Landmaster and the on-foot form on the surface
// (Player_CheckPolyCollision's ground branch), where every other form takes damage. Every
// family member skips its terrain walk while this is true, so "terrain ahead / below"
// can never mean the ground being driven on. A forward guard today rather than an
// observed fix: no rails level pairs a ground form with a heightfield box (Macbeth's
// terrain bump is outside Object_GetPolyCollider's rails scenery dispatch, as the engine
// excludes it from Player_CollisionCheck's scenery loop), and on foot exists only in
// Versus, which the cues leave out of scope.
bool ObstacleCommon_TerrainIsFloor();
