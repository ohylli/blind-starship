# The directional obstacle cues: design record

Status: v1 implemented 2026-09-15 (rails only); terrain surface walk for the below cue
added 2026-09-17; all-range support added 2026-09-23. Companion to
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
warn-worthy obstacle) is asked three questions in `ObstacleDirectionCue_OnPostUpdate`.
They are stated here in rails terms (the course is the −Z track, "beside" is world X,
"above/below" world Y); all-range asks the same three in the heading's frame — see
"All-range: the heading frame" below.

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

## Terrain below

A heightfield mesh (the scan's `polyHeightfield` boxes: Corneria's bumps, the reefs, the
island, the mountains) is hit only from above, at the surface, and its box spans the
whole hill. As first committed the below cue measured to the box top — the hill's
*peak* — wherever the ship was over the footprint, so the chord droned near full over
much of Corneria (178 bumps). For that family the vertical question is now answered by
the surface itself (`ObstacleDirectionCue_TerrainBelow`):

- **The height comes from the engine, in one read.** `Col2_CheckSurface` computes the
  surface height under a point on the way to its hit verdict and writes it out whether or
  not the point is low enough to hit; `Object_PolyHeightfieldSurfaceY` reads it. It is
  the number the crash test compares against (mesh-local and integer-truncated, quirks
  included), which is what "how far above a crash am I" wants. The originally planned
  bisection over `Object_PolyHeightfieldHit` was dropped as ten probes for the same
  number.
- **The walk.** For a terrain box in the Z window with the ship within the margin of its
  footprint in X, the course is sampled from beneath the ship (or the box's near face, if
  still ahead) to the far face or the lookahead, whichever is nearer, every 100 units —
  the family's shared grid (`kObstacleWalkStep` / `ObstacleCommon_WalkSteps` in
  `ObstacleCommon.h`), the same one the ahead cue's walk samples on, which is what makes
  the per-sample claim below exactly the ahead cue's hit. The grid is anchored where the
  ahead cue's own walk over the box starts. On rails that is the same starting point
  except when the engine's range gate pushes the ahead cue's start further in, and the
  grid then shifts to match it (an A/B trace on 2026-09-23 showed this as the only rails
  difference: one extra sample and a few units of clearance, on a dive).
  Each step reads the surface under the course and one margin to either side and keeps
  the highest; the clearance is the ship's Y above it, and the smallest clearance over
  the walk is the box's candidate in the ordinary below contest. Samples the engine's XZ
  range gate would never test are dropped. At the default lookahead that is at most 39
  reads per bump.
- **Ownership is per sample, not per box.** One hill is both "ground 300 below me" and "a
  slope rising into my course", so the box rule's wholesale claim does not fit. An
  *upcoming* sample whose surface is within the margin of the course is the ahead cue's —
  the same threshold its heightfield walk hits at — and is skipped; the rest still
  compete. A rising slope therefore sounds as the buzz plus a near-full below chord:
  "the thing ahead is ground, climb", which the box rule cannot say. The sample beneath
  the ship is the alongside case and pins at full however close. Known, accepted
  overlap: the ahead cue's walk also starts beneath the ship, so skimming within the
  margin sounds both.
- The lookahead makes the chord anticipate: it rises as a hill's peak comes within the
  lookahead, about a second before the ship is over it, exactly as an upcoming box does.
- The side question still reads a terrain box as a box, box claim included.

## All-range: the heading frame

In all-range the ship flies in any direction, so "left" and "above" are relative to the
heading. The three questions are asked in the **heading frame**: the aim heading
(`Player_AimForward`), the horizontal vector to the craft's right, and the canopy
direction, together `Player_AimBasis` in `src/port/PlayerAim.h`. Bank is ignored, as in
the enemy cue's body frame. So "above" is toward the canopy (world up in level flight,
tilted with the nose when climbing or diving), and "beside" is what a turn would bring
onto the course. The chords and their mappings are unchanged.

- **The boxes stay axis-aligned in the world, so the clearance is computed, not read
  off.** Seen from a turned heading a box is a rotated block. Rotating its center into
  the frame and reusing the per-axis extents would overstate its size (by up to about 40%
  for a cube, far more for a long wall seen at 45 degrees), and that error would go
  straight into the pan and loudness. `ObstacleDirectionCue_SliceRange` instead takes the
  part of the box inside the lookahead window and within the margin of the course on one
  steering axis, and finds its exact offset range along the other: the extremes of a
  linear function over a convex polytope, evaluated at every candidate vertex (box
  corners, box edges crossing the slab planes, slab-plane lines crossing the box). On
  rails the same computation gives the rails rule's numbers back exactly, so one code
  path serves both modes; a 300-frame A/B trace at `obstacle-scout` against the previous
  build matched on every sample. Verified in all-range on 2026-09-23 against a
  brute-force recomputation from the `cues` dump (16 winners on Sector Z in yawed and
  pitched headings, all within 1.5 units, the sampling grid's resolution).
- **Question 1** uses the box's extent along the course (its center's course distance
  plus or minus the projection of its half-extents); on rails that is the near and far
  Z faces.
- **Question 2 is the ahead cue's own test.** A box is the ahead cue's when
  `ObstacleScan_CourseSpan`, the function the ahead cue itself calls (the footprint test
  on rails, the ray test in all-range), has the course running into it with the entry
  still ahead. Sharing the function is what keeps "the two never claim the same box"
  true in a turned heading.
- **Terrain below** walks the tilted course: samples along the aim ray over the box's
  footprint (widened by the margin), the clearance being the course's height above the
  engine's surface there, measured straight down. Rails keeps its own span (the box's Z
  extent) so its sampling is unchanged; the grid anchoring above applies to both.
- **Silent during a U-turn or somersault** (`player->state == PLAYERSTATE_U_TURN` or
  `player->somersault`). The player is not steering then, so "you would hit it by
  steering that way" does not apply, and the frame swings round (and past 90 degrees of
  pitch turns upside down) faster than a chord could say anything useful. Rails keeps
  the world frame through a somersault and is unchanged. The ahead cue is not gated.
- **Arwing only**, through `Player_AimAnglesValid`, the same gate as the ahead cue; every
  solo arena flies an Arwing.
- **The non-lockable fighters are skipped** (`OBJ_ACTOR_ALLRANGE` in `gActors`: the
  all-range wingmates and the allied craft). They pass the obstacle predicate, but they
  fly: a wingmate beside you does not keep that space closed, and they are near you
  constantly. The ahead cue still warns of one crossing the course. On Katina, where the
  allies are the only obstacles, the directional cues are therefore silent. The skip is
  by object id, so it applies on rails too, where that id does not normally appear.

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

## Scope — deliberate, with attach points

- **Solid poly meshes are boxes too.** Fortuna's Mountain 3 is collided by the engine
  as a solid triangle mesh (the CollisionHeader family), not a heightfield, so it gets
  no surface walk and the below chord reads its bounding-box top (the peak) anywhere
  over its footprint (seen live 2026-09-23 as a constant clearance of 286 while crossing
  one). The ahead cue sees the same box. Refining it would need a surface probe for that
  mesh family; deferred.
- **Rotated objects.** The scan ignores `obj.rot` (`ObstacleScan.h`), and all-range
  scenery is more often yawed. For the ahead cue's yes/no answer the margin absorbs it;
  here it shifts a yawed wall's clearance, and so its pan or loudness. Accepted for now;
  the fix belongs in the shared scan, where it would help every obstacle cue.
- **Terrain is box-only for the beside pair.** A heightfield bump's box spans the whole
  hill, so a hill flown beside near its peak height reports the box face, not the slope at
  the ship's altitude. Minor on rails (hills are wide at the base); more noticeable on
  Fortuna, whose 24 Mountain 1 hills are roughly 2000 wide, 600 tall and 2150 deep. They are
  kept in rather than dropped (the user's call, 2026-09-23); the refinement, a sideways
  walk over `Object_PolyHeightfieldSurfaceY`, is planned as a later step. Below is refined — see "Terrain
  below" above.
- **Ground vehicles get no terrain below.** For the Landmaster and on foot the engine
  seats the player on a heightfield rather than crashing them
  (`Player_CheckPolyCollision`), so both terrain walks — this cue's and the ahead cue's —
  are skipped for those forms, through the family's one predicate
  (`ObstacleCommon_TerrainIsFloor`). A forward guard rather than an observed fix: no
  rails level pairs a ground form with a heightfield box today (Macbeth's terrain bump is
  outside the poly dispatch), and on foot exists only in Versus.
- **The engine's poly range gate is applied only to the terrain walk** (the ahead cue
  clips its course to it). For the box tests a mesh inside the side or vertical band is
  inside the engine's XZ gate anyway, and the bands are far shorter than the gate radii.
- **Ground, water, and lava stay out** — they are not objects (`docs/game-world.md` §8);
  the user chose to leave the ground plane out of the below cue for now.
- **A future "upcoming vs alongside" split** is already classified: the debug mirror's
  `upcoming` flag says whether the winning box's near face is still ahead — or, for a
  terrain walk winner, whether the winning *sample* is ahead rather than beneath the
  ship, the per-sample restatement of the same question. A second cue distinguishing
  the two would read the same classification, not re-derive it.
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
  heightfield poly box) was the below winner at clearance 176, level 0.95, under the
  original box-top rule (see the terrain note below for what it reads now). About 130 frames on, `OBJ_SCENERY_CO_BUILDING_1` becomes
  the above winner at clearance 491 (level 0.36) closing to 317 (0.68): flying under an
  overhang. Further into the city both side voices sound at once (a building each side,
  the closer at pan 0.25, the farther at 0.71), and the reversed pan mapping reads
  correctly in the dump — a wall at clearance 158 sits at 0.21, one at 748 at 0.94.
- **Terrain below** (verified live 2026-09-17, same checkpoint, defaults, flying straight
  at Y 350): bump 4 (`gScenery` slot 9) wins through the walk (`fromTerrainWalk: true`) at
  clearance 181 against its peak (`surfaceY` 169) with `sampleT` counting down from 1200
  to 0 as the peak approaches, then the clearance opens 203, 229, 265, 302, 339 down the
  back slope (level 0.89 falling to 0.60) where the box-top rule held 176 / 0.95 the whole
  way; the next bump (slot 12) fades in from 315. `terrainWalk.probes` peaks at 39. Diving
  (`input stick 0 60 12`): once the course comes within the margin of the slope ahead the
  ahead cue takes the bump (`heightfield: true`, gap 700 closing to 0), `terrainWalk.claimed`
  goes to 1, and the below chord holds near full on the nearest unclaimed sample, then
  pins at 1.0 on the sample beneath the ship (clearance down to 24) until the climb-out.
- The debug server's `cues` command reports, under each of the three ids, the gates
  (`aimValid` and `noManeuver` among them, both always true on rails), the course
  `forward` vector, `fightersSkipped`, the scan counters plus `inWindow` (boxes inside the
  lookahead window)
  and `aheadClaimed` (solid boxes the ahead cue owns whole; a heightfield box is never
  claimed whole, its claim being per sample, so it never counts here), the effective
  knobs, and per direction the candidate count and the winner (array/slot/objId/record,
  `upcoming`, the winning `clear`, `gap` (the box's nearest course distance: the near Z
  face on rails, where it replaced the old `gapZ` key), delta and half-extents, and the pushed `pan`
  or `level`; a terrain walk winner adds `fromTerrainWalk`, `surfaceY` and `sampleT`, the
  winning sample's course distance), plus the terrain walk's
  `terrainWalk.tested/claimed/probes` counters. Join `slot` against `objects scenery` to
  cross-check positions.
- The F1 volume-slider previews play each chord at reference loudness exactly as it
  sounds in play (the above/below drones, the pulsed beside chord): the previews are the
  timbre check.
