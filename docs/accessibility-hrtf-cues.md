# HRTF positional audio cues (Steam Audio backend)

The accessibility cues now render as real 3D HRTF audio instead of through the
SF64 audio engine. A blind player gets true binaural direction (left/right and
front/back) plus a distance model, which the SF64 engine cannot do (see
`audio-system.md` §9 for that ceiling).

This doc records the **decision and the open questions**. It deliberately does
not duplicate the API or the implementation — those live in the code and its
comments:

- `src/port/accessibility/Cue3D.h` — the backend-agnostic seam: the full API,
  the coordinate convention, and the threading contract. Read this header rather
  than any prose copy of it.
- `src/port/accessibility/Cue3DSteamAudio.cpp` — the Steam Audio + miniaudio
  backend: the −Z axis mapping, the per-source binaural effect, the lock-free
  publish, and the inverse-distance model.
- `src/port/mods/AccessibilityCues.cpp` — the two cue consumers (ring + enemy),
  the dual-backend switching, and the lazy load / pause handling.
- `docs/accessibility-cues-tuning.md` — the tunable knobs on both paths.

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

The SF64-engine cue path is **kept but suppressed** while the 3D path is active.
The runtime CVar `gAccessibilityCue3D` (default on) selects the backend for both
cues, and flipping it mid-level switches cleanly. The native path may be removed
later if HRTF proves sufficient, but the SF64-SFX knowledge is preserved in the
tuning doc since the game-native SFX may still be used for other things.

## Known limitations / open questions

These are the live unknowns to pick up if we return to this — none block the
current cues, but each is a real gap.

- **Master volume / BGM integration is not wired.** Running a second OS audio
  device means the cues bypass the game's audio integration: BGM ducking
  (`SFX_FLAG_19`) and the `gGameMasterVolume` CVar do **not** apply to the Steam
  Audio stream. The seam reserves `Cue3D_SetGain` (a distance-independent
  multiplier layered on top of the backend's falloff) as the lever to wire a
  master-volume control by hand, but nothing drives it yet.
  - *Pause silence is already solved* — don't re-investigate it. The draw loop
    (and thus `GamePostUpdateEvent`) keeps firing while `gPlayState ==
    PLAY_PAUSE`, so a looping cue kept sounding through an in-level pause;
    `AccessibilityCues_IsPaused()` now gates both listeners so pause stops the
    cue like any other failed precondition. Because the frozen game state means
    the next unpaused tick re-acquires the same target, the cue restarts cleanly.

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
- `docs/accessibility-cues-tuning.md` — the tunable knobs on both the HRTF and
  SF64-engine paths.
- `docs/accessibility-prism-spike-result.md` — the PRISM integration this build
  setup mirrors.
