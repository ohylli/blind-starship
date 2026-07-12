#pragma once

// Live Cue3D test bench for the F1 developer menu. Exercises every capability of the
// accessibility audio stack by ear, with no restart: the raw Cue3D seam (Cue3D.h) drives a
// pair of continuous sources (render mode, pitch, interval, low-pass, orbit, start/stop
// ramps), and a hidden one-shot Cue (Cue.h) drives the PlayOnce / generator path. Every
// control is a CVar so the whole bench is live-editable. Supersedes the throwaway
// SpatialAudioTest smoke test: the orbit is now one bench control and the
// restart-required wart is gone.
//
// Wiring: CueBench_Init() from Accessibility_Init registers a GamePostUpdateEvent listener
// unconditionally (the listener early-returns cheaply while the bench is off, the pattern
// every accessibility listener uses) and the hidden one-shot cue. CueBench_Shutdown() from
// Accessibility_Exit drops the bench's backend handles BEFORE Cue3D_Shutdown frees them.
//
// Threading: main-thread-only, like the Cue3D game-thread API and the Cue layer it drives.

#ifdef __cplusplus

// CVar names, shared with the ImguiUI.cpp submenu that draws the controls so both sides
// spell them the same way. All live-editable; the listener re-reads them every tick.
inline constexpr const char* kCueBenchActiveCVar = "gAccessibilityCueBench.Active";
inline constexpr const char* kCueBenchSynthToneCVar = "gAccessibilityCueBench.SynthTone";
inline constexpr const char* kCueBenchOrbitCVar = "gAccessibilityCueBench.Orbit";
inline constexpr const char* kCueBenchModeCVar = "gAccessibilityCueBench.Mode";
inline constexpr const char* kCueBenchPitchCVar = "gAccessibilityCueBench.Pitch";
inline constexpr const char* kCueBenchIntervalCVar = "gAccessibilityCueBench.Interval";
inline constexpr const char* kCueBenchLowPassCVar = "gAccessibilityCueBench.LowPass";
inline constexpr const char* kCueBenchRapidRetriggerCVar = "gAccessibilityCueBench.RapidRetrigger";
inline constexpr const char* kCueBenchStartStopStressCVar = "gAccessibilityCueBench.StartStopStress";

extern "C" {
#endif

// Register the bench's CVars, its hidden one-shot cue, and its per-frame listener. Call
// once from Accessibility_Init. The listener stays cheap while the bench CVar is off.
void CueBench_Init(void);

// Drop the bench's backend handles and latch the listener inert. Call from
// Accessibility_Exit BEFORE Cue3D_Shutdown, so no later tick touches a freed source.
void CueBench_Shutdown(void);

// Request one hidden-cue one-shot at the bench's current position on the next tick. The
// "Play one-shot" button calls this; the listener drains the requests so every press sounds
// (the actual PlayOnce runs on the game tick, alongside the rapid-retrigger stress path).
void CueBench_RequestOneShot(void);

#ifdef __cplusplus
}
#endif
