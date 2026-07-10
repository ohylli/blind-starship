# Tuning the accessibility audio cues

The cues render through the Steam Audio HRTF backend (see
`docs/accessibility-hrtf-cues.md`). There is no longer a second SF64-audio-engine
path — it was removed, along with the `gAccessibilityCue3D` toggle. The pieces are:

- **The `Cue` layer** (`src/port/accessibility/Cue.{h,cpp}`) — game-agnostic: each cue owns its sound file, its per-cue volume CVar, the three-factor gain, and a preview mode. A registry lets the settings UI enumerate cues.
- **The consumer mod** (`src/port/mods/AccessibilityCues.{cpp,h}`) — the Star Fox side, housing two cues sharing the `gAccessibilityAudioCues` CVar:
  - **Ring cue** — tracks the next Training ring ahead of the Arwing (scoped to `LEVEL_TRAINING`).
  - **Enemy cue** — tracks the closest cueable enemy ahead of the Arwing's aim line, on any on-rails level. Coordinate frame is body-frame (rotated by the player's yaw + pitch) rather than world-frame; see `docs/accessibility-enemy-cue.md` for the derivation.

Direction is now the HRTF backend's job (true left/right and front/back). The mod still drives pitch from altitude via `AccessibilityCues_ComputeFreqModFromY`, layered on top of the HRTF (the generic HRTF's own elevation cue is weak — see `docs/accessibility-hrtf-cues.md`). Distance drives volume through the backend's inverse-distance model. Most knobs below apply to either cue; function names are prefixed `Ring` or `Enemy` to disambiguate.

For background on *why* the cues are shaped this way (player-relative coordinate frame, Y→pitch instead of Y→pan), see `docs/audio-system.md` and `docs/game-world.md`.

---

## The knobs, in order of "most likely to matter"

### The sound file

Each cue loads a WAV from `assets/accessibility/` (`ring.wav`, `enemy.wav`), named in `AccessibilityCues_Init` where the cues are registered. Swap the file to change the sound. Favor sounds with clearly different timbres between the two cues, so both can fire simultaneously on Training without blending into one sound. Unlike the old SF64-SFX path, there are no bank/flag/range constraints — the HRTF backend plays an arbitrary mono WAV; distance, direction, and pitch are all applied by the backend or the mod, not baked into the sound.

### Ring cue distance falloff

Two knobs near the top of `src/port/accessibility/Cue3DSteamAudio.cpp`, controlling the HRTF distance model for every cue.

Unlike the SFX engine — which folds distance, pan, and elevation into one opaque path — the HRTF backend splits the jobs: Steam Audio's binaural effect handles *direction*, and a separate inverse-distance model handles *loudness-vs-distance*. `ProduceBlock` calls `iplDistanceAttenuationCalculate` (type `INVERSEDISTANCE`, gain ≈ 1/d past the near plateau) once per audio block, using the magnitude of the player-relative position the cue pushes through `Cue3D_SetPosition`.

The wrinkle: Steam Audio measures distance in **meters**, but SF64 world units are huge (rings spawn ~3000 units ahead, clamp box ±5000). Feeding raw units to a 1/d model would be near-silent, so we scale units → meters first.

- `kWorldUnitsPerMeter` (default 1000) — overall steepness/loudness. It sits in the numerator of the gain (`1 / (units / thisKnob)`), so **larger = louder and flatter** (a given distance maps to fewer "meters", hence less falloff), **smaller = quieter and steeper**. Raise it if a distant ring fades out too aggressively; lower it if a far ring is distractingly loud. At 1000: 3000u → 3m → gain ~0.33; 5000u → 5m → 0.2.
- `kMinDistanceMeters` (default 1) — near plateau. A source closer than this gets no attenuation (gain capped at full), so the cue stops getting louder once you're basically on top of the ring. Larger = a wider full-volume bubble at close range.

`Cue3D_SetGain` is a separate, distance-*independent* multiplier applied on top of this falloff. It is now wired to the three-factor volume chain (see "Cue volume" below), not just reserved — it does *not* shape distance.

Elevation caveat: this is a *pure* inverse-distance model — it uses the full 3D distance including the vertical component, so a ring high above reads as genuinely farther (quieter). If a high ring sounds too faint, dividing the y component before the distance calc is the equivalent knob to add (the older SF64 path de-emphasised vertical separation at `y/2.5`).

### Y→pitch sensitivity

The `1000.0f` divisor in `AccessibilityCues_ComputeFreqModFromY` (shared by both cues). This is "how many world units of altitude difference equals one octave." Smaller = more aggressive pitch swing for small altitude changes.

The `±1.0f` clamp on the following lines bounds how extreme the pitch ever gets. Raising the bounds (e.g. ±2) gives a wider expressive range at the cost of stretching the sample badly at the extremes.

For the enemy cue, "altitude" is altitude relative to the Arwing's aim, not world-up — body-frame Y, computed in `AccessibilityCues_BuildWorldToBodyMatrix` + `Matrix_MultVec3fNoTranslate`. So the ring cue and enemy cue interpret "above" differently when the Arwing is pitched.

### Y→pitch direction

Also in `AccessibilityCues_ComputeFreqModFromY`: `octaves = y / 1000.0f`. Negate this to flip the mapping (lower pitch = higher altitude). Currently higher pitch = above. The change applies to both cues simultaneously.

### "Drop the cue when behind the player"

- Ring cue, `AccessibilityCues_FindNextTrainingRing`: `if (dz >= 0.0f) continue;` filters by world Z relative to `player->trueZpos`. As written, the moment a ring is at or behind the Arwing it stops contributing. Could be relaxed to `dz >= someThreshold` if you want a brief tail as you pass through — but in practice the distance falloff already fades the trailing ring, and the next ring becomes the target on the very next tick.
- Enemy cue, `AccessibilityCues_FindClosestEnemiesAhead`: `if (bodyDelta.z >= 0.0f) continue;` filters by body-frame Z (i.e., behind the *aim line*, not behind world position). The HRTF backend can render behind-you sources, so relaxing this to also cue enemies behind the aim is now technically possible — the drop is a deliberate design choice (keep the cue to threats you can turn toward) rather than a backend limitation as it was on the old SF64 path.

### Cue volume

Volume is a per-cue CVar, not a file static. The effective per-source gain is `gGameMasterVolume` × `gAccessibilityCueMasterVolume` × `gAccessibilityCueVolume.<Id>` (e.g. `.Ring` / `.Enemy`), computed by `Cue::PushGain` and pushed to `Cue3D_SetGain` every tick the cue is driven. All three are exposed as sliders under F1 → Blind Starship → Cue volumes ("All cues" master + one per cue), each with a Preview button. If a cue feels drowned out, raise its per-cue slider (or the WAV's own level); the per-cue CVar defaults to 1. Reverb is no longer a knob — the HRTF backend has no reverb stage, so if a cue sounds too dry, bake the "space" into the WAV.

### Which levels the cues fire in

- Ring cue, `AccessibilityCues_OnRingPostUpdate`: `if (... gCurrentLevel != LEVEL_TRAINING) ...`. Hard-scoped to Training because that's where rings live; broadening would also mean re-picking what to target outside Training.
- Enemy cue, `AccessibilityCues_OnEnemyPostUpdate`: `if (... gLevelMode != LEVELMODE_ON_RAILS) ...`. Fires on every on-rails level. All-range is deferred but the body-frame math already works there — the gate is the only blocker.

### What counts as a target

- Ring cue, `AccessibilityCues_FindNextTrainingRing`: status `OBJ_ACTIVE`, id `OBJ_ITEM_TRAINING_RING`, `state == 0`. The state filter is the subtle one — state 1 means the ring is in its fly-to-player animation after collection, and excluding it prevents the cue from chasing the collection animation. If you ever want a faint background cue for *all* visible rings plus a louder cue for the nearest, this function is where that splits.
- Enemy cue, `AccessibilityCues_IsCueableEnemy`: matches the engine's missile lock-on (`PlayerShot_FindLockTarget` in `fox_beam.c:1741`) — `status == OBJ_ACTIVE`, `info.targetOffset != 0.0f`. On-rails enemies all spawn as `OBJ_ACTOR_EVENT` and resolve into real targets only after `EVOP_INIT_ACTOR` rewrites `info.targetOffset` from the per-event table; filtering by id would reject them. To extend coverage (bosses, hazards, non-lockable damage-dealers like `OBJ_ACTOR_CO_RADAR`), add an explicit allow-list here. See `docs/accessibility-enemy-cue.md` § "Enemy detection criteria" for the catalogue of intentional misses.

### How many enemies sound at once

The enemy cue voices the N closest lockable enemies simultaneously, each on its own HRTF voice with sticky enemy→voice assignment (an enemy keeps its voice as long as it stays in the top N, so targets don't swap voices frame-to-frame). The knobs:

- **`gAccessibilityEnemyCueVoices`** (default 2, clamped 1–`kAccessibilityEnemyCueMaxVoices` = 4) — how many targets to voice. Runtime-tunable: F1 → Blind Starship → "Enemy locator voices". Set to 1 for the original single-target behavior.
- **Multi-voice headroom trim**, `Cue::PushGain` (`Cue.cpp`) — with N voices playing, each is trimmed by 1/√N so near-identical loops don't sum hot. If two voices feel too quiet next to one, soften or drop this.
- **Per-voice identity pitch**, `kVoiceIdentityPitch` (`Cue.cpp`) — a per-voice-slot pitch multiplier for telling simultaneous copies of the same loop apart. All 1.0 (off) today, deliberately: pitch already carries the elevation signal, so a detune would read as a false above/below. If spatial separation alone proves insufficient by ear, prefer per-voice timbre (WAV variants) before touching this.

### CVar default

`CVarRegisterInteger("gAccessibilityAudioCues", 1)` in `AccessibilityCues_Init`. Default-on for now; flip the second arg to `0` to make it opt-in. The CVar is toggleable at runtime from the console — the listener early-returns and kills any active cue when it's off, so no restart needed.

---

## What's not easily tunable from this file

A few limits live in the HRTF backend or its integration rather than in dial settings here:

- **Elevation from the generic HRTF is weak.** `IPL_HRTFTYPE_DEFAULT` only renders a strong vertical cue when the source is near the aim line, which is why the Y→pitch layer above exists at all. Sharpening the HRTF's *own* elevation means a personalized SOFA HRTF (`IPL_HRTFTYPE_SOFA` + a user-supplied file); see `docs/accessibility-hrtf-cues.md`.
- **No BGM ducking / cross-stream priority.** The cues play on a second OS audio device the game's audio path never reaches, so BGM ducking (`SFX_FLAG_19`) does not touch them, and there is no automatic ducking of the cues while TTS speaks (review CUE3D-16). The gain lever to build ducking on now exists (`Cue3D_SetGain` via `Cue::PushGain`) but nothing drives it for ducking yet.
- **No mixing/limiter beyond the backend's output trim.** Steam Audio spatializes but does not mix; the backend sums sources by hand with a `kOutputTrim` (0.6) plus a hard safety clip (`Cue3DSteamAudio.cpp`). The Cue layer's per-cue 1/√N multi-voice trim (above) keeps one cue's own voices in check, but *different* cues still sum untrimmed onto that clip — the practical ceiling on how many cues can overlap before the mix is compressed.
