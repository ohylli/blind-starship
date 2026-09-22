#pragma once

// Shared policy helpers for the Star Fox cue listeners. The cues themselves (sound file,
// volume CVar, preview, lazy loading, voice pools) live in the game-agnostic Cue class
// over the Cue3D HRTF seam; the files in this directory are the Star Fox side — one file
// per cue or cue family (RingCue, EnemyCue, AimCue, ObstacleAheadCue, ObstacleDirectionCue),
// each registering its cue(s) and deciding, per game tick, what it targets, coordinated by
// ../AccessibilityCues.cpp.
// This header holds what the cue files share: the master toggle, the height->pitch
// mapping and its knobs, the sanitized knob reads, voice placement on the unity-gain
// arc, the damped-tone generator, and small scalar helpers. The game-coupled shared
// scans live in CueScan.h (enemies) and ObstacleScan.h (obstacle hitbox boxes). The old
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

#include <math.h>
#include <vector>

struct CueTarget; // port/accessibility/Cue.h

// The master cue toggle, named here so every reader (the listeners, the F1 widgets in
// ImguiUI.cpp, the debug server's `cues` dump) shares one spelling.
inline constexpr const char* kAudioCuesEnabledCVar = "gAccessibilityAudioCues";

// Height->pitch cue knobs (Developer -> Blind Starship). The toggle gates the whole
// effect; the two floats are the sensitivity divisor (world height per octave) and the
// max pitch deviation (octaves at the height extremes). Shared by the mapping in
// CueCommon.cpp and the F1 sliders in ImguiUI.cpp. Defaults reproduce the old
// hard-coded mapping exactly.
inline constexpr const char* kCuePitchForHeightCVar = "gAccessibilityCuePitchForHeight";
inline constexpr const char* kCuePitchScaleCVar = "gAccessibilityCuePitchScale";               // world units per octave
inline constexpr const char* kCuePitchRangeOctavesCVar = "gAccessibilityCuePitchRangeOctaves"; // max octaves of bend
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

// ===== Knob reads =====
//
// Every float knob is read through one of these, never CVarGetFloat directly: a knob is
// reachable from the console, the debug server and a hand-edited config, so the stored
// value can be NaN, negative or absurd. The guards are written as !(x >= lo) rather than
// (x < lo) on purpose — every comparison against NaN is false, so the natural spelling
// would wave a NaN straight through, and a NaN knob once rode out as a NaN playback
// rate, an out-of-bounds read on the audio thread. The seam's setters validate too; this
// keeps the bad value from ever being built.

// A float knob that must be at least `lo` (NaN or below -> the default), clamped to `hi`
// (a hand-edited config or the debug server can store more than the slider allows).
float CueCommon_ReadFloat(const char* cvar, float def, float lo, float hi = INFINITY);

// A strictly positive float knob — a distance or divisor where zero is as bad as
// negative (zero, negative or NaN -> the default), clamped to `hi`.
float CueCommon_ReadPositiveFloat(const char* cvar, float def, float hi = INFINITY);

// ===== Placing a voice =====

// The backend's unity-gain radius, or a stand-in when no backend reports one (only the
// stub backend, where nothing plays). Every cue that does not encode distance places its
// voice EXACTLY here: PAN mode still applies the backend's distance attenuation (only
// DIRECT drops it), so a voice off the radius would turn distance into an unintended
// loudness signal.
float CueCommon_UnityRadius();

// Place a voice on the unity-gain arc at a stereo pan of `pan` (-1 hard left .. 1 hard
// right, clamped, NaN -> center). PAN mode derives its pan purely from the horizontal
// direction (x over the x/z length), so radius * (pan, 0, sqrt(1 - pan^2)) renders a
// constant-power pan of exactly `pan` with the attenuation pinned at unity; y is
// irrelevant to PAN and set to 0. DIRECT mode ignores the position entirely — cues in
// that mode place at pan 0 just to keep the `cues` voice dump readable. Only x/y/z are
// written; pitch, interval, low-pass and level stay the caller's.
void CueCommon_PlaceOnPanArc(CueTarget& target, float pan);

// Shared generator for the synthesized pulse cues (the aim click, the obstacle buzz):
// leadInSec of silence, then toneSec of a damped sine at toneHz (plus an optional second
// harmonic at harmonicMix, normalized so the peak stays at amplitude), with a
// raised-cosine tail of tailSec. Mono float PCM at sampleRate, for CueSpec::generator.
//
// Two constraints every caller inherits (they used to live duplicated in each cue's
// generator, and a fix to either must not fork again):
//   - The lead-in is load-bearing: the backend fades each interval restart in from
//     silence over ~5 ms, and these sounds carry their energy in the first few
//     milliseconds — without the lead-in the ramp eats the attack (~6 dB). It also
//     delays every pulse by that constant, inaudible for a cadence signal. (Under the
//     RESAMPLE pitch style the lead-in scales with pitch.)
//   - The whole buffer (leadInSec + toneSec) must finish inside the fastest interval the
//     cue's sliders allow, or pulses truncate at the restart; the tail pins the buffer
//     to zero so the restart's rewind cannot click.
std::vector<float> CueCommon_GenerateDampedTone(int sampleRate, float leadInSec, float toneSec, float toneHz,
                                                float harmonicMix, float decayPerSec, float amplitude, float tailSec);
