# Accessibility enemy audio cue — design notes

This document is the working brief for adding a positional audio cue that helps a blind player locate enemies, sibling to the existing training-ring cue at `src/port/mods/AccessibilityCues.{cpp,h}`. It captures decisions as they are made so that future sessions can pick up the context without re-deriving it. It is expected to grow.

First scope is on-rails levels. All-range is a follow-up, but the design below was chosen so the same code works in both modes.

For background on how the world is laid out (where enemies live, how to iterate them, hitbox/targetability fields), see `docs/game-world.md`. For how a positional SFX is emitted and which knobs the audio engine exposes, see `docs/audio-system.md` and `docs/accessibility-cues-tuning.md`.

---

## What the cue is

A continuous positional SFX attached to an enemy, conceptually the same shape as the ring cue: distance drives volume, lateral offset drives stereo pan, vertical offset drives a pitch shift. The difference from the ring cue is the coordinate frame the lateral/vertical offset is measured in (see next section).

What exactly counts as "an enemy" and which SFX represents one is deferred — see "Open questions" below.

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

## Implementation sketch

The cue mod will live at `src/port/mods/AccessibilityEnemyCues.{cpp,h}` (sibling to the ring cue), gated by a new CVar (provisionally `gAccessibilityEnemyCues`), and registered the same way the ring cue is — a listener on `GamePostUpdateEvent` that early-returns if the CVar is off, picks a target enemy, computes the body-frame source vector, and keeps a positional SFX attached to it via `Audio_PlaySfx` + `Audio_KillSfxBySource`.

Per-frame update shape, mirroring `AccessibilityCues_RefreshSource`:

1. World delta from the Arwing: `dx = enemy.pos.x - player->pos.x`, `dy = enemy.pos.y - player->pos.y`, `dz = enemy.pos.z - player->trueZpos`. (Note `trueZpos`, not `pos.z` — see `docs/game-world.md` §10.)
2. Build a calc matrix from the player's aim angles (yaw, then pitch — bank not needed) and apply it as the inverse rotation to the world delta to obtain `bodyDelta`. Equivalent to: rotate by `-yaw` around Y, then by `-pitch` around X. Implemented via the same `Matrix_RotateY` / `Matrix_RotateX` + `Matrix_MultVec3fNoTranslate` pattern used elsewhere in `fox_play.c`.
3. Feed `bodyDelta.x` to `sCueSrc[0]` (pan), `bodyDelta.y` to `sCueSrc[1]` (drives the Y→pitch shift exactly as the ring cue does today), and the appropriate forward component to `sCueSrc[2]`. Then `Object_ClampSfxSource` and proceed as the ring cue does.

The forward component for `sCueSrc[2]` needs a small note: the ring cue uses `-(enemy.pos.z - player->trueZpos)` because in on-rails the engine wants positive `sCueSrc[2]` to mean "ahead." With body-frame rotation, `bodyDelta.z` is already "ahead of aim" in the Arwing's own coordinates, so the sign convention may need a flip relative to the ring cue. To be confirmed at implementation time.

---

## A secondary judgment call: pan magnitude

The engine pans by the raw value of `sCueSrc[0]` (with internal clamping). With body-frame rotation, `bodyDelta.x` is the perpendicular distance from the aim line, in world units. That means an enemy 100 units off-axis reads the same to the engine whether it is 500 or 5000 units ahead.

An alternative would be to feed an angular measure — `atan2(bodyDelta.x, bodyDelta.z)` or similar — so the same angular offset gives the same pan regardless of distance. This would be more "aim-correction-shaped" feedback.

Decision for the first pass: keep the raw-distance form, matching the ring cue. Revisit only if play-testing makes the difference matter.

---

## Open questions (for future sessions)

These are deliberately left unanswered for now. No speculation is recorded here; the next session that picks one up should start from scratch.

- **Enemy detection criteria.** What predicate on a `gActors[]` slot (and possibly `gBosses[]`) makes it count as an enemy worth cueing. `gActors[]` holds many non-enemy entities (teammates, missiles, event scripts, cutscene props — see `docs/game-world.md` §3 and §6). The relevant raw signals are `obj.id`, `info.damage`, `info.targetOffset`, the `aiType` field, and the `OBJECT_TYPE_ACTOR_EVENT` distinction. Concrete criteria are TBD.
- **Cue SFX choice.** Which `NA_SE_*` to use. The bank/range/flag constraints listed in `docs/accessibility-cues-tuning.md` apply unchanged. The choice should also be distinguishable from the ring cue so the two cues are not confused in levels where both could fire.
- **Single vs. multiple simultaneous cues.** Whether to cue one enemy at a time (nearest? most threatening?) or several concurrently. TBD.
- **All-range mode pass.** The math is mode-agnostic, but enemy detection in all-range may want different filters (Star Wolf fighters, teammates-vs-enemies distinction via `aiType`), and the higher enemy count may force the single-vs-multiple question to a head.

---

## What is settled so far

- Cue panning uses aim-relative coordinates derived by rotating the world delta into the Arwing's body frame.
- Same math handles on-rails and all-range; first implementation pass targets on-rails.
- Distance is the unchanged length of the world delta (drives volume).
- Mod lives at `src/port/mods/AccessibilityEnemyCues.{cpp,h}`, mirrors the ring cue's structure and event wiring.
- Pan magnitude is raw perpendicular distance, not angle, for the first pass.
- The on-screen reticle global is not used.
