#pragma once

#include <stdbool.h>

// Cue3D: a backend-agnostic seam for 3D-positional audio cues. The caller loads a
// mono sound, plays it, and moves it around the listener in 3D; a backend (Steam
// Audio's binaural HRTF spatializer, in Cue3DSteamAudio.cpp) turns that into stereo.
// The backend owns the full spatialization: both DIRECTION (HRTF) and DISTANCE
// attenuation are derived from the position the caller pushes, so the caller just
// reports where the source is and lets the backend decide how it sounds. That includes
// deliberately exaggerating cues a raw HRTF renders too weakly — e.g. the rear-hemisphere
// muffle + gain dip for front/back — so an alternate backend should reproduce those,
// not treat them as a spatializer artifact. Keeping
// this interface free of any spatializer detail means an alternate backend (e.g.
// OpenAL Soft) can drop in behind the same header without touching callers.
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

// Defaults for Cue3D_SetRearEffect — also the values in effect if it is never called.
// Defined at the seam so the backend's initial state and the settings layer's CVar
// registration share one source of truth. Tuning guidance: docs/accessibility-cues-tuning.md.
#define CUE3D_REAR_CUTOFF_HZ_DEFAULT 1000.0f
#define CUE3D_REAR_GAIN_DIP_DEFAULT 0.25f
#define CUE3D_REAR_TREMOLO_DEPTH_DEFAULT 0.5f
#define CUE3D_REAR_TREMOLO_HZ_DEFAULT 10.0f

#ifdef __cplusplus
extern "C" {
#endif

// Open the audio device and create the spatializer. Idempotent. Until this
// succeeds, the Load/Play/etc. calls below are no-ops.
void Cue3D_Init(void);

// Stop the device and release every source and spatializer object. Safe to call
// even if Init never ran or failed.
void Cue3D_Shutdown(void);

// The backend's negotiated output sample rate, in Hz — the rate Cue3D_LoadPcm
// expects its PCM to already be at. Determined at Cue3D_Init when the OS device is
// opened (it may differ from the 48 kHz request); before Init it reports the default
// request rate. Lets a caller synthesize PCM at the right rate instead of hardcoding
// a constant that must track the backend. Returns 0 if no backend is compiled in.
int Cue3D_GetSampleRate(void);

// The distance, in the game's world units, at or inside which the backend applies no
// distance attenuation — a source placed exactly here renders at unity gain. Lets a
// caller position a sound at "reference loudness" (the settings-menu preview) without
// duplicating the backend's attenuation tuning. Returns 0 if no backend is compiled in.
float Cue3D_GetUnityGainDistance(void);

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
// +z ahead). Both the DIRECTION and the DISTANCE (the vector's length) matter: the
// backend spatializes by direction and attenuates by distance, so a far source is
// rendered quieter than a near one. Cheap; call it every frame to move the source.
void Cue3D_SetPosition(Cue3DSource* source, float x, float y, float z);

// Set a flat linear gain on the source (1.0 = unchanged), applied on TOP of the
// backend's distance attenuation. Use it for distance-independent level — not for
// falloff, which the backend derives from the position passed to Cue3D_SetPosition.
// The Cue layer (Cue.h) drives this every tick with game master x cue master x
// per-cue volume; it is also the lever a future TTS-ducking pass would use. BGM
// ducking (SFX_FLAG_19) still does not reach this second audio device — see
// docs/accessibility-hrtf-cues.md "Known limitations".
void Cue3D_SetGain(Cue3DSource* source, float gain);

// Set a playback-rate multiplier on the source (1.0 = native rate / no shift,
// 2.0 = one octave up, 0.5 = one octave down). Cheap; safe to call every frame.
// Used as an explicit elevation cue (higher source -> higher pitch) layered on
// top of the spatializer's own positional rendering.
void Cue3D_SetPitch(Cue3DSource* source, float rate);

// Silence the source. It stays loaded and can be played again.
void Cue3D_Stop(Cue3DSource* source);

// Configure the rear-hemisphere front/back exaggeration the backend applies to EVERY
// source (see the header comment above): at full rear a source is low-passed down to
// `cutoffHz`, dipped by `gainDip` (0..1 fraction of gain removed), and amplitude-pulsed
// ("tremolo") with `tremoloDepth` (0..1, 0 = off) at `tremoloHz`. All three blend in
// smoothly across the rear hemisphere and leave the front hemisphere untouched. Cheap,
// lock-free, safe to call any time — the settings sliders drive it live. Until the first
// call the CUE3D_REAR_*_DEFAULT values above are in effect.
void Cue3D_SetRearEffect(float cutoffHz, float gainDip, float tremoloDepth, float tremoloHz);

#ifdef __cplusplus
}
#endif
