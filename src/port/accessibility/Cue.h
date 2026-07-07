#pragma once

// Cue: one named positional audio cue rendered through the Cue3D seam (Cue3D.h), plus a
// registry the settings/glossary UI can enumerate. This layer is game-agnostic — it knows
// about sound files, volume CVars, and 3D positions, but nothing about Star Fox. The
// game-side policy (which cues exist, what they track, when they start/stop) lives in the
// consumer mod (src/port/mods/AccessibilityCues.cpp), which registers cues at init and
// drives them from its event listeners.
//
// Each cue owns:
//   - lazy loading of its sound (Cue3D_Init + Cue3D_Load on first use, with a failure
//     latch so a broken file isn't retried every frame),
//   - its per-cue volume CVar ("gAccessibilityCueVolume.<id>", 0..1, default 1), applied
//     together with the game master volume and the cue master volume — see PushGain(),
//   - a preview mode for the settings UI / future cue glossary: the sound plays straight
//     ahead at the distance the backend renders at unity gain, so what the player hears is
//     exactly their volume setting. Preview suspends gameplay control of the cue (Start /
//     SetTarget / Stop no-op) and auto-expires after a few seconds via
//     CueRegistry::TickPreviews().
//
// Threading: everything here is main-thread-only, like the Cue3D game-thread API it wraps.

#include <string>
#include <vector>

#include "Cue3D.h"

// Master volume for ALL cues (0..1 float CVar, default 1). Registered by CueRegistry on
// first use; named here so the settings UI and PushGain() share one spelling.
inline constexpr const char* kCueMasterVolumeCVar = "gAccessibilityCueMasterVolume";

class Cue {
  public:
    // --- Identity, for the settings UI / glossary ---
    const char* Id() const { return mId.c_str(); }
    const char* Name() const { return mName.c_str(); }
    const char* Description() const { return mDescription.c_str(); }
    const char* VolumeCVar() const { return mVolumeCVar.c_str(); }

    // --- Gameplay control (the consumer mod's listeners) ---

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

    // --- Preview (settings UI / glossary) ---

    // Play the cue straight ahead at unity distance gain for a few seconds, so the player
    // can hear the sound at their chosen volume. Interrupts gameplay playback and any other
    // cue's preview; while previewing, gameplay control is suspended (the consumer's
    // listeners resume it naturally on the tick after the preview ends). Calling this again
    // while previewing restarts the preview window.
    void StartPreview();
    void StopPreview();
    bool IsPreviewing() const { return mState == State::Previewing; }

    // The last SetTarget values (post-clamp, i.e. what was actually pushed to the
    // backend). For the consumer's diagnostic traces.
    float TargetX() const { return mTargetX; }
    float TargetY() const { return mTargetY; }
    float TargetZ() const { return mTargetZ; }
    float TargetPitch() const { return mTargetPitch; }

    // Recompute gGameMasterVolume x cue master x per-cue volume and push it to the live
    // source. SetTarget/Start/StartPreview already do this; the settings UI calls it on
    // slider change so a running preview tracks the slider live.
    void PushGain();

  private:
    friend Cue* CueRegistry_Register(const char* id, const char* name, const char* description, const char* wavPath);
    friend void CueRegistry_TickPreviews();
    friend void CueRegistry_UnloadAll();

    Cue(const char* id, const char* name, const char* description, const char* wavPath);

    bool EnsureLoaded();

    enum class State {
        Idle,       // silent; gameplay may start it
        Playing,    // sounding under gameplay control
        Previewing, // sounding under UI control; gameplay calls no-op
    };

    std::string mId;
    std::string mName;
    std::string mDescription;
    std::string mWavPath;    // app-relative, resolved at load time
    std::string mVolumeCVar; // "gAccessibilityCueVolume.<id>"

    Cue3DSource* mSource = nullptr;
    bool mLoadFailed = false; // don't retry a failed load every frame

    State mState = State::Idle;
    float mTargetX = 0.0f, mTargetY = 0.0f, mTargetZ = 1.0f;
    float mTargetPitch = 1.0f;
    double mPreviewTicksLeft = 0.0; // game ticks until the preview expires
};

// The registry: created once at consumer-mod init, enumerated by the settings UI, never
// destroyed (Cue objects live for the process; UnloadAll only drops backend handles, so a
// cached Cue* can never dangle).

// Create and register a cue. `id` must be unique and CVar-name-safe (e.g. "Ring");
// `name`/`description` are the human-readable strings the settings UI / glossary shows;
// `wavPath` is app-relative (e.g. "assets/accessibility/ring.wav"). All cues loop — the
// backend's one-shot EOF handling has a latent lost-update issue (review CUE3D-13) that
// must be fixed before one-shot cues are added here.
Cue* CueRegistry_Register(const char* id, const char* name, const char* description, const char* wavPath);

// Every registered cue, in registration order. For the settings UI / glossary.
const std::vector<Cue*>& CueRegistry_All();

// Expire timed previews. The consumer mod calls this once per game tick.
void CueRegistry_TickPreviews();

// Stop every cue and drop all backend handles (call right before Cue3D_Shutdown, which
// frees the underlying sources). The Cue objects stay registered and reusable: a later
// Start() would lazily reload. Fixes the dangling-handle latency of review CUE3D-6.
void CueRegistry_UnloadAll();
