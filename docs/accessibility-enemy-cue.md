# Accessibility enemy audio cue — design notes

This document is the working brief for adding a positional audio cue that helps a blind player locate enemies, sibling to the existing training-ring cue at `src/port/mods/AccessibilityCues.{cpp,h}`. It captures decisions as they are made so that future sessions can pick up the context without re-deriving it. It is expected to grow.

First scope is on-rails levels. All-range is a follow-up, but the design below was chosen so the same code works in both modes.

**Status: implemented.** The first pass lives alongside the ring cue in `src/port/mods/AccessibilityCues.cpp`. SFX is `NA_SE_KA_UFO_ENGINE`, single closest enemy ahead of the aim line, fired on every `LEVELMODE_ON_RAILS` level. The sections below are the design rationale and remaining open questions; code is the source of truth for current behavior.

For background on how the world is laid out (where enemies live, how to iterate them, hitbox/targetability fields), see `docs/game-world.md`. For how a positional SFX is emitted and which knobs the audio engine exposes, see `docs/audio-system.md` and `docs/accessibility-cues-tuning.md`.

---

## What the cue is

A continuous positional SFX attached to an enemy, conceptually the same shape as the ring cue: distance drives volume, lateral offset drives stereo pan, vertical offset drives a pitch shift. The difference from the ring cue is the coordinate frame the lateral/vertical offset is measured in (see next section).

Which SFX represents an enemy is deferred — see "Open questions" below.

---

## The key decision: aim-relative panning

The ring cue feeds the engine a world-space delta `(enemy.pos - player.pos)` and lets the engine pan from its X component. That works in on-rails only because the Arwing is always pointed roughly along the level's forward axis, so "world X" and "the player's right" happen to coincide. In all-range the Arwing can face any direction, and world X stops corresponding to "my right" the moment the player turns.

For the enemy cue we instead rotate that world delta into the Arwing's body frame before feeding it to the engine. After the rotation, the X component means "how far to the right of where I'm pointing" and the Y component means "how far above where I'm pointing," regardless of the Arwing's heading in world space. Distance (the length of the delta) is unchanged, because rotating a vector doesn't change its length.

This is the same math `Player_SetupShot` (`src/engine/fox_play.c:3023-3036`) already uses to compute a laser's velocity vector, just inverted. There it builds a calc matrix from the player's aim angles and transforms a body-frame `(0, 0, speed)` vector into world space. We build the same matrix and transform a world-space delta into body frame.

The angles that define the Arwing's aim, in the form `Player_SetupShot` uses, are:

- Yaw: `player->rot.y + player->yRot_114`
- Pitch: `player->rot.x + player->xRot_120 + player->aerobaticPitch`
- Bank: `player->bankAngle` — irrelevant for direction; matters only for shot start offset.

These fields are on the `Player` struct at `include/sf64player.h:210`, `:218`, `:221`, `:327`, etc.

The math is identical in on-rails and all-range. What differs is only the range of values the angles take: small deviations from forward in on-rails, full 360° in all-range.

---

## A rejected alternative: the on-screen reticle global

`D_display_801613E0[0..1]` (`src/engine/fox_display.c:16`) holds the on-screen reticle positions, computed inside `Display_Arwing` / `Display_Landmaster` by transforming a forward-axis point through `gGfxMatrix`. It is tempting as a ready-made "where the player is aiming" point, and the spawner mod (`src/mods/spawner.c:47-60`) reads it for click-to-spawn placement.

It is not the right primitive for the cue:

- It lives in `gGfxMatrix` space (camera-relative), not world space. The spawner mod gets away with it because in on-rails the camera is approximately forward-aligned over short distances; that doesn't generalise.
- It is computed at draw time, not as part of the per-frame logic update. Our cue runs on `GamePostUpdateEvent`, before draw.
- Body-frame rotation of the world delta gives the same pan/pitch information with cleaner provenance and works the same in all-range without modification.

---

## Enemy detection criteria

First-pass predicate, applied while walking `gActors[]`:

```c
actor->obj.status == OBJ_ACTIVE
&& actor->obj.id != OBJ_ACTOR_EVENT
&& actor->info.targetOffset != 0.0f
```

This is the same predicate the engine itself uses for laser lock-on — see `PlayerShot_FindLockTarget` (`src/engine/fox_beam.c:1741`) and the follow-up tracking pass at `:2152`. Both sites check exactly `actor->obj.status == OBJ_ACTIVE && actor->info.targetOffset != 0.0f`. By inheriting that predicate, the cue automatically picks up every dynamic adjustment the game already does to disable lock-on:

- **Teammates in all-range** — `ActorAllRange_SpawnTeam` zeroes `info.targetOffset` for aiType ≤ AI360_PEPPY right after `Object_SetInfo` copies the default from `gObjectInfo[]` (`fox_360.c:421`). Fox/Falco/Slippy/Peppy never read as targets.
- **Event handlers** — aiType `AI360_EVENT_HANDLER` actors get `info.targetOffset = 0.0f` every update tick (`fox_360.c:1300`).
- **Defeated / downed states** — various per-level scripts zero `targetOffset` when an enemy transitions out of its hostile state (e.g. `fox_sz.c:842`, `fox_ka.c:2230`, `fox_ti.c:870`, `fox_fo.c:513`).

The `OBJ_ACTOR_EVENT` exclusion is a separate concern: those slots run the level's scripted-event bytecode (`ActorEvent_Update`), have `gNoHitbox`, and are invisible. The master table has `targetOffset = 0.0f` for them (`fox_edata_info.c:319`) so they'd already be filtered, but the explicit id check makes the intent obvious to a reader and survives any future table change.

Note that `actor->info` is the actor's *per-instance copy* of the per-class `ObjectInfo`, not a pointer back to the master table. `Object_SetInfo` (called from `Actor_Load`) does a struct copy at load time; subsequent dynamic writes like the ones above only affect that instance. This is why the engine's own lock-on check reads `actor->info.targetOffset` rather than `gObjectInfo[actor->obj.id].targetOffset`.

### What this catches

Standard "shoot me" enemies across all on-rails levels: Garudas, Skibots, HopBots, Desert Rovers, the Delphor *head*, Fekuda, ZBird, Z-Gulls, Troika, Tankers, Radar Buoys, Bolse shield reactors / laser cannons, Fortuna radars, seeking missiles, Andross laser emitters, etc. In all-range it also catches Star Wolf members (Wolf / Leon / Pigma / Andrew) — `OBJ_ACTOR_ALLRANGE` has `targetOffset = 1.0f` in the master table and only the teammate / event-handler branches zero it, so the Star Wolf instances stay lockable.

### Known intentional misses (deferred, not bugs)

- **Bosses in `gBosses[4]`.** Out of scope for now per the design lead. Bosses need per-encounter cues anyway (multiple parts, weak points, phase changes) and are tracked as separate future work.
- **Boss-shaped actors stored in `gActors[]`.** A few large enemies live in the actor array but have `targetOffset = 0.0f` because the game uses a separate UI for them — Zoness's `OBJ_ACTOR_ZO_DODORA` (the sea-snake mini-boss) and Titania's `OBJ_ACTOR_TI_DELPHOR` body (the sand-worm; its head segment `TI_DELPHOR_HEAD` *is* lockable and will cue normally). Treat these the same as `gBosses[]`: case-by-case later.
- **Non-lockable damage-dealing enemies.** A handful of small turret-style enemies have `damage > 0` but `targetOffset = 0.0f` — e.g. `OBJ_ACTOR_CO_RADAR`, `OBJ_ACTOR_ME_MORA`. They will not cue with the lock-on predicate. If play-testing shows they matter, they can be added by an explicit id allow-list later.
- **Environmental hazards.** Landmines (`OBJ_ACTOR_TI_LANDMINE`, oddly lockable so it *is* caught) aside, the bulk of hazards have `damage > 0` and `targetOffset = 0.0f`: `MA_BOULDER`, `MA_FALLING_BOULDER`, `MA_BOMBDROP`, `MA_BARRIER`, `MA_*_LOCK_BAR`, `MA_TRAIN_CAR_*`, the Macbeth locomotive, `TI_BOULDER`, `TI_BOMB`, Venom 1 pillars, `AND_BRAIN_WASTE`, etc. These are deliberately out of the *enemy* cue's scope. A future "hazard cue" is a distinct feature category — different SFX (so the player can tell "enemy ahead" from "obstacle ahead"), probably different distance scaling, and the producer-side hook may also differ (most hazards spawn from `gLevelObjects` rather than dynamically, and many sit still). Lumping them into the enemy cue now would prejudge that design.

A consequence of starting from the lock-on predicate: the *set of cueable enemies* is identical to the set the player could lock onto and missile, which is a meaningful gameplay set even before play-testing — it is the same set the game itself treats as "primary targets."

---

## Implementation sketch

The enemy cue lives in the existing `src/port/mods/AccessibilityCues.{cpp,h}` alongside the ring cue rather than a sibling file. The two cues share enough shape — the Start/Stop pattern around `Audio_PlaySfx` + `Audio_KillSfxBySource`, the Y→freqMod math, and the gate-find-refresh-or-stop orchestration — that one TU with two parallel state blocks reads more naturally than two near-duplicate files. The file's name is already plural. A bigger separation-of-concerns pass can come later, once both cues work and the real common shape is visible from two concrete data points instead of one.

v1 reuses the existing `gAccessibilityAudioCues` master toggle for both cues. A finer-grained per-cue split (`gAccessibilityRingCue` / `gAccessibilityEnemyCue` / …) is deferred until someone wants independent control.

The enemy cue gets its own file-static state block (`sEnemyCueSrc`, `sEnemyCueFreqMod`, …) because both cues can fire simultaneously, and its own `GamePostUpdateEvent` listener following the same shape as the ring cue's: early-return if disabled or out of context, pick a target enemy, refresh-or-start, stop if none. The ring cue's existing state will need a rename in the same pass (`sCueSrc` → `sRingCueSrc`, etc.) for disambiguation.

Small shared bits worth factoring as the second cue lands — not before:

- A `ComputeFreqModFromY(f32 y) → f32` helper. The six-line Y→octaves→`powf(2, ·)` math is identical for both cues.
- Optionally an internal `CueState` struct (`src[3]`, `freqMod`, `volMod`, `reverb`, `active`) bundling the per-cue state so the two blocks stay visually parallel.

Nothing more ambitious (base class, cue registry, polyphony manager) belongs in v1.

Per-frame update math for the enemy cue, mirroring `AccessibilityCues_RefreshSource` but rotating into body frame:

1. World delta from the Arwing: `dx = enemy.pos.x - player->pos.x`, `dy = enemy.pos.y - player->pos.y`, `dz = enemy.pos.z - player->trueZpos`. (Note `trueZpos`, not `pos.z` — see `docs/game-world.md` §10.)
2. Build a calc matrix from the player's aim angles (yaw, then pitch — bank not needed) and apply it as the inverse rotation to the world delta to obtain `bodyDelta`. Equivalent to: rotate by `-yaw` around Y, then by `-pitch` around X. Implemented via the same `Matrix_RotateY` / `Matrix_RotateX` + `Matrix_MultVec3fNoTranslate` pattern used elsewhere in `fox_play.c`.
3. Feed `bodyDelta.x` to the enemy cue's `src[0]` (pan), `bodyDelta.y` to `src[1]` (drives the Y→pitch shift exactly as the ring cue does today), and the appropriate forward component to `src[2]`. Then `Object_ClampSfxSource` and proceed as the ring cue does.

The forward component for `src[2]` needs a small note: the ring cue uses `-(ring.pos.z - player->trueZpos)` because in on-rails the engine wants positive `src[2]` to mean "ahead." With body-frame rotation, `bodyDelta.z` is already "ahead of aim" in the Arwing's own coordinates, so the sign convention may need a flip relative to the ring cue. To be confirmed at implementation time.

**Resolved at implementation:** the cue keeps the ring cue's "positive src[2] = ahead" convention. The body-frame rotation is built *without* the `+180°` yaw that `Player_SetupArwingShot` applies to its laser-direction matrix — that choice flips the body Z axis so `bodyDelta.z < 0` means "ahead of aim" (matching the world convention where the player flies in `-Z`). The cue then feeds `src[2] = -bodyDelta.z`, exactly parallel to the ring cue's flip. Dropping the `+180°` also makes body `+X` equal "right of aim" without a second sign flip on the pan axis.

---

## A secondary judgment call: pan magnitude

The engine pans by the raw value of `sCueSrc[0]` (with internal clamping). With body-frame rotation, `bodyDelta.x` is the perpendicular distance from the aim line, in world units. That means an enemy 100 units off-axis reads the same to the engine whether it is 500 or 5000 units ahead.

An alternative would be to feed an angular measure — `atan2(bodyDelta.x, bodyDelta.z)` or similar — so the same angular offset gives the same pan regardless of distance. This would be more "aim-correction-shaped" feedback.

Decision for the first pass: keep the raw-distance form, matching the ring cue. Revisit only if play-testing makes the difference matter.

---

## Open questions (for future sessions)

- **Cue SFX choice.** *Resolved (first pass):* `NA_SE_KA_UFO_ENGINE` — bank 1, range 3, no `SFX_FLAG_22/23`, importance 0x70, sustained UFO whir clearly distinct from the ring cue's beam-charge tone. Revisit if play-testing finds it competes with combat SFX or blends with engine drones.
- **Single vs. multiple simultaneous cues.** *Resolved (first pass):* single closest enemy ahead of aim (3D Euclidean distance in body-frame coordinates). Revisit once all-range lands and the higher enemy density makes the trade-off concrete.
- **All-range mode pass.** The math is mode-agnostic. The lock-on predicate already handles the teammates-vs-enemies distinction (teammate `targetOffset` is zeroed by the game), so all-range works without modification — the only gate is the `gLevelMode == LEVELMODE_ON_RAILS` check in `AccessibilityCues_OnEnemyPostUpdate`. The higher enemy count in all-range may force the single-vs-multiple question to a head.
- **Forward-of-aim filter.** *Resolved (first pass):* drop enemies behind the aim line (`bodyDelta.z >= 0`). Reason: the engine's stereo pan uses `|z|`, so behind-aim sources pan center and are ambiguous with directly-ahead. Cost: blind to threats from behind. Revisit if "rear-attack awareness" reads as more important than the directional ambiguity.
- **Coverage for non-lockable enemies and bosses.** Whether to extend past the lock-on predicate to include the deferred categories listed under "Known intentional misses" — boss-shaped actors (Dodora, Delphor body), non-lockable damage-dealers (CO_RADAR, ME_MORA), bosses in `gBosses[]`. Likely to grow into an explicit per-id allow-list rather than a broader predicate.
- **Hazard cue.** A separate cue category for damage-dealing obstacles (boulders, mines, falling bombs, train cars, barriers, pillars). Distinct from the enemy cue in SFX, scaling, and likely producer-side hook. Out of scope for this design doc.

---

## What is settled so far

- Cue panning uses aim-relative coordinates derived by rotating the world delta into the Arwing's body frame.
- Same math handles on-rails and all-range; first implementation pass targets on-rails.
- Distance is the unchanged length of the world delta (drives volume).
- Lives in the existing `src/port/mods/AccessibilityCues.{cpp,h}` alongside the ring cue, gated by the same `gAccessibilityAudioCues` CVar; structure deliberately parallel so small shared helpers (Y→freqMod, an internal `CueState` struct) can emerge once both cues work. A bigger split into separate files and/or per-cue CVars is deferred.
- Pan magnitude is raw perpendicular distance, not angle, for the first pass.
- The on-screen reticle global is not used.
- Enemy predicate is the lock-on proxy: `status == OBJ_ACTIVE && id != OBJ_ACTOR_EVENT && info.targetOffset != 0.0f`, matching the engine's `PlayerShot_FindLockTarget` filter.
- Star Wolf members (Wolf / Leon / Pigma / Andrew) *are* lockable and so are covered by the predicate; only the four playable teammates are explicitly zeroed.
- Bosses (`gBosses[]`), boss-shaped actors with `targetOffset = 0` (Dodora, Delphor body), non-lockable damage-dealers (CO_RADAR, ME_MORA), and environmental hazards are deferred — not cued by the first pass.
