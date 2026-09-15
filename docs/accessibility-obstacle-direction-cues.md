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
  corridor sounds both walls at once. Rendered in plain stereo pan (`CUE3D_MODE_PAN`) at
  constant loudness; **the pan magnitude is the signal**.
- **Obstacle above** (`ObstacleAbove`) and **Obstacle below** (`ObstacleBelow`) —
  separate chords, centered (`CUE3D_MODE_DIRECT`); **the loudness is the signal**.

Each is a two-sine chord a just perfect fourth apart (4:3), rooted an octave apart in
pitch order low, middle, high for below, beside, above: C3, G3, C4. The register tells the
three apart; the shared buzz is a damped pulse and a different shape entirely. The
generator (`ObstacleDirectionCue_GenerateChord`) snaps the root by a few cents so the loop
buffer holds a whole number of cycles of both partials and loops without a click. Nothing
pulses: the ahead buzz stays the only pulsing cue, so pulse rate keeps its one meaning.

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
2. **Is it the ahead cue's?** A box the ship's position is inside, widened by the shared
   **safety margin**, on both lateral axes is on course and belongs to the ahead cue
   alone. These cues skip it. The ahead cue never says which edge is nearest, and these
   never say "the wall ahead is more to your left".
3. **Which direction, and how close?** The box must be outside the margin on exactly one
   axis and inside it on the other. The outside axis names the pair (X for beside, Y for
   above/below); the sign of the box center's offset from the ship picks the member; the
   clearance beyond the box face on that axis is the signal, and it must be under that
   pair's band distance (**side distance**, default 800; **vertical distance**, default
   600) to count. A corner box, outside the margin on both axes, is nobody's in v1.

Per direction the box with the smallest clearance wins. Ties and multiple candidates are
not blended: one box per direction per tick.

## The mappings

Both bands run from the margin (clearance equal to the margin: as close as a box can be
without being the ahead cue's) to the band distance.

- **Beside: pan, reversed.** A box at the band's edge is panned hard to its side; a box at
  the margin sits near the center. "The closer the sound is to center, the closer the
  obstacle is to you" was chosen by ear over the naive mapping and matches how the aim
  cue places things. A **pan floor** (default 0.2 of full pan) keeps the closest possible
  wall off dead center, where left and right would be indistinguishable at exactly the
  moment they matter most. Implementation note: the backend pans by the sine of the
  source's azimuth, so the voice is placed at `(m·r, 0, sqrt(1 − m²)·r)` for pan
  magnitude `m` at the unity-gain radius `r`, and the distance never changes across the
  band.
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
  Verified live 2026-09-15, stepping from the checkpoint with every knob at its default:
  the checkpoint sits *inside* the Z span of a rock-wall pair (`OBJ_SCENERY_CO_ROCKWALL`,
  hitbox half-extents 169/433/898) — the left wall is on course (clearance 123 under the
  150 margin) and belongs to the ahead cue, the right wall is alongside 238 units to the
  right and is the side cue's winner at pan 0.31. Bump 4 (`OBJ_SCENERY_CO_BUMP_4`, the
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
- The F1 volume-slider previews play each chord as a seamless drone at reference loudness,
  which is exactly what the chord is: the previews are the timbre check.
