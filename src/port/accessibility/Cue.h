#pragma once

// Cue: one named positional audio cue rendered through the Cue3D seam (Cue3D.h), plus a
// registry the settings/glossary UI can enumerate. This layer is game-agnostic — it knows
// about sound files, volume CVars, and 3D positions, but nothing about Star Fox. The
// game-side policy (which cues exist, what they track, when they start/stop) lives in the
// consumer mod's per-cue files (src/port/mods/accessibility_cues/, coordinated by
// src/port/mods/AccessibilityCues.cpp), which register cues at init and drive them from
// their event listeners.
//
// A Cue is a DEFINITION plus a pool of VOICES. The definition is what the player sees:
// identity, sound file, the ONE volume CVar, the glossary entry, the preview. A voice is
// one backend source with its own position/pitch/playing state, so the same cue can sound
// at several positions at once (e.g. the N closest enemies) while the player still tunes a
// single "Enemy locator" slider. The pool size is fixed at registration (default 1).
//
// Each cue owns:
//   - lazy loading of its sound (Cue3D_Init + one Cue3D_Load — or one generator run +
//     Cue3D_LoadPcm — per voice on first use, with a failure latch so a broken file or a
//     throwing generator isn't retried every frame),
//   - its per-cue volume CVar ("gAccessibilityCueVolume.<id>", 0..1, default 1), applied
//     together with the game master volume and the cue master volume — see PushGain(),
//   - a preview mode for the settings UI / future cue glossary: the sound plays straight
//     ahead at the distance the backend renders at unity gain, so what the player hears is
//     exactly their volume setting. Preview suspends gameplay control of the cue (the
//     gameplay methods no-op) and auto-expires after a few seconds via CueRegistry_Tick().
//
// Driving a cue (pick the style matching its registration — voice 0 is shared by the
// first two, and mixing looping/one-shot APIs on the wrong cue no-ops with a warn log):
//   - Single-voice convenience (looping cues): SetTarget / Start / Stop, exactly the
//     pre-pool API. Drives voice 0, which stays under manual control (never reaped).
//   - Keyed multi-voice (looping cues): call TargetVoice(key, ...) once per target per
//     game tick. The key names the target (the consumer picks it, e.g. from an actor slot
//     + identity); the same key keeps the same voice across ticks (sticky), so targets
//     don't swap voices frame-to-frame. Refresh-or-stop: a keyed voice whose key was not
//     re-targeted by the next CueRegistry_Tick() is silenced automatically, so a driving
//     listener never bookkeeps voice lifetimes — it just targets what it wants audible
//     each tick. StopAllVoices() is the instant-silence path for gated ticks (pause, cue
//     disabled).
//   - Fire-and-forget (one-shot cues, CueSpec::loop = false): PlayOnce(target) plays the
//     sound once at that position; no refresh, no reap. If the pool is exhausted the
//     oldest in-flight one-shot is stolen (a re-trigger beats a drop).
//
// Threading: everything here is main-thread-only, like the Cue3D game-thread API it wraps.

#include <stdint.h>
#include <functional>
#include <string>
#include <vector>

#include "Cue3D.h"

// Master volume for ALL cues (0..1 float CVar, default 1). Registered by CueRegistry on
// first use; named here so the settings UI and PushGain() share one spelling.
inline constexpr const char* kCueMasterVolumeCVar = "gAccessibilityCueMasterVolume";

// The rear-effect knobs (float CVars; see Cue3D_SetRearEffect for what each does), named
// here so the settings UI and CueRegistry share one spelling. Registered by CueRegistry on
// first use, with the seam's CUE3D_REAR_*_DEFAULT values; CueRegistry_Tick re-reads them
// every tick and pushes them to the backend, so a change from a slider OR the console is
// live without a restart. The seam clamps whatever it is handed, so no caller has to.
inline constexpr const char* kCueRearCutoffCVar = "gAccessibilityCueRearCutoffHz";
inline constexpr const char* kCueRearGainDipCVar = "gAccessibilityCueRearGainDip";
inline constexpr const char* kCueRearTremoloDepthCVar = "gAccessibilityCueRearTremoloDepth";
inline constexpr const char* kCueRearTremoloHzCVar = "gAccessibilityCueRearTremoloHz";

// Pitch realization style (int CVar, 1 = spectral pitch shifter, 0 = playback-rate
// resample; see Cue3D_SetPitchStyle for the tradeoff). Same registration/push cycle as
// the rear knobs above. The default lives here (not at the seam) on purpose: the backend's
// own no-CVar fallback is RESAMPLE, and this settings-layer default of "shifter on" is the
// experiment under evaluation. Registration, re-read, and the F1 checkbox all share it.
inline constexpr const char* kCuePitchShiftCVar = "gAccessibilityCuePitchShift";
inline constexpr int kCuePitchShiftDefault = 1;

// Everything a cue is registered WITH (vs. the id/name/description identity strings, which
// stay explicit parameters). Exactly one of wavPath/generator must be set — the cue's
// sound either comes from a file or is synthesized once per voice at load time.
struct CueSpec {
    const char* wavPath = nullptr; // app-relative (e.g. "assets/accessibility/ring.wav")
    // Synthesized alternative to a WAV: called once per voice at load, at the backend's
    // negotiated rate (the sampleRate argument, from Cue3D_GetSampleRate), returning mono
    // float PCM handed to Cue3D_LoadPcm.
    std::function<std::vector<float>(int sampleRate)> generator;
    int maxVoices = 1;                // voice-pool size, clamped to [1, 8]
    bool loop = true;                 // false = one-shot cue, driven via PlayOnce only
    Cue3DMode mode = CUE3D_MODE_HRTF; // pushed to each voice's source at load
    // Per-source pitch-realization override (see Cue3D_SetSourcePitchStyle); GLOBAL follows
    // the settings-layer A/B (kCuePitchShiftCVar). Pushed at load, like `mode`.
    Cue3DSourcePitchStyle pitchStyle = CUE3D_SOURCE_PITCH_GLOBAL;
    bool hiddenFromSettings = false;  // bench/internal cues stay out of the volume-slider list
};

// Dynamic per-voice parameters, pushed on every SetTarget / TargetVoice / PlayOnce.
// The position is listener-relative in the game convention: +x right, +y up, +z ahead.
struct CueTarget {
    float x = 0.0f, y = 0.0f, z = 1.0f;
    float pitch = 1.0f;
    float intervalSec = 0.0f; // restart cadence, see Cue3D_SetInterval; 0 = seamless loop
    float lowPassHz = 0.0f;   // per-source muffle, see Cue3D_SetLowPass; 0 = off
    // Per-voice attenuation UNDER the volume sliders, 0..1 (1 = the slider's full level).
    // For cues whose loudness IS a signal (the obstacle above/below cues encode vertical
    // clearance this way): the slider keeps meaning "the loudest this cue ever gets" and
    // the consumer scales each voice below it. Multiplied into PushGain()'s product per
    // voice, so it also honors the multi-voice headroom trim; previews ignore it (a
    // preview is always the reference loudness). NaN or out-of-range values are clamped
    // to [0, 1] with NaN pinned to 1 — "no signal" must not silence a cue by accident.
    float level = 1.0f;
};

// Read-only state snapshots for debug tooling (the debug server's `cues` command). Value
// copies of the Cue layer's own game-thread bookkeeping — never a readback from the Cue3D
// backend, which stays push-only. Caveat: while a preview is running, the voice fields
// describe the SUSPENDED gameplay state (the preview drives voice 0's source directly at
// unity distance), so previewing=true means "what you hear is not what you read here".
struct CueVoiceSnapshot {
    int index = 0;
    bool playing = false;  // Cue-layer bookkeeping; advisory for one-shots between ticks
    bool loaded = false;   // backend source exists (lazy load has run and succeeded)
    bool keyed = false;    // acquired via TargetVoice, subject to the refresh-or-stop reap
    uint64_t key = 0;      // meaningful only when keyed
    CueTarget target;      // last pushed position/pitch/interval/low-pass
    float effectivePitch = 1.0f; // target.pitch x the voice slot's identity pitch, as pushed
    float gain = 0.0f;           // baseGain x headroom trim x target.level while playing; 0 when silent
};

struct CueSnapshot {
    bool loop = true;
    int maxVoices = 1;
    Cue3DMode mode = CUE3D_MODE_HRTF;
    Cue3DSourcePitchStyle pitchStyle = CUE3D_SOURCE_PITCH_GLOBAL;
    bool hiddenFromSettings = false;
    bool previewing = false;
    bool loadFailed = false;
    float gainBoost = 1.0f;
    float baseGain = 0.0f; // game master x cue master x per-cue volume x gainBoost
    int playingVoices = 0;
    std::vector<CueVoiceSnapshot> voices; // size == maxVoices, in slot order
};

class Cue {
  public:
    // --- Identity, for the settings UI / glossary ---
    const char* Id() const { return mId.c_str(); }
    const char* Name() const { return mName.c_str(); }
    const char* Description() const { return mDescription.c_str(); }
    const char* VolumeCVar() const { return mVolumeCVar.c_str(); }
    bool HiddenFromSettings() const { return mSpec.hiddenFromSettings; }

    // --- Gameplay control, single-voice convenience (drives voice 0; looping cues) ---

    // Remember the cue's target (position/pitch/interval/low-pass — see CueTarget) and
    // push it (plus the current gain) to the live source. Safe to call every tick,
    // playing or not — the remembered target is what Start() begins from, so the first
    // audible block is already positioned correctly.
    void SetTarget(const CueTarget& t);

    // Position/pitch-only convenience, delegating to the CueTarget overload with the
    // remaining fields at their defaults. Keeps pre-CueTarget drivers unchanged.
    void SetTarget(float x, float y, float z, float pitch);

    // Begin looping playback at the last SetTarget position. Idempotent while playing.
    // Lazily loads the sound on first use; a load failure makes this a no-op (logged once).
    void Start();

    // Silence the cue. Idempotent. Does NOT interrupt a running preview — gated listeners
    // call Stop() every tick, and that must not cut a preview short.
    void Stop();

    // --- Gameplay control, keyed multi-voice (looping cues) ---

    // Aim one voice at a target (same conventions as SetTarget) and start it if it isn't
    // sounding yet. Sticky by `key`; call once per live target per game tick — see the
    // header comment for the refresh-or-stop contract. If every voice is taken by a key
    // refreshed this same tick, the call is dropped (trace-logged), so drive at most as
    // many targets as the pool size passed at registration.
    void TargetVoice(uint64_t key, const CueTarget& t);

    // Position/pitch-only convenience, delegating like the SetTarget overload.
    void TargetVoice(uint64_t key, float x, float y, float z, float pitch);

    // --- Gameplay control, fire-and-forget (one-shot cues, CueSpec::loop = false) ---

    // Play the cue's sound once at the given target. Reclaims voices whose one-shots have
    // finished; if the pool is exhausted mid-flight, steals the oldest playing one-shot
    // (a re-trigger beats a drop). No-ops while previewing (the UI owns the cue) and on
    // looping cues (caller bug; one-time warn log).
    void PlayOnce(const CueTarget& t);

    // Silence every voice now, without waiting for the reap. Idempotent, preview-safe:
    // like Stop(), gated listeners may call it every tick.
    void StopAllVoices();

    // --- Preview (settings UI / glossary) ---

    // Play the cue straight ahead at unity distance gain for a few seconds, so the player
    // can hear the sound at their chosen volume. Interrupts gameplay playback (all voices)
    // and any other cue's preview; while previewing, gameplay control is suspended (the
    // consumer's listeners resume it naturally on the tick after the preview ends).
    // Calling this again while previewing restarts the preview window.
    void StartPreview();
    void StopPreview();
    bool IsPreviewing() const { return mPreviewing; }

    // Recompute gGameMasterVolume x cue master x per-cue volume (x gain boost, below) and
    // push it to every live voice. SetTarget/TargetVoice/Start/StartPreview already do this;
    // the settings UI calls it on slider change so a running preview tracks the slider live.
    void PushGain();

    // Per-cue loudness normalization multiplied into PushGain()'s product UNDER the user's
    // volume sliders (which stay 0..1): for sounds that are inherently quieter at equal
    // sample level — a short click against a sustained loop — so every cue's slider means
    // "relative to a sensible default" rather than leaving one cue with no headroom at 100%.
    // Sanitized here (NaN / non-positive -> 1, capped at 8); applies to previews too, so the
    // preview's loudness stays honest. Safe to push every tick from a CVar.
    void SetGainBoost(float boost);

    // Value-copy state dump for debug tooling — see the CueSnapshot comment above for what
    // it does and does not describe. Main-thread-only, like everything else here.
    CueSnapshot Snapshot() const;

  private:
    friend Cue* CueRegistry_Register(const char* id, const char* name, const char* description, const CueSpec& spec);
    friend void CueRegistry_Tick();
    friend void CueRegistry_UnloadAll();

    // One backend source and its published state. `hasKey` marks a voice acquired via
    // TargetVoice — only those are subject to the refresh-or-stop reap; voice 0 under the
    // single-voice API never sets it, and one-shot voices (PlayOnce) never set it either.
    struct Voice {
        Cue3DSource* source = nullptr;
        bool playing = false;
        bool hasKey = false;
        bool refreshed = false; // TargetVoice'd since the last CueRegistry_Tick
        uint64_t key = 0;
        CueTarget target;        // last pushed position/pitch/interval/low-pass
        uint64_t startStamp = 0; // per-Cue monotonic PlayOnce order, for oldest-steal
    };

    Cue(const char* id, const char* name, const char* description, const CueSpec& spec);

    // The gain formula and the multi-voice headroom trim, shared by PushGain() and
    // Snapshot() so the reported gain can never drift from the pushed gain. The per-voice
    // level (CueTarget::level) is the third factor, sanitized in one place for both.
    float ComputeGain() const;
    static float HeadroomTrim(int playingCount);
    static float VoiceLevel(const Voice& voice);

    bool EnsureVoiceLoaded(Voice& voice);
    Voice* AcquireVoice(uint64_t key);
    void StopVoice(Voice& voice);
    // Push the voice's stored target (position + identity-scaled pitch + interval + low-pass)
    // to its live source. Assumes voice.source is non-null and the caller wants it audible.
    void PushVoiceParams(Voice& voice);
    // Log once when a looping-cue API (Start/SetTarget/TargetVoice) is used on a one-shot cue
    // or PlayOnce on a looping cue — a caller bug, not UB; the call otherwise no-ops.
    void WarnWrongApi(const char* method);
    void Tick(); // preview expiry + the keyed-voice reap; called by CueRegistry_Tick

    std::string mId;
    std::string mName;
    std::string mDescription;
    std::string mVolumeCVar; // "gAccessibilityCueVolume.<id>"
    CueSpec mSpec;           // registration options (wav path / generator kept alive here)
    std::string mWavPath;    // owned copy of mSpec.wavPath (the registrant's pointer may not outlive registration)

    std::vector<Voice> mVoices; // fixed size (= registration maxVoices) for the cue's life
    float mGainBoost = 1.0f;    // loudness normalization, see SetGainBoost
    bool mLoadFailed = false;   // don't retry a failed load every frame
    uint64_t mNextStartStamp = 1; // feeds Voice::startStamp on each PlayOnce
    bool mWarnedWrongApi = false; // one-time warn for looping-API-on-one-shot (and vice versa)

    bool mPreviewing = false;
    double mPreviewTicksLeft = 0.0; // game ticks until the preview expires
};

// The registry: created once at consumer-mod init, enumerated by the settings UI, never
// destroyed (Cue objects live for the process; UnloadAll only drops backend handles, so a
// cached Cue* can never dangle).

// Create and register a cue. `id` must be unique and CVar-name-safe (e.g. "Ring");
// `name`/`description` are the human-readable strings the settings UI / glossary shows;
// everything else (sound, pool size, loop/one-shot, render mode, settings visibility) is
// in the spec. Pool sizes are clamped to [1, 8] — the backend has 16 source slots shared
// by ALL cues, so keep pools small.
Cue* CueRegistry_Register(const char* id, const char* name, const char* description, const CueSpec& spec);

// Every registered cue, in registration order. For the settings UI / glossary.
const std::vector<Cue*>& CueRegistry_All();

// Per-game-tick housekeeping: expires timed previews and reaps keyed voices that were not
// re-targeted since the previous call. The consumer mod calls this once per game tick; the
// reap tolerates running either before or after the driving listeners within a tick
// (acquisition may steal a playing-but-unrefreshed voice, so a changed target set never
// starves — see Cue::AcquireVoice).
void CueRegistry_Tick();

// Stop every cue and drop all backend handles (call right before Cue3D_Shutdown, which
// frees the underlying sources). The Cue objects stay registered and reusable: a later
// Start() would lazily reload. Fixes the dangling-handle latency of review CUE3D-6.
void CueRegistry_UnloadAll();
