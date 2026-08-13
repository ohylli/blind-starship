# Tuning the accessibility audio cues

The cues render through the Steam Audio HRTF backend (see
`docs/accessibility-hrtf-cues.md`). There is no longer a second SF64-audio-engine
path — it was removed, along with the `gAccessibilityCue3D` toggle. The pieces are:

- **The `Cue` layer** (`src/port/accessibility/Cue.{h,cpp}`) — game-agnostic: each cue owns its sound file, its per-cue volume CVar, the three-factor gain, and a preview mode. A registry lets the settings UI enumerate cues.
- **The consumer mod** (`src/port/mods/accessibility_cues/`, one file pair per cue plus the shared `CueCommon`/`CueScan`/`ObstacleScan` helpers, coordinated by `src/port/mods/AccessibilityCues.{cpp,h}`) — the Star Fox side, housing four cues sharing the `gAccessibilityAudioCues` CVar:
  - **Ring cue** — tracks the next Training ring ahead of the Arwing (scoped to `LEVEL_TRAINING`).
  - **Enemy cue** — tracks the closest cueable enemies relative to the Arwing's aim: ahead of the aim line on on-rails levels, the full sphere (including behind you) in solo all-range mode. Coordinate frame is body-frame (rotated by the player's yaw + pitch) rather than world-frame; see `docs/accessibility-enemy-cue.md` for the derivation.
  - **Aim cue ("Aim guide")** — the inverse concern of the ring/enemy cues: a repeated synthesized click encoding the *aim itself* (stereo pan = left/right, pitch = up/down), sped up geiger-counter-style as the aim line nears a lockable enemy. Has its own additional toggle (`gAccessibilityAimCue`) because a continuous click is the most fatiguing cue to leave on. See "Aim cue" below for all its knobs.
  - **Obstacle cue ("Obstacle warning")** — a collision warning, not a locator: a centered low buzz that pulses faster as the player closes on a collidable, non-lockable obstacle they are on course to hit. On-rails only; own toggle `gAccessibilityObstacleCue`. See "Obstacle cue" below and `docs/accessibility-obstacle-cue.md`.

Direction is now the HRTF backend's job (true left/right and front/back). The mod still drives pitch from altitude via `CueCommon_ComputeFreqModFromY`, layered on top of the HRTF (the generic HRTF's own elevation cue is weak — see `docs/accessibility-hrtf-cues.md`). Distance drives volume through the backend's inverse-distance model. Most knobs below apply to either cue; function names are prefixed `Ring` or `Enemy` to disambiguate.

For background on *why* the cues are shaped this way (player-relative coordinate frame, Y→pitch instead of Y→pan), see `docs/audio-system.md` and `docs/game-world.md`.

---

## The knobs, in order of "most likely to matter"

### The sound file

The ring and enemy cues load a WAV from `assets/accessibility/` (`ring.wav`, `enemy.wav`), named in `RingCue_Register` / `EnemyCue_Register` where the cues are registered; the aim cue synthesizes its click instead (see "Aim cue" below). Swap the file to change a WAV cue's sound. Favor sounds with clearly different timbres between the two cues, so both can fire simultaneously on Training without blending into one sound. Unlike the old SF64-SFX path, there are no bank/flag/range constraints — the HRTF backend plays an arbitrary mono WAV; distance, direction, and pitch are all applied by the backend or the mod, not baked into the sound.

### Ring cue distance falloff

Two knobs near the top of `src/port/accessibility/Cue3DSteamAudio.cpp`, controlling the HRTF distance model for every cue.

Unlike the SFX engine — which folds distance, pan, and elevation into one opaque path — the HRTF backend splits the jobs: Steam Audio's binaural effect handles *direction*, and a separate inverse-distance model handles *loudness-vs-distance*. `ProduceBlock` calls `iplDistanceAttenuationCalculate` (type `INVERSEDISTANCE`, gain ≈ 1/d past the near plateau) once per audio block, using the magnitude of the player-relative position the cue pushes through `Cue3D_SetPosition`.

The wrinkle: Steam Audio measures distance in **meters**, but SF64 world units are huge (rings spawn ~3000 units ahead, clamp box ±5000). Feeding raw units to a 1/d model would be near-silent, so we scale units → meters first.

- `kWorldUnitsPerMeter` (default 1000) — overall steepness/loudness. It sits in the numerator of the gain (`1 / (units / thisKnob)`), so **larger = louder and flatter** (a given distance maps to fewer "meters", hence less falloff), **smaller = quieter and steeper**. Raise it if a distant ring fades out too aggressively; lower it if a far ring is distractingly loud. At 1000: 3000u → 3m → gain ~0.33; 5000u → 5m → 0.2.
- `kMinDistanceMeters` (default 1) — near plateau. A source closer than this gets no attenuation (gain capped at full), so the cue stops getting louder once you're basically on top of the ring. Larger = a wider full-volume bubble at close range.

`Cue3D_SetGain` is a separate, distance-*independent* multiplier applied on top of this falloff. It is now wired to the three-factor volume chain (see "Cue volume" below), not just reserved — it does *not* shape distance.

Elevation caveat: this is a *pure* inverse-distance model — it uses the full 3D distance including the vertical component, so a ring high above reads as genuinely farther (quieter). If a high ring sounds too faint, dividing the y component before the distance calc is the equivalent knob to add (the older SF64 path de-emphasised vertical separation at `y/2.5`).

### Y→pitch on/off, range, and sensitivity

The whole altitude→pitch layer is now a runtime toggle plus two sliders under **F1 → Developer → Blind Starship** (`CueCommon_ComputeFreqModFromY` reads them live each tick, so no rebuild):

- **Height-to-pitch cue** — `gAccessibilityCuePitchForHeight` (default on). Off = the cue keeps its native pitch and altitude is conveyed only by the (weak) HRTF elevation.
- **Pitch height sensitivity** — `gAccessibilityCuePitchScale` (default 1000, the old divisor). "How many world units of altitude difference equals one octave." Smaller = more aggressive pitch swing for small altitude changes.
- **Pitch range** — `gAccessibilityCuePitchRangeOctaves` (default 1.0, the old ±1 octave clamp). Bounds how extreme the pitch ever gets. Larger (e.g. 2) gives a wider expressive range at the cost of stretching the sample badly at the extremes.
- **Spectral pitch shifter** — `gAccessibilityCuePitchShift` (default on). Selects *how* the backend realizes the pitch multiplier (`Cue3D_SetPitchStyle`). On = spectral shifter (Signalsmith Stretch): each sound keeps its length and character and only the pitch moves, at the cost of ~0.1 s latency on the cue *content* (onsets, pitch changes — pulse cadence and spatial position are unaffected) and more CPU per source. Off = classic playback-rate change: artifact- and latency-free, but higher also means faster and thinner (the "chipmunk" effect). Applies to every cue and to the bench's pitch slider, independent of the height-to-pitch toggle above. Both styles are deliberately kept while the choice settles by ear; expect a collapse to one style eventually.

Both defaults reproduce the original hard-coded mapping exactly. The CVar names and defaults live in `accessibility_cues/CueCommon.h`.

For the enemy cue, "altitude" is altitude relative to the Arwing's aim, not world-up — body-frame Y, computed in `CueScan_BuildWorldToBodyMatrix` + `Matrix_MultVec3fNoTranslate`. So the ring cue and enemy cue interpret "above" differently when the Arwing is pitched.

### Y→pitch direction

Also in `CueCommon_ComputeFreqModFromY`: `octaves = y / scale`. Negate this to flip the mapping (lower pitch = higher altitude). Currently higher pitch = above. The change applies to both cues simultaneously. (This one is still a code change, not a slider.)

### "Drop the cue when behind the player"

- Ring cue, `RingCue_FindNextTrainingRing`: `if (dz >= 0.0f) continue;` filters by world Z relative to `player->trueZpos`. As written, the moment a ring is at or behind the Arwing it stops contributing. Could be relaxed to `dz >= someThreshold` if you want a brief tail as you pass through — but in practice the distance falloff already fades the trailing ring, and the next ring becomes the target on the very next tick.
- Enemy cue, `CueScan_ForEachCueableEnemy` (the scan shared with the aim cue's geiger angle): the behind-the-aim drop (`bodyDelta.z >= 0`, body-frame Z — behind the *aim line*, not behind world position) is **on-rails only**. In all-range the full sphere cues, since threats come from behind there and the HRTF renders the rear hemisphere. The HRTF alone proved too weak for front/back by ear, so the backend now exaggerates it — see "Rear effects" below.

### Rear effects (front/back exaggeration)

The backend exaggerates "behind you" on **every** source, keyed off the normalized forward component of the position: zero effect across the entire front hemisphere, blending in smoothly across the rear hemisphere (no audible seam as a target crosses your shoulder). Exists because the generic HRTF's own front/back rendering was too weak to read in all-range play. Three stacked effects, four knobs — all **runtime CVars with sliders under F1 → Developer → Blind Starship**, so they can be compared and re-balanced by ear without a rebuild (each effect zeroes out via its own slider). Defaults are the `CUE3D_REAR_*_DEFAULT` values in `Cue3D.h`; the plumbing is `CueRegistry_Tick` (`Cue.cpp`) re-reading the four CVars every game tick → `Cue3D_SetRearEffect` → atomics the audio callback reads once per block. A pull-per-tick rather than a push-on-slider-change, so a value set from the **console** goes live too, not just one dragged on a slider.

`Cue3D_SetRearEffect` clamps every knob to a sane range and rejects non-finite values (falling back to the default). That is not belt-and-braces: a cutoff ≤ 0 puts the one-pole low-pass outside the unit circle, and it diverges to NaN within a single audio block — which then sticks to that source's filter state for the rest of the process. Keep the validation if you add a knob.

- **Muffle** — `gAccessibilityCueRearCutoffHz` (default 1000): one-pole low-pass cutoff when the source is dead behind; the source keeps its character but loses its highs — an exaggerated head shadow, the classic audio-game "behind you" convention. Lower = duller behind. Only as strong as the high-frequency content in the cue's WAV: if a cue sounds too similar front vs. back even at a low cutoff, brighten the WAV.
- **Volume dip** — `gAccessibilityCueRearGainDip` (default 0.0, i.e. disabled): fraction of gain removed when dead behind. Deliberately mild when used: volume already encodes distance, so a deep dip would read as "far away", not "behind". 0 disables.
- **Tremolo** — `gAccessibilityCueRearTremoloDepth` (default 0.5) and `gAccessibilityCueRearTremoloHz` (default 10): amplitude pulsing for rear sources. Not naturalistic, but maximally salient and unambiguous — nothing else in the cue system pulses, so it cannot be confused with distance, elevation, or a quiet sound. Depth 0 disables; with several enemy voices behind you at once, high depths may get noisy.

Ring cue and the F1 previews are unaffected in practice — both only ever render sources ahead. The best A/B environment is the **Cue3D test bench** in the same Developer menu (no restart needed — it replaced the old restart-required Spatial audio test): enable its Orbit control and the sound circles the head once every 4 s, sweeping front → side → rear, while the sliders retune it live mid-orbit.

### All-range range limit

`kEnemyCueAllRangeMaxDist` (default 10000, in `accessibility_cues/CueScan.h`) — enemy-cue-only, all-range-only. On-rails needs no cutoff because the engine only keeps nearby objects loaded, but all-range loads the whole arena (radius 8000–20000+ units depending on level), and past the ±5000 clamp box every target sounds the same faint volume — so without a limit, far dogfighters drone constantly. Smaller = quieter arenas where silence means "nothing in range"; larger = hear (the direction of) distant fights sooner. Rebuild-to-tune; promote to a CVar + F1 slider if it needs live adjustment.

### Cue volume

Volume is a per-cue CVar, not a file static. The effective per-source gain is `gGameMasterVolume` × `gAccessibilityCueMasterVolume` × `gAccessibilityCueVolume.<Id>` (e.g. `.Ring` / `.Enemy`), computed by `Cue::PushGain` and pushed to `Cue3D_SetGain` every tick the cue is driven. All three are exposed as sliders under F1 → Blind Starship → Cue volumes ("All cues" master + one slider per registered cue, generated from the registry so future cues appear automatically), each with a Preview button that plays the cue for ~3 s straight ahead at the distance the backend renders at unity gain — so the preview's loudness *is* the volume setting. Previews suspend gameplay control of that cue and auto-expire. If a cue feels drowned out, raise its per-cue slider (or the WAV's own level); the per-cue CVar defaults to 1. Reverb is no longer a knob — the HRTF backend has no reverb stage, so if a cue sounds too dry, bake the "space" into the WAV.

### Which levels the cues fire in

- Ring cue, `RingCue_OnPostUpdate`: `if (... gCurrentLevel != LEVEL_TRAINING) ...`. Hard-scoped to Training because that's where rings live; broadening would also mean re-picking what to target outside Training.
- Enemy cue, `EnemyCue_OnPostUpdate`: fires on every on-rails level and in solo all-range (`gLevelMode == LEVELMODE_ALL_RANGE && !gVersusMode`) — including mid-level transitions (Corneria/Sector Y bosses, Andross, Training's battle phase), which just flip `gLevelMode`. Multiplayer Versus shares the all-range mode flag but is gated out as untested.

### What counts as a target

- Ring cue, `RingCue_FindNextTrainingRing`: status `OBJ_ACTIVE`, id `OBJ_ITEM_TRAINING_RING`, `state == 0`. The state filter is the subtle one — state 1 means the ring is in its fly-to-player animation after collection, and excluding it prevents the cue from chasing the collection animation. If you ever want a faint background cue for *all* visible rings plus a louder cue for the nearest, this function is where that splits.
- Enemy cue, `CueScan_IsCueableEnemy`: matches the engine's missile lock-on (`PlayerShot_FindLockTarget` in `fox_beam.c:1741`) — `status == OBJ_ACTIVE`, `info.targetOffset != 0.0f`. On-rails enemies all spawn as `OBJ_ACTOR_EVENT` and resolve into real targets only after `EVOP_INIT_ACTOR` rewrites `info.targetOffset` from the per-event table; filtering by id would reject them. To extend coverage (bosses, hazards, non-lockable damage-dealers like `OBJ_ACTOR_CO_RADAR`), add an explicit allow-list here. See `docs/accessibility-enemy-cue.md` § "Enemy detection criteria" for the catalogue of intentional misses.

### How many enemies sound at once

The enemy cue voices the N closest lockable enemies simultaneously, each on its own HRTF voice with sticky enemy→voice assignment (an enemy keeps its voice as long as it stays in the top N, so targets don't swap voices frame-to-frame). The knobs:

- **`gAccessibilityEnemyCueVoices`** — how many targets to voice. Clamped to 1–`kAccessibilityEnemyCueMaxVoices`, and starts at `kAccessibilityEnemyCueDefaultVoices`; both live in `accessibility_cues/EnemyCue.h`. Runtime-tunable: F1 → Blind Starship → "Enemy locator voices". Set to 1 for the original single-target behavior.
- **Multi-voice headroom trim**, `Cue::PushGain` (`Cue.cpp`) — with N voices playing, each is trimmed by 1/√N so near-identical loops don't sum hot. If two voices feel too quiet next to one, soften or drop this.
- **Per-voice identity pitch**, `kVoiceIdentityPitch` (`Cue.cpp`) — a per-voice-slot pitch multiplier for telling simultaneous copies of the same loop apart. All 1.0 (off) today, deliberately: pitch already carries the elevation signal, so a detune would read as a false above/below. If spatial separation alone proves insufficient by ear, prefer per-voice timbre (WAV variants) before touching this.

### Aim cue

The aim cue is deliberately *not* an HRTF cue: it renders in `CUE3D_MODE_PAN` (plain constant-power stereo) at the backend's unity-gain distance, so pan, pitch, and repeat rate are pure functions of the aim with no distance falloff and no rear effect. It also pins the playback-rate pitch style per-source (`CueSpec::pitchStyle = CUE3D_SOURCE_PITCH_RESAMPLE`) regardless of the global spectral-shifter A/B — the shifter's ~0.1 s latency and transient softening would smear the click attack that *is* the signal. Its sound is synthesized (`AimCue_GenerateClick`), not a WAV: ~12 ms of silence then a ~20 ms damped-sine tick. The lead-in silence is load-bearing — the backend fades every pulse restart in over ~5 ms, and without the lead-in that ramp eats the click's attack (~6 dB of loudness); the tick must also finish inside the fastest repeat interval. Retune the timbre by editing the constants in the generator (rebuild-to-tune on purpose — the *mapping* knobs below are the ones that need by-ear iteration).

What pan and pitch encode differs by mode:

- **On rails** — the projected aim point relative to the corridor center: lateral drift off the path centerline plus the stick deflection projected `gAccessibilityAimCueProjDist` world units ahead (default 1200, the near reticle's distance), normalized by the corridor half-extents the engine clamps flight to (`pathWidth`/`pathHeight`). With a neutral stick this degrades to "where am I on screen". A larger projection distance weights the stick deflection more against the drift; smaller reads more like a pure position indicator.
- **All-range** — no corridor exists, so pan encodes the stick's yaw deflection (full pan at `gAccessibilityAimCueYawRangeDeg`, default 55°) and pitch the aim's world elevation angle (full bend at `gAccessibilityAimCuePitchRangeDeg`, default 90°).

Shared knobs, all live CVars with sliders under **F1 → Developer → Blind Starship → Aim guide**:

- **Pitch bend** — `gAccessibilityAimCueOctaves` (default 2.0): octaves the click bends at the vertical extremes, same shape as the Y→pitch range knob but independent of it (and independent of the height-to-pitch toggle — in PAN mode pitch is the only vertical channel there is).
- **Geiger mapping** — `gAccessibilityAimCueGeigerAngleDeg` (default 30°), `gAccessibilityAimCueGeigerSlowSec` (default 0.8), `gAccessibilityAimCueGeigerFastSec` (default 0.06): the smallest angle between the aim line and any cueable enemy (same predicate and scoping as the enemy cue, including the all-range range limit) maps linearly from the slow interval at/beyond the max angle — and when no enemy is in scope — down to the fast interval dead on target.
- **Click loudness** — `gAccessibilityAimCueBoost` (default 2.0): a gain boost applied *under* the volume sliders via `Cue::SetGainBoost`, because a short click reads perceptually quieter than the sustained ring/enemy loops at the same sample level and the per-cue volume slider (0–100%) has no headroom above its default. Applies to the settings-menu preview too, so the preview stays honest.

Scope: every on-rails level and solo all-range, **Arwing only** (`player->form == FORM_ARWING`) — the mappings read Arwing aim fields; Landmaster/Blue-Marine need their own mappings (future work). Sign conventions are derived, not guessed: the stick is negated into `rot` (`fox_play.c`), so `rot.y < 0` means "aiming right" and a positive total pitch angle means "aiming up" — if a direction ever reads flipped by ear, the sign notes in `AimCue_OnPostUpdate` say which term to flip.

### Obstacle cue ("Obstacle warning")

A collision *warning*, not a locator: a low synthesized buzz (~300 Hz damped tone, `ObstacleAheadCue_GenerateBuzz` — an octave-and-more below the aim click so the two never blend) that fires only when the player is on a collision course with a collidable, non-lockable obstacle, and pulses faster the closer the impact. Renders `CUE3D_MODE_DIRECT` (dead center, no HRTF, no distance attenuation, no rear effect) at constant loudness and constant pitch — **the pulse interval is deliberately the only signal**, so "faster" can never be confused with "louder because near" or "higher because above". The planned directional obstacle cues (left/right pan, above/below) will carry the "which way to dodge" half; this cue stays the "how soon" half. Design record, the collision-course derivation, and the catalogue of intentional misses (poly-mesh scenery, rotated boxes, ground/water, curved paths): `docs/accessibility-obstacle-cue.md`.

The course test (`ObstacleAheadCue_OnPostUpdate` over `ObstacleScan_ForEachBox`): an obstacle's hitbox record is "on course" when its near face is ahead within the warning distance and the player's current X/Y sits inside the record's footprint expanded by the safety margin. The nearest on-course record drives the interval, linearly from the slow interval at the warning distance down to the fast interval at contact. Per-record, not per-object, so a gate with a pillar–lintel–pillar hitbox stays correctly silent when you are lined up with the gap.

Knobs, all live CVars with sliders under **F1 → Developer → Blind Starship → Obstacle warning**:

- **Warning distance** — `gAccessibilityObstacleCueWarnDist` (default 2000): near-face gap where the buzz starts. ~1.7 s of warning at cruise speed. Values near 3000 are dishonest — on-rails streaming spawns objects roughly 3000 units ahead, so a farther threshold just means the buzz starts mid-ramp when the object streams in.
- **Slow / fast interval** — `gAccessibilityObstacleCueSlowSec` (default 0.6) / `gAccessibilityObstacleCueFastSec` (default 0.07): the pulse cadence endpoints. The fast slider floors at `kObstacleCueMinIntervalSec`, the buzz buffer's length budget — a shorter interval would truncate every pulse at the restart. A `static_assert` in the buzz generator enforces the budget, so retuning the timbre longer without moving that constant (in `ObstacleAheadCue.h`) fails the build.
- **Safety margin** — `gAccessibilityObstacleCueMarginXY` (default 150): world units of slack around the hitbox footprint. Exists because the test uses the ship's center point while the engine collides four body/wing points (wings at roughly ±40 units), and because a warning should precede a graze, not coincide with it. The knob most worth sweeping first by ear: higher warns about near misses, lower only about direct hits.
- **Buzz loudness** — `gAccessibilityObstacleCueBoost` (default 1.5): gain boost under the volume sliders (`Cue::SetGainBoost`), like the aim click's — but lower, since the longer, lower buzz reads louder than the click at equal sample level.

Scope and toggles: on-rails levels only (all-range is a planned follow-up needing a heading-projected course test), any vehicle, gated by the master `gAccessibilityAudioCues` plus its own `gAccessibilityObstacleCue` (default on, F1 → Blind Starship → "Obstacle warning"). That toggle is documented as the **obstacle-cue family master** — the future directional cues will read the same CVar rather than adding one each. Note for Training: Minimal training (default on) strips exactly the obstacle set this cue warns about — same shared predicate `Object_IsObstacle` — so in Training the cue is intentionally silent; tune it on Corneria instead (the `obstacle-scout` debug checkpoint sits just before a rock-wall pair and a building). The preview button plays the buzz as a seamless drone (previews force interval 0, a pre-existing quirk shared with the aim cue) — that is the timbre check, not the cadence.

### CVar default

`CVarRegisterInteger("gAccessibilityAudioCues", 1)` in `CueCommon_RegisterCVars` (called from `AccessibilityCues_Init`). Default-on for now; flip the second arg to `0` to make it opt-in. The CVar is toggleable at runtime from the console — the listener early-returns and kills any active cue when it's off, so no restart needed.

---

## What's not easily tunable from this file

A few limits live in the HRTF backend or its integration rather than in dial settings here:

- **Elevation from the generic HRTF is weak.** `IPL_HRTFTYPE_DEFAULT` only renders a strong vertical cue when the source is near the aim line, which is why the Y→pitch layer above exists at all. Sharpening the HRTF's *own* elevation means a personalized SOFA HRTF (`IPL_HRTFTYPE_SOFA` + a user-supplied file); see `docs/accessibility-hrtf-cues.md`.
- **No BGM ducking / cross-stream priority.** The cues play on a second OS audio device the game's audio path never reaches, so BGM ducking (`SFX_FLAG_19`) does not touch them, and there is no automatic ducking of the cues while TTS speaks (review CUE3D-16). The gain lever to build ducking on now exists (`Cue3D_SetGain` via `Cue::PushGain`) but nothing drives it for ducking yet.
- **No mixing/limiter beyond the backend's output trim.** Steam Audio spatializes but does not mix; the backend sums sources by hand with a `kOutputTrim` (0.6) plus a hard safety clip (`Cue3DSteamAudio.cpp`). The Cue layer's per-cue 1/√N multi-voice trim (above) keeps one cue's own voices in check, but *different* cues still sum untrimmed onto that clip — the practical ceiling on how many cues can overlap before the mix is compressed.
