#pragma once

#include <stdbool.h>

// Cue3D: a backend-agnostic seam for 3D-positional audio cues. The caller loads a
// mono sound, plays it, and moves it around the listener in 3D; a backend (Steam
// Audio's binaural HRTF spatializer, in Cue3DSteamAudio.cpp) turns that into stereo.
// Keeping this interface free of any spatializer detail means an alternate backend
// (e.g. OpenAL Soft) can drop in behind the same header without touching callers.
//
// The backend opens its own OS audio device alongside libultraship's; the OS mixer
// combines the two streams (same coexistence model as PRISM/Tolk for TTS).
//
// Coordinate convention — the SEAM owns it, the backend adapts. Positions are the
// player-relative offset in the GAME's convention: +x right, +y up, +z ahead (see
// docs/audio-system.md §6). Any spatializer-specific axis remapping (e.g. Steam
// Audio's -Z-forward space) happens INSIDE the backend, never in the caller — so
// game code never learns the spatializer's axis convention.
//
// Threading: Cue3D_Load / Cue3D_LoadPcm allocate and decode on the calling (main)
// thread. Cue3D_Play / SetPosition / SetGain / SetPitch / Stop are cheap, lock-free,
// and safe to call every game frame; the audio callback reads the published values.

typedef struct Cue3DSource Cue3DSource;

#ifdef __cplusplus
extern "C" {
#endif

// Open the audio device and create the spatializer. Idempotent. Until this
// succeeds, the Load/Play/etc. calls below are no-ops.
void Cue3D_Init(void);

// Stop the device and release every source and spatializer object. Safe to call
// even if Init never ran or failed.
void Cue3D_Shutdown(void);

// The backend's fixed output sample rate, in Hz — the rate Cue3D_LoadPcm expects
// its PCM to already be at. Lets a caller synthesize PCM at the right rate instead
// of hardcoding a constant that must track the backend. Returns 0 if no backend is
// compiled in.
int Cue3D_GetSampleRate(void);

// Load a sound from a file (any format miniaudio decodes — WAV/FLAC/MP3),
// downmixed to mono and resampled to the backend's rate. Returns NULL on failure
// or if no source slot is free. `loop` makes playback wrap forever.
Cue3DSource* Cue3D_Load(const char* path, bool loop);

// Load a sound from an in-memory mono float buffer already at the backend's rate.
// The bytes are copied; the caller's buffer need not outlive the call. Returns
// NULL on failure / no free slot.
Cue3DSource* Cue3D_LoadPcm(const float* monoPcm, int frames, bool loop);

// Begin (or resume) playback of a loaded source. A one-shot source that has already
// finished restarts from the beginning.
void Cue3D_Play(Cue3DSource* source);

// Set the source's listener-relative position (game convention: +x right, +y up,
// +z ahead). Cheap; call it every frame to move the source.
void Cue3D_SetPosition(Cue3DSource* source, float x, float y, float z);

// Set a flat linear gain on the source (1.0 = unchanged). No distance attenuation
// is applied by the backend — that's the caller's model, if any.
void Cue3D_SetGain(Cue3DSource* source, float gain);

// Set a playback-rate multiplier on the source (1.0 = native rate / no shift,
// 2.0 = one octave up, 0.5 = one octave down). Cheap; safe to call every frame.
// Used as an explicit elevation cue (higher source -> higher pitch) layered on
// top of the spatializer's own positional rendering.
void Cue3D_SetPitch(Cue3DSource* source, float rate);

// Silence the source. It stays loaded and can be played again.
void Cue3D_Stop(Cue3DSource* source);

#ifdef __cplusplus
}
#endif
