#pragma once

#include <stdbool.h>

// Cue3D: a backend-agnostic seam for 3D-positional audio cues. The caller loads a
// mono sound, plays it, and moves it around the listener in 3D; a backend (Steam
// Audio's binaural HRTF spatializer, in Cue3DSteamAudio.cpp) turns that into stereo.
// The backend owns the full spatialization: both DIRECTION (HRTF) and DISTANCE
// attenuation are derived from the position the caller pushes, so the caller just
// reports where the source is and lets the backend decide how it sounds. That includes
// deliberately exaggerating cues a raw HRTF renders too weakly — e.g. the rear-hemisphere
// muffle + gain dip + tremolo for front/back — so an alternate backend should reproduce
// those, not treat them as a spatializer artifact.
//
// What the seam therefore carries is PERCEPTUAL POLICY, not "no spatializer detail":
// Cue3D_SetRearEffect prescribes an audible outcome that every backend owes the player,
// and its parameters happen to be spelled in the DSP units the tuning phase needs (a
// cutoff and an LFO rate in Hz). An alternate backend (e.g. OpenAL Soft, whose direct-path
// filter is a high-frequency gain RATIO and which has no per-source tremolo) has to map
// those onto its own primitives rather than pass them through. That is a real cost, and it
// is deliberate while the values are still being settled by ear: once they are, the honest
// end-state for this seam is FEWER floats, not more — a single perceptual "rear emphasis"
// strength with the DSP constants baked into each backend. Treat the four-float shape as a
// tuning-phase interface, not a fixture. Everything else here is genuinely backend-neutral,
// so a swap stays a one-file change that no caller sees. The PAN and DIRECT render modes
// (Cue3D_SetMode) are part of that owed outcome too: they are deliberately DRY renderings —
// no HRTF, no rear effect — and an alternate backend must honor them as such (trivially:
// they are backend-neutral math), so the one-file-swap property is intact.
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
// thread. Cue3D_Play / SetPosition / SetGain / SetPitch / SetInterval / SetMode /
// SetLowPass / Stop are cheap, lock-free, and safe to call every game frame; the audio
// callback reads the published values. Cue3D_IsPlaying reads a value the callback
// publishes back and may lag it by one audio block — advisory, not a fence.

typedef struct Cue3DSource Cue3DSource;

// Defaults for Cue3D_SetRearEffect — also the values in effect if it is never called.
// Defined at the seam so the backend's initial state and the settings layer's CVar
// registration share one source of truth. Tuning guidance: docs/accessibility-cues-tuning.md.
#define CUE3D_REAR_CUTOFF_HZ_DEFAULT 600.0f
#define CUE3D_REAR_GAIN_DIP_DEFAULT 0.0f
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

// True once Cue3D_Init has actually opened the OS device and built the spatializer —
// the runtime "can cues sound at all" probe (the stub backend always says false).
// Distinct from the compile-time probe (GetUnityGainDistance() > 0): a backend can be
// compiled in and still fail to open its device.
bool Cue3D_IsActive(void);

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

// Start playback of a loaded source from the BEGINNING of its sound. EVERY call
// restarts at sample 0: a looping source that was Stopped does not resume mid-loop,
// and a Play issued while already playing rewinds rather than no-ops. That rewind is
// deliberate — it is exactly what reliable one-shot re-trigger needs, and for short
// cue loops a restart is inaudible-to-preferable.
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

// Set a pitch multiplier on the source (1.0 = native pitch / no shift,
// 2.0 = one octave up, 0.5 = one octave down). Cheap; safe to call every frame.
// Used as an explicit elevation cue (higher source -> higher pitch) layered on
// top of the spatializer's own positional rendering. HOW the multiplier is
// realized is selected by Cue3D_SetPitchStyle below; in RESAMPLE style it is
// applied with a short (~20 ms) slew, so per-game-frame updates glide instead
// of stepping, and a re-Play snaps straight to the latest rate.
void Cue3D_SetPitch(Cue3DSource* source, float rate);

// How the backend realizes Cue3D_SetPitch, for ALL sources (global, like
// Cue3D_SetRearEffect). An A/B experiment toggle while the elevation-pitch sound
// is being settled by ear — expect it to collapse to one style once judged:
//   CUE3D_PITCH_RESAMPLE — playback-rate change. Artifact-free and latency-free,
//     but pitch drags duration and timbre with it (the "chipmunk" effect): a cue
//     pitched an octave up plays twice as fast and thin.
//   CUE3D_PITCH_SHIFT    — spectral pitch shifter (Signalsmith Stretch). Duration
//     and envelope stay fixed; costs ~0.1 s of latency on the AUDIO CONTENT
//     (onsets, pitch changes — pulse cadence and spatial position are unaffected)
//     plus some transient softening, and noticeably more CPU per source.
// Cheap, lock-free, safe to call any time; takes effect within a block (~21 ms).
typedef enum Cue3DPitchStyle { CUE3D_PITCH_RESAMPLE = 0, CUE3D_PITCH_SHIFT } Cue3DPitchStyle;
void Cue3D_SetPitchStyle(Cue3DPitchStyle style);

// Per-source override of how THAT source realizes Cue3D_SetPitch, layered over the global
// Cue3D_SetPitchStyle: GLOBAL (the default) follows the global A/B choice above; RESAMPLE
// and SHIFT pin the style regardless of it. For sounds whose realization is structural
// rather than a matter of taste — a click cue pins RESAMPLE because the shifter's ~0.1 s
// latency and transient softening would blunt the very attack that carries the signal.
// Cheap and lock-free like the other setters; the Cue layer pushes it once at load (it is
// a property of the sound, not a per-frame signal).
typedef enum Cue3DSourcePitchStyle {
    CUE3D_SOURCE_PITCH_GLOBAL = 0, // follow Cue3D_SetPitchStyle
    CUE3D_SOURCE_PITCH_RESAMPLE,
    CUE3D_SOURCE_PITCH_SHIFT,
} Cue3DSourcePitchStyle;
void Cue3D_SetSourcePitchStyle(Cue3DSource* source, Cue3DSourcePitchStyle style);

// Silence the source. It stays loaded and can be played again.
void Cue3D_Stop(Cue3DSource* source);

// Advisory playback status: true while the source is audibly rendering. For looping
// sources this mirrors Play/Stop; for one-shots it goes false when the sound finishes.
// May lag reality by one audio block (~21 ms) — treat as advisory, not a fence.
bool Cue3D_IsPlaying(Cue3DSource* source);

// Restart cadence for a LOOPING source, in wall-clock seconds (independent of pitch).
// 0 (default) = seamless loop, exactly the pre-interval behavior. > 0 = the sound
// restarts from its beginning every `seconds`, measured start-to-start: silence pads
// the gap when the interval exceeds the sound's length; a shorter interval truncates
// and restarts. Sample-accurate (the audio callback owns the timing). Cheap; push it
// every tick like position — a live rate change takes effect immediately. Ignored on
// one-shot sources.
void Cue3D_SetInterval(Cue3DSource* source, float seconds);

// How the source is rendered into stereo. Position (Cue3D_SetPosition) stays the input
// in every mode; the mode selects the renderer:
//   CUE3D_MODE_HRTF   — binaural HRTF + distance attenuation + rear effect (default).
//   CUE3D_MODE_PAN    — constant-power stereo pan from the horizontal direction;
//                       distance attenuation applies; rear effect and HRTF do not.
//                       Front/back and elevation collapse (inherent to stereo pan).
//   CUE3D_MODE_DIRECT — dead center, no spatialization, no distance attenuation, no
//                       rear effect; gain/pitch/interval/low-pass still apply.
typedef enum Cue3DMode { CUE3D_MODE_HRTF = 0, CUE3D_MODE_PAN, CUE3D_MODE_DIRECT } Cue3DMode;
void Cue3D_SetMode(Cue3DSource* source, Cue3DMode mode);

// Per-source low-pass "muffle", independent of (in series with) the rear muffle.
// cutoffHz <= 0 disables it (the default). Applies in every render mode.
void Cue3D_SetLowPass(Cue3DSource* source, float cutoffHz);

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
