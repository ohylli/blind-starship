# Audio backend alternatives — feasibility notes

This document captures a discussion held during accessibility-cue exploration about whether to bypass the SF64 audio engine entirely and use a modern third-party audio library for cue playback. We decided to stay on the SF64 engine for now and revisit only if its limitations become a real blocker. These notes preserve the framing so future work can pick the discussion back up without re-deriving it.

## The question

Given the limitations of the SF64 audio engine documented in [audio-system.md](audio-system.md) §9 — no HRTF / elevation cue, no front/back distinction in stereo, the ±1200 pan clamp, polyphony pressure on SFX banks, the YAML-driven asset pipeline for new samples, the `SFX_BANK_SYSTEM` positional gotcha, the split-screen zero-out — could we bypass it for accessibility cues and use the "modern audio backend" libultraship provides for actual playback on modern devices?

## What libultraship's audio surface actually is

libultraship does not provide a modern audio engine in the spatialization or mixing sense. Its public surface in `libultraship/src/public/bridge/audiobridge.h:16` is a single function, `AudioPlayerPlayFrame(buf, len)`: a raw PCM sink wrapping SDL2 / WASAPI. No mixer, no spatializer, no sample loader, no DSP. The SF64 engine in `src/audio/` does all of that, producing the pre-mixed stereo/5.1 buffer that `GameEngine::HandleAudioThread` (`src/port/Engine.cpp:369`) hands off to that sink.

So "bypass game audio, use the modern backend" is not directly meaningful — there is no modern engine waiting underneath. In practice, "bypass" means embedding a third-party audio library that opens its own OS audio device alongside libultraship's, with the OS mixer combining the two streams. PRISM-via-Tolk already follows this pattern for TTS.

## What would motivate a swap

Two classes of pain came up. The first is asset-pipeline and engine-quirk pain: adding new samples requires Torch YAML changes, SFX IDs bake bank/range/flags into a packed 32-bit constant, `SFX_BANK_SYSTEM` silently drops position, per-bank polyphony budgets cap simultaneous cues, split-screen zeroes positions, and iteration speed for cue tuning is slowed by all of the above. The second is the spatial-fidelity ceiling: the engine has no HRTF, Y is not used for pan (no elevation cue), stereo cannot convey "directly behind," X saturates past ±1200, and the camera-relative source frame is the wrong frame for on-rails cues (see `audio-system.md` §6 for the workaround on the last point).

The first class can be addressed by a simpler library; the second class only by an HRTF-capable one.

## Tradeoffs of swapping

Adds another `ExternalProject_Add` dependency, with the integration tax already documented for PRISM in [accessibility-prism-spike-result.md](accessibility-prism-spike-result.md). Loses free integration with game-audio behavior: BGM ducking via `SFX_FLAG_19`, pause silence, the `gGameMasterVolume` CVar — each would need manual wiring. Two audio paths to maintain and debug instead of one. Switch CI builds (`.github/workflows/switch.yml`) would lose any feature not supported by the chosen library; this is not a real concern for this fork because PRISM also doesn't support Switch and Switch is not an accessibility-fork target.

## Library options surveyed

Two tiers, divided by whether the library has true HRTF-based 3D positioning or only amplitude-pan math.

**Tier 1 — HRTF-capable (true 3D):**

- **OpenAL Soft** (https://github.com/kcat/openal-soft) — LGPL-2.1, fine when dynamically linked. Built-in HRTF with multiple datasets, full 3D listener+source API, Doppler. Mature and widely used in games. Most pragmatic option if we go down this path. Pairs with `dr_libs` (already fetched in `CMakeLists.txt:227`) for sample loading.
- **Steam Audio / Phonon** (https://valvesoftware.github.io/steam-audio/) — Apache 2.0. Best-in-class HRTF plus occlusion, reflections, and ambisonics. A DSP library rather than a complete audio engine — needs to be paired with one. Assessed as overkill for cue work.
- **miniaudio + Steam Audio binding** (https://miniaud.io/) — single-header MIT/public-domain audio engine plus Apache plugin. Modern stack, cleaner build than OpenAL Soft, heavier conceptually. miniaudio standalone is not HRTF-capable.

**Tier 2 — no HRTF (would solve asset pipeline and quirks but not the spatial ceiling):**

- **SDL_mixer** (https://github.com/libsdl-org/SDL_mixer) — SDL2 is already a transitive dependency through libultraship, so this has the lowest build-side friction of any candidate. Loads WAV/Ogg/MP3/FLAC directly from `mods/` and gives free polyphony, but `Mix_SetPosition` is amplitude/pan math — the same fundamental ceiling as the SF64 engine.
- **SoLoud** (zlib) and **miniaudio standalone** (MIT / public-domain) — 3D positioning APIs, but amplitude-pan plus distance attenuation under the hood, not HRTF.

## Decision

Stay on the SF64 audio engine for the current cue work. The engine handles X→pan + distance→volume + reverb adequately for most cues, and `audio-system.md` already maps how to drive it cleanly. Revisit if one of two things becomes a real blocker. If iteration speed on cues is genuinely held back by the YAML asset pipeline, **SDL_mixer** is the lowest-friction swap — it solves the asset and polyphony pain without claiming an HRTF gain. If headphone elevation or true 3D turns out to matter for cue effectiveness with blind users, **OpenAL Soft + dr_libs** is the most pragmatic HRTF stack.

Steam Audio in any form was assessed as overkill — its occlusion / reflection / ambisonics features are beyond what cue work needs and would not justify the extra integration complexity over OpenAL Soft.
