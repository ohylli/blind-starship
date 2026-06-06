#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Spatial 3D audio transport: a miniaudio playback device hosting Steam Audio's
// binaural HRTF spatializer. Star-Fox-agnostic, mirroring the PRISM TTS
// transport in Tts.{h,cpp} — it knows nothing about the game, only about
// turning a source position into HRTF-spatialized stereo.
//
// Opens its own OS audio device alongside libultraship's; the OS mixer combines
// the two streams (same coexistence model as PRISM/Tolk for TTS).
//
// SMOKE-TEST PHASE: SpatialAudio_Init starts a single HRTF-spatialized tone that
// slowly orbits the listener's head, to validate the Steam Audio + miniaudio
// integration end to end (download/link/DLL-copy, context+HRTF creation, the
// binaural apply, and audio-callback block-size matching). Gated by the
// gAccessibilitySpatialTest CVar from the consumer side (Accessibility.cpp).
// The generic "play sample at 3D position" API replaces this once the chain is
// confirmed audible.
void SpatialAudio_Init(void);
void SpatialAudio_Shutdown(void);

#ifdef __cplusplus
}
#endif
