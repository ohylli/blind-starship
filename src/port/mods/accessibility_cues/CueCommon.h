#pragma once

// Shared policy helpers for the Star Fox cue listeners. The cues themselves (sound file,
// volume CVar, preview, lazy loading, voice pools) live in the game-agnostic Cue class
// over the Cue3D HRTF seam; the files in this directory are the Star Fox side — one file
// per cue (RingCue, EnemyCue, AimCue), each registering its cue and deciding, per game
// tick, what it targets, coordinated by ../AccessibilityCues.cpp. This header holds what
// the cue files share: the master toggle, the height->pitch mapping and its knobs, and
// small scalar helpers. The game-coupled shared enemy scan lives in CueScan.h. The old
// SF64-audio-engine cue path was removed once the HRTF backend proved out: the game
// engine's camera-relative panning and per-frame SFX lifetime made it a dead end for
// continuous navigation cues (see docs/accessibility-hrtf-cues.md), so game SFX are
// reserved for future one-shot flourishes, not primary cues.
//
// The cue listeners gate on Accessibility_PlayerHasControl() (shared predicate,
// Accessibility.cpp). The 3D cue backend runs its own OS audio device that the
// game's pause/cutscene handling never reaches, so a looping Cue3D source keeps
// sounding until we stop it by hand — the listeners treat "no control" as the
// signal to stop, and the first in-control tick re-acquires targets and restarts.
//
// ===== Last-tick policy debug state (shared discipline) =====
//
// Each cue header declares a <X>CueDebug struct and a <X>Cue_DebugState() accessor — a
// mirror of what that cue's listener decided on its most recent GamePostUpdateEvent tick,
// read by the debug server's `cues` command (it merges this "why" layer with the Cue
// layer's per-voice "what" from Cue::Snapshot). Each listener resets its section at the
// top of its tick and fills it in place through a reference, so every exit path publishes
// automatically — gated ticks record the gate terms with active=false. In-place writes
// are safe because the reader (the debug server's dispatch) runs on the same game thread,
// between ticks: a partially written section is never observable. `active` uniformly
// means "the cue was driven this tick". Plain scalars only, never an Actor*/Item*: a
// section read after the level ends must not dangle. frame == -1 means the section has
// never been written.

// The master cue toggle, named here so every reader (the listeners, the F1 widgets in
// ImguiUI.cpp, the debug server's `cues` dump) shares one spelling.
inline constexpr const char* kAudioCuesEnabledCVar = "gAccessibilityAudioCues";

// Height->pitch cue knobs (Developer -> Blind Starship). The toggle gates the whole
// effect; the two floats are the sensitivity divisor (world height per octave) and the
// max pitch deviation (octaves at the height extremes). Shared by the mapping in
// CueCommon.cpp and the F1 sliders in ImguiUI.cpp. Defaults reproduce the old
// hard-coded mapping exactly.
inline constexpr const char* kCuePitchForHeightCVar = "gAccessibilityCuePitchForHeight";
inline constexpr const char* kCuePitchScaleCVar = "gAccessibilityCuePitchScale"; // world units per octave
inline constexpr const char* kCuePitchRangeOctavesCVar =
    "gAccessibilityCuePitchRangeOctaves"; // max octaves of bend
inline constexpr float kCuePitchScaleDefault = 1000.0f;
inline constexpr float kCuePitchRangeOctavesDefault = 1.0f;

// Registers the master toggle and the height->pitch knobs. Called once from
// AccessibilityCues_Init, before the per-cue Register functions.
void CueCommon_RegisterCVars();

// The master toggle, read fresh each tick so it can be flipped at runtime.
bool CueCommon_IsEnabled();

// Shared Y->pitch mapping: higher source = higher pitch, gated and tuned by the CVars
// above (see docs/accessibility-cues-tuning.md).
float CueCommon_ComputeFreqModFromY(float y);

// Turn a raw listener-relative offset (game convention: +x right, +y up, +z ahead) into
// what a cue voice is fed: the clamped source vector and the Y-derived pitch.
void CueCommon_ComputeCueTarget(float dx, float dy, float dz, float outSrc[3], float* outFreq);

// Clamp a normalized signal to [-1, 1]; NaN pins to center (0).
float CueCommon_ClampUnit(float v);
