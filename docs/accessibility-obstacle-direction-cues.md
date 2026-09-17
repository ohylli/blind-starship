# The directional obstacle cues: design record

Status: v1 implemented 2026-09-15 (rails only, box-only terrain). Companion to
`docs/accessibility-obstacle-cue.md` (the ahead cue, whose scan these cues share and whose
catalogue of what is and is not an obstacle applies here unchanged) and
`docs/accessibility-cues-tuning.md` (the knobs). This file records what the three cues
say, why they are shaped this way, and the v1 scope decisions — read it before "fixing" a
silence or a drone.

## What they are

Three sustained chords that say **"the space beside / above / below you is closed"**:
there is something solid there that you are *not* going to hit if you keep flying
straight, but *would* hit if you steered that way. They are the "which way is closed"
half of the obstacle family; the ahead cue is the "how soon" half. The two never claim
the same box, so each sound keeps a single meaning.

- **Obstacle beside** (`ObstacleSide`) — one chord, two voices keyed left and right, so a
  corridor sounds both walls at once. Rendered in plain stereo pan (`CUE3D_MODE_PAN`);
  **the pan magnitude is the signal**, loudness is not (see the trim note under "The
  mappings").
- **Obstacle above** (`ObstacleAbove`) and **Obstacle below** (`ObstacleBelow`) —
  separate chords, centered (`CUE3D_MODE_DIRECT`); **the loudness is the signal**.

Each is a two-sine chord a just perfect fourth apart (4:3), rooted an octave apart in
pitch order low, middle, high for below, beside, above: C3, G3, C4. The register tells the
three apart; the shared buzz is a damped pulse and a different shape entirely. The
generator (`ObstacleDirectionCue_GenerateChord`) snaps the root by a few cents so the loop
buffer holds a whole number of cycles of both partials and loops without a click.

The beside chord is pulsed at a **fixed** rate (`kSidePulseHz`, 5 Hz, one pulse per loop
buffer: a few-ms attack from a floor, then a decay back to it). Reason: a constant-power
pan is only a level difference between the ears, and a steady low tone is the sound the
ear places worst from level alone, so the drone read as a vaguer "how far left" than the
aim click by ear (2026-09-16). Each pulse is a fresh onset to localize. The rate never
varies, so it is not a signal and the ahead buzz's varying pulse rate keeps its one
meaning; above and below stay drones, there is nothing to localize dead-center.

## The rule

Every game tick, each box the shared scan yields (`ObstacleScan_ForEachBox` — one box per
solid hitbox record, poly-mesh bounding box, or boxed sphere collider of every
warn-worthy obstacle) is asked three questions, on rails, in
`ObstacleDirectionCue_OnPostUpdate`:

1. **Is it alongside me now, or about to be?** The box's near Z face must be ahead within
   the **lookahead** (default 1200 units, about a second of cruise flight), or the ship
   must already be between the near and far faces. Past the far face the box is dropped.
   This deliberately differs from the ahead cue's "past the near face, the engine owns
   it" rule: a wall alongside still matters, since drifting into it is the whole risk.
2. **Is it the ahead cue's?** An *upcoming* box the ship's position is inside, widened by
   the shared **safety margin**, on both lateral axes is on course and the ahead cue is
   warning about it. These cues skip it. The ahead cue never says which edge is nearest,
   and these never say "the wall ahead is more to your left". A box already alongside is
   never the ahead cue's — it drops a box the moment the ship reaches its near face — so
   an alongside box is classified next however close it is. (v1 as first committed
   skipped every on-course box, upcoming or not, which left the nearest wall of a pair
   the ship was between silent in every cue; fixed the same day.)
3. **Which direction, and how close?** The ship must be outside the box's footprint on an
   axis and within the margin of it on the other. The outside axis names the pair (X for
   beside, Y for above/below); the sign of the box center's offset from the ship picks
   the member; the clearance beyond the box face on that axis is the signal, and it must
   be under that pair's band distance (**side distance**, default 800; **vertical
   distance**, default 600) to count. For an upcoming box that clearance is at least the
   margin (anything closer was question 2's); an alongside box can be closer and pins at
   the band's near end. A box outside on both axes beyond the margin is a corner and
   nobody's in v1; one within the margin on both (only possible alongside, hugging a
   corner) goes to the nearer face; one the ship is inside on both axes is the engine's.

Per direction the box with the smallest clearance wins. Ties and multiple candidates are
not blended: one box per direction per tick.

## The mappings

Both bands run from the margin (clearance equal to the margin: as close as an upcoming
box can be without being the ahead cue's) to the band distance. An alongside box closer
than the margin pins at the band's near end.

- **Beside: pan, reversed.** A box at the band's edge is panned hard to its side; a box at
  the margin sits near the center. "The closer the sound is to center, the closer the
  obstacle is to you" was chosen by ear over the naive mapping; it stands on its own as a
  preference (the aim cue's pan encodes the player's own offset, not a target's distance,
  so there is no convention being inherited). A **pan floor** (default 0.2 of full pan)
  keeps the closest possible wall off dead center, where left and right would be
  indistinguishable at exactly the moment they matter most. Implementation note: the
  backend pans by the sine of the source's azimuth, so the voice is placed at
  `(m·r, 0, sqrt(1 − m²)·r)` for pan magnitude `m` at the unity-gain radius `r`, and the
  distance never changes across the band. Loudness is not a signal here, but it is not
  perfectly constant either: the Cue layer's multi-voice headroom trim
  (`Cue::HeadroomTrim`, 1/sqrt(N)) lowers both side voices by about 3 dB while the
  second wall sounds and lifts the survivor back when one leaves the band. Accepted for
  now since the step is shared by both voices and pan carries the meaning; if it reads
  as "closer" by ear, opting the side cue out of the trim is the fix.
- **Above / below: loudness.** Full at the margin, the **level floor** (default 0.15 of
  the volume slider) at the band's edge, so the onset is audible rather than a fade-in the
  player cannot place. This uses the Cue layer's per-voice `CueTarget::level`, added for
  these cues: the slider keeps meaning "the loudest this cue ever gets".

## Why one file for three cues

The convention is one file pair per cue. These three share `ObstacleDirectionCue.{h,cpp}`
because they are one rule with the axes swapped, and a rule that lived in two files would
fork. Each still has its own registry id, volume slider, and preview. The debug server's
`cues` dump reports the shared decision under each of the three ids, with only the
direction(s) that id renders.

## v1 scope — deliberate, with attach points

- **Rails only.** In all-range "left" is relative to the heading, not world X; the box
  offsets need rotating into the aim frame before the same three questions apply. The
  ahead cue took the same two-step path (`docs/accessibility-obstacle-cue.md`, its
  all-range ray test). The listener stops all three cues in all-range mode; the `cues`
  dump's `railsOnly` gate is the tell.
- **Terrain is box-only.** A heightfield bump's box spans the whole hill, so the below cue
  reports "terrain below" whenever the ship is over a bump's footprint, at the box top's
  clearance rather than the slope's. On Corneria, which places 178 bumps, the below chord
  is therefore on and fairly loud over much of the level at cruise altitude. Accepted for
  v1; the refinement is a surface-distance probe (a short bisection on Y over
  `Object_PolyHeightfieldHit`, the engine's own surface test that the ahead cue's
  heightfield walk uses) replacing the box top for that family. If the drone bites before
  that lands, excluding `polyHeightfield` boxes from the vertical pair is a one-line policy
  change in the listener.
- **The engine's poly range gate is not applied** (the ahead cue clips its course to it).
  A mesh inside the side or vertical band is inside the engine's XZ gate anyway, and the
  bands are far shorter than the gate radii.
- **Ground, water, and lava stay out** — they are not objects (`docs/game-world.md` §8);
  the user chose to leave the ground plane out of the below cue for now.
- **A future "upcoming vs alongside" split** is already classified: the debug mirror's
  `upcoming` flag says whether the winning box's near face is still ahead. A second cue
  distinguishing the two would read the same classification, not re-derive it.
- Everything in the ahead cue's catalogue of intentional misses (rotated boxes used
  unrotated, sprites not scanned, lockable actors never counted, bosses and wingmates
  excluded) applies here through the shared scan.

## Testing notes

- **Minimal training silences these cues in Training by design**, like the ahead cue.
  Tune on Corneria from the `obstacle-scout` debug checkpoint (`tools/checkpoints.json`).
  Verified live 2026-09-16 (paused warp, one step, every knob at its default): the
  checkpoint sits *inside* the Z span of a rock-wall pair (`OBJ_SCENERY_CO_ROCKWALL`,
  hitbox half-extents 169/433/898) with a third wall coming up. The alongside left wall,
  47 units to the left (under the 150 margin), is the side cue's left winner pinned at
  the pan floor 0.2 — this is the box the first-committed v1 left silent in every cue
  (the question-2 gap above). The alongside right wall, 238 units to the right, is the
  right winner at pan 0.31. The upcoming left wall (clearance 123, near face 335 ahead)
  is the ahead cue's, and the direction cues skip exactly that one (`aheadClaimed` 1). Bump 4 (`OBJ_SCENERY_CO_BUMP_4`, the
  heightfield poly box) is the below winner at clearance 176, level 0.95 — the box-only
  over-report described above. About 130 frames on, `OBJ_SCENERY_CO_BUILDING_1` becomes
  the above winner at clearance 491 (level 0.36) closing to 317 (0.68): flying under an
  overhang. Further into the city both side voices sound at once (a building each side,
  the closer at pan 0.25, the farther at 0.71), and the reversed pan mapping reads
  correctly in the dump — a wall at clearance 158 sits at 0.21, one at 748 at 0.94.
- The debug server's `cues` command reports, under each of the three ids, the gates
  (`railsOnly` among them), the scan counters plus `inWindow` (boxes inside the Z window)
  and `aheadClaimed` (boxes the ahead cue owns), the effective knobs, and per direction
  the candidate count and the winner (array/slot/objId/record, `upcoming`, the winning
  `clear`, `gapZ`, delta and half-extents, and the pushed `pan` or `level`). Join `slot`
  against `objects scenery` to cross-check positions.
- The F1 volume-slider previews play each chord at reference loudness exactly as it
  sounds in play (the above/below drones, the pulsed beside chord): the previews are the
  timbre check.
