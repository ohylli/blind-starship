# Tuning the accessibility audio cues

The cue mod lives at `src/port/mods/AccessibilityCues.{cpp,h}` and now houses two cues sharing the `gAccessibilityAudioCues` CVar:

- **Ring cue** — attaches a continuous SFX to the next Training ring ahead of the Arwing (scoped to `LEVEL_TRAINING`).
- **Enemy cue** — attaches a continuous SFX to the closest cueable enemy ahead of the Arwing's aim line, on any on-rails level. Coordinate frame is body-frame (rotated by the player's yaw + pitch) rather than world-frame; see `docs/accessibility-enemy-cue.md` for the derivation.

Both cues drive pan from X, volume from distance, and pitch from altitude via the same `AccessibilityCues_ComputeFreqModFromY` helper. Almost every knob below applies to either cue — function and variable names are prefixed `Ring` or `Enemy` to disambiguate.

For background on *why* the cues are shaped this way (player-relative coordinate frame, Y→pitch instead of Y→pan, bank/range constraints), see `docs/audio-system.md` and `docs/game-world.md`.

---

## The knobs, in order of "most likely to matter"

### The SFX itself

`RING_CUE_SFX` (Training-ring cue) and `ENEMY_CUE_SFX` (enemy cue) macros near the top of `AccessibilityCues.cpp`. Alternative candidates considered for the ring cue are listed in a comment above the macro definition. When picking the enemy cue's SFX, favor a sound with a clearly different timbre from the ring cue's, so both can fire simultaneously on Training without blending into one sound.

Constraints to keep in mind if reaching for another `NA_SE_*`:

- **Bank nibble** (top hex digit of the ID) must be 1, 2, or 3. Bank 0 (player) has a "force center pan when source z is in ±200" special case that fires at the worst possible moment for our cue; bank 4 (system) ignores position entirely.
- **Avoid `SFX_FLAG_22`** — would disable distance attenuation and so kill the volume-from-distance cue.
- **Avoid `SFX_FLAG_23`** — random per-frame pitch wobble; fights the Y→pitch mapping.
- **Prefer range 2 or 3** (the two bits at position 16/17 of the ID; see `docs/audio-system.md` §3 for the decoding). Range 0/1 dies at ~1650/2200 world units; rings spawn ~3000 units ahead, so a low-range SFX would be inaudible until the player is already close.

### Ring cue distance falloff (Cue3D / HRTF backend only)

Two knobs near the top of `src/port/accessibility/Cue3DSteamAudio.cpp`, applying **only** to the HRTF ring cue (`gAccessibilityCue3D` on). The SF64 SFX-engine path attenuates internally and ignores them.

Unlike the SFX engine — which folds distance, pan, and elevation into one opaque path — the HRTF backend splits the jobs: Steam Audio's binaural effect handles *direction*, and a separate inverse-distance model handles *loudness-vs-distance*. `ProduceBlock` calls `iplDistanceAttenuationCalculate` (type `INVERSEDISTANCE`, gain ≈ 1/d past the near plateau) once per audio block, using the magnitude of the player-relative position the cue pushes through `Cue3D_SetPosition`.

The wrinkle: Steam Audio measures distance in **meters**, but SF64 world units are huge (rings spawn ~3000 units ahead, clamp box ±5000). Feeding raw units to a 1/d model would be near-silent, so we scale units → meters first.

- `kWorldUnitsPerMeter` (default 1000) — overall steepness/loudness. It sits in the numerator of the gain (`1 / (units / thisKnob)`), so **larger = louder and flatter** (a given distance maps to fewer "meters", hence less falloff), **smaller = quieter and steeper**. Raise it if a distant ring fades out too aggressively; lower it if a far ring is distractingly loud. At 1000: 3000u → 3m → gain ~0.33; 5000u → 5m → 0.2.
- `kMinDistanceMeters` (default 1) — near plateau. A source closer than this gets no attenuation (gain capped at full), so the cue stops getting louder once you're basically on top of the ring. Larger = a wider full-volume bubble at close range.

`Cue3D_SetGain` is a separate, distance-*independent* multiplier applied on top of this falloff — reserved for a per-cue trim or a future master-volume / pause-silence lever, not for shaping distance.

Elevation caveat: this is a *pure* inverse-distance model — it uses the full 3D distance including the vertical component, so a ring high above reads as genuinely farther (quieter). The SF64 path de-emphasised vertical separation (`y/2.5`); if a high ring sounds too faint here, dividing the y component before the distance calc is the equivalent knob to add.

### Y→pitch sensitivity

The `1000.0f` divisor in `AccessibilityCues_ComputeFreqModFromY` (shared by both cues). This is "how many world units of altitude difference equals one octave." Smaller = more aggressive pitch swing for small altitude changes.

The `±1.0f` clamp on the following lines bounds how extreme the pitch ever gets. Raising the bounds (e.g. ±2) gives a wider expressive range at the cost of stretching the sample badly at the extremes.

For the enemy cue, "altitude" is altitude relative to the Arwing's aim, not world-up — body-frame Y, computed in `AccessibilityCues_BuildWorldToBodyMatrix` + `Matrix_MultVec3fNoTranslate`. So the ring cue and enemy cue interpret "above" differently when the Arwing is pitched.

### Y→pitch direction

Also in `AccessibilityCues_ComputeFreqModFromY`: `octaves = y / 1000.0f`. Negate this to flip the mapping (lower pitch = higher altitude). Currently higher pitch = above. The change applies to both cues simultaneously.

### "Drop the cue when behind the player"

- Ring cue, `AccessibilityCues_FindNextTrainingRing`: `if (dz >= 0.0f) continue;` filters by world Z relative to `player->trueZpos`. As written, the moment a ring is at or behind the Arwing it stops contributing. Could be relaxed to `dz >= someThreshold` if you want a brief tail as you pass through — but in practice the engine's distance falloff already fades the trailing ring, and the next ring becomes the target on the very next tick.
- Enemy cue, `AccessibilityCues_FindClosestEnemyAhead`: `if (bodyDelta.z >= 0.0f) continue;` filters by body-frame Z (i.e., behind the *aim line*, not behind world position). Relaxing this would mean cueing enemies behind the Arwing too; expect ambiguous-direction center-pan because the engine's stereo path uses `|z|` — pitch can still convey altitude.

### Volume and reverb the cue gets

Per-cue: `sRingCueVolMod` / `sRingCueReverb` and `sEnemyCueVolMod` / `sEnemyCueReverb` (file statics, defaults `1.0f` / `0`). The engine reads these every audio frame.

- A cue feels drowned out by gameplay SFX → bump its `*VolMod` to ~1.5–2.0.
- A cue feels too dry / disembodied → add small `*Reverb` (e.g. 20–40 out of 127); gives it more "space."

### Pan saturation at distance

Not a variable, but worth knowing: the engine internally clamps `|x|` to 1200 before computing pan, so anything past 1200 world units of lateral offset reads as "fully left/right." If a ring appears far to one side, pan is already pinned.

To get finer directional discrimination at long distances, pre-clamp `sRingCueSrc[0]` / `sEnemyCueSrc[0]` to a smaller window (e.g. ±800) in the corresponding `RefreshRingSource` / `RefreshEnemySource` before calling `Object_ClampSfxSource`, so the engine's pan ramp stays in the useful compressing range rather than instantly saturating.

### Which levels the cues fire in

- Ring cue, `AccessibilityCues_OnRingPostUpdate`: `if (... gCurrentLevel != LEVEL_TRAINING) ...`. Hard-scoped to Training because that's where rings live; broadening would also mean re-picking what to target outside Training.
- Enemy cue, `AccessibilityCues_OnEnemyPostUpdate`: `if (... gLevelMode != LEVELMODE_ON_RAILS) ...`. Fires on every on-rails level. All-range is deferred but the body-frame math already works there — the gate is the only blocker.

### What counts as a target

- Ring cue, `AccessibilityCues_FindNextTrainingRing`: status `OBJ_ACTIVE`, id `OBJ_ITEM_TRAINING_RING`, `state == 0`. The state filter is the subtle one — state 1 means the ring is in its fly-to-player animation after collection, and excluding it prevents the cue from chasing the collection animation. If you ever want a faint background cue for *all* visible rings plus a louder cue for the nearest, this function is where that splits.
- Enemy cue, `AccessibilityCues_IsCueableEnemy`: matches the engine's missile lock-on (`PlayerShot_FindLockTarget` in `fox_beam.c:1741`) — `status == OBJ_ACTIVE`, `info.targetOffset != 0.0f`. On-rails enemies all spawn as `OBJ_ACTOR_EVENT` and resolve into real targets only after `EVOP_INIT_ACTOR` rewrites `info.targetOffset` from the per-event table; filtering by id would reject them. To extend coverage (bosses, hazards, non-lockable damage-dealers like `OBJ_ACTOR_CO_RADAR`), add an explicit allow-list here. See `docs/accessibility-enemy-cue.md` § "Enemy detection criteria" for the catalogue of intentional misses.

### CVar default

`CVarRegisterInteger("gAccessibilityAudioCues", 1)` in `AccessibilityCues_Init`. Default-on for now; flip the second arg to `0` to make it opt-in. The CVar is toggleable at runtime from the console — the listener early-returns and kills any active cue when it's off, so no restart needed.

---

## What's not easily tunable from this file

A few limits are real constraints of the SF64 audio path rather than dial settings:

- **No "directly behind" pan in stereo.** The engine's stereo pan uses `|z|`, so a source straight in front and a source straight behind both pan center. We sidestep this by dropping the cue when the ring is no longer ahead — but it's the reason "things behind me" can't be conveyed with the engine alone in stereo.
- **Y → distance only, not pan.** Altitude contributes only to distance falloff (at ÷2.5 weight) and never to L/R pan. That's why we drive pitch from Y ourselves; there is no panning lever for elevation.
- **Polyphony eviction.** If a scene fills the bank's slots, the engine evicts by importance. The current pick has importance 0x60 so it should win most evictions, but an unusually busy moment could still drop it. Authoring a custom sample with a higher importance byte would be the fix.
