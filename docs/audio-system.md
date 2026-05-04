# Starship audio system

This document covers the existing Star Fox 64 audio system as it lives in this fork, with a focus on what is useful for accessibility cues — positional cues that tell a blind player where the closest enemy or hazard is, what direction it is in, and whether it is above or below.

It is meant for two readers:

1. **A software engineer who is new to game audio.** It explains the moving parts at a level that does not assume prior synth/DSP/middleware knowledge.
2. **Future Claude Code sessions.** It maps the concepts to the actual files, types, entry points, and per-frame hooks so we don't have to re-discover the system every time.

The document focuses on what is *practically usable from a port-side accessibility mod*. It does not try to cover the whole N64 audio library faithfully — there's plenty of detail in `include/sf64audio_provisional.h` and the `src/audio/*.c` files for that.

---

## 1. The 90-second mental model

The audio engine is the original Star Fox 64 N64 audio library, decompiled into C, running inside the port. It has three roughly independent paths and one shared mixer at the bottom:

- **BGM / fanfare** — the level music and short jingles (course clear, game over, etc.). Driven by *sequences* (MIDI-like scripts) that play instruments out of a *soundfont* (a small bundle of sampled instruments + envelope/loop metadata, used as the instrument bank for sequences). Does not have a position in the world.
- **Voice** — the captured voice lines from Fox, Falco, Slippy, Peppy, Pepper. Streamed as samples and triggered by an ID. Does not have a position in the world (it's center-channel).
- **SFX** — short, often one-shot effects: lasers, explosions, beeps, alarms, engine loops, footsteps. **This is the only path that knows about 3D position.** The accessibility cues we want fit naturally here.

All three paths feed into the same synthesis stage, which produces a final stereo (or 5.1) buffer that libultraship hands off to the OS audio device.

```
   game code ─┬─► SEQ_PLAYER_BGM ──────┐
              ├─► SEQ_PLAYER_FANFARE ──┤
              ├─► SEQ_PLAYER_VOICE ────┤
              └─► SEQ_PLAYER_SFX ──────┴─► synthesis ─► AI buffer ─► libultraship audio player
```

The unit of work in the synthesizer is a **note**: one running voice, one sample being resampled, panned, and amplitude-enveloped. There is a fixed pool of notes and the engine allocates from it; the pool is small (single digits of simultaneous SFX per "bank"), so polyphony is limited and the engine evicts low-priority sounds when it runs out.

A small surprise from the N64: even SFX are played by a sequence player. Each playing SFX runs as a tiny sequence script on one channel of `SEQ_PLAYER_SFX`. The reason is hardware history: on the N64, the same RSP routine handled both music and sound effects, so everything was funneled through the sequencer. This rarely matters for cue work — we don't author sequences, the engine already has one for every sound — but it's why you'll see "sequence channel" come up when reading the SFX code.

For accessibility cues, **the only API surface we touch is one game-side function** (`Audio_PlaySfx`) **plus a handful of helpers for stopping sounds and converting world positions to camera space.** The rest is the engine's job. We don't have to write a synth, we don't have to know how the note pool works, we just need an SFX ID and a vec3.

---

## 2. Files and what each one is for

Game-side audio (decompiled SF64 C code), all in `src/audio/`:

| File | What it is for |
|------|----------------|
| `audio_general.c` | The high-level interface. Owns `sSfxRequests`, `sSfxBanks`, `Audio_PlaySfx`, BGM/fanfare/voice sequencing, the falloff/pan/pitch math, audio spec switching. **This is where accessibility code mostly interacts.** |
| `audio_thread.c` | The audio thread loop on the game side. Drains `AudioCmd` queues, calls into synthesis. |
| `audio_seqplayer.c` | Sequence player: parses `.seq` data, walks channel/layer scripts, allocates notes. Used by all four `SEQ_PLAYER_*` players. |
| `audio_playback.c` | Per-note state machine. `Audio_InitNoteSub` (line 68) is where the SFX bank's `pan` (0–127) becomes per-speaker gains (`gHeadsetPanVolume[]`, `gStereoPanVolume[]`, surround cosine panning). The 5.1 cosine-pan block is around line 142. |
| `audio_synthesis.c`, `audio_load.c`, `audio_heap.c`, `audio_effects.c`, `mixer.c`, `note_data.c`, `wave_samples.c`, `audio_context.c` | Synthesis, asset loading, internal lookup tables, ADSR/vibrato. **Not relevant to cue work.** |

Game-side headers:

| File | Contents |
|------|----------|
| `include/sf64audio_external.h` | Public game-facing API (`AudioType`, `AudioSpecID`, `Audio_SetVolume`, `Audio_PlayVoice`). |
| `include/sf64audio_provisional.h` | Full audio struct dump. Big and authoritative — `SfxRequest`, `SfxBankEntry`, `SequencePlayer`, `Note`, `NoteSubEu`, sample/instrument/drum types. Use this to look up types when reading the C code. |
| `include/sfx.h` | The SFX ID list (`NA_SE_*`), `AUDIO_PLAY_SFX` macro, `SFX_BANK_*` enum, `SFX_FLAG_*`, `SFX_RANGE`/`SFX_PACK` helpers. **The list of every SFX the game can play lives here.** |
| `include/bgm.h` | The BGM ID list (`NA_BGM_*`) and `AUDIO_PLAY_BGM` macro. |
| `include/audioseq_cmd.h`, `include/audiothread_cmd.h` | The `SEQCMD_*` and `AUDIOCMD_*` macros — the queued message format used to talk to the audio thread. `SEQ_PLAYER_BGM`/`FANFARE`/`SFX`/`VOICE` enum is in `audioseq_cmd.h:9`. |

Port-side audio bridge:

| File | What it is for |
|------|----------------|
| `src/port/audio/GameAudio.h` | Tiny shared struct holding the audio thread, mutex, and condvars. |
| `src/port/Engine.cpp` | `GameEngine::HandleAudioThread` (line 369) — the `std::thread` that calls `AudioThread_CreateNextAudioBuffer` and pushes the result into `AudioPlayerPlayFrame`. `gGameMasterVolume` CVar is applied here (line 176 of `audio_playback.c`). |
| `libultraship/src/public/bridge/audiobridge.h` | The audio output API the port uses: `AudioPlayerPlayFrame`, `AudioPlayerBuffered`, `GetNumAudioChannels` (returns 2 or 6), `GetAudioChannels` (stereo / 5.1). |

Where SFX positions are *converted from world to camera space* (the producer side of the position vector):

| Function | File:line | What it does |
|----------|-----------|--------------|
| `Object_SetSfxSourceToPos(f32* sfxSrc, Vec3f* worldPos)` | `src/engine/fox_edisplay.c:1578` | World position in, camera-space position out. **The canonical "convert world position into something `Audio_PlaySfx` will accept" function.** Rotates by camera yaw/pitch (no axis flip) and clamps the result. |
| `Object_SetSfxSourceToView(f32* sfxSrc, Vec3f* viewPos)` | `:1614` | Skips the camera transform. Use when the caller already has the position in view space. |
| `Object_UpdateSfxSource(f32* sfxSrc)` | `:1598` | Re-projects an already-stored source through a stashed view matrix — used to keep moving sources in sync without recomputing the world position. |
| `Object_ClampSfxSource(f32* sfxSrc)` | `:1557` | Clamps to ±5000 in X/Z and ±2000 in Y. All the `SetSfxSource*` helpers run this at the end. |
| `Display_SetupPlayerSfxPos` | `src/engine/fox_display.c:1747` | Sets the player's own sfx source and computes its velocity (used for Doppler). |

---

## 3. The SFX ID is a packed 32-bit value

A single `NA_SE_*` constant in `sfx.h` is not just a sound name — it carries metadata that the audio engine reads to decide *how* to play the sound. From `sfx.h:38–68`:

```
SFX_BANK   bits 31..28   which SFX bank (0=player, 1/2/3=generic, 4=system)
flags      bits 18..23 and bit 27   per-effect modifiers (see flag table)
SFX_STATE  bit 24        always 1 on real SFX (the engine treats 0 as "no sound")
                         (bits 25..26 are unused)
SFX_RANGE  bits 16..17   short / medium / long / very-long max audible range
SFX_IMPORT bits 8..15    importance, used to decide who survives when polyphony is full
SFX_INDEX  bits 0..7     the index into the bank's sample table
```

A worked example. `NA_SE_EN_EXPLOSION_M` is `0x2903B009`:

- nibble 31..28 = `0x2` → `SFX_BANK_2` (a generic bank, so this *is* positional).
- nibble 27..24 = `0x9` = `1001` → bit 27 set (`SFX_FLAG_27`, allow duplicates) and bit 24 set (`SFX_STATE`).
- byte 23..16 = `0x03` → bits 16 and 17 set, so `SFX_RANGE = 3` (the longest audible range tier).
- byte 15..8 = `0xB0` → importance = 0xB0 (high; this won't get evicted easily).
- byte 7..0 = `0x09` → index 9 into bank 2.

You almost never construct an ID from scratch. For cues, just pick a `NA_SE_*` from `sfx.h` that sounds appropriate, or define a new one (and add the asset) if a unique sound is needed.

### The five SFX banks

`SfxBankId` in `sfx.h:70`:

- `SFX_BANK_PLAYER` (0) — the player's own ship sounds (laser, boost, brake, engine).
- `SFX_BANK_1` (1), `SFX_BANK_2` (2), `SFX_BANK_3` (3) — generic banks for environment and enemy SFX. The split is loose; many sounds appear in only one bank. **For positional cues, pick from these banks.**
- `SFX_BANK_SYSTEM` (4) — UI / HUD / non-positional alarms (cursor, decide, lock-on tone, low-shield warning, mission-clear).

⚠ **Important gotcha:** `Audio_SetSfxProperties` (`audio_general.c:515`) only computes pan / falloff / reverb / freq-mod for banks 0–3. **For `SFX_BANK_SYSTEM`, position is ignored** — pan stays at center (64), volume stays at full, distance does not attenuate. So if a positional cue uses a system-bank `NA_SE_*`, the vec3 will silently do nothing. Choose a sound from a player or generic bank, or define a new one in those banks.

A second silent-drop pitfall: if `sSfxBankMuted[bankId]` is set, `Audio_PlaySfx` returns without queuing the request and there is no error or log. If a cue inexplicably doesn't play, check the mute state of its bank.

Each bank has its own ring buffer of entries (`sSfxBanks[5][20]`) and its own simultaneous-channel budget (`sChannelsPerBank` in `audio_general.c:67`). The 5.1 layout (row index 3 in the table) is `{ 6, 0, 2, 0, 4 }` — player bank gets 6, system bank gets 4, banks 1 and 3 get 2, bank 2 gets 0. The exact numbers vary per channel layout.

### SFX flags

From `sfx.h:40–47`:

| Flag | Effect |
|------|--------|
| `SFX_FLAG_18` | Distance ignores Z (treats source as 2D) — **only when source is in front of camera (zPos > 0)**. |
| `SFX_FLAG_19` | Temporarily ducks the BGM channel while this SFX plays. |
| `SFX_FLAG_20` | Priority ignores distance — the SFX always wins on importance alone. |
| `SFX_FLAG_21` | Reverb is *not* boosted by distance (reverb stays at the script-set level). |
| `SFX_FLAG_22` | Volume is *not* attenuated by distance (the SFX is at full volume regardless). |
| `SFX_FLAG_23` | Adds a small random per-frame pitch wobble ("noisy"). |
| `SFX_FLAG_27` | Allow duplicate concurrent requests with the same `(bankId, source)`. |

For accessibility cues, `SFX_FLAG_22` (constant volume) and `SFX_FLAG_20` (always priority) are useful when we want the cue to be reliably audible at any distance. `SFX_FLAG_18` is useful when altitude shouldn't affect distance attenuation. Conversely, leaving these flags off gives natural distance-based volume falloff for free.

Flags are **baked into the `NA_SE_*` constant** — the engine reads them out of the SFX ID, not from a runtime parameter. We pick or assign flags when defining the sound, not on a per-call basis.

---

## 4. What the engine does with a source position

The single most important function is `Audio_GetSfxPan` in `audio_general.c:455`. Reading its body once is worth more than a long explanation.

The source vector is `f32[3]` = `[x, y, z]`, **in camera space**:

- **X**: positive = right of the camera, negative = left.
- **Y**: positive = above the camera, negative = below.
- **Z**: positive = in front of the camera, negative = behind.

This matches SF64's world-space convention: the player flies in the world's +Z direction (the path direction), and `Object_SetSfxSourceToPos` rotates the world delta by camera yaw/pitch with no axis flip, so view-space +Z is also "forward". (This is the opposite of the OpenGL convention where −Z is forward — the engine does not use that convention.)

### Pan: where the sound sits in left/right (or surround) — the `pan` field, 0..127

In stereo (`GetNumAudioChannels() == 2`), `Audio_GetSfxPan` clamps `|x|` and `|z|` to 1200, then computes a 0..127 pan value (64 = center). The math has a "front cone" and a "side" branch:

- If the source is roughly in front of the camera (`|z| > |x|`), the pan is dominated by `x / z` and is gentle (`pan = x / (2.5 * |z|) + 0.5`). Front-and-slightly-right gives only a slight right pan.
- If the source is to the side (`|x| >= |z|`), the pan saturates aggressively toward the corresponding ear.
- A degenerate `(0, _, 0)` returns center.

In 5.1 (`GetNumAudioChannels() == 6`), `atan2f(x, -z)` produces a full-circle angle which is then mapped onto 0..127 pan, and `Audio_InitNoteSub` (`audio_playback.c:142`) uses a configurable per-speaker cosine panning law (CVars `gPositionFrontLeft/Right`, `gPositionRearLeft/Right`) to actually distribute volume across the four corner speakers. 5.1 only kicks in if the OS audio device is configured for 6 channels — on stereo headphones the surround math is irrelevant regardless of how the source is positioned.

There is also a "binaural-ish" channel layout `SFXCHAN_3` that hard-pans by token instead of by position — not the default, but worth knowing exists.

**Y is not used in pan** in either layout. There is no head-related elevation cue out of the box.

A small special case: for player-bank SFX whose source is "right on top of the camera" (`-200 < z < 200`), pan is forced to center. This avoids hard left/right swings on the player's own ship sounds.

⚠ **Stereo can't convey "directly behind."** In stereo, a source straight in front and a source straight behind both produce roughly center-pan. Headphone-only listeners get no front/back cue from the engine. 5.1 covers this; for stereo we'd have to do something else (a different sound for "behind", a delay, or a spectral filter — but none of that exists today).

### Volume: distance falloff — the `volMod` field, multiplicative

`Audio_GetSfxFalloff` in `audio_general.c:388` uses `entry->distance`, which is `sqrt(x² + (y/2.5)² + z²)` — note the **y/2.5 scale**: vertical separation counts for less than horizontal separation. The falloff curve has three regions; the values below are the *pre-square* numbers, and the function squares them at the end (`falloff = SQ(falloff)`), so the perceived curve is quadratic:

- 0 .. range/5: full volume (1.0 → 1.0 squared).
- range/5 .. range: gentle taper (down to 0.81 → 0.66 squared).
- range .. 33000: linear ramp to zero (squared, so quadratic in practice).
- > 33000: silent.

`range` is set by the SFX's `SFX_RANGE` field. The four range tiers (0..3) give 33000/20, 33000/15, 33000/10.5, 33000/5.2 respectively — higher range → audible from farther.

### Pitch: distance and randomness — the `freqMod` field, multiplicative

`Audio_GetSfxFreqMod` in `audio_general.c:494` does two things:

- A small upward pitch shift with distance (up to +20% at max range). Subtle; mostly used to give distant sounds a slightly different timbre.
- If `SFX_FLAG_23` is set, a random per-frame pitch wobble in the −8% range.

We can also apply our own pitch by passing a non-default `freqMod` pointer to `Audio_PlaySfx`. The engine reads `*entry->freqMod` every audio update and **multiplies it on top of the engine's distance pitch shift**. So if we want a clean "Y → pitch" mapping for a cue, either set `SFX_FLAG_22` (which also disables the +20% distance pitch — see line 502) or accept up to 20% upward drift at long range. For most cue work the drift is negligible.

A small additional quirk: `Audio_GetSfxFreqMod` (line 509) bumps the result by ×1.1 when `(token & 2)` is set and the channel layout is anything other than `SFXCHAN_0`. For most layouts this means a token of 2, 3, 6, 7, … gets a hidden 10% pitch bump. If a cue chooses tokens for de-dup purposes only, prefer 0, 1, 4, or 5 to avoid the bump.

### Reverb: distance-driven — the `reverbAdd` field, additive 0..127

`Audio_GetSfxReverb` in `audio_general.c:432` adds a distance-proportional reverb (up to 40 over a quarter of max range), plus contributions from the SFX script (`seqScriptIO[6]`), an environment-specific `sEnvReverb`, and an audio-spec-level `sAudioSpecReverb`. We can also pass our own per-call `reverbAdd` pointer.

### Doppler shift (player-only by default)

`Audio_UpdateDopplerShift` in `audio_general.c:574` applies a Doppler factor based on relative speed (`(distance_now - distance_next) / soundSpeed`). It's wired into a few player-engine SFX (`Audio_UpdateEngineSfx`) but not into arbitrary SFX. To add Doppler to a cue, compute it ourselves and write the factor into the `freqMod` slot we own.

---

## 5. The exact API for "play a sound at this 3D position"

The macro most game code uses (defined in `sfx.h:38`):

```c
#include "sfx.h"  // already pulled in via global.h

AUDIO_PLAY_SFX(NA_SE_LOCK_ON, sourcePosVec3, token);
```

`AUDIO_PLAY_SFX` is sugar for the underlying call:

```c
Audio_PlaySfx(sfxId,
              sourcePosVec3,        // f32[3], camera-space, kept alive across frames
              token,                // u8 used for de-dup / matching
              &gDefaultMod,         // f32*: extra freq multiplier; default 1.0
              &gDefaultMod,         // f32*: extra volume multiplier; default 1.0
              &gDefaultReverb);     // s8*: extra reverb to add; default 0
```

`gDefaultSfxSource`, `gDefaultMod`, and `gDefaultReverb` live at `audio_general.c:77–80`. Pass `gDefaultSfxSource` (a static `{0,0,0}`) when you want a non-positional cue.

⚠ **Critical: the vec3 must outlive the call.** The engine stores the *pointer* in the bank entry; it re-reads the position every audio frame for as long as the entry is alive. Passing a stack vec3 from a function that returns will read freed memory on the next audio frame.

The same is true for `freqMod`, `volMod`, and `reverbAdd` — pass pointers to live variables (statics in the mod, or fields on a mod-owned struct), not transient stack values. This is also what makes "moving sources" work without an extra API: just keep updating the same `f32[3]`.

`token` is mostly opaque to the engine but is consulted by the kill helpers and a few pan side-cases. For player-attached sounds the convention is to pass the player ID; otherwise 0 or 4 are common.

### Stopping cues

In `sfx.h:11–17`:

- `Audio_KillSfxBySource(sourcePtr)` — kill anything pointing at this vec3 across all banks.
- `Audio_KillSfxBySourceAndId(sourcePtr, sfxId)` — kill a specific ID at this source.
- `Audio_KillSfxById(sfxId)` — kill every active instance of this SFX.
- `Audio_KillSfxByTokenAndId(token, sfxId)` — kill matching token+id pairs.
- `Audio_KillSfxByBank(bankId)` — kill every cue in a specific bank.
- `Audio_KillSfxByBankAndSource(bankId, sourcePtr)` — kill all SFX from a specific bank at a source.
- `Audio_StopSfxByBankAndSource(bankId, sourcePtr)` — stop (don't fully evict) at a source.
- `Audio_KillAllSfx()` — nuke everything.

Implementations live around `audio_general.c:1615–1750`. For a continuous cue (looping engine, alarm), the entry stays alive as long as the SFX has the "loop" state in its sequence script — usually until you call one of these. For a one-shot cue, the engine drops the entry on its own when the sequence finishes.

### The two structs you'll see in the code

From `sf64audio_provisional.h`:

- `SfxRequest` (size 0x18) — the queue entry built by `Audio_PlaySfx`. Fields: `sfxId, source, token, freqMod, volMod, reverbAdd`. Lives in `sSfxRequests[256]` (a ring buffer).
- `SfxBankEntry` (size 0x30) — the longer-lived per-bank record. Adds `xPos/yPos/zPos` (split-out pointers into the source vec3), `distance`, `priority`, `state`, `freshness`, `prev/next/channelIndex`. Lives in `sSfxBanks[5][20]`.

You usually don't need to manipulate these directly — `Audio_PlaySfx` builds the request, `Audio_ProcessSfxRequest` migrates it into a bank entry. They're worth knowing if you're tracing a cue through the audio frame.

---

## 6. Coordinate handling for accessibility cues

This is the part that matters for the planned cues. The game already maintains, every frame, the world-space position of every active actor (player, enemies, hazards, item rings, lock-on targets) in their `obj.pos` field. Many actors *also* maintain a `sfxSource[3]` already in camera space.

For a new cue ("play this sound from where the closest enemy is"), the path is:

1. Find the world-space `Vec3f` of the target (e.g. `gActors[i].obj.pos`).
2. Run it through `Object_SetSfxSourceToPos(myCueSrc, &targetWorldPos)` *each game frame* if the target moves. For a single-shot cue, calling it once is fine.
3. Call `Audio_PlaySfx(cueSfxId, myCueSrc, token, &cueFreqMod, &cueVolMod, &cueReverb)`.

`Object_SetSfxSourceToPos` already handles:

- Translation: the actual line is `src.z = pos->z + gPathProgress - gPlayCamEye.z`. `gPathProgress` is added to the *target's* world Z to compensate for the running rail-shooter path offset, then `gPlayCamEye` is subtracted. X and Y are simple `pos - camEye`.
- Rotation by `gPlayer[0].camYaw` and `camPitch`, no axis flip, so the result is in camera space with +Z = forward.
- Clamping to ±5000 X/Z and ±2000 Y so absurd distances don't break the math.

Things to be aware of:

- ⚠ **Split-screen multiplayer zeros all SFX positions.** `Object_SetSfxSourceToPos` and friends bail out and write `(0,0,0)` when `gCamCount != 1`. Positional cues only work in single-player mode out of the box. For a multiplayer-safe path we'd have to bypass the camera-relative transform and drive `AUDIOCMD_CHANNEL_SET_PAN/VOL_SCALE` per pane ourselves.
- **Pan saturates aggressively past `|x| ≈ 1200`.** That's because `Audio_GetSfxPan` clamps `|x|` (and `|z|`) to 1200 before computing pan; anything beyond reads as "directly to the side." For fine directional discrimination we may want to constrain X before passing it in, or bypass `Audio_GetSfxPan` entirely.
- **Y does not contribute to pan.** It contributes to falloff only (and even there, ÷ 2.5). To convey "above vs below" we map Y onto pitch — see Section 8.
- **SFX outside max range are silently dropped.** Set `SFX_FLAG_22` on the cue if it must be audible regardless of distance.

---

## 7. The audio thread, timing, and per-frame hooks

`Audio_PlaySfx` does *not* directly start a sound. It writes a request into a ring buffer that is drained on the next audio update. The audio update is driven by `GameEngine::HandleAudioThread` in `src/port/Engine.cpp:369`, which runs in its own `std::thread`, signalled from the game loop via condvars. Each iteration calls `AudioThread_CreateNextAudioBuffer`, ticks the SFX bank state machines via `Audio_Update`, advances sequences, and pushes synthesized samples to libultraship via `AudioPlayerPlayFrame`.

Practical consequences:

- **There is ~one audio frame of latency** (~33 ms) between calling `Audio_PlaySfx` and the sound starting. Imperceptible for cue work.
- **Don't call `Audio_PlaySfx` from a tight loop hoping for sample-accurate retriggering.** The same SFX ID + same source pointer will match an existing entry and be folded into it (importance breaks ties). Use `SFX_FLAG_27` if you actually want duplicate concurrent instances.
- **Position pointers are read on the audio update, not at call time.** As long as we keep the vec3 stable across frames, we don't need to re-call `Audio_PlaySfx`.

### Where to hook a per-frame position update

The port's event bus (`src/port/hooks/`) defines several per-frame events:

- `GamePreUpdateEvent` / `GamePostUpdateEvent` — `EngineEvent.h`, fired around each game update tick (`fox_game.c:352`, `fox_game.c:614`).
- `PlayUpdateEvent` — `EngineEvent.h`, **cancellable wrapper** around `Play_Update()` (`fox_play.c:7169`). Fires *before* `Play_Update` runs, so the camera has not yet been updated when the listener fires.
- `PlayerPreUpdateEvent` / `PlayerPostUpdateEvent` — `EngineEvent.h`, fired around each player's update (passes `Player*`).
- `ObjectUpdateEvent` — `ActorEvent.h`, per-actor (passes the actor).

For positional cues, **`GamePostUpdateEvent` is the right hook**: it fires once per game tick after `Play_Update()` has run, so the camera transform we feed `Object_SetSfxSourceToPos` is current. `PlayUpdateEvent` is *not* what we want — it's a cancellable wrapper that fires *before* the camera updates, so the cue's position would lag one frame behind the visuals. `PlayerPostUpdateEvent` is also fine if the cue is naturally per-player.

### Bypassing the bank machinery

If a cue's logic doesn't fit the bank model and we need to drive `SEQ_PLAYER_SFX` channels by hand, the macros in `include/audiothread_cmd.h` are the way: `AUDIOCMD_CHANNEL_SET_PAN`, `AUDIOCMD_CHANNEL_SET_VOL_SCALE`, `AUDIOCMD_CHANNEL_SET_FREQ_SCALE`, `AUDIOCMD_CHANNEL_SET_REVERB_VOLUME`, `AUDIOCMD_CHANNEL_SET_PAN_WEIGHT`. These are exactly what `Audio_SetSfxProperties` already uses internally; reach for them only if the bank/queue model gets in our way.

---

## 8. A worked recipe for a positional accessibility cue

This is what the next cue mod should look like, end-to-end. It mirrors the existing port-mod conventions (`src/port/mods/PortEnhancements.{c,h}`, `src/port/mods/Accessibility.{c,h}`): static state, CVar-gated, listener-registered at init.

### Where the code lives

Unlike the TTS path — which has a port-level transport at `src/port/accessibility/` (PRISM init/shutdown) and a consumer mod at `src/port/mods/Accessibility.{c,h}` — **positional cues need no port-level transport**. The SFX engine *is* the transport. A cue mod is purely consumer-mod material and lives entirely under `src/port/mods/`.

The headers a cue mod typically needs: `global.h` (umbrella for game C code, brings in `Vec3f`, `Actor`, `f32`/`s32`), `sfx.h` (SFX IDs and `AUDIO_PLAY_SFX`), `port/hooks/Events.h` (umbrella for the event bus), and `<math.h>` (only if you call `powf` for pitch).

### Triggering vs ticking

Following the project's event-driven announcement pattern (see `CLAUDE.md`'s "Accessibility fork" section): use the event bus to **start and stop** cues at semantic transitions, and use `GamePostUpdateEvent` (or `PlayerPostUpdateEvent`) to **refresh per-frame state** (position vec3, pitch from Y, etc.) for any active cue. Don't poll game state to detect the trigger — fire a `DEFINE_EVENT(...)` at the precise transition and listen for it. If the trigger you want doesn't yet have an event, follow CLAUDE.md's recipe: add `DEFINE_EVENT` in the appropriate `src/port/hooks/list/*.h`, `REGISTER_EVENT` in `PortEnhancements_Register()`, and `CALL_EVENT` at the producer site.

### Example: a one-shot positional cue

```c
// src/port/mods/AccessibilityCues.c
static f32 sHazardSrc[3];           // camera-space pos, must outlive the call

void AccessibilityCues_HazardAppeared(Vec3f* worldPos) {
    if (!CVarGetInteger("gAccessibility.PositionalCues", 0)) return;
    Object_SetSfxSourceToPos(sHazardSrc, worldPos);
    AUDIO_PLAY_SFX(NA_SE_<some_short_tone>, sHazardSrc, /*token=*/0);
}
```

The vec3 is a file-static, so it lives forever; the engine can safely re-read it every audio frame even after the function returns. Once the SFX's sequence finishes, the engine drops the entry.

### Example: a moving cue with X→pan, distance→volume, Y→pitch

```c
// src/port/mods/AccessibilityCues.c
static f32 sCueSrc[3];
static f32 sCueFreqMod = 1.0f;      // we own this; the engine reads it every audio frame
static f32 sCueVolMod  = 1.0f;
static s8  sCueReverb  = 0;
static Actor* sCueTarget = NULL;

void AccessibilityCues_StartTracking(Actor* tgt) {
    sCueTarget = tgt;
    Object_SetSfxSourceToPos(sCueSrc, &tgt->obj.pos);
    Audio_PlaySfx(NA_SE_<your_loop_tone>,
                  sCueSrc, /*token=*/0,
                  &sCueFreqMod, &sCueVolMod, &sCueReverb);
}

// Listener signature: void(IEvent*). The IEvent* points at the event struct
// (e.g. cast to GamePostUpdateEvent* — but this event has no extra fields).
static void OnGamePostUpdate(IEvent* ev) {
    if (sCueTarget == NULL) return;
    if (!CVarGetInteger("gAccessibility.PositionalCues", 0)) return;
    // Refresh camera-space position for the audio engine to re-read.
    Object_SetSfxSourceToPos(sCueSrc, &sCueTarget->obj.pos);
    // Map camera-space Y to pitch. sCueSrc[1] is already camera-space Y.
    // 1 octave per 1000 units, clamped, is a reasonable starting point.
    f32 octaves = sCueSrc[1] / 1000.0f;
    if (octaves >  1.0f) octaves =  1.0f;
    if (octaves < -1.0f) octaves = -1.0f;
    sCueFreqMod = powf(2.0f, octaves);
}

void AccessibilityCues_Stop(void) {
    sCueTarget = NULL;
    Audio_KillSfxBySource(sCueSrc);
}

void AccessibilityCues_Init(void) {
    REGISTER_LISTENER(GamePostUpdateEvent, OnGamePostUpdate, EVENT_PRIORITY_NORMAL);
}
```

`REGISTER_LISTENER` takes three arguments — eventType, callback, priority — see `EventSystem.h:65` and the existing usage in `Accessibility.c:69–72`. The callback signature is `void(IEvent*)`, not `void(void*)`. CVar-gating happens at the start/stop call sites and inside the listener (so it's toggleable mid-cue), not at registration — the same pattern as `Accessibility_IsScreenReaderEnabled()` in the TTS layer. Use `CVarGetInteger("g...", default)` (see existing usage in `PortEnhancements.c`).

### Sample-choice gotcha

Pitch-shifting via `freqMod` shifts the **whole sample's** spectrum — fine for a clean tonal cue (a sine-ish beep), but a recognizable timbre (a voice clip, a textured explosion) will sound wrong when shifted ±1 octave. Pick or author a tonal sample for any cue that uses pitch as a dimension.

### Adding a brand new sound

If none of the existing `NA_SE_*` IDs fit the cue, adding a new short sample is more involved than dropping a `.wav`: it has to be registered in the asset pipeline. The relevant YAMLs live in `assets/yaml/{us,jp,eu,cn}/rev{0,1}/audio*` and Torch generates code into `src/assets/`. This is unexplored territory in this fork; expect it to take a few hours the first time. PRISM-driven TTS (`src/port/accessibility/`) is a separate, simpler path for *spoken* cues, but it sits outside this audio system entirely and isn't suited to short positional tones.

---

## 9. Capabilities and limitations, summarized

What the system gives us for free with one `Audio_PlaySfx` call and a camera-space vec3:

- Stereo pan from X position. Full surround in 5.1.
- Volume falloff from 3D distance (Y counts ÷ 2.5).
- Distance-based reverb tail.
- Distance-based pitch shift (modest; up to +20%).
- Per-call freq/vol/reverb modulation via pointer parameters that the engine re-reads every frame.
- Polyphony/priority management — the engine evicts low-importance SFX when oversubscribed.
- Doppler is available but only wired up for the player engine SFX. Easy to add to a cue mod by writing into the `freqMod` slot we own.

What the system does **not** give us:

- Elevation cue (Y → ear). No HRTF, no high-shelf-from-above. Y feeds into distance only. Encoding "above/below" requires a second cue dimension we drive ourselves; pitch is the natural fit.
- A front/back distinction in stereo. Headphones-only listeners get no front/back cue; 5.1 covers it.
- Reliable behavior in split-screen multiplayer. Source positions get zeroed in 2P+.
- Positional behavior on `SFX_BANK_SYSTEM` sounds. They always play center, full volume.
- Arbitrary samples without going through the asset pipeline.

---

## 10. Reading guide for accessibility-cue work

When the next task is "play a positional cue when X happens," the reading order is:

1. `include/sfx.h` — pick a sound. Note the bank, range, and flags it carries; verify it's not in `SFX_BANK_SYSTEM`.
2. `src/audio/audio_general.c:1280` — `Audio_PlaySfx` and the request queue.
3. `src/audio/audio_general.c:455` — `Audio_GetSfxPan` to know what your X/Z position will mean.
4. `src/audio/audio_general.c:388` — `Audio_GetSfxFalloff` for what your distance will mean.
5. `src/engine/fox_edisplay.c:1578` — `Object_SetSfxSourceToPos` for the world-to-camera-space conversion.
6. `src/port/hooks/list/EngineEvent.h` and `src/port/hooks/list/ActorEvent.h` — pick a per-frame event to register on (usually `GamePostUpdateEvent`, since `PlayUpdateEvent` fires before the camera updates).
7. `src/port/mods/PortEnhancements.{c,h}` and `src/port/mods/Accessibility.{c,h}` — worked examples of CVar-gated, listener-registered mods that match the pattern positional cues should follow.
8. `src/mods/sfxjukebox.c` — a worked example of calling `AUDIO_PLAY_SFX` from a port-level mod.

The recommended cue scheme — X → pan, Y → pitch, distance → volume — uses three orthogonal psychoacoustic dimensions, requires no HRTF or DSP work, and slots cleanly into the engine's existing positional path on banks 0–3. For a clean tonal sample, the engine handles X → pan and distance → volume on its own; the cue mod only has to drive `freqMod` from camera-space Y.
