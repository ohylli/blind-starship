#pragma once

// Throwaway smoke-test driver for the Cue3D seam (Cue3D.h). Reproduces the step-1
// orbiting-source test through the new backend-agnostic API: loads the enemy cue
// sound (assets/accessibility/enemy.wav), plays it as one Cue3D source, and orbits
// it around the listener once every 4 s by calling Cue3D_SetPosition each game
// frame. Gated by gAccessibilitySpatialTest from Accessibility.cpp; delete this
// file once a real in-game cue replaces it.

#ifdef __cplusplus
extern "C" {
#endif

// Initializes Cue3D, starts the orbiting test tone, and registers the per-frame
// listener that moves it. Call once, only when the test is enabled.
void SpatialAudioTest_Start(void);

// Detaches the test from its source so the per-frame listener goes inert. Call
// before Cue3D_Shutdown so the listener never touches a freed source.
void SpatialAudioTest_Stop(void);

#ifdef __cplusplus
}
#endif
