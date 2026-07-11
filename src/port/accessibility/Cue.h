#pragma once

// Cue: one named positional audio cue rendered through the Cue3D seam (Cue3D.h), plus a
// registry the settings/glossary UI can enumerate. This layer is game-agnostic — it knows
// about sound files, volume CVars, and 3D positions, but nothing about Star Fox. The
// game-side policy (which cues exist, what they track, when they start/stop) lives in the
// consumer mod (src/port/mods/AccessibilityCues.cpp), which registers cues at init and
// drives them from its event listeners.
//
// A Cue is a DEFINITION plus a pool of VOICES. The definition is what the player sees:
// identity, sound file, the ONE volume CVar, the glossary entry, the preview. A voice is
// one backend source with its own position/pitch/playing state, so the same cue can sound
// at several positions at once (e.g. the N closest enemies) while the player still tunes a
// single "Enemy locator" slider. The pool size is fixed at registration (default 1).
//
// Each cue owns:
//   - lazy loading of its sound (Cue3D_Init + one Cue3D_Load per voice on first use, with
//     a failure latch so a broken file isn't retried every frame),
//   - its per-cue volume CVar ("gAccessibilityCueVolume.<id>", 0..1, default 1), applied
//     together with the game master volume and the cue master volume — see PushGain(),
//   - a preview mode for the settings UI / future cue glossary: the sound plays straight
//     ahead at the distance the backend renders at unity gain, so what the player hears is
//     exactly their volume setting. Preview suspends gameplay control of the cue (the
//     gameplay methods no-op) and auto-expires after a few seconds via CueRegistry_Tick().
//
// Driving a cue, two ways (use one or the other per cue — voice 0 is shared):
//   - Single-voice convenience: SetTarget / Start / Stop, exactly the pre-pool API. Drives
//     voice 0, which stays under manual control (never reaped).
//   - Keyed multi-voice: call TargetVoice(key, ...) once per target per game tick. The key
//     names the target (the consumer picks it, e.g. from an actor slot + identity); the
//     same key keeps the same voice across ticks (sticky), so targets don't swap voices
//     frame-to-frame. Refresh-or-stop: a keyed voice whose key was not re-targeted by the
//     next CueRegistry_Tick() is silenced automatically, so a driving listener never
//     bookkeeps voice lifetimes — it just targets what it wants audible each tick.
//     StopAllVoices() is the instant-silence path for gated ticks (pause, cue disabled).
//
// Threading: everything here is main-thread-only, like the Cue3D game-thread API it wraps.

#include <stdint.h>
#include <string>
#include <vector>

#include "Cue3D.h"

// Master volume for ALL cues (0..1 float CVar, default 1). Registered by CueRegistry on
// first use; named here so the settings UI and PushGain() share one spelling.
inline constexpr const char* kCueMasterVolumeCVar = "gAccessibilityCueMasterVolume";

// The rear-effect knobs (float CVars; see Cue3D_SetRearEffect for what each does), named
// here so the settings UI and Cue_PushRearEffectFromCVars share one spelling. Defaults are
// the seam's CUE3D_REAR_*_DEFAULT values.
inline constexpr const char* kCueRearCutoffCVar = "gAccessibilityCueRearCutoffHz";
inline constexpr const char* kCueRearGainDipCVar = "gAccessibilityCueRearGainDip";
inline constexpr const char* kCueRearTremoloDepthCVar = "gAccessibilityCueRearTremoloDepth";
inline constexpr const char* kCueRearTremoloRateCVar = "gAccessibilityCueRearTremoloHz";

// Read the four rear-effect CVars (registering their defaults on first call) and push them
// to Cue3D_SetRearEffect. Called once at consumer-mod init so persisted config values take
// effect, and by the settings UI on slider change so rear-effect tuning is live.
void Cue_PushRearEffectFromCVars();

class Cue {
  public:
    // --- Identity, for the settings UI / glossary ---
    const char* Id() const { return mId.c_str(); }
    const char* Name() const { return mName.c_str(); }
    const char* Description() const { return mDescription.c_str(); }
    const char* VolumeCVar() const { return mVolumeCVar.c_str(); }

    // --- Gameplay control, single-voice convenience (drives voice 0) ---

    // Remember the cue's listener-relative target (game convention: +x right, +y up,
    // +z ahead) and pitch multiplier, and push them (plus the current gain) to the live
    // source. Safe to call every tick, playing or not — the remembered target is what
    // Start() begins from, so the first audible block is already positioned correctly.
    void SetTarget(float x, float y, float z, float pitch);

    // Begin looping playback at the last SetTarget position. Idempotent while playing.
    // Lazily loads the sound on first use; a load failure makes this a no-op (logged once).
    void Start();

    // Silence the cue. Idempotent. Does NOT interrupt a running preview — gated listeners
    // call Stop() every tick, and that must not cut a preview short.
    void Stop();

    // --- Gameplay control, keyed multi-voice ---

    // Aim one voice at a listener-relative target (same conventions as SetTarget) and
    // start it if it isn't sounding yet. Sticky by `key`; call once per live target per
    // game tick — see the header comment for the refresh-or-stop contract. If every voice
    // is taken by a key refreshed this same tick, the call is dropped (trace-logged), so
    // drive at most as many targets as the pool size passed to CueRegistry_Register.
    void TargetVoice(uint64_t key, float x, float y, float z, float pitch);

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

    // Recompute gGameMasterVolume x cue master x per-cue volume and push it to every live
    // voice. SetTarget/TargetVoice/Start/StartPreview already do this; the settings UI
    // calls it on slider change so a running preview tracks the slider live.
    void PushGain();

  private:
    friend Cue* CueRegistry_Register(const char* id, const char* name, const char* description, const char* wavPath,
                                     int maxVoices);
    friend void CueRegistry_Tick();
    friend void CueRegistry_UnloadAll();

    // One backend source and its published state. `hasKey` marks a voice acquired via
    // TargetVoice — only those are subject to the refresh-or-stop reap; voice 0 under the
    // single-voice API never sets it.
    struct Voice {
        Cue3DSource* source = nullptr;
        bool playing = false;
        bool hasKey = false;
        bool refreshed = false; // TargetVoice'd since the last CueRegistry_Tick
        uint64_t key = 0;
        float x = 0.0f, y = 0.0f, z = 1.0f;
        float pitch = 1.0f;
    };

    Cue(const char* id, const char* name, const char* description, const char* wavPath, int maxVoices);

    bool EnsureVoiceLoaded(Voice& voice);
    Voice* AcquireVoice(uint64_t key);
    void StopVoice(Voice& voice);
    void Tick(); // preview expiry + the keyed-voice reap; called by CueRegistry_Tick

    std::string mId;
    std::string mName;
    std::string mDescription;
    std::string mWavPath;    // app-relative, resolved at load time
    std::string mVolumeCVar; // "gAccessibilityCueVolume.<id>"

    std::vector<Voice> mVoices; // fixed size (= registration maxVoices) for the cue's life
    bool mLoadFailed = false;   // don't retry a failed load every frame

    bool mPreviewing = false;
    double mPreviewTicksLeft = 0.0; // game ticks until the preview expires
};

// The registry: created once at consumer-mod init, enumerated by the settings UI, never
// destroyed (Cue objects live for the process; UnloadAll only drops backend handles, so a
// cached Cue* can never dangle).

// Create and register a cue. `id` must be unique and CVar-name-safe (e.g. "Ring");
// `name`/`description` are the human-readable strings the settings UI / glossary shows;
// `wavPath` is app-relative (e.g. "assets/accessibility/ring.wav"); `maxVoices` sizes the
// voice pool (clamped to [1, 8] — the backend has 16 source slots shared by ALL cues, so
// keep pools small). All cues loop — the backend's one-shot EOF handling has a latent
// lost-update issue (review CUE3D-13) that must be fixed before one-shot cues are added.
Cue* CueRegistry_Register(const char* id, const char* name, const char* description, const char* wavPath,
                          int maxVoices = 1);

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
