# Tuning the accessibility audio cue

The cue mod lives at `src/port/mods/AccessibilityCues.{c,h}`. It currently fires only in Training mode, attaching a continuous positional SFX to the next ring ahead of the Arwing and driving its pan from X, volume from distance, and pitch from altitude.

This document lists the parts of the code that are intended as knobs — what each one does, what changing it costs, and where to find it.

For background on *why* the cue is shaped this way (player-relative coordinate frame, Y→pitch instead of Y→pan, bank/range constraints), see `docs/audio-system.md` and `docs/game-world.md`.

---

## The knobs, in order of "most likely to matter"

### The SFX itself

`ACCESSIBILITY_CUE_SFX` macro near the top of `AccessibilityCues.c`. Alternative candidates considered are listed in a comment above the macro definition.

Constraints to keep in mind if reaching for another `NA_SE_*`:

- **Bank nibble** (top hex digit of the ID) must be 1, 2, or 3. Bank 0 (player) has a "force center pan when source z is in ±200" special case that fires at the worst possible moment for our cue; bank 4 (system) ignores position entirely.
- **Avoid `SFX_FLAG_22`** — would disable distance attenuation and so kill the volume-from-distance cue.
- **Avoid `SFX_FLAG_23`** — random per-frame pitch wobble; fights the Y→pitch mapping.
- **Prefer range 2 or 3** (the two bits at position 16/17 of the ID; see `docs/audio-system.md` §3 for the decoding). Range 0/1 dies at ~1650/2200 world units; rings spawn ~3000 units ahead, so a low-range SFX would be inaudible until the player is already close.

### Y→pitch sensitivity

The `1000.0f` divisor in `AccessibilityCues_RefreshSource`. This is "how many world units of altitude difference equals one octave." Smaller = more aggressive pitch swing for small altitude changes.

The `±1.0f` clamp on the following lines bounds how extreme the pitch ever gets. Raising the bounds (e.g. ±2) gives a wider expressive range at the cost of stretching the sample badly at the extremes.

### Y→pitch direction

Also in `AccessibilityCues_RefreshSource`: `octaves = sCueSrc[1] / 1000.0f`. Negate this to flip the mapping (lower pitch = higher altitude). Currently higher pitch = above.

### "Drop the cue when behind the player"

`AccessibilityCues_FindNextTrainingRing`: `if (dz >= 0.0f) continue;`. As written, the moment a ring is at or behind the Arwing it stops contributing. Could be relaxed to `dz >= someThreshold` if you want a brief tail as you pass through — but in practice the engine's distance falloff already fades the trailing ring, and the next ring becomes the target on the very next tick.

### Volume and reverb the cue gets

`sCueVolMod` (file static, default `1.0f`) and `sCueReverb` (default `0`). The engine reads these every audio frame.

- Cue feels drowned out by gameplay SFX → bump `sCueVolMod` to ~1.5–2.0.
- Cue feels too dry / disembodied → add small `sCueReverb` (e.g. 20–40 out of 127); gives it more "space."

### Pan saturation at distance

Not a variable, but worth knowing: the engine internally clamps `|x|` to 1200 before computing pan, so anything past 1200 world units of lateral offset reads as "fully left/right." If a ring appears far to one side, pan is already pinned.

To get finer directional discrimination at long distances, pre-clamp `sCueSrc[0]` to a smaller window (e.g. ±800) in `RefreshSource` so the engine's pan ramp stays in the useful compressing range rather than instantly saturating.

### Which levels the cue fires in

`AccessibilityCues_OnGamePostUpdate`: `if (... gCurrentLevel != LEVEL_TRAINING) ...`. Currently hard-scoped to training. Broadening to other on-rails levels is just adding more allowed levels — though non-training levels have no training rings, so we'd also need to pick what to target (enemies, hazards, gold rings).

### What counts as a target

`AccessibilityCues_FindNextTrainingRing`. Currently: status `OBJ_ACTIVE`, id `OBJ_ITEM_TRAINING_RING`, `state == 0`.

The state filter is the subtle one: state 1 means the ring is in its fly-to-player animation after collection. Excluding it prevents the cue from chasing the collection animation. If you ever want a faint background cue for *all* visible rings plus a louder cue for the nearest, this function is where that splits.

### CVar default

`CVarRegisterInteger("gAccessibilityAudioCues", 1)` in `AccessibilityCues_Init`. Default-on for now; flip the second arg to `0` to make it opt-in. The CVar is toggleable at runtime from the console — the listener early-returns and kills any active cue when it's off, so no restart needed.

---

## What's not easily tunable from this file

A few limits are real constraints of the SF64 audio path rather than dial settings:

- **No "directly behind" pan in stereo.** The engine's stereo pan uses `|z|`, so a source straight in front and a source straight behind both pan center. We sidestep this by dropping the cue when the ring is no longer ahead — but it's the reason "things behind me" can't be conveyed with the engine alone in stereo.
- **Y → distance only, not pan.** Altitude contributes only to distance falloff (at ÷2.5 weight) and never to L/R pan. That's why we drive pitch from Y ourselves; there is no panning lever for elevation.
- **Polyphony eviction.** If a scene fills the bank's slots, the engine evicts by importance. The current pick has importance 0x60 so it should win most evictions, but an unusually busy moment could still drop it. Authoring a custom sample with a higher importance byte would be the fix.
