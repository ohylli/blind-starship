# HRTF positional audio cues (Steam Audio backend)

The accessibility cues render as real 3D HRTF audio. A blind player gets true
binaural direction (left/right and front/back) plus a distance model, which the
SF64 audio engine cannot do (see `audio-system.md` §9 for that ceiling). This is
now the *only* cue path — the earlier SF64-audio-engine path was removed once the
HRTF backend proved out (see "What was chosen and why" below).

This doc records the **decision and the open questions**. It deliberately does
not duplicate the API or the implementation — those live in the code and its
comments:

- `src/port/accessibility/Cue3D.h` — the backend-agnostic seam: the full API,
  the coordinate convention, and the threading contract. Read this header rather
  than any prose copy of it.
- `src/port/accessibility/Cue3DSteamAudio.cpp` — the Steam Audio + miniaudio
  backend: the −Z axis mapping, the per-source binaural effect, the lock-free
  publish, and the inverse-distance model.
- `src/port/accessibility/Cue.{h,cpp}` — the game-agnostic `Cue` layer over the
  seam: one named cue owns its sound (lazy load + failure latch), its per-cue
  volume CVar, the three-factor gain, a preview mode, and a registry the settings
  UI enumerates. This sits between the consumer mod and the seam.
- `src/port/mods/AccessibilityCues.cpp` — the Star Fox side: registers the two
  cues (ring + enemy) and decides, per game tick, what each one targets.
- `docs/accessibility-cues-tuning.md` — the tunable knobs.

## What was chosen and why

We had been driving cues through the SF64 audio engine, whose limits are no
HRTF, no elevation cue (Y feeds distance only), no front/back in stereo, a
±1200 pan clamp, and asset-pipeline friction (`audio-system.md` §9). To
experiment with whether true spatialization improves cue effectiveness for a
blind player, we moved to real HRTF audio.

We surveyed the options in `audio-backend-alternatives.md` (now superseded by
this decision). **Steam Audio** (a binaural spatializer-only DSP, Apache 2.0)
hosted by **miniaudio** (a single-header playback engine) won over OpenAL Soft:
generally better HRTF, and it supports personalized **SOFA** HRTF import — which
matters for a blind power user. The extra plumbing cost was acceptable *because
both options hide behind the same `Cue3D` seam*, so OpenAL Soft remains a
one-file backend swap if Steam Audio ever disappoints. The backend opens its own
OS audio device alongside libultraship's; the OS mixer combines the streams (the
same coexistence model PRISM/Tolk already use for TTS).

Build integration mirrors PRISM: Steam Audio 4.8.1 (prebuilt zip) and miniaudio
0.11.21 (single header) are fetched at configure time, gated behind the
`HAVE_STEAM_AUDIO` compile def (defined for every non-Switch build); `phonon.dll`
is POST_BUILD-copied next to the exe. See the Steam Audio block in
`CMakeLists.txt`.

### Three layers

The cue code is now split into three layers, so a new cue is a few lines of Star
Fox policy rather than a copy of the whole backend dance:

- **The `Cue3D` seam** (`Cue3D.h` + `Cue3DSteamAudio.cpp`) — the backend. Knows
  Steam Audio and the OS audio device; knows nothing above it.
- **The `Cue` layer** (`Cue.{h,cpp}`) — game-agnostic. Each `Cue` is one named
  cue that owns its sound file, its lazy load (with a failure latch so a broken
  file isn't retried every frame), its per-cue volume CVar, the three-factor gain
  push, and a preview mode. A small registry (`CueRegistry_Register` / `_All` /
  `_TickPreviews` / `_UnloadAll`) lets the settings UI enumerate every cue. This
  layer knows about sounds, volumes, and 3D positions, but nothing about Star Fox.
- **The consumer mod** (`AccessibilityCues.cpp`) — the Star Fox side. Registers
  the two cues ("Ring guide", "Enemy locator") and drives them from its
  `GamePostUpdateEvent` listeners, deciding each tick what each cue targets.

### The SF64-engine path is gone

We had briefly kept the SF64-audio-engine cue path alongside the 3D one, selected
by a `gAccessibilityCue3D` CVar. That path — and the CVar and its F1 checkbox —
have now been **removed** (the CUE3D-1 decision of 2026-06-30 recorded it as
temporary scaffolding). Cues always render through the HRTF backend. The SF64
engine's camera-relative panning and per-frame SFX lifetime made it a dead end for
*continuous* navigation cues; game SFX are reserved for future one-shot flourishes,
not primary cues. One consequence: a build without `HAVE_STEAM_AUDIO` (e.g. Switch)
has no cues at all. The seam still keeps OpenAL Soft a one-file swap away if Steam
Audio ever disappoints.

## Capabilities

Two review findings that had been blocking further cue work are now resolved.
The one-shot re-trigger race (review CUE3D-13 — a lost update between the game
thread's `Play` and the audio thread clearing `playing` at end-of-sound) is
fixed by splitting ownership: the game thread owns `playing` and a `startGen`
counter, the audio thread owns the playback cursor and an advisory `audible`
flag it publishes back each block, and `Cue3D_IsPlaying` reads that flag rather
than `playing` directly. And the start/stop click question (review CUE3D-15) is
fixed with a short (~5 ms) gain ramp the backend applies on every start, stop,
and restart.

On top of that, the seam picked up: reliable one-shot playback (`PlayOnce` on a
`Cue` registered with `CueSpec::loop = false`); a per-source repeat interval for
looping cues (`Cue3D_SetInterval` / `CueTarget::intervalSec` — a geiger-style
restart cadence, independent of pitch); a choice of render mode per source
(`Cue3D_SetMode` / `Cue3DMode`: today's HRTF path, a dry constant-power stereo
pan, or dead-center direct); a per-source low-pass "muffle" independent of the
rear effect (`Cue3D_SetLowPass`); and synthesized-tone cues that need no WAV
(`CueSpec::generator`, run once per voice at `Cue3D_GetSampleRate()`). The exact
contracts — units, defaults, threading — live in the doc comments in `Cue3D.h`
and `Cue.h`; read those rather than a prose copy here.

A live **Cue3D test bench** (F1 → Developer → Blind Starship → "Cue3D test
bench"; `accessibility/CueBench.cpp`) exercises all of the above by ear, with
every control taking effect immediately — no rebuild, no restart. It replaces
the old `SpatialAudioTest` smoke test, which required a restart to change
anything and only ever exercised the orbit.

## Known limitations / open questions

These are the live unknowns to pick up if we return to this — none block the
current cues, but each is a real gap.

- **Volume is now wired; BGM/TTS ducking is not.** The cues run on a second OS
  audio device that the game's audio path never reaches, so the game master volume
  used to skip them. That is fixed: the effective per-source gain is now
  **`gGameMasterVolume` × `gAccessibilityCueMasterVolume` ×
  `gAccessibilityCueVolume.<Id>`** (the game's existing master, deliberately made
  to reach the second device; a new cue-only master, default 1; and a per-cue
  trim, default 1 — e.g. `gAccessibilityCueVolume.Ring` / `.Enemy`). `Cue::PushGain`
  computes this and pushes it via `Cue3D_SetGain` every tick a cue is driven. What
  is **still not wired**: BGM ducking (`SFX_FLAG_19`) does not reach the second
  device, and there is no ducking of the cues while TTS speaks (review CUE3D-16).
  The gain lever those would need now exists, so ducking is a future subtraction
  on top of it rather than new plumbing.
  - *Volume UI.* The F1 → Blind Starship → **Cue volumes** submenu exposes an "All
    cues" master slider, one slider per registered cue (generated from the
    registry, so future cues appear automatically), and a per-cue **Preview**
    button that plays the cue for ~3 s straight ahead at the distance the backend
    renders at unity gain — so the preview's loudness *is* the volume setting.
    Previews suspend gameplay control of that cue and auto-expire via a game-tick
    listener. This is the seed of a planned "cue glossary" (browse cues, hear
    samples).
  - *Pause silence is already solved* — don't re-investigate it. The draw loop
    (and thus `GamePostUpdateEvent`) keeps firing while `gPlayState ==
    PLAY_PAUSE`, so a looping cue kept sounding through an in-level pause;
    `AccessibilityCues_IsPaused()` now gates both listeners so pause stops the
    cue like any other failed precondition. Because the frozen game state means
    the next unpaused tick re-acquires the same target, the cue restarts cleanly.

- **One `Cue` is one voice — simultaneous instances of the same cue (e.g. cueing
  the closest N enemies, not just the closest one) need a small, contained
  extension.** Today a `Cue` owns exactly one backend source, one target, and one
  Idle/Playing/Previewing state, so the same cue cannot sound at two positions at
  once. The backend already supports it (16 independent source slots; each
  `Cue3D_Load` of the same WAV is an independent source), and no call sites or
  seam changes are needed — the extension lives entirely inside `Cue`: split "cue
  definition" (identity, WAV, description, the **one** volume CVar and glossary
  entry — players tune "Enemy locator" once, not per slot) from "voice" (a
  `{source, state, target}` tuple), give the definition a small voice pool with
  an acquire/per-voice-SetTarget API, and keep the current single-voice methods
  as the convenience path so existing cues don't change. Preview stays
  per-definition. Do **not** fake it by registering the same WAV as a second cue —
  that grows a second volume slider and glossary entry, the wrong player-facing
  shape. Non-plumbing work that comes with it: sticky enemy→voice assignment so
  targets don't swap voices frame-to-frame (the click risk this raised, review
  CUE3D-15, is resolved — see "Capabilities" above), per-voice pitch/timbre
  offsets so identical loops stay distinguishable, clip-guard headroom with
  more concurrent voices, and
  (only if it ever matters) a shared-PCM load in the seam, since each voice
  currently keeps its own decoded copy.

- **Spoken failure notice (Class-2) not yet built.** The CUE3D-1 decision deferred
  a one-time spoken PRISM notice for the case where Steam Audio init or asset load
  fails while the OS audio path (and thus the screen reader) is still alive. The
  failure points log today; the spoken notice remains a cheap, narrowly scoped
  follow-up, not yet implemented.

- **Front/back is also weak under the generic HRTF, so the backend exaggerates it.**
  By-ear testing of the all-range enemy cue confirmed the classic front/back confusion:
  the HRTF alone did not make "behind you" readable. `ProduceBlock` now muffles
  (one-pole low-pass), mildly dips, and amplitude-pulses (tremolo) rear-hemisphere
  sources, blended smoothly by the rear angle — front hemisphere untouched. All four
  knobs are live CVar sliders (F1 → Developer → Blind Starship) so the three effects
  can be compared and re-balanced by ear without a rebuild; see
  `docs/accessibility-cues-tuning.md` § "Rear effects". As with elevation below, a
  personalized SOFA HRTF is the path to improving the *underlying* cue, at which point
  the exaggeration could be dialled back.

- **Elevation is weak under the generic HRTF, so Y→pitch was kept on top.** HRTF
  gives a *real* elevation cue from source Y, but with the default
  `IPL_HRTFTYPE_DEFAULT` it only becomes strong when the source is nearly on the
  aim line. A log capture during a ring approach showed the normalized elevation
  `dy` stuck at ~0.07–0.21 (≈4–12°) for most of the approach and only spiking in
  the final half-second — too late to correct the aim. So the old `Y→pitch`
  scheme (`audio-system.md` §8) was **re-introduced** on the 3D path, layered on
  top of the HRTF via `Cue3D_SetPitch`: it carries the vertical signal through
  the long approach (distance-independent) while the HRTF handles
  left/right/front-back and takes over elevation near the target. The path to
  sharpening the HRTF's *own* elevation rendering is a **personalized SOFA HRTF**
  (`IPL_HRTFTYPE_SOFA` + `.sofaFileName`/`.sofaData` — see `phonon.h`), at which
  point the Y→pitch layer could potentially be dialled back.

- **Distance-model elevation handling.** The backend's inverse-distance model is
  *pure* — it uses the full 3D distance including the vertical component, so a
  ring high above reads as genuinely farther (quieter). The SF64 path
  de-emphasised vertical separation (`y/2.5`). If a high target sounds too faint,
  dividing the y component before the distance calc is the equivalent knob to add
  (see the tuning doc's elevation caveat).

## Pointers

- `docs/audio-system.md` — the SF64 audio engine and its limits (§9), cue
  coordinate handling (§6).
- `docs/audio-backend-alternatives.md` — the original backend survey (superseded
  by this doc); preserved for the OpenAL Soft fallback framing.
- `docs/accessibility-cues-tuning.md` — the tunable knobs on the HRTF cues.
- `docs/accessibility-prism-spike-result.md` — the PRISM integration this build
  setup mirrors.
